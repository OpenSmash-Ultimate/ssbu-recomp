/* Track R1 (TEST ONLY, behind SSBU_R1): save / restore of the simulation state, the base of run-ahead and rollback.
 *   SSBU_R1=<N>,<K>[,<P>,<C>]  at fight frame N (counted at player 1's pad poll from the input mark "fight", one per
 *   fight frame in SSBU_DETERMINISTIC mode) the guest state is SAVED at the main loop's frame end (main 0x710386fc30
 *   called from 0x7103747b78: logic of the iteration done, present / tick / fence done); the fight runs K frames
 *   (player 1's pad values are recorded); at fight frame N + K the state is RESTORED and the same K frames run again
 *   with the recorded pad values (the input sequence does not advance meanwhile, so afterwards it continues exactly
 *   where the first pass left it); then every P frames again, C cycles in all (default P = 0, C = 1).
 * What is saved: every committed read-write page of (1) the game's address region(s) (nn::os::AllocateAddressRegion,
 * the one reservation of >= 16 GB), (2) the guest image and the loaded modules (0x7100000000..0x7300000000: .data /
 * .bss), (3) the main thread's guest stack, (4) the guest TLS blocks (tpidr) of every guest thread; plus the main
 * thread's guest registers at the save point. Restore = page compare, only differing pages are copied back.
 * Check: at the end of the re-run the pages the first pass changed are compared with their first-pass end state
 * (SSBU_R1 report "verify"), next to the fighters' hash (SSBU_FIGHT_HASH) that the comparison script reads.
 * Nothing here runs without SSBU_R1. No game content in this file. */
#ifndef _WIN32
/* Linux build: not available yet. The save / restore is built on Windows' vectored exception handlers (the write marks)
 * and on reading suspended threads' contexts; on Linux that would be signal handlers and ucontext. Off as when SSBU_R1 and
 * runahead are not set: every entry answers "off", so the callers take their normal paths. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <windows.h>
__declspec(dllexport) uint8_t *r1_m1, *r1_m2;
__declspec(dllexport) uint64_t r1_lo1, r1_n1, r1_n2;
int r1_on(void) {
    static int said;
    int gfx_runahead(void);
    if (!said && ((getenv("SSBU_R1") && getenv("SSBU_R1")[0] != '0') || (getenv("SSBU_RUNAHEAD") ? getenv("SSBU_RUNAHEAD")[0] != '0' : gfx_runahead()))) {
        said = 1;
        printf("r1: state save / restore (SSBU_R1, run-ahead) is not available in the Linux build yet: off\n");
    }
    return 0;
}
int r1_ra_phase(void) { return 0; }
int r1_ra_quiet(void) { return 0; }
int r1_ra_present_mode(void) { return 0; }
void r1_stats_report(void) {}
int r1_poll(int ticks) { (void)ticks; return 0; }
void r1_pad(uint64_t *b, int *lx, int *ly, int *rx, int *ry) { (void)b; (void)lx; (void)ly; (void)rx; (void)ry; }
void r1_init(void) { r1_on(); }
void r1_on_decommit(void *p, size_t n) { (void)p; (void)n; }
void r1_on_commit(void *p, size_t n) { (void)p; (void)n; }
void r1_wrote(uint64_t a, uint64_t n) { (void)a; (void)n; }
void r1_ww_note(void *const *pages, size_t cnt) { (void)pages; (void)cnt; }
void r1_ww_note_range(const void *p, size_t n) { (void)p; (void)n; }
int r1_ww_taken(const void *p, size_t n) { (void)p; (void)n; return 0; }
void r1_nosim(uint64_t a, uint64_t n) { (void)a; (void)n; }
void r1_read_retry_note(void) {}
void r1_register_heap(void) { r1_on(); }
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "game_state_internal.h"                         /* what rb_learned.c (its own translation unit) shares with this file */
/* Functions and variables of other runtime files that only this file uses: */
extern int g_r1_keep_fibers;

void os_r1_objs(void (*cb)(uint64_t key, int signaled, int count, uint64_t len, uint64_t thread, uint64_t stack, uint64_t stack_size));     /* os_threads.c */

R1Session g_session = { .enabled = -1 };      /* the one test session (see R1Session in game_state_internal.h) */
Ctx g_ctx_save;
static double g_qpf;
double now_ms(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / g_qpf; }
FILE *g_log;
/* (ot2) a 310 ms freeze in BOTH games at once, near frame 1283: the game thread was in fflush < WriteFile under rbg_step_body,
 * a LOG line waiting on the disk (the build is on a hard drive). A match logs ~370 lines, one per rollback, each flushed on the
 * game thread. So while a rollback match is on the lines go to memory and a host thread (not a guest thread: no rollback holds it)
 * writes them every 50 ms; the rest of the time LOG writes and flushes as before. SSBU_R1_LOG_SYNC=1: always straight. */
#define RBL_CAP (4u << 20)
static char g_rbl_buf[2][RBL_CAP]; static size_t g_rbl_n; static int g_rbl_cur, g_rbl_on, g_rbl_sync = -1; static long long g_rbl_lost;
static CRITICAL_SECTION g_rbl_cs, g_rbl_wcs; static INIT_ONCE g_rbl_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK rbl_init(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_rbl_cs); InitializeCriticalSection(&g_rbl_wcs); return TRUE; }
void rb_log_drain(void) {
    if (!g_log) return;
    InitOnceExecuteOnce(&g_rbl_once, rbl_init, NULL, NULL);
    EnterCriticalSection(&g_rbl_wcs);                   /* one writer at a time: the buffer it writes is not the one LOG fills */
    EnterCriticalSection(&g_rbl_cs);
    char *b = g_rbl_buf[g_rbl_cur]; size_t n = g_rbl_n; long long lost = g_rbl_lost;
    g_rbl_cur ^= 1; g_rbl_n = 0; g_rbl_lost = 0;
    LeaveCriticalSection(&g_rbl_cs);
    if (n) fwrite(b, 1, n, g_log);
    if (lost) fprintf(g_log, "r1: LOG: %lld lines lost (the memory buffer was full)\n", lost);
    if (n || lost) fflush(g_log);
    LeaveCriticalSection(&g_rbl_wcs);
}
static DWORD WINAPI rbl_thread(LPVOID p) { (void)p; for (;;) { Sleep(50); rb_log_drain(); } }
void rb_log_async(int on) {
    if (g_rbl_sync < 0) { const char *e = getenv("SSBU_R1_LOG_SYNC"); g_rbl_sync = e && e[0] == '1'; }
    if (g_rbl_sync || !g_log) return;
    InitOnceExecuteOnce(&g_rbl_once, rbl_init, NULL, NULL);
    static int started;
    if (on && !started) { HANDLE h = CreateThread(NULL, 0, rbl_thread, NULL, 0, NULL); if (!h) return; CloseHandle(h); started = 1; }
    if (on) { g_rbl_on = 1; return; }
    g_rbl_on = 0; rb_log_drain();                       /* off: what is in memory first, so the lines stay in order */
}
void rb_log(const char *fmt, ...) {
    char sb[1024], *b = sb; va_list ap;
    va_start(ap, fmt); int n = vsnprintf(sb, sizeof sb, fmt, ap); va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof sb && (b = malloc((size_t)n + 1))) { va_start(ap, fmt); vsnprintf(b, (size_t)n + 1, fmt, ap); va_end(ap); }
    if (!b) { b = sb; n = sizeof sb - 1; }
    if (g_rbl_on) {
        EnterCriticalSection(&g_rbl_cs);
        if (g_rbl_n + (size_t)n <= RBL_CAP) { memcpy(g_rbl_buf[g_rbl_cur] + g_rbl_n, b, (size_t)n); g_rbl_n += (size_t)n; } else g_rbl_lost++;
        LeaveCriticalSection(&g_rbl_cs);
    } else if (g_log) {
        InitOnceExecuteOnce(&g_rbl_once, rbl_init, NULL, NULL);
        EnterCriticalSection(&g_rbl_wcs);               /* after a drain still writing, not in between its lines */
        fwrite(b, 1, (size_t)n, g_log); fflush(g_log);
        LeaveCriticalSection(&g_rbl_wcs);
    }
    if (b != sb) free(b);
}
char g_born[2048]; int g_nborn;
uint64_t g_catch_addr;
__declspec(dllexport) uint8_t *r1_m1, *r1_m2;          /* the exe's own copy (r1src\sdklift_gen.c built with R1_MARK) */
__declspec(dllexport) uint64_t r1_lo1, r1_n1, r1_n2;
int g_fast = -1, g_learn = 5, g_fcheck;

/* end-of-first-pass copies of the pages the first pass changed */

EPage *g_e; size_t g_ne, g_ecap;
/* player 1's pad values, recorded in the first pass and given again in the re-run */

static PadRec g_pad[8192]; static int g_npad, g_ipad;
/* timing */
static double g_t_pass1, g_t_rerun, g_t_first_frame;
static long long g_pass_frames;

/* run-ahead (round 5): SSBU_RUNAHEAD=1 (or graphics.ini `runahead = on`, gfx_options.c); default OFF.
 * R2 (r1_ring.c): SSBU_RB_SIM (the snapshot ring's rollback simulation) uses the same machinery; default OFF. */
     
int ra_wanted(void) {
    static int w = -1;
    if (w < 0) { const char *e = getenv("SSBU_RUNAHEAD"); int gfx_runahead(void); w = rgs_wanted() || (e ? (e[0] && e[0] != '0') : gfx_runahead()); }
    return w;
}
int r1_on(void) {
    if (g_session.enabled < 0) {
        const char *e = getenv("SSBU_R1");
        g_session.enabled = (e && e[0] && e[0] != '0') || ra_wanted();
        if (g_session.enabled) {
            if (e) sscanf(e, "%d,%d,%d,%d", &g_session.start_frame, &g_session.run_frames, &g_session.period, &g_session.cycles);
            if (g_session.run_frames <= 0) g_session.run_frames = 8;
            if (g_session.cycles <= 0) g_session.cycles = 1;
            if (g_session.period < 2 * g_session.run_frames + 2) g_session.period = 2 * g_session.run_frames + 2;
            g_session.target_frame = g_session.start_frame;
            LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_qpf = (double)f.QuadPart;
            const char *lp = getenv("SSBU_R1_LOG");
            g_log = lp && lp[0] ? fopen(lp, "w") : NULL;
            if (!g_log) g_log = stdout;
        }
    }
    return g_session.enabled;
}

/* ---------------------------------------------------------------- the hooks */
static ImportFn g_orig_frame_end;
static int g_postponed, g_k_run; static long long g_save_ff;
extern int g_r1_keep_fibers;
static double g_t_mark;
/* round 2 (3): per fight frame on main: poll -> frame-end entry (the logic step and what the loop does after it), the
 * frame end itself (present / acquire / tick / fence), and frame-end to frame-end; for normal frames and re-run frames.
 */
double g_t_poll, g_t_fe_prev; static int g_fe_busy;
static double g_st[2][3]; static long long g_stn[2];
static void r1_stats_now(void);
void r1_stats_report(void) {
    static int done; if (done++) return;
    r1_stats_now();
}
static void r1_stats_now(void) {
    for (int k = 0; k < 2; k++) if (g_stn[k])
        LOG("r1: TIMING %s frames (%lld): poll -> frame end %.3f ms, frame end call %.3f ms, frame end -> frame end %.3f ms (means, main thread, wall)\n",
            k ? "re-run" : "normal", g_stn[k], g_st[k][0] / g_stn[k], g_st[k][1] / g_stn[k], g_st[k][2] / g_stn[k]);
}
static void r1_frame_end(Ctx *c, void *user) {
    (void)user;
    uint64_t lr = c->x[30];
    double te = now_ms();
    { extern volatile int g_rgp_seg; if (g_rgp_seg == 2) g_rgp_seg = 3; }
    int rerun = g_session.phase == 2;
    g_orig_frame_end(c);         /* (round 2 tried skipping the whole frame end in re-run frames: the game crashed in the first one) */
    if (lr != GA(0x7103747b7cull)) return;
    if (g_ra > 0) { fls_index_find(); ra_frame_end(te); return; }         /* run-ahead (round 5): the save / restore is at the kick after the submission, not here */
    if (g_session.fight_frame > 0 && g_t_poll > 0 && g_t_fe_prev > 0 && !g_fe_busy) { int k = rerun; g_st[k][0] += te - g_t_poll; g_st[k][1] += now_ms() - te; g_st[k][2] += te - g_t_fe_prev; g_stn[k]++; }
    g_t_fe_prev = te;
    { static long long nfe; if (++nfe % 600 == 0) { LOG("r1: (frame end %lld)\n", nfe); r1_stats_now(); } } { int ph = g_session.phase; static int last_ph; g_fe_busy = 0; (void)last_ph; last_ph = ph; }
    fls_index_find();
    if (g_session.fight_frame > 0 && g_session.phase < 3) {
        int idle = fibers_idle();
        if (!idle && g_wait_idle && (g_session.phase == 0 || g_session.phase == 1) && g_session.fight_frame >= g_session.target_frame) { g_postponed++; if (g_busy_log > 0) g_busy_log--; return; }   /* a job is half done: the next frame end */
    }
    if (g_session.phase == 0 && g_session.fight_frame >= g_session.target_frame) {
        if (g_postponed) LOG("r1: save postponed %d frame end(s): a fiber was in the middle of a job\n", g_postponed);
        g_postponed = 0;
        if (g_mark > 0) { if (g_session.cycle == 0) save(c); else mark_save(c); }      /* (cycle 0: save() starts the marks itself) */
        else if (g_fast > 0 && g_session.cycle >= g_learn) { if (!g_nfp) fast_build(); if (g_fcheck) save(c); fast_save(c); } else save(c);
        g_session.phase = 1; g_fe_busy = 1; g_npad = 0; g_save_ff = g_session.fight_frame; g_session.target_frame = g_session.fight_frame + g_session.run_frames; g_t_mark = now_ms();
    } else if (g_session.phase == 1 && g_session.fight_frame >= g_session.target_frame) {
        if (g_postponed) LOG("r1: restore postponed %d frame end(s): a fiber was in the middle of a job\n", g_postponed);
        g_postponed = 0; g_k_run = (int)(g_session.fight_frame - g_save_ff);
        g_t_pass1 = now_ms() - g_t_mark;
        if (g_mark > 0) mark_restore(c);
        else if (g_fast > 0 && g_session.cycle >= g_learn) fast_restore(c); else restore(c);
        g_session.phase = 2; g_fe_busy = 1; g_ipad = 0; g_session.fight_frame = g_save_ff; g_session.target_frame = g_session.fight_frame + g_k_run; g_t_mark = now_ms();
        if (g_ar) LOG("r1: guest C heap: %llu allocations, %llu frees so far (counters inside the heap: rolled back too)\n", (unsigned long long)((uint64_t *)g_ar)[50], (unsigned long long)((uint64_t *)g_ar)[51]);
        LOG("r1: first pass %d frames in %.2f ms (%.2f ms per frame, wall, render included); %d pad polls recorded\n", g_k_run, g_t_pass1, g_t_pass1 / g_k_run, g_npad);
    } else if (g_session.phase == 2 && g_session.fight_frame == g_session.target_frame) {
        g_t_rerun = now_ms() - g_t_mark;
        LOG("r1: re-run %d frames in %.2f ms (%.2f ms per frame); %d of %d recorded pad polls given\n", g_k_run, g_t_rerun, g_t_rerun / g_k_run, g_ipad, g_npad);
        if (g_mark <= 0 && (!(g_fast > 0 && g_session.cycle >= g_learn) || g_fcheck)) verify();
        release(); g_fe_busy = 1;
        g_session.cycle++;
        if (g_session.cycle < g_session.cycles) { g_session.phase = 0; g_session.target_frame = g_session.fight_frame + g_session.period - g_k_run; } else { g_session.phase = 3; LOG("r1: done, %d cycles\n", g_session.cycle); fast_report(); mark_report(); if (g_prot > 0) prot_report(); r1_stats_report(); }
    }
}
/* nnhid.c, player 1's poll: ticks = fight frames since the last poll (0 before the fight). Returns 1 while the
 * re-run is replaying (the input sequence must not advance). */
int r1_poll(int ticks) {
    g_session.fight_frame += ticks;
    g_t_poll = now_ms();
    { extern volatile int g_rgp_seg; g_rgp_seg = 2; }
    if (g_ra > 0 && rgs_wanted()) return rgs_poll();     /* R2 simulation: a re-run pass's poll does not advance it either */
    if (g_ra > 0) return g_ra_phase == 1;                 /* run-ahead: the speculative pass's poll does not advance the input sequence */
    return g_session.phase == 2 && g_ipad < g_npad;
}
/* nnhid.c, player 1's values just before they are written: recorded in the first pass, replaced in the re-run */
/* nnhid.c, port 2's values just before they are written (R3 online rollback only) */
void r1_pad_id(int id, uint64_t *b, int *lx, int *ly, int *rx, int *ry) { if (g_ra > 0 && rgs_wanted() && rbg_on()) rbg_pad(id, b, lx, ly, rx, ry); }
int r1_net_on(void) { return rbn_on() > 0; }
void r1_pad(uint64_t *b, int *lx, int *ly, int *rx, int *ry) {
    if (g_ra > 0 && rgs_wanted()) { rgs_pad(b, lx, ly, rx, ry); return; }      /* R2 simulation: recorded per pass, given again in its re-run */
    if (g_ra > 0) {                                       /* run-ahead: the speculative pass gets player 1's last real values */
        if (g_ra_phase == 1) { *b = g_ra_pad.b; *lx = g_ra_pad.lx; *ly = g_ra_pad.ly; *rx = g_ra_pad.rx; *ry = g_ra_pad.ry; }
        else { PadRec r = {*b, *lx, *ly, *rx, *ry}; g_ra_pad = r; }
        return;
    }
    if (g_session.phase == 1 && g_npad < 8192) { PadRec r = {*b, *lx, *ly, *rx, *ry}; g_pad[g_npad++] = r; }
    else if (g_session.phase == 2 && g_ipad < g_npad) { PadRec r = g_pad[g_ipad++]; *b = r.b; *lx = r.lx; *ly = r.ly; *rx = r.rx; *ry = r.ry; }
}
void r1_init(void) {
    if (!r1_on()) return;
    if (getenv("SSBU_R1_PARK_MS")) g_guest_threads.park_ms = atoi(getenv("SSBU_R1_PARK_MS"));
    AddVectoredExceptionHandler(1, r1_veh);
    atexit(r1_stats_report);
    g_audio_free = getenv("SSBU_R1_AUDIO_FREE") != NULL; g_async_cap = 65536; g_async = malloc(g_async_cap * sizeof *g_async);
    g_mark = getenv("SSBU_R1_MARK") != NULL;
    g_prot = getenv("SSBU_R1_MARK_PROT") != NULL; if (g_prot) AddVectoredExceptionHandler(1, prot_veh); g_mcheck = getenv("SSBU_R1_MARK_CHECK") != NULL; if (g_mark && !mark_bind()) g_mark = 0;
    g_fast = getenv("SSBU_R1_FAST") != NULL; g_fcheck = getenv("SSBU_R1_FAST_CHECK") != NULL; if (getenv("SSBU_R1_LEARN")) g_learn = atoi(getenv("SSBU_R1_LEARN"));
    g_r1_keep_fibers = 1; g_wait_idle = getenv("SSBU_R1_WAIT_IDLE") != NULL; g_busy_log = g_wait_idle ? 3 : 0;
    ppool_init();                                          /* R4: the presentation pool of the learned-set save */
    call_trace_init();                                     /* (debug, SSBU_TRACE_CALL) a hook on one guest function */
    if (ra_wanted()) ra_init(); else g_ra = 0;
    int id = hook_install(GA(0x710386fc30ull), r1_frame_end, NULL, &g_orig_frame_end);
    LOG("r1: SSBU_R1 save at fight frame %d, %d frames, every %d frames, %d cycles; hook on the frame end 710386fc30: %s\n", g_session.start_frame, g_session.run_frames, g_session.period, g_session.cycles, id ? "installed" : "FAILED");
}

#endif
