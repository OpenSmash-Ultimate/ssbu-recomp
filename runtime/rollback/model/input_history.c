/* Rollback netcode: the input queues (API in rb.h). SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What this file keeps, per frame number:
 *   - the LOCAL player's pads. A pad read at frame F is stored for frame F + delay (input delay), so the other game has
 *     time to receive it before it needs it; the game gets, at frame F, the pad that was read `delay` frames earlier.
 *   - each REMOTE player's pads as they arrive from the network, and, for a frame that has to run before its pad has
 *     arrived, the GUESS that was used (the player's last real pad, repeated). When the real pad arrives and differs
 *     from the guess, that frame is a MISMATCH: the caller must roll back to before it and run it again.
 * Frames are counted from 1. Each table below is a ring of RB_RING slots indexed by frame & RING_MASK; every slot also
 * stores the frame number it holds, so a slot left over from an older lap of the ring is never taken for a current one. */
#include <stdlib.h>
#include <string.h>
#include "../rollback.h"

#define RING_MASK (RB_RING - 1)

static int g_nplayers;                      /* players in the match */
static int g_local_player;                  /* which of them this game controls */
static int g_input_delay;                   /* frames between reading the local pad and using it */
static int g_first_mismatch_frame;          /* earliest frame whose guess was wrong and was not handled yet; 0 = none */

static RbPad g_local_pad[RB_RING];          /* the local player's pad for each frame (already delayed) */
static int g_local_pad_frame[RB_RING];      /* the frame each slot of g_local_pad holds */
static int g_local_newest_frame;            /* newest frame that has a local pad */

typedef struct {
    RbPad pad[RB_RING]; int pad_frame[RB_RING];          /* real pads received */
    RbPad guess[RB_RING]; int guess_frame[RB_RING];      /* the guess used for a frame that ran before its pad arrived (0 = none pending) */
    int newest_consecutive;                              /* newest frame such that this player's pads for every frame up to it were received */
} RemotePlayer;
static RemotePlayer g_remote[RB_MAX_PLAYERS];

/* Starts a match: empties every table. `local` is this game's player index, `delay` the input delay in frames (clamped to 0..15). */
void rb_input_reset(int nplayers, int local, int delay) {
    memset(g_local_pad, 0, sizeof g_local_pad); memset(g_local_pad_frame, 0, sizeof g_local_pad_frame); memset(g_remote, 0, sizeof g_remote);
    g_nplayers = nplayers < 1 ? 1 : nplayers > RB_MAX_PLAYERS ? RB_MAX_PLAYERS : nplayers;
    g_local_player = local;
    g_input_delay = delay < 0 ? 0 : delay > 15 ? 15 : delay;
    g_local_newest_frame = g_first_mismatch_frame = 0;
}
/* The input delay in force (after the clamp in rb_input_reset). */
int rb_input_delay(void) { return g_input_delay; }

/* A stick value inside the rest zone is exactly 0: controller noise must not cause a rollback. (rb6) Outside it the value
 * is rounded toward 0 to a step of 1 << SSBU_RB_STICK_Q (default 8: 256 levels per axis, finer than a GameCube stick): a
 * stick held still wobbles by a few of the 65536 steps, and every wobble of the other player's stick was a wrong guess
 * and a rollback. Both games must use the same value (it changes the pads both simulate); 0 = exact values as before. */
static int stick_shift(void) { static int q = -1; if (q < 0) { const char *e = getenv("SSBU_RB_STICK_Q"); q = e && *e ? atoi(e) : 8; if (q < 0 || q > 12) q = 8; } return q; }
static int16_t stick_to_rest(int16_t v) {
    if (v >= -RB_STICK_REST && v <= RB_STICK_REST) return 0;
    int m = ~((1 << stick_shift()) - 1);
    return (int16_t)(v >= 0 ? (v & m) : -((-(int)v) & m));
}
/* The pad with all four stick axes passed through stick_to_rest; both sides apply it before a pad is stored or sent, so they agree. */
RbPad rb_pad_rest(RbPad p) { p.lx = stick_to_rest(p.lx); p.ly = stick_to_rest(p.ly); p.rx = stick_to_rest(p.rx); p.ry = stick_to_rest(p.ry); return p; }
/* 1 when the two pads have the same buttons and the same four stick values. */
int rb_pad_equal(const RbPad *a, const RbPad *b) {
    return a->buttons == b->buttons && a->lx == b->lx && a->ly == b->ly && a->rx == b->rx && a->ry == b->ry;
}

/* The local player's pad was read as `now` at `frame`: it is stored for frame + delay (sticks passed through rb_pad_rest),
 * and the pad that applies at `frame` itself (the one read `delay` frames ago) is returned; nothing pressed in the first
 * `delay` frames of a match, which are filled with empty pads here. */
RbPad rb_input_local(int frame, RbPad now) {
    static const RbPad zero;
    int store_frame = frame + g_input_delay;
    for (int f = g_local_newest_frame + 1; f < store_frame; f++) { g_local_pad[f & RING_MASK] = zero; g_local_pad_frame[f & RING_MASK] = f; }     /* the first frames: nothing pressed */
    if (store_frame > g_local_newest_frame) { g_local_pad[store_frame & RING_MASK] = rb_pad_rest(now); g_local_pad_frame[store_frame & RING_MASK] = store_frame; g_local_newest_frame = store_frame; }
    return g_local_pad_frame[frame & RING_MASK] == frame ? g_local_pad[frame & RING_MASK] : zero;
}
/* The newest frame the local player has a pad for (frame + delay of the latest rb_input_local call); 0 before the first. */
int rb_input_local_newest_frame(void) { return g_local_newest_frame; }
/* Gives the local pad of `frame` through *out and returns 1, or returns 0 when that frame has none (never stored, in the future,
 * or already overwritten by a later lap of the ring). Used to send recent pads again. */
int rb_input_local_get(int frame, RbPad *out) {
    if (frame < 1 || frame > g_local_newest_frame || g_local_pad_frame[frame & RING_MASK] != frame) return 0;
    *out = g_local_pad[frame & RING_MASK];
    return 1;
}

/* A pad of remote player `p` for `frame` arrived from the network. Ignored when it is not a valid remote player, is older than
 * the ring, or the frame is already known (the sender repeats unconfirmed pads in every message). Otherwise it is stored, the
 * "newest consecutive" frame moves forward over every frame now complete, and, if that frame had already run on a guess that
 * differs from this real pad, it becomes the first mismatch (the earliest one is kept). */
void rb_input_remote_put(int p, int frame, RbPad pad) {
    if (p < 0 || p >= g_nplayers || p == g_local_player || frame < 1) return;
    RemotePlayer *r = &g_remote[p];
    if (frame <= r->newest_consecutive - RB_RING + 1) return;      /* older than the ring */
    if (r->pad_frame[frame & RING_MASK] == frame) return;          /* a redundant copy */
    pad = rb_pad_rest(pad);
    r->pad[frame & RING_MASK] = pad; r->pad_frame[frame & RING_MASK] = frame;
    while (r->pad_frame[(r->newest_consecutive + 1) & RING_MASK] == r->newest_consecutive + 1) r->newest_consecutive++;
    if (r->guess_frame[frame & RING_MASK] == frame) {              /* this frame ran on a guess: was it right? */
        if (!rb_pad_equal(&r->guess[frame & RING_MASK], &pad) && (!g_first_mismatch_frame || frame < g_first_mismatch_frame)) g_first_mismatch_frame = frame;
        r->guess_frame[frame & RING_MASK] = 0;
    }
}
/* Newest frame such that player `p`'s pads for every frame up to it have arrived (0 = none, or `p` is not a valid player). */
int rb_input_remote_newest_consecutive_frame(int p) { return p >= 0 && p < g_nplayers ? g_remote[p].newest_consecutive : 0; }

/* The pad player `p` uses at `frame`. For the local player: the stored local pad (empty when there is none). For a remote
 * player: the real pad when it has arrived; otherwise a GUESS, the player's newest real pad before `frame` repeated (empty
 * when there is none in the last RB_RING frames), remembered so rb_input_remote_put can check it when the real pad arrives.
 * *predicted (may be NULL) is set to 1 for a guess, 0 otherwise. */
RbPad rb_input_get(int p, int frame, int *predicted) {
    static const RbPad zero;
    if (predicted) *predicted = 0;
    if (p < 0 || p >= g_nplayers) return zero;
    if (p == g_local_player) { RbPad out = zero; rb_input_local_get(frame, &out); return out; }
    RemotePlayer *r = &g_remote[p];
    if (r->pad_frame[frame & RING_MASK] == frame) return r->pad[frame & RING_MASK];
    /* not received: the newest real pad before it, repeated */
    RbPad guess = zero;
    for (int f = frame - 1; f >= 1 && f > frame - RB_RING; f--) if (r->pad_frame[f & RING_MASK] == f) { guess = r->pad[f & RING_MASK]; break; }
    r->guess[frame & RING_MASK] = guess; r->guess_frame[frame & RING_MASK] = frame;
    if (predicted) *predicted = 1;
    return guess;
}

/* Returns the earliest frame whose guess turned out wrong since the last call (0 = none) and forgets it: the caller rolls back to the state before that frame. */
int rb_input_take_mismatch(void) { int m = g_first_mismatch_frame; g_first_mismatch_frame = 0; return m; }
/* Gives a mismatch back (keeps the earlier one if there already is one): the rollback could not be done now (the state load was refused), so it is taken again next tick. */
void rb_input_put_mismatch(int m) { if (m > 0 && (!g_first_mismatch_frame || m < g_first_mismatch_frame)) g_first_mismatch_frame = m; }

/* The newest frame every player's real pad is known for: the local player's newest stored pad, lowered to each remote player's
 * newest consecutive frame. No rollback can reach back before this frame. */
int rb_input_finalized(void) {
    int finalized = g_local_newest_frame;
    for (int p = 0; p < g_nplayers; p++) if (p != g_local_player && g_remote[p].newest_consecutive < finalized) finalized = g_remote[p].newest_consecutive;
    return finalized;
}
