/* THE MEMORY MAP: which ranges of the game's memory the save covers (window 1, window 2, the extras, thread stacks), found once per match.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "game_state_internal.h"

MemoryMap g_memory_map;                      /* the one memory map (see MemoryMap in game_state_internal.h) */

/* ---------------------------------------------------------------- the ranges */
static void add_range(uint8_t *a, size_t n) {
    if (g_memory_map.range_count >= MAXR) return;
    if (g_memory_map.range_count && g_memory_map.ranges[g_memory_map.range_count - 1].a + g_memory_map.ranges[g_memory_map.range_count - 1].n == a) { g_memory_map.ranges[g_memory_map.range_count - 1].n += n; return; }
    g_memory_map.ranges[g_memory_map.range_count].a = a; g_memory_map.ranges[g_memory_map.range_count].n = n; g_memory_map.range_count++;
}
int rw(DWORD p) { return (p & 0xff) == PAGE_READWRITE || (p & 0xff) == PAGE_EXECUTE_READWRITE; }
/* the committed read-write parts of [lo, hi) */
static void add_span(uint8_t *lo, uint8_t *hi) {
    MEMORY_BASIC_INFORMATION m;
    for (uint8_t *p = lo; p < hi && VirtualQuery(p, &m, sizeof m); p = (uint8_t *)m.BaseAddress + m.RegionSize) {
        if (m.State == MEM_COMMIT && rw(m.Protect) && !(m.Protect & PAGE_GUARD)) {
            uint8_t *a = (uint8_t *)m.BaseAddress, *b = a + m.RegionSize;
            if (a < lo) a = lo;
            if (b > hi) b = hi;
            add_range(a, (size_t)(b - a));
        }
    }
}
static void add_alloc(void *inside) {
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(inside, &m, sizeof m) || !m.AllocationBase) return;
    uint8_t *base = (uint8_t *)m.AllocationBase, *p = base;
    while (VirtualQuery(p, &m, sizeof m) && m.AllocationBase == base) p = (uint8_t *)m.BaseAddress + m.RegionSize;
    add_span(base, p);
}
static void find_heaps(void) {             /* once: the reservations of >= 16 GB (the game's address region) */
    MEMORY_BASIC_INFORMATION m;
    uint8_t *cur = NULL; size_t span = 0;
    for (uint8_t *p = (uint8_t *)0x10000; (uintptr_t)p < 0x7FFFFFFF0000ull && VirtualQuery(p, &m, sizeof m); p = (uint8_t *)m.BaseAddress + m.RegionSize) {
        if (m.AllocationBase != cur) {
            if (cur && span >= (16ull << 30) && g_memory_map.heap_count < 8) g_memory_map.heap_base[g_memory_map.heap_count++] = cur;
            cur = (uint8_t *)m.AllocationBase; span = 0;
        }
        if (cur) span += m.RegionSize;
    }
    if (cur && span >= (16ull << 30) && g_memory_map.heap_count < 8) g_memory_map.heap_base[g_memory_map.heap_count++] = cur;
    for (int i = 0; i < g_memory_map.heap_count; i++) LOG("r1: game address region %d at %p\n", i, (void *)g_memory_map.heap_base[i]);
}
/* the scan above walks the whole address space (~130 ms): the match handshake runs it on the set-up screen so the first
 * fight's base save does not (the regions are made at boot; collect already keeps them for every later match) */
void r1_find_heaps_early(void) { if (!g_memory_map.heap_count) find_heaps(); }
static uint64_t g_ex[512][2]; static int g_nex;                       /* other guest threads' stacks: never saved / restored */
static void ex_cb(uint64_t key, int sig, int cnt, uint64_t len, uint64_t thread, uint64_t stack, uint64_t size) {
    (void)key; (void)sig; (void)cnt; (void)len; (void)thread;
    if (stack && size && g_nex < 512) { g_ex[g_nex][0] = stack & ~(uint64_t)(PG - 1); g_ex[g_nex][1] = (stack + size + PG - 1) & ~(uint64_t)(PG - 1); g_nex++; }
}
uint8_t *g_ar;
static int cmp_rng(const void *x, const void *y) { const Rng *a = x, *b = y; return a->a < b->a ? -1 : a->a > b->a; }
/* host DLLs that run fighter code (fighters_bin\*.dll, track S): their writable sections are simulation state too */

static void add_fighter_dlls(void) {
    HANDLE sn = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (sn == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me; me.dwSize = sizeof me;
    int nmod = 0; size_t bytes = 0;
    for (BOOL ok = Module32First(sn, &me); ok; ok = Module32Next(sn, &me)) {
        if (!strstr(me.szExePath, "fighters_bin") || getenv("SSBU_R1_NO_DLLS")) continue;
        uint8_t *b = me.modBaseAddr;
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
        IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
            if (sh[i].Characteristics & IMAGE_SCN_MEM_WRITE) {
                size_t n = (sh[i].Misc.VirtualSize + PG - 1) & ~(size_t)(PG - 1);
                add_span(b + sh[i].VirtualAddress, b + sh[i].VirtualAddress + n); bytes += n;
            }
        nmod++;
    }
    CloseHandle(sn);
    if (g_session.cycle == 0 && g_session.phase == 0) LOG("r1: fighter DLLs: %d, writable sections %.1f KB\n", nmod, bytes / 1024.0);
}
/* (R3 hs13, tw1j) the full save's copy of every range (r->s, ~1.9 GB in a match) is page memory of its own, not CRT heap: the
 * first save of the ring / run-ahead gives it back (decommits it) range by range while mark_setup moves it into the shadow,
 * so the peak commit is ~one copy of the game's memory instead of two (tw1j: a failed malloc there, not checked, crashed in
 * memcpy while the PC had 4-8 GB of free commit). Every allocation of that save is checked: on failure the save fails
 * cleanly (g_save_failed = 2) and the caller turns rollback off for the match. */
void *rs_alloc(size_t n) { return n ? VirtualAlloc(NULL, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) : NULL; }
void rs_free(void *p) { if (p) VirtualFree(p, 0, MEM_RELEASE); }

int g_rs_drop, g_rs_direct;                                   /* set by the first save's callers (the ring, run-ahead): r->s given back during mark_setup */
static int g_rb_test_nomem = -1;                        /* SSBU_RB_NET_TEST_NOMEM (test only): 1 = the commit check says no, 2 = an allocation of the full save fails, 3 = the ring's own allocations fail, 4 = the shadow's commit fails */
/* SSBU_RB_NET_NOROLLBACK=1: the match is played WITHOUT rollback (delay-based: every frame waits for the other player's pad), the way the game's own online play works.
 * It uses the same path as "no memory for the ring" (the ring's base is not made), so nothing is rolled back and no state is saved. */
int rb_norollback_asked(void) { static int v = -1; if (v < 0) { const char *e = getenv("SSBU_RB_NET_NOROLLBACK"); v = e && e[0] && e[0] != '0'; } return v; }
int rb_test_nomem(void) { if (g_rb_test_nomem < 0) { const char *e = getenv("SSBU_RB_NET_TEST_NOMEM"); g_rb_test_nomem = e ? atoi(e) : 0; if (rb_norollback_asked()) g_rb_test_nomem = 1; } return g_rb_test_nomem; }
double g_save_need_mb, g_save_avail_mb;          /* the last commit check (MB) */
void collect(Ctx *c) {
    for (int i = 0; i < g_memory_map.range_count; i++) { rs_free(g_memory_map.ranges[i].s); g_memory_map.ranges[i].s = NULL; free(g_memory_map.ranges[i].dirty); free(g_memory_map.ranges[i].async); }
    g_memory_map.range_count = 0;
    if (!g_memory_map.heap_count) find_heaps();
    for (int i = 0; i < g_memory_map.heap_count; i++) add_alloc(g_memory_map.heap_base[i]);
    add_span((uint8_t *)0x7100000000ull, (uint8_t *)0x7300000000ull);
    add_fighter_dlls();
    { size_t pmn; uint8_t *pm = ppool_bookkeeping(&pmn); if (pm && pmn) add_span(pm, pm + pmn); }      /* R4: the presentation pool's bookkeeping (rb_learned.c) */
    if (g_catch_addr && !getenv("SSBU_R1_NO_CATCH")) {     /* nnunwind.c: the catch frames (guest CFA -> host jmp_buf) */
        size_t before = 0, after = 0; for (int i = 0; i < g_memory_map.range_count; i++) before += g_memory_map.ranges[i].n;
        size_t cn = 8192 * 16; { const char *e = getenv("SSBU_R1_TEST_CATCH_KB"); if (e && atoi(e) > 0 && (size_t)atoi(e) * 1024 < cn) cn = (size_t)atoi(e) * 1024; }   /* (hs15, TEST ONLY) only that much of the table in the state: the pp3h crash on purpose */
        add_span((uint8_t *)(uintptr_t)g_catch_addr, (uint8_t *)(uintptr_t)g_catch_addr + cn);
        for (int i = 0; i < g_memory_map.range_count; i++) after += g_memory_map.ranges[i].n;
        static int said; if (!said++ || after - before < 8192 * 16) LOG("r1: the catch table (%u KB) is in the state with %.1f KB%s\n", 8192 * 16 / 1024, (after - before) / 1024.0, after - before < 8192 * 16 ? ": NOT ALL OF IT (pages never written are copy-on-write; hs15)" : "");
    }
    if (g_ar) add_alloc(g_ar);
    add_alloc((void *)(uintptr_t)c->x[31]);
    add_alloc((void *)(uintptr_t)c->tpidr);
    Ctx *tc; const char *nm; HANDLE h;
    for (int i = 0; guest_thread_info(i, &tc, &nm, &h); i++) if (tc->tpidr) add_alloc((void *)(uintptr_t)tc->tpidr);
    qsort(g_memory_map.ranges, (size_t)g_memory_map.range_count, sizeof g_memory_map.ranges[0], cmp_rng);
    int o = 0;                                        /* drop duplicates (two threads in one block) */
    for (int i = 0; i < g_memory_map.range_count; i++) {
        if (o && g_memory_map.ranges[i].a < g_memory_map.ranges[o - 1].a + g_memory_map.ranges[o - 1].n) continue;
        g_memory_map.ranges[o++] = g_memory_map.ranges[i];
    }
    g_memory_map.range_count = o;
    if (!getenv("SSBU_R1_CUT_STACKS")) return;
    g_nex = 0; os_r1_objs(ex_cb);
    for (int e = 0; e < g_nex; e++) {                 /* cut [lo, hi) out of every range */
        uint8_t *lo = (uint8_t *)(uintptr_t)g_ex[e][0], *hi = (uint8_t *)(uintptr_t)g_ex[e][1];
        for (int i = 0; i < g_memory_map.range_count; i++) {
            Rng *r = &g_memory_map.ranges[i]; uint8_t *a = r->a, *b = r->a + r->n;
            if (hi <= a || lo >= b) continue;
            if (lo > a && hi < b && g_memory_map.range_count < MAXR) {     /* split */
                memmove(&g_memory_map.ranges[i + 2], &g_memory_map.ranges[i + 1], (size_t)(g_memory_map.range_count - i - 1) * sizeof g_memory_map.ranges[0]); g_memory_map.range_count++;
                g_memory_map.ranges[i + 1].a = hi; g_memory_map.ranges[i + 1].n = (size_t)(b - hi); r->n = (size_t)(lo - a);
            } else if (lo <= a && hi >= b) { r->n = 0; }
            else if (lo <= a) { r->a = hi; r->n = (size_t)(b - hi); }
            else r->n = (size_t)(lo - a);
        }
    }
    int o2 = 0; for (int i = 0; i < g_memory_map.range_count; i++) if (g_memory_map.ranges[i].n) g_memory_map.ranges[o2++] = g_memory_map.ranges[i];
    g_memory_map.range_count = o2;
}

