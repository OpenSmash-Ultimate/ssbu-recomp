/* Track R2 (TEST ONLY, default OFF): the SNAPSHOT RING for rollback, on top of R1's marked-page save (r1_state.c).
 * Included by r1_state.c (it uses that file's statics: the mark maps, the flat shadow of the first full save, the
 * thread holds, the fiber save, the parallel page copy). No game content in this file. Plan: plan\phases\R2_snapshot_ring.md.
 *
 * Design ("pointer shadow + undo log"):
 *  - every guest page the ring tracks has a global id: window 1 (the game's 62 GB region) pages, then window 2
 *    (image + modules) pages, then the "extras" (main's guest stack, TLS blocks, fighter DLL data, catch table, malloc
 *    arena: the ranges r1 copies whole);
 *  - g_rg_idx[id] says where the page's value AT THE NEWEST CAPTURE is: 0 = the flat shadow of the first full save
 *    (never written since), 1 = decommitted by the game, >= 2 = a 4 KB buffer of the page pool;
 *  - capture(f): every page marked since the previous capture (and every extras page that differs) gets a NEW pool
 *    buffer with its current bytes (one copy, done by main + helper threads); the slot of f keeps the list
 *    (id, previous idx) = an undo log: the buffers of the values at the previous capture are not copied, only moved
 *    into the log. Plus per slot: main's registers, the other threads' wait places and registers, the fibers' host
 *    state, the command buffers' recordings in progress (nvn_gl.c's save area, through the linker map);
 *  - load(g): the undo logs of the slots newer than g are applied newest first, pointers only (no copy); every page
 *    they name, every page marked since the newest capture and every extras page is then copied ONCE from its value at
 *    g into memory; the slots newer than g are dropped (g becomes the newest), older ones stay loadable;
 *  - the oldest slot is dropped when the ring is full (SSBU_RB_DEPTH, default 7 frames back = 8 states).
 * Public API (a future rb_rollback.c, at the safe point run-ahead uses, the kick after the submission):
 *    int r1_ring_capture(long long frame, Ctx *c);  1 = captured; 0 = not now (a thread not at a wait in time)
 *    int r1_ring_load(long long frame, Ctx *c);     1 = loaded (c = main's registers of that frame); 0 = not in the ring; -1 = refused for now (pub1: a thread was running)
 *    void r1_ring_release(void);                    every slot dropped
 *    long long r1_ring_oldest(void), r1_ring_newest(void);   (-1 = empty)
 *    double r1_ring_last_ms(int load);              the last capture / load in ms
 * Test driver (SSBU_RB_SIM=<N>[,<K>[,<warm>]]): a ROLLBACK SIMULATION in one process: every pass of the fight is
 * captured; every N real passes the state of k passes back is loaded (k = 1..K rotating) and those passes run again
 * with player 1's recorded pads, not presented (run-ahead's quiet phase); tests\rb_sim_cmp.py checks the real timeline
 * against a run without it and every re-run frame against the frame it replays. SSBU_RB_SIM_SKIP=<mask>: run-ahead's
 * kick-skip mask in re-run passes except the last one (experiment). SSBU_RB_PARK_MS (default 30): hold limit. */
#include "game_state_internal.h"
/* Functions and variables of other runtime files that only this file uses: */
void nnunwind_catch_reinsert(uint64_t cfa, uint64_t jb);
void nnunwind_catch_free(unsigned i);

extern volatile int g_rgp_seg;                            /* (R2 round 3, profile 3: which part of a pass main is in) */

/* (hs13) g_rg_nomem: 1 = the ring's base could not be made for lack of memory (nothing kept: tried again at the next match),
 * 2 = the ring broke on the way (its own allocations or a rebase failed: off for the rest of the process). rb_rollback.c then
 * plays the match without rollback (each frame waits for the other player's pad: delay-based, never a guess). */
int g_rg_nomem;

static RgSlot *g_rg; int g_rg_cap, g_rg_head, g_rg_count, g_rg_ready, g_rg_depth = 7;
uint32_t *g_rg_idx; static uint8_t *g_rg_touch; static uint32_t *g_rg_tl; static size_t g_rg_ntl, g_rg_tlcap;
size_t g_rg_n1, g_rg_n2, g_rg_nx;               /* page counts: window 1, window 2, extras */
uint8_t **g_rg_xa; static uint8_t **g_rg_xb;                   /* extras page e: its address, its flat (first save) bytes */
static uint8_t *g_rg_pool; uint32_t g_rg_bump, g_rg_pcom, *g_rg_free, g_rg_nfree;
static uint8_t *g_rg_cbs; static int *g_rg_ncbs; static int g_rg_cb_ok = -1;
static double g_rg_park_ms = 30;
int g_rg_keep, g_rg_kept, g_rg_keep_on = -1; long long g_rg_kept_n, g_rg_kept_same;   /* capture -> load at the same safe point: threads stay held */
long long g_rg_caps, g_rg_cap_fail, g_rg_loads, g_rg_overflow;
double g_rg_last[2];
int g_rgs_on; long long g_rgv_target;     /* (simulation / verify, below) */
/* costs: capture (all, hold, sweep + queue, copy, fibers / threads / cb), load per k */

double *g_rgc[5]; size_t *g_rgc_pg, *g_rgc_xp; int g_rgc_n;

double g_rgl[RG_MAXSLOT][RG_NL][3]; size_t g_rgl_pg[RG_MAXSLOT][RG_NL]; int g_rgl_n[RG_MAXSLOT];

static uint8_t *rg_vres(size_t n) { return VirtualAlloc(NULL, n, MEM_RESERVE, PAGE_READWRITE); }
static int rg_vcom(uint8_t *base, size_t *com, size_t need, size_t step) {   /* commit [0, need) of a reservation */
    if (need <= *com) return 1;
    size_t to = (need + step - 1) / step * step;
    if (!VirtualAlloc(base + *com, to - *com, MEM_COMMIT, PAGE_READWRITE)) return 0;
    *com = to; return 1;
}
/* (hs14) THE POOL RUNNING OUT IN A MATCH. Before: a capture that found no pool buffer (or no log room) went on without the
 * rest of its pages (their marks already cleared by the sweep), then dropped every older state: a later rollback could find
 * no state, or load a newest state with stale pages (= a desync). Now (SSBU_RB_POOLFIX=0: the old way):
 *  - g_rg_need = the oldest frame a rollback may still need (rb_rollback.c: the newest frame with both players' pads known;
 *    -1 = not known): the newest slot at or before it and every newer one are never dropped;
 *  - out of buffers in a capture: first the slots older than that one are dropped (their buffers go back to the pool);
 *  - still none: the capture is GIVEN UP cleanly: the pages it took go back to their previous values, every page it swept
 *    is marked again, nothing else changes (as a capture skipped for a thread not at its wait); g_rg_short tells
 *    rb_rollback.c, which then runs no frame on a guess (each real pass waits for the other player's pad) until a capture
 *    succeeds again: no rollback is ever needed for a state the ring could not keep;
 *  - the ring full with its oldest slot the only one at or before g_rg_need: the capture is skipped instead of dropping it.
 * Test (forces it): SSBU_RB_NET_TEST_POOL=<from frame>,<frames>[,<mode>]: captures of those frames get at most 64 buffers;
 * mode 1 (default): then only buffers given back to the pool, 2: none at all. */
long long g_rg_loading_until = -1;
long long g_rg_need = -1; int g_rg_poolfix = -1, g_rg_short; long long g_rg_short_n, g_rg_freed_slots, g_rg_skip_full;
static long long g_rg_tp_f = -2, g_rg_tp_n; static int g_rg_tp_mode, g_rg_tp_active, g_rg_tp_gets; long long g_rg_tp_refused;
static uint32_t rg_pool_get(void) {
    if (g_rg_tp_active && ++g_rg_tp_gets > 64) {          /* (test) */
        if (g_rg_tp_mode != 2 && g_rg_nfree) return g_rg_free[--g_rg_nfree];
        g_rg_tp_refused++; return 0;
    }
    if (g_rg_nfree) return g_rg_free[--g_rg_nfree];
    if (g_rg_bump >= RG_POOL_MAX) return 0;
    if (g_rg_bump >= g_rg_pcom) {
        if (!VirtualAlloc(g_rg_pool + (size_t)g_rg_pcom * PG, 256 * PG, MEM_COMMIT, PAGE_READWRITE)) return 0;
        g_rg_pcom += 256;
    }
    return 2 + g_rg_bump++;
}
static void rg_pool_put(uint32_t i) { if (i >= 2) g_rg_free[g_rg_nfree++] = i; }
static uint8_t *rg_pool_ptr(uint32_t i) { return g_rg_pool + (size_t)(i - 2) * PG; }
uint8_t *rg_addr(uint32_t id) {
    if (id < g_rg_n1) return g_win1 + (size_t)id * PG;
    if (id < g_rg_n1 + g_rg_n2) return (uint8_t *)(uintptr_t)WIN2 + (size_t)(id - g_rg_n1) * PG;
    return g_rg_xa[id - g_rg_n1 - g_rg_n2];
}
/* the bytes of page `id` as recorded at the newest capture (NULL = zero page, (uint8_t *)1 = decommitted) */
uint8_t *rg_src(uint32_t id) {
    uint32_t v = g_rg_idx[id];
    if (v >= 2) return rg_pool_ptr(v);
    if (v == RG_DECOM) return (uint8_t *)1;
    if (id < g_rg_n1) return g_sh1c[id] ? g_sh1 + (size_t)id * PG : NULL;
    if (id < g_rg_n1 + g_rg_n2) { size_t j = id - g_rg_n1; return g_sh2c[j] ? g_sh2 + j * PG : NULL; }
    return g_rg_xb[id - g_rg_n1 - g_rg_n2];
}
static int rg_push(RgSlot *s, uint32_t id, uint32_t old) {
    if (!rg_vcom((uint8_t *)s->ent, &s->entcom, (s->nent + 1) * 8, 1u << 20)) return 0;
    s->ent[2 * s->nent] = id; s->ent[2 * s->nent + 1] = old; s->nent++;
    return 1;
}
static void rg_touch(uint32_t id) {
    if (g_rg_touch[id]) return;
    if (g_rg_ntl >= g_rg_tlcap) { g_rg_overflow++; return; }
    g_rg_touch[id] = 1; g_rg_tl[g_rg_ntl++] = id;
}
/* every page marked since the last sweep, in both windows (the same block lists as mark_sweep); marks cleared */
static void rg_sweep(void (*fn)(uint32_t id, int w, size_t j)) {
    if (g_full_sweep < 0) g_full_sweep = getenv("SSBU_R1_FULL_SWEEP") != NULL;
    if (!g_blk1 && !g_full_sweep && g_win1) blk1_init();
    if (g_blk1 && g_blk1_dirty) blk1_rebuild();
    for (int w = 0; w < 2; w++) {
        uint8_t *map = w ? *pm2 : *pm1; size_t np = (size_t)((w ? WN2 : g_wn1) >> 12);
        uint8_t *mem = w ? (uint8_t *)(uintptr_t)WIN2 : g_win1;
        uint64_t *q = (uint64_t *)map;
        uint32_t *bl = !g_blk1 ? NULL : w ? g_blk2_list : g_blk1_list;
        size_t nq = !bl ? np / 8 : (w ? g_blk2_n : g_blk1_n) * 8;
        for (size_t qi = 0; qi < nq; qi++) {
            size_t i = !bl ? qi : (size_t)bl[qi >> 3] * 8 + (qi & 7);
            if (!q[i]) continue;
            for (size_t j = i * 8; j < i * 8 + 8; j++) {
                if (!map[j]) continue;
                map[j] = 0;
                if (g_audio_free && is_async(mem + j * PG)) continue;
                if (w ? g_ns2[j] : g_ns1[j]) continue;          /* not simulation */
                fn((uint32_t)(w ? g_rg_n1 + j : j), w, j);
            }
        }
    }
}
static RgSlot *g_rg_cur; static int g_rg_fail_now; int g_rgs_left;
/* Pages the capture in progress has recorded so far (0 when none is in progress). rb_learned.c uses it to count the pages it adds. */
long long rg_current_slot_pages(void) { return g_rg_cur ? (long long)g_rg_cur->pages : 0; }
/* R4: the learned-set save (the ring on the normal, unmarked game build) is rb_learned.c, its own translation unit (r1_internal.h) */
/* (R2 round 3) SSBU_RB_SIM_PAGES=1: the captured pages counted per 1 MB block and pass kind (0 real, 1 re-run with the skips, 2 the
 * last re-run pass): which memory a frame writes, and which of it only the presentation writes (absent from the skipped passes). */
int g_rgpg_on = -1; static int g_rgpg_kind; long long g_rgpg_caps[3];

 RgPageCount g_rgpg[RGPG_N];
static void rgpg_note(uint8_t *a) {
    uint64_t b = (uint64_t)(uintptr_t)a >> 20; unsigned h = (unsigned)((b * 0x9e3779b97f4a7c15ull) >> 50) % RGPG_N;
    for (int i = 0; i < 64; i++) { unsigned k = (h + (unsigned)i) % RGPG_N; if (g_rgpg[k].blk == b) { g_rgpg[k].n[g_rgpg_kind]++; return; } if (!g_rgpg[k].blk) { g_rgpg[k].blk = b; g_rgpg[k].n[g_rgpg_kind]++; return; } }
}
static uint32_t rg_pool_get_cap(void); static void rg_remark(uint32_t id);
static void rg_cap_page(uint32_t id, int w, size_t j) {
    if (g_rg_fail_now) { if (g_rg_poolfix) rg_remark(id); return; }
    uint32_t old = g_rg_idx[id];
    if (!w && g_dc[j]) {                                        /* decommitted by the game: its value is "decommitted" */
        if (old == RG_DECOM) return;
        if (!rg_push(g_rg_cur, id, old)) { g_rg_fail_now = 1; if (g_rg_poolfix) rg_remark(id); return; }
        g_rg_idx[id] = RG_DECOM; g_rg_cur->pages++;
        return;
    }
    uint32_t nb = rg_pool_get_cap();
    if (!nb || !rg_push(g_rg_cur, id, old)) { if (nb) rg_pool_put(nb); g_rg_fail_now = 1; if (g_rg_poolfix) rg_remark(id); return; }
    cp_add(rg_pool_ptr(nb), rg_addr(id));
    g_rg_idx[id] = nb; g_rg_cur->pages++;
    if (g_rgpg_on > 0) rgpg_note(rg_addr(id));
}
static void rg_load_page(uint32_t id, int w, size_t j) { (void)w; (void)j; rg_touch(id); }
static void rg_remark(uint32_t id) {                     /* (hs14) a window page marked as written again (a capture given up) */
    if (id < g_rg_n1) (*pm1)[id] = 1; else if (id < g_rg_n1 + g_rg_n2) (*pm2)[id - g_rg_n1] = 1;
}
static void rg_slot_drop(RgSlot *s, int log_too) {     /* log_too: the values in its undo log are no longer reachable */
    if (log_too) for (size_t i = 0; i < s->nent; i++) rg_pool_put(s->ent[2 * i + 1]);
    s->nent = 0; s->frame = -1;
}

/* (hs14) the oldest slots no rollback can need (a newer slot at or before g_rg_need exists) dropped; their buffers back to the pool */
static int rg_free_unneeded(void) {
    int n = 0;
    if (g_rg_need < 0) return 0;
    while (g_rg_count >= 2 && rg_slot(1)->frame <= g_rg_need) { rg_slot_drop(rg_slot(0), 1); g_rg_head = (g_rg_head + 1) % g_rg_cap; g_rg_count--; n++; }
    g_rg_freed_slots += n;
    return n;
}
static uint32_t rg_pool_get_cap(void) {                  /* a buffer for the capture now running */
    uint32_t nb = rg_pool_get();
    if (!nb && g_rg_poolfix && rg_free_unneeded()) nb = rg_pool_get();
    return nb;
}
static void rg_cb_find(void) {
    if (g_rg_cb_ok >= 0) return;
    uint64_t a = map_sym("g_cbs"), n = map_sym("g_ncbs"), all = map_sym("g_all_cb");
    size_t nvn_gl_r1_cb_size(void);                      /* (R3 pub1) the entry size from nvn_gl.c itself: the compiler of the package exe put g_ncbs between the two arrays */
    g_rg_cb_ok = a && n && all && (all - a == 512ull * RG_CBS_SIZE || nvn_gl_r1_cb_size() == RG_CBS_SIZE);
    if (g_rg_cb_ok) { g_rg_cbs = (uint8_t *)(uintptr_t)a; g_rg_ncbs = (int *)(uintptr_t)n; }
    LOG("r1: RING: command-buffer save area of nvn_gl.c %s (g_cbs %llx, g_ncbs %llx, g_all_cb %llx)\n", g_rg_cb_ok ? "found: saved per slot" : "NOT found (layout differs): the newest capture's is put back at every load",
        (unsigned long long)a, (unsigned long long)n, (unsigned long long)all);
}
static int rg_cb_capture(RgSlot *s) {
    s->ncbs = 0; s->cbd_used = 0;
    if (!g_rg_cb_ok) return 1;
    int n = *g_rg_ncbs; if (n < 0 || n > 512) return 0;
    memcpy(s->cbs, g_rg_cbs, (size_t)n * RG_CBS_SIZE);
    for (int i = 0; i < n; i++) {
        uint8_t *e = g_rg_cbs + (size_t)i * RG_CBS_SIZE;
        size_t len = *(size_t *)(e + 16); uint8_t *d = *(uint8_t **)(e + 128);
        if (!*(void **)e || !len || !d) continue;
        if (!rg_vcom(s->cbd, &s->cbd_com, s->cbd_used + len, 1u << 20)) return 0;
        memcpy(s->cbd + s->cbd_used, d, len); s->cbd_used += len;
    }
    s->ncbs = n;
    return 1;
}
static void rg_cb_load(RgSlot *s) {
    if (g_rg_cb_ok) {
        size_t off = 0;
        for (int i = 0; i < s->ncbs; i++) {
            uint8_t *e = g_rg_cbs + (size_t)i * RG_CBS_SIZE, *se = s->cbs + (size_t)i * RG_CBS_SIZE;
            uint8_t *d = *(uint8_t **)(e + 128); size_t cap = *(size_t *)(e + 136);
            memcpy(e, se, 128);                                 /* cb + the record's copy; the save area's own buffer stays */
            size_t len = *(size_t *)(se + 16);
            if (*(void **)se && len && *(uint8_t **)(se + 128)) {
                if (cap < len) { cap = len + 65536; d = realloc(d, cap); }
                memcpy(d, s->cbd + off, len); off += len;
            }
            *(uint8_t **)(e + 128) = d; *(size_t *)(e + 136) = cap;
        }
        *g_rg_ncbs = s->ncbs;
    }
    nvn_r1_cb_restore();
}
/* ---- QUIET STATES (option 1). A rollback can only be undone cleanly when, at the state it goes back to, every Windows "worker" (the job fibers the game's scripts run on) was at one of its
 * usual pauses: the host side of a fiber (its stack, its place in the job system) is not part of the memory the ring rewinds, and a state taken while a fiber stood somewhere unusual (in the
 * middle of a script shutting down, say) is the kind after which the real matches crashed or froze. Each capture is marked quiet or not:
 *   - every guest thread the capture held is at a wait (parked);
 *   - every fiber that has run stands at a known pause site (the import it is in and the call site). The sites of the first capture of a fight are known; a new site becomes known once it has
 *     been seen in QS_LEARN captures in a row (a new character's normal pause), so a fight does not stay "noisy" for ever.
 * A load takes the newest QUIET state at or before the frame it needs; with none, the newest state as before (a rollback is never refused: that would desync an online match).
 * SSBU_RB_QUIET=0: off (always the newest state). The counts go to the log, which is also the measurement of how often states are noisy and why. */
#define QS_MAX 64
#define QS_LEARN 30
static struct { const char *imp; uint64_t from; int known; long long streak, last_cap; } g_qs[QS_MAX]; static int g_nqs;
static long long g_qs_capno, g_qs_quiet, g_qs_noisy, g_qs_loads_quiet, g_qs_loads_older, g_qs_loads_fallback, g_qs_extra;
static int g_qs_on = -1;
static int qs_same(const char *a, const char *b) { return a == b || (a && b && !strcmp(a, b)); }
static void qs_mark(RgSlot *s) {
    if (g_qs_on < 0) { const char *e = getenv("SSBU_RB_QUIET"); g_qs_on = !(e && e[0] == '0'); }
    s->qset = 1; s->quiet = 1;
    if (!g_qs_on) return;
    int first = g_nqs == 0; g_qs_capno++;
    const char *why = NULL; char detail[160] = "";
    for (int i = 0; i < g_guest_threads.count; i++) {
        Th *t = &g_guest_threads.table[i];
        if (t->held && !t->parked && !why) { why = "a thread not at a wait"; snprintf(detail, sizeof detail, "%s in %s", t->name ? t->name : "?", t->c && t->c->in_import ? t->c->in_import : "guest code"); }
    }
    for (int i = 0; i < g_guest_fibers.count; i++) {
        Fib *f = &g_guest_fibers.table[i];
        if (!f->c) continue;                                  /* set up, never run */
        int k; for (k = 0; k < g_nqs; k++) if (qs_same(g_qs[k].imp, f->imp) && g_qs[k].from == f->from) break;
        if (k == g_nqs) { if (g_nqs >= QS_MAX) continue; g_qs[k].imp = f->imp; g_qs[k].from = f->from; g_qs[k].known = first; g_qs[k].streak = 0; g_qs[k].last_cap = 0; g_nqs++; }
        if (g_qs[k].last_cap != g_qs_capno) {
            g_qs[k].streak = g_qs[k].last_cap == g_qs_capno - 1 ? g_qs[k].streak + 1 : 1; g_qs[k].last_cap = g_qs_capno;
            if (!g_qs[k].known && g_qs[k].streak >= QS_LEARN) { g_qs[k].known = 1; LOG("r1: QUIET: pause site %s @%llx learned (seen in %d captures in a row)\n", f->imp ? f->imp : "guest code", (unsigned long long)f->from, QS_LEARN); }
        }
        if (!g_qs[k].known && !why) { why = "a fiber at an unusual place"; snprintf(detail, sizeof detail, "%s @%llx", f->imp ? f->imp : "guest code", (unsigned long long)f->from); }
    }
    s->quiet = why == NULL;
    if (s->quiet) g_qs_quiet++; else { g_qs_noisy++; if (g_qs_noisy <= 20) LOG("r1: QUIET: state of frame %lld is NOT quiet: %s (%s)\n", s->frame, why, detail); }
}
/* The state a rollback to `target` loads (see above). Every load site uses it: the online controller, the forced-rollback simulation, the safety net. */
long long rg_best_state(long long target) {
    long long best = -1, bq = -1;
    for (int i = 0; i < g_rg_count; i++) {
        RgSlot *sl = rg_slot(i); long long f = sl->frame;
        if (f < 0 || f > target) continue;
        if (f > best) best = f;
        if ((!sl->qset || sl->quiet) && f > bq) bq = f;
    }
    long long n = g_qs_loads_quiet + g_qs_loads_fallback + 1;
    if (bq >= 0) {
        g_qs_loads_quiet++;
        if (bq != best) { g_qs_loads_older++; g_qs_extra += best - bq; if (g_qs_loads_older <= 20) LOG("r1: QUIET: rollback to %lld: the newest state (%lld) is not quiet, state %lld loaded instead (%lld more frames again)\n", target, best, bq, best - bq); }
    } else if (best >= 0) { g_qs_loads_fallback++; if (g_qs_loads_fallback <= 20) LOG("r1: QUIET: rollback to %lld: no quiet state at or before it in the ring: the newest (%lld) loaded as before\n", target, best); }
    if (n % 100 == 0) rg_quiet_report();
    return bq >= 0 ? bq : best;
}
void rg_quiet_report(void) {
    LOG("r1: QUIET: captures quiet %lld / not quiet %lld; loads from a quiet state %lld (%lld of them an older one than the newest, %lld frames run again more in all), loads with no quiet state %lld\n",
        g_qs_quiet, g_qs_noisy, g_qs_loads_quiet, g_qs_loads_older, g_qs_extra, g_qs_loads_fallback);
}
/* ---- PAGES COMMITTED OUTSIDE THE RING DURING THE FIGHT (the general case of the fiber guest stacks above). The third big reservation is saved as committed at the ring's base; what the game
 * commits in it during the fight (new objects, coroutine stacks: 400-700 pages in a fight) was never saved nor restored. An object DESTROYED inside a rolled-back window stayed destroyed after the
 * load while everything around it went back to when it was alive: Steve's blocks (2026-10-28) made the render dispatch threads call a virtual method of such an object and land in its base class's
 * abstract entry, which calls std::terminate. Each capture now copies every such page (the list page_tracker_learned.c keeps: g_xls_out_pg), a load writes them back (committing a page the game
 * decommitted since). The fibers' guest stacks are left to rg_gstk_load and its checks (a running fiber's stack is not written under it). 2-3 MB per capture. SSBU_R1_OUTPAGES=0: off. */
extern uint64_t g_xls_out_pg[]; extern volatile LONG g_xls_out_npg;
int fibers_stack_ranges(uint64_t *lo, uint64_t *hi, int max);   /* threads.c */
#define OPH 16                                               /* entry: page address, (pad), then the 4 KB */
static int g_opg_on = -1; static long long g_opg_caps, g_opg_pages, g_opg_gone, g_opg_full, g_opg_loads, g_opg_written, g_opg_skip, g_opg_recommit;
static void opg_copy_page(uint8_t *d, const uint8_t *s) { memcpy(d, s, PG); }
static void (*volatile g_opg_cp)(uint8_t *, const uint8_t *) = opg_copy_page;      /* (through a pointer the compiler cannot see through: __try then covers the copy, write_marks.c) */
static int opg_try_copy(uint8_t *d, const uint8_t *s) { __try { g_opg_cp(d, s); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static void rg_opg_capture(RgSlot *s) {
    if (g_opg_on < 0) { const char *e = getenv("SSBU_R1_OUTPAGES"); g_opg_on = !(e && e[0] == '0'); }
    s->opg_used = 0;
    if (!g_opg_on || !s->opg) return;
    int n = (int)g_xls_out_npg; if (n > 65536) n = 65536;
    for (int i = 0; i < n; i++) {
        uint64_t a = g_xls_out_pg[i]; if (!a) continue;
        size_t need = s->opg_used + OPH + PG;
        if (!rg_vcom(s->opg, &s->opg_com, need, 1u << 20)) { g_opg_full++; break; }
        uint8_t *o = s->opg + s->opg_used;
        if (!opg_try_copy(o + OPH, (const uint8_t *)(uintptr_t)a)) { g_opg_gone++; g_xls_out_pg[i] = 0; continue; }      /* not committed now: not part of this state; off the list (a later commit puts it back on: page_tracker_learned.c) */
        ((uint64_t *)o)[0] = a; ((uint64_t *)o)[1] = 0;
        s->opg_used = need;
    }
    g_opg_caps++; g_opg_pages += (long long)(s->opg_used / (OPH + PG));
    if (g_opg_caps == 1 || g_opg_caps % 2000 == 0)
        LOG("r1: OUTPAGES: %lld captures, %.0f pages each on average (%.1f MB), %lld not committed at a capture, %lld out of room; loads %lld: %lld pages written back, %lld left to the fiber stacks, %lld committed again\n",
            g_opg_caps, (double)g_opg_pages / g_opg_caps, (double)g_opg_pages / g_opg_caps * PG / 1048576.0, g_opg_gone, g_opg_full, g_opg_loads, g_opg_written, g_opg_skip, g_opg_recommit);
}
static void rg_opg_load(const RgSlot *s) {
    if (!g_opg_on || !s->opg || !s->opg_used) return;
    static uint64_t lo[2048], hi[2048]; int nr = fibers_stack_ranges(lo, hi, 2048);
    for (size_t off = 0; off + OPH + PG <= s->opg_used; off += OPH + PG) {
        const uint8_t *o = s->opg + off; uint64_t a = ((const uint64_t *)o)[0];
        int in_stack = 0; for (int r = 0; r < nr && !in_stack; r++) if (a + PG > lo[r] && a < hi[r]) in_stack = 1;
        if (in_stack) { g_opg_skip++; continue; }
        if (!opg_try_copy((uint8_t *)(uintptr_t)a, o + OPH)) {                                          /* decommitted since the capture: commit it again */
            if (VirtualAlloc((void *)(uintptr_t)a, PG, MEM_COMMIT, PAGE_READWRITE) && opg_try_copy((uint8_t *)(uintptr_t)a, o + OPH)) g_opg_recommit++;
            continue;
        }
        g_opg_written++;
    }
    g_opg_loads++;
}
/* ---- THE FIBERS' GUEST STACKS (the root cause of the post-rollback crashes, 2026-10-28). A coroutine made after the ring's base gets its guest stack in pages committed after it, which the ring
 * never saves (the third big reservation is "extras" as committed at the base): the failing throw's sp, x29 and return-address slot were all in such a stack, "NOT saved by the ring". A load put
 * the coroutine's registers and host stack back to the capture, while its guest stack still held what it wrote AFTER the capture: resumed, it read frames from the future (a wrong x29 / return
 * address: the throw that could not be walked, the cascades, Lua's index2value). Each capture now copies, for every fiber that has run, the live part of its guest stack [sp, top); a load writes
 * it back. Tens of fibers with shallow stacks: a few hundred KB per capture. SSBU_R1_GSTACK=0: off. */
static int g_gstk_on = -1; static long long g_gstk_caps, g_gstk_bytes, g_gstk_fail, g_gstk_loads;
static long long g_gstk_skip;
#define GSH 48                                              /* entry header: key, stack lo, stack hi, copy start, copy length, (pad) */
static void rg_gstk_capture(RgSlot *s) {
    if (g_gstk_on < 0) { const char *e = getenv("SSBU_R1_GSTACK"); g_gstk_on = !(e && e[0] == '0'); }
    s->gstk_used = 0;
    if (!g_gstk_on || !s->gstk) return;
    for (int i = 0; i < s->nfib; i++) {
        Fib *f = &s->fib[i];
        if (!f->c) continue;                                    /* never ran: nothing on its stack yet */
        uint64_t lo = f->obj[4], hi = f->obj[4] + f->obj[5], sp = f->sp;
        if (!f->obj[5] || sp < lo || sp >= hi) continue;
        uint64_t a = sp & ~(uint64_t)15; size_t n = (size_t)(hi - a), need = s->gstk_used + GSH + ((n + 15) & ~(size_t)15);
        if (n > (16u << 20) || !rg_vcom(s->gstk, &s->gstk_com, need, 1u << 20)) { g_gstk_fail++; break; }
        uint64_t *h = (uint64_t *)(s->gstk + s->gstk_used);
        h[0] = f->key; h[1] = lo; h[2] = hi; h[3] = a; h[4] = n; h[5] = 0;
        memcpy((uint8_t *)h + GSH, (const void *)(uintptr_t)a, n);
        s->gstk_used = need;
    }
    g_gstk_caps++; g_gstk_bytes += (long long)s->gstk_used;
    if (g_gstk_caps == 1 || g_gstk_caps % 2000 == 0) LOG("r1: GSTACK: %lld captures, %.1f KB of fiber guest stacks each on average, %lld that ran out of room, %lld stacks not written back at loads (in use / not put back)\n", g_gstk_caps, g_gstk_bytes / 1024.0 / g_gstk_caps, g_gstk_fail, g_gstk_skip);
}
int fibers_live_sp_in(uint64_t lo, uint64_t hi);          /* threads.c */
/* Called right after fibers_restore_all. A fiber's guest stack is written back only when the load put that fiber back (its registers / host stack are the capture's too), and never under a
 * context that runs now on that memory (a fiber born since, or one not put back, using a stack the game reused): that clobbered live frames (a dense run: both render dispatch threads
 * called std::terminate after the first version of this wrote stacks unconditionally). */
static void rg_gstk_load(const RgSlot *s) {
    if (!g_gstk_on || !s->gstk) return;
    for (size_t off = 0; off + GSH <= s->gstk_used; ) {
        const uint64_t *h = (const uint64_t *)(s->gstk + off); uint64_t key = h[0], lo = h[1], hi = h[2], a = h[3]; size_t n = (size_t)h[4];
        off += GSH + ((n + 15) & ~(size_t)15);
        int i; for (i = 0; i < g_guest_fibers.count; i++) if (g_guest_fibers.table[i].key == key) break;
        /* only a fiber put back WHOLE (switched away at the capture and now: its host stack and registers are the capture's): a fiber that was running on a thread at its usual wait gets its
         * registers only, its host side stays where it is now, and old locals written under it (a render dispatch thread's job loop) made the game call std::terminate */
        if (i == g_guest_fibers.count || g_guest_fibers.table[i].rok != 2 || fibers_live_sp_in(lo, hi)) { g_gstk_skip++; continue; }
        memcpy((void *)(uintptr_t)a, (const uint8_t *)h + GSH, n);
    }
    g_gstk_loads++;
}
static void rg_meta_capture(RgSlot *s, Ctx *c) {       /* threads still held: their places, the fibers, the command buffers */
    s->ctx = *c;
    nnsdk_tls_save(s->tls);
    s->nth = g_guest_threads.count; s->ninfo = g_guest_threads.info_count;
    memcpy(s->th, g_guest_threads.table, (size_t)g_guest_threads.count * sizeof *g_guest_threads.table);
    for (int i = 0; i < s->nth; i++) { s->th[i].held = 0; s->th[i].cok = 0; }
    s->nfib = g_guest_fibers.count; memcpy(s->fib, g_guest_fibers.table, (size_t)g_guest_fibers.count * sizeof *g_guest_fibers.table);
    s->fpool_used = 0;
    if (g_guest_fibers.pool_used && rg_vcom(s->fpool, &s->fpool_com, g_guest_fibers.pool_used, 1u << 20)) { memcpy(s->fpool, g_guest_fibers.pool, g_guest_fibers.pool_used); s->fpool_used = g_guest_fibers.pool_used; }
    else if (g_guest_fibers.pool_used) for (int i = 0; i < s->nfib; i++) s->fib[i].susp = 0;
    if (!rg_cb_capture(s)) s->ncbs = 0;
    rg_gstk_capture(s);
    rg_opg_capture(s);
    qs_mark(s);
}
static void rg_free_all(void) {                         /* (hs13) a ring that could not be made: what it took is given back */
    if (g_rg_idx) VirtualFree(g_rg_idx, 0, MEM_RELEASE);
    if (g_rg_touch) VirtualFree(g_rg_touch, 0, MEM_RELEASE);
    if (g_rg_tl) VirtualFree(g_rg_tl, 0, MEM_RELEASE);
    if (g_rg_pool) VirtualFree(g_rg_pool, 0, MEM_RELEASE);
    if (g_rg_free) VirtualFree(g_rg_free, 0, MEM_RELEASE);
    free(g_rg_xa); free(g_rg_xb);
    for (int i = 0; g_rg && i < g_rg_cap; i++) {
        RgSlot *s = &g_rg[i];
        if (s->ent) VirtualFree(s->ent, 0, MEM_RELEASE);
        if (s->fpool) VirtualFree(s->fpool, 0, MEM_RELEASE);
        if (s->gstk) VirtualFree(s->gstk, 0, MEM_RELEASE);
        if (s->opg) VirtualFree(s->opg, 0, MEM_RELEASE);
        if (s->cbd) VirtualFree(s->cbd, 0, MEM_RELEASE);
        free(s->fib); free(s->cbs);
    }
    free(g_rg);
    g_rg_idx = g_rg_tl = g_rg_free = NULL; g_rg_touch = g_rg_pool = NULL; g_rg_xa = g_rg_xb = NULL; g_rg = NULL;
    g_rg_pcom = 0; g_rg_bump = 0; g_rg_nfree = 0;
}
static int rg_init(void) {
    g_rg_n1 = g_win1 ? (size_t)(g_wn1 >> 12) : 0; g_rg_n2 = (size_t)(WN2 >> 12);
    g_rg_nx = 0; for (int i = 0; i < g_nx; i++) g_rg_nx += g_x[i].n / PG;
    size_t tot = g_rg_n1 + g_rg_n2 + g_rg_nx;
    { const char *e = getenv("SSBU_RB_DEPTH"); if (e) g_rg_depth = atoi(e); if (g_rg_depth < 1) g_rg_depth = 1; if (g_rg_depth > RG_MAXSLOT - 1) g_rg_depth = RG_MAXSLOT - 1; }
    g_rg_cap = g_rg_depth + 1;
    g_rg_idx = VirtualAlloc(NULL, tot * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rg_touch = VirtualAlloc(NULL, tot, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rg_tlcap = 8u << 20; g_rg_tl = VirtualAlloc(NULL, g_rg_tlcap * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rg_pool = rg_vres((size_t)RG_POOL_MAX * PG);
    g_rg_free = VirtualAlloc(NULL, (size_t)RG_POOL_MAX * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_rg_xa = malloc((g_rg_nx + 1) * sizeof *g_rg_xa); g_rg_xb = malloc((g_rg_nx + 1) * sizeof *g_rg_xb);
    g_rg = calloc((size_t)g_rg_cap, sizeof *g_rg);
    if (rb_test_nomem() == 3 || !g_rg_idx || !g_rg_touch || !g_rg_tl || !g_rg_pool || !g_rg_free || !g_rg_xa || !g_rg_xb || !g_rg) { LOG("r1: RING: allocation FAILED%s\n", rb_test_nomem() == 3 ? " (TEST: SSBU_RB_NET_TEST_NOMEM=3)" : ""); rg_free_all(); return 0; }
    size_t e = 0;
    for (int i = 0; i < g_nx; i++) for (size_t o = 0; o < g_x[i].n; o += PG) { g_rg_xa[e] = g_x[i].a + o; g_rg_xb[e] = g_x[i].b + o; e++; }
    for (int i = 0; i < g_rg_cap; i++) {
        RgSlot *s = &g_rg[i]; s->frame = -1;
        s->ent = (uint32_t *)rg_vres(RG_ENT_RES); s->fpool = rg_vres(RG_FP_RES); s->cbd = rg_vres(RG_CB_RES); s->gstk = rg_vres(RG_GS_RES); s->gstk_used = s->gstk_com = 0; s->opg = rg_vres(RG_OP_RES); s->opg_used = s->opg_com = 0;
        s->fib = malloc(1024 * sizeof *s->fib); s->cbs = malloc(512 * RG_CBS_SIZE);
        if (!s->ent || !s->fpool || !s->cbd || !s->fib || !s->cbs) { LOG("r1: RING: slot allocation FAILED\n"); rg_free_all(); return 0; }
    }
    for (int k = 0; k < 5; k++) if (!g_rgc[k]) g_rgc[k] = calloc(RG_NST, sizeof(double));
    if (!g_rgc_pg) g_rgc_pg = calloc(RG_NST, sizeof(size_t));
    if (!g_rgc_xp) g_rgc_xp = calloc(RG_NST, sizeof(size_t));
    for (int k = 0; k < 5; k++) if (!g_rgc[k]) { LOG("r1: RING: statistics allocation FAILED\n"); rg_free_all(); return 0; }
    if (!g_rgc_pg || !g_rgc_xp) { LOG("r1: RING: statistics allocation FAILED\n"); rg_free_all(); return 0; }
    if (getenv("SSBU_RB_PARK_MS")) g_rg_park_ms = atof(getenv("SSBU_RB_PARK_MS"));
    rg_cb_find(); cp_init();
    LOG("r1: RING on: depth %d frames (%d states), pages: window 1 %zu, window 2 %zu, extras %zu (%.1f MB); pool up to %.0f GB; hold limit %.1f ms\n",
        g_rg_depth, g_rg_cap, g_rg_n1, g_rg_n2, g_rg_nx, g_rg_nx * PG / 1048576.0, (double)RG_POOL_MAX * PG / 1073741824.0, g_rg_park_ms);
    return 1;
}
RgSlot *rg_slot(int i) { return &g_rg[(g_rg_head + i) % g_rg_cap]; }     /* i = 0 oldest */
void r1_ring_let_go(void) { if (g_rg_kept) { unpark(); g_rg_kept = 0; } }      /* after a capture made with g_rg_keep and no load */
/* (R3 hs11) the threads held for a load that follows at once, WITHOUT a capture of the state that is about to be thrown away
 * (the load finds what was written since the newest capture by its own sweep); r1_ring_load / r1_ring_let_go lets them go */
int r1_ring_hold(void) {
    if (!g_rg_ready || g_rg_kept) return g_rg_kept;
    if (!ra_hold_quiet(g_rg_park_ms)) return 0;
    g_rg_kept = 1; return 1;
}
long long r1_ring_oldest(void) { return g_rg_count ? rg_slot(0)->frame : -1; }
long long r1_ring_newest(void) { return g_rg_count ? rg_slot(g_rg_count - 1)->frame : -1; }
double r1_ring_last_ms(int load) { return g_rg_last[load ? 1 : 0]; }
void r1_ring_release(void) {                              /* every slot dropped: the newest values stay (g_rg_idx), the logs' old values go back to the pool */
    for (int i = 0; i < g_rg_count; i++) rg_slot_drop(rg_slot(i), 1);
    g_rg_count = 0; g_rg_head = 0;
}
/* (R3 hs16, test) the metadata parts of captures / loads, ms (printed with SSBU_RB_HOLD_PROF) */
static double g_mp[9], g_mp_b[2]; static long long g_mp_n[2];
void rg_meta_prof_report(void) {
    if (g_hp_on <= 0) return;
    double a = g_mp_n[0] ? (double)g_mp_n[0] : 1, b = g_mp_n[1] ? (double)g_mp_n[1] : 1;
    LOG("r1: META PROF captures %lld: fibers %.4f (fiber pool %.0f KB), command buffers %.4f (%.0f KB), threads %.4f, let go %.4f ms; loads %lld: thread registers %.4f, fibers %.4f, catch table %.4f, command buffers %.4f, let go %.4f ms\n",
        g_mp_n[0], g_mp[0] / a, g_mp_b[0] / a / 1024, g_mp[1] / a, g_mp_b[1] / a / 1024, g_mp[2] / a, g_mp[3] / a, g_mp_n[1], g_mp[4] / b, g_mp[5] / b, g_mp[6] / b, g_mp[7] / b, g_mp[8] / b);
}
int r1_ring_capture(long long frame, Ctx *c) {
    R1_CTR("capture (frame)", frame, 0);
    double t0 = now_ms(); int seg_was = g_rgp_seg; g_rgp_seg = 4;
    if (!g_rg_ready) {                                    /* the first time: r1's full save (shadow filled, marks on: one hitch), the ring's base */
        g_rgp_seg = seg_was;
        if (g_rg_nomem) return 0;                         /* (hs13) out of memory: not tried again in this match */
        g_save_failed = 0; g_ra_wd_pause = 1; g_rs_drop = 1;
        { const char *e = getenv("SSBU_RB_BASE_DIRECT"); g_rs_direct = !(e && e[0] == '0'); }     /* (one copy of the memory, not two: about half the match-start hitch) */
        if (g_rs_direct) cp_init();     /* the copy threads, made before the game is held (mark_setup copies through them) */
        save(c); release(); g_rs_drop = 0; g_rs_direct = 0;
        g_ra_wd_pause = 0;
        if (g_save_failed == 2) { g_rg_nomem = 1; LOG("r1: RING: the base state was NOT made (out of memory: free commit %.0f MB, needed %.0f MB + 1024 MB left for the game): no rollback ring\n", g_save_avail_mb, g_save_need_mb); return 0; }
        if (g_save_failed) return 0;
        g_ra_ready = 1; g_session.cycle = 1;
        if (g_mark <= 0) { g_ra_off = 1; LOG("r1: RING: no marks (the game DLL with marks is not loaded): OFF\n"); return 0; }
        double ti0 = now_ms();
        if (!rg_init()) { g_rg_nomem = 2; if (!rbg_on()) g_ra_off = 1; LOG("r1: RING: its own allocations FAILED (out of memory): no rollback ring for the rest of this session\n"); return 0; }
        g_rg_ready = 1;
        {   const char *e = getenv("SSBU_RB_AUDIO_OFF");              /* the audio threads stop for the rest of the fight (hostrt.c rb_audio_off_gate) */
            if (e && e[0] && e[0] != '0') { InterlockedExchange(&g_rb_audio_off, 1); LOG("r1: RING: SSBU_RB_AUDIO_OFF: the audio threads stop at their next SDK call until the fight is over\n"); } }
        RgSlot *s = &g_rg[0]; g_rg_head = 0; g_rg_count = 1;
        s->frame = frame; s->nent = 0;
        rg_meta_capture(s, &g_ctx_save);
        LOG("r1: RING: base state (frame %lld): full save in %.1f ms (ring setup %.1f ms of it)\n", frame, now_ms() - t0, now_ms() - ti0);
        return 1;
    }
    if (g_rg_poolfix < 0) { const char *e = getenv("SSBU_RB_POOLFIX"); g_rg_poolfix = !(e && e[0] == '0'); }
    if (g_rg_tp_f == -2) { const char *e = getenv("SSBU_RB_NET_TEST_POOL"); g_rg_tp_f = -1; g_rg_tp_mode = 1; if (e && *e) { sscanf(e, "%lld,%lld,%d", &g_rg_tp_f, &g_rg_tp_n, &g_rg_tp_mode); LOG("r1: RING TEST: captures of frames %lld .. %lld get at most 64 pool buffers (mode %d)\n", g_rg_tp_f, g_rg_tp_f + g_rg_tp_n - 1, g_rg_tp_mode); } }
    g_rg_tp_active = g_rg_tp_f >= 0 && frame >= g_rg_tp_f && frame < g_rg_tp_f + g_rg_tp_n; g_rg_tp_gets = 0;
    if (g_rg_poolfix && g_rg_count == g_rg_cap && g_rg_need >= 0 && g_rg_cap >= 2 && rg_slot(1)->frame > g_rg_need) {      /* (hs14) full, and its oldest state is still needed */
        g_rgp_seg = seg_was; g_rg_skip_full++;
        if (g_rg_skip_full <= 10) LOG("r1: RING: capture at frame %lld skipped: the ring is full and its oldest state (frame %lld) is the newest one at or before frame %lld, which a rollback may still need\n", frame, rg_slot(0)->frame, g_rg_need);
        return 0;
    }
    if (!ra_hold_quiet(g_rg_park_ms)) { g_rgp_seg = seg_was; g_rg_cap_fail++;
        /* (lo16j) ResLoadingThread busy at the match end: a load a few frames later found it moved on to another wait, the memory went
         * back under it and the game died after 9 s of fault recovery. A thread that is neither audio nor a task worker not at a wait
         * means the game is loading: no frame on a guess for a while (the controller), so no rollback has to cross the load */
        if (g_ra_busy && !strstr(g_ra_busy, "TaskWorker") && !strstr(g_ra_busy, "job fiber") && !strstr(g_ra_busy, "nus3::audio") && !strstr(g_ra_busy, "sindou") && !strstr(g_ra_busy, "AudioDevice")) g_rg_loading_until = frame + 30;
        if (g_rg_cap_fail <= 20 || g_rg_cap_fail % 100 == 0) LOG("r1: RING: capture %lld (frame %lld) skipped: thread %s not at a wait within %.1f ms\n", g_rg_cap_fail, frame, g_ra_busy ? g_ra_busy : "?", g_rg_park_ms); return 0; }
    double t1 = now_ms();
    if (g_rg_count == g_rg_cap) { rg_slot_drop(rg_slot(0), 1); g_rg_head = (g_rg_head + 1) % g_rg_cap; g_rg_count--; }
    RgSlot *s = rg_slot(g_rg_count);
    s->frame = frame; s->nent = 0; s->pages = 0; s->xpages = 0;
    g_rg_cur = s; g_rg_fail_now = 0;
    static uint8_t *xdiff; static size_t xdn; if (xdn < g_rg_nx + 1) { free(xdiff); xdn = g_rg_nx + 1; xdiff = calloc(xdn, 1); }   /* (hs12: the extras can change at a rebase) */
    static uint32_t *xjob; static size_t xjn; size_t nxj = 0;
    if (xjn < g_rg_nx + 1) { free(xjob); xjn = g_rg_nx + 1; xjob = malloc(xjn * sizeof *xjob); }
    xww_poll();                                           /* (LS-X-WW) the pages the OS saw written since the last poll join the set */
    for (size_t e = 0; e < g_rg_nx; e++) {      /* (round 3) extras compared by main + the copy threads: not the pages of the extras' learned set that were never written (rb_learned.c, LS-X), nor a page not committed now; xjob[q] = the extras page of compare job q */
        if (xls_skip(e)) continue;
        xjob[nxj++] = (uint32_t)e; cp_add(g_rg_xa[e], rg_src((uint32_t)(g_rg_n1 + g_rg_n2 + e)));
    }
    g_xls_caps++; g_xls_jobs += (long long)nxj;
    g_cp_cmp = xdiff; cp_run(); g_cp_cmp = NULL;
    xls_decay(xjob, xdiff, nxj);                          /* (LS-X) learned pages that stopped changing go read-only again */
    if (g_rgpg_on < 0) g_rgpg_on = getenv("SSBU_RB_SIM_PAGES") != NULL;
    g_rgpg_kind = g_ra_phase != 2 ? 0 : g_rgs_left > 1 ? 1 : 2; g_rgpg_caps[g_rgpg_kind]++;
    if (g_ls_active) ls_capture_pages(rg_cap_page); else rg_sweep(rg_cap_page);      /* R4: the learned set (rb_learned.c) instead of the marks */
    for (size_t q = 0; q < nxj && !g_rg_fail_now; q++) {                /* extras: the pages that differ from their newest value */
        size_t e = xjob[q]; uint32_t id = (uint32_t)(g_rg_n1 + g_rg_n2 + e);
        if (!xdiff[q]) continue;
        uint32_t nb = rg_pool_get_cap();
        if (!nb || !rg_push(s, id, g_rg_idx[id])) { if (nb) rg_pool_put(nb); g_rg_fail_now = 1; break; }
        cp_add(rg_pool_ptr(nb), g_rg_xa[e]); g_rg_idx[id] = nb; s->xpages++;
    }
    double t2 = now_ms();
    cp_run();
    if (g_rg_fail_now && g_rg_poolfix) {                    /* (hs14) given up: this capture's pages back to their previous values, marked again */
        size_t undone = s->nent;
        for (size_t n = s->nent; n-- > 0;) { uint32_t id = s->ent[2 * n], old = s->ent[2 * n + 1]; rg_pool_put(g_rg_idx[id]); g_rg_idx[id] = old; rg_remark(id); }
        s->nent = 0; s->frame = -1; s->pages = s->xpages = 0;
        if (g_rg_keep) g_rg_kept = 1; else unpark();
        g_rg_short = 1; g_rg_short_n++; g_rgp_seg = seg_was;
        if (g_rg_short_n <= 20 || g_rg_short_n % 100 == 0) LOG("r1: RING: capture at frame %lld GIVEN UP: out of pool memory (pool %u buffers made, %u free; %lld old states freed so far; test refusals %lld): %zu pages put back, nothing lost; states kept %d (frames %lld .. %lld), needed from frame %lld\n",
            frame, g_rg_bump, g_rg_nfree, g_rg_freed_slots, g_rg_tp_refused, undone, g_rg_count, r1_ring_oldest(), r1_ring_newest(), g_rg_need);
        return 0;
    }
    g_rg_short = 0;
    rg_audit();                                             /* (debug, SSBU_RB_AUDIT) pages the capture should have taken and did not */
    rg_pagedump(frame);                                     /* (debug, SSBU_RB_PAGEDUMP) every page's hash, to compare two online games */
    double t3 = now_ms();
    ls_after_capture();                                     /* R4 (SSBU_LS_DECAY) */
    fibers_save(); double t3a = now_ms();
    nvn_r1_cb_save(); double t3b = now_ms();
    rg_meta_capture(s, c);
    osobj_capture(frame);                                   /* (SSBU_STATE_OBJS) events and semaphores at this capture */
    sm_capture(frame);                                      /* (debug, SSBU_STATE_MEASURE) fingerprints of the threads and objects at this capture */
    double t4 = now_ms();
    if (g_rg_keep) g_rg_kept = 1;                          /* a load follows at once (r1_ring_load / r1_ring_let_go lets them go) */
    else unpark();                                         /* every thread, the audio ones too (a real pass follows) */
    double t5 = now_ms();
    g_mp[0] += t3a - t3; g_mp[1] += t3b - t3a; g_mp[2] += t4 - t3b; g_mp[3] += t5 - t4; g_mp_n[0]++; g_mp_b[0] += (double)g_guest_fibers.pool_used; g_mp_b[1] += (double)s->cbd_used;
    g_rg_count++; g_rg_caps++;
    if (g_rg_fail_now) {                                   /* out of pool / log room: the newest values are right, the older states are not reachable */
        LOG("r1: RING: capture at frame %lld ran out of room (pool %u buffers): older states dropped\n", frame, g_rg_bump);
        while (g_rg_count > 1) { rg_slot_drop(rg_slot(0), 1); g_rg_head = (g_rg_head + 1) % g_rg_cap; g_rg_count--; }
    }
    g_rg_last[0] = t5 - t0; g_rgp_seg = seg_was;
    {   static int nslow; if (t5 - t0 > 40 && ++nslow <= 40) LOG("r1: RING: SLOW capture at frame %lld: %.1f ms: hold %.1f, sweep %.1f (pages %u + extras %u), copy %.1f, fibers %.1f, command buffers %.1f, rest %.1f\n",
        frame, t5 - t0, t1 - t0, t2 - t1, (unsigned)s->pages, (unsigned)s->xpages, t3 - t2, t3a - t3, t3b - t3a, t5 - t3b); }
    if (g_rgc_n < RG_NST) { int i = g_rgc_n++; g_rgc[0][i] = t5 - t0; g_rgc[1][i] = t1 - t0; g_rgc[2][i] = t2 - t1; g_rgc[3][i] = t3 - t2; g_rgc[4][i] = (t4 - t3) + (t5 - t4); g_rgc_pg[i] = s->pages; g_rgc_xp[i] = s->xpages; }
    return 1;
}
/* (R3 hs12) a NEW BASE for the next match of the same process (two online matches in one session). Between two matches
 * the game loaded and freed a lot through host code that marks nothing (file loads, module loads, the loader threads), so
 * the ring's newest values (g_rg_idx, the flat shadow) are stale: every slot is dropped and, with every guest thread held,
 * the base is made again the way the first full save made it, in place: every committed read-write page of both windows
 * is copied into the shadow (its value = the shadow again, pool buffers freed), the shadow of a page not committed now is
 * decommitted (= a zero page, as for a page the first save never saw), the marks are cleared, the extras (pages outside
 * the windows: main's guest stack, TLS blocks, fighter DLL data, catch table, malloc arena) are listed again from scratch
 * (their count may change), host stacks made since are left out, modules loaded since get the mark maps. Then slot 0 = this
 * frame (threads, fibers, command buffers as a capture). 1 = done; 0 = not now (a thread not at a wait: next pass). */
static void rg_noop(uint32_t id, int w, size_t j) { (void)id; (void)w; (void)j; }
int r1_ring_rebase(long long frame, Ctx *c) {
    if (g_rg_nomem == 2) return 0;                        /* (hs13) the ring broke earlier: no rollback */
    if (!g_rg_ready) return r1_ring_capture(frame, c);
    double t0 = now_ms();
    r1_ring_let_go();
    r1_ring_release();
    if (!ra_hold_quiet(200)) { LOG("r1: RING rebase (frame %lld): no quiet moment within 200 ms: next pass\n", frame); return 0; }
    double t1 = now_ms();
    hs_exclude_all();
    int nmod = 0;
    {   HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        MODULEENTRY32 me; me.dwSize = sizeof me;
        for (BOOL ok = sn != INVALID_HANDLE_VALUE && Module32First(sn, &me); ok; ok = Module32Next(sn, &me)) {
            uint8_t **a1 = (uint8_t **)GetProcAddress(me.hModule, "r1_m1"), **a2 = (uint8_t **)GetProcAddress(me.hModule, "r1_m2");
            uint64_t *l1 = (uint64_t *)GetProcAddress(me.hModule, "r1_lo1"), *n1 = (uint64_t *)GetProcAddress(me.hModule, "r1_n1"), *n2 = (uint64_t *)GetProcAddress(me.hModule, "r1_n2");
            if (!a1 || !a2 || !l1 || !n1 || !n2) continue;
            if (*a1 != *pm1) { *a1 = *pm1; *a2 = *pm2; *l1 = (uint64_t)(uintptr_t)g_win1; MemoryBarrier(); *n2 = WN2; *n1 = g_wn1; nmod++; LOG("r1: RING rebase: mark maps set in %s (loaded after the first save)\n", me.szModule); }
        }
        if (sn != INVALID_HANDLE_VALUE) CloseHandle(sn); }
    rg_sweep(rg_noop);                                    /* marks since the last capture: the copy below takes everything */
    g_rg_ntl = 0;
    collect(c);
    size_t nwin = g_rg_n1 + g_rg_n2, copied = 0, dropped = 0, freed = 0;
    uint8_t *in = calloc(nwin, 1);
    static Extra nx[8192]; int nnx = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {
        Rng *r = &g_memory_map.ranges[i];
        for (size_t p = 0; p < r->n / PG; p++) {
            uint8_t *a = r->a + p * PG, *cm; size_t pi;
            uint8_t *sh = in_shadow(a, &cm, &pi);
            if (sh) {
                int w2 = cm == g_sh2c;
                if (w2 ? g_ns2[pi] : g_ns1[pi]) continue;                    /* not simulation (a host stack) */
                in[w2 ? g_rg_n1 + pi : pi] = 1;
                if (!sh_commit(sh, cm, pi)) { free(in); g_rg_nomem = 2; unpark(); LOG("r1: RING rebase: commit of the shadow FAILED (out of memory): no rollback ring for the rest of this session\n"); return 0; }
                memcpy(sh, a, PG); copied++;
                if (!w2) g_dc[pi] = 0;
            } else if (nnx < 8192) {
                if (nnx && nx[nnx - 1].a + nx[nnx - 1].n == a) nx[nnx - 1].n += PG;
                else { nx[nnx].a = a; nx[nnx].n = PG; nx[nnx].b = NULL; nnx++; }
            }
        }
    }
    for (size_t id = 0; id < nwin; id++) {
        if (g_rg_idx[id] != RG_FLAT) { if (g_rg_idx[id] >= 2) freed++; rg_pool_put(g_rg_idx[id]); g_rg_idx[id] = RG_FLAT; }
        if (in[id]) continue;
        int w2 = id >= g_rg_n1; size_t pi = w2 ? id - g_rg_n1 : id; uint8_t *cm = w2 ? g_sh2c : g_sh1c;
        if (cm[pi]) { VirtualFree((w2 ? g_sh2 : g_sh1) + pi * PG, PG, MEM_DECOMMIT); cm[pi] = 0; dropped++; }
    }
    free(in);
    {   /* the extras again */
        size_t npg = 0; for (int i = 0; i < nnx; i++) npg += nx[i].n / PG;
        size_t tot = nwin + npg;
        for (size_t e = 0; e < g_rg_nx; e++) rg_pool_put(g_rg_idx[nwin + e]);
        uint32_t *nidx = VirtualAlloc(NULL, tot * 4, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint8_t *ntouch = VirtualAlloc(NULL, tot, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        uint8_t **nxa = malloc((npg + 1) * sizeof *nxa), **nxb = malloc((npg + 1) * sizeof *nxb);
        if (!nidx || !ntouch || !nxa || !nxb) { LOG("r1: RING rebase: allocation FAILED (out of memory): no rollback ring for the rest of this session\n"); g_rg_nomem = 2; unpark(); return 0; }
        memcpy(nidx, g_rg_idx, nwin * 4);
        VirtualFree(g_rg_idx, 0, MEM_RELEASE); VirtualFree(g_rg_touch, 0, MEM_RELEASE); free(g_rg_xa); free(g_rg_xb);
        for (int i = 0; i < g_nx; i++) free(g_x[i].b);
        g_rg_idx = nidx; g_rg_touch = ntouch; g_rg_xa = nxa; g_rg_xb = nxb;
        g_nx = nnx; g_xbytes = 0; size_t e = 0;
        for (int i = 0; i < nnx; i++) {
            g_x[i] = nx[i]; g_x[i].b = malloc(g_x[i].n);
            if (!g_x[i].b) { g_nx = i; g_rg_nx = e; g_rg_nomem = 2; unpark(); LOG("r1: RING rebase: allocation of an extra FAILED (out of memory): no rollback ring for the rest of this session\n"); return 0; }
            memcpy(g_x[i].b, g_x[i].a, g_x[i].n); g_xbytes += g_x[i].n;
            for (size_t o = 0; o < g_x[i].n; o += PG) { g_rg_xa[e] = g_x[i].a + o; g_rg_xb[e] = g_x[i].b + o; e++; }
        }
        g_rg_nx = npg;
    }
    double t2 = now_ms();
    RgSlot *s = &g_rg[0]; g_rg_head = 0; g_rg_count = 1;
    s->frame = frame; s->nent = 0; s->pages = 0; s->xpages = 0;
    fibers_save();
    nvn_r1_cb_save();
    rg_meta_capture(s, c);
    ls_new_base();                                          /* R4: the windows read-only again, the learned set emptied */
    unpark();
    g_rg_caps++;
    LOG("r1: RING rebase: base state (frame %lld) made again in %.1f ms (hold %.1f, copy %.1f): %zu window pages copied into the shadow (%.1f MB), %zu pool buffers freed, %zu shadow pages of pages not committed now dropped; extras %d ranges %.1f MB (%zu pages); %d modules got the mark maps\n",
        frame, now_ms() - t0, t1 - t0, t2 - t1, copied, copied * (double)PG / 1048576.0, freed, dropped, g_nx, g_xbytes / 1048576.0, g_rg_nx, nmod);
    return 1;
}
/* OPTION 1 (root cause): pages the game commits DURING a fight in the big reservations, outside what the ring saved at its base, are never captured nor restored; after a rollback they hold future
 * content while the rest is rewound, and the game (the Lua interpreter's jemalloc memory) then reads an object that should not exist -> index2value crashes, a std::terminate, the fault cascade the
 * safety net cannot fix because the reloaded state carries the same stale pages. Here the ring's base is simply made again (r1_ring_rebase: it re-scans every committed page and rebuilds the extras
 * to include the new ones) once enough such pages have piled up, rate-limited so it cannot thrash. A rebase is a full save: a brief hitch. SSBU_RB_REBASE=<pages> sets the threshold (0 = off, default
 * 96); SSBU_RB_REBASE_GAP=<frames> the least frames between rebases (default 120). Called at a real-pass safe point, no re-run outstanding, threads not held. The hitch-free version (adding the pages
 * to the ring as they appear, no full save) is the follow-up once this confirms the pages are the cause. */
int rb_maybe_rebase(long long i, Ctx *c) {
    extern volatile LONG64 g_xls_out_commit;
    static long long thr = -1, gap, seen_at, last_frame = -1000000;
    if (thr < 0) { const char *e = getenv("SSBU_RB_REBASE"); thr = e ? atoll(e) : 0; e = getenv("SSBU_RB_REBASE_GAP"); gap = e ? atoll(e) : 120; }
    if (thr <= 0 || !g_rg_ready) return 0;
    long long out = (long long)g_xls_out_commit;
    if (out - seen_at < thr || i - last_frame < gap) return 0;
    seen_at = out; last_frame = i;
    double t0 = now_ms();
    int ok = r1_ring_rebase(i, c);
    LOG("r1: REBASE at frame %lld: %lld pages committed outside the ring since the last base -> base remade (%s, %.0f ms); rollbacks before this frame can no longer be taken\n",
        i, out, ok ? "done" : "refused, next time", now_ms() - t0);
    return ok;
}
/* (R3 hs13: tl4 / nmr / le3a / lg2a / lh4, c00000ff in ntdll's unwinder at a lua2cpp L2CExceptionShutdown throw, about 1 loop
 * run in 5 at the same frame) nnunwind.c's catch table g_catch maps a guest frame (CFA) to a host jmp_buf ON A HOST STACK.
 * The table is state (an extra, rolled back with the rest), but only SOME host stacks go back with it: those of the fibers
 * that were switched away at the capture and are copied back by fibers_restore_all. Every other host stack (the threads,
 * the fibers running now, a fiber that got a new host fiber since) keeps its present frames, while the table got the
 * entries of the capture: a coroutine whose entry frame was registered on its new host fiber found, after a load, the older
 * entry of the same guest frame pointing into its previous host fiber, and longjmp'ed across stacks (proved by the
 * nnunwind.c check: "CROSS-STACK longjmp", lh4a / lh4b). Now, after a load: the restored table keeps only the entries whose
 * jmp_buf lies on a host stack that was copied back; the present entries of every other stack are put back.
 * SSBU_R1_CATCHFIX=0: the old way (test). */

static int g_rgc_fix = -1; long long g_rgc_kept, g_rgc_dropped, g_rgc_loads_changed;
static RgCatch g_rgc_now[8192];
static void rgc_before(void) {                          /* threads held, before the load copies: the table as it is now */
    if (g_rgc_fix < 0) { const char *e = getenv("SSBU_R1_CATCHFIX"); g_rgc_fix = !(e && e[0] == '0'); }
    if (!g_rgc_fix || !g_catch_addr) return;
    memcpy(g_rgc_now, (void *)(uintptr_t)g_catch_addr, sizeof g_rgc_now);
}
void nnunwind_catch_reinsert(uint64_t cfa, uint64_t jb); void nnunwind_catch_free(unsigned i);   /* nnunwind.c (hs15: its hash and probing) */
/* (hs15) after a load: the host fiber stacks still in use (some guest fiber's host fiber, a thread's current fiber, a stack a
 * thread's stack pointer is on); a catch record on any other host fiber stack ever made is dead (the coroutine there was made
 * again on a new host fiber, or the fiber exists only in states thrown away): dropped instead of kept for ever. These dead
 * records piled up at the same CFA (h15e0: 25 of one frame) and, with the old hash, overwrote live ones (pp3h). */
static uint64_t g_rgc_live[2048]; static int g_rgc_nlive; long long g_rgc_dead;
/* ---- LOAD FLIGHT RECORDER (measurement for option 2). Every ring load, the state of the Windows "workers" (job fibers) and of the C++ catch table: fibers put back, not put back, replaced,
 * fibers BORN since that state (they exist now and not in it: the ring leaves them as they are, and nothing counted them before), threads not at their wait, catch records kept / dropped. The last
 * 16 loads are kept in memory and written to the log when the safety net absorbs a fault or the game is about to end (rg_dump_recent_loads), so a crash says what the loads before it did. Loads
 * with anything unusual are also logged as they happen (first 40). */
#define LR_N 16
static char g_lr[LR_N][320]; static long long g_lr_n;
static long long g_lr_born_total, g_lr_unusual;
int fibers_born_count(char *buf, size_t n);             /* threads.c: right after fibers_restore_all */
static void rg_load_note(long long frame, int k, long long kept0, long long dropped0, long long dead0) {
    char bb[200]; int born = fibers_born_count(bb, sizeof bb);
    long long kept = g_rgc_kept - kept0, dropped = g_rgc_dropped - dropped0, dead = g_rgc_dead - dead0;
    int unusual = born || g_guest_fibers.not_restored || g_guest_fibers.replaced || g_guest_threads.mismatch;
    g_lr_born_total += born; if (unusual) g_lr_unusual++;
    char *o = g_lr[g_lr_n % LR_N];
    snprintf(o, 320, "load %lld: frame %lld (%d states back), pass %lld%s: fibers put back %d, NOT put back %d, replaced %d, born since %d%s; threads not at their wait %d; catch records kept %lld dropped %lld dead %lld",
             g_lr_n + 1, frame, k, g_rgs_next, g_ra_phase == 2 ? " (re-run)" : "", g_guest_fibers.restored, g_guest_fibers.not_restored, g_guest_fibers.replaced, born, born ? bb : "",
             g_guest_threads.mismatch, kept, dropped, dead);
    g_lr_n++;
    if (unusual && g_lr_unusual <= 40) LOG("r1: LOADREC unusual %s\n", o);
}
void rg_dump_recent_loads(const char *why) {
    static volatile LONG dumping; if (InterlockedExchange(&dumping, 1)) return;
    LOG("r1: LOADREC: %s; the last %d loads (of %lld; %lld unusual, %lld fibers born in all):\n", why, (int)(g_lr_n < LR_N ? g_lr_n : LR_N), g_lr_n, g_lr_unusual, g_lr_born_total);
    for (long long i = g_lr_n > LR_N ? g_lr_n - LR_N : 0; i < g_lr_n; i++) LOG("r1:   %s\n", g_lr[i % LR_N]);
    InterlockedExchange(&dumping, 0);
}
static void rgc_live_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    (void)key; (void)c; (void)stack; (void)size;
    if (fiber && g_rgc_nlive < 2048) g_rgc_live[g_rgc_nlive++] = *(uint64_t *)((uint8_t *)fiber + 0x20);
}
static int rgc_dead(uint64_t jb) {                     /* jb on a host fiber stack that nothing uses now */
    size_t i; for (i = 0; i < g_nbf; i++) if (jb >= g_bf_lo[i] && jb < g_bf_hi[i]) break;
    if (i == g_nbf) return 0;                           /* not a fiber stack made by os_threads.c (a thread's own stack): kept */
    uint64_t lo = g_bf_lo[i], hi = g_bf_hi[i];
    for (int k = 0; k < g_rgc_nlive; k++) if (g_rgc_live[k] == lo) return 0;
    for (int k = 0; k < g_guest_fibers.live_count; k++) if (g_guest_fibers.live_rsp[k] >= lo && g_guest_fibers.live_rsp[k] < hi) return 0;     /* (live_rsps of fibers_restore_all: a thread is on it) */
    return 1;
}
static void rgc_after(void) {                           /* after the copies and fibers_restore_all (g_rest_lo / g_rest_hi: the stacks copied back) */
    if (!g_rgc_fix || !g_catch_addr) return;
    RgCatch *t = (RgCatch *)(uintptr_t)g_catch_addr; long long dropped = 0, kept = 0;
    static int deadfix = -1; if (deadfix < 0) { const char *e = getenv("SSBU_R1_CATCHDEAD"); deadfix = !(e && e[0] == '0'); }
    if (deadfix) { g_rgc_nlive = 0; os_r1_fibers(rgc_live_cb); AcquireSRWLockShared(&g_bf_lock); }
    static int tr = -1; if (tr < 0) tr = getenv("SSBU_UNWIND_TRACE") != NULL;
    if (tr) { void nnunwind_trace_note(const char *what, uint64_t cfa, uint64_t jb);
              for (int i = 0; i < 8192; i++) if (t[i].cfa > 1) nnunwind_trace_note(rest_stack(t[i].jb) ? "load: restored record kept (its stack copied back)" : "load: restored record dropped", t[i].cfa, t[i].jb);
              for (int k = 0; k < 8192; k++) if (g_rgc_now[k].cfa > 1) nnunwind_trace_note(rest_stack(g_rgc_now[k].jb) ? "load: present record not kept (its stack copied back)" : deadfix && rgc_dead(g_rgc_now[k].jb) ? "load: present record dead" : "load: present record kept", g_rgc_now[k].cfa, g_rgc_now[k].jb); }
    for (int i = 0; i < 8192; i++) if (t[i].cfa > 1 && !rest_stack(t[i].jb)) { nnunwind_catch_free((unsigned)i); dropped++; }
    for (int k = 0; k < 8192; k++) {                    /* nnunwind.c catch_put, without its lock (every guest thread is held) */
        if (g_rgc_now[k].cfa <= 1 || !g_rgc_now[k].jb || rest_stack(g_rgc_now[k].jb)) continue;
        if (deadfix && rgc_dead(g_rgc_now[k].jb)) { g_rgc_dead++; continue; }
        nnunwind_catch_reinsert(g_rgc_now[k].cfa, g_rgc_now[k].jb); kept++;     /* (the records are per host stack: nnunwind.c catch_here) */
    }
    if (deadfix) ReleaseSRWLockShared(&g_bf_lock);
    g_rgc_dropped += dropped; g_rgc_kept += kept; if (dropped || kept) g_rgc_loads_changed++;
}
long long g_rg_refused;                        /* (R3 pub1) loads refused: a thread still running after the hold */
long long g_rg_ldpg;                           /* (R3 net3) pages to load, all loads (the written ones: g_cp_wrote) */
long long g_rg_audio_busy;                     /* loads refused because an audio thread was still in guest code holding a mutex (hostrt.c) */
int r1_ring_load(long long frame, Ctx *c) {
    int si = -1;
    for (int i = 0; i < g_rg_count; i++) if (rg_slot(i)->frame == frame) si = i;
    if (si < 0) return 0;
    { int rb_audio_wait_parked(double); if (!rb_audio_wait_parked(8.0)) { g_rg_audio_busy++; return -1; } }   /* an audio thread still runs guest code (holds a mutex): load next pass, when it has parked (hostrt.c) */
    double t0 = now_ms(); g_rgp_seg = 5;
    RgSlot *g = rg_slot(si);
    int k = g_rg_count - 1 - si;
    static Th cur[256]; int ncur = g_rg_kept ? g_guest_threads.count : 0;  /* threads still held from the capture just made (g_rg_keep) */
    if (ncur) memcpy(cur, g_guest_threads.table, (size_t)ncur * sizeof *cur);
    g_rg_kept = 0;
    g_guest_threads.info_count = g->ninfo;
    stop_born_threads();
    if (g_nborn) LOG("r1: RING: %d guest threads created after frame %lld, held for good:%s\n", g_nborn, frame, g_born);
    g_guest_threads.count = g->nth; memcpy(g_guest_threads.table, g->th, (size_t)g->nth * sizeof *g_guest_threads.table);
    if (ncur) g_rg_kept_n++;
    for (int j = 0; j < ncur; j++) {                      /* held at the same wait as in capture g: stays held; otherwise let go (the loop below waits for it) */
        if (!cur[j].held) continue;
        int keep = 0;
        for (int i = 0; i < g_guest_threads.count; i++) if (g_guest_threads.table[i].h == cur[j].h) { if (g_guest_threads.table[i].parked && cur[j].from == g_guest_threads.table[i].from && cur[j].imp == g_guest_threads.table[i].imp) { g_guest_threads.table[i].held = 1; g_guest_threads.table[i].rsp = cur[j].rsp; g_guest_threads.table[i].rip = cur[j].rip; g_guest_threads.table[i].cok = cur[j].cok; keep = 1; } break; }
        if (keep) g_rg_kept_same++; else ResumeThread(cur[j].h);
    }
    for (double tq = now_ms();;) {                        /* each thread at the wait it was held at in that capture, no job running */
        ra_hold_restore();
        if (ra_fibers_quiet() || now_ms() - tq > 200) { if (g_ra_busy_fibers) LOG("r1: RING load: %d fibers still busy after 200 ms\n", g_ra_busy_fibers); break; }
        for (int i = 0; i < g_guest_threads.count; i++) if (g_guest_threads.table[i].held) { ResumeThread(g_guest_threads.table[i].h); g_guest_threads.table[i].held = 0; }
        SwitchToThread();
    }
    /* (R3 pub1: fx2j / fx3h, c0000005 in the sound thread's memcpy right after "nus3::audio update not back at its wait after 500 ms
     * ... now guest code") a thread that is still RUNNING guest code (not blocked at a wait) after the hold's 500 ms was suspended in
     * the middle of its code, and the load then changed the memory under its registers. Now such a load is REFUSED: every thread
     * goes on, nothing was changed yet, the wrong guess stays pending (rb_rollback.c) and the next tick loads again (one frame
     * further back). SSBU_R1_RUNFIX=0: the old way (loaded anyway). SSBU_RB_NET_TEST_REFUSE=<n> (TEST ONLY): every n-th load is
     * refused as if a thread were running (the timeline must not change). */
    {   static int runfix = -1, tn = -1; static long long tc;
        if (runfix < 0) { const char *e = getenv("SSBU_R1_RUNFIX"); runfix = !(e && e[0] == '0'); e = getenv("SSBU_RB_NET_TEST_REFUSE"); tn = e ? atoi(e) : 0; }
        int forced = tn > 0 && ++tc % tn == 0;
        if ((runfix && g_guest_threads.running) || forced) {
            unpark();
            Ctx *tc2; const char *nm2; HANDLE h2;                  /* the threads made after that frame (stop_born_threads) go on too */
            for (int i = g_guest_threads.info_count; guest_thread_info(i, &tc2, &nm2, &h2); i++) if (h2 && GetThreadId(h2) != GetCurrentThreadId()) ResumeThread(h2);
            g_rg_refused++;
            if (g_rg_refused <= 20 || g_rg_refused % 100 == 0) LOG("r1: RING load of frame %lld (k %d) REFUSED (%lld so far): %s; every thread goes on, the rollback is tried again next tick\n",
                frame, k, g_rg_refused, forced ? "test switch SSBU_RB_NET_TEST_REFUSE" : "a thread still running guest code after 500 ms");
            g_rgp_seg = 1;
            return -1;
        }
    }
    double t1 = now_ms();
    sm_load(frame);                                         /* (debug, SSBU_STATE_MEASURE) what differs from the capture, before anything is restored */
    R1_CTR("load (state of frame)", frame, 0);
    rgc_before();
    g_rg_ntl = 0;
    if (g_rgs_on > 0) rgv_before(si, r1_ring_newest());
    for (int i = g_rg_count - 1; i > si; i--) {           /* undo logs newest first: pointers only */
        RgSlot *s = rg_slot(i);
        for (size_t n = 0; n < s->nent; n++) {
            uint32_t id = s->ent[2 * n], old = s->ent[2 * n + 1];
            rg_pool_put(g_rg_idx[id]); g_rg_idx[id] = old; rg_touch(id);
        }
        rg_slot_drop(s, 0);
    }
    g_rg_count = si + 1;
    if (g_ls_active) ls_for_each_learned_page(rg_load_page); else rg_sweep(rg_load_page);      /* written since the newest capture (R4: the learned set) */
    {   size_t nl = 0;
        xww_poll();                                       /* (LS-X-WW) written since the newest capture */
        for (size_t e = 0; e < g_rg_nx; e++) { if (xls_skip(e)) continue; rg_touch((uint32_t)(g_rg_n1 + g_rg_n2 + e)); nl++; }      /* (LS-X: an extras page never written since the base still holds its value at every state) */
        g_xls_loads++; g_xls_loadpages += (long long)nl; }
    size_t pages = g_rg_ntl; g_rg_ldpg += (long long)pages;
    for (size_t n = 0; n < g_rg_ntl; n++) {               /* each page once, from its value at `frame` */
        uint32_t id = g_rg_tl[n]; g_rg_touch[id] = 0;
        uint8_t *a = rg_addr(id), *src = rg_src(id);
        if (src == (uint8_t *)1) continue;                 /* decommitted then: left as it is */
        if (id < g_rg_n1 && g_dc[id]) { if (!src) continue; VirtualAlloc(a, PG, MEM_COMMIT, PAGE_READWRITE); g_dc[id] = 0; }
        if (!src) memset(a, 0, PG); else cp_add(a, src);
    }
    {   /* (R3 net3) SSBU_RB_LOADCMP=1 (option, default off: measured no faster, na0 / na1): a page is written only if it differs from the value loaded */
        static int lc = -1; if (lc < 0) { const char *e = getenv("SSBU_RB_LOADCMP"); lc = e && e[0] == '1'; }
        g_cp_cond = lc; g_cp_restore = 1; cp_run(); g_cp_restore = 0; g_cp_cond = 0; }      /* (a page the game decommitted since the capture is committed again, write_marks.c) */
    double t2 = now_ms();
    g_guest_fibers.count = g->nfib; memcpy(g_guest_fibers.table, g->fib, (size_t)g->nfib * sizeof *g_guest_fibers.table);
    if (g->fpool_used) memcpy(g_guest_fibers.pool, g->fpool, g->fpool_used);
    g_guest_fibers.pool_used = g->fpool_used;
    { static int on = -1; if (on < 0) { const char *e = getenv("SSBU_R1_TLS"); on = !(e && e[0] == '0'); } if (on) nnsdk_tls_load(g->tls); }      /* (SSBU_R1_TLS=0: not restored, to compare) */
    restore_thread_regs(); double t2a = now_ms();
    long long lr_kept0 = g_rgc_kept, lr_dropped0 = g_rgc_dropped, lr_dead0 = g_rgc_dead;
    fibers_restore_all(); double t2b = now_ms();
    rg_opg_load(g);                                       /* the pages committed outside the ring during the fight, as at that capture */
    rg_gstk_load(g);                                      /* the fibers' guest stacks as at that capture (pages the ring itself may not save) */
    rgc_after(); double t2c = now_ms();
    rg_load_note(frame, k, lr_kept0, lr_dropped0, lr_dead0);       /* (measurement) what this load did to the workers and the catch table */
    rg_cb_load(g);
    osobj_load(frame);                                      /* (SSBU_STATE_OBJS) events and semaphores back to what this capture recorded */
    rg_roundtrip_after_load();                              /* (debug, SSBU_RB_ROUNDTRIP) every page must now be what the ring recorded at this frame */
    double t3 = now_ms();
    unpark();
    g_mp[4] += t2a - t2; g_mp[5] += t2b - t2a; g_mp[6] += t2c - t2b; g_mp[7] += t3 - t2c; g_mp[8] += now_ms() - t3; g_mp_n[1]++;
    *c = g->ctx;
    g_rg_loads++;
    if (g_guest_threads.mismatch || g_guest_fibers.not_restored) LOG("r1: RING load of frame %lld (k %d): %d threads not at their wait, %d fibers not restored\n", frame, k, g_guest_threads.mismatch, g_guest_fibers.not_restored);
    g_rg_last[1] = now_ms() - t0; g_rgp_seg = 1;
    if (k < RG_MAXSLOT && g_rgl_n[k] < RG_NL) { int i = g_rgl_n[k]++; g_rgl[k][i][0] = now_ms() - t0; g_rgl[k][i][1] = t1 - t0; g_rgl[k][i][2] = t2 - t1; g_rgl_pg[k][i] = pages; (void)t3; }
    return 1;
}


/* Which part of the save an address belongs to, for crash reports: "window 1" / "window 2" / "an extras page" (the ring saves these) or "NOT saved by the ring". */
const char *r1_addr_class(uint64_t a) {
    if (g_win1 && a >= (uint64_t)(uintptr_t)g_win1 && a < (uint64_t)(uintptr_t)g_win1 + g_wn1) return "window 1, saved";
    if (a >= WIN2 && a < WIN2 + WN2) return "window 2, saved";
    for (size_t e = 0; g_rg_xa && e < g_rg_nx; e++) if ((uint64_t)(uintptr_t)g_rg_xa[e] == (a & ~(uint64_t)(PG - 1))) return "an extras page, saved";
    return "NOT saved by the ring";
}
