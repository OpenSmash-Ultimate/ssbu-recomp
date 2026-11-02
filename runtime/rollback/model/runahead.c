/* RUN-AHEAD of one frame (SSBU_RUNAHEAD=1): speculative frame ahead of the present, on the same save / restore.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "game_state_internal.h"
/* Functions and variables of other runtime files that only this file uses: */
uint64_t hook_address(const char *name);
int hook_install_name(const char *name, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);

/* ---------------------------------------------------------------- R1 round 5: RUN-AHEAD of one frame (SSBU_RUNAHEAD=1 or
 * graphics.ini `runahead = on`; default OFF; needs the marks: ssbu_game_p15r1.dll + the marked modules).
 * The game's main loop (FUN 0x7103747270) per iteration: frame end (present bookkeeping, acquire) -> SUBMIT of the command
 * buffers recorded during the previous iteration (with pipe: the picture goes to the window here and the game is held
 * until the tick) -> kick 0x7103749b20 -> render preparation of the last logic frame -> pad poll -> logic -> back. So the
 * picture of logic frame N is recorded in iteration N+1 and submitted in N+2: one frame of the game's own.
 * Here, at the kick right after the submission (hook on FUN 0x7103549170 from 0x7103749b20): the state is SAVED and a
 * SPECULATIVE pass runs one whole iteration (preparation and recording of the last REAL logic frame, a logic frame whose
 * result is thrown away, frame end, submission): its submission carries the picture of the last real logic frame and is
 * the one presented (pipe). Back at the kick the state is RESTORED and the REAL pass runs the same iteration (its own
 * submission is a duplicate of that picture: not presented, the game not held). So every picture reaches the screen one
 * iteration earlier, and it is the picture of real logic (nothing predicted): the speculative pass's own logic frame
 * never reaches the screen. Its pad input does not matter; player 1's last real values are given (the input sequence
 * and the L1 instrument see only real polls). The audio threads stay held from the save to the restore (they never see
 * a speculative frame). A save that cannot hold every thread at a wait within SSBU_RUNAHEAD_PARK_MS (default 3) is
 * skipped: that frame is a normal one (one frame later, nothing repeated). Only while the game's FighterManager says
 * the fight is on (ready-go). */
int g_ra = -1, g_ra_phase, g_ra_ready, g_ra_after_quiet, g_ra_off;
static double g_ra_park_ms = 3.0;
static ImportFn g_orig_kick, g_orig_line;
static long long g_ra_specs, g_ra_skips, g_ra_kicks, g_ra_fe;
static uint64_t g_ra_site;                                /* GA(13.0.4 0x7103749b24) at init; SSBU_RUNAHEAD_SITE=<return address> (diagnosis) */
double g_rap[3][4]; long long g_rapn[3];  /* per pass kind (0 normal, 1 speculative, 2 real after a restore): pass start -> poll, poll -> frame end, frame end call, frame end -> next save point */
static double g_ra_fe_end; static int g_ra_fe_kind; double g_t_poll;
static double g_ra_comp[2][5];static long long g_ra_ncomp[2];      /* save / restore: sweep + page copy, extras, fibers, command buffers, release */
volatile int g_ra_wd_pause, g_ra_wd_base;   /* 1 = paused, 2 = long limit (R3 online match, rb_rollback.c) */static int g_ra_submit_mode;
static int g_ra_skip = 1; static long long g_ra_skipped_kicks, g_ra_skipped_lines;      /* default 1: the preparation kicks of a speculative pass (pictures and hash checked identical); 2 (the render kick) changes 0.3 % of the picture */
static long long g_ra_skip_why[4];                      /* holds that failed: task worker, audio, other thread, a job fiber running */
const char *g_ra_busy;
static const char *g_ra_busy_imp;                       /* (R3 lwr, log) the import the busy thread was in */
const char *g_ra_busy_first_save(void) { static char b[300]; snprintf(b, sizeof b, "%s, in import %s", g_ra_busy ? g_ra_busy : "?", g_ra_busy_imp ? g_ra_busy_imp : "none"); return b; }
double g_ra_t_mark, g_ra_spec_sum, g_ra_real_sum; static long long g_ra_nspec, g_ra_nreal;
static double g_ra_s[16384], g_ra_r[16384], g_ra_sh[16384], g_ra_rh[16384]; static size_t g_ra_sp[16384], g_ra_rp[16384]; static int g_ra_ns, g_ra_nr;
static char *g_ra_kind; static long long g_ra_nkind, g_ra_kcap;              /* per fight frame (frame line calls): 'R' real, 'S' speculative */
static volatile LONG g_ra_lines;
PadRec g_ra_pad;
int r1_ra_phase(void) { return g_ra > 0 ? g_ra_phase : 0; }
int r1_ra_quiet(void) { return g_ra > 0 && g_ra_phase == 2; }          /* nvn_gl.c: the real pass's submission is not presented, the game not held */
/* nvn_gl.c queue_present: 0 = as usual; 1 = bookkeeping only (the picture went to the window at the submission); 2 = nothing */
int r1_ra_present_mode(void) {
    if (g_ra <= 0) return 0;
    if (g_ra_phase == 2) return g_ra_submit_mode ? 1 : 2;      /* (submit mode: the real pass's frame end is the only one of the tick) */
    if (g_ra_phase == 1) return 1;
    if (g_ra_after_quiet) { g_ra_after_quiet = 0; return 1; }
    return 0;
}
uint64_t hook_address(const char *name);             /* fighter_sdk\hook_host.c (included by nnro.c) */
int hook_install_name(const char *name, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);
int ra_fight_on(void) {
    static uint64_t inst;
    if (!inst) inst = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
    uint64_t mgr = inst ? rd64(inst) : 0, impl = mgr ? rd64(mgr) : 0;
    return impl && rd8(impl + 0xd2);
}
/* a suspended thread is safe to keep held only if it is BLOCKED in a system call (its RIP right after a `syscall`
 * instruction): one that is suspended while it runs our import code may hold the os object table lock or an object's
 * critical section (seen: nus3::audio update held inside obj() -> the game's thread blocked on that lock for good) */
static uint64_t g_tb_rsp, g_tb_rip;                     /* (hs16) the context th_blocked read last */
static int th_blocked(HANDLE h) {
    CONTEXT x; memset(&x, 0, sizeof x); x.ContextFlags = CONTEXT_CONTROL;
    g_tb_rsp = g_tb_rip = 0;
    if (!GetThreadContext(h, &x)) return 0;
    g_tb_rsp = x.Rsp; g_tb_rip = x.Rip;
    const uint8_t *r = (const uint8_t *)(uintptr_t)x.Rip;
    static uintptr_t lo[8], hi[8]; static int nc;           /* (R2) executable regions already queried: system-call stubs stay put */
    uintptr_t a = (uintptr_t)(r - 2); int known = 0;
    for (int i = 0; i < nc; i++) if (a >= lo[i] && a + 2 <= hi[i]) { known = 1; break; }
    if (!known) {
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(r - 2, &m, sizeof m) || m.State != MEM_COMMIT || !(m.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE | PAGE_EXECUTE_READWRITE))) return 0;
        if (!(m.Protect & PAGE_EXECUTE_READWRITE)) { int k = nc < 8 ? nc++ : (int)(a >> 12) & 7; lo[k] = (uintptr_t)m.BaseAddress; hi[k] = lo[k] + m.RegionSize; }
    }
    return r[-2] == 0x0f && r[-1] == 0x05;
}
static int ra_audio(const char *n) { return strstr(n, "nus3::audio") || strstr(n, "sindou") || strstr(n, "AudioDevice"); }
static int ra_free(const char *n) { return strstr(n, "nopus") != NULL; }      /* never in a waiting import; its pages (decoder state, output) are not simulation */
/* save: every other guest thread held inside a waiting import (its place and registers noted); 0 = one was not there in time */
/* (R3 hs16, TEST ONLY) SSBU_RB_HOLD_PROF=1: where a hold's time goes, per thread (suspend + context + retries), and the
 * fiber check; printed in the rollback report */
int g_hp_on = -1; static struct { const char *n; double ms, mx; long long cnt, spins; } g_hp[64]; static int g_hp_n; static double g_hp_fib_ms; static long long g_hp_fib_n, g_hp_holds;
static void hp_add(const char *n, double ms, int spins) {
    if (g_ra_phase == 2 && ra_audio(n)) n = !strcmp(n, "nus3::audio update") ? "(re-run passes) nus3::audio update" : "(re-run passes) other sound thread";
    int i; for (i = 0; i < g_hp_n; i++) if (g_hp[i].n == n || !strcmp(g_hp[i].n, n)) break;
    if (i == g_hp_n) { if (g_hp_n >= 64) return; g_hp[g_hp_n++].n = n; }
    g_hp[i].ms += ms; g_hp[i].cnt++; g_hp[i].spins += spins; if (ms > g_hp[i].mx) g_hp[i].mx = ms;
}
void r1_hold_prof_report(void) {
    if (g_hp_on <= 0) return;
    LOG("r1: HOLD PROF: %lld holds; fiber checks %lld, %.4f ms each\n", g_hp_holds, g_hp_fib_n, g_hp_fib_n ? g_hp_fib_ms / g_hp_fib_n : 0.0);
    for (int i = 0; i < g_hp_n; i++) LOG("r1: HOLD PROF thread %s: %lld times, mean %.4f ms, max %.3f ms, retries %lld\n", g_hp[i].n, g_hp[i].cnt, g_hp[i].ms / (g_hp[i].cnt ? g_hp[i].cnt : 1), g_hp[i].mx, g_hp[i].spins);
}
static double g_ra_hold_end = 1e300;                    /* ra_hold_quiet's deadline: one slow thread after another (each under the park limit) added up to 130-345 ms at the match start */
static int ra_hold_save(void) {
    DWORD me = GetCurrentThreadId(); Ctx *tc; const char *nm; HANDLE h;
    if (g_hp_on < 0) g_hp_on = getenv("SSBU_RB_HOLD_PROF") && atoi(getenv("SSBU_RB_HOLD_PROF")) > 0;
    if (g_hp_on) g_hp_holds++;
    g_guest_threads.count = 0;
    for (int i = 0; guest_thread_info(i, &tc, &nm, &h) && g_guest_threads.count < 256; i++) {
        if (!h || GetThreadId(h) == me) continue;
        Th *t = &g_guest_threads.table[g_guest_threads.count++]; memset(t, 0, sizeof *t); t->h = h; t->c = tc; t->name = nm ? nm : "?";
    }
    g_guest_threads.info_count = 0; while (guest_thread_info(g_guest_threads.info_count, &tc, &nm, &h)) g_guest_threads.info_count++;
    static int all_first = -1; if (all_first < 0) { const char *e = getenv("SSBU_RB_HOLD_ALL"); all_first = rgs_wanted() && e && atoi(e) != 0; }      /* opt-in: saved ~0.07 ms only; rb2f1 crashed with it (cause not shown) */
    static uint8_t pre[256];                                /* (R2 round 3, ring only) every thread suspended first, then checked: the kernel delivers the suspends together */
    if (all_first) for (int i = 0; i < g_guest_threads.count; i++) pre[i] = !ra_free(g_guest_threads.table[i].name) && SuspendThread(g_guest_threads.table[i].h) != (DWORD)-1;
    for (int i = 0; i < g_guest_threads.count; i++) {
        Th *t = &g_guest_threads.table[i];
        if (ra_free(t->name)) continue;
        double t0 = now_ms();
        if (all_first && pre[i]) {
            pre[i] = 0;
            if (waiting_import(t->c->in_import) && th_blocked(t->h)) { t->held = 1; t->parked = 1; t->rsp = g_tb_rsp; t->rip = g_tb_rip; t->cok = 1; t->from = t->c->import_from; t->imp = t->c->in_import; t->regs = *t->c; continue; }
            ResumeThread(t->h);
            for (int j = i + 1; j < g_guest_threads.count; j++) if (pre[j]) { ResumeThread(g_guest_threads.table[j].h); pre[j] = 0; }      /* a thread not at a wait: the others go too (it may wait for one of them) */
        }
        int spins = 0;
        for (;;) {
            if (SuspendThread(t->h) == (DWORD)-1) break;
            if (waiting_import(t->c->in_import) && th_blocked(t->h)) { t->held = 1; t->parked = 1; t->rsp = g_tb_rsp; t->rip = g_tb_rip; t->cok = 1; break; }
            ResumeThread(t->h); spins++;
            if (now_ms() - t0 > g_ra_park_ms || now_ms() > g_ra_hold_end) {
                if (g_hp_on) hp_add(t->name, now_ms() - t0, spins);
                for (int j = 0; j < i; j++) if (g_guest_threads.table[j].held) { ResumeThread(g_guest_threads.table[j].h); g_guest_threads.table[j].held = 0; }
                if (all_first) for (int j = i + 1; j < g_guest_threads.count; j++) if (pre[j]) { ResumeThread(g_guest_threads.table[j].h); pre[j] = 0; }
                g_ra_busy = t->name; g_ra_busy_imp = t->c->in_import;      /* (R3 lwr) and the import it was in (log) */
                if (strstr(t->name, "TaskWorker")) g_ra_skip_why[0]++; else if (ra_audio(t->name)) g_ra_skip_why[1]++; else g_ra_skip_why[2]++;
                return 0;
            }
            SwitchToThread();
        }
        if (g_hp_on) hp_add(t->name, now_ms() - t0, spins);
        t->from = t->c->import_from; t->imp = t->c->in_import; t->regs = *t->c;
    }
    return 1;
}
static void ra_release_save(void) { for (int i = 0; i < g_guest_threads.count; i++) if (g_guest_threads.table[i].held && !ra_audio(g_guest_threads.table[i].name)) { ResumeThread(g_guest_threads.table[i].h); g_guest_threads.table[i].held = 0; } }
/* restore: each thread at the SAME wait as at the save (the audio threads were never let go) */
void ra_hold_restore(void) {
    g_guest_threads.mismatch = 0; g_guest_threads.running = 0;
    for (int i = 0; i < g_guest_threads.count; i++) {
        Th *t = &g_guest_threads.table[i];
        t->same = 0;
        if (t->held) { t->same = 1; continue; }
        t->cok = 0;
        if (!t->parked) continue;
        double t0 = now_ms();
        for (;;) {
            if (SuspendThread(t->h) == (DWORD)-1) break;
            if (t->c->import_from == t->from && t->c->in_import == t->imp && th_blocked(t->h)) { t->held = 1; t->same = 1; t->rsp = g_tb_rsp; t->rip = g_tb_rip; t->cok = 1; break; }
            /* (R3 hs11) the sound thread sindou::update alternates SleepThread / WaitEvent; at a ring load it sometimes waits for an
             * event no held thread signals, and this loop waited the whole 500 ms (a half-second freeze, 2 in 10 normal-speed runs).
             * An audio thread blocked at another waiting import after 5 ms is taken as it is (what the 500 ms limit did anyway).
             * SSBU_RB_AUDIO_WAIT=0: the old way. */
            {   static int aw = -1; if (aw < 0) { const char *e = getenv("SSBU_RB_AUDIO_WAIT"); aw = !(e && e[0] == '0'); }
                if (aw && rgs_wanted() && ra_audio(t->name) && now_ms() - t0 > 5 && waiting_import(t->c->in_import) && th_blocked(t->h)) { t->held = 1; t->rsp = g_tb_rsp; t->rip = g_tb_rip; t->cok = 1; g_guest_threads.audio_other++;
                    { static int said_a; if (said_a++ < 200) LOG("r1: hold for a load: sound thread %s taken at ANOTHER wait after %.1f ms: saved at %s @%llx, now %s @%llx (loads like this so far: %lld)\n", t->name ? t->name : "?",
                        now_ms() - t0, t->imp ? t->imp : "guest code", (unsigned long long)t->from, t->c->in_import ? t->c->in_import : "guest code", (unsigned long long)t->c->import_from, g_guest_threads.audio_other); }
                    break; } }
            /* (lo16j) a thread other than audio blocked at ANOTHER wait has moved on since the capture (ResLoadingThread: SleepThread then,
             * WaitEvent now): its stack would go back under it. Counted as running, so the load is refused, after 50 ms instead of 500 */
            if (!ra_audio(t->name) && now_ms() - t0 > 50 && waiting_import(t->c->in_import) && th_blocked(t->h) && !getenv("SSBU_RB_MOVED_OK")) {
                t->held = 1; g_guest_threads.mismatch++; g_guest_threads.running++;
                static int said_m; if (said_m++ < 20) LOG("r1: hold for a load: thread %s moved on to another wait: held at %s @%llx then, now %s @%llx\n", t->name ? t->name : "?",
                    t->imp ? t->imp : "guest code", (unsigned long long)t->from, t->c->in_import ? t->c->in_import : "guest code", (unsigned long long)t->c->import_from);
                break; }
            if (now_ms() - t0 > 500) { t->held = 1; g_guest_threads.mismatch++; if (!(waiting_import(t->c->in_import) && th_blocked(t->h))) g_guest_threads.running++;
                static int said; if (said++ < 20) LOG("r1: hold for a load: thread %s not back at its wait after 500 ms: held at %s @%llx then, now %s @%llx, blocked %d\n", t->name ? t->name : "?",
                    t->imp ? t->imp : "guest code", (unsigned long long)t->from, t->c->in_import ? t->c->in_import : "guest code", (unsigned long long)t->c->import_from, th_blocked(t->h));
                break; }
            ResumeThread(t->h);
            SwitchToThread();
        }
    }
}

/* the job system at the kick is NOT quiet as at the frame end (the preparation jobs kicked at 0x71037494f8 may still run on
 * the task workers, which always look parked in SwitchToFiber): every fiber that is running on a thread must be inside a
 * waiting import (not SwitchToFiber: that one is a worker between jobs or a job switching). Checked with the threads held. */
int g_ra_busy_fibers;
static void ra_quiet_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    (void)stack; (void)size; (void)fiber;
    if (!os_r1_fiber_tid(key)) return;
    if (!c || !c->in_import || strstr(c->in_import, "SwitchToFiber") || !waiting_import(c->in_import)) g_ra_busy_fibers++;
}
int ra_fibers_quiet(void) { g_ra_busy_fibers = 0; os_r1_fibers(ra_quiet_cb); return g_ra_busy_fibers == 0; }
static long long g_ra_fiber_waits, g_ra_fiber_skips; static double g_ra_fiber_wait_ms;
/* hold the threads at a quiet moment: retry until `limit` ms */
int ra_hold_quiet(double limit) {
    double t0 = now_ms(); int tries = 0;
    for (;;) {
        g_ra_hold_end = t0 + limit; int ok = ra_hold_save(); g_ra_hold_end = 1e300;
        if (ok) {
            double tf = g_hp_on > 0 ? now_ms() : 0; int fq = ra_fibers_quiet(); if (g_hp_on > 0) { g_hp_fib_ms += now_ms() - tf; g_hp_fib_n++; }
            if (fq) { if (tries) { g_ra_fiber_waits++; g_ra_fiber_wait_ms += now_ms() - t0; } return 1; }
            for (int i = 0; i < g_guest_threads.count; i++) if (g_guest_threads.table[i].held) { ResumeThread(g_guest_threads.table[i].h); g_guest_threads.table[i].held = 0; }
            g_ra_busy = "a job fiber (task worker)"; g_ra_skip_why[3]++;
        }
        tries++;
        if (now_ms() - t0 > limit) return 0;
        SwitchToThread();
    }
}
static int ra_save(Ctx *c) {
    double t0 = now_ms();
    {   /* the window's refresh count (written by the game's VSync Count Thread at each tick, + the count at the last present)
         * is wall time: rolled back, every tick's count would be lost (the tick falls inside the speculative pass's hold) */
        uint64_t display_wnd_count_addr(void); void r1_nosim(uint64_t a, uint64_t n);
        static uint64_t done; uint64_t a = display_wnd_count_addr();
        if (a && a != done && !getenv("SSBU_RUNAHEAD_COUNT_ROLL")) { r1_nosim(a, 16); done = a; LOG("r1: RA: the window's refresh count page %llx is not rolled back\n", (unsigned long long)(a & ~0xfffull)); }
    }
    if (!ra_hold_quiet(g_ra_park_ms)) return 0;
    double t1 = now_ms();
    cp_init(); g_cp_defer = 1;
    size_t n = mark_sweep(0);
    g_cp_defer = 0;
    double t2 = now_ms();
    for (int i = 0; i < g_nx; i++) cp_add_range(g_x[i].b, g_x[i].a, g_x[i].n);
    cp_run();                                            /* pages + extras, main + helper threads */
    double t3 = now_ms();
    fibers_save();
    double t4 = now_ms();
    g_ra_cb_bytes = nvn_r1_cb_save();                    /* the command buffers' recordings in progress (host data) */
    double t5 = now_ms();
    ra_release_save();
    g_ra_comp[0][0] += t2 - t1; g_ra_comp[0][1] += t3 - t2; g_ra_comp[0][2] += t4 - t3; g_ra_comp[0][3] += t5 - t4; g_ra_comp[0][4] += now_ms() - t5; g_ra_ncomp[0]++;
    g_mctx = *c;
    if (g_ra_ns < 16384) { g_ra_s[g_ra_ns] = now_ms() - t0; g_ra_sh[g_ra_ns] = t1 - t0; g_ra_sp[g_ra_ns] = n; g_ra_ns++; }
    return 1;
}
static void ra_restore(Ctx *c) {
    double t0 = now_ms();
    stop_born_threads();
    if (g_nborn) LOG("r1: RA: %d guest threads created in a speculative pass, held for good:%s\n", g_nborn, g_born);
    for (double tq = now_ms();;) {                       /* the same quiet moment as at the save (the speculative pass's jobs finished) */
        ra_hold_restore();
        if (ra_fibers_quiet() || now_ms() - tq > 200) { if (g_ra_busy_fibers) LOG("r1: RA restore: %d fibers still busy after 200 ms\n", g_ra_busy_fibers); break; }
        for (int i = 0; i < g_guest_threads.count; i++) if (g_guest_threads.table[i].held && !ra_audio(g_guest_threads.table[i].name)) { ResumeThread(g_guest_threads.table[i].h); g_guest_threads.table[i].held = 0; }
        g_ra_fiber_waits++;
        SwitchToThread();
    }
    double t1 = now_ms();
    cp_init(); g_cp_defer = 1;
    size_t n = mark_sweep(1);
    g_cp_defer = 0;
    double t2 = now_ms();
    for (int i = 0; i < g_nx; i++) cp_add_range(g_x[i].a, g_x[i].b, g_x[i].n);
    cp_run();
    double t3 = now_ms();
    restore_thread_regs();
    fibers_restore_all();
    double t4 = now_ms();
    nvn_r1_cb_restore();
    double t5 = now_ms();
    unpark();
    g_ra_comp[1][0] += t2 - t1; g_ra_comp[1][1] += t3 - t2; g_ra_comp[1][2] += t4 - t3; g_ra_comp[1][3] += t5 - t4; g_ra_comp[1][4] += now_ms() - t5; g_ra_ncomp[1]++;
    *c = g_mctx;
    if (g_guest_threads.mismatch || g_guest_fibers.not_restored) LOG("r1: RA restore at fight frame %lld: %d threads not at their save wait, %d fibers not restored\n", g_session.fight_frame, g_guest_threads.mismatch, g_guest_fibers.not_restored);
    if (g_ra_nr < 16384) { g_ra_r[g_ra_nr] = now_ms() - t0; g_ra_rh[g_ra_nr] = t1 - t0; g_ra_rp[g_ra_nr] = n; g_ra_nr++; }
}
/* Round 6, SSBU_RUNAHEAD_MODE=submit (experiment, needs a check of the pictures): no speculative pass at all. At the save point
 * the game's submission step (FUN 0x71037f0590: ends the render buffers recorded in the last real pass, records its own, submits
 * them) is CALLED directly with the arguments of its last call from the main loop; its picture is presented (pipe); then the
 * state is restored and the real pass runs (its own submission of the same buffers is not run on the GPU). Per tick: real pass
 * + save + submission + restore. Open: the render jobs of the next pass write data the submitted lists read at replay (the 0.3 %
 * of the picture of SSBU_RUNAHEAD_SKIP=2): that part may lag one frame. */
static ImportFn g_orig_submit;
static uint64_t g_sub_x0, g_sub_x1, g_sub_arr[16], g_sub_lr; static int g_sub_n, g_sub_ok, g_sub_calling; static long long g_sub_calls, g_sub_direct;
static void ra_submit_hook(Ctx *c, void *user) {      /* records the main loop's call */
    (void)user;
    if (!g_sub_calling && c->x[0] && c->x[2] <= 16) {
        g_sub_x0 = c->x[0]; g_sub_x1 = c->x[1]; g_sub_n = (int)c->x[2]; g_sub_lr = c->x[30];
        for (int i = 0; i < g_sub_n; i++) g_sub_arr[i] = rd64(g_sub_x1 + 8ull * (uint64_t)i);
        g_sub_ok = 1; g_sub_calls++;
        if (g_sub_calls <= 3 || g_sub_calls % 3000 == 0) LOG("r1: RA: submission step called from %llx: context %llx, %d buffers at %llx\n", (unsigned long long)g_sub_lr, (unsigned long long)g_sub_x0, g_sub_n, (unsigned long long)g_sub_x1);
    }
    g_orig_submit(c);
}
static void ra_direct_submit(Ctx *c) {
    Ctx t = *c;
    uint64_t sp = (c->x[31] - 0x4000) & ~15ull;          /* below the caller's frame (main's guest stack is saved / restored whole) */
    for (int i = 0; i < g_sub_n; i++) wr64(sp + 8ull * (uint64_t)i, g_sub_arr[i]);
    t.x[0] = g_sub_x0; t.x[1] = sp; t.x[2] = (uint64_t)g_sub_n; t.x[31] = sp - 0x100; t.x[30] = g_sub_lr;
    g_sub_calling = 1; g_orig_submit(&t); g_sub_calling = 0;
    g_sub_direct++;
}
static void ra_step(Ctx *c) {
    double t = now_ms();
    g_ra_kicks++;
    if (g_ra_fe_kind) { int k = g_ra_fe_kind - 1; g_rap[k][3] += t - g_ra_fe_end; g_rapn[k]++; g_ra_fe_kind = 0; }
    if (rgs_wanted()) { rgs_step(c); return; }            /* R2: the snapshot ring's rollback simulation instead of run-ahead */
    if (g_ra_phase == 1) {                               /* end of the speculative pass: its picture is presented; back to the save point */
        g_ra_spec_sum += t - g_ra_t_mark; g_ra_nspec++;
        ra_restore(c);
        g_ra_phase = 2; g_ra_t_mark = now_ms();
        return;
    }
    int was_quiet = g_ra_phase == 2;
    if (was_quiet) { g_ra_real_sum += t - g_ra_t_mark; g_ra_nreal++; }
    g_ra_phase = 0;
    int ok = 0;
    if (!g_ra_ready && (g_ra_kicks == 1 || g_ra_kicks % 300 == 0)) LOG("r1: RA: save point reached %lld times, fight frame %lld, ready-go %d\n", g_ra_kicks, g_session.fight_frame, ra_fight_on());
    if (!g_ra_off && ra_fight_on()) {
        if (!g_ra_ready) {                               /* the first time: a full save, the shadow and the marks (one hitch) */
            double t0 = now_ms();
            g_save_failed = 0; g_ra_wd_pause = 1; g_rs_drop = 1;
            save(c); release(); g_rs_drop = 0;
            g_ra_wd_pause = 0;
            if (g_save_failed == 2) { g_ra_off = 1; LOG("r1: RA: first save not made (out of memory): run-ahead OFF\n"); return; }
            if (g_save_failed) return;
            g_ra_ready = 1; g_session.cycle = 1; ok = 1;
            LOG("r1: RA: first save (full, shadow filled, marks on) at fight frame %lld in %.1f ms\n", g_session.fight_frame, now_ms() - t0);
            if (g_mark <= 0) { g_ra_off = 1; ok = 0; LOG("r1: RA: no marks (the game DLL with marks is not loaded): run-ahead OFF\n"); }
        } else ok = ra_save(c);
        if (!ok && g_ra_ready && !g_ra_off) { g_ra_skips++; if (g_ra_skips <= 20 || g_ra_skips % 100 == 0) LOG("r1: RA: save %lld skipped at fight frame %lld: thread %s not at a wait within %.1f ms\n", g_ra_skips, g_session.fight_frame, g_ra_busy ? g_ra_busy : "?", g_ra_park_ms); }
    }
    if (!ok) { if (was_quiet) g_ra_after_quiet = 1; g_ra_t_mark = now_ms(); return; }
    g_ra_specs++;
    g_ra_phase = 1; g_ra_t_mark = now_ms();
    if (g_ra_submit_mode && g_sub_ok) {                  /* submit mode: the submission step now, then back to the save point at once */
        double ts = now_ms();
        ra_direct_submit(c);
        g_ra_spec_sum += now_ms() - ts; g_ra_nspec++;
        ra_restore(c);
        g_ra_phase = 2; g_ra_t_mark = now_ms();
    }
}
static ImportFn g_orig_kick2;
static void ra_kick(Ctx *c, void *user) {            /* hook on FUN 0x710354c720 (wait on a job counter): a point at its RETURN */
    (void)user;
    uint64_t lr = c->x[30];
    double tw = now_ms();
    g_orig_kick(c);
    rgp_wait_note(lr, now_ms() - tw);
    if (lr == g_ra_site) ra_step(c);
}
/* The task workers are busy at the save point (the preparation jobs of the kick at 0x71037494f8 run for 10 ms and more),
 * but blocked and idle just before that kick. So they are HELD from that kick (H1) to the save point after the
 * submission (H2), in every pass: at H2 they are always in the same state (blocked at their idle wait, the kick's jobs
 * queued and not started, its event signaled and not consumed), which is what a save / restore needs. Cost: the kick's
 * jobs start after the submission instead of during it. A watchdog lets them go if H2 does not come within 200 ms. */
static HANDLE g_ra_wh[16]; static int g_ra_nwh; volatile LONG g_ra_wh_on; static HANDLE g_ra_wd_ev; long long g_ra_wh_fail, g_ra_wd_fired;
static void ra_workers_release(void) { if (InterlockedExchange(&g_ra_wh_on, 0)) for (int i = 0; i < g_ra_nwh; i++) ResumeThread(g_ra_wh[i]); g_ra_nwh = 0; }
static DWORD WINAPI ra_watchdog(void *p) {
    (void)p;
    for (;;) {
        WaitForSingleObject(g_ra_wd_ev, INFINITE);
        double t0 = now_ms();
        while (WaitForSingleObject(g_ra_wd_ev, 200) == WAIT_TIMEOUT) {
            if (g_ra_wd_pause == 1) continue;            /* (the first, full save takes seconds; R3: the safe point's waits) */
            if (g_ra_wd_pause == 2 && now_ms() - t0 < 10000) continue;      /* R3 online match: a slow pass must not let them go (10 s: H2 never came) */
            if (g_ra_wh_on) { g_ra_wd_fired++; ra_workers_release(); }
            break;
        }
    }
    return 0;
}
static void ra_workers_hold(void) {
    Ctx *tc; const char *nm; HANDLE h; DWORD me = GetCurrentThreadId();
    g_ra_nwh = 0;
    for (int i = 0; guest_thread_info(i, &tc, &nm, &h) && g_ra_nwh < 16; i++) {
        if (!h || GetThreadId(h) == me || !nm || !strstr(nm, "TaskWorker")) continue;
        if (SuspendThread(h) == (DWORD)-1) continue;
        if (!th_blocked(h)) { ResumeThread(h); g_ra_wh_fail++; continue; }      /* busy: not held (that pass will likely skip its save) */
        g_ra_wh[g_ra_nwh++] = h;
    }
    InterlockedExchange(&g_ra_wh_on, 1);
    SetEvent(g_ra_wd_ev);
}
static void ra_kick2(Ctx *c, void *user) {           /* hook on FUN 0x7103549170 (kick of a job group): points BEFORE the call */
    (void)user;
    uint64_t lr = c->x[30];
    if (lr == GA(0x71037494fcull) && g_ra_site == GA(0x7103749b24ull) && !g_ra_off && (g_ra_phase || ra_fight_on())) {
        ra_workers_hold();      /* H1 */
        {   /* (hs10, test only) SSBU_RB_NET_TEST_H1=<fight frame>,<ms>: a slow pass at the first H1 from that fight frame on (the hold watchdog's case) */
            static long long tf = -2; static int tms;
            if (tf == -2) { const char *e = getenv("SSBU_RB_NET_TEST_H1"); tf = -1; if (e && *e) sscanf(e, "%lld,%d", &tf, &tms); }
            if (tf > 0 && g_session.fight_frame >= tf && g_ra_phase == 0) { tf = -1; Sleep((DWORD)tms); LOG("r1: TEST: a slow pass of %d ms at fight frame %lld, after the workers' hold (watchdog releases so far %lld)\n", tms, g_session.fight_frame, g_ra_wd_fired); }
        }
    }
    if (lr == g_ra_site) { ra_step(c); if (g_ra_wh_on) { ra_workers_release(); SetEvent(g_ra_wd_ev); } g_ra_wd_pause = g_ra_wd_base; }      /* H2 (R3: rb_rollback.c pauses the watchdog while it runs here, and keeps the long limit during the match) */
    if ((g_ra_phase == 1 && g_ra_skip) || rgs_skip_now()) {      /* SSBU_RUNAHEAD_SKIP (experiment): job groups of the speculative pass whose result is thrown away (R2: SSBU_RB_SIM_SKIP, re-run passes but the last) */
        int prep = c->x[30] == GA(0x7103749b24ull) || c->x[30] == GA(0x710374b4f8ull) || c->x[30] == GA(0x710374b528ull) || c->x[30] == GA(0x710374b558ull), rend = c->x[30] == GA(0x710374c0d8ull);
        int logic = c->x[30] == GA(0x710374c398ull) || c->x[30] == GA(0x710374c5b8ull);
        int la = c->x[30] == GA(0x71037494fcull), lb = c->x[30] == GA(0x7103749598ull);      /* (R2) preparation lists A / B */
        int sk = g_ra_phase == 1 ? g_ra_skip : rgs_skip_now();
        if (((sk & 1) && prep) || ((sk & 2) && rend) || ((sk & 4) && logic) || (g_ra_phase == 2 && (((sk & 16) && la) || ((sk & 32) && lb)))) {
            if (rend && g_ra_phase == 2) {                    /* (R2) the culling job of the render list sets this byte; the main loop spins on it at 0x7103749f48 */
                uint64_t p = rd64(GA(0x71053365a8ull)); void r1_wrote(uint64_t a, uint64_t n);
                if (p) { r1_wrote(p + 0x140, 1); *(volatile uint8_t *)(uintptr_t)(p + 0x140) = 1; }
            }
            g_ra_skipped_kicks++; return;
        }
    }
    g_orig_kick2(c);
}
static void ra_line(Ctx *c, void *user) {           /* the fighters' frame line: a fight frame */
    (void)user;
    InterlockedIncrement(&g_ra_lines);
    if (g_ra_phase == 1 && (g_ra_skip & 8)) { g_ra_skipped_lines++; return; }      /* SSBU_RUNAHEAD_SKIP bit 8: a speculative pass's fighters do not run their frame (its result is thrown away) */
    g_orig_line(c);
}
static void ra_report(void); 
void ra_frame_end(double te) {
    double fe = now_ms();
    if (g_ra_ready && g_t_poll > g_ra_t_mark && te > g_t_poll) { int k = g_ra_phase; g_rap[k][0] += g_t_poll - g_ra_t_mark; g_rap[k][1] += te - g_t_poll; g_rap[k][2] += fe - te; g_ra_fe_end = fe; g_ra_fe_kind = k + 1; }
    g_ra_fe++;
    { static long long n; if (++n % 1200 == 0 && (g_ra_ready || rbg_noring_now())) ra_report(); }      /* (hs14: a match without the ring too) */
    if (InterlockedExchange(&g_ra_lines, 0) > 0) {      /* this iteration ran a fight frame: real or speculative (for tests\r1_ra_cmp.py) */
        if (g_ra_nkind >= g_ra_kcap) { g_ra_kcap = g_ra_kcap ? g_ra_kcap * 2 : 65536; g_ra_kind = realloc(g_ra_kind, (size_t)g_ra_kcap); }
        g_ra_kind[g_ra_nkind++] = g_ra_phase == 1 || (g_ra_phase == 2 && rgs_wanted()) ? 'S' : 'R';      /* (R2 simulation: a re-run pass = 'S') */
    }
}
static double pct(double *v, int n, int p) { if (!n) return 0; qsort(v, (size_t)n, sizeof *v, cmp_d); return v[(size_t)n * p / 100 < (size_t)n ? (size_t)n * p / 100 : (size_t)n - 1]; }
static void ra_report(void) {
    if (g_ra <= 0) return;
    double ps = 0, pr = 0; for (int i = 0; i < g_ra_ns; i++) ps += (double)g_ra_sp[i]; for (int i = 0; i < g_ra_nr; i++) pr += (double)g_ra_rp[i];
    LOG("r1: RA report: %lld speculative passes, %lld saves skipped (a thread not at a wait or a job running), %lld kicks; %lld holds waited for running jobs (%.3f ms in all at saves); spec pass mean %.3f ms, real pass mean %.3f ms (wall, kick to kick)\n",
        g_ra_specs, g_ra_skips, g_ra_kicks, g_ra_fiber_waits, g_ra_fiber_wait_ms, g_ra_nspec ? g_ra_spec_sum / g_ra_nspec : 0, g_ra_nreal ? g_ra_real_sum / g_ra_nreal : 0);
    LOG("r1: RA save: median %.3f p99 %.3f ms (hold median %.3f p99 %.3f), %.0f pages mean; restore: median %.3f p99 %.3f ms (hold median %.3f p99 %.3f), %.0f pages mean; extras %.1f MB each way\n",
        pct(g_ra_s, g_ra_ns, 50), pct(g_ra_s, g_ra_ns, 99), pct(g_ra_sh, g_ra_ns, 50), pct(g_ra_sh, g_ra_ns, 99), g_ra_ns ? ps / g_ra_ns : 0,
        pct(g_ra_r, g_ra_nr, 50), pct(g_ra_r, g_ra_nr, 99), pct(g_ra_rh, g_ra_nr, 50), pct(g_ra_rh, g_ra_nr, 99), g_ra_nr ? pr / g_ra_nr : 0, g_xbytes / 1048576.0);
    LOG("r1: RA holds that failed (attempts): task worker not blocked %lld, audio thread not blocked %lld, other thread %lld, a job fiber running %lld; site %llx; workers busy at H1 %lld, watchdog releases %lld; kicks skipped in speculative passes %lld, fighters' frame lines skipped %lld (mask %d); command-buffer recordings saved %.1f KB (last)\n", g_ra_skip_why[0], g_ra_skip_why[1], g_ra_skip_why[2], g_ra_skip_why[3], (unsigned long long)g_ra_site, g_ra_wh_fail, g_ra_wd_fired, g_ra_skipped_kicks, g_ra_skipped_lines, g_ra_skip, g_ra_cb_bytes / 1024.0);
    for (int k = 0; k < 2; k++) if (g_ra_ncomp[k]) LOG("r1: RA %s parts (means, ms): map sweep %.3f, copy of the pages + extras (parallel) %.3f, fibers %.3f, command buffers %.3f, threads let go %.3f\n", k ? "restore" : "save",
        g_ra_comp[k][0] / g_ra_ncomp[k], g_ra_comp[k][1] / g_ra_ncomp[k], g_ra_comp[k][2] / g_ra_ncomp[k], g_ra_comp[k][3] / g_ra_ncomp[k], g_ra_comp[k][4] / g_ra_ncomp[k]);
    for (int k = 0; k < 3; k++) if (g_rapn[k]) LOG("r1: RA %s passes (%lld), means ms: pass start -> poll %.3f, poll -> frame end %.3f, frame end call %.3f, frame end -> save point %.3f\n",
        k == 0 ? "normal" : k == 1 ? "speculative" : "real", g_rapn[k], g_rap[k][0] / g_rapn[k], g_rap[k][1] / g_rapn[k], g_rap[k][2] / g_rapn[k], g_rap[k][3] / g_rapn[k]);
    static long long printed;
    for (long long i = printed; i < g_ra_nkind; i += 200, printed = i < g_ra_nkind ? i : g_ra_nkind) LOG("r1: RA kinds %lld: %.*s\n", i + 1, (int)(g_ra_nkind - i < 200 ? g_ra_nkind - i : 200), g_ra_kind + i);
    if (rgs_wanted()) rgs_report();
}

void ra_init(void) {
    g_ra = 1;
    if (getenv("SSBU_RUNAHEAD_PARK_MS")) g_ra_park_ms = atof(getenv("SSBU_RUNAHEAD_PARK_MS"));
    g_ra_site = GA(0x7103749b24ULL);
    if (getenv("SSBU_RUNAHEAD_SITE")) g_ra_site = strtoull(getenv("SSBU_RUNAHEAD_SITE"), NULL, 16);
    if (getenv("SSBU_RUNAHEAD_SKIP")) g_ra_skip = atoi(getenv("SSBU_RUNAHEAD_SKIP"));
    { const char *m = getenv("SSBU_RUNAHEAD_MODE"); g_ra_submit_mode = m && !strcmp(m, "submit"); }
    if (g_ra_submit_mode) LOG("r1: RUN-AHEAD mode submit (no speculative pass; the submission step 71037f0590 is called at the save point): hook %s\n", hook_install(GA(0x71037f0590ull), ra_submit_hook, NULL, &g_orig_submit) ? "installed" : "FAILED");
    g_ra_wd_ev = CreateEventA(NULL, FALSE, FALSE, NULL); CloseHandle(CreateThread(NULL, 0, ra_watchdog, NULL, 0, NULL));
    g_guest_threads.park_ms = 20;
    g_mark = mark_bind(); if (!g_mark) g_mark = ls_use_exe_maps();      /* R4: no marked DLL: the learned set (rb_learned.c) */
    int k = hook_install(GA(0x710354c720ull), ra_kick, NULL, &g_orig_kick) && hook_install(GA(0x7103549170ull), ra_kick2, NULL, &g_orig_kick2);
    int l = hook_install_name("_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv", ra_line, NULL, &g_orig_line);
    LOG("r1: RUN-AHEAD on (one frame): marks %s; save point: the return of the job-counter wait 710354c720 called at 710374c028 (preparation done, before the render kick): %s; frame line counter: %s; park %.1f ms\n",
        g_mark > 0 ? "yes" : "NO (run-ahead will stay off)", k ? "installed" : "FAILED", l ? "installed or waiting" : "FAILED", g_ra_park_ms);
    if (rgs_wanted() && rbg_on()) {                       /* R3: re-run passes but the last skip the preparation kicks + lists (49) and the presentation callees (127), proven exact in R2 round 3 */
        g_rgs_skip = getenv("SSBU_RB_SIM_SKIP") ? atoi(getenv("SSBU_RB_SIM_SKIP")) : 49;
        if (!getenv("SSBU_RB_SIM_LITE")) _putenv("SSBU_RB_SIM_LITE=127");
        rbn_init();
        LOG("r1: RB NET on (R3 online rollback on the snapshot ring): kick skip mask in re-run passes %d\n", g_rgs_skip);
        rgs_lite_init(); }
    else if (rgs_wanted()) { if (getenv("SSBU_RB_SIM_SKIP")) g_rgs_skip = atoi(getenv("SSBU_RB_SIM_SKIP"));
        LOG("r1: RB SIM on (R2 snapshot ring): a rollback every %d real passes, k = 1..%d rotating, after %d captures; kick skip mask in re-run passes %d\n", g_rgs_n, g_rgs_k, g_rgs_warm, g_rgs_skip);
        rgs_lite_init(); }
    atexit(ra_report);
}

