/* OLDER EXPERIMENT, round 3 (SSBU_R1_MARK=1): the exact written set, from store marks made by a marked game DLL.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "../game_state_internal.h"

/* ---------------------------------------------------------------- R1 round 3: the exact written set (SSBU_R1_MARK=1)
 * With ssbu_game_p15r1.dll (built with -DR1_MARK) every guest store of the lifted code marks its page in a byte map
 * (runtime.h R1M): window 1 = the game's address region, window 2 = 0x7100000000.. (image + modules). The first save is a
 * full one; it also fills a SHADOW (a reservation as large as each window, committed where needed) with the memory. From
 * then on a save copies only the pages marked since the last save / restore into the shadow, a restore copies the pages
 * marked since the save back from it. Memory outside the windows (main's guest stack, TLS blocks, fighter DLL data, the
 * catch table) is copied whole each time ("extras"). SSBU_R1_MARK_CHECK=1: each save is also a full one and after the
 * restore every page that still differs from the full snapshot is counted (a writer that does not mark) and put back. */
int g_mark = -1, g_mcheck, g_prot_on, g_prot = -1;
 
Ctx g_mctx;
uint8_t **pm1, **pm2; uint64_t *plo1, *pn1, *pn2;
uint8_t *g_win1; uint8_t *g_sh1, *g_sh1c, *g_sh2, *g_sh2c; uint64_t g_wn1;
uint8_t *g_dc;
uint8_t *g_ns1, *g_ns2; 

Extra g_x[8192]; int g_nx; size_t g_xbytes;
double g_ms_s[4096], g_ms_r[4096], g_ms_scan[4096]; size_t g_pg_s[4096], g_pg_r[4096]; int g_nms, g_nmr;
long long g_miss_total, g_miss_remarked, g_miss_remarked_last; int g_miss_cycles;

int mark_bind(void) {
    const char *dn = getenv("SSBU_R1_GAMEDLL");
    HMODULE m = GetModuleHandleA(dn && dn[0] ? dn : "ssbu_game_r1.dll");          /* the players' rollback pair (scripts/setup.py --rollback) */
    if (!m && !(dn && dn[0])) m = GetModuleHandleA("ssbu_game_p15r1.dll");      /* the study builds' name */
    if (!m) { LOG("r1: MARK: the game DLL with marks is not loaded: off\n"); return 0; }
    pm1 = (uint8_t **)GetProcAddress(m, "r1_m1"); pm2 = (uint8_t **)GetProcAddress(m, "r1_m2");
    plo1 = (uint64_t *)GetProcAddress(m, "r1_lo1"); pn1 = (uint64_t *)GetProcAddress(m, "r1_n1"); pn2 = (uint64_t *)GetProcAddress(m, "r1_n2");
    if (!pm1 || !pm2 || !plo1 || !pn1 || !pn2) { LOG("r1: MARK: the DLL has no r1_m1.. exports: off\n"); return 0; }
    return 1;
}
uint8_t *in_shadow(uint8_t *a, uint8_t **cmap, size_t *pi) {
    if (g_win1 && a >= g_win1 && a < g_win1 + g_wn1) { *pi = (size_t)(a - g_win1) >> 12; *cmap = g_sh1c; return g_sh1 + (a - g_win1); }
    if ((uintptr_t)a >= WIN2 && (uintptr_t)a < WIN2 + WN2) { *pi = (size_t)((uintptr_t)a - WIN2) >> 12; *cmap = g_sh2c; return g_sh2 + ((uintptr_t)a - WIN2); }
    return NULL;
}
static long long g_sh_fail;                             /* (hs13) shadow commits that failed */
int sh_commit(uint8_t *sh, uint8_t *cmap, size_t pi) {
    if (cmap[pi]) return 1;
    size_t c0 = pi & ~(size_t)15;                         /* 64 KB at a time */
    if (rb_test_nomem() == 4 || !VirtualAlloc(sh - (pi - c0) * PG, 16 * PG, MEM_COMMIT, PAGE_READWRITE)) { g_sh_fail++; return 0; }
    memset(cmap + c0, 1, 16);
    return 1;
}
/* R3 (hs13 crash, 2026-10-20): window 2 (0x7100000000 + 8 GB) is the guest image + the script modules, but the OS may put HOST
 * memory in its free parts: in hs13 the main thread's own Windows stack was at 0x72659f4000. Marked by instrumented code (a host
 * local written through a pointer), its page went into the ring and a load copied the old page back over the live stack while
 * main waited for the copy threads (cp_run): the return address read 0 -> c0000005 at rip 0 on main. Every host thread stack and
 * every host fiber stack inside a window is "not simulation" (g_ns1 / g_ns2): never captured, loaded, restored or checked.
 * SSBU_R1_HOST_STACKS=0: the old way (test only). */
long long g_hs_pages, g_hs_ranges, g_hs_fibers_later;
static int g_hs_on = -1;
static void hs_exclude(uint64_t lo, uint64_t hi, const char *what) {
    if (!g_ns1 || !g_ns2 || hi <= lo || hi - lo > (1ull << 32)) return;
    long long n = 0;
    for (uint64_t p = lo & ~(uint64_t)(PG - 1); p < hi; p += PG) {
        if (g_win1 && p >= (uint64_t)(uintptr_t)g_win1 && p < (uint64_t)(uintptr_t)g_win1 + g_wn1) { size_t i = (size_t)(p - (uint64_t)(uintptr_t)g_win1) >> 12; if (!g_ns1[i]) { g_ns1[i] = 1; n++; } }
        else if (p >= WIN2 && p < WIN2 + WN2) { size_t i = (size_t)(p - WIN2) >> 12; if (!g_ns2[i]) { g_ns2[i] = 1; n++; } }
    }
    if (n) { g_hs_pages += n; g_hs_ranges++; LOG("r1: host stack inside a guest window left out of the state: %s %llx..%llx (%lld pages)\n", what, (unsigned long long)lo, (unsigned long long)hi, n); }
}
/* (hs15) every host fiber stack ever made (DeallocationStack .. StackBase): a catch record (nnunwind.c) on one that no guest
 * fiber uses any more after a ring load is dead (r1_ring.c rgc_after drops it instead of keeping it for ever) */
uint64_t *g_bf_lo, *g_bf_hi; size_t g_nbf, g_cbf; SRWLOCK g_bf_lock = SRWLOCK_INIT;
static void bf_add(void *fiber) {
    uint64_t lo = *(uint64_t *)((uint8_t *)fiber + 0x20), hi = *(uint64_t *)((uint8_t *)fiber + 0x10);
    if (!(lo < hi)) return;
    AcquireSRWLockExclusive(&g_bf_lock);
    size_t i; for (i = 0; i < g_nbf; i++) if (g_bf_lo[i] == lo) { g_bf_hi[i] = hi; break; }
    if (i == g_nbf) {
        if (g_nbf == g_cbf) { size_t nc = g_cbf ? g_cbf * 2 : 1024; uint64_t *a = realloc(g_bf_lo, nc * 8), *b = a ? realloc(g_bf_hi, nc * 8) : NULL;
                              if (a) g_bf_lo = a; if (b) { g_bf_hi = b; g_cbf = nc; } }
        if (g_nbf < g_cbf) { g_bf_lo[g_nbf] = lo; g_bf_hi[g_nbf] = hi; g_nbf++; }
    }
    ReleaseSRWLockExclusive(&g_bf_lock);
}
static void hs_fiber_cb(uint64_t key, Ctx *c, uint64_t stack, uint64_t size, void *fiber) {
    (void)key; (void)c; (void)stack; (void)size;
    if (fiber) bf_add(fiber);
    if (fiber) hs_exclude(*(uint64_t *)((uint8_t *)fiber + 0x20), *(uint64_t *)((uint8_t *)fiber + 0x10), "fiber stack");
}
extern void (*g_r1_fiber_born)(void *fiber);           /* os_threads.c: a host fiber made later (fib_init, a fresh one at a restore) */
static void hs_fiber_born(void *fiber) { if (fiber) { long long b = g_hs_pages; hs_fiber_cb(0, NULL, 0, 0, fiber); if (g_hs_pages != b) g_hs_fibers_later++; } }
static uint64_t g_hs_note[16][2]; static int g_hs_nnote;     /* host stacks this exe makes itself (not threads, not guest fibers): noted, left out at the setup */
void r1_host_stack_note(uint64_t lo, uint64_t hi) { if (g_hs_nnote < 16) { g_hs_note[g_hs_nnote][0] = lo; g_hs_note[g_hs_nnote][1] = hi; g_hs_nnote++; } if (g_ns2 && (g_hs_on < 0 || g_hs_on)) hs_exclude(lo, hi, "host stack (noted)"); }

void hs_exclude_all(void) {
    if (g_hs_on < 0) { const char *e = getenv("SSBU_R1_HOST_STACKS"); g_hs_on = !(e && e[0] == '0'); }
    if (!g_hs_on) return;
    typedef LONG (WINAPI *QitFn)(HANDLE, int, void *, ULONG, ULONG *);
    QitFn qit = (QitFn)(void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te; DWORD pid = GetCurrentProcessId(); int nth = 0;
    for (BOOL ok = qit && sn != INVALID_HANDLE_VALUE && Thread32First(sn, &te); ok; ok = Thread32Next(sn, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID); if (!h) continue;
        HsTbi tb; memset(&tb, 0, sizeof tb);
        if (qit(h, 0, &tb, sizeof tb, NULL) >= 0 && tb.teb) {
            uint8_t *teb = tb.teb; char nm[48]; snprintf(nm, sizeof nm, "thread %lu stack", (unsigned long)te.th32ThreadID);
            hs_exclude(*(uint64_t *)(teb + 0x1478), *(uint64_t *)(teb + 0x08), nm);    /* DeallocationStack .. StackBase (the whole reservation) */
            nth++;
        }
        CloseHandle(h);
    }
    if (sn != INVALID_HANDLE_VALUE) CloseHandle(sn);
    os_r1_fibers(hs_fiber_cb);
    for (int i = 0; i < g_hs_nnote; i++) hs_exclude(g_hs_note[i][0], g_hs_note[i][1], "host stack (noted)");
    g_r1_fiber_born = hs_fiber_born;
    {   /* what else lives in window 2 (a line per allocation that has committed read-write pages; diagnosis) */
        MEMORY_BASIC_INFORMATION m; uint8_t *cur = NULL; size_t com = 0; int shown = 0;
        for (uint8_t *p = (uint8_t *)(uintptr_t)WIN2; (uintptr_t)p < WIN2 + WN2 && VirtualQuery(p, &m, sizeof m); p = (uint8_t *)m.BaseAddress + m.RegionSize) {
            if (m.AllocationBase != cur) {
                if (cur && com && shown++ < 40) LOG("r1:   window 2 allocation %s: %.1f MB committed read-write\n", where(cur), com / 1048576.0);
                cur = m.AllocationBase; com = 0;
            }
            if (m.State == MEM_COMMIT && rw(m.Protect)) com += m.RegionSize;
        }
        if (cur && com && shown++ < 40) LOG("r1:   window 2 allocation %s: %.1f MB committed read-write\n", where(cur), com / 1048576.0);
    }
    LOG("r1: host stacks: %d threads and the host fibers looked at; %lld pages in %lld ranges inside the guest windows left out\n", nth, g_hs_pages, g_hs_ranges);
}
/* (hs13) a mark_setup that could not finish: everything it made is given back, no mark map is set (marks stay off) */
static void mark_undo(void) {
    if (g_sh1) VirtualFree(g_sh1, 0, MEM_RELEASE);
    if (g_sh2) VirtualFree(g_sh2, 0, MEM_RELEASE);
    free(g_sh1c); free(g_sh2c); free(g_dc); free(g_ns1); free(g_ns2);
    g_sh1 = g_sh2 = g_sh1c = g_sh2c = g_dc = g_ns1 = g_ns2 = NULL;
    for (int i = 0; i < g_nx; i++) { free(g_x[i].b); g_x[i].b = NULL; }
    g_nx = 0; g_xbytes = 0; g_win1 = NULL; g_wn1 = 0;
}
/* after the first (full) save: shadows, extras, maps on. 1 = done; 0 = out of memory (mark_undo done: no marks) */
int mark_setup(void) {
    size_t big = 0; uint8_t *base = NULL;
    for (int i = 0; i < g_memory_map.heap_count; i++) {
        MEMORY_BASIC_INFORMATION m; uint8_t *p = g_memory_map.heap_base[i];
        while (VirtualQuery(p, &m, sizeof m) && m.AllocationBase == g_memory_map.heap_base[i]) p = (uint8_t *)m.BaseAddress + m.RegionSize;
        if ((size_t)(p - g_memory_map.heap_base[i]) > big && g_memory_map.heap_base[i] != g_ar) { big = (size_t)(p - g_memory_map.heap_base[i]); base = g_memory_map.heap_base[i]; }
    }
    g_win1 = base; g_wn1 = big;
    g_mctx = g_ctx_save;                               /* main's registers of this (full) save */
    for (int i = 0; i < g_memory_map.heap_count; i++) LOG("r1: MARK: reservation >= 16 GB at %p%s\n", (void *)g_memory_map.heap_base[i], g_memory_map.heap_base[i] == base ? " (window 1)" : g_memory_map.heap_base[i] == g_ar ? " (malloc arena)" : " (NOT a window: its pages are extras)");
    g_sh1 = VirtualAlloc(NULL, g_wn1, MEM_RESERVE, PAGE_NOACCESS); g_sh1c = calloc(g_wn1 >> 12, 1);
    g_dc = calloc(g_wn1 >> 12, 1); g_ns1 = calloc(g_wn1 >> 12, 1); g_ns2 = calloc(WN2 >> 12, 1);
    g_sh2 = VirtualAlloc(NULL, WN2, MEM_RESERVE, PAGE_NOACCESS); g_sh2c = calloc(WN2 >> 12, 1);
    if (!g_sh1 || !g_sh1c || !g_dc || !g_ns1 || !g_ns2 || !g_sh2 || !g_sh2c) { LOG("r1: MARK setup: allocation of the shadow / its maps FAILED: marks OFF\n"); mark_undo(); return 0; }
    hs_exclude_all();
    g_nx = 0; g_xbytes = 0;
    size_t shb = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {                    /* the extras: the runs of pages outside both windows */
        Rng *r = &g_memory_map.ranges[i];
        for (size_t p = 0; p < r->n / PG; p++) {
            uint8_t *cm; size_t pi;
            if (in_shadow(r->a + p * PG, &cm, &pi)) continue;
            if (g_nx < 8192) {
                if (g_nx && g_x[g_nx - 1].a + g_x[g_nx - 1].n == r->a + p * PG) g_x[g_nx - 1].n += PG;
                else { g_x[g_nx].a = r->a + p * PG; g_x[g_nx].n = PG; g_x[g_nx].b = NULL; g_nx++; }
            }
        }
    }
    for (int i = 0; i < g_nx; i++) {
        g_x[i].b = malloc(g_x[i].n); g_xbytes += g_x[i].n;
        if (!g_x[i].b) { LOG("r1: MARK setup: allocation of %.1f KB for an extra FAILED: marks OFF\n", g_x[i].n / 1024.0); mark_undo(); return 0; }
    }
    {   /* the bytes of the save point into the shadow and the extras; with g_rs_drop the copy of the full save is given back 1 MB at a time behind */
        int cx = 0; size_t xo = 0; long long dropped = 0;
        for (int i = 0; i < g_memory_map.range_count; i++) {
            Rng *r = &g_memory_map.ranges[i]; size_t np = r->n / PG, d0 = 0;
            for (size_t p = 0; p < np; p++) {
                uint8_t *a = r->a + p * PG, *cm; size_t pi; uint8_t *sh = in_shadow(a, &cm, &pi);
                if (sh) {
                    if (!sh_commit(sh, cm, pi)) { LOG("r1: MARK setup: commit of the shadow FAILED after %.1f MB%s: marks OFF\n", shb / 1048576.0, rb_test_nomem() == 4 ? " (TEST: SSBU_RB_NET_TEST_NOMEM=4)" : ""); mark_undo(); return 0; }
                    if (r->s) memcpy(sh, r->s + p * PG, PG); else cp_add(sh, a); shb += PG;
                } else {
                    if (cx < g_nx && xo == g_x[cx].n) { cx++; xo = 0; }
                    if (cx < g_nx && g_x[cx].a + xo == a) { if (r->s) memcpy(g_x[cx].b + xo, r->s + p * PG, PG); else cp_add(g_x[cx].b + xo, a); xo += PG; }
                }
                if (g_rs_drop && r->s && (p + 1 - d0 == 256 || p + 1 == np)) { VirtualFree(r->s + d0 * PG, (p + 1 - d0) * PG, MEM_DECOMMIT); dropped += (long long)(p + 1 - d0); d0 = p + 1; }
            }
        }
        cp_run();                                       /* (direct base save) the live pages, copied by the pool's threads: the game is held */
        if (g_rs_drop) LOG("r1: MARK setup: the copy of the full save given back while the shadow took it (%.1f MB): peak commit ~one copy\n", dropped * (double)PG / 1048576.0);
    }
    uint8_t *m1 = VirtualAlloc(NULL, g_wn1 >> 12, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *m2 = VirtualAlloc(NULL, WN2 >> 12, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!m1 || !m2) { if (m1) VirtualFree(m1, 0, MEM_RELEASE); if (m2) VirtualFree(m2, 0, MEM_RELEASE); LOG("r1: MARK setup: allocation of the mark maps FAILED: marks OFF\n"); mark_undo(); return 0; }
    *pm1 = m1; *pm2 = m2; *plo1 = (uint64_t)(uintptr_t)g_win1; MemoryBarrier(); *pn2 = WN2; *pn1 = g_wn1;
    r1_m1 = m1; r1_m2 = m2; r1_lo1 = (uint64_t)(uintptr_t)g_win1; MemoryBarrier(); r1_n2 = WN2; r1_n1 = g_wn1;
    {   int nmod = 0; char names[1500]; int o = 0;
        HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        MODULEENTRY32 me; me.dwSize = sizeof me;
        for (BOOL ok = sn != INVALID_HANDLE_VALUE && Module32First(sn, &me); ok; ok = Module32Next(sn, &me)) {
            uint8_t **a1 = (uint8_t **)GetProcAddress(me.hModule, "r1_m1"), **a2 = (uint8_t **)GetProcAddress(me.hModule, "r1_m2");
            uint64_t *l1 = (uint64_t *)GetProcAddress(me.hModule, "r1_lo1"), *n1 = (uint64_t *)GetProcAddress(me.hModule, "r1_n1"), *n2 = (uint64_t *)GetProcAddress(me.hModule, "r1_n2");
            if (!a1 || !a2 || !l1 || !n1 || !n2) continue;
            *a1 = m1; *a2 = m2; *l1 = (uint64_t)(uintptr_t)g_win1; MemoryBarrier(); *n2 = WN2; *n1 = g_wn1;
            nmod++; if (o < 1400) o += snprintf(names + o, sizeof names - o, " %s", me.szModule);
        }
        if (sn != INVALID_HANDLE_VALUE) CloseHandle(sn);
        names[o] = 0;
        LOG("r1: MARK maps set in the exe and %d modules:%s\n", nmod, names);
    }
    blk1_init();
    for (int i = 0; i < g_nx; i++) if (g_x[i].n >= 65536) LOG("r1:   extra %s + %.0f KB\n", where(g_x[i].a), g_x[i].n / 1024.0);
    LOG("r1: MARK on: window 1 = region at %p, %.1f GB (map %.1f MB), window 2 = %llx + %.0f GB (map %.1f MB); shadow filled with %.1f MB; extras %d ranges %.1f MB copied whole each time\n",
        (void *)g_win1, g_wn1 / 1073741824.0, (g_wn1 >> 12) / 1048576.0, WIN2, WN2 / 1073741824.0, (WN2 >> 12) / 1048576.0, shb / 1048576.0, g_nx, g_xbytes / 1048576.0);
    ls_start_after_base();                                   /* R4: the learned-set save protects the windows (threads still held) */
    return 1;
}
/* every marked page: dir 0 memory -> shadow (save), 1 shadow -> memory (restore); marks cleared. Returns pages. */
/* Round 5 (cheaper sweep): window 1 is 62 GB (a 15.5 MB map) but the game commits only ~2 GB of it: the map is swept only
 * in the 64-page blocks that ever had committed pages (the first save's ranges + every commit since: nnsdk.c
 * r1_on_commit). SSBU_R1_FULL_SWEEP=1: the whole map as before. */
uint8_t *g_blk1; uint32_t *g_blk1_list, *g_blk2_list; size_t g_blk1_n, g_blk1_cap, g_blk2_n; volatile LONG g_blk1_dirty; int g_full_sweep = -1;
void blk1_add(size_t p0, size_t p1) {           /* pages [p0, p1) of window 1 */
    if (!g_blk1) return;
    for (size_t b = p0 >> 6; b <= (p1 - 1) >> 6 && b < (g_wn1 >> 18); b++) if (!g_blk1[b]) { g_blk1[b] = 1; InterlockedExchange(&g_blk1_dirty, 1); }
}
void blk1_rebuild(void) {
    size_t nb = (size_t)(g_wn1 >> 18), k = 0;
    for (size_t b = 0; b < nb; b++) k += g_blk1[b];
    if (k > g_blk1_cap) { g_blk1_cap = k + 4096; g_blk1_list = realloc(g_blk1_list, g_blk1_cap * sizeof *g_blk1_list); }
    g_blk1_n = 0; for (size_t b = 0; b < nb; b++) if (g_blk1[b]) g_blk1_list[g_blk1_n++] = (uint32_t)b;
    g_blk1_dirty = 0;
}
void blk1_init(void) {                           /* mark_setup, threads held */
    if (g_full_sweep < 0) g_full_sweep = getenv("SSBU_R1_FULL_SWEEP") != NULL;
    if (g_full_sweep || g_blk1 || !g_win1) return;
    uint8_t *b = calloc((size_t)(g_wn1 >> 18) + 1, 1);
    if (!b) return;                                     /* (hs13) out of memory: the whole map is swept (slower, the same pages) */
    {   /* window 2 (image + modules, 8 GB): the 64-page blocks of the first save's ranges there (nothing is committed there in a fight) */
        size_t nb2 = (size_t)(WN2 >> 18); uint8_t *b2 = calloc(nb2, 1); size_t k = 0;
        if (!b2) { free(b); return; }
        for (int i = 0; i < g_memory_map.range_count; i++) {
            uintptr_t a = (uintptr_t)g_memory_map.ranges[i].a;
            if (a < WIN2 || a >= WIN2 + WN2) continue;
            for (size_t q = (size_t)((a - WIN2) >> 18); q <= (size_t)((a + g_memory_map.ranges[i].n - 1 - WIN2) >> 18) && q < nb2; q++) b2[q] = 1;
        }
        for (size_t q = 0; q < nb2; q++) k += b2[q];
        g_blk2_list = malloc((k + 1) * sizeof *g_blk2_list); g_blk2_n = 0;
        if (!g_blk2_list) { free(b2); free(b); return; }
        g_blk1 = b;
        for (size_t q = 0; q < nb2; q++) if (b2[q]) g_blk2_list[g_blk2_n++] = (uint32_t)q;
        free(b2);
    }
    for (int i = 0; i < g_memory_map.range_count; i++) if (g_memory_map.ranges[i].a >= g_win1 && g_memory_map.ranges[i].a < g_win1 + g_wn1) blk1_add((size_t)(g_memory_map.ranges[i].a - g_win1) >> 12, (size_t)(g_memory_map.ranges[i].a + g_memory_map.ranges[i].n - g_win1) >> 12);
}
/* Round 5 (cheaper copy, run-ahead only): the pages a sweep finds are copied by the main thread and SSBU_RUNAHEAD_COPY_THREADS
 * helper threads (default 2; 0 = the main thread alone), in equal shares. The extras go the same way (cp_add_range). */

static CopyJob *g_cj; size_t g_ncj; static size_t g_cjcap;
static int g_ncp = -1;
static HANDLE g_cp_go[8], g_cp_done[8];
void cp_add(uint8_t *d, const uint8_t *s) { if (g_ncj >= g_cjcap) { g_cjcap = g_cjcap ? g_cjcap * 2 : 65536; g_cj = realloc(g_cj, g_cjcap * sizeof *g_cj); } g_cj[g_ncj].d = d; g_cj[g_ncj].s = s; g_ncj++; }
void cp_add_range(uint8_t *d, const uint8_t *s, size_t n) { for (size_t o = 0; o < n; o += PG) cp_add(d + o, s + o); }
uint8_t *g_cp_cmp;                             /* (R2 round 3) set: the queued jobs are COMPARED (g_cp_cmp[i] = 1 if they differ), not copied */
/* (R3 net3) g_cp_cond set: each queued page is copied only if it differs from its source (a ring load: most learned pages
 * already hold the value being loaded; reading both is cheaper than writing the page); g_cp_wrote[k] counts the copies */
int g_cp_cond; volatile long long g_cp_wrote[9];
/* (2026-10-27) A page that the game decommitted after the ring listed it (the extras are the ranges committed at the last full save; jemalloc
 * purges its arena afterwards, and a guest thread's stack / TLS goes away with the thread) faults when a job reads or writes it: c0000005 in
 * the first capture's compare (joiner of a Local Wireless room match). The job is skipped instead (its compare reads "equal", its restore is
 * not done: the page is gone) and counted in g_cp_fault. A guard per page costs nothing when no page faults (x64 SEH is table driven); a VirtualQuery
 * per region before every capture did the same check and cost 26 ms per capture once the extras were fragmented (measured, Ryzen 9 7900X). */
volatile LONG64 g_cp_fault;
/* The page operations go through a volatile function pointer: clang 9 builds the __try's scope table only from calls that may throw, and treats memcpy / memcmp
 * (builtins, nounwind) as unable to, so a fault inside them was NOT caught (found 2026-10-27: a write fault in memcpy killed the game through the unhandled-exception
 * report, while the reads in memcmp were caught). A call through a pointer the compiler cannot see through is covered. */
static int cp_op_cmp(uint8_t *d, const uint8_t *s) { return memcmp(d, s, PG) != 0; }
static int cp_op_copy(uint8_t *d, const uint8_t *s) { memcpy(d, s, PG); return 1; }
static int cp_op_cond(uint8_t *d, const uint8_t *s) { if (memcmp(d, s, PG)) { memcpy(d, s, PG); return 1; } return 0; }
int (*volatile cp_f_cmp)(uint8_t *, const uint8_t *) = cp_op_cmp;
static int (*volatile cp_f_copy)(uint8_t *, const uint8_t *) = cp_op_copy;
static int (*volatile cp_f_cond)(uint8_t *, const uint8_t *) = cp_op_cond;
/* A restore copy that faults because the game decommitted the destination page after the state was captured: the state being restored HAD that page
 * committed with data (window 1 does the same with g_dc), so the page is committed again and the copy retried. Without this the page stayed gone and the
 * restored game found a missing page (or, after a purge and reuse, the allocator's old content) where its memory had been. Only restores set g_cp_restore;
 * a capture's fault (a page gone on the READ side) is still only skipped. Counted: g_cp_fault skipped, g_cp_recommitted put back. */
volatile int g_cp_restore;
volatile LONG64 g_cp_recommitted;
static int cp_recommit(uint8_t *d, const uint8_t *s) {
    if (g_cp_restore && VirtualAlloc(d, PG, MEM_COMMIT, PAGE_READWRITE)) {
        __try { cp_f_copy(d, s); InterlockedIncrement64(&g_cp_recommitted); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    InterlockedIncrement64(&g_cp_fault); return 0;
}
static void cp_part(int k) { size_t a = g_ncj * (size_t)k / (size_t)(g_ncp + 1), b = g_ncj * (size_t)(k + 1) / (size_t)(g_ncp + 1);
    if (g_cp_cmp) { for (size_t i = a; i < b; i++) { int d = 0; __try { d = cp_f_cmp(g_cj[i].d, g_cj[i].s); } __except (EXCEPTION_EXECUTE_HANDLER) { d = 0; InterlockedIncrement64(&g_cp_fault); } g_cp_cmp[i] = (uint8_t)d; } }
    else if (g_cp_cond) { long long w = 0; for (size_t i = a; i < b; i++) { __try { w += cp_f_cond(g_cj[i].d, g_cj[i].s); } __except (EXCEPTION_EXECUTE_HANDLER) { w += cp_recommit(g_cj[i].d, g_cj[i].s); } } g_cp_wrote[k < 9 ? k : 8] += w; }
    else for (size_t i = a; i < b; i++) { __try { cp_f_copy(g_cj[i].d, g_cj[i].s); } __except (EXCEPTION_EXECUTE_HANDLER) { cp_recommit(g_cj[i].d, g_cj[i].s); } } }
static DWORD WINAPI cp_thread(void *p) { int k = (int)(intptr_t)p; for (;;) { WaitForSingleObject(g_cp_go[k], INFINITE); cp_part(k + 1); SetEvent(g_cp_done[k]); } return 0; }
void cp_init(void) {
    if (g_ncp >= 0) return;
    const char *e = getenv("SSBU_RUNAHEAD_COPY_THREADS"); g_ncp = e ? atoi(e) : rgs_wanted() ? 6 : 2;      /* (R2 round 3: the ring, 6) */ if (g_ncp > 8) g_ncp = 8; if (g_ncp < 0) g_ncp = 0;
    SYSTEM_INFO si; GetSystemInfo(&si); if (g_ncp > (int)si.dwNumberOfProcessors - 2) g_ncp = (int)si.dwNumberOfProcessors > 2 ? (int)si.dwNumberOfProcessors - 2 : 0;
    for (int k = 0; k < g_ncp; k++) { g_cp_go[k] = CreateEventA(NULL, FALSE, FALSE, NULL); g_cp_done[k] = CreateEventA(NULL, FALSE, FALSE, NULL); HANDLE t = CreateThread(NULL, 0, cp_thread, (void *)(intptr_t)k, 0, NULL); SetThreadPriority(t, THREAD_PRIORITY_HIGHEST); CloseHandle(t); }
}
void cp_run(void) {                            /* every queued page, then the queue is empty */
    if (g_ncp > 0 && g_ncj >= 64) { for (int k = 0; k < g_ncp; k++) SetEvent(g_cp_go[k]); cp_part(0); WaitForMultipleObjects((DWORD)g_ncp, g_cp_done, TRUE, INFINITE); }
    else { int keep = g_ncp; g_ncp = 0; cp_part(0); g_ncp = keep; }
    g_ncj = 0;
    { static LONG64 seen; if (g_cp_fault != seen) { seen = g_cp_fault; LOG("r1: %lld page jobs skipped so far because the page was gone (the game decommitted it after the extras were listed)\n", (long long)g_cp_fault); LOG("r1: %lld restore copies committed a page again first (the game had decommitted it since the capture)\n", (long long)g_cp_recommitted); } }
}
int g_cp_defer;                                /* mark_sweep queues its copies (run-ahead) instead of doing them */
size_t mark_sweep(int dir) {
    size_t n = 0;
    if (g_full_sweep < 0) g_full_sweep = getenv("SSBU_R1_FULL_SWEEP") != NULL;
    if (!g_blk1 && !g_full_sweep && g_win1) blk1_init();
    if (g_blk1 && g_blk1_dirty) blk1_rebuild();
    for (int w = 0; w < 2; w++) {
        uint8_t *map = w ? *pm2 : *pm1; size_t np = (size_t)((w ? WN2 : g_wn1) >> 12);
        uint8_t *mem = w ? (uint8_t *)(uintptr_t)WIN2 : g_win1, *sh = w ? g_sh2 : g_sh1, *cm = w ? g_sh2c : g_sh1c;
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
                if (w ? g_ns2[j] : g_ns1[j]) continue;          /* not simulation */     /* audio's pages: not rolled back */
                if (!dir) { if (!w && g_dc[j]) continue; sh_commit(sh + j * PG, cm, j); if (g_cp_defer) cp_add(sh + j * PG, mem + j * PG); else memcpy(sh + j * PG, mem + j * PG, PG); }
                else if (cm[j]) {
                    if (!w && g_dc[j]) { VirtualAlloc(mem + j * PG, PG, MEM_COMMIT, PAGE_READWRITE); g_dc[j] = 0; }
                    if (g_cp_defer) cp_add(mem + j * PG, sh + j * PG); else memcpy(mem + j * PG, sh + j * PG, PG);
                }
                n++;
            }
        }
    }
    return n;
}
void mark_save(Ctx *c) {
    if (g_mcheck) {                                     /* the full snapshot of the check is taken INSIDE the same hold (round 4) */
        collect(c);
        for (int i = 0; i < g_memory_map.range_count; i++) g_memory_map.ranges[i].s = rs_alloc(g_memory_map.ranges[i].n);
    }
    double t0 = now_ms();
    park(0);
    double t1 = now_ms();
    if (g_session.cycle < 6) slow_report("at the save");
    size_t n = mark_sweep(0);
    double t2 = now_ms();
    if (g_mcheck) for (int i = 0; i < g_memory_map.range_count; i++) memcpy(g_memory_map.ranges[i].s, g_memory_map.ranges[i].a, g_memory_map.ranges[i].n);
    for (int i = 0; i < g_nx; i++) memcpy(g_x[i].b, g_x[i].a, g_x[i].n);
    /* (the catch table is one of the extras) */
    fibers_save();
    if (g_prot > 0) prot_all(1);                        /* debug: first writes fault from here on */
    unpark();
    g_mctx = *c;
    double t3 = now_ms();
    if (g_nms < 4096) { g_ms_s[g_nms] = t3 - t1; g_ms_scan[g_nms] = t2 - t1; g_pg_s[g_nms] = n; g_nms++; }
    LOG("r1: cycle %d SAVE at fight frame %lld (marks): %zu pages, %.3f ms (+ %.3f ms holding the threads)\n", g_session.cycle, g_session.fight_frame, n, t3 - t1, t1 - t0);
}
static void mark_check(void) {                         /* threads still held */
    size_t miss = 0; int shown = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {
        Rng *r = &g_memory_map.ranges[i];
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(r->a, &m, sizeof m) || m.State != MEM_COMMIT || (uint8_t *)m.BaseAddress + m.RegionSize < r->a + r->n) continue;
        for (size_t p = 0; p < r->n / PG; p++)
            if (!(r->async && r->async[p]) && !(g_audio_free && is_async(r->a + p * PG)) && !is_nosim(r->a + p * PG) && memcmp(r->a + p * PG, r->s + p * PG, PG)) {
                miss++;
                { uint8_t *mp; size_t mi; if (win_index(r->a + p * PG, &mp, &mi) && mp[mi]) g_miss_remarked++; }      /* marked again since the restore's sweep: written NOW by a thread that is not held */
                if (shown++ < 12) {     /* round 6: whose allocation (a guest thread's stack / TLS block, by thread name)? */
                    const char *owner = ""; MEMORY_BASIC_INFORMATION mq; Ctx *tc; const char *nm; HANDLE h;
                    if (VirtualQuery(r->a + p * PG, &mq, sizeof mq))
                        for (int ti = 0; guest_thread_info(ti, &tc, &nm, &h); ti++) {
                            MEMORY_BASIC_INFORMATION m2;
                            if ((tc->tpidr && VirtualQuery((void *)(uintptr_t)tc->tpidr, &m2, sizeof m2) && m2.AllocationBase == mq.AllocationBase) ||
                                (tc->x[31] && VirtualQuery((void *)(uintptr_t)tc->x[31], &m2, sizeof m2) && m2.AllocationBase == mq.AllocationBase)) { owner = nm ? nm : "?"; break; }
                        }
                    LOG("r1:   MARK missed %s%s%s%s\n", where(r->a + p * PG), g_miss_remarked_last != g_miss_remarked ? " (marked since the restore: a running thread wrote it during the hold)" : "", owner[0] ? " in the stack / TLS allocation of thread " : "", owner);
                }
                g_miss_remarked_last = g_miss_remarked;
                memcpy(r->a + p * PG, r->s + p * PG, PG);
                uint8_t *cm; size_t pi; uint8_t *sh = in_shadow(r->a + p * PG, &cm, &pi);
                if (sh) { sh_commit(sh, cm, pi); memcpy(sh, r->s + p * PG, PG); }
            }
    }
    g_miss_total += (long long)miss; g_miss_cycles++;
    LOG("r1: MARK check: %zu pages differed after the marked restore (unmarked writers); put back from the full snapshot\n", miss);
}
void mark_restore(Ctx *c) {
    double t0 = now_ms();
    park(1);
    if (g_prot > 0 && g_prot_on) { prot_all(0); if (g_session.cycle % 5 == 4) prot_report(); }
    double t1 = now_ms();
    size_t n = mark_sweep(1);
    for (int i = 0; i < g_nx; i++) memcpy(g_x[i].a, g_x[i].b, g_x[i].n);
    /* (the catch table is one of the extras) */
    restore_thread_regs();
    fibers_restore_all();
    double t2 = now_ms();
    if (g_mcheck) mark_check();
    if (g_prot > 0) prot_all(1);                        /* debug: also the frames between this restore and the next save (a stale shadow page comes from there) */
    unpark();
    *c = g_mctx;
    if (g_nmr < 4096) { g_ms_r[g_nmr] = t2 - t1; g_pg_r[g_nmr] = n; g_nmr++; }
    LOG("r1: cycle %d RESTORE at fight frame %lld (marks): %zu pages, %.3f ms (+ %.3f ms holding the threads; %d threads not at their save wait, %d fibers not restored)\n",
        g_session.cycle, g_session.fight_frame, n, t2 - t1, t1 - t0, g_guest_threads.mismatch, g_guest_fibers.not_restored);
}
