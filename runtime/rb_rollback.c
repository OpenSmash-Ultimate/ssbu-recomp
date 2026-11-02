/* R3 (TEST ONLY, default OFF): the GAME SIDE of online rollback (plan\phases\R3_online_rollback.md). Included by
 * r1_state.c after r1_ring.c: it drives R2's snapshot ring (r1_ring_capture / r1_ring_load) and its re-run passes
 * (g_ra_phase 2, the input sequence held, nothing presented) from rb_net.c's input queues instead of the SSBU_RB_SIM
 * schedule. Cut point: both games run the match as a LOCAL versus match (same start: same match settings and seeds), the
 * game's own Local Wireless netcode is not used in the match; only the pads of ports 1 and 2 are exchanged:
 *  - nnhid.c fill_pad, ports 1 / 2, while the match is on: the local player's pad of a real pass is handed to the input
 *    queue (kept for frame + delay, sent); the game gets rb_net's pad of each player for the frame of the pass (a remote
 *    pad not received yet = the last one received, repeated);
 *  - the safe point after each pass (the kick after the submission 0x7103749b24, the same as run-ahead / SSBU_RB_SIM):
 *    received messages are applied; a wrong guess at frame m -> the ring state of frame m - 1 is loaded and the passes
 *    m .. now are run again with the corrected pads (re-run passes but the last skip presentation work: SSBU_RB_SIM_SKIP /
 *    SSBU_RB_SIM_LITE, default 49 / 127 here); then the next real pass waits for its 60 Hz tick (SSBU_RB_NET_PACE=0: no
 *    wait), and for the other player when it is more than SSBU_RB_NET_AHEAD frames behind.
 * Frames: the ring's base capture (the first safe point of the fight) is frame 0; real pass n is frame n.
 * No game content in this file. SPDX-License-Identifier: GPL-3.0-or-later */
#include "rb.h"
int rbn_on(void); void rbn_init(void); void rbn_start(long long f0); int rbn_started(void); int rbn_local_player(void); int rbn_ahead(void);
void rbn_local_pad(long long f, RbPad now); RbPad rbn_pad(int player, long long f);
void rbn_drain(long long cur); int rbn_take_mismatch(void); long long rbn_confirmed(void); int rbn_peer_gone(void);
void rbn_pass_hash(long long f); void rbn_finalize(long long cur); double rbn_pace_extra(long long f); void rbn_report(FILE *o, long long frames);
int rbn_delay(void); int rbn_rtt_ms(void); double rbn_offset_ms(void); void rbn_resend(void); void rbn_status(char *out, size_t n); void rbn_hello(void); int rbn_peer_started(void); void rbn_choose_delay(void);

int rbn_match_cancelled(void); void rbn_match_reset(void); int rbn_match_number(void);      /* (hs12) */
void rbn_set_lockstep(int on); int rbn_loop(void);      /* (hs13) */
/* (hs13) a match WITHOUT the ring (r1_ring.c g_rg_nomem: not enough memory for the snapshot ring, e.g. a PC short of commit
 * memory: tw1j crashed there): the pads are still exchanged, but every real pass waits until the other player's pad of
 * its frame has arrived (delay-based; the input delay is chosen for that: rb_net.c lockstep), so no frame ever runs on a
 * guess and the two games stay identical; the other game may still roll back. SSBU_RB_NET_TEST_NOMEM forces it (test). */
static int g_rbg_noring; static long long g_rbg_noring_matches;
static int rbg_noring_now(void) { return g_rbg_noring; }
int r1_ring_rebase(long long frame, Ctx *c);            /* r1_ring.c (hs12) */
void input_mark(const char *what);                      /* nnhid.c: the input sequence can wait for it (@match1on, @match1over, ...) */
/* (hs12) matches in one session: 0 = between matches, 1 = an online match runs (its base state made), 2 = a match that was
 * not agreed (handshake failed / given up) runs locally (Final Destination, one stock): no ring, and rbg_pad walks player 1 off the stage */
static int g_rbg_in_match, g_rbg_rebase; static double g_rbg_quit_t0;
/* (R3 net3, runs pe3 / pe4: DESYNC from frame 2) the base state is the first capture of the fight; when it cannot be made at the
 * first safe point with the fight on ("RA: first save: no quiet moment within 200 ms: not now"), it was made one pass later, and
 * that game called fight frame 218 "frame 0" while the other called 217 so: every frame one apart. Now the frames are counted
 * from the first safe point with the fight on (g_rbg_ff_first) on both games: a base made `late` passes later is frame `late`,
 * and the passes before it get no pad (as frames 1..delay on the other game). SSBU_RB_NET_BASEFIX=0: the old way (test). */
static long long g_rbg_ff_first = -1; static int g_rbg_basefix = -1;
static int g_rbg = -1;
static int rbg_on(void) { if (g_rbg < 0) g_rbg = rbn_on() > 0; return g_rbg; }
static long long g_rbg_rb, g_rbg_k[RG_MAXSLOT + 2], g_rbg_rerun, g_rbg_miss, g_rbg_stalls, g_rbg_capfail, g_rbg_capskip, g_rbg_lastlocal, g_rbg_real, g_rbg_gone;
static double g_rbg_stall_ms, g_rbg_load_ms, g_rbg_tick0, g_rbg_next, g_rbg_last_go;
static volatile LONG g_rbg_last_k, g_rbg_max_k;
#define RBG_NT 200000
static float *g_rbg_work, *g_rbg_gap, *g_rbg_wcyc; static ULONG64 g_rbg_cyc_go; static unsigned char *g_rbg_wk; static int g_rbg_nw, g_rbg_ng, g_rbg_pace = -1, g_rbg_tick_k;
static HANDLE g_rbg_timer;
/* (hs10) the task workers at the safe point H2: r1_state.c holds them from the kick H1 to H2, so at H2 the kick's jobs are
 * queued and their (host-side) wake-up event is signaled and not consumed; a ring load relies on that, since the nn::os
 * objects are not part of the ring. The 200 ms watchdog of that hold let them go while this file waited at H2 (a stall for
 * the other player, the start barrier): they consumed the event and ran the jobs, and a rollback loaded right after left the
 * game's job counters waiting for jobs no worker would ever be woken for: the main thread spun in the job wait f_710354a120
 * forever (rnh2 / rnh3 / hs7). Now the watchdog is paused while this file runs at H2 (SSBU_RB_NET_WDFIX=0: the old way). */
static int g_rbg_wdfix = -1; static long long g_rbg_wd_before, g_rbg_rb_unheld;
static int g_rbg_detail = -1;
static long long g_rbg_test_f = -2; static int g_rbg_test_ms, g_rbg_test_k;    /* SSBU_RB_NET_TEST=<frame>,<ms>[,<k>]: a stall of ms at frame's H2, then a rollback of k */
/* (hs11) frame time of rollback ticks. SSBU_RB_NET_RECAP: which re-run passes capture their state: 0 = every one from the
 * newest frame with both pads known on (the old way), 1 = only that frame (the oldest any later rollback can need) and the
 * last one, 2 = every other one from that frame, and the last; 3 = the last one only; 4 = (hs16, option) that frame
 * only, NOT the last one (unless it is that frame): the last state (frame i) is loaded only by a later wrong guess at exactly
 * frame i + 1 with no other rollback in between; that one then loads frame cf and runs i - cf frames more (loop,4 bot 7: 18 of
 * 161 rollbacks became k 5; one capture of the two saved per rollback; median a little lower, p90 / p99 higher: not default).
 * 5 = (R3 net3, DEFAULT) 4 for a rollback of exactly 3 frames, 1 for the others: the 3-frame rollback ticks (just over the frame
 * time; 4 or more are over it anyway and doing the same there made the later rollbacks longer: fz5 k 4 -> 7) save a capture; a
 * later wrong guess at exactly the next frame then runs at most as many frames as this one.
 * A later rollback whose state was not captured loads
 * the newest older one and runs a frame or two more. SSBU_RB_NET_KEEP: before a load, 1 = a capture with the threads kept
 * held (the old way), 2 = the threads held without a capture (default; the capture was thrown away by the load), 0 = neither. */
static int g_rbg_recap = -1, g_rbg_keep = -1, g_rbg_cur_k; static long long g_rbg_recap_first;
static double g_rbg_t_pre, g_rbg_t_load, g_rbg_t_rerun, g_rbg_t_recap, g_rbg_t_pass0; static long long g_rbg_n_rerun, g_rbg_n_recap;

/* (hs14) every capture of the match tells the ring the oldest frame a rollback may still need (r1_ring.c g_rg_need): the
 * newest frame with both players' pads known (a wrong guess can only be at a later frame, and loads the state before it) */
static long long g_rbg_short_waits;
/* (R3 pub1) a load refused at a wrong guess at frame g_rbg_pend (r1_ring_load -1): the state before it must stay in the ring
 * until the rollback is made next tick; g_rbg_refused_tick: no second try in the same tick (each try may hold for 500 ms) */
static int g_rbg_pend, g_rbg_refused_tick;
void rbn_put_mismatch(int m);
static int rbg_capture(long long i, Ctx *c) { g_rg_need = rbn_confirmed(); if (g_rbg_pend > 0 && g_rbg_pend - 1 < g_rg_need) g_rg_need = g_rbg_pend - 1; return r1_ring_capture(i, c); }
void rbn_stats(int *delay, int *last, int *maxk) { *delay = rbn_delay(); *last = (int)g_rbg_last_k; *maxk = (int)g_rbg_max_k; }   /* net_relay.c net_stats_get (F2) */

static void rbg_wait_until(double t) {                  /* a high-resolution timer for the long part, a spin for the last 0.3 ms */
    for (;;) {
        double now = now_ms(), left = t - now;
        if (left <= 0) return;
        if (left > 1.3 && g_rbg_timer) { LARGE_INTEGER due; due.QuadPart = -(LONGLONG)((left - 1.0) * 10000.0); SetWaitableTimer(g_rbg_timer, &due, 0, NULL, NULL, FALSE); WaitForSingleObject(g_rbg_timer, 100); }
        else if (left > 0.3) Sleep(0);
        else YieldProcessor();
    }
}
static int rbg_rollback(Ctx *c, long long i, int m) {
    long long target = m - 1, best = -1;
    for (int s = 0; s < g_rg_count; s++) { long long f = rg_slot(s)->frame; if (f <= target && f > best) best = f; }
    if (best < 0) {
        g_rbg_miss++; g_rbg_pend = 0;
        if (g_rbg_miss <= 10) LOG("r1: RB NET: wrong guess at frame %lld but no state at or before frame %lld in the ring (oldest %lld): NOT rolled back (desync)\n", (long long)m, target, r1_ring_oldest());
        return 0;
    }
    double t0 = now_ms();
    if (!g_ra_wh_on) { g_rbg_rb_unheld++; if (g_rbg_rb_unheld <= 10) LOG("r1: RB NET: rollback at frame %lld while the task workers are NOT held (watchdog releases so far %lld): the load may leave the job system waiting\n", i, g_ra_wd_fired); }
    if (g_rbg_refused_tick) { rbn_put_mismatch(m); return 0; }      /* (R3 pub1) refused once this tick: next tick */
    int lr = r1_ring_load(best, c);
    if (lr < 0) { g_rbg_refused_tick = 1; g_rbg_pend = m; rbn_put_mismatch(m); return 0; }      /* (R3 pub1) refused (a thread running): the wrong guess stays pending */
    if (!lr) { g_rbg_miss++; return 0; }
    g_rbg_pend = 0;
    g_rbg_load_ms += now_ms() - t0; g_rbg_t_load += now_ms() - t0; g_rbg_recap_first = -1; g_rbg_t_pass0 = now_ms();
    int k = (int)(i - best);
    g_rgs_left = k; g_rgs_next = best + 1; g_ra_phase = 2; g_rbg_cur_k = k;
    g_rbg_rb++; g_rbg_k[k < RG_MAXSLOT + 1 ? k : RG_MAXSLOT + 1]++; g_rbg_rerun += k; g_rbg_tick_k += k;
    InterlockedExchange(&g_rbg_last_k, k); if (k > g_rbg_max_k) InterlockedExchange(&g_rbg_max_k, k);
    if (g_rbg_rb <= 20 || g_rbg_rb % 200 == 0 || g_rbg_detail > 0) LOG("r1: RB NET: rollback %lld at frame %lld: wrong guess at %d, state of frame %lld loaded in %.3f ms, %d frames again\n", g_rbg_rb, i, m, best, now_ms() - t0, k);
    return 1;
}
/* (hs15, TEST ONLY) SSBU_RB_NET_TEST_SCHED=<frame>:<state>,... : at the real safe point of that frame, a rollback that loads
 * that state (same pads), taken like a wrong guess: replays a pair's rollback schedule in one loop game */
static int rbg_sched(long long i, int m) {
    static int sn = -1; static long long sf[64], sl[64];
    if (sn < 0) { sn = 0; const char *e = getenv("SSBU_RB_NET_TEST_SCHED"); while (e && *e && sn < 64) { long long a, b; if (sscanf(e, "%lld:%lld", &a, &b) != 2) break; sf[sn] = a; sl[sn] = b; sn++; e = strchr(e, ','); if (e) e++; } }
    for (int q = 0; q < sn; q++) if (sf[q] == i) { sf[q] = -1; LOG("r1: RB NET TEST_SCHED: rollback at frame %lld to the state of frame %lld\n", i, sl[q]); if (m <= 0 || m > sl[q] + 1) m = (int)(sl[q] + 1); }
    return m;
}
/* at the safe point (H2) of every pass while the fight runs (r1_ring.c rgs_step) */
static void rbg_step_body(Ctx *c) {
    long long i = g_rgs_next;
    g_rbg_refused_tick = 0;
    if (g_rbg_wdfix < 0) { const char *e = getenv("SSBU_RB_NET_WDFIX"); g_rbg_wdfix = !e || atoi(e) != 0; e = getenv("SSBU_RB_NET_DETAIL"); g_rbg_detail = e && atoi(e) > 0;
        e = getenv("SSBU_RB_NET_RECAP"); g_rbg_recap = e ? atoi(e) : 5; e = getenv("SSBU_RB_NET_KEEP"); g_rbg_keep = e ? atoi(e) : 2; }
    if (g_rbg_wdfix) g_ra_wd_pause = 1;                 /* (until H2 lets the workers go: r1_state.c ra_kick2) */
    if (g_rbg_wd_before != g_ra_wd_fired) { if (g_rg_ready) LOG("r1: RB NET: the task-worker watchdog let them go before the safe point of frame %lld (%lld times so far)\n", i, g_ra_wd_fired); g_rbg_wd_before = g_ra_wd_fired; }
    if (g_rbg_in_match == 3) return;                    /* (R3 lwr, run lwr2: both games crashed here) a room fight already running stays one until its fight-off:
                                                         * the room's match is set up (handshake, rbn_match_cancelled() 0 again) while the waiting-stage fight still runs */
    if (rbn_match_cancelled() == 2) {                   /* (R3 pub2) a Local Wireless room fight: the game's own netcode, no ring, pads untouched */
        if (!g_rbg_in_match) { g_rbg_in_match = 3; LOG("r1: RB NET: a Local Wireless room fight: no rollback (the game's own netcode)\n"); }
        return;
    }
    if (rbn_match_cancelled()) {                        /* (hs12) not agreed with the other game: no rollback; rbg_pad leaves this match */
        if (!g_rbg_in_match) { g_rbg_in_match = 2; g_rbg_quit_t0 = now_ms(); LOG("r1: RB NET: this match was not agreed with the other game: no rollback, leaving it (player 1 walks off the stage)\n"); }
        return;
    }
    if (!g_rbg_in_match) {                              /* the base state: r1's full save (one hitch); (hs12) from the second match on: the ring's base made again */
        if (g_rbg_basefix < 0) { const char *e = getenv("SSBU_RB_NET_BASEFIX"); g_rbg_basefix = !(e && e[0] == '0'); }
        if (g_rbg_ff_first < 0) g_rbg_ff_first = g_ff;
        {   static int tl = -1; if (tl < 0) { const char *e = getenv("SSBU_RB_NET_TEST_LATEBASE"); tl = e ? atoi(e) : 0; }     /* (net3, TEST ONLY) the base made n passes late */
            if (tl > 0 && g_ff - g_rbg_ff_first < tl) { LOG("r1: RB NET TEST_LATEBASE: no base state at fight frame %lld\n", g_ff); return; } }
        if (g_rbg_basefix) g_rgs_next = i = g_ff - g_rbg_ff_first;      /* (net3) the base's frame number: 0, or the passes it came late */
        if (g_rbg_rebase) { if (!g_rbg_basefix) g_rgs_next = i = 0; if (!r1_ring_rebase(i, c) && !g_rg_nomem) return; }
        else { r1_ring_capture(i, c); if (g_rbg_wdfix) g_ra_wd_pause = 1; if (!g_rg_ready && !g_rg_nomem) return; }   /* (the full save sets the pause back to 0) */
        g_rbg_noring = g_rg_nomem != 0;
        rbn_set_lockstep(g_rbg_noring);
        if (g_rbg_noring) {
            g_rbg_noring_matches++;
            LOG("r1: RB NET: ROLLBACK OFF FOR THIS MATCH: not enough memory for the snapshot ring (%s); the match goes on delay-based: every frame waits for the other player's pad\n",
                g_rg_nomem == 1 ? "the base state needs about one copy of the game's memory in free commit" : "the ring broke on the way: off for the rest of this session");
            printf("[rbnet] rollback OFF for this match (not enough memory): delay-based, the game waits for the other player's input every frame\n"); fflush(stdout);
        }
        if (g_rbg_wdfix) g_ra_wd_pause = 1;
        g_rbg_in_match = 1;
        if (i > 0) LOG("r1: RB NET: the base state was made %lld passes after the first safe point with the fight on (fight frame %lld): it is frame %lld, as on the other game (net3)\n", i, g_rbg_ff_first, i);
        rbn_start(i);
        rbn_pass_hash(0);                               /* (hs12) the checksum of frame 1 starts here (run tw8: frame-line lines from before the base, a different number on the two games, made frame 1 of match 2 differ) */
        { char mk[24]; snprintf(mk, sizeof mk, "match%don", rbn_match_number()); input_mark(mk); }
        if (g_rbg_wdfix) g_ra_wd_base = 2;              /* from now on the hold watchdog has the long limit (H2 sets it back to this) */
        g_rbg_wd_before = g_ra_wd_fired;                /* (releases before the match, e.g. at its slow first passes, are harmless: no state in the ring yet) */
        g_rgs_next = i + 1;
        if (g_rbg_pace < 0) { const char *e = getenv("SSBU_RB_NET_PACE"); g_rbg_pace = !e || atoi(e) != 0; }
        { const char *e = getenv("SSBU_RB_NET_LASTLITE"); g_rgs_lastlite = !e || atoi(e) != 0; }      /* default on here: proven exact (rl4L, rl4K); =0 runs the last re-run pass fully */
        if (!g_rbg_timer) g_rbg_timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!g_rbg_timer) g_rbg_timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
        if (!g_rbg_work) { g_rbg_work = calloc(RBG_NT, sizeof *g_rbg_work); g_rbg_gap = calloc(RBG_NT, sizeof *g_rbg_gap); g_rbg_wcyc = calloc(RBG_NT, sizeof *g_rbg_wcyc); g_rbg_wk = calloc(RBG_NT, 1); }
        LOG("r1: RB NET: match start, base state = frame %lld (fight frame %lld); pacing %s; re-run skip mask %d, lite mask %d%s\n", i, g_ff, g_rbg_pace ? "60 Hz" : "off", g_rgs_skip, g_rgs_lite, g_rgs_lastlite ? " (the last re-run pass too)" : "");
        {   /* start barrier: both games begin frame 1 together (the one that finished loading first waits here, at frame 0,
             * instead of stalling seconds at frame 8 with states it might have to roll back to: rnh2 / rnh3 hung after that) */
            double tb = now_ms(), lastx = 0;
            while (!rbn_peer_started() && now_ms() - tb < 120000) {
                if (now_ms() - lastx >= 50) { lastx = now_ms(); rbn_hello(); }
                rbn_drain(i);
                rbg_wait_until(now_ms() + 1.0);
            }
            rbn_hello(); rbn_choose_delay();
            if (i > (rbn_delay() > RB_LEADIN ? rbn_delay() : RB_LEADIN)) LOG("r1: RB NET: WARNING: the base state is frame %lld, later than the empty first frames (input delay %d, at least RB_LEADIN): the other player's first pads may differ (desync risk)\n", i, rbn_delay());
            LOG("r1: RB NET: start barrier: the other game %s after %.0f ms\n", rbn_peer_started() ? "is in the match" : "DID NOT ANSWER (going on)", now_ms() - tb);
        }
        if (g_rbg_noring && rbn_loop()) {               /* (hs13) loop test: the bot's pads start at frame 1 (on a network frames 1..delay are empty on both sides) */
            double tw = now_ms(); while (rbn_confirmed() < 1 && now_ms() - tw < 5000) { rbn_drain(i); rbg_wait_until(now_ms() + 0.5); }
        }
        g_rbg_tick0 = now_ms(); g_rbg_next = 0; g_rbg_cyc_go = rgs_cyc_now();
        return;
    }
    rbn_pass_hash(i);
    { extern volatile long long g_unwind_trace_frame; g_unwind_trace_frame = i + 1; }      /* (hs15) the frame the next pass simulates, for SSBU_UNWIND_TRACE */
    { static int tr = -1; if (tr < 0) tr = getenv("SSBU_RB_NET_TRACE") ? atoi(getenv("SSBU_RB_NET_TRACE")) : 0;
      if (tr > 0 && i <= tr) LOG("r1: RB NET trace: safe point after pass %lld, phase %d, left %d, remote pads up to %lld, %.3f ms\n", i, g_ra_phase, g_rgs_left, rbn_confirmed(), now_ms()); }
    if (g_ra_phase == 2) {                              /* a re-run pass ended: its state is needed only from the newest frame with every pad known on */
        long long cf = rbn_confirmed(); int want;
        if (g_rbg_recap_first < 0 && i >= cf) g_rbg_recap_first = i;          /* the first re-run frame a later rollback may load */
        g_rbg_t_rerun += now_ms() - g_rbg_t_pass0; g_rbg_n_rerun++;
        int r4 = g_rbg_recap == 4 || (g_rbg_recap == 5 && g_rbg_cur_k == 3);
        if (g_rgs_left <= 1) want = !r4 || g_rbg_recap_first < 0 || g_rbg_recap_first == i;     /* (4: not the last, see above) */
        else if (i < cf) want = 0;
        else if (g_rbg_recap == 0) want = 1;
        else if (g_rbg_recap == 2) want = ((i - g_rbg_recap_first) & 1) == 0;
        else if (g_rbg_recap == 3) want = 0;                                  /* (3: the last one only; a later rollback may load the state this one loaded) */
        else if (r4) want = i == g_rbg_recap_first;
        else want = i == g_rbg_recap_first;
        if (want) { double tc = now_ms(); if (!rbg_capture(i, c)) g_rbg_capfail++; g_rbg_t_recap += now_ms() - tc; g_rbg_n_recap++; } else g_rbg_capskip++;
        g_rgs_next = i + 1;
        if (--g_rgs_left > 0) { g_rbg_t_pass0 = now_ms(); return; }
        g_ra_phase = 0; g_ra_after_quiet = 1; g_rgs_left = 0;
    } else if (g_rbg_noring) {                          /* (hs13) a real pass ended, no ring: nothing to capture, nothing ran on a guess */
        g_rgs_next = i + 1; g_rbg_real++;
        rbn_drain(i);
        int m = rbn_take_mismatch();
        if (m > 0 && m <= i) { g_rbg_miss++; if (g_rbg_miss <= 10) LOG("r1: RB NET: wrong guess at frame %d WITHOUT the ring (frame %lld): cannot be rolled back (desync)\n", m, i); }
    } else {                                            /* a real pass ended */
        g_rgs_next = i + 1; g_rbg_real++;
        rbn_drain(i);
        int m = rbg_sched(i, rbn_take_mismatch());
        if (m > 0 && m <= i) {
            /* captured anyway, with the threads kept held for the load that follows (R2 round 2: load hold 3.5 -> 0.2 ms);
             * SSBU_RB_NET_KEEP=0: no capture, the load holds the threads itself */
            double tp = now_ms();
            if (g_rbg_keep == 1) { g_rg_keep = 1; if (!rbg_capture(i, c)) g_rbg_capfail++; g_rg_keep = 0; }
            else if (g_rbg_keep == 2) r1_ring_hold();
            g_rbg_t_pre += now_ms() - tp;
            if (rbg_rollback(c, i, m)) return;
            r1_ring_let_go();
            if (g_rbg_keep != 1 && !rbg_capture(i, c)) g_rbg_capfail++;
        } else if (!rbg_capture(i, c)) g_rbg_capfail++;
        if (g_rbg_test_f == -2) { const char *e = getenv("SSBU_RB_NET_TEST"); g_rbg_test_f = -1; if (e && *e) { long long tf = -1; int ms = 0, k = 0; sscanf(e, "%lld,%d,%d", &tf, &ms, &k); g_rbg_test_f = tf; g_rbg_test_ms = ms; g_rbg_test_k = k; } }
        if (g_rbg_test_f > 0 && i >= g_rbg_test_f && !g_rbg_noring) {    /* (the first real pass from that frame on without a rollback of its own) */
            g_rbg_test_f = -1;                        /* test: a long stall at this safe point, then a rollback (same pads: the timeline must not change) */
            double ts = now_ms(); rbg_wait_until(ts + g_rbg_test_ms);
            LOG("r1: RB NET TEST: stalled %.0f ms at frame %lld (watchdog releases so far %lld, workers held %d); rollback of %d frames\n", now_ms() - ts, i, g_ra_wd_fired, (int)g_ra_wh_on, g_rbg_test_k);
            if (g_rbg_test_k > 0 && rbg_rollback(c, i, (int)(i - g_rbg_test_k + 1))) return;
        }
        {   /* (hs15, TEST ONLY) SSBU_RB_NET_TEST_EH=<k>[,<from frame>]: after a real pass in which a C++ throw or the entry of a
             * catch frame happened (lua2cpp coroutines: nnunwind.c), a rollback of k frames (k 0: 1..6 in turn) with the same pads:
             * the throw / entry runs again in the re-run passes (pp3h crashed at such a throw); the timeline must not change */
            static int eh_k = -2; static long long eh_from; static long eh_last; static int eh_turn; static long long eh_n;
            long nnunwind_events(void);
            if (eh_k == -2) { const char *e = getenv("SSBU_RB_NET_TEST_EH"); eh_k = -1; if (e && *e) { eh_from = 8; sscanf(e, "%d,%lld", &eh_k, &eh_from); } eh_last = nnunwind_events(); }
            if (eh_k >= 0 && !g_rbg_noring) {
                long ev = nnunwind_events();
                if (ev != eh_last && i >= eh_from) {
                    int k = eh_k > 0 ? eh_k : 1 + (eh_turn++ % 6);
                    if (k > i - 1) k = (int)(i - 1);
                    eh_last = ev; eh_n++;
                    if (eh_n <= 20 || eh_n % 100 == 0) LOG("r1: RB NET TEST_EH: %lld: throws / catch entries at frame %lld: rollback of %d frames\n", eh_n, i, k);
                    if (k > 0 && rbg_rollback(c, i, (int)(i - k + 1))) return;
                }
                eh_last = ev;
            }
        }
    }
    /* the next pass is a real one (frame i + 1): pending wrong guesses first, then the other player, then the tick */
    double tw = now_ms();
    if (g_rbg_tick0 > 0 && g_rbg_nw < RBG_NT) { g_rbg_work[g_rbg_nw] = (float)(tw - g_rbg_tick0); g_rbg_wcyc[g_rbg_nw] = (float)((rgs_cyc_now() - g_rbg_cyc_go) / 1e6); g_rbg_wk[g_rbg_nw] = (unsigned char)(g_rbg_tick_k > 255 ? 255 : g_rbg_tick_k); g_rbg_nw++; }
    g_rbg_tick0 = 0;
    int stalled = 0; double ts = 0;
    for (;;) {
        rbn_drain(i);
        int m = rbn_take_mismatch();
        if (m > 0 && m <= i) { if (g_rbg_noring) { g_rbg_miss++; if (g_rbg_miss <= 10) LOG("r1: RB NET: wrong guess at frame %d WITHOUT the ring: cannot be rolled back (desync)\n", m); }
                               else { if (stalled) { g_rbg_stall_ms += now_ms() - ts; } if (rbg_rollback(c, i, m)) return; } }
        /* (hs14) the ring short of pool memory (its last capture given up): no frame on a guess until a capture succeeds */
        if (g_rgs_next - rbn_confirmed() <= (g_rbg_noring || g_rg_short ? 0 : rbn_ahead())) break;
        if (!g_rbg_noring && g_rg_short && g_rgs_next - rbn_confirmed() <= rbn_ahead() && !stalled) g_rbg_short_waits++;
        if (rbn_peer_gone()) { if (!g_rbg_gone++) LOG("r1: RB NET: no message from the other player for 5 s at frame %lld: going on alone (guesses)\n", i); break; }
        if (!stalled) { stalled = 1; ts = now_ms(); g_rbg_stalls++; r1_ring_let_go(); }
        {   /* while waiting: our newest pads again every 50 ms (a lost datagram must not leave both games waiting), a line every 2 s */
            static double last_tx, last_log; double now = now_ms();
            if (now - last_tx >= 50) { last_tx = now; rbn_resend(); }
            if (now - ts >= 2000 && now - last_log >= 2000) { last_log = now; char st[200]; rbn_status(st, sizeof st);
                LOG("r1: RB NET: waiting for the other player at frame %lld for %.0f ms: %s\n", i, now - ts, st); }
        }
        rbg_wait_until(now_ms() + 0.5);
    }
    if (stalled) g_rbg_stall_ms += now_ms() - ts;
    r1_ring_let_go();
    rbn_finalize(g_rbg_pend > 0 && g_rbg_pend - 1 < i ? g_rbg_pend - 1 : i);      /* (R3 pub1) a refused load: its frames are not final yet */
    if (g_rbg_pace) {
        double period = 1000.0 / 60.0 + rbn_pace_extra(g_rgs_next), now = now_ms();
        if (g_rbg_next <= 0 || now - g_rbg_next > 3 * period) g_rbg_next = now;      /* (far behind: no catching up in a burst) */
        rbg_wait_until(g_rbg_next);
        g_rbg_next += period;
    }
    double go = now_ms();
    if (g_rbg_last_go > 0 && g_rbg_ng < RBG_NT) g_rbg_gap[g_rbg_ng++] = (float)(go - g_rbg_last_go);
    g_rbg_last_go = go; g_rbg_tick0 = go; g_rbg_tick_k = 0; g_rbg_cyc_go = rgs_cyc_now();
    if (g_rbg_real % 600 == 0) LOG("r1: RB NET: frame %lld: %lld rollbacks (%lld frames again), %lld stalls (%.0f ms), round trip %d ms, delay %d, remote pads up to %lld, clock ahead of the other game by %.1f ms\n",
                                   i, g_rbg_rb, g_rbg_rerun, g_rbg_stalls, g_rbg_stall_ms, rbn_rtt_ms(), rbn_delay(), rbn_confirmed(), rbn_offset_ms());
}
/* (hs11, TEST ONLY) SSBU_RB_NET_TEST_STK=1: the hs13 crash on purpose. Everything this file does at H2 runs on a host stack
 * of 1 MB placed at a fixed free address inside window 2 (0x7180000000 or above; in hs13 it was main's own Windows stack at
 * 0x72659f4000), set up before the first save (TEB stack limits switched with it); at every H2 the page under the stack
 * pointer is marked as written (what an instrumented write through a pointer to a host local did in hs13). Normally that
 * stack is left out of the state (r1_host_stack_note); with SSBU_R1_HOST_STACKS=0 a load copies an old page of it back
 * while H2 waits for the copy threads (hs13: c0000005 at rip 0). */
static int g_rbt = -1; static uint8_t *g_rbt_stk; static Ctx *g_rbt_c; static long long g_rbt_marks;
static void rbt_body(void) {
    volatile uint8_t probe[64]; probe[0] = 1;
    void r1_wrote(uint64_t a, uint64_t n);
    r1_wrote((uint64_t)(uintptr_t)probe & ~0xfffull, 0x1000); g_rbt_marks++;
    rbg_step_body(g_rbt_c);
}
static void rbg_step(Ctx *c) {
    if (g_rbt < 0) {
        const char *e = getenv("SSBU_RB_NET_TEST_STK"); g_rbt = e && atoi(e) > 0;
        for (uint64_t a = 0x7180000000ull; g_rbt && !g_rbt_stk && a < 0x7300000000ull; a += 0x10000000ull)
            g_rbt_stk = VirtualAlloc((void *)(uintptr_t)a, 1u << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (g_rbt && !g_rbt_stk) g_rbt = 0;
        if (g_rbt) { r1_host_stack_note((uint64_t)(uintptr_t)g_rbt_stk, (uint64_t)(uintptr_t)g_rbt_stk + (1u << 20));
            LOG("r1: RB NET TEST_STK: H2 runs on a host stack at %p..%p (inside window 2)\n", (void *)g_rbt_stk, (void *)(g_rbt_stk + (1u << 20))); }
    }
#ifndef _WIN32                                          /* (the test switches the TEB's stack limits with it: Windows only) */
    if (g_rbt) { g_rbt = 0; LOG("r1: RB NET TEST_STK: Windows only: off\n"); }
#endif
    if (!g_rbt) { rbg_step_body(c); return; }
#ifdef _WIN32
    g_rbt_c = c;
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb(); void *ob = tib->StackBase, *ol = tib->StackLimit;
    uint8_t *top = g_rbt_stk + (1u << 20) - 256;
    tib->StackBase = g_rbt_stk + (1u << 20); tib->StackLimit = g_rbt_stk;
    __asm__ volatile("movq %%rsp, %%rbx\n\t"
                     "movq %0, %%rsp\n\t"
                     "subq $32, %%rsp\n\t"
                     "callq *%1\n\t"
                     "movq %%rbx, %%rsp\n\t"
                     : : "r"(top), "r"(rbt_body)
                     : "rbx", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "cc", "memory");
    tib->StackBase = ob; tib->StackLimit = ol;
#endif
}
/* nnhid.c (through r1_pad / r1_pad_id): the pad of port id (0 / 1) in the pass now running */
static void rbg_pad(int id, uint64_t *b, int *lx, int *ly, int *rx, int *ry) {
    if (g_rbg_in_match == 2 && (id == 0 || id == 1)) {  /* (hs12) a match that was not agreed: this PC's player 1 pauses, then holds L + R + A (the game's own way out of a match), again every 4 s */
        /* (debug_mode.c makes it Final Destination with one stock) player 1 walks off the left edge for 20 s, which ends the
         * match the game's own way (results, then the stage select and the character select: run hw2 showed that pause +
         * L + R + A did NOT end it); after that the pause / L + R + A cycle as a last resort */
        uint64_t bb = 0; int stick = 0; double el = now_ms() - g_rbg_quit_t0;
        /* (R3 pub1, testers 2026-10-20: "controls spazzing, moving by itself, can't press anything", then "fighting myself") the
         * player is told on screen for the whole time that this game is leaving the match by itself, and after 60 s the pad is
         * HIS again (the notice says how to quit: + then L + R + A) instead of being driven for ever. SSBU_RB_NET_LEAVE_MS: the
         * 60 s (test). */
        {   static double next_note, seen_t0; static int given_back; static double leave_ms = -1; extern void menu_notice(const char *text); const char *rbn_hs_why(void);
            if (leave_ms < 0) { const char *e = getenv("SSBU_RB_NET_LEAVE_MS"); leave_ms = e && atof(e) > 0 ? atof(e) : 60000.0; }
            if (seen_t0 != g_rbg_quit_t0) { seen_t0 = g_rbg_quit_t0; next_note = 0; given_back = 0; }
            if (el >= leave_ms) {
                if (!given_back) { given_back = 1; LOG("r1: RB NET: the match that was not agreed did not end by itself in %.0f s: the controls are the player's again\n", leave_ms / 1000.0);
                    char t[400]; snprintf(t, sizeof t, "No online match (%s).\nThis game could not leave the match by itself: the controls are yours again.\nTo quit: press + (pause), then hold L + R + A.", rbn_hs_why()); menu_notice(t); }
                return;                                   /* the player's own pad from now on */
            }
            if (el >= next_note) { next_note = el + 500;
                char t[400]; snprintf(t, sizeof t, "No online match (%s).\nLeaving this match automatically (%.0f s): your fighter walks off the stage by itself,\nthen the game goes back to the character select. Your controls come back there.", rbn_hs_why(), el / 1000.0);
                menu_notice(t); }
        }
        if (id == 0 && el < 20000) stick = -32767;
        else if (id == 0) { long long t = (long long)(el - 20000) % 4000; if (t >= 300 && t < 450) bb = 1ull << 10; else if (t >= 1200 && t < 3600) bb = (1ull << 6) | (1ull << 7) | 1ull; }
        *b = bb; *lx = stick; *ly = *rx = *ry = 0; return;
    }
    if (!g_rbg_in_match && g_rbg_ff_first >= 0 && g_rbg_basefix > 0 && (id == 0 || id == 1)) { *b = 0; *lx = *ly = *rx = *ry = 0; return; }   /* (net3) a pass before a late base: no pad */
    if ((!g_rg_ready && !g_rbg_noring) || g_ra_off || !rbn_started() || id < 0 || id > 1) return;
    long long f = g_rgs_next;
    if (f < 1) return;
    if (id == 0 && g_ra_phase != 2 && f > g_rbg_lastlocal) {          /* a real pass: this game's own pad (port 1's devices / input sequence) */
        RbPad now; now.buttons = *b & 0xffffull; now.lx = (int16_t)*lx; now.ly = (int16_t)*ly; now.rx = (int16_t)*rx; now.ry = (int16_t)*ry;
        rbn_local_pad(f, now); g_rbg_lastlocal = f;
    }
    RbPad p = rbn_pad(id, f);
    *b = p.buttons; *lx = p.lx; *ly = p.ly; *rx = p.rx; *ry = p.ry;
}
static void rbg_report(void);
/* (hs12) r1_ring.c rgs_step, every safe point while no fight is on: the end of a match (once): its report, the ring
 * emptied, the session reset for the next match (whose first safe point makes the ring's base again: r1_ring_rebase) */
static void rbg_fight_off(void) {
    if (!g_rbg_in_match) return;
    if (g_rbg_in_match == 1) { rbg_report(); r1_ring_let_go(); if (g_rg_ready) r1_ring_release(); g_rbg_rebase = 1; }
    else if (g_rbg_in_match == 3) LOG("r1: RB NET: the Local Wireless room fight is over\n");
    else LOG("r1: RB NET: the match that was not agreed is over after %.1f s (player 1 walked off)\n", (now_ms() - g_rbg_quit_t0) / 1000.0);
    if (g_rg_nomem == 1) g_rg_nomem = 0;                /* (hs13) nothing was kept: the next match tries the ring again */
    g_rbg_noring = 0;
    LOG("r1: RB NET: match %d over: ring emptied, session reset for the next match\n", rbn_match_number());
    { char mk[24]; snprintf(mk, sizeof mk, "match%dover", g_rbg_in_match == 1 ? rbn_match_number() : 0); if (g_rbg_in_match == 1) input_mark(mk); else input_mark("localmatchover"); }
    g_rbg_in_match = 0; g_rbg_ff_first = -1;
    g_rbg_rb = 0; memset(g_rbg_k, 0, sizeof g_rbg_k); g_rbg_rerun = g_rbg_miss = g_rbg_stalls = g_rbg_capfail = g_rbg_capskip = g_rbg_lastlocal = g_rbg_real = g_rbg_gone = 0;
    g_rbg_stall_ms = g_rbg_load_ms = g_rbg_tick0 = g_rbg_next = g_rbg_last_go = 0;
    g_rbg_last_k = 0; g_rbg_max_k = 0; g_rbg_short_waits = 0; g_rg_short = 0; g_rg_need = -1; g_rbg_nw = g_rbg_ng = g_rbg_tick_k = 0; g_rbg_rb_unheld = 0; g_rbg_recap_first = 0;
    g_rbg_t_pre = g_rbg_t_load = g_rbg_t_rerun = g_rbg_t_recap = g_rbg_t_pass0 = 0; g_rbg_n_rerun = g_rbg_n_recap = 0;
    g_rbg_wd_before = g_ra_wd_fired;
    g_rbg_pend = 0;
    rbn_match_reset();
}
static int cmp_f(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static void rbg_dist(const char *what, const float *v, int n, const unsigned char *kk, int want) {
    float *w = malloc((size_t)(n ? n : 1) * sizeof *w); int m = 0; double s = 0, over = 0;
    for (int j = 0; j < n; j++) if (!kk || want < 0 || (want == 0 ? kk[j] == 0 : kk[j] > 0)) { w[m++] = v[j]; s += v[j]; over += v[j] > 1000.0 / 60.0; }
    if (m) { qsort(w, (size_t)m, sizeof *w, cmp_f);
        LOG("r1: RB NET TIMING %s: %d, mean %.2f median %.2f p90 %.2f p99 %.2f max %.2f ms; over 16.67 ms: %.0f (%.2f%%)\n", what, m, s / m, w[m / 2], w[m * 9 / 10], w[m * 99 / 100], w[m - 1], over, 100.0 * over / m); }
    free(w);
}
static void rbg_report(void) {
    if (g_rbg <= 0 || (!g_rg_ready && !g_rbg_noring) || !g_rbg_real) return;
    if (g_rbg_noring) LOG("r1: RB NET report: this match ran WITHOUT rollback (not enough memory for the ring: %s); matches without the ring so far %lld\n", g_rg_nomem == 2 ? "broken for the session" : "base not made", g_rbg_noring_matches);
    LOG("r1: RB NET report: %lld real frames, %lld rollbacks, %lld frames run again (k: 1 %lld, 2 %lld, 3 %lld, 4 %lld, 5 %lld, 6 %lld, 7 %lld, more %lld); rollbacks impossible (state not in the ring) %lld\n",
        g_rbg_real, g_rbg_rb, g_rbg_rerun, g_rbg_k[1], g_rbg_k[2], g_rbg_k[3], g_rbg_k[4], g_rbg_k[5], g_rbg_k[6], g_rbg_k[7], g_rbg_k[RG_MAXSLOT + 1] + g_rbg_k[8], g_rbg_miss);
    LOG("r1: RB NET report: stalls for the other player %lld (%.1f ms in all; %lld of them because the ring was short of memory); captures failed %lld, re-run captures not needed %lld; load mean %.3f ms; peer silent %lld\n",
        g_rbg_stalls, g_rbg_stall_ms, g_rbg_short_waits, g_rbg_capfail, g_rbg_capskip, g_rbg_rb ? g_rbg_load_ms / g_rbg_rb : 0.0, g_rbg_gone);
    LOG("r1: RB NET report: ring memory (hs14, fix %s): captures given up %lld, old states freed early %lld, captures skipped to keep the oldest needed state %lld\n",
        g_rg_poolfix ? "on" : "OFF", g_rg_short_n, g_rg_freed_slots, g_rg_skip_full);
    LOG("r1: RB NET report: task-worker watchdog releases %lld (since the process start), rollbacks with the workers not held %lld (watchdog paused at H2: %s)\n", g_ra_wd_fired, g_rbg_rb_unheld, g_rbg_wdfix ? "yes" : "NO");
    LOG("r1: RB NET report: loads that took the sound thread at another wait after 5 ms %lld; threads not at their wait after 500 ms %d (last load); loads refused, a thread still running (pub1) %lld\n", g_th_audio_other, g_th_mismatch, g_rg_refused);
    LOG("r1: RB NET report: fiber host stacks in use at a load (not copied back) %lld, seen %lld; fibers found running by their thread (TEB) at a capture %lld; catch table at loads: entries dropped %lld, live entries put back %lld, loads changed %lld (hs13)\n", g_fib_live_skip, g_fib_live_seen, g_fib_teb_running, g_rgc_dropped, g_rgc_kept, g_rgc_loads_changed);
    { void nnunwind_catch_stats(long long *, long long *, long long *, long long *, long long *); long long l, d, cr, fu, mp; nnunwind_catch_stats(&l, &d, &cr, &fu, &mp);
      LOG("r1: RB NET report: catch table (hs15): %lld live records, %lld removed slots, %lld puts found no free slot in their window, %lld table full, longest probe %lld; dead records dropped at loads %lld (host fiber stacks known %zu)\n", l, d, cr, fu, mp, g_rgc_dead, g_nbf); }
    LOG("r1: RB NET report: host stack pages inside the guest windows left out of the state %lld (%lld ranges, %lld from fibers made later)\n", g_hs_pages, g_hs_ranges, g_hs_fibers_later);
    if (g_rbg_rb) LOG("r1: RB NET report: rollback parts (ms per rollback): before the load %.3f (keep mode %d), load %.3f, re-run passes %.3f (%.2f per rollback, %.3f each), re-run captures %.3f (%.2f per rollback, recap mode %d)\n",
        g_rbg_t_pre / g_rbg_rb, g_rbg_keep, g_rbg_t_load / g_rbg_rb, g_rbg_t_rerun / g_rbg_rb, (double)g_rbg_n_rerun / g_rbg_rb, g_rbg_n_rerun ? g_rbg_t_rerun / g_rbg_n_rerun : 0.0, g_rbg_t_recap / g_rbg_rb, (double)g_rbg_n_recap / g_rbg_rb, g_rbg_recap);
    r1_hold_prof_report(); rg_meta_prof_report();
    LOG("r1: RB NET report: live host stack check (hs16): thread contexts taken from the hold %lld, asked again %lld (SSBU_R1_LIVECACHE %s)\n", g_live_cached, g_live_asked, g_live_cache ? "on" : "OFF");
    rbg_dist("work per tick (real pass + its rollback, without waits)", g_rbg_work, g_rbg_nw, g_rbg_wk, -1);
    rbg_dist("work per tick without rollback", g_rbg_work, g_rbg_nw, g_rbg_wk, 0);
    rbg_dist("work per tick with rollback", g_rbg_work, g_rbg_nw, g_rbg_wk, 1);
    rbg_dist("time between real passes (tick to tick)", g_rbg_gap, g_rbg_ng, NULL, -1);
    rbg_dist("main-thread CPU per tick with rollback (million cycles, shown as ms)", g_rbg_wcyc, g_rbg_nw, g_rbg_wk, 1);
    rbg_dist("main-thread CPU per tick without rollback (million cycles, shown as ms)", g_rbg_wcyc, g_rbg_nw, g_rbg_wk, 0);
    rbn_report(g_log, g_rbg_real);
}
