/* C++ ABI runtime (libc++abi's role): exceptions, guard variables, RTTI support, the exception classes of
 * <exception>/<stdexcept>/<new>/<typeinfo>, and the DATA symbols the game imports from the C++ runtime
 * (vtables and typeinfo objects), built as host tables laid out per the Itanium C++ ABI.
 *
 * Sources of each layout: [sdk] = read from the real SDK module (binaries\13.0.4\sdk.elf; `python sdkdata.py
 * <symbol>` dumps a data object with its relocations, `python sdkdis.py <symbol>` disassembles a function).
 *
 * EXCEPTIONS -- measured on main 13.0.4 (python eh_census.py; python eh_census2.py):
 *   295 call sites of __cxa_throw (91 std::bad_function_call, 68 std::exception-derived game types, 63 std::out_of_range,
 *   47 std::bad_alloc, 17 std::length_error, 9 boost exceptions), 17 of __cxa_rethrow, and about 10,400 calls of
 *   libc++'s __throw_length_error / __throw_out_of_range helpers (libcxx.c).
 *   .eh_frame (found through MOD0) has 110,842 FDEs, 1,040 with an LSDA: 10,346 call-site records, 7,291 with a landing
 *   pad: 3,668 clean-up only, 40 exception-specification filters, 3,583 with a catch clause = 3,540 distinct pads:
 *     3,456 go straight to __clang_call_terminate (noexcept boundaries), 66 are clean-ups that end there,
 *     14 are `catch (...) { clean up; throw; }` (they rethrow),
 *     1 is `catch (std::ios_base::failure &)` at 0x7103839de8 (only the stream library could throw that; ours never does),
 *     4 are `catch (...) { }` that SWALLOW the exception: 0x710382d0a0 and 0x710382d1cc in f_7103828300,
 *       0x7103847670 and 0x710384779c in f_7103841d10 (two text parsers; the protected calls are
 *       f_7103835e60 / f_7103836070 / f_7103836150 / f_7103836230 / f_71038368e0, i.e. tokenising and boost::lexical_cast).
 *   So on the console every throw ends in std::terminate, except a throw inside those four protected regions.
 *
 * What is implemented here: the exception RUNTIME (interface and flow in eh_runtime.h): exception objects with
 * libc++abi's header layout, per-context throw state, __cxa_throw / rethrow / begin_catch / end_catch / _Unwind_Resume /
 * exception_ptr with their real semantics, the personality as a query over the guest's own LSDA tables (catch clauses
 * with base-class matching, catch (...), exception specifications, clean-ups), and the table of loaded images.
 * Transferring control to a landing pad is the unwinder's job (nnunwind.c, installed with eh_set_unwinder). Without an
 * unwinder every throw ends as std::terminate does: a report (type, what(), guest call stack) and exit code 3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "eh_runtime.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P64(p) ((uint64_t)(uintptr_t)(p))

void libc2_install_data(void);           /* nnlibc2.c */
void register_libc2_imports(void);
void cxxlib_install_data(void);          /* nncxxlib.c */
void register_cxxlib_imports(void);
uint64_t import_address(int sym);       /* hostrt.c */
void cxx_string_init(Ctx *c, uint64_t str, const char *p, size_t n);     /* libcxx.c */
const char *cxx_string_data(uint64_t str, size_t *n);

/* ---------------------------------------------------------------- small helpers shared with nncxxlib.c */
uint64_t cxxrt_main_symbol(const char *name) {           /* address of a function the game itself defines and exports */
    for (int i = 0; i < dyn_sym_count; i++)
        if (dyn_syms[i].defined && strcmp(dyn_syms[i].name, name) == 0) return GUEST_BASE + dyn_syms[i].value;
    return 0;
}
/* P3 (hitch log: 150 + 70 ms per throw of lua2cpp::L2CExceptionShutdown, tests/logs/p3b_eh.log): VirtualQuery walks the
 * page tables to the end of the run of pages with the same state, which takes 2-3 ms inside the game's heap (gigabytes
 * committed in one run), and one throw asks about 22 addresses there (the fiber's stack). The answer for memory the GAME
 * manages (regions from nn::os::AllocateAddressRegion, registered with cxxrt_guest_region) is therefore remembered per
 * thread. Host memory is asked about every time, as before. SSBU_NO_READABLE_CACHE=1 = ask every time everywhere.
 *
 * PR #2 (jfb112697): what makes a remembered run wrong is guest memory being decommitted, released or protected
 * (cxxrt_mem_changed, called by those entry points before and after the change; prototype in hostrt.h). Forgetting
 * everything at each such event left one slow question per throw whenever the game had freed pages anywhere since the
 * thread's last throw (measured once by the contributor, one scripted match of 300 s on one PC: 73 of 1,545 throws took
 * 1.0-7.4 ms up to their landing pad, each of them one VirtualQuery over a run of 200-700 MB; the other throws
 * 0.02-0.1 ms). The events are therefore kept with their address range (the last RD_CHANGES of them), and a remembered
 * run is only cut back by the events that overlap it: the part of a committed, accessible run that no such event touched
 * is still committed and accessible. A record older than the kept events is forgotten. Same match with this: 8 of 1,497
 * throws over 1 ms (1.4-6.3 ms). Those are the first question a thread asks about a run (the record starts at the page
 * asked about, so a fiber stack lower in the same run asks again), which this does not remove.
 * SSBU_READABLE_FORGET_ALL=1 = the behaviour before: every event forgets every record. */
static struct { uint64_t base, size; } g_guest_region[64];
static volatile LONG g_guest_regions, g_mem_epoch = 1;
#define RD_CHANGES 256      // PR #2 (jfb112697): ring of the last changes of guest memory, with their page ranges
static struct { uint64_t lo, hi; } g_rd_change[RD_CHANGES];       /* event number k (the value g_mem_epoch got) is in slot k % RD_CHANGES */
static SRWLOCK g_rd_change_lock = SRWLOCK_INIT;
static int g_rd_nocache = -1, g_rd_forget_all;
static volatile LONG64 g_rd_asked, g_rd_cached, g_rd_kept;
void cxxrt_guest_region(uint64_t base, uint64_t size) {
    LONG i = InterlockedIncrement(&g_guest_regions) - 1;
    if (i < 64) { g_guest_region[i].size = size; g_guest_region[i].base = base; } else InterlockedDecrement(&g_guest_regions);
}
/* Guest memory [base, base + size) is about to change or has just changed. size 0 = the whole region that starts at
 * base (a release), or everything when no such region is registered. */
void cxxrt_mem_changed(uint64_t base, uint64_t size) {      // PR #2 (jfb112697): was (void); the event now carries its range
    uint64_t lo = base & ~0xfffull, hi = (base + size + 0xfff) & ~0xfffull;      /* the OS changes whole pages */
    if (!size || hi < lo) {
        lo = 0; hi = ~0ull;
        if (!size) for (int i = 0; i < g_guest_regions && i < 64; i++) if (g_guest_region[i].base == base) { lo = base; hi = base + g_guest_region[i].size; break; }
    }
    AcquireSRWLockExclusive(&g_rd_change_lock);
    ULONG k = (ULONG)g_mem_epoch + 1;
    g_rd_change[k % RD_CHANGES].lo = lo; g_rd_change[k % RD_CHANGES].hi = hi;
    g_mem_epoch = (LONG)k;
    ReleaseSRWLockExclusive(&g_rd_change_lock);
}
void cxxrt_readable_report(FILE *out) {      // PR #2 (jfb112697): also counts answers from a record that outlived a change elsewhere
    fprintf(out, "nncxxrt: readable checks: %lld answered by VirtualQuery, %lld from the per-thread record of game-managed memory (%lld of them from a record that was older than a change of guest memory elsewhere)\n",
            (long long)g_rd_asked, (long long)g_rd_cached, (long long)g_rd_kept);
}
typedef struct { uint64_t lo, hi; LONG epoch; } RdRecord;
/* The record holds [p, p + n) and is older than the newest event: applies the events since then to it. 0 = one of them
 * touches [p, p + n) or the events are no longer kept (the record is dropped). */
static int rd_bring_up_to_date(RdRecord *r, uint64_t p, size_t n) {      // PR #2 (jfb112697): trims a record by the changes that overlap it
    int ok = 1;
    AcquireSRWLockShared(&g_rd_change_lock);
    ULONG now = (ULONG)g_mem_epoch, from = (ULONG)r->epoch;
    if (now - from > RD_CHANGES) ok = 0;
    for (ULONG k = from + 1; ok && k - from <= now - from; k++) {
        uint64_t lo = g_rd_change[k % RD_CHANGES].lo, hi = g_rd_change[k % RD_CHANGES].hi;
        if (hi <= r->lo || lo >= r->hi) continue;
        if (lo >= p + n) r->hi = lo;                  /* the event is above what is asked about: the run ends there now */
        else if (hi <= p) r->lo = hi;                 /* below: it starts there */
        else ok = 0;
    }
    r->epoch = (LONG)now;
    ReleaseSRWLockShared(&g_rd_change_lock);
    if (!ok) r->lo = r->hi = 0;
    return ok;
}
int cxxrt_readable(uint64_t p, size_t n) {
    static __declspec(thread) RdRecord t_ok[4];
    static __declspec(thread) unsigned t_next;
    MEMORY_BASIC_INFORMATION mi;
    if (p < 0x10000) return 0;
    LONG epoch = g_mem_epoch;
    int guest = 0;
    if (g_rd_nocache < 0) { g_rd_forget_all = getenv("SSBU_READABLE_FORGET_ALL") != NULL; g_rd_nocache = getenv("SSBU_NO_READABLE_CACHE") != NULL; }      // PR #2 (jfb112697): SSBU_READABLE_FORGET_ALL=1 = the old forget-everything rule
    if (!g_rd_nocache) {
        for (int i = 0; i < g_guest_regions && i < 64 && !guest; i++) guest = p - g_guest_region[i].base < g_guest_region[i].size;
        if (guest) for (int i = 0; i < 4; i++) {      // PR #2 (jfb112697): an older record is brought up to date instead of being dropped
            if (p < t_ok[i].lo || p + n > t_ok[i].hi) continue;
            if (t_ok[i].epoch == epoch) { g_rd_cached++; return 1; }
            if (!g_rd_forget_all && rd_bring_up_to_date(&t_ok[i], p, n)) { g_rd_cached++; g_rd_kept++; return 1; }
        }
    }
    g_rd_asked++;
    if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
    if (guest && g_mem_epoch == epoch) { unsigned k = t_next++ & 3; t_ok[k].lo = (uint64_t)(uintptr_t)mi.BaseAddress; t_ok[k].hi = t_ok[k].lo + mi.RegionSize; t_ok[k].epoch = epoch; }
    return p + n <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
}
/* Calls guest (or host-table) code at addr with up to three arguments; returns x0. */
uint64_t cxxrt_call(Ctx *c, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2) {
    uint64_t lr = c->x[30];
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2;
    call_guest(c, addr);
    c->x[30] = lr;
    return c->x[0];
}
void cxxrt_guest_delete(Ctx *c, uint64_t p) {            /* the game's own operator delete (it defines and exports it) */
    static uint64_t del;
    if (!del) del = cxxrt_main_symbol("_ZdlPv");
    if (!p) return;
    if (!del) { printf("\nnncxxrt: the game does not export operator delete\n"); report_and_exit(3); }
    uint64_t x0 = c->x[0];
    cxxrt_call(c, del, p, 0, 0);
    c->x[0] = x0;
}
void cxxrt_print_guest_stack(void) {
#ifdef _WIN32
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    printf("guest call stack (innermost first):\n");
    uint64_t last = 0;
    for (int depth = 0, shown = 0; depth < 600 && shown < 48; depth++) {
        uint64_t g = guest_function_of(ctx.Rip);
        if (g && g != last) { printf("  f_%llx\n", (unsigned long long)g); trace_printf("  f_%llx\n", (unsigned long long)g); shown++; last = g; }
        DWORD64 base;
        RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
        if (!rf) break;
        void *hd; DWORD64 est;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &est, NULL);
        if (!ctx.Rip) break;
    }
#else
    uintptr_t pcs[600];
    int n = plat_backtrace(pcs, 600);
    printf("guest call stack (innermost first):\n");
    uint64_t last = 0;
    for (int i = 0, shown = 0; i < n && shown < 48; i++) {
        uint64_t g = guest_function_of(pcs[i]);
        if (g && g != last) { printf("  f_%llx\n", (unsigned long long)g); trace_printf("  f_%llx\n", (unsigned long long)g); shown++; last = g; }
    }
#endif
}
static void fatal(Ctx *c, const char *fmt, ...) {
    char msg[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("\nC++ RUNTIME: %s (guest caller 0x%llx, in f_%llx)\n", msg, (unsigned long long)c->x[30] - 4, (unsigned long long)c->fn);
    trace_printf("C++ RUNTIME: %s (guest caller 0x%llx)\n", msg, (unsigned long long)c->x[30] - 4);
    cxxrt_print_guest_stack();
    report_and_exit(3);
}

/* ---------------------------------------------------------------- RTTI
 * [sdk] typeinfo objects: { vptr = <kind table> + 0x10, const char *name } ; __si_class_type_info adds { base } ;
 * __vmi_class_type_info adds { u32 flags, u32 base_count, { base, long offset_flags }[] } with offset = offset_flags >> 8,
 * bit 0 = virtual (then the offset indexes the vtable), bit 1 = public. The kind is recognised by the vptr, so each
 * imported __cxxabiv1 table has its own address; their virtual functions are never called by the game. */
static uint64_t g_vt_class[12], g_vt_si[12], g_vt_vmi[12], g_vt_fund[12], g_vt_ptr[12];
enum { TI_OTHER, TI_CLASS, TI_SI, TI_VMI };
static int ti_kind(uint64_t ti) {
    if (!cxxrt_readable(ti, 16)) return TI_OTHER;
    uint64_t v = rd64(ti);
    return v == P64(&g_vt_class[2]) ? TI_CLASS : v == P64(&g_vt_si[2]) ? TI_SI : v == P64(&g_vt_vmi[2]) ? TI_VMI : TI_OTHER;
}
const char *cxxrt_type_name(uint64_t ti) {
    if (!cxxrt_readable(ti, 16)) return "?";
    uint64_t n = rd64(ti + 8) & 0x7fffffffffffffffull;       /* the top bit marks a non-unique name */
    return cxxrt_readable(n, 2) ? (const char *)(uintptr_t)n : "?";
}
static int ti_same(uint64_t a, uint64_t b) { return a == b || (a && b && strcmp(cxxrt_type_name(a), cxxrt_type_name(b)) == 0); }
/* Collects the addresses of every `target` sub-object inside the object of dynamic type `ti` at `addr`. */
static void ti_collect(uint64_t ti, uint64_t addr, uint64_t target, uint64_t *out, int *n, int max, int depth) {
    if (depth > 16) return;
    if (ti_same(ti, target)) {
        for (int i = 0; i < *n; i++) if (out[i] == addr) return;      /* a shared virtual base is one sub-object */
        if (*n < max) out[(*n)++] = addr;
        return;
    }
    int kind = ti_kind(ti);
    if (kind == TI_SI) ti_collect(rd64(ti + 16), addr, target, out, n, max, depth + 1);
    else if (kind == TI_VMI) {
        uint32_t count = rd32(ti + 20);
        for (uint32_t i = 0; i < count && i < 64; i++) {
            uint64_t base = rd64(ti + 24 + 16ull * i);
            int64_t of = (int64_t)rd64(ti + 32 + 16ull * i), off = of >> 8;
            if (of & 1) {                                              /* virtual base: its offset is in the vtable */
                if (!cxxrt_readable(addr, 8) || !cxxrt_readable(rd64(addr) + (uint64_t)off, 8)) continue;
                off = (int64_t)rd64(rd64(addr) + (uint64_t)off);
            }
            ti_collect(base, addr + (uint64_t)off, target, out, n, max, depth + 1);
        }
    }
}
/* Address of the unique `target` base of an object, or 0. */
static uint64_t ti_upcast(uint64_t ti, uint64_t obj, uint64_t target) {
    uint64_t found[8];
    int n = 0;
    ti_collect(ti, obj, target, found, &n, 8, 0);
    return n == 1 ? found[0] : 0;
}
/* void *__dynamic_cast(const void *sub, const type_info *src, const type_info *dst, ptrdiff_t src2dst):
 * down-casts and cross-casts over single, multiple and virtual inheritance; access (public/private) is not checked. */
uint64_t cxxrt_dynamic_cast(uint64_t sub, uint64_t src, uint64_t dst) {
    if (!sub) return 0;
    uint64_t vptr = rd64(sub), whole = sub + rd64(vptr - 16), dyn = rd64(vptr - 8);
    uint64_t dsts[16], srcs[16];
    int nd = 0, ns = 0, hits = 0;
    uint64_t hit = 0;
    ti_collect(dyn, whole, dst, dsts, &nd, 16, 0);
    for (int i = 0; i < nd; i++) {                                     /* a dst object that contains this src sub-object */
        ns = 0;
        ti_collect(dst, dsts[i], src, srcs, &ns, 16, 0);
        for (int k = 0; k < ns; k++) if (srcs[k] == sub) { hit = dsts[i]; hits++; break; }
    }
    if (hits == 1) return hit;
    if (hits > 1) return 0;
    ns = 0;
    ti_collect(dyn, whole, src, srcs, &ns, 16, 0);                     /* cross-cast: dst must be unambiguous */
    for (int k = 0; k < ns; k++) if (srcs[k] == sub) return nd == 1 ? dsts[0] : 0;
    return 0;
}
static void abi_dynamic_cast(Ctx *c) { RET(cxxrt_dynamic_cast(A0, A1, A2)); }

/* ---------------------------------------------------------------- exception objects and the EH runtime (eh_runtime.h)
 * [sdk __cxa_throw 0x4c9ea0, __cxa_begin_catch 0x4c9fb8] the header in front of a thrown object is libc++abi's
 * __cxa_exception, 0x80 bytes; the fields the SDK code touches are at the same offsets here. The two handler
 * pointers libc++abi keeps there are not used by anything, so their slots hold our magic and the object size. */
typedef struct {
    volatile LONG64 refs;            /* -0x80 */
    uint64_t type, dtor;             /* -0x78 std::type_info *, -0x70 void (*)(void *) */
    uint64_t magic, size;            /* -0x68, -0x60 */
    uint64_t next;                   /* -0x58 next older caught exception of the context (thrown object pointer) */
    int32_t handlers, selector;      /* -0x50 handler count (negative while rethrown), -0x4c */
    uint64_t action, lsda, catch_temp;
    uint64_t adjusted;               /* -0x30 what __cxa_begin_catch returns */
    uint64_t primary;                /* -0x28 dependent exception (std::rethrow_exception): the real object; else 0 */
    uint64_t uw_class, uw_cleanup, uw_private[2];        /* -0x20 _Unwind_Exception */
} ExcHeader;
#define EXC_MAGIC 0x43584345584e4e21ull
#define UW_CLASS 0x434c4e47432b2b00ull                   /* "CLNGC++\0"; +1 for a dependent exception */
static ExcHeader *exc_header(uint64_t obj) {
    if (!obj) return NULL;
    ExcHeader *h = (ExcHeader *)(uintptr_t)(obj - sizeof(ExcHeader));
    return cxxrt_readable(P64(h), sizeof *h) && h->magic == EXC_MAGIC ? h : NULL;
}
static uint64_t exc_object(uint64_t obj) { ExcHeader *h = exc_header(obj); return h && h->primary ? h->primary : obj; }   /* the C++ object */
uint64_t cxxrt_allocate_exception(size_t size) {
    ExcHeader *h = _aligned_malloc(sizeof *h + (size ? size : 1), 16);
    if (!h) { printf("\nC++ RUNTIME: out of memory allocating an exception of %llu bytes\n", (unsigned long long)size); report_and_exit(3); }
    memset(h, 0, sizeof *h + size);
    h->magic = EXC_MAGIC;
    h->size = size;
    h->uw_class = UW_CLASS;
    return P64(h + 1);
}
uint64_t eh_allocate(size_t size) { return cxxrt_allocate_exception(size); }
static void exc_free(uint64_t obj) {
    ExcHeader *h = exc_header(obj);
    if (h) { h->magic = 0; _aligned_free(h); }
}
void cxxrt_exception_addref(uint64_t obj) { ExcHeader *h = exc_header(obj); if (h) InterlockedIncrement64(&h->refs); }
void cxxrt_exception_release(Ctx *c, uint64_t obj) {
    ExcHeader *h = exc_header(obj);
    if (!h || InterlockedDecrement64(&h->refs) != 0) return;
    if (h->dtor) { uint64_t x0 = c->x[0]; cxxrt_call(c, h->dtor, obj, 0, 0); c->x[0] = x0; }
    exc_free(obj);
}
/* Makes a thrown-object record ready to be held by an exception_ptr (reference count 1). */
void cxxrt_exception_init(uint64_t obj, uint64_t type, uint64_t dtor) {
    ExcHeader *h = exc_header(obj);
    h->type = type; h->dtor = dtor; h->refs = 1;
}
const char *eh_type_name(uint64_t exception) { ExcHeader *h = exc_header(exception); return h ? cxxrt_type_name(h->type) : "?"; }

/* ---- per-context state: each guest thread and each guest fiber has its own Ctx */
typedef struct { Ctx *c; uint64_t in_flight, caught; int uncaught; } EhState;
/* 2026-10-13, the owner's freeze (ssbu_native_freeze.txt: main thread spinning in eh_state under eh_throw for 20 s at
 * game frame 33777). The state used to live in a table of 4,096 entries keyed by the ADDRESS of the Ctx, entries never
 * removed: every Ctx that ever threw at a new address (contexts on the stack, contexts of threads that came and went)
 * took an entry for ever, and with the table full the insert loop below never found a free slot and never ended.
 * The C++ ABI keeps this state per THREAD (__cxa_get_globals); a guest fiber is a Windows fiber here (os_threads.c), so
 * fiber-local storage is exactly "one state per guest thread and per guest fiber", needs no table, cannot fill up, and is
 * freed by the system when the thread or fiber ends. A context on the stack now shares the state of the fiber it runs
 * on, which is what a throw that crosses it needs. SSBU_EH_STATE=table restores the old table (bounded: it stops with a
 * report instead of spinning). */
#define EH_STATES 4096
static EhState g_eh_state[EH_STATES];
static SRWLOCK g_eh_state_lock = SRWLOCK_INIT;
static DWORD g_eh_fls = FLS_OUT_OF_INDEXES;
static volatile LONG g_eh_mode;                     /* 0 not decided, 1 fiber-local, 2 the old table */
static void WINAPI eh_fls_free(void *p) {            /* a thread or fiber ended: its list and states go with it */
    struct { int n, cap, clock; EhState **e; int *used; } *f = p;
    if (!f) return;
    for (int i = 0; i < f->n; i++) free(f->e[i]);
    free(f->e); free(f->used); free(f);
}
static EhState *eh_state_table(Ctx *c) {
    unsigned h = (unsigned)((P64(c) >> 6) * 2654435761u) % EH_STATES;
    for (int probe = 0; probe < EH_STATES; probe++, h = (h + 1) % EH_STATES) {
        Ctx *seen = g_eh_state[h].c;
        if (seen == c) return &g_eh_state[h];
        if (!seen) break;
    }
    AcquireSRWLockExclusive(&g_eh_state_lock);
    int probe = 0;
    for (; probe < EH_STATES; probe++, h = (h + 1) % EH_STATES) {
        if (g_eh_state[h].c == c) break;
        if (!g_eh_state[h].c) { g_eh_state[h].c = c; break; }
    }
    ReleaseSRWLockExclusive(&g_eh_state_lock);
    if (probe == EH_STATES) { printf("\nC++ RUNTIME: the exception-state table is full (%d contexts)\n", EH_STATES); report_and_exit(3); }
    return &g_eh_state[h];
}
static EhState *eh_state(Ctx *c) {
    if (!g_eh_mode) {
        AcquireSRWLockExclusive(&g_eh_state_lock);
        if (!g_eh_mode) {
            const char *e = getenv("SSBU_EH_STATE");
            if (e && !strcmp(e, "table")) g_eh_mode = 2;
            else { g_eh_fls = FlsAlloc(eh_fls_free); g_eh_mode = g_eh_fls != FLS_OUT_OF_INDEXES ? 1 : 2; }
        }
        ReleaseSRWLockExclusive(&g_eh_state_lock);
    }
    if (g_eh_mode == 2) return eh_state_table(c);
    /* A small list per host thread / fiber, keyed by the Ctx as before (cxxrt_selftest.h: "throw state belongs to the
     * Ctx"): private to the fiber, so no lock and no other thread can touch it. Each state is its own allocation (a
     * pointer handed out stays valid). Beyond EH_FIBER_KEEP entries an IDLE one (nothing in flight, nothing caught, no
     * uncaught count: indistinguishable from a fresh state) that was not among the last used is given to the new Ctx:
     * contexts on the stack and contexts long gone no longer accumulate. */
    typedef struct { int n, cap, clock; EhState **e; int *used; } EhFiber;
    enum { EH_FIBER_KEEP = 64 };
    EhFiber *f = (EhFiber *)FlsGetValue(g_eh_fls);
    if (!f) {
        f = (EhFiber *)calloc(1, sizeof *f);
        if (!f) { printf("\nC++ RUNTIME: no memory for the exception state of a thread\n"); report_and_exit(3); }
        FlsSetValue(g_eh_fls, f);
    }
    f->clock++;
    for (int i = 0; i < f->n; i++) if (f->e[i]->c == c) { f->used[i] = f->clock; return f->e[i]; }
    if (f->n >= EH_FIBER_KEEP) {
        int best = -1;
        for (int i = 0; i < f->n; i++) {
            EhState *s = f->e[i];
            if (!s->in_flight && !s->caught && !s->uncaught && f->clock - f->used[i] > 8 && (best < 0 || f->used[i] < f->used[best])) best = i;
        }
        if (best >= 0) { f->e[best]->c = c; f->used[best] = f->clock; return f->e[best]; }
    }
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 16;
        EhState **e = (EhState **)realloc(f->e, (size_t)cap * sizeof *e);
        int *u = e ? (int *)realloc(f->used, (size_t)cap * sizeof *u) : NULL;
        if (e) f->e = e;
        if (u) f->used = u;
        if (!e || !u) { printf("\nC++ RUNTIME: no memory for the exception states of a thread\n"); report_and_exit(3); }
        f->cap = cap;
    }
    EhState *s = (EhState *)calloc(1, sizeof *s);
    if (!s) { printf("\nC++ RUNTIME: no memory for an exception state\n"); report_and_exit(3); }
    s->c = c;
    f->e[f->n] = s; f->used[f->n] = f->clock; f->n++;
    return s;
}
uint64_t eh_in_flight(Ctx *c) { return eh_state(c)->in_flight; }
/* nnunwind.c, smashline's interpreter stub took exception `exc` (its landing code ran and returns to the interpreter's
 * caller): the exception is over for this context. The stub's code never calls __cxa_begin_catch / __cxa_end_catch, so the
 * context stayed "with an exception" for good (uncaught 1, never idle again, so never reused): HDR's script fibers throw
 * L2CExceptionShutdown this way many times a minute. `uncaught_before` = the count before the landing code ran. */
void eh_stub_done(Ctx *c, uint64_t exc, int uncaught_before) {
    EhState *st = eh_state(c);
    st->in_flight = 0;
    if (st->uncaught == uncaught_before && st->uncaught > 0) st->uncaught--;
    if (st->caught == exc) { ExcHeader *h = exc_header(exc); st->caught = h ? h->next : 0; }
}
int eh_uncaught_count(Ctx *c) { return eh_state(c)->uncaught; }

/* ---- loaded images */
typedef struct { uint64_t base, size, hdr, table; uint32_t count; } EhImage;
#define EH_IMAGES 256
static EhImage g_eh_image[EH_IMAGES];
static SRWLOCK g_eh_image_lock = SRWLOCK_INIT;

static uint64_t rd_uleb(uint64_t *p) { uint64_t r = 0; int s = 0; for (;;) { uint8_t b = rd8((*p)++); r |= (uint64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) return r; } }
static int64_t rd_sleb(uint64_t *p) {
    uint64_t r = 0; int s = 0;
    for (;;) { uint8_t b = rd8((*p)++); r |= (uint64_t)(b & 0x7f) << s; s += 7; if (!(b & 0x80)) { if ((b & 0x40) && s < 64) r |= ~0ull << s; return (int64_t)r; } }
}
/* DW_EH_PE encoded value at *p (guest memory, already relocated). A stored zero means "none". */
static uint64_t rd_enc(uint64_t *p, int enc, uint64_t datarel) {
    uint64_t at = *p, v;
    if (enc == 0xff) return 0;
    switch (enc & 0x0f) {
    case 0x00: case 0x04: case 0x0c: v = rd64(at); *p += 8; break;
    case 0x01: v = rd_uleb(p); break;
    case 0x09: v = (uint64_t)rd_sleb(p); break;
    case 0x02: v = rd16(at); *p += 2; break;
    case 0x0a: v = (uint64_t)(int64_t)(int16_t)rd16(at); *p += 2; break;
    case 0x03: v = rd32(at); *p += 4; break;
    case 0x0b: v = (uint64_t)(int64_t)(int32_t)rd32(at); *p += 4; break;
    default: return 0;
    }
    if (!v) return 0;
    if ((enc & 0x70) == 0x10) v += at;
    else if ((enc & 0x70) == 0x30) v += datarel;
    if (enc & 0x80) v = cxxrt_readable(v, 8) ? rd64(v) : 0;
    return v;
}
int eh_register_image(uint64_t base, uint64_t size, uint64_t hdr) {
    if (!hdr) {
        if (!cxxrt_readable(base, 16)) return 0;
        uint64_t mod0 = base + rd32(base + 4);
        if (!cxxrt_readable(mod0, 0x18) || rd32(mod0) != 0x30444f4d) return 0;       /* "MOD0" */
        hdr = mod0 + (uint64_t)(int64_t)(int32_t)rd32(mod0 + 0x10);
    }
    if (!cxxrt_readable(hdr, 12) || rd8(hdr) != 1 || rd8(hdr + 3) != 0x3b) return 0;      /* version 1, table of datarel sdata4 pairs */
    uint64_t p = hdr + 4;
    (void)rd_enc(&p, rd8(hdr + 1), hdr);                                              /* .eh_frame itself: not needed */
    uint64_t count = rd_enc(&p, rd8(hdr + 2), hdr);
    if (!count) return 0;
    AcquireSRWLockExclusive(&g_eh_image_lock);
    int slot = -1;
    for (int i = 0; i < EH_IMAGES; i++) {
        if (g_eh_image[i].base == base && g_eh_image[i].size) { slot = i; break; }
        if (slot < 0 && !g_eh_image[i].size) slot = i;
    }
    if (slot >= 0) { EhImage im = {base, size, hdr, p, (uint32_t)count}; g_eh_image[slot] = im; }
    ReleaseSRWLockExclusive(&g_eh_image_lock);
    return slot >= 0;
}
/* Resume entries of main: only a game DLL generated by recomp_eh.py exports game_eh_bind (game_shim_eh.c). */
typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } MainResume;
static const MainResume *g_main_resume;
static int g_main_resume_n;
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t));               /* nnunwind.c */
static void eh_bind_main(void) {
    HMODULE dll = NULL;
    typedef void (*BindFn)(void (*)(Ctx *, void (*)(Ctx *, uint64_t)), const MainResume **, int *);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&fn_table[0], &dll)) return;
    BindFn bind = (BindFn)(void *)GetProcAddress(dll, "game_eh_bind");
    if (bind) bind(nro_catch_call, &g_main_resume, &g_main_resume_n);
    trace_printf("nncxxrt: main %s\n", bind ? "has landing-pad resume entries" : "was lifted without resume entries (exceptions cannot be handled in main code)");
}
EhResumeFn eh_main_resume_entry(uint64_t lp) {
    int lo = 0, hi = g_main_resume_n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_main_resume[mid].lp == lp) return g_main_resume[mid].fn;
        if (g_main_resume[mid].lp < lp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}
void eh_unregister_image(uint64_t base) {
    AcquireSRWLockExclusive(&g_eh_image_lock);
    for (int i = 0; i < EH_IMAGES; i++) if (g_eh_image[i].base == base) memset(&g_eh_image[i], 0, sizeof g_eh_image[i]);
    ReleaseSRWLockExclusive(&g_eh_image_lock);
}
int eh_find_function(uint64_t pc, EhFunction *out) {
    EhImage im = {0};
    memset(out, 0, sizeof *out);
    AcquireSRWLockShared(&g_eh_image_lock);
    for (int i = 0; i < EH_IMAGES; i++)
        if (g_eh_image[i].size && pc >= g_eh_image[i].base && pc - g_eh_image[i].base < g_eh_image[i].size) { im = g_eh_image[i]; break; }
    ReleaseSRWLockShared(&g_eh_image_lock);
    if (!im.size) return 0;
    /* binary search table: { s32 initial location, s32 FDE address }, both relative to the header, sorted */
    int64_t rel = (int64_t)(pc - im.hdr);
    uint32_t lo = 0, hi = im.count;
    while (hi - lo > 1) { uint32_t mid = lo + (hi - lo) / 2; if ((int32_t)rd32(im.table + 8ull * mid) <= rel) lo = mid; else hi = mid; }
    if ((int32_t)rd32(im.table + 8ull * lo) > rel) return 0;
    uint64_t fde = im.hdr + (uint64_t)(int64_t)(int32_t)rd32(im.table + 8ull * lo + 4);
    uint64_t cie = fde + 4 - rd32(fde + 4);
    /* CIE: length, id, version, augmentation string, code/data alignment, return register, augmentation data */
    int lsda_enc = 0xff, fde_enc = 0, has_z = 0;
    uint64_t p = cie + 8;
    int version = rd8(p++);
    uint64_t aug = p;
    while (rd8(p)) p++;
    p++;
    (void)rd_uleb(&p); (void)rd_sleb(&p);
    if (version == 1) p++; else (void)rd_uleb(&p);
    if (rd8(aug) == 'z') {
        has_z = 1;
        (void)rd_uleb(&p);
        for (uint64_t a = aug + 1; rd8(a); a++) {
            if (rd8(a) == 'L') lsda_enc = rd8(p++);
            else if (rd8(a) == 'R') fde_enc = rd8(p++);
            else if (rd8(a) == 'P') { int e = rd8(p++); (void)rd_enc(&p, e & 0x7f, 0); }
        }
    }
    p = fde + 8;
    uint64_t start = rd_enc(&p, fde_enc, im.hdr), range = rd_enc(&p, fde_enc & 0x0f, 0);
    if (pc < start || pc - start >= range) return 0;
    out->image_base = im.base; out->start = start; out->end = start + range; out->fde = fde;
    if (has_z) { (void)rd_uleb(&p); if (lsda_enc != 0xff) out->lsda = rd_enc(&p, lsda_enc, im.hdr); }
    return 1;
}

/* ---- personality: the LSDA of the function, read as libc++abi's __gxx_personality_v0 reads it */
static int type_catches(uint64_t catch_ti, uint64_t thrown_ti, uint64_t obj, uint64_t *adjusted) {
    *adjusted = obj;
    if (!catch_ti || ti_same(catch_ti, thrown_ti)) return 1;                          /* catch (...) or the same type */
    uint64_t base = ti_upcast(thrown_ti, obj, catch_ti);                              /* a unique base class */
    if (base) { *adjusted = base; return 1; }
    return 0;
}
int eh_personality(Ctx *c, uint64_t call_pc, uint64_t exception, EhPad *out) {
    ExcHeader *h = exc_header(exception);
    (void)c;
    memset(out, 0, sizeof *out);
    out->exception = exception;
    if (!eh_find_function(call_pc, &out->function) || !out->function.lsda) return EH_NONE;
    uint64_t p = out->function.lsda, func = out->function.start;
    int lp_enc = rd8(p++);
    uint64_t lp_start = lp_enc != 0xff ? rd_enc(&p, lp_enc, 0) : func;
    int tt_enc = rd8(p++);
    uint64_t tt_base = 0;
    if (tt_enc != 0xff) { uint64_t off = rd_uleb(&p); tt_base = p + off; }
    int cs_enc = rd8(p++);
    uint64_t cs_len = rd_uleb(&p), cs_end = p + cs_len, lp = 0, action = 0;
    int found = 0;
    while (p < cs_end) {
        uint64_t s = rd_enc(&p, cs_enc & 0x0f, 0), l = rd_enc(&p, cs_enc & 0x0f, 0), pad = rd_enc(&p, cs_enc & 0x0f, 0), act = rd_uleb(&p);
        if (call_pc - func >= s && call_pc - func < s + l) { lp = pad; action = act; found = 1; break; }
        if (call_pc - func < s) break;
    }
    if (!found || !lp) return EH_NONE;
    out->landing_pad = lp_start + lp;
    out->adjusted = exc_object(exception);
    if (!action) { out->kind = EH_CLEANUP; return EH_CLEANUP; }
    uint64_t thrown = h ? h->type : 0, obj = exc_object(exception);
    int tt_size = (tt_enc & 0x0f) == 0x03 || (tt_enc & 0x0f) == 0x0b ? 4 : (tt_enc & 0x0f) == 0x02 || (tt_enc & 0x0f) == 0x0a ? 2 : 8;
    int cleanup = 0;
    for (uint64_t a = cs_end + action - 1;;) {
        uint64_t q = a;
        int64_t filter = rd_sleb(&q);
        uint64_t next_at = q;
        int64_t next = rd_sleb(&q);
        if (filter > 0) {                                                             /* catch clause */
            uint64_t tp = tt_base - (uint64_t)filter * (uint64_t)tt_size;
            uint64_t catch_ti = rd_enc(&tp, tt_enc, 0);
            if (!h && catch_ti) { /* an exception of unknown origin only matches catch (...) */ }
            else if (type_catches(catch_ti, thrown, obj, &out->adjusted)) { out->selector = filter; out->kind = EH_HANDLER; return EH_HANDLER; }
        } else if (filter < 0) {                                                      /* exception specification: a list of allowed types */
            uint64_t list = tt_base + (uint64_t)(-filter - 1), adj;
            int allowed = 0;
            for (;;) {
                uint64_t index = rd_uleb(&list);
                if (!index) break;
                uint64_t tp = tt_base - index * (uint64_t)tt_size;
                if (type_catches(rd_enc(&tp, tt_enc, 0), thrown, obj, &adj)) { allowed = 1; break; }
            }
            if (!allowed) { out->selector = filter; out->adjusted = obj; out->kind = EH_HANDLER; return EH_HANDLER; }
        } else cleanup = 1;
        if (!next) break;
        a = next_at + (uint64_t)next;
    }
    if (cleanup) { out->kind = EH_CLEANUP; return EH_CLEANUP; }
    out->landing_pad = 0;
    return EH_NONE;
}
void eh_enter_pad(Ctx *c, const EhPad *pad) {
    ExcHeader *h = exc_header(pad->exception);
    if (h) { h->adjusted = pad->adjusted; h->selector = (int32_t)pad->selector; h->lsda = pad->function.lsda; }
    eh_state(c)->in_flight = 0;
    c->x[0] = pad->exception - 0x20;
    c->x[1] = (uint64_t)pad->selector;
}

/* ---- terminate and raise */
static uint64_t g_ti_exception[2];
void eh_terminate(Ctx *c, const char *why) {
#ifdef _WIN32
    { void rg_dump_recent_loads(const char *w); rg_dump_recent_loads("the game is about to end (std::terminate)"); }
    { void rb_log_drain(void); rb_log_drain(); }
#endif
    EhState *st = eh_state(c);
    uint64_t exc = st->in_flight ? st->in_flight : st->caught;
    ExcHeader *h = exc_header(exc);
    uint64_t obj = exc_object(exc), type = h ? h->type : 0;
    st->in_flight = 0;
    printf("\nstd::terminate: %s (guest caller 0x%llx, last function f_%llx)\n", why, (unsigned long long)c->x[30] - 4, (unsigned long long)c->fn);
    trace_printf("std::terminate: %s (guest caller 0x%llx)\n", why, (unsigned long long)c->x[30] - 4);
    if (h) {
        const char *what = NULL;
        char extra[160] = "";
        printf("  exception type: %s\n", cxxrt_type_name(type));
        trace_printf("  exception type: %s\n", cxxrt_type_name(type));
        uint64_t base = ti_upcast(type, obj, P64(g_ti_exception));
        if (base && cxxrt_readable(base, 8) && cxxrt_readable(rd64(base), 0x18)) {
            uint64_t w = cxxrt_call(c, rd64(rd64(base) + 0x10), base, 0, 0);          /* virtual const char *what() const */
            if (cxxrt_readable(w, 1)) what = (const char *)(uintptr_t)w;
        } else {
            int n = snprintf(extra, sizeof extra, "  object (%llu bytes):", (unsigned long long)h->size);
            for (uint64_t i = 0; i + 8 <= h->size && i < 32; i += 8) n += snprintf(extra + n, sizeof extra - (size_t)n, " %016llx", (unsigned long long)rd64(obj + i));
            if (h->size < 8 && h->size >= 4) snprintf(extra + n, sizeof extra - (size_t)n, " %08x", rd32(obj));
        }
        if (what) { printf("  what(): %s\n", what); trace_printf("  what(): %s\n", what); }
        else if (extra[0]) { printf("%s (not derived from std::exception)\n", extra); trace_printf("%s\n", extra); }
    }
    cxxrt_print_guest_stack();
    report_and_exit(3);
}
static EhRaiseFn g_eh_raise;
void eh_set_unwinder(EhRaiseFn raise) { g_eh_raise = raise; }
static void eh_raise(Ctx *c, uint64_t exc, int reason, const char *how) {
    EhState *st = eh_state(c);
    st->in_flight = exc;
    if (g_eh_raise) g_eh_raise(c, reason);
    if (st->in_flight) {
        char why[160];
        snprintf(why, sizeof why, g_eh_raise ? "%s: no handler was found" : "%s: no unwinder is installed in this build, so no handler can run", how);
        eh_terminate(c, why);
    }
}
void eh_throw(Ctx *c, uint64_t exception, uint64_t type, uint64_t dtor) {
    ExcHeader *h = exc_header(exception);
    if (!h) { printf("\nC++ RUNTIME: throw of an object at 0x%llx that __cxa_allocate_exception did not create\n", (unsigned long long)exception); cxxrt_print_guest_stack(); report_and_exit(3); }
    h->type = type; h->dtor = dtor; h->refs = 1; h->handlers = 0; h->primary = 0; h->uw_class = UW_CLASS;
    eh_state(c)->uncaught++;
    eh_raise(c, exception, EH_RAISE_THROW, "uncaught exception");
}
/* Used by nncxxlib.c: throw an exception object prepared with cxxrt_exception_init, or stop when there is none. */
void cxxrt_throw_report(Ctx *c, const char *how, uint64_t obj, uint64_t type) {
    ExcHeader *h = exc_header(obj);
    if (h) eh_throw(c, obj, type, h->dtor);
    else eh_terminate(c, how);
}

static void abi_allocate_exception(Ctx *c) { RET(cxxrt_allocate_exception((size_t)A0)); }
static void abi_free_exception(Ctx *c) { exc_free(A0); }
static void abi_throw(Ctx *c) { eh_throw(c, A0, A1, A2); }              /* (void *obj, std::type_info *type, void (*dtor)(void *)) */
static void abi_begin_catch(Ctx *c) {                                   /* (_Unwind_Exception *) -> adjusted object pointer */
    uint64_t exc = A0 + 0x20;
    ExcHeader *h = exc_header(exc);
    EhState *st = eh_state(c);
    st->in_flight = 0;
    if (!h) { RET(exc); return; }                                       /* foreign exception: nothing to track */
    h->handlers = (h->handlers < 0 ? -h->handlers : h->handlers) + 1;   /* [sdk] a rethrown exception being caught again */
    if (st->caught != exc) { h->next = st->caught; st->caught = exc; }
    if (st->uncaught > 0) st->uncaught--;
    RET(h->adjusted ? h->adjusted : exc_object(exc));
}
static void exc_drop(Ctx *c, uint64_t exc) {                            /* the reference a throw holds */
    ExcHeader *h = exc_header(exc);
    if (h && h->primary) { uint64_t primary = h->primary; exc_free(exc); cxxrt_exception_release(c, primary); }
    else cxxrt_exception_release(c, exc);
}
static void abi_end_catch(Ctx *c) {
    EhState *st = eh_state(c);
    uint64_t exc = st->caught, x0 = c->x[0], x1 = c->x[1];             /* called from clean-up code too: keep x0/x1 */
    ExcHeader *h = exc_header(exc);
    if (!h) return;
    if (h->handlers < 0) {                                              /* rethrown: it stays alive for the new handler */
        if (++h->handlers == 0) st->caught = h->next;
    } else if (--h->handlers == 0) {
        st->caught = h->next;
        exc_drop(c, exc);
    }
    c->x[0] = x0; c->x[1] = x1;
}
static void abi_rethrow(Ctx *c) {
    EhState *st = eh_state(c);
    ExcHeader *h = exc_header(st->caught);
    if (!h) eh_terminate(c, "__cxa_rethrow with no exception being handled");
    h->handlers = -h->handlers;
    st->uncaught++;
    eh_raise(c, st->caught, EH_RAISE_THROW, "uncaught rethrown exception");
}
static void abi_unwind_resume(Ctx *c) { eh_raise(c, A0 + 0x20, EH_RAISE_RESUME, "_Unwind_Resume at the outermost frame"); }
static void abi_get_exception_ptr(Ctx *c) { ExcHeader *h = exc_header(A0 + 0x20); RET(h && h->adjusted ? h->adjusted : exc_object(A0 + 0x20)); }
static void abi_personality(Ctx *c) { eh_terminate(c, "__gxx_personality_v0 called directly (the port's unwinder uses eh_personality)"); }
static void abi_call_unexpected(Ctx *c) {                               /* (_Unwind_Exception *): std::unexpected -> terminate */
    abi_begin_catch(c);
    eh_terminate(c, "an exception violated an exception specification (__cxa_call_unexpected)");
}
static void abi_terminate(Ctx *c) { eh_terminate(c, "std::terminate() called"); }
static void abi_pure_virtual(Ctx *c) { fatal(c, "pure virtual function called"); }
static void abi_deleted_virtual(Ctx *c) { fatal(c, "deleted virtual function called"); }
static uint64_t g_ti_bad_typeid[3], g_ti_bad_cast[3], g_vt_bad_cast[5], g_vt_bad_typeid[5], g_vt_length_error[5], g_vt_out_of_range[5],
                g_ti_length_error[3], g_ti_out_of_range[3];
uint64_t cxxrt_refstring_new(const char *s, size_t n);
static void throw_simple(Ctx *c, uint64_t *vt, uint64_t *ti) {         /* an exception class that is only a vptr */
    uint64_t obj = cxxrt_allocate_exception(8);
    wr64(obj, P64(&vt[2]));
    eh_throw(c, obj, P64(ti), vt[2]);
}
static void throw_logic(Ctx *c, uint64_t *vt, uint64_t *ti, const char *msg) {      /* { vptr, refstring } */
    uint64_t obj = cxxrt_allocate_exception(16);
    wr64(obj, P64(&vt[2]));
    wr64(obj + 8, cxxrt_refstring_new(msg, strlen(msg)));
    eh_throw(c, obj, P64(ti), vt[2]);
}
static uint64_t g_vt_ios_failure[5], g_ti_ios_failure[3], g_ti_error_category_rt[2], g_vt_iostream_category[9], g_iostream_category[1];
/* std::ios_base::failure = system_error = { runtime_error { vptr, refstring }, error_code { int, const error_category * } };
 * libc++ throws it with make_error_code(io_errc::stream) = { 1, iostream_category() } */
void cxxrt_throw_ios_failure(Ctx *c, const char *msg) {
    char text[160];
    snprintf(text, sizeof text, "%s: unspecified iostream_category error", msg);
    uint64_t obj = cxxrt_allocate_exception(0x20);
    wr64(obj, P64(&g_vt_ios_failure[2]));
    wr64(obj + 8, cxxrt_refstring_new(text, strlen(text)));
    wr64(obj + 0x10, 1);
    wr64(obj + 0x18, P64(g_iostream_category));
    eh_throw(c, obj, P64(g_ti_ios_failure), g_vt_ios_failure[2]);
}
static void iocat_name(Ctx *c) { RET("iostream"); }
static void iocat_ret_this(Ctx *c) { RET(A0); }
static void iocat_condition(Ctx *c) { uint64_t self = A0; RET((uint32_t)A1); c->x[1] = self; }
static void iocat_equivalent_cond(Ctx *c) { RET(rd32(A2) == (uint32_t)A1 && rd64(A2 + 8) == A0); }
static void iocat_equivalent_code(Ctx *c) { RET(rd64(A1 + 8) == A0 && rd32(A1) == (uint32_t)A2); }
static void iocat_message(Ctx *c) { const char *m = (int32_t)A1 == 1 ? "unspecified iostream_category error" : "unspecified generic_category error"; cxx_string_init(c, c->x[8], m, strlen(m)); }
static void abi_bad_typeid(Ctx *c) { throw_simple(c, g_vt_bad_typeid, g_ti_bad_typeid); }
static void abi_bad_cast(Ctx *c) { throw_simple(c, g_vt_bad_cast, g_ti_bad_cast); }
/* libc++ __vector_base_common / __basic_string_common ::__throw_length_error / __throw_out_of_range */
static void throw_vector_length(Ctx *c) { throw_logic(c, g_vt_length_error, g_ti_length_error, "vector"); }
static void throw_vector_range(Ctx *c) { throw_logic(c, g_vt_out_of_range, g_ti_out_of_range, "vector"); }
static void throw_string_length(Ctx *c) { throw_logic(c, g_vt_length_error, g_ti_length_error, "basic_string"); }
static void throw_string_range(Ctx *c) { throw_logic(c, g_vt_out_of_range, g_ti_out_of_range, "basic_string"); }
static void abi_current_exception_type(Ctx *c) { ExcHeader *h = exc_header(eh_state(c)->caught); RET(h ? h->type : 0); }
static void abi_uncaught(Ctx *c) { RET(eh_state(c)->uncaught > 0); }                /* std::uncaught_exception() */
static void abi_uncaught_count(Ctx *c) { RET((int64_t)eh_state(c)->uncaught); }     /* std::uncaught_exceptions() */

/* std::exception_ptr: one pointer to the thrown object (null = empty); it shares the object's reference count */
static void eptr_copy(Ctx *c) { uint64_t p = rd64(A1); wr64(A0, p); cxxrt_exception_addref(p); }
static void eptr_dtor(Ctx *c) { uint64_t self = A0; cxxrt_exception_release(c, rd64(self)); RET(self); }
static void eptr_assign(Ctx *c) {
    uint64_t self = A0, other = rd64(A1), old = rd64(self);
    if (old != other) { cxxrt_exception_addref(other); wr64(self, other); cxxrt_exception_release(c, old); }
    RET(self);
}
static void eptr_rethrow(Ctx *c) {                      /* std::rethrow_exception(exception_ptr): passed by reference (non-trivial type) */
    uint64_t obj = cxxrt_readable(A0, 8) ? rd64(A0) : 0;
    ExcHeader *h = exc_header(obj);
    if (!h) eh_terminate(c, "std::rethrow_exception of an empty exception_ptr");
    /* libc++abi: a dependent exception -- its own header (handler count, adjusted pointer) referring to the object */
    uint64_t dep = cxxrt_allocate_exception(0);
    ExcHeader *d = exc_header(dep);
    cxxrt_exception_addref(obj);
    d->type = h->type; d->primary = obj; d->uw_class = UW_CLASS + 1; d->refs = 1;
    eh_state(c)->uncaught++;
    eh_raise(c, dep, EH_RAISE_THROW, "uncaught exception from std::rethrow_exception");
}
static void eptr_current(Ctx *c) {                      /* std::current_exception(): result through x8 */
    uint64_t obj = exc_object(eh_state(c)->caught);
    cxxrt_exception_addref(obj);
    wr64(c->x[8], obj);
}

/* ---------------------------------------------------------------- guard variables (thread-safe static initialisation)
 * Itanium ABI: the first byte of the 64-bit guard is 1 once the object is initialised (the game tests it inline).
 * As in libc++abi the second byte marks "initialisation in progress" and the upper half records the initialising
 * thread, so other threads wait and a recursive initialisation is reported instead of deadlocking. */
static CRITICAL_SECTION g_guard_lock;
static CONDITION_VARIABLE g_guard_cv;
static INIT_ONCE g_guard_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK guard_init(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    InitializeCriticalSection(&g_guard_lock);
    InitializeConditionVariable(&g_guard_cv);
    return TRUE;
}
static void abi_guard_acquire(Ctx *c) {
    uint64_t g = A0;
    int r;
    InitOnceExecuteOnce(&g_guard_once, guard_init, NULL, NULL);
    EnterCriticalSection(&g_guard_lock);
    for (;;) {
        if (rd8(g)) { r = 0; break; }
        if (!rd8(g + 1)) { wr8(g + 1, 1); wr32(g + 4, GetCurrentThreadId()); r = 1; break; }
        if (rd32(g + 4) == GetCurrentThreadId()) {
            LeaveCriticalSection(&g_guard_lock);
            fatal(c, "recursive initialisation of the static object guarded at 0x%llx", (unsigned long long)g);
        }
        SleepConditionVariableCS(&g_guard_cv, &g_guard_lock, INFINITE);
    }
    LeaveCriticalSection(&g_guard_lock);
    RET(r);
}
static void guard_finish(uint64_t g, int done) {
    InitOnceExecuteOnce(&g_guard_once, guard_init, NULL, NULL);
    EnterCriticalSection(&g_guard_lock);
    wr32(g + 4, 0);
    wr8(g + 1, 0);
    if (done) { MemoryBarrier(); wr8(g, 1); }
    WakeAllConditionVariable(&g_guard_cv);
    LeaveCriticalSection(&g_guard_lock);
}
static void abi_guard_release(Ctx *c) { guard_finish(A0, 1); }
static void abi_guard_abort(Ctx *c) { guard_finish(A0, 0); }

/* ---------------------------------------------------------------- standard exception classes
 * [sdk] std::exception = { vptr }; vtable: ~D1, ~D0, what. std::logic_error / runtime_error = { vptr, __libcpp_refstring }
 * where the refstring is a pointer to the characters, preceded by { size_t len, size_t cap, int count } (count = extra
 * references; the destructor frees the block at chars - 0x18 when the atomic decrement goes negative). */
static uint64_t g_vt_exception[5], g_vt_bad_alloc[5], g_vt_bad_cast[5], g_vt_bad_typeid[5], g_vt_logic_error[5],
                g_vt_out_of_range[5], g_vt_length_error[5], g_vt_invalid_argument[5], g_vt_runtime_error[5];
static uint64_t g_ti_logic_error[3], g_ti_runtime_error[3], g_ti_out_of_range[3], g_ti_length_error[3], g_ti_invalid_argument[3],
                g_ti_bad_alloc[3], g_ti_system_error[3], g_ti_ios_failure[3], g_ti_streambuf[2], g_ti_shared_count[2],
                g_ti_shared_weak_count[5], g_ti_bool[2], g_ti_float[2], g_ti_int[2], g_ti_uint[2];
uint64_t cxxrt_ti_logic_error(void) { return P64(g_ti_logic_error); }
uint64_t cxxrt_ti_exception(void) { return P64(g_ti_exception); }
uint64_t cxxrt_vt_logic_error(void) { return P64(&g_vt_logic_error[2]); }
uint64_t cxxrt_class_ti_vptr(int kind) { return kind == TI_SI ? P64(&g_vt_si[2]) : kind == TI_VMI ? P64(&g_vt_vmi[2]) : P64(&g_vt_class[2]); }

uint64_t cxxrt_refstring_new(const char *s, size_t n) {
    uint64_t *rep = malloc(0x18 + n + 1);
    rep[0] = n; rep[1] = n; rep[2] = 0;
    memcpy(rep + 3, s, n);
    ((char *)(rep + 3))[n] = 0;
    return P64(rep + 3);
}
static void refstring_addref(uint64_t s) { if (s) InterlockedIncrement((volatile LONG *)(uintptr_t)(s - 8)); }
static void refstring_release(uint64_t s) { if (s && InterlockedDecrement((volatile LONG *)(uintptr_t)(s - 8)) < 0) free((void *)(uintptr_t)(s - 0x18)); }

static void exc_dtor(Ctx *c) { RET(A0); }                                      /* ~exception, ~bad_alloc, ~bad_cast (D1/D2) */
static void exc_dtor_delete(Ctx *c) { cxxrt_guest_delete(c, A0); }             /* D0 */
static void exc_what(Ctx *c) { RET("std::exception"); }
static void bad_alloc_ctor(Ctx *c) { wr64(A0, P64(&g_vt_bad_alloc[2])); }
static void bad_alloc_what(Ctx *c) { RET("std::bad_alloc"); }
static void bad_cast_ctor(Ctx *c) { wr64(A0, P64(&g_vt_bad_cast[2])); }
static void bad_cast_what(Ctx *c) { RET("std::bad_cast"); }
static void bad_typeid_what(Ctx *c) { RET("std::bad_typeid"); }
static void logic_ctor_cstr(Ctx *c) {
    uint64_t self = A0;
    const char *s = (const char *)(uintptr_t)A1;
    wr64(self, P64(&g_vt_logic_error[2]));
    wr64(self + 8, cxxrt_refstring_new(s, strlen(s)));
    RET(self);
}
static void logic_ctor_string(Ctx *c) {
    uint64_t self = A0;
    size_t n;
    const char *s = cxx_string_data(A1, &n);
    wr64(self, P64(&g_vt_logic_error[2]));
    wr64(self + 8, cxxrt_refstring_new(s, n));
    RET(self);
}
static void logic_ctor_copy(Ctx *c) {
    uint64_t self = A0, s = rd64(A1 + 8);
    wr64(self, P64(&g_vt_logic_error[2]));
    wr64(self + 8, s);
    refstring_addref(s);
    RET(self);
}
static void logic_dtor(Ctx *c) { uint64_t self = A0; refstring_release(rd64(self + 8)); wr64(self + 8, 0); RET(self); }
static void logic_dtor_delete(Ctx *c) { uint64_t self = A0; refstring_release(rd64(self + 8)); cxxrt_guest_delete(c, self); }
static void logic_what(Ctx *c) { RET(rd64(A0 + 8)); }
static void ti_virtual_trap(Ctx *c) { fatal(c, "a virtual function of a std::type_info object was called (%s)", hostfn_current_name()); }

static void make_vtable(uint64_t *vt, uint64_t *ti, const char *cls, ImportFn d1, ImportFn d0, ImportFn what) {
    char name[72];
    vt[0] = 0;
    vt[1] = P64(ti);
    snprintf(name, sizeof name, "%s::~%s [D1]", cls, cls); vt[2] = hostfn_address(name, d1);
    snprintf(name, sizeof name, "%s::~%s [D0]", cls, cls); vt[3] = hostfn_address(name, d0);
    snprintf(name, sizeof name, "%s::what", cls); vt[4] = hostfn_address(name, what);
}
static void make_ti(uint64_t *ti, int kind, const char *name, uint64_t *base) {
    ti[0] = kind == TI_SI ? P64(&g_vt_si[2]) : kind == TI_VMI ? P64(&g_vt_vmi[2]) : kind == TI_CLASS ? P64(&g_vt_class[2]) : P64(&g_vt_fund[2]);
    ti[1] = P64(name);
    if (kind == TI_SI) ti[2] = P64(base);
}

void cxxrt_install_data(void) {
    static const struct { uint64_t *vt; const char *name; } kinds[] = {
        {g_vt_class, "__class_type_info"}, {g_vt_si, "__si_class_type_info"}, {g_vt_vmi, "__vmi_class_type_info"},
        {g_vt_fund, "__fundamental_type_info"}, {g_vt_ptr, "__pointer_type_info"},
    };
    for (int k = 0; k < 5; k++)
        for (int i = 2; i < 12; i++) {
            char name[72];
            snprintf(name, sizeof name, "%s virtual #%d", kinds[k].name, i - 2);
            kinds[k].vt[i] = hostfn_address(name, ti_virtual_trap);
        }
    import_set_address("_ZTVN10__cxxabiv117__class_type_infoE", g_vt_class);
    import_set_address("_ZTVN10__cxxabiv120__si_class_type_infoE", g_vt_si);
    import_set_address("_ZTVN10__cxxabiv121__vmi_class_type_infoE", g_vt_vmi);
    import_set_address("_ZTVN10__cxxabiv123__fundamental_type_infoE", g_vt_fund);
    import_set_address("_ZTVN10__cxxabiv119__pointer_type_infoE", g_vt_ptr);

    /* [sdk] the hierarchy and the name strings are those of the SDK's typeinfo objects */
    make_ti(g_ti_exception, TI_CLASS, "St9exception", NULL);
    make_ti(g_ti_logic_error, TI_SI, "St11logic_error", g_ti_exception);
    make_ti(g_ti_runtime_error, TI_SI, "St13runtime_error", g_ti_exception);
    make_ti(g_ti_out_of_range, TI_SI, "St12out_of_range", g_ti_logic_error);
    make_ti(g_ti_length_error, TI_SI, "St12length_error", g_ti_logic_error);
    make_ti(g_ti_invalid_argument, TI_SI, "St16invalid_argument", g_ti_logic_error);
    make_ti(g_ti_bad_alloc, TI_SI, "St9bad_alloc", g_ti_exception);
    make_ti(g_ti_bad_cast, TI_SI, "St8bad_cast", g_ti_exception);
    make_ti(g_ti_bad_typeid, TI_SI, "St10bad_typeid", g_ti_exception);
    make_ti(g_ti_system_error, TI_SI, "NSt3__112system_errorE", g_ti_runtime_error);
    make_ti(g_ti_ios_failure, TI_SI, "NSt3__18ios_base7failureE", g_ti_system_error);
    make_ti(g_ti_streambuf, TI_CLASS, "NSt3__115basic_streambufIcNS_11char_traitsIcEEEE", NULL);
    make_ti(g_ti_shared_count, TI_CLASS, "NSt3__114__shared_countE", NULL);
    make_ti(g_ti_shared_weak_count, TI_VMI, "NSt3__119__shared_weak_countE", NULL);
    g_ti_shared_weak_count[2] = 0x0000000100000000ull;                 /* flags 0, one base */
    g_ti_shared_weak_count[3] = P64(g_ti_shared_count);
    g_ti_shared_weak_count[4] = 0;                                     /* offset 0, not public, not virtual */
    make_ti(g_ti_bool, TI_OTHER, "b", NULL); make_ti(g_ti_float, TI_OTHER, "f", NULL);
    make_ti(g_ti_int, TI_OTHER, "i", NULL); make_ti(g_ti_uint, TI_OTHER, "j", NULL);
    import_set_address("_ZTISt9exception", g_ti_exception);
    import_set_address("_ZTISt11logic_error", g_ti_logic_error);
    import_set_address("_ZTISt13runtime_error", g_ti_runtime_error);
    import_set_address("_ZTISt12out_of_range", g_ti_out_of_range);
    import_set_address("_ZTISt12length_error", g_ti_length_error);
    import_set_address("_ZTISt16invalid_argument", g_ti_invalid_argument);
    import_set_address("_ZTISt9bad_alloc", g_ti_bad_alloc);
    import_set_address("_ZTISt8bad_cast", g_ti_bad_cast);
    import_set_address("_ZTISt10bad_typeid", g_ti_bad_typeid);
    import_set_address("_ZTINSt3__112system_errorE", g_ti_system_error);
    import_set_address("_ZTINSt3__18ios_base7failureE", g_ti_ios_failure);
    import_set_address("_ZTINSt3__115basic_streambufIcNS_11char_traitsIcEEEE", g_ti_streambuf);
    import_set_address("_ZTINSt3__119__shared_weak_countE", g_ti_shared_weak_count);
    import_set_address("_ZTIb", g_ti_bool); import_set_address("_ZTIf", g_ti_float);
    import_set_address("_ZTIi", g_ti_int); import_set_address("_ZTIj", g_ti_uint);

    /* [sdk] _ZTVSt9exception / _ZTVSt12out_of_range / _ZTVSt12length_error: { 0, typeinfo, D1, D0, what } */
    make_vtable(g_vt_exception, g_ti_exception, "std::exception", exc_dtor, exc_dtor_delete, exc_what);
    make_vtable(g_vt_bad_alloc, g_ti_bad_alloc, "std::bad_alloc", exc_dtor, exc_dtor_delete, bad_alloc_what);
    make_vtable(g_vt_bad_cast, g_ti_bad_cast, "std::bad_cast", exc_dtor, exc_dtor_delete, bad_cast_what);
    make_vtable(g_vt_bad_typeid, g_ti_bad_typeid, "std::bad_typeid", exc_dtor, exc_dtor_delete, bad_typeid_what);
    make_vtable(g_vt_logic_error, g_ti_logic_error, "std::logic_error", logic_dtor, logic_dtor_delete, logic_what);
    make_vtable(g_vt_runtime_error, g_ti_runtime_error, "std::runtime_error", logic_dtor, logic_dtor_delete, logic_what);
    make_vtable(g_vt_out_of_range, g_ti_out_of_range, "std::out_of_range", logic_dtor, logic_dtor_delete, logic_what);
    make_vtable(g_vt_length_error, g_ti_length_error, "std::length_error", logic_dtor, logic_dtor_delete, logic_what);
    make_vtable(g_vt_ios_failure, g_ti_ios_failure, "std::ios_base::failure", logic_dtor, logic_dtor_delete, logic_what);
    make_ti(g_ti_error_category_rt, TI_CLASS, "NSt3__114error_categoryE", NULL);
    g_vt_iostream_category[0] = 0; g_vt_iostream_category[1] = P64(g_ti_error_category_rt);
    g_vt_iostream_category[2] = hostfn_address("iostream_category::~ [D1]", iocat_ret_this);
    g_vt_iostream_category[3] = hostfn_address("iostream_category::~ [D0]", iocat_ret_this);
    g_vt_iostream_category[4] = hostfn_address("iostream_category::name", iocat_name);
    g_vt_iostream_category[5] = hostfn_address("iostream_category::default_error_condition", iocat_condition);
    g_vt_iostream_category[6] = hostfn_address("iostream_category::equivalent(int, error_condition)", iocat_equivalent_cond);
    g_vt_iostream_category[7] = hostfn_address("iostream_category::equivalent(error_code, int)", iocat_equivalent_code);
    g_vt_iostream_category[8] = hostfn_address("iostream_category::message", iocat_message);
    g_iostream_category[0] = P64(&g_vt_iostream_category[2]);
    make_vtable(g_vt_invalid_argument, g_ti_invalid_argument, "std::invalid_argument", logic_dtor, logic_dtor_delete, logic_what);
    import_set_address("_ZTVSt9exception", g_vt_exception);
    import_set_address("_ZTVSt9bad_alloc", g_vt_bad_alloc);
    import_set_address("_ZTVSt8bad_cast", g_vt_bad_cast);
    import_set_address("_ZTVSt11logic_error", g_vt_logic_error);
    import_set_address("_ZTVSt13runtime_error", g_vt_runtime_error);
    import_set_address("_ZTVSt12out_of_range", g_vt_out_of_range);
    import_set_address("_ZTVSt12length_error", g_vt_length_error);
    import_set_address("_ZTVSt16invalid_argument", g_vt_invalid_argument);

    libc2_install_data();
    cxxlib_install_data();
}

/* Replace what an earlier file registered: these were placeholders (not thread-safe guards in boot.c). */
static const ImportDef cxxrt_replace[] = {
    {"__cxa_guard_acquire", abi_guard_acquire}, {"__cxa_guard_release", abi_guard_release}, {"__cxa_guard_abort", abi_guard_abort},
    /* libcxx.c had these as traps: they construct and throw std::length_error / std::out_of_range */
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", throw_vector_length},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv", throw_vector_range},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", throw_string_length},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv", throw_string_range},
};
/* Installed only where nothing else provides the function. */
static const ImportDef cxxrt_fill[] = {
    {"__cxa_allocate_exception", abi_allocate_exception}, {"__cxa_free_exception", abi_free_exception}, {"__cxa_throw", abi_throw},
    {"__cxa_begin_catch", abi_begin_catch}, {"__cxa_end_catch", abi_end_catch}, {"__cxa_rethrow", abi_rethrow},
    {"_Unwind_Resume", abi_unwind_resume}, {"__gxx_personality_v0", abi_personality}, {"__cxa_call_unexpected", abi_call_unexpected},
    {"_ZSt9terminatev", abi_terminate}, {"__cxa_pure_virtual", abi_pure_virtual}, {"__cxa_deleted_virtual", abi_deleted_virtual},
    {"__cxa_bad_typeid", abi_bad_typeid}, {"__cxa_bad_cast", abi_bad_cast}, {"__dynamic_cast", abi_dynamic_cast},
    {"__cxa_current_exception_type", abi_current_exception_type}, {"_ZSt18uncaught_exceptionv", abi_uncaught}, {"_ZSt19uncaught_exceptionsv", abi_uncaught_count},
    {"__cxa_get_exception_ptr", abi_get_exception_ptr},
    {"_ZNSt13exception_ptrC1ERKS_", eptr_copy}, {"_ZNSt13exception_ptrC2ERKS_", eptr_copy}, {"_ZNSt13exception_ptrD1Ev", eptr_dtor},
    {"_ZNSt13exception_ptrD2Ev", eptr_dtor}, {"_ZNSt13exception_ptraSERKS_", eptr_assign},
    {"_ZSt17rethrow_exceptionSt13exception_ptr", eptr_rethrow}, {"_ZSt17current_exceptionv", eptr_current},
    {"_ZNSt9exceptionD0Ev", exc_dtor_delete}, {"_ZNSt9exceptionD1Ev", exc_dtor}, {"_ZNSt9exceptionD2Ev", exc_dtor}, {"_ZNKSt9exception4whatEv", exc_what},
    {"_ZNSt9bad_allocC1Ev", bad_alloc_ctor}, {"_ZNSt9bad_allocC2Ev", bad_alloc_ctor}, {"_ZNSt9bad_allocD1Ev", exc_dtor}, {"_ZNSt9bad_allocD2Ev", exc_dtor},
    {"_ZNKSt9bad_alloc4whatEv", bad_alloc_what},
    {"_ZNSt8bad_castC1Ev", bad_cast_ctor}, {"_ZNSt8bad_castC2Ev", bad_cast_ctor}, {"_ZNSt8bad_castD1Ev", exc_dtor}, {"_ZNSt8bad_castD2Ev", exc_dtor},
    {"_ZNSt11logic_errorC1EPKc", logic_ctor_cstr}, {"_ZNSt11logic_errorC2EPKc", logic_ctor_cstr},
    {"_ZNSt11logic_errorC1ERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE", logic_ctor_string},
    {"_ZNSt11logic_errorC2ERKNSt3__112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE", logic_ctor_string},
    {"_ZNSt11logic_errorC1ERKS_", logic_ctor_copy}, {"_ZNSt11logic_errorC2ERKS_", logic_ctor_copy},
    {"_ZNSt11logic_errorD1Ev", logic_dtor}, {"_ZNSt11logic_errorD2Ev", logic_dtor}, {"_ZNKSt11logic_error4whatEv", logic_what},
    {"_ZNSt12out_of_rangeD1Ev", logic_dtor}, {"_ZNSt12out_of_rangeD2Ev", logic_dtor}, {"_ZNSt12out_of_rangeD0Ev", logic_dtor_delete},
    {"_ZNSt12length_errorD1Ev", logic_dtor}, {"_ZNSt12length_errorD2Ev", logic_dtor}, {"_ZNSt12length_errorD0Ev", logic_dtor_delete},
    {"_ZNSt16invalid_argumentD1Ev", logic_dtor}, {"_ZNSt16invalid_argumentD2Ev", logic_dtor},
    {"_ZNSt13runtime_errorD1Ev", logic_dtor}, {"_ZNSt13runtime_errorD2Ev", logic_dtor}, {"_ZNKSt13runtime_error4whatEv", logic_what},
};

static void cxxrt_selftest(void);

void register_cxxrt_imports(void) {
    register_imports(cxxrt_replace, (int)(sizeof cxxrt_replace / sizeof cxxrt_replace[0]));
    for (int i = 0; i < (int)(sizeof cxxrt_fill / sizeof cxxrt_fill[0]); i++)
        if (!import_lookup(cxxrt_fill[i].name)) register_imports(&cxxrt_fill[i], 1);
    register_cxxlib_imports();
    register_libc2_imports();
    if (!eh_register_image(GUEST_BASE, 0x7500000ull, 0)) printf("nncxxrt: the exception tables of main were not found\n");
    eh_bind_main();
    const char *path = getenv("SSBU_DUMP_UNREG_FINAL");      /* every import that has no handler after ALL registrations */
    if (path && path[0]) {
        FILE *f = fopen(path, "w");
        for (int i = 0; f && i < dyn_sym_count; i++)
            if (!dyn_syms[i].defined && dyn_syms[i].name[0] && !import_lookup(dyn_syms[i].name)) fprintf(f, "%d %s\n", (int)(import_address(i) != IMPORT_BASE + (uint64_t)i * IMPORT_SLOT), dyn_syms[i].name);
        if (f) fclose(f);
    }
    if (getenv("SSBU_CXXRT_SELFTEST")) cxxrt_selftest();
}

#include "cxxrt_selftest.h"
