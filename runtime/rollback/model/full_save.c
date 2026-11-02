/* THE FULL SAVE / RESTORE (SSBU_R1): the whole state copied into a shadow and put back, the base the ring starts from.
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Split out of r1_state.c; what it shares with the other rollback files is declared in game_state_internal.h. */
#include "game_state_internal.h"

/* ---------------------------------------------------------------- save / restore */

int g_mark;  
int g_ra, g_save_failed;        /* (R3 lwr) the thread that kept the first save from being made */
size_t nvn_r1_cb_save(void); void nvn_r1_cb_restore(void); size_t g_ra_cb_bytes;      /* nvn_gl.c */
static size_t g_saved_bytes;
void save(Ctx *c) {
    double t0 = now_ms();
    collect(c);
    double t1 = now_ms();
    g_saved_bytes = 0;
    {   /* (hs13) the commit this needs, checked first: the copy below (decommitted again while the shadow takes it when
         * g_rs_drop) + the shadow (as much again) + the ring's maps and pool; 1 GB more is left for the game and the driver */
        size_t tot = 0; for (int i = 0; i < g_memory_map.range_count; i++) tot += g_memory_map.ranges[i].n;
        MEMORYSTATUSEX ms; ms.dwLength = sizeof ms;
        double need = (g_rs_drop ? 1.0 : 2.0) * tot / 1048576.0 + 512.0, avail = GlobalMemoryStatusEx(&ms) ? ms.ullAvailPageFile / 1048576.0 : 1e12;
        g_save_need_mb = need; g_save_avail_mb = avail;
        if (rb_test_nomem() == 1 || avail < need + 1024.0) {
            LOG("r1: FULL SAVE NOT MADE: not enough free commit memory: it needs %.0f MB (+ 1024 MB left for the game), %.0f MB free%s\n", need, avail, rb_test_nomem() == 1 ? " (TEST: SSBU_RB_NET_TEST_NOMEM=1)" : "");
            g_save_failed = 2; return;
        }
    }
    {   int bad = 0;
        for (int i = 0; i < g_memory_map.range_count && !bad; i++) {
            Rng *r = &g_memory_map.ranges[i];
            r->s = g_rs_direct || (rb_test_nomem() == 2 && i == g_memory_map.range_count / 2) ? NULL : rs_alloc(r->n);
            r->dirty = calloc(r->n / PG, 1);
            r->async = calloc(r->n / PG, 1);
            if ((!r->s && !g_rs_direct) || !r->dirty || !r->async) { bad = 1; LOG("r1: FULL SAVE NOT MADE: allocation of %.1f MB for range %d of %d FAILED%s\n", r->n / 1048576.0, i, g_memory_map.range_count, rb_test_nomem() == 2 ? " (TEST: SSBU_RB_NET_TEST_NOMEM=2)" : ""); }
        }
        if (!bad && !g_guest_fibers.pool) { g_guest_fibers.pool = malloc(g_guest_fibers.pool_cap); if (!g_guest_fibers.pool) { bad = 1; LOG("r1: FULL SAVE NOT MADE: fiber pool allocation FAILED\n"); } }
        if (!bad && g_ecap < 65536) { void *ne = realloc(g_e, 65536 * sizeof *g_e); if (ne) { g_e = ne; g_ecap = 65536; } else { bad = 1; LOG("r1: FULL SAVE NOT MADE: list allocation FAILED\n"); } }   /* no allocation while threads are held */
        if (bad) { release(); g_save_failed = 2; return; }
    }
    if (g_fast > 0 && g_session.cycle < g_learn) async_learn();
    double tp0 = now_ms();
    if (g_ra > 0) { if (!ra_hold_quiet(200)) { LOG("r1: RA: first save: no quiet moment within 200 ms: not now (busy: %s)\n", g_ra_busy_first_save()); g_save_failed = 1; return; } } else park_quiet(0);
    fibers_save();
    double t1b = now_ms();
    for (int i = 0; i < g_memory_map.range_count; i++) { if (!g_rs_direct) memcpy(g_memory_map.ranges[i].s, g_memory_map.ranges[i].a, g_memory_map.ranges[i].n); g_saved_bytes += g_memory_map.ranges[i].n; }
    double t2 = now_ms();
    {   /* pages the OTHER threads write while main is held here (audio, vsync counter, loaders, render threads): they are
         * not the simulation's; they are left out of restore and verify. SSBU_R1_PAUSE=<ms> (default 50, 0 = none) */
        const char *pe = getenv("SSBU_R1_PAUSE"); int pause = g_rs_direct ? 0 : pe ? atoi(pe) : 30;     /* (the ring base: no copy to compare with; its async pages were only ever used by the full restore) */
        size_t np = 0; int shown = 0;
        if (pause > 0) {
            Sleep((DWORD)pause);
            for (int i = 0; i < g_memory_map.range_count; i++) {
                Rng *r = &g_memory_map.ranges[i];
                for (size_t p = 0; p < r->n / PG; p++)
                    if (memcmp(r->a + p * PG, r->s + p * PG, PG)) {
                        r->async[p] = 1; np++; if (g_audio_free) async_add(r->a + p * PG);
                        memcpy(r->s + p * PG, r->a + p * PG, PG);
                        if (g_session.cycle == 0 && shown < 40) g_e[shown++].a = r->a + p * PG;
                    }
            }
        }
        int mfail = 0; double tm0 = now_ms();
        if (g_mark > 0 && g_session.cycle == 0) { g_ctx_save = *c; mfail = !mark_setup(); }
        LOG("r1: SAVE TIMES: pause + compare %.1f ms, mark setup %.1f ms\n", tm0 - t2, now_ms() - tm0);     /* round 5: the marks start while the threads are still held (no unmarked gap after this full save) */
        if (g_ra > 0 && !mfail) nvn_r1_cb_save();
        unpark();
        if (mfail) { release(); g_save_failed = 2; return; }
        for (int i = 0; i < shown; i++) LOG("r1:   async page (written by host threads while main and the guest threads were held %d ms): %s\n", pause, where(g_e[i].a));
        LOG("r1: %zu pages written by host threads during a %d ms hold: left out of restore / verify; holding the guest threads took %.2f ms\n", np, pause, t1b - tp0);
    }
    g_ctx_save = *c;
    g_nos = 0; os_r1_objs(os_cb); qsort(g_os, (size_t)g_nos, sizeof *g_os, cmp_os);
    LOG("r1: cycle %d SAVE at fight frame %lld: %d ranges, %.1f MB copied in %.2f ms (+ %.2f ms to list the ranges)\n", g_session.cycle, g_session.fight_frame, g_memory_map.range_count, g_saved_bytes / 1048576.0, t2 - t1, t1 - t0);
    if (g_session.cycle == 0) {
        for (int i = 0; i < g_memory_map.range_count; i++) if (g_memory_map.ranges[i].n >= (4u << 20)) LOG("r1:   range %s + %.1f MB\n", where(g_memory_map.ranges[i].a), g_memory_map.ranges[i].n / 1048576.0);
        park_report("at the save");
        LOG("r1: %d fibers at the save:\n", g_guest_fibers.count); fibers_report();
    }
    { int ns = 0; for (int i = 0; i < g_guest_fibers.count; i++) ns += g_guest_fibers.table[i].susp;
      LOG("r1: fibers: %d running on a thread, %d in all, %d switched away (host stack + fiber block + context saved: %.1f KB), %d switched away but not readable\n", g_guest_fibers.running, g_guest_fibers.count, ns, g_guest_fibers.bytes / 1024.0, g_guest_fibers.unreadable_stack); }
}
/* the learned set (union of all changed pages so far): what a fixed-set save / restore would copy */
size_t g_learn_pages;
static double learned_copy_ms(int restore_dir) {
    static uint8_t *buf; static size_t cap;
    if (cap < g_learn_pages * PG) { free(buf); cap = g_learn_pages * PG + (64u << 20); buf = malloc(cap); }
    double t0 = now_ms(); size_t o = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {
        Rng *r = &g_memory_map.ranges[i];
        if (!r->learn) continue;
        for (size_t p = 0; p < r->n / PG; p++) if (r->learn[p]) {
            if (restore_dir) memcpy(buf + o, r->a + p * PG, PG); else memcpy(buf + o, r->a + p * PG, PG);
            o += PG;
        }
    }
    return now_ms() - t0;
}
      /* the learned set survives the range list */
Learn g_l[MAXR]; int g_nl;
static void learn_merge(void) {
    g_learn_pages = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {
        Rng *r = &g_memory_map.ranges[i];
        int j; for (j = 0; j < g_nl; j++) if (g_l[j].a == r->a && g_l[j].n == r->n) break;
        if (j == g_nl && g_nl < MAXR) { g_l[g_nl].a = r->a; g_l[g_nl].n = r->n; g_l[g_nl].learn = calloc(r->n / PG, 1); g_nl++; }
        if (j < g_nl) { for (size_t p = 0; p < r->n / PG; p++) g_l[j].learn[p] |= r->dirty[p]; r->learn = g_l[j].learn; }
    }
    for (int j = 0; j < g_nl; j++) for (size_t p = 0; p < g_l[j].n / PG; p++) g_learn_pages += g_l[j].learn[p];
}
const char *where(uint8_t *a) {
    static char b[96];
    if ((uintptr_t)a >= 0x7100000000ull && (uintptr_t)a < 0x7107500000ull) snprintf(b, sizeof b, "main+%llx", (unsigned long long)((uintptr_t)a - 0x7100000000ull));
    else if ((uintptr_t)a >= 0x7200000000ull && (uintptr_t)a < 0x7300000000ull) snprintf(b, sizeof b, "module area+%llx", (unsigned long long)((uintptr_t)a - 0x7200000000ull));
    else { int h = -1; for (int i = 0; i < g_memory_map.heap_count; i++) if (a >= g_memory_map.heap_base[i] && a < g_memory_map.heap_base[i] + (64ull << 30)) h = i;
           if (h >= 0) snprintf(b, sizeof b, "region%d+%llx", h, (unsigned long long)(a - g_memory_map.heap_base[h])); else snprintf(b, sizeof b, "stack/tls %p", (void *)a); }
    return b;
}

void stop_born_threads(void) {      /* guest threads created in the window do not exist at the save point: held for good */
    Ctx *tc; const char *nm; HANDLE h; int o = 0; g_nborn = 0;
    for (int i = g_guest_threads.info_count; guest_thread_info(i, &tc, &nm, &h); i++) {
        if (!h || GetThreadId(h) == GetCurrentThreadId()) continue;
        SuspendThread(h); g_nborn++;
        if (o < 1900) o += snprintf(g_born + o, sizeof g_born - o, " [%s %s]", nm ? nm : "?", tc->in_import ? tc->in_import : "guest code");
    }
    g_born[o] = 0;
}
void restore(Ctx *c) {
    double tp0 = now_ms();
    if (!getenv("SSBU_R1_KEEP_NEW_THREADS")) stop_born_threads();
    park_quiet(1);
    double t0 = now_ms();
    g_ne = 0;
    size_t pages = 0, ranges_hit = 0, ranges_hit_bytes = 0, skipped = 0;
    for (int i = 0; i < g_memory_map.range_count; i++) {
        Rng *r = &g_memory_map.ranges[i];
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(r->a, &m, sizeof m) || m.State != MEM_COMMIT || (uint8_t *)m.BaseAddress + m.RegionSize < r->a + r->n) { skipped++; if (!VirtualAlloc(r->a, r->n, MEM_COMMIT, PAGE_READWRITE)) continue; }   /* decommitted by the game in the window: committed again, its bytes come back below */
        size_t hit = 0;
        for (size_t p = 0; p < r->n / PG; p++) {
            uint8_t *a = r->a + p * PG, *s = r->s + p * PG;
            if (r->async && r->async[p]) continue;
            if (memcmp(a, s, PG)) {
                if (g_ne >= g_ecap) continue;          /* (65,536 pages = 256 MB: not reached) */
                g_e[g_ne].a = a; memcpy(g_e[g_ne].d, a, PG); g_ne++;
                memcpy(a, s, PG);
                r->dirty[p] = 1; hit++;
            }
        }
        pages += hit;
        if (hit) { ranges_hit++; ranges_hit_bytes += r->n; }
    }
    restore_thread_regs();
    fibers_restore();
    double t1 = now_ms();
    unpark();
    *c = g_ctx_save;                                   /* the main thread's guest registers at the save point */
    LOG("r1: holding the guest threads at their save-point waits took %.2f ms; %d of %d not at the same wait\n", t0 - tp0, g_guest_threads.mismatch, g_guest_threads.count);
    if (g_session.cycle == 0 || g_guest_threads.mismatch) park_report("at the restore");
    LOG("r1: fibers at the restore: %d at their save-point state (registers restored), %d not, %d new\n", g_guest_fibers.restored, g_guest_fibers.not_restored, g_guest_fibers.replaced);
    learn_merge();
    LOG("r1: cycle %d RESTORE at fight frame %lld: %zu pages changed (%.1f MB) in %zu of %d ranges (those ranges %.1f MB); compare + copy back %.2f ms; %zu ranges partly decommitted in the window (committed again)\n",
        g_session.cycle, g_session.fight_frame, pages, pages * PG / 1048576.0, ranges_hit, g_memory_map.range_count, ranges_hit_bytes / 1048576.0, t1 - t0, skipped);
    {   /* where the changed pages are: one line per contiguous group (first 40) */
        int shown = 0;
        for (size_t i = 0; i < g_ne && shown < 40; ) {
            size_t j = i + 1;
            while (j < g_ne && g_e[j].a == g_e[j - 1].a + PG) j++;
            if (g_session.cycle == 0) LOG("r1:   changed %s: %zu pages\n", where(g_e[i].a), j - i);
            shown++; i = j;
        }
    }
    LOG("r1: learned set (union of changed pages, all cycles) %zu pages = %.1f MB: memcpy of it %.2f ms\n", g_learn_pages, g_learn_pages * PG / 1048576.0, learned_copy_ms(0));
    os_compare("at the restore");
}
void verify(void) {
    double t0 = now_ms();
    size_t bad = 0, extra = 0; int shown = 0;
    for (size_t i = 0; i < g_ne; i++) {
        MEMORY_BASIC_INFORMATION mq; if (!VirtualQuery(g_e[i].a, &mq, sizeof mq) || mq.State != MEM_COMMIT) continue;     /* decommitted in the re-run */
        if (memcmp(g_e[i].a, g_e[i].d, PG)) {
            bad++;
            if (shown++ < 24) {
                size_t o = 0; while (o < PG && g_e[i].a[o] == g_e[i].d[o]) o++;
                LOG("r1:   re-run differs at %s (+%zx: %02x vs %02x first pass)\n", where(g_e[i].a), o, g_e[i].a[o], g_e[i].d[o]);
            }
        }
    }
    for (int i = 0; i < g_memory_map.range_count; i++) {                   /* pages the re-run changed but the first pass did not */
        Rng *r = &g_memory_map.ranges[i];
        MEMORY_BASIC_INFORMATION m;
        if (!VirtualQuery(r->a, &m, sizeof m) || m.State != MEM_COMMIT || (uint8_t *)m.BaseAddress + m.RegionSize < r->a + r->n) continue;
        for (size_t p = 0; p < r->n / PG; p++)
            if (!r->dirty[p] && !(r->async && r->async[p]) && memcmp(r->a + p * PG, r->s + p * PG, PG)) {
                extra++;
                if (shown++ < 36) LOG("r1:   re-run changed %s, the first pass did not\n", where(r->a + p * PG));
            }
    }
    LOG("r1: cycle %d VERIFY at fight frame %lld (end of the re-run): %zu of %zu changed pages differ from the first pass's end state, %zu pages changed only in the re-run (%.1f ms)\n",
        g_session.cycle, g_session.fight_frame, bad, g_ne, extra, now_ms() - t0);
    os_compare("at the end of the re-run");
}
void release(void) { for (int i = 0; i < g_memory_map.range_count; i++) { rs_free(g_memory_map.ranges[i].s); g_memory_map.ranges[i].s = NULL; free(g_memory_map.ranges[i].dirty); g_memory_map.ranges[i].dirty = NULL; free(g_memory_map.ranges[i].async); g_memory_map.ranges[i].async = NULL; } }

