/* Rollback netcode: the input queues (rb.h). SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include "rb.h"

#define M (RB_RING - 1)
static int g_np, g_local, g_delay, g_mismatch;
static RbPad g_lpad[RB_RING];
static int g_lframe[RB_RING], g_llatest;
static struct {
    RbPad pad[RB_RING]; int frame[RB_RING];       /* real pads received */
    RbPad pred[RB_RING]; int pframe[RB_RING];     /* what was used for frames not received yet */
    int latest;                                   /* newest frame with every earlier one received */
} g_r[RB_MAX_PLAYERS];

void rb_input_reset(int nplayers, int local, int delay) {
    memset(g_lpad, 0, sizeof g_lpad); memset(g_lframe, 0, sizeof g_lframe); memset(g_r, 0, sizeof g_r);
    g_np = nplayers < 1 ? 1 : nplayers > RB_MAX_PLAYERS ? RB_MAX_PLAYERS : nplayers;
    g_local = local;
    g_delay = delay < 0 ? 0 : delay > 15 ? 15 : delay;
    g_llatest = g_mismatch = 0;
}
int rb_input_delay(void) { return g_delay; }

static int16_t rest1(int16_t v) { return (v >= -RB_STICK_REST && v <= RB_STICK_REST) ? 0 : v; }
RbPad rb_pad_rest(RbPad p) { p.lx = rest1(p.lx); p.ly = rest1(p.ly); p.rx = rest1(p.rx); p.ry = rest1(p.ry); return p; }
int rb_pad_equal(const RbPad *a, const RbPad *b) {
    return a->buttons == b->buttons && a->lx == b->lx && a->ly == b->ly && a->rx == b->rx && a->ry == b->ry;
}

RbPad rb_input_local(int frame, RbPad now) {
    static const RbPad zero;
    int f = frame + g_delay;
    for (int g = g_llatest + 1; g < f; g++) { g_lpad[g & M] = zero; g_lframe[g & M] = g; }     /* the first frames: nothing pressed */
    if (f > g_llatest) { g_lpad[f & M] = f <= RB_LEADIN ? zero : rb_pad_rest(now); g_lframe[f & M] = f; g_llatest = f; }
    return g_lframe[frame & M] == frame ? g_lpad[frame & M] : zero;
}
int rb_input_local_latest(void) { return g_llatest; }
int rb_input_local_get(int frame, RbPad *out) {
    if (frame < 1 || frame > g_llatest || g_lframe[frame & M] != frame) return 0;
    *out = g_lpad[frame & M];
    return 1;
}

void rb_input_remote_put(int p, int frame, RbPad pad) {
    if (p < 0 || p >= g_np || p == g_local || frame < 1) return;
    if (frame <= g_r[p].latest - RB_RING + 1) return;              /* older than the ring */
    if (g_r[p].frame[frame & M] == frame) return;                  /* a redundant copy */
    pad = rb_pad_rest(pad);
    g_r[p].pad[frame & M] = pad; g_r[p].frame[frame & M] = frame;
    while (g_r[p].frame[(g_r[p].latest + 1) & M] == g_r[p].latest + 1) g_r[p].latest++;
    if (g_r[p].pframe[frame & M] == frame) {                       /* this frame ran on a guess: was it right? */
        if (!rb_pad_equal(&g_r[p].pred[frame & M], &pad) && (!g_mismatch || frame < g_mismatch)) g_mismatch = frame;
        g_r[p].pframe[frame & M] = 0;
    }
}
int rb_input_remote_latest(int p) { return p >= 0 && p < g_np ? g_r[p].latest : 0; }

RbPad rb_input_get(int p, int frame, int *predicted) {
    static const RbPad zero;
    if (predicted) *predicted = 0;
    if (p < 0 || p >= g_np) return zero;
    if (p == g_local) { RbPad out = zero; rb_input_local_get(frame, &out); return out; }
    if (g_r[p].frame[frame & M] == frame) return g_r[p].pad[frame & M];
    /* not received: the newest real pad before it, repeated */
    RbPad guess = zero;
    for (int f = frame - 1; f >= 1 && f > frame - RB_RING; f--) if (g_r[p].frame[f & M] == f) { guess = g_r[p].pad[f & M]; break; }
    g_r[p].pred[frame & M] = guess; g_r[p].pframe[frame & M] = frame;
    if (predicted) *predicted = 1;
    return guess;
}

int rb_input_take_mismatch(void) { int m = g_mismatch; g_mismatch = 0; return m; }
void rb_input_put_mismatch(int m) { if (m > 0 && (!g_mismatch || m < g_mismatch)) g_mismatch = m; }      /* (R3 pub1) a load refused: taken again next tick */

int rb_input_finalized(void) {
    int fin = g_llatest;
    for (int p = 0; p < g_np; p++) if (p != g_local && g_r[p].latest < fin) fin = g_r[p].latest;
    return fin;
}
