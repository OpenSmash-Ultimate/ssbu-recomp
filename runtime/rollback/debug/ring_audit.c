/* RING AUDIT (SSBU_RB_AUDIT=<n>: every n captures; unset or 0 = off). Debugging aid for "a rollback restored the wrong state".
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The ring keeps, for every page of the saved memory, its value at the newest capture (rg_src). Right after a capture, with every
 * guest thread still held, that value must equal what is in memory, for every page the game can write. A page that differs now
 * was changed since the previous capture and the capture did NOT take it: the page tracker (the learned set, the write watch, the
 * marks) missed a write, so a later rollback would put an old value back, or leave a newer one in place, for that page.
 * The forced-rollback simulation (SSBU_RB_SIM) cannot find this: it replays the same pads, so a stale page is overwritten with the
 * same values again. A rollback in a real match replays DIFFERENT pads, and the game can then read a page that was never restored.
 *
 * What it does: compares every committed page of window 1, window 2 and the extras with the ring's newest value, one 4 KB page at
 * a time, skipping pages marked "not simulation" (host stacks, audio output) and, with SSBU_R1_AUDIO_FREE, the asynchronous ones.
 * A page that cannot be read (decommitted meanwhile) is skipped. It prints, per audit, how many pages differ in each memory kind and
 * the largest 64 KB blocks that hold differing pages (where() names the block), with the first differing words of one page.
 * Other threads of this exe (the renderer's copy-back of GPU results, loaders) can write guest memory while the audit runs: a block
 * that shows up once may be that; a block that shows up in audit after audit is a missed write. Cost: reads all committed memory
 * twice, about 0.3 to 0.6 s per audit on the machine of the study, so use n of 60 or more. */
#include "../model/game_state_internal.h"

static int g_au_every = -1;
static long long g_au_captures, g_au_runs;

typedef struct { uint64_t block; long long pages; uint8_t *first; } AuBlock;

/* Compares page `a` (page id `id`) with the ring's newest value. 1 = differs, 0 = same or not comparable. */
static int audit_page_differs(uint8_t *a, uint32_t id) {
    static uint8_t zero[PG];
    const uint8_t *src = rg_src(id);
    if (src == (const uint8_t *)1) return 0;                 /* decommitted at the newest capture */
    if (!src) src = zero;                                    /* a page that was all zero */
    if (is_nosim(a)) return 0;
    if (g_audio_free && is_async(a)) return 0;
    int d = 0;
    __try { d = cp_f_cmp(a, src); } __except (EXCEPTION_EXECUTE_HANDLER) { d = 0; }
    return d;
}

static void audit_note(AuBlock *tab, int *n, int cap, uint8_t *a) {
    uint64_t blk = (uint64_t)(uintptr_t)a >> 16;
    for (int i = 0; i < *n; i++) if (tab[i].block == blk) { tab[i].pages++; return; }
    if (*n < cap) { tab[*n].block = blk; tab[*n].pages = 1; tab[*n].first = a; (*n)++; }
}

/* Who owns the memory block that contains `a`: the TLS block or the stack of a guest thread, or the stack of a fiber (same Windows
 * allocation); empty when none of them. Written to the audit line so a differing block can be tied to a thread. */
static void audit_owner(uint8_t *a, char *out, size_t cap) {
    out[0] = 0;
    MEMORY_BASIC_INFORMATION m; if (!VirtualQuery(a, &m, sizeof m)) return;
    void *ab = m.AllocationBase;
    Ctx *tc; const char *nm; HANDLE h; int o = 0;
    for (int i = 0; guest_thread_info(i, &tc, &nm, &h) && o < (int)cap - 120; i++) {
        MEMORY_BASIC_INFORMATION q;
        if (tc->tpidr && VirtualQuery((void *)(uintptr_t)tc->tpidr, &q, sizeof q) && q.AllocationBase == ab) o += snprintf(out + o, cap - o, " [TLS of %s]", nm ? nm : "?");
        if (tc->x[31] && VirtualQuery((void *)(uintptr_t)tc->x[31], &q, sizeof q) && q.AllocationBase == ab) o += snprintf(out + o, cap - o, " [stack of %s]", nm ? nm : "?");
    }
    for (int i = 0; i < g_guest_fibers.count && o < (int)cap - 120; i++) {
        MEMORY_BASIC_INFORMATION q; const Fib *f = &g_guest_fibers.table[i];
        if (f->sp && VirtualQuery((void *)(uintptr_t)f->sp, &q, sizeof q) && q.AllocationBase == ab) o += snprintf(out + o, cap - o, " [stack of a fiber %llx]", (unsigned long long)f->key);
    }
}

static void audit_run(const char *what, long long which);
/* Called by r1_ring_capture right after the capture's copies, every guest thread still held. */
void rg_audit(void) {
    if (g_au_every < 0) { const char *e = getenv("SSBU_RB_AUDIT"); g_au_every = e ? atoi(e) : 0; }
    if (g_au_every <= 0 || ++g_au_captures % g_au_every) return;
    audit_run("capture", g_au_captures);
}
/* ROUND TRIP (SSBU_RB_ROUNDTRIP=<n>: every n-th load; unset or 0 = off): the same comparison right after a LOAD, with every guest thread
 * still held and everything restored. A load of frame F must leave every page exactly as the ring recorded it at F, and the ring's
 * "newest value" of a page IS the value at F after a load. A page that differs now was not restored (or was changed by a host thread in
 * the meantime): the load did not give back the state that was saved. This is the check "save -> run -> load -> compare with the save".
 * Called by r1_ring_load before it lets the threads go. */
void rg_roundtrip_after_load(void) {
    static int every = -1; static long long loads;
    if (every < 0) { const char *e = getenv("SSBU_RB_ROUNDTRIP"); every = e ? atoi(e) : 0; }
    if (every <= 0 || ++loads % every) return;
    audit_run("load", loads);
}
static void audit_run(const char *what, long long which) {
    double t0 = now_ms();
    enum { CAP = 512 };
    static AuBlock tab[CAP]; int nb = 0;
    long long checked[3] = {0, 0, 0}, bad[3] = {0, 0, 0};      /* window 1, window 2, extras */
    uint8_t *w1 = g_win1; uint64_t w1n = g_wn1;
    for (int i = 0; i < g_memory_map.range_count; i++) {
        Rng *r = &g_memory_map.ranges[i];
        int kind; uint32_t base;
        if (r->a >= w1 && r->a < w1 + w1n) { kind = 0; base = (uint32_t)((r->a - w1) >> 12); }
        else if ((uint64_t)(uintptr_t)r->a >= WIN2 && (uint64_t)(uintptr_t)r->a < WIN2 + WN2) { kind = 1; base = (uint32_t)(g_rg_n1 + (((uint64_t)(uintptr_t)r->a - WIN2) >> 12)); }
        else continue;                                           /* not in a window: the extras below cover it */
        for (size_t p = 0; p < r->n / PG; p++) {
            uint8_t *a = r->a + p * PG;
            checked[kind]++;
            if (audit_page_differs(a, base + (uint32_t)p)) { bad[kind]++; audit_note(tab, &nb, CAP, a); }
        }
    }
    for (size_t e = 0; e < g_rg_nx; e++) {
        uint8_t *a = g_rg_xa[e];
        checked[2]++;
        if (audit_page_differs(a, (uint32_t)(g_rg_n1 + g_rg_n2 + e))) { bad[2]++; audit_note(tab, &nb, CAP, a); }
    }
    g_au_runs++;
    {   char nh[600]; int o = 0; nh[0] = 0;        /* threads that were NOT held during the capture (they ran while memory was copied) */
        for (int i = 0; i < g_guest_threads.count && o < 560; i++) if (!g_guest_threads.table[i].held) o += snprintf(nh + o, sizeof nh - o, " [%s]", g_guest_threads.table[i].name);
        if (g_au_runs <= 3 || nh[0]) LOG("r1: RING AUDIT threads not held during the capture:%s\n", nh[0] ? nh : " none"); }
    LOG("r1: RING AUDIT %lld (after %s %lld): pages checked window 1 %lld, window 2 %lld, extras %lld; DIFFER from the ring's newest value: window 1 %lld, window 2 %lld, extras %lld; %d blocks of 64 KB; took %.0f ms\n",
        g_au_runs, what, which, checked[0], checked[1], checked[2], bad[0], bad[1], bad[2], nb, now_ms() - t0);
    for (int shown = 0; shown < 12 && nb > 0; shown++) {         /* the largest blocks first */
        int best = 0; for (int i = 1; i < nb; i++) if (tab[i].pages > tab[best].pages) best = i;
        AuBlock b = tab[best]; tab[best] = tab[--nb];
        char words[300]; int o = 0; words[0] = 0;
        {   /* the first differing words of the first page: where it is (offset in the page), memory now, ring's value */
            uint32_t id = 0; int have = 0;
            uint8_t *a = b.first;
            if (a >= w1 && a < w1 + w1n) { id = (uint32_t)((a - w1) >> 12); have = 1; }
            else if ((uint64_t)(uintptr_t)a >= WIN2 && (uint64_t)(uintptr_t)a < WIN2 + WN2) { id = (uint32_t)(g_rg_n1 + (((uint64_t)(uintptr_t)a - WIN2) >> 12)); have = 1; }
            else for (size_t e = 0; e < g_rg_nx; e++) if (g_rg_xa[e] == a) { id = (uint32_t)(g_rg_n1 + g_rg_n2 + e); have = 1; break; }
            const uint8_t *s = have ? rg_src(id) : NULL; static uint8_t zero[PG];
            if (have && s != (const uint8_t *)1) {
                if (!s) s = zero;
                const uint64_t *x = (const uint64_t *)a, *y = (const uint64_t *)s; int nd = 0;
                __try { for (int k = 0; k < PG / 8 && nd < 3; k++) if (x[k] != y[k]) { nd++; o += snprintf(words + o, sizeof words - o, " +%03x: ring %llx now %llx", k * 8, (unsigned long long)y[k], (unsigned long long)x[k]); } }
                __except (EXCEPTION_EXECUTE_HANDLER) { }
            }
        }
        char own[400]; audit_owner(b.first, own, sizeof own);
        LOG("r1: RING AUDIT   block %p (%s): %lld pages differ; first page %p; owner:%s;%s\n", (void *)(uintptr_t)(b.block << 16), where((uint8_t *)(uintptr_t)(b.block << 16)), b.pages, (void *)b.first, own[0] ? own : " none of the guest threads' TLS or stacks", words);
    }
}

/* PAGE DUMP (SSBU_RB_PAGEDUMP=<n>: every n-th frame; unset or 0 = off). Debugging aid for "the two games of an online match split".
 * The frame checksum only covers the fighters, so a difference in the stage, items or anything else shows up only once it reaches a
 * fighter, frames later. This writes, after the capture of every n-th frame, a 64-bit hash of every saved page (the ring's newest
 * value: what the capture took) to <SSBU_RB_NET_LOG>.pages: "PGD1", the frame (int64), the page count (uint32), then one hash per
 * page id (0 = zero page, 1 = decommitted, 2 = not simulation / asynchronous). Page ids of the two windows are guest addresses, so
 * they line up between two games; extras are host allocations and may not. A frame captured again after a rollback is written
 * again: the last record of a frame is its final state. Comparing two games' files (tests/pagediff.py) gives the pages that differ
 * first. Cost: hashes all saved memory, about as long as an audit. */
void rg_pagedump(long long frame) {
    static int every = -1; static FILE *f; static uint64_t *h; static size_t hn;
    if (every < 0) {
        const char *e = getenv("SSBU_RB_PAGEDUMP"), *l = getenv("SSBU_RB_NET_LOG");
        every = e ? atoi(e) : 0;
        if (every > 0) { char path[1024]; snprintf(path, sizeof path, "%s.pages", l && *l ? l : "rb_pagedump"); f = fopen(path, "wb");
            LOG("r1: PAGE DUMP every %d frames to %s%s\n", every, path, f ? "" : ": cannot open"); }
        if (!f) every = 0;
    }
    if (every <= 0 || frame <= 0 || frame % every) return;
    double t0 = now_ms();
    uint32_t total = (uint32_t)(g_rg_n1 + g_rg_n2 + g_rg_nx);
    if (hn < total) { free(h); h = malloc(sizeof *h * total); hn = h ? total : 0; if (!h) return; }
    for (uint32_t id = 0; id < total; id++) {
        const uint8_t *s = rg_src(id); uint8_t *a = rg_addr(id);
        if (s == (const uint8_t *)1) { h[id] = 1; continue; }
        if (a && (is_nosim(a) || (g_audio_free && is_async(a)))) { h[id] = 2; continue; }
        if (!s) { h[id] = 0; continue; }
        const uint64_t *w = (const uint64_t *)s; uint64_t x = 0x9e3779b97f4a7c15ull;
        for (int k = 0; k < PG / 8; k++) { x ^= w[k]; x *= 0xff51afd7ed558ccdull; x ^= x >> 32; }
        h[id] = x < 3 ? x + 3 : x;
    }
    int64_t fr = frame;
    fwrite("PGD1", 1, 4, f); fwrite(&fr, 8, 1, f); fwrite(&total, 4, 1, f); fwrite(h, 8, total, f); fflush(f);
    static int logged; if (logged++ < 3) LOG("r1: PAGE DUMP frame %lld: %u pages (window 1 %zu at %p, window 2 %zu at %llx, extras %zu), %.0f ms\n", frame, total, g_rg_n1, (void *)g_win1, g_rg_n2, (unsigned long long)WIN2, g_rg_nx, now_ms() - t0);
}
