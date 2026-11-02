/* The contract between the rollback state files. SPDX-License-Identifier: GPL-3.0-or-later
 *
 * r1_state.c (the process-wide save: windows, shadow copy, page compare, thread table) includes r1_ring.c (the snapshot ring)
 * and rb_rollback.c, so those three are still ONE translation unit. rb_learned.c (which pages the game writes, and the
 * presentation pool) is its own translation unit: this header is everything it shares with the others, in both directions.
 * Nothing else may be reached across the boundary. Keep the list short; when something here can go, remove it.
 *
 * Only Windows builds use these files (the Linux / macOS build of r1_state.c is a set of stubs and has no rb_learned.c). */
#ifndef R1_INTERNAL_H
#define R1_INTERNAL_H
#include <stdint.h>
#include <stdio.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "../rollback.h"                  /* RbPad and the rest of the netcode contract */

/* ---- what r1_state.c provides ------------------------------------------------------------------------------------------ */
#define PG 4096                              /* the page size every table here is counted in */
#define WIN2 0x7100000000ull                 /* window 2: the guest image and the loaded modules start here ... */
#define WN2 0x200000000ull                   /* ... and it is this many bytes long */
#define LOG(...) rb_log(__VA_ARGS__)          /* a line of the r1 log file */
extern FILE *g_log;                          /* the r1 log file */
void rb_log(const char *fmt, ...);          /* game_state.c: straight to g_log, or into memory while a rollback match is on (rb_log_async) */
void rb_log_async(int on);                  /* on: LOG lines go to memory, a host thread writes them; off: the rest written now, then straight again */
void rb_log_drain(void);                    /* write out what is in memory now (the crash filter) */

/* A run of guest memory the save covers (a, n bytes), with its copy in the shadow (s) and per-page flags. */
typedef struct { uint8_t *a; size_t n; uint8_t *s; uint8_t *dirty; uint8_t *learn; uint8_t *async; } Rng;   /* dirty / learn: one byte per page */
/* The memory map (memory_map.c): every range of guest memory the save covers, and the big address regions the game reserved.
 * Only memory_map.c changes it; everyone else reads it. */
#define MAXR 8192                         /* most ranges the map can hold */
typedef struct {
    Rng ranges[MAXR];                        /* the ranges the save covers, sorted by address, no overlaps */
    int range_count;                         /* how many entries of ranges[] are in use */
    uint8_t *heap_base[8];                   /* start of each address region of 16 GB or more that the game reserved (its heap) */
    int heap_count;                          /* how many entries of heap_base[] are in use */
} MemoryMap;
extern MemoryMap g_memory_map;

extern uint8_t *g_win1;                      /* window 1: where the game's big address region starts ... */
extern uint64_t g_wn1;                       /* ... and its size in bytes */
extern uint8_t *g_dc;                        /* window 1, one byte per page: 1 = the page is decommitted */
extern uint8_t *g_ns1, *g_ns2;               /* "not simulation" flags, one byte per page of window 1 / window 2 (host stacks, audio output) */
extern int g_audio_free;                     /* SSBU_R1_AUDIO_FREE: asynchronously written (audio) pages are left out of the save */

/* The write-mark maps. A game DLL built with store marks writes a 1 in m1 / m2 for each page it writes; pm1 pm2 plo1 pn1 pn2 point at
 * wherever that DLL keeps them. ls_use_exe_maps points them at the exe's own r1_m1 r1_m2 r1_lo1 r1_n1 r1_n2 instead, which nothing marks. */
extern uint8_t **pm1, **pm2; extern uint64_t *plo1, *pn1, *pn2;
__declspec(dllexport) extern uint8_t *r1_m1, *r1_m2;
__declspec(dllexport) extern uint64_t r1_lo1, r1_n1, r1_n2;

extern size_t g_ncj;                         /* page jobs queued for the parallel copy / compare */
extern uint8_t *g_cp_cmp;                    /* when set, the queued jobs are COMPARED (g_cp_cmp[i] = 1 if the pages differ) instead of copied */
void cp_add(uint8_t *d, const uint8_t *s);   /* queues the page copy / compare d <-> s */
void cp_run(void);                           /* runs every queued job on the main and helper threads, then empties the queue */
int is_async(uint8_t *a);                    /* 1 when page `a` is written asynchronously (audio output) and left out of the save */
double now_ms(void);                         /* milliseconds on the high-resolution clock */

int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);      /* hostrt.c: a hook on a game function */
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h);                           /* the i-th guest thread, 0 when there is none */

/* ---- what r1_ring.c provides ------------------------------------------------------------------------------------------- */
int rgs_wanted(void);                        /* 1 when the ring is wanted (online rollback, or the SSBU_RB_SIM test) */
extern uint8_t **g_rg_xa;                  /* extras page e (in the order of g_x): its address */
uint8_t *rg_addr(uint32_t id);               /* address of the page with ring id `id` (window 1, window 2, then the extras) */
uint8_t *rg_src(uint32_t id);                /* the bytes of that page at the newest capture (NULL = zero page, (uint8_t *)1 = decommitted) */
long long rg_current_slot_pages(void);       /* pages the capture in progress has recorded so far (0 when none is in progress) */

/* ---- what rb_learned.c provides ---------------------------------------------------------------------------------------- */
extern int g_ls_active;                      /* 1 from a base until the fight is over: pages are protected and the learned set is being built */
extern volatile LONG64 g_read_retry;         /* nnfs.c read_at: reads repeated after ERROR_NOACCESS */

int ls_use_exe_maps(void);                   /* learned set on: use the exe's own mark maps and return 1; otherwise 0 */
void ls_start_after_base(void);              /* the base was just saved (threads held): protect the windows and start learning */
void ls_for_each_learned_page(void (*fn)(uint32_t id, int w, size_t j));      /* fn(page id, in window 2, index in the window) for each learned page */
void ls_capture_pages(void (*fn)(uint32_t id, int w, size_t j));              /* the same, but only the pages that changed since the newest capture */
void ls_after_capture(void);                 /* after a capture's copies (threads held): drop pages that stopped changing, if SSBU_LS_DECAY is set */
void ls_new_base(void);                      /* a new base inside a fight: protect everything again with an empty set */
void ls_fight_over(void);                    /* the fight ended: make every page writable again */
void ls_range_event(uint64_t a, uint64_t n, int what);      /* memory changed without a page fault: 0 host write, 1 decommit, 2 commit */
void ls_report(void);                        /* end-of-run report of the learned set and the pool */
uint8_t *ppool_bookkeeping(size_t *n);       /* the presentation pool's bookkeeping region, so it is saved with the rest; *n is its size */
extern volatile LONG g_rb_audio_off;         /* hostrt.c: 1 = the audio threads are stopped (SSBU_RB_AUDIO_OFF=1, for the length of a fight) */
int rb_norollback_asked(void);              /* memory_map.c: SSBU_RB_NET_NOROLLBACK=1, play the match delay-based, without rollback */
void rg_roundtrip_after_load(void);          /* debug/ring_audit.c (SSBU_RB_ROUNDTRIP=<n>): after a load, every page must be as recorded at that frame */
void osobj_capture(long long frame);        /* model/os_objects_state.c (SSBU_STATE_OBJS=1): record the events and semaphores at a capture */
void osobj_load(long long frame);           /* ... and put them back at the load of that capture */
int os_r1_obj_set(uint64_t key, int signaled, int count);      /* os_threads.c: set one object's signalled flag / count, wake its waiters */
/* controller/fault_recovery.c: the safety net (absorb a fault in a leaf helper, reload the last healthy state at the end of the pass) */
int rb_fault_pending(void); int rb_fault_recover(long long i, Ctx *c, long long *best_out); void rb_fault_pass_clean(long long i); void rb_fault_give_up(void); void rb_fault_report(void); void rb_fault_test_tick(void);
void nnsdk_tls_save(uint64_t *out); void nnsdk_tls_load(const uint64_t *in);      /* nnsdk.c */
void call_trace_poll(void);                 /* debug/call_trace.c: retries the SSBU_TRACE_BADLR hook until its module is loaded (called each pass) */
void call_trace_init(void);                 /* debug/call_trace.c (SSBU_TRACE_CALL=<function>,<call site>): log the calls of one guest function from one call site */
void sm_capture(long long frame);            /* debug/state_measure.c (SSBU_STATE_MEASURE=1): fingerprints of threads and objects at a capture */
void sm_load(long long frame);               /* ... and the comparison at a load, before anything is restored */
void sm_report(int final);                   /* ... the tables, every 100 loads and at the end */
void ppool_init(void);
void rg_audit(void);                         /* debug/ring_audit.c: after a capture, lists the pages that differ from the ring's newest value (SSBU_RB_AUDIT=<n>) */
void rg_pagedump(long long frame);           /* debug/ring_audit.c: every saved page's hash after a capture, to a file (SSBU_RB_PAGEDUMP=<n>) */
/* The extras' learned set (page_tracker_learned.c, "LS-X"): the ring asks which extras pages need comparing, and tells the set how each compare ended. */
extern long long g_xls_caps, g_xls_jobs, g_xls_loads, g_xls_loadpages;      /* statistics: captures, pages compared, loads, pages loaded */
int xls_skip(size_t e);                      /* 1 when extras page e was not written since the base, so it is neither compared nor loaded */
void xls_decay(const uint32_t *xjob, const uint8_t *xdiff, size_t nxj);      /* after a capture's compare: pages that stopped changing leave the set */
void xww_poll(void);                         /* ask the OS which extras pages were written since the last poll (SSBU_LS_X_WW=1) */
extern volatile LONG64 g_cp_fault;           /* page jobs skipped because the page was gone (write_marks.c) */
extern volatile int g_cp_restore; extern volatile LONG64 g_cp_recommitted;   /* write_marks.c: 1 while a restore copies pages back; the pages it committed again */
extern int (*volatile cp_f_cmp)(uint8_t *, const uint8_t *);      /* compare one page through a pointer so a fault is caught by __try (write_marks.c) */                       /* sets up the presentation pool (once, from r1_init) */


/* ---- includes the pieces need */
#include <tlhelp32.h>

/* ---- macros that used to sit in the middle of r1_state.c / r1_ring.c / rb_rollback.c */
#define FIB_BLOCK 0x500
#define R1_CTR(w, a, b) do { if (g_r1_ctr < 0) g_r1_ctr = getenv("SSBU_UNWIND_TRACE") != NULL; if (g_r1_ctr) nnunwind_trace_note(w, (uint64_t)(a), (uint64_t)(b)); } while (0)
#define AR_SIZE (32ull << 30)
#define ARH ((ArHdr *)g_ar)
#define RA0 (c->x[0])
#define RA1 (c->x[1])
#define RA2 (c->x[2])
#define RG_MAXSLOT 16
#define RG_FLAT 0u
#define RG_DECOM 1u
#define RG_POOL_MAX (1u << 20)                 /* 4 GB of 4 KB buffers (reserved, committed as used) */
#define RG_ENT_RES (64ull << 20)               /* per slot: room for 8 M (id, old) pairs */
#define RG_FP_RES (256ull << 20)               /* per slot: fibers' host stacks */
#define RG_CB_RES (64ull << 20)                /* per slot: command-buffer recordings */
#define RG_OP_RES (512ull << 20)               /* per slot: the pages committed outside the ring during the fight (snapshot_ring.c rg_opg_*) */
#define RG_GS_RES (64ull << 20)                /* per slot: the live part of every fiber's GUEST stack (snapshot_ring.c rg_gstk_*) */
#define RG_CBS_SIZE 144                        /* nvn_gl.c CbSave: CmdBuf *cb; CmdBuf copy (120: data, len, cap, ...); data; cap */
#define RG_NST 65536
#define RG_NL 4096
#define RGPG_N 16384
#define RGL_N ((int)(sizeof g_rgl_h / sizeof g_rgl_h[0]))
#define RGP_N 4096
#define RGP_NX 4
#define RGS_SN 65536
#define RGV_MAX (8u << 20)
#define RGVD_MAX 8192
#define RGV_NB 4096
#define RBG_NT 200000

/* ---- types shared by the pieces */
typedef struct { uint8_t *a; uint8_t d[PG]; } EPage;

typedef struct { uint64_t b; int lx, ly, rx, ry; } PadRec;

typedef struct { uint64_t key; int sig, cnt; uint64_t len; } OsRec;

typedef struct { HANDLE h; Ctx *c; const char *name; uint64_t from; const char *imp; Ctx regs; int held, parked, same; double wait_ms; uint64_t rsp, rip; int cok; } Th;

typedef struct { uint64_t key; uint64_t obj[6]; void *fls; uint8_t flsb[392]; Ctx *c; void *fiber; Ctx regs; const char *imp; uint64_t from, sp; int susp; uint8_t blk[FIB_BLOCK]; uint8_t *stk; size_t n; uint64_t rsp; int rok; } Fib;      /* rok: put back by the last fibers_restore_all */

/* The other guest threads while memory is copied (threads.c). The ring holds each at a wait (parks it), copies, and lets it go. */
typedef struct {
    Th table[256];                           /* one entry per other guest thread: its handle, context, the wait it is held at */
    int count;                               /* entries of table[] in use */
    int info_count;                          /* guest threads the host reports (guest_thread_info) at the last count */
    int mismatch;                            /* threads found not parked when checked (counted for the log) */
    int running;                             /* (ra_hold_restore) threads still RUNNING, not blocked at a wait, after park_ms */
    int park_ms;                             /* how long to wait for the other threads to stop at a wait (default 500) */
    long long audio_other;                   /* ring loads that took an audio thread at another wait (ra_hold_restore) */
} GuestThreads;
extern GuestThreads g_guest_threads;

/* The job system's fibers (threads.c): a switched-away fiber keeps host state outside guest memory, so it is saved apart. */
typedef struct {
    Fib table[1024];                         /* the fibers saved at the last save */
    int count;                               /* entries of table[] in use */
    int running;                             /* fibers some thread was running at the save */
    int unreadable_stack;                    /* fibers no thread was running whose host stack could not be read (not saved) */
    int restored;                            /* at a restore: fibers found as saved and put back */
    int not_restored;                        /* at a restore: fibers NOT put back (changed, or their stack was in use) */
    int replaced;                            /* at a restore: fibers that were replaced since the save */
    uint8_t *pool;                           /* memory the saved host stacks are copied into ... */
    size_t pool_used, pool_cap;              /* ... bytes used and bytes reserved (default 256 MB) */
    size_t bytes;                            /* bytes of fiber state saved */
    long long live_seen, live_skipped;       /* fibers whose host stack a running thread was on: seen / not copied back */
    long long teb_running;                   /* fibers taken as running at a capture because a thread's TEB names them (run tid said not) */
    int live_count;                          /* entries of live_rsp[] in use */
    uint64_t live_rsp[512];                  /* stack pointers of every guest thread and of this one, read before a restore */
    int live_cache;                          /* -1 not decided, 1 reuse the registers read while held, 0 ask Windows again (SSBU_R1_LIVECACHE=0) */
    long long live_cached, live_asked;       /* how many registers came from the cache / were asked of Windows */
} GuestFibers;
extern GuestFibers g_guest_fibers;

/* The R1 test session (game_state.c): SSBU_R1=<start_frame>,<run_frames>[,<period>,<cycles>] saves the state at a fight frame, lets the fight
 * run, restores it and runs the same frames again. SSBU_RB_SIM and run-ahead reuse the same machinery. */
typedef struct {
    int enabled;                             /* -1 not decided yet, 0 off, 1 on (r1_on) */
    int start_frame;                         /* N: the fight frame at which the state is saved */
    int run_frames;                          /* K: frames run before the restore (default 8) */
    int period;                              /* P: frames between two cycles (default 2 * K + 2) */
    int cycles;                              /* C: how many save / run / restore cycles (default 1) */
    int cycle;                               /* the cycle in progress */
    int phase;                               /* 0 idle, 1 first pass, 2 re-run, 3 done */
    long long fight_frame;                   /* fight frames counted so far */
    long long target_frame;                  /* the fight frame of the next save (start_frame at first) */
} R1Session;
extern R1Session g_session;

typedef struct { uint8_t *a; size_t n; uint8_t *learn; } Learn;

typedef struct { uint8_t *a; size_t n; uint8_t *b; } Extra;

typedef struct { LONG st; void *teb; void *pid, *tid; ULONG_PTR mask; LONG pri, bpri; } HsTbi;

typedef struct { uint8_t *d; const uint8_t *s; } CopyJob;

typedef struct { uint64_t rip, caller; char mod[48]; const char *thr; uint64_t gfn; int n; } Site;

typedef struct {
    long long frame;
    uint32_t *ent; size_t nent, entcom;
    Ctx ctx;
    Th th[256]; int nth, ninfo;
    Fib *fib; int nfib; uint8_t *fpool; size_t fpool_used, fpool_com;
    int ncbs; uint8_t *cbs; uint8_t *cbd; size_t cbd_used, cbd_com;
    size_t pages, xpages;
    uint64_t tls[64];                                   /* nn::os thread-local storage slots (nnsdk.c: one host table), as at the capture */
    int quiet, qset;                                    /* every worker at a usual pause when it was taken (snapshot_ring.c rg_best_state) */
    uint8_t *gstk; size_t gstk_used, gstk_com;
    uint8_t *opg; size_t opg_used, opg_com;             /* the pages committed outside the ring, as at the capture */          /* the fibers' guest stacks, [sp, top) each, as at the capture */
} RgSlot;

typedef struct { uint64_t blk; long long n[3]; } RgPageCount;

typedef struct { uint64_t cfa; uint64_t jb; } RgCatch;

typedef struct { uint64_t a; int grp; uint64_t ret; ImportFn orig; long long n[2], sk; double ms[2]; } RgsLite;

typedef struct { uint64_t key; long long n[2]; double incl[2], excl[2]; int dmin; } RgpE;

typedef struct { uint64_t fn; long long self[2][6], incl[2][6]; } RgsS;

typedef struct { uint64_t bump, committed; uint64_t head[48]; uint64_t allocs, frees; } ArHdr;


/* ---- variables one piece defines and another uses (the piece that defines each is named) */
extern long long g_r1_fls_stale;    /* model/threads.c */
extern void (*g_r1_fiber_born)(void *);    /* model/tracking_experiments/write_marks.c */
extern Ctx g_ctx_save;    /* model/game_state.c */
extern int g_nborn;    /* model/game_state.c */
extern char g_born[2048];    /* model/game_state.c */
extern uint64_t g_catch_addr;    /* model/game_state.c */
extern int g_fcheck;    /* model/game_state.c */
extern int g_fast;    /* model/game_state.c */
extern int g_learn;    /* model/game_state.c */
extern size_t g_ne;    /* model/game_state.c */
extern size_t g_ecap;    /* model/game_state.c */
extern EPage *g_e;    /* model/game_state.c */
extern uint8_t *g_ar;    /* model/memory_map.c */
extern int g_rs_drop;    /* model/memory_map.c */
extern double g_save_avail_mb;    /* model/memory_map.c */
extern double g_save_need_mb;    /* model/memory_map.c */
extern OsRec *g_os;    /* model/threads.c */
extern int g_nos;    /* model/threads.c */
extern int g_r1_ctr;    /* model/threads.c */
extern int g_busy_log;    /* model/threads.c */
extern int g_wait_idle;    /* model/threads.c */
extern int g_mark;    /* model/full_save.c */
extern int g_ra;    /* model/full_save.c */
extern int g_save_failed;    /* model/full_save.c */
extern size_t g_ra_cb_bytes;    /* model/full_save.c */
extern size_t g_learn_pages;    /* model/full_save.c */
extern int g_nl;    /* model/full_save.c */
extern Learn g_l[8192];    /* model/full_save.c */
extern size_t g_async_cap;    /* model/tracking_experiments/fast_save.c */
extern uint8_t **g_async;    /* model/tracking_experiments/fast_save.c */
extern size_t g_nfp;    /* model/tracking_experiments/fast_save.c */
extern int g_prot;    /* model/tracking_experiments/write_marks.c */
extern int g_mcheck;    /* model/tracking_experiments/write_marks.c */
extern int g_prot_on;    /* model/tracking_experiments/write_marks.c */
extern Ctx g_mctx;    /* model/tracking_experiments/write_marks.c */
extern uint8_t *g_sh1;    /* model/tracking_experiments/write_marks.c */
extern uint8_t *g_sh2c;    /* model/tracking_experiments/write_marks.c */
extern uint8_t *g_sh2;    /* model/tracking_experiments/write_marks.c */
extern uint8_t *g_sh1c;    /* model/tracking_experiments/write_marks.c */
extern size_t g_xbytes;    /* model/tracking_experiments/write_marks.c */
extern Extra g_x[8192];    /* model/tracking_experiments/write_marks.c */
extern int g_nx;    /* model/tracking_experiments/write_marks.c */
extern size_t g_pg_r[4096];    /* model/tracking_experiments/write_marks.c */
extern size_t g_pg_s[4096];    /* model/tracking_experiments/write_marks.c */
extern double g_ms_s[4096];    /* model/tracking_experiments/write_marks.c */
extern int g_nmr;    /* model/tracking_experiments/write_marks.c */
extern int g_nms;    /* model/tracking_experiments/write_marks.c */
extern double g_ms_scan[4096];    /* model/tracking_experiments/write_marks.c */
extern double g_ms_r[4096];    /* model/tracking_experiments/write_marks.c */
extern long long g_miss_remarked;    /* model/tracking_experiments/write_marks.c */
extern int g_miss_cycles;    /* model/tracking_experiments/write_marks.c */
extern long long g_miss_total;    /* model/tracking_experiments/write_marks.c */
extern long long g_hs_pages;    /* model/tracking_experiments/write_marks.c */
extern long long g_hs_ranges;    /* model/tracking_experiments/write_marks.c */
extern long long g_hs_fibers_later;    /* model/tracking_experiments/write_marks.c */
extern uint64_t *g_bf_hi;    /* model/tracking_experiments/write_marks.c */
extern uint64_t *g_bf_lo;    /* model/tracking_experiments/write_marks.c */
extern SRWLOCK g_bf_lock;    /* model/tracking_experiments/write_marks.c */
extern size_t g_nbf;    /* model/tracking_experiments/write_marks.c */
extern size_t g_blk2_n;    /* model/tracking_experiments/write_marks.c */
extern size_t g_blk1_n;    /* model/tracking_experiments/write_marks.c */
extern volatile LONG g_blk1_dirty;    /* model/tracking_experiments/write_marks.c */
extern uint32_t *g_blk1_list;    /* model/tracking_experiments/write_marks.c */
extern uint32_t *g_blk2_list;    /* model/tracking_experiments/write_marks.c */
extern int g_full_sweep;    /* model/tracking_experiments/write_marks.c */
extern uint8_t *g_blk1;    /* model/tracking_experiments/write_marks.c */
extern int g_cp_cond;    /* model/tracking_experiments/write_marks.c */
extern volatile long long g_cp_wrote[9];    /* model/tracking_experiments/write_marks.c */
extern int g_cp_defer;    /* model/tracking_experiments/write_marks.c */
extern int g_ra_after_quiet;    /* model/runahead.c */
extern int g_ra_ready;    /* model/runahead.c */
extern int g_ra_phase;    /* model/runahead.c */
extern int g_ra_off;    /* model/runahead.c */
extern double g_rap[3][4];    /* model/runahead.c */
extern long long g_rapn[3];    /* model/runahead.c */
extern double g_t_poll;    /* model/runahead.c */
extern volatile int g_ra_wd_pause;    /* model/runahead.c */
extern volatile int g_ra_wd_base;    /* model/runahead.c */
extern const char *g_ra_busy;    /* model/runahead.c */
extern double g_ra_t_mark;    /* model/runahead.c */
extern PadRec g_ra_pad;    /* model/runahead.c */
extern int g_hp_on;    /* model/runahead.c */
extern int g_ra_busy_fibers;    /* model/runahead.c */
extern volatile LONG g_ra_wh_on;    /* model/runahead.c */
extern long long g_ra_wd_fired;    /* model/runahead.c */
extern int g_rg_nomem;    /* model/snapshot_ring.c */
extern int g_rg_count;    /* model/snapshot_ring.c */
extern int g_rg_ready;    /* model/snapshot_ring.c */
extern uint32_t *g_rg_idx;    /* model/snapshot_ring.c */
extern size_t g_rg_n1;    /* model/snapshot_ring.c */
extern size_t g_rg_nx;    /* model/snapshot_ring.c */
extern size_t g_rg_n2;    /* model/snapshot_ring.c */
extern uint32_t g_rg_bump;    /* model/snapshot_ring.c */
extern uint32_t g_rg_nfree;    /* model/snapshot_ring.c */
extern int g_rg_keep;    /* model/snapshot_ring.c */
extern int g_rg_keep_on;    /* model/snapshot_ring.c */
extern long long g_rg_kept_same;    /* model/snapshot_ring.c */
extern long long g_rg_kept_n;    /* model/snapshot_ring.c */
extern long long g_rg_loads;    /* model/snapshot_ring.c */
extern long long g_rg_overflow;    /* model/snapshot_ring.c */
extern long long g_rg_cap_fail;    /* model/snapshot_ring.c */
extern long long g_rg_caps;    /* model/snapshot_ring.c */
extern double g_rg_last[2];    /* model/snapshot_ring.c */
extern long long g_rgv_target;    /* model/snapshot_ring.c */
extern int g_rgs_on;    /* model/snapshot_ring.c */
extern size_t *g_rgc_pg;    /* model/snapshot_ring.c */
extern int g_rgc_n;    /* model/snapshot_ring.c */
extern double * g_rgc[5];    /* model/snapshot_ring.c */
extern size_t *g_rgc_xp;    /* model/snapshot_ring.c */
extern double g_rgl[16][4096][3];    /* model/snapshot_ring.c */
extern int g_rgl_n[16];    /* model/snapshot_ring.c */
extern size_t g_rgl_pg[16][4096];    /* model/snapshot_ring.c */
extern long long g_rg_skip_full;    /* model/snapshot_ring.c */
extern long long g_rg_freed_slots;    /* model/snapshot_ring.c */
extern int g_rg_poolfix;    /* model/snapshot_ring.c */
extern long long g_rg_need;    /* model/snapshot_ring.c */
extern int g_rg_short;    /* model/snapshot_ring.c */
extern long long g_rg_loading_until;    /* model/snapshot_ring.c */
extern long long g_rg_short_n;    /* model/snapshot_ring.c */
extern long long g_rg_tp_refused;    /* model/snapshot_ring.c */
extern int g_rgs_left;    /* model/snapshot_ring.c */
extern long long g_rgpg_caps[3];    /* model/snapshot_ring.c */
extern int g_rgpg_on;    /* model/snapshot_ring.c */
extern RgPageCount g_rgpg[16384];    /* model/snapshot_ring.c */
extern long long g_rgc_dropped;    /* model/snapshot_ring.c */
extern long long g_rgc_loads_changed;    /* model/snapshot_ring.c */
extern long long g_rgc_kept;    /* model/snapshot_ring.c */
extern long long g_rgc_dead;    /* model/snapshot_ring.c */
extern long long g_rg_refused;    /* model/snapshot_ring.c */
extern long long g_rg_ldpg;    /* model/snapshot_ring.c */
extern int g_rgs_lastlite;    /* debug/ring_simulation.c */
extern int g_rgs_warm;    /* debug/ring_simulation.c */
extern int g_rgs_skip;    /* debug/ring_simulation.c */
extern int g_rgs_k;    /* debug/ring_simulation.c */
extern int g_rgs_n;    /* debug/ring_simulation.c */
extern long long g_rgs_next;    /* debug/ring_simulation.c */
extern int g_rgs_lite;    /* debug/ring_simulation.c */
extern volatile int g_rgp_seg;    /* debug/ring_simulation.c */

/* ---- functions one piece defines and another calls */
void os_r1_objs(void (*cb)(uint64_t key, int signaled, int count, uint64_t len, uint64_t thread, uint64_t stack, uint64_t stack_size));    /* model/game_state.c */
void os_r1_fibers(void (*cb)(uint64_t key, Ctx *c, uint64_t stack, uint64_t stack_size, void *fiber));    /* model/threads.c */
DWORD os_r1_fiber_tid(uint64_t key);    /* model/threads.c */
void nnunwind_trace_note(const char *what, uint64_t cfa, uint64_t jb);    /* model/threads.c */
size_t nvn_r1_cb_save(void);    /* model/full_save.c */
void nvn_r1_cb_restore(void);    /* model/full_save.c */
int rbn_on(void);    /* controller/rollback_controller.c */
void rbn_init(void);    /* controller/rollback_controller.c */
int ra_wanted(void);    /* model/game_state.c */
int r1_on(void);    /* model/game_state.c */
int rw(DWORD p);    /* model/memory_map.c */
extern int g_rs_direct;    /* model/memory_map.c: the ring base: no copy of the save of its own, mark_setup copies the live memory into the shadow (one copy, not two) */
void *rs_alloc(size_t n);    /* model/memory_map.c */
void rs_free(void *p);    /* model/memory_map.c */
int rb_test_nomem(void);    /* model/memory_map.c */
void collect(Ctx *c);    /* model/memory_map.c */
void os_cb(uint64_t key, int sig, int cnt, uint64_t len, uint64_t thread, uint64_t stack, uint64_t size);    /* model/threads.c */
int cmp_os(const void *x, const void *y);    /* model/threads.c */
void os_compare(const char *when);    /* model/threads.c */
int waiting_import(const char *s);    /* model/threads.c */
void park(int restore_mode);    /* model/threads.c */
void slow_report(const char *when);    /* model/threads.c */
void unpark(void);    /* model/threads.c */
void restore_thread_regs(void);    /* model/threads.c */
void park_report(const char *when);    /* model/threads.c */
uint64_t map_sym(const char *want);    /* model/threads.c */
void fls_index_find(void);    /* model/threads.c */
int rest_stack(uint64_t a);    /* model/threads.c */
void fibers_restore_all(void);    /* model/threads.c */
int fibers_idle(void);    /* model/threads.c */
void park_quiet(int restore_mode);    /* model/threads.c */
void fibers_save(void);    /* model/threads.c */
void fibers_restore(void);    /* model/threads.c */
void fibers_report(void);    /* model/threads.c */
void save(Ctx *c);    /* model/full_save.c */
const char *where(uint8_t *a);    /* model/full_save.c */
void stop_born_threads(void);    /* model/full_save.c */
void restore(Ctx *c);    /* model/full_save.c */
void verify(void);    /* model/full_save.c */
void release(void);    /* model/full_save.c */
void async_add(uint8_t *a);    /* model/tracking_experiments/fast_save.c */
void async_learn(void);    /* model/tracking_experiments/fast_save.c */
void fast_build(void);    /* model/tracking_experiments/fast_save.c */
void fast_save(Ctx *c);    /* model/tracking_experiments/fast_save.c */
void fast_restore(Ctx *c);    /* model/tracking_experiments/fast_save.c */
int cmp_d(const void *a, const void *b);    /* model/tracking_experiments/fast_save.c */
void fast_report(void);    /* model/tracking_experiments/fast_save.c */
int mark_bind(void);    /* model/tracking_experiments/write_marks.c */
uint8_t *in_shadow(uint8_t *a, uint8_t **cmap, size_t *pi);    /* model/tracking_experiments/write_marks.c */
int sh_commit(uint8_t *sh, uint8_t *cmap, size_t pi);    /* model/tracking_experiments/write_marks.c */
void r1_host_stack_note(uint64_t lo, uint64_t hi);    /* model/tracking_experiments/write_marks.c */
void hs_exclude_all(void);    /* model/tracking_experiments/write_marks.c */
int mark_setup(void);    /* model/tracking_experiments/write_marks.c */
void blk1_add(size_t p0, size_t p1);    /* model/tracking_experiments/write_marks.c */
void blk1_rebuild(void);    /* model/tracking_experiments/write_marks.c */
void blk1_init(void);    /* model/tracking_experiments/write_marks.c */
void cp_add_range(uint8_t *d, const uint8_t *s, size_t n);    /* model/tracking_experiments/write_marks.c */
void cp_init(void);    /* model/tracking_experiments/write_marks.c */
size_t mark_sweep(int dir);    /* model/tracking_experiments/write_marks.c */
void mark_save(Ctx *c);    /* model/tracking_experiments/write_marks.c */
void mark_restore(Ctx *c);    /* model/tracking_experiments/write_marks.c */
int win_index(uint8_t *a, uint8_t **map, size_t *pi);    /* model/tracking_experiments/write_protection.c */
LONG CALLBACK prot_veh(EXCEPTION_POINTERS *e);    /* model/tracking_experiments/write_protection.c */
void prot_all(int on);    /* model/tracking_experiments/write_protection.c */
void prot_open(uint64_t a, uint64_t n);    /* model/tracking_experiments/write_protection.c */
void prot_report(void);    /* model/tracking_experiments/write_protection.c */
void mark_report(void);    /* model/tracking_experiments/write_protection.c */
LONG CALLBACK r1_veh(EXCEPTION_POINTERS *e);    /* model/tracking_experiments/write_protection.c */
const char *g_ra_busy_first_save(void);    /* model/runahead.c */
int ra_fight_on(void);    /* model/runahead.c */
void r1_hold_prof_report(void);    /* model/runahead.c */
void ra_hold_restore(void);    /* model/runahead.c */
int ra_fibers_quiet(void);    /* model/runahead.c */
int ra_hold_quiet(double limit);    /* model/runahead.c */
void ra_frame_end(double te);    /* model/runahead.c */
void ra_init(void);    /* model/runahead.c */
int is_nosim(uint8_t *a);    /* model/guest_heap.c */
int rbg_noring_now(void);    /* controller/rollback_controller.c */
int rbg_on(void);    /* controller/rollback_controller.c */
void rbg_step(Ctx *c);    /* controller/rollback_controller.c */
void rbg_pad(int id, uint64_t *b, int *lx, int *ly, int *rx, int *ry);    /* controller/rollback_controller.c */
void rbg_fight_off(void);    /* controller/rollback_controller.c */
void rbg_report(void);    /* controller/rollback_controller.c */
RgSlot *rg_slot(int i);
long long rg_best_state(long long target); void rg_quiet_report(void);    /* model/snapshot_ring.c: the state a rollback loads (the newest quiet one) */    /* model/snapshot_ring.c */
void r1_ring_let_go(void);    /* model/snapshot_ring.c */
int r1_ring_hold(void);    /* model/snapshot_ring.c */
long long r1_ring_oldest(void);    /* model/snapshot_ring.c */
void r1_ring_release(void);    /* model/snapshot_ring.c */
void rg_meta_prof_report(void);    /* model/snapshot_ring.c */
int r1_ring_capture(long long frame, Ctx *c);    /* model/snapshot_ring.c */
int r1_ring_rebase(long long frame, Ctx *c);    /* model/snapshot_ring.c */
int r1_ring_load(long long frame, Ctx *c);    /* model/snapshot_ring.c */
ULONG64 rgs_cyc_now(void);    /* debug/ring_simulation.c */
void rgs_step(Ctx *c);    /* debug/ring_simulation.c */
void rgp_wait_note(uint64_t lr, double dt);    /* debug/ring_simulation.c */
void rgs_lite_init(void);    /* debug/ring_simulation.c */
void rgv_before(int si, long long target);    /* debug/ring_simulation.c */
int rgs_skip_now(void);    /* debug/ring_simulation.c */
int rgs_poll(void);    /* debug/ring_simulation.c */
void rgs_pad(uint64_t *b, int *lx, int *ly, int *rx, int *ry);    /* debug/ring_simulation.c */
void rgs_report(void);    /* debug/ring_simulation.c */

#endif
