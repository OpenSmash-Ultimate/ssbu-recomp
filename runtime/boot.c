/* Boot driver: runs the game's own start-up code (nninitStartup, static
 * constructors, nnMain) on the lifted binary and records which functions of
 * other modules it asks for, in order. Unimplemented imports return 0. */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include <windows.h>
#include <psapi.h>
#include <time.h>
#include "runtime.h"
#include "hostrt.h"

#define NNINIT_STARTUP GA(GUEST_BASE + 0x2c55e0ULL)
#define NN_MAIN GA(GUEST_BASE + 0x2c5620ULL)
#define INIT_ARRAY (GUEST_BASE + g_main.init_array)       /* from the dynamic section (hostrt.c); 13.0.4: 0x52a7be0 */
#define INIT_ARRAY_SIZE g_main.init_arraysz                 /* 13.0.4: 0x990 */

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(n) ((void *)(uintptr_t)c->x[n])

/* ---- C heap */
static void i_malloc(Ctx *c) { RET(_aligned_malloc(A0 ? (size_t)A0 : 1, 16)); }
static void i_free(Ctx *c) { _aligned_free(P(0)); }
static void i_calloc(Ctx *c) { size_t n = (size_t)(A0 * A1); void *p = _aligned_malloc(n ? n : 1, 16); if (p) memset(p, 0, n); RET(p); }
static void i_realloc(Ctx *c) { RET(_aligned_realloc(P(0), A1 ? (size_t)A1 : 1, 16)); }
static void i_aligned_alloc(Ctx *c) { RET(_aligned_malloc(A1 ? (size_t)A1 : 1, A0 < 16 ? 16 : (size_t)A0)); }
static void i_new_aligned(Ctx *c) { RET(_aligned_malloc(A0 ? (size_t)A0 : 1, A1 < 16 ? 16 : (size_t)A1)); }
static void i_posix_memalign(Ctx *c) {
    void *p = _aligned_malloc(A2 ? (size_t)A2 : 1, A1 < 16 ? 16 : (size_t)A1);
    wr64(A0, (uint64_t)p);
    RET(p ? 0 : 12);
}
static void i_usable_size(Ctx *c) { RET(A0 ? _aligned_msize(P(0), 16, 0) : 0); }

/* ---- C strings and memory */
static void i_memcpy(Ctx *c) { memcpy(P(0), P(1), (size_t)A2); }
static void i_memmove(Ctx *c) { memmove(P(0), P(1), (size_t)A2); }
static void i_memset(Ctx *c) { memset(P(0), (int)A1, (size_t)A2); }
static void i_memcmp(Ctx *c) { RET((int64_t)memcmp(P(0), P(1), (size_t)A2)); }
static void i_memchr(Ctx *c) { RET(memchr(P(0), (int)A1, (size_t)A2)); }
static void i_strlen(Ctx *c) { RET(strlen(P(0))); }
static void i_strnlen(Ctx *c) { RET(strnlen(P(0), (size_t)A1)); }
static void i_strcmp(Ctx *c) { RET((int64_t)strcmp(P(0), P(1))); }
static void i_strncmp(Ctx *c) { RET((int64_t)strncmp(P(0), P(1), (size_t)A2)); }
static void i_strcpy(Ctx *c) { strcpy(P(0), P(1)); }
static void i_strncpy(Ctx *c) { strncpy(P(0), P(1), (size_t)A2); }
static void i_strcat(Ctx *c) { strcat(P(0), P(1)); }
static void i_strchr(Ctx *c) { RET(strchr(P(0), (int)A1)); }
static void i_strrchr(Ctx *c) { RET(strrchr(P(0), (int)A1)); }
static void i_strstr(Ctx *c) { RET(strstr(P(0), P(1))); }

/* ---- C++ runtime */
static void i_guard_acquire(Ctx *c) { RET(rd8(A0) == 0); }
static void i_guard_release(Ctx *c) { wr8(A0, 1); }
static void i_ret0(Ctx *c) { RET(0); }
static void i_abort(Ctx *c) { guest_trap("guest called abort()", c->x[30] - 4); }

/* ---- math: arguments and result in v0/v1 */
#define M1F(n, fn) static void n(Ctx *c) { float r = fn(c->v[0].f[0]); vzero(&c->v[0]); c->v[0].f[0] = r; }
#define M2F(n, fn) static void n(Ctx *c) { float r = fn(c->v[0].f[0], c->v[1].f[0]); vzero(&c->v[0]); c->v[0].f[0] = r; }
#define M1D(n, fn) static void n(Ctx *c) { double r = fn(c->v[0].df[0]); vzero(&c->v[0]); c->v[0].df[0] = r; }
#define M2D(n, fn) static void n(Ctx *c) { double r = fn(c->v[0].df[0], c->v[1].df[0]); vzero(&c->v[0]); c->v[0].df[0] = r; }
M1F(m_sinf, sinf) M1F(m_cosf, cosf) M1F(m_tanf, tanf) M1F(m_asinf, asinf) M1F(m_acosf, acosf) M1F(m_atanf, atanf)
M1F(m_expf, expf) M1F(m_exp2f, exp2f) M1F(m_logf, logf) M1F(m_log2f, log2f) M1F(m_log10f, log10f) M1F(m_sqrtf, sqrtf)
M1F(m_floorf, floorf) M1F(m_ceilf, ceilf) M1F(m_roundf, roundf) M1F(m_truncf, truncf) M1F(m_fabsf, fabsf)
M2F(m_atan2f, atan2f) M2F(m_powf, powf) M2F(m_fmodf, fmodf)
M1D(m_sin, sin) M1D(m_cos, cos) M1D(m_tan, tan) M1D(m_asin, asin) M1D(m_acos, acos) M1D(m_atan, atan)
M1D(m_exp, exp) M1D(m_log, log) M1D(m_log2, log2) M1D(m_log10, log10) M1D(m_sqrt, sqrt)
M1D(m_floor, floor) M1D(m_ceil, ceil) M1D(m_round, round) M1D(m_trunc, trunc) M1D(m_fabs, fabs)
M2D(m_atan2, atan2) M2D(m_pow, pow) M2D(m_fmod, fmod)

static const ImportDef base_imports[] = {
    {"malloc", i_malloc}, {"free", i_free}, {"calloc", i_calloc}, {"realloc", i_realloc},
    {"aligned_alloc", i_aligned_alloc}, {"memalign", i_aligned_alloc}, {"posix_memalign", i_posix_memalign},
    {"malloc_usable_size", i_usable_size},
    {"_Znwm", i_malloc}, {"_Znam", i_malloc}, {"_ZdlPv", i_free}, {"_ZdaPv", i_free},
    {"_ZnwmRKSt9nothrow_t", i_malloc}, {"_ZnamRKSt9nothrow_t", i_malloc},
    {"_ZdlPvm", i_free}, {"_ZdaPvm", i_free}, {"_ZdlPvRKSt9nothrow_t", i_free}, {"_ZdaPvRKSt9nothrow_t", i_free},
    {"_ZnwmSt11align_val_t", i_new_aligned}, {"_ZnamSt11align_val_t", i_new_aligned},
    {"_ZdlPvSt11align_val_t", i_free}, {"_ZdaPvSt11align_val_t", i_free},
    {"memcpy", i_memcpy}, {"memmove", i_memmove}, {"memset", i_memset}, {"memcmp", i_memcmp}, {"memchr", i_memchr},
    {"strlen", i_strlen}, {"strnlen", i_strnlen}, {"strcmp", i_strcmp}, {"strncmp", i_strncmp}, {"strcpy", i_strcpy},
    {"strncpy", i_strncpy}, {"strcat", i_strcat}, {"strchr", i_strchr}, {"strrchr", i_strrchr}, {"strstr", i_strstr},
    {"__cxa_guard_acquire", i_guard_acquire}, {"__cxa_guard_release", i_guard_release}, {"__cxa_guard_abort", i_ret0},
    {"__cxa_atexit", i_ret0}, {"abort", i_abort},
    {"sinf", m_sinf}, {"cosf", m_cosf}, {"tanf", m_tanf}, {"asinf", m_asinf}, {"acosf", m_acosf}, {"atanf", m_atanf},
    {"expf", m_expf}, {"exp2f", m_exp2f}, {"logf", m_logf}, {"log2f", m_log2f}, {"log10f", m_log10f}, {"sqrtf", m_sqrtf},
    {"floorf", m_floorf}, {"ceilf", m_ceilf}, {"roundf", m_roundf}, {"truncf", m_truncf}, {"fabsf", m_fabsf},
    {"atan2f", m_atan2f}, {"powf", m_powf}, {"fmodf", m_fmodf},
    {"sin", m_sin}, {"cos", m_cos}, {"tan", m_tan}, {"asin", m_asin}, {"acos", m_acos}, {"atan", m_atan},
    {"exp", m_exp}, {"log", m_log}, {"log2", m_log2}, {"log10", m_log10}, {"sqrt", m_sqrt},
    {"floor", m_floor}, {"ceil", m_ceil}, {"round", m_round}, {"trunc", m_trunc}, {"fabs", m_fabs},
    {"atan2", m_atan2}, {"pow", m_pow}, {"fmod", m_fmod},
};

void register_sdk_imports(void);         /* nnsdk.c */
void register_cxx_imports(void);         /* libcxx.c */
void register_thread_imports(void);      /* os_threads.c */
void register_nvn_imports(void);         /* nvn_stub.c */
void register_fs_imports(void);          /* nnfs.c */
void register_clib_imports(void);        /* clib.c */
void register_time_imports(void);        /* nntime.c */
void register_hid_imports(void);         /* nnhid.c */
void register_iostream_imports(void);    /* nniostream.c */
void register_display_imports(void);     /* display.c */
void register_pl_imports(void);          /* nnpl.c */
void register_fontll_imports(void);      /* nnfontll.c */
void register_movie_imports(void);       /* nnmovie.c */
void register_misc_imports(void);        /* nnmisc.c */
void register_audio_imports(void);       /* nnaudio.c */
void register_ro_imports(void);          /* nnro.c */
void register_sdk2_imports(void);        /* nnsdk2.c: sdklift_*.c, nnmii.c, nnapplet.c, nnalbum.c, nnnet.c, nnfsdir.c */
void register_movie_mf_imports(void);    /* nnmovie_mf.c */
void cxxrt_install_data(void);           /* nncxxrt.c: vtables / typeinfo objects of the C++ runtime (before runtime_init) */
void register_unwind_imports(void);      /* nnunwind.c: C++ exception unwinding through lifted module code */
void register_cxxrt_imports(void);       /* nncxxrt.c + nncxxlib.c + nnlibc2.c: C and C++ runtime, fills what is still missing */
void display_init(void);
int guest_thread_count(void);
extern uint64_t g_main_stack_base, g_main_stack_size;

void dump_guest_threads(FILE *out);      /* os_threads.c */
void icall_dump(FILE *out, uint64_t main_ctx);   /* hostrt.c, SSBU_ICALLS */
uint64_t icall_first_x0(uint64_t target);
static Ctx *g_main_ctx;

static const char *g_elf;

/* Debug aid: SSBU_PROF=<start s>,<duration s> samples the main guest thread's host stack every ~2 ms in that
 * window, unwinds it to guest functions and prints every (caller -> callee) pair seen with its sample count. */
#define PROF_MAX 8192
static struct { uint64_t callee, caller; unsigned n; } g_prof[PROF_MAX];
static int g_prof_used;
static HANDLE g_prof_target;
static DWORD WINAPI profiler(LPVOID arg) {
    int start = 0, dur = 0;
    (void)arg;
    char mode = 's';
    if (sscanf(getenv("SSBU_PROF"), "%d,%d,%c", &start, &dur, &mode) < 2 || dur <= 0) return 0;
    Sleep((DWORD)start * 1000);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)dur * 1000;
    unsigned samples = 0;
    if (mode == 'f') {        /* SSBU_PROF=<start>,<dur>,f : spin reading c->fn / x30: every function the main thread enters */
        while (GetTickCount64() < end) {
            for (int r = 0; r < 4096; r++) {
                uint64_t callee = g_main_ctx->fn, caller = g_main_ctx->x[30];
                unsigned h = (unsigned)((callee >> 2) * 2654435761u) % PROF_MAX;
                while (g_prof[h].callee && g_prof[h].callee != callee) h = (h + 1) % PROF_MAX;
                if (!g_prof[h].callee) { if (g_prof_used >= PROF_MAX - 16) continue; g_prof[h].callee = callee; g_prof[h].caller = caller; g_prof_used++; }
                g_prof[h].n++;
                samples++;
            }
        }
        printf("\n[prof] %u reads of the main thread's current function (function, first lr seen: reads)\n", samples);
        for (int j = 0; j < PROF_MAX; j++)
            if (g_prof[j].callee)
                printf("[prof] f_%llx lr %llx: %u\n", (unsigned long long)g_prof[j].callee, (unsigned long long)g_prof[j].caller, g_prof[j].n);
        return 0;
    }
#ifdef _WIN32      /* Linux build: no other thread's context to sample (no SuspendThread) */
    while (GetTickCount64() < end) {
        CONTEXT ctx;
        uint64_t chain[64];
        int k = 0;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_FULL;
        SuspendThread(g_prof_target);
        GetThreadContext(g_prof_target, &ctx);
        for (int depth = 0; depth < 400 && k < 64; depth++) {
            uint64_t g = guest_function_of(ctx.Rip);
            if (g && (k == 0 || chain[k - 1] != g)) chain[k++] = g;
            DWORD64 base;
            RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
            if (!rf) break;
            void *hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &est, NULL);
            if (!ctx.Rip) break;
        }
        ResumeThread(g_prof_target);
        samples++;
        for (int i = 0; i < k; i++) {
            uint64_t callee = chain[i], caller = i + 1 < k ? chain[i + 1] : 0;
            int j;
            for (j = 0; j < g_prof_used; j++) if (g_prof[j].callee == callee && g_prof[j].caller == caller) break;
            if (j == g_prof_used) { if (g_prof_used == PROF_MAX) continue; g_prof[g_prof_used].callee = callee; g_prof[g_prof_used].caller = caller; g_prof_used++; }
            g_prof[j].n++;
        }
        for (ULONGLONG t0 = GetTickCount64(); GetTickCount64() - t0 < 1;) YieldProcessor();
    }
#endif
    printf("\n[prof] %u samples of the main thread (callee <- caller: samples)\n", samples);
    for (int j = 0; j < g_prof_used; j++)
        printf("[prof] f_%llx <- f_%llx: %u\n", (unsigned long long)g_prof[j].callee, (unsigned long long)g_prof[j].caller, g_prof[j].n);
    return 0;
}

#include "prof15.c"       /* SSBU_PROF15: sampling profiler of every busy thread (test only). Included here, like nvn_vk.c includes its parts, so that
                           * every link that has boot.c has it (build.py's source list is not changed): do NOT name prof15.c as a source of its own. */

static size_t main_stack_size_from_npdm(void) {
    const char *ov = getenv("SSBU_MAIN_STACK");
    if (ov && strtoull(ov, NULL, 0) >= 0x10000) return (size_t)(strtoull(ov, NULL, 0) + 0xfff) & ~(size_t)0xfff;
    char path[1024];
    snprintf(path, sizeof path, "%s", g_elf);
    char *sl = strrchr(path, '/'), *bs = strrchr(path, '\\');
    char *cut = sl > bs ? sl : bs;
    snprintf(cut ? cut + 1 : path, sizeof path - (size_t)((cut ? cut + 1 : path) - path), "main.npdm");
    FILE *f = fopen(path, "rb");
    unsigned char h[0x20];
    if (!f || fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "META", 4)) {
        fprintf(stderr, "missing input: %s (the program's meta data: main thread stack size)\n", path); exit(7);
    }
    fclose(f);
    uint32_t size; memcpy(&size, h + 0x1c, 4);
    printf("main.npdm: main thread stack 0x%x bytes, priority %u, default core %u\n", size, h[0xe], h[0xf]);
    return ((size_t)size + 0xfff) & ~(size_t)0xfff;
}

/* how much of the main guest stack was ever written (pages are zero until used) */
void main_stack_report(void) {
    extern uint64_t g_main_stack_base, g_main_stack_size;
    if (!g_main_stack_base) return;
    const uint64_t *p = (const uint64_t *)(uintptr_t)g_main_stack_base, *e = p + g_main_stack_size / 8;
    while (p < e && !*p) p++;
    MEMORY_BASIC_INFORMATION lo, hi;
    VirtualQuery((void *)(uintptr_t)(g_main_stack_base - 0x1000), &lo, sizeof lo);
    VirtualQuery((void *)(uintptr_t)(g_main_stack_base + g_main_stack_size), &hi, sizeof hi);
    printf("main guest stack: 0x%llx bytes at %llx, deepest use 0x%llx bytes (%.1f%%); page below: %s, page above: %s\n",
           (unsigned long long)g_main_stack_size, (unsigned long long)g_main_stack_base,
           (unsigned long long)((const uint8_t *)e - (const uint8_t *)p), 100.0 * (double)((const uint8_t *)e - (const uint8_t *)p) / (double)g_main_stack_size,
           lo.State == MEM_RESERVE ? "no access" : "ACCESSIBLE", hi.State == MEM_RESERVE ? "no access" : "ACCESSIBLE");
}

static DWORD WINAPI guest_main(LPVOID arg) {
    (void)arg;
    static Ctx c;
    /* close5: the main thread's stack has the size the program's own meta data asks for (main.npdm, META header +0x1c:
     * 0x100000 for this game; read from the file next to the ELF), with no-access pages below and above it, so that an
     * overflow faults as it does on the console instead of running on. Guest frames are the original's (the lifted code
     * moves the guest SP exactly as the ARM code does); the host's own frames live on the host stack and are not
     * guest-visible. SSBU_MAIN_STACK=<bytes> overrides (diagnosis only). */
    size_t stack_size = main_stack_size_from_npdm();
    uint8_t *stack_res = VirtualAlloc(NULL, stack_size + 0x20000, MEM_RESERVE, PAGE_NOACCESS);
    uint8_t *stack = stack_res ? VirtualAlloc(stack_res + 0x10000, stack_size, MEM_COMMIT, PAGE_READWRITE) : NULL;
    if (!stack) { fprintf(stderr, "cannot allocate the main thread's stack (0x%llx bytes)\n", (unsigned long long)stack_size); exit(1); }
    uint8_t *tls = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    c.x[31] = (uint64_t)(stack + stack_size);          /* the initial SP is the top of the stack (16-byte aligned: the size is whole pages) */
    c.tpidr = (uint64_t)tls;
    { uint64_t os_main_thread_type(void); memcpy(tls + 0x1f8, &(uint64_t){os_main_thread_type()}, 8); }   /* [sdk 0x3349dc..e0] nn::os::detail::ThreadManager(): the main thread's ThreadType pointer */
    g_main_ctx = &c;
    { void module_coverage_start(Ctx *main_ctx); module_coverage_start(&c); }   /* nnro.c: SSBU_MODCOV=1 */
    { void os_apply_main_sched(void); os_apply_main_sched(); }                  /* os_threads.c: SSBU_OS_CONSOLE_CORES=1 */
    { void hitch_set_main(Ctx *c); hitch_set_main(&c); }      /* hitch.c: SSBU_HITCH */
    g_main_stack_base = (uint64_t)stack;
    g_main_stack_size = stack_size;

    printf("\n[phase 1] nninitStartup\n");
    trace_printf("=== phase 1: nninitStartup\n");
    call_guest(&c, NNINIT_STARTUP);

    int count = (int)(INIT_ARRAY_SIZE / 8);
    printf("[phase 2] %d static constructors\n", count);
    trace_printf("=== phase 2: static constructors\n");
    for (int i = 0; i < count; i++) {
        uint64_t fn = rd64(INIT_ARRAY + (uint64_t)i * 8);
        if (!fn) continue;
        trace_printf("--- constructor %d at %llx\n", i, (unsigned long long)fn);
        call_guest(&c, fn);
    }

    { void debug_mode_init(void); debug_mode_init(); }       /* debug_mode.c: test-only fast mode, nothing without SSBU_* variables */
    printf("[phase 3] nnMain\n");
    trace_printf("=== phase 3: nnMain\n");
    call_guest(&c, NN_MAIN);
    { void clib_run_atexit(Ctx *c); clib_run_atexit(&c); }   /* clib.c: functions registered with atexit, as a normal exit does */

    printf("\nnnMain returned.\n");
    print_import_summary(stdout);
    return 0;
}

#ifndef _WIN32
void plat_print_build_identity(const char *exe, size_t dir_len);     /* plat_posix.c: the same lines from the ELF files */
#endif
/* The first lines of every log: which binaries these are (link time stamps of the exe and of the DLLs beside it, the values
 * a minidump also shows) and when the process started, so that a log from another PC names its build. File names only: logs
 * get posted, and the folders name the Windows user. */
static void print_build_identity(void) {
    char exe[MAX_PATH], name[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    const char *slash = strrchr(exe, '\\');
    size_t dl = slash ? (size_t)(slash - exe + 1) : 0;
    SYSTEMTIME st; GetLocalTime(&st);
    printf("build: %s, started %04d-%02d-%02d %02d:%02d:%02d, process %lu\n", exe + dl, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
#ifdef _WIN32
    HMODULE mods[256];
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof mods[0] && i < 256; i++) {
        if (!GetModuleFileNameA(mods[i], name, MAX_PATH) || _strnicmp(name, exe, dl)) continue;      /* ours: the exe's folder */
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)mods[i] + ((const IMAGE_DOS_HEADER *)mods[i])->e_lfanew);
        __time64_t t = nt->FileHeader.TimeDateStamp;
        struct tm tm;
        char when[32] = "?";
        if (!_gmtime64_s(&tm, &t)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", &tm);
        printf("build: %-22s linked %s (stamp %08lx, image %lu bytes)\n", name + dl, when, (unsigned long)nt->FileHeader.TimeDateStamp, (unsigned long)nt->OptionalHeader.SizeOfImage);
    }
#else
    (void)name;
    plat_print_build_identity(exe, dl);
#endif
}

/* (pf1) The lifted code is ~1.3 GB of DLLs (game library + nro_bin modules) that Windows reads from disk on the first call of
 * each page. At fight start every new move then waits on the disk: 1-7 s freezes on a hard drive, both games of a PC at once
 * (they share the pages). Read each module in ahead on a background-priority thread instead. SSBU_CODE_PREFETCH=0: off. */
typedef BOOL (WINAPI *PrefetchFn)(HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);

static DWORD WINAPI code_prefetch_thread(LPVOID arg) {
    static PrefetchFn prefetch;
    if (!prefetch) prefetch = (PrefetchFn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "PrefetchVirtualMemory");
    HMODULE m = (HMODULE)arg;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)m + ((const IMAGE_DOS_HEADER *)m)->e_lfanew);
    WIN32_MEMORY_RANGE_ENTRY r = {m, nt->OptionalHeader.SizeOfImage};
    char name[MAX_PATH] = "?";
    GetModuleFileNameA(m, name, MAX_PATH);
    const char *base = strrchr(name, '\\');
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);      /* low-priority I/O too: the game's own reads go first */
    DWORD t0 = GetTickCount();
    BOOL ok = prefetch && prefetch(GetCurrentProcess(), 1, &r, 0);
    printf("code prefetch: %s, %.0f MB %s in %lu ms\n", base ? base + 1 : name, r.NumberOfBytes / 1048576.0, ok ? "read" : "NOT read", GetTickCount() - t0);
    return 0;
}

void code_prefetch(HMODULE m) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_CODE_PREFETCH"); on = !(e && e[0] == '0'); }
    if (on && m) { HANDLE t = CreateThread(NULL, 0, code_prefetch_thread, m, 0, NULL); if (t) CloseHandle(t); }
}

/* the exe and the game library (the big modules of the exe's folder; nro_bin modules are done by nnro.c as they load) */
static void code_prefetch_boot(void) {
    char exe[MAX_PATH], name[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH), need = 0;
    while (n && exe[n - 1] != '\\') n--;
    HMODULE mods[256];
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    for (DWORD i = 0; i < need / sizeof mods[0] && i < 256; i++) {
        if (!GetModuleFileNameA(mods[i], name, MAX_PATH) || _strnicmp(name, exe, n) || strchr(name + n, '\\')) continue;
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)mods[i] + ((const IMAGE_DOS_HEADER *)mods[i])->e_lfanew);
        if (nt->OptionalHeader.SizeOfImage >= (16u << 20)) code_prefetch(mods[i]);
    }
}

/* SSBU_CRASH_TEST=<seconds>: a write to address 0 after that time, on a thread of its own: tries the whole crash path of a
 * PC (crash report, minidump) without waiting for a real crash. Test only. */
static DWORD WINAPI crash_test(LPVOID arg) {
    (void)arg;
    Sleep((DWORD)(atof(getenv("SSBU_CRASH_TEST")) * 1000.0));
    printf("SSBU_CRASH_TEST: writing to address 0 now\n");
    *(volatile int *)(uintptr_t)0 = 1;
    return 0;
}

/* A launch without scripts/run.py (the exe started by itself, from build\work): the RomFS folder that run.py would pass is
 * taken from ..\state.json ("romfs", written by scripts/setup.py) when that folder has data.arc. A variable that is set is
 * left alone. Without this a direct start looked for ..\romfs, found no data.arc, and the game crashed reading the file it
 * could not open. (The add-on content list is found by nnmisc.c itself.) */
static char g_state_romfs[MAX_PATH];                /* the folder ..\state.json names, if launch_defaults read one: check_romfs names it */
static int g_state_romfs_lossy;                     /* ... and its name has characters the ANSI code page of this Windows cannot hold */
static char *put_utf8(char *o, unsigned cp) {
    if (cp < 0x80) *o++ = (char)cp;
    else if (cp < 0x800) { *o++ = (char)(0xc0 | cp >> 6); *o++ = (char)(0x80 | (cp & 0x3f)); }
    else if (cp < 0x10000) { *o++ = (char)(0xe0 | cp >> 12); *o++ = (char)(0x80 | (cp >> 6 & 0x3f)); *o++ = (char)(0x80 | (cp & 0x3f)); }
    else { *o++ = (char)(0xf0 | cp >> 18); *o++ = (char)(0x80 | (cp >> 12 & 0x3f)); *o++ = (char)(0x80 | (cp >> 6 & 0x3f)); *o++ = (char)(0x80 | (cp & 0x3f)); }
    return o;
}
static const char *json_blank(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++; return p; }
static unsigned hex4(const char *p) {                   /* the 4 hex digits at p, or ~0u */
    unsigned v = 0;
    for (int i = 0; i < 4; i++, p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : (*p | 32) >= 'a' && (*p | 32) <= 'f' ? (*p | 32) - 'a' + 10 : -1;
        if (d < 0) return ~0u;
        v = v << 4 | (unsigned)d;
    }
    return v;
}
/* The JSON string whose opening quote is at `q`, as a path in the ANSI code page that the runtime's file calls use. \\ \" \/
 * and \uXXXX (surrogate pairs too) are decoded: json.dump writes every non-ASCII character of a path as \uXXXX. A path the
 * code page cannot hold goes through its 8.3 name when the volume has one; else `lossy` is set and the unknown characters
 * are '?'. Returns 0 when the string is cut off or too long. */
static int json_path(const char *q, char *out, size_t size, int *lossy) {
    char u8[4 * MAX_PATH], *o = u8;
    WCHAR w[MAX_PATH], sw[MAX_PATH];
    BOOL lost = FALSE;
    const char *p = q + 1;
    for (; *p && *p != '"'; p++) {
        unsigned cp = (unsigned char)*p;
        if (o - u8 > (int)sizeof u8 - 5) return 0;
        if (cp == '\\' && p[1]) {
            cp = (unsigned char)*++p;
            if (cp == 'u') {
                unsigned u = hex4(p + 1);
                if (u == ~0u) return 0;
                p += 4;
                if (u >= 0xd800 && u < 0xdc00 && p[1] == '\\' && p[2] == 'u') {          /* the first half of a surrogate pair */
                    unsigned lo = hex4(p + 3);
                    if (lo >= 0xdc00 && lo < 0xe000) { u = 0x10000 + ((u - 0xd800) << 10) + (lo - 0xdc00); p += 6; }
                }
                cp = u;
            }
        }
        o = put_utf8(o, cp);
    }
    if (*p != '"') return 0;
    *o = 0;
    *lossy = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, u8, -1, w, MAX_PATH) || !WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)size, NULL, &lost)) return 0;
    if (lost) {
        DWORD n = GetShortPathNameW(w, sw, MAX_PATH);
        BOOL lost2 = FALSE;
        if (n && n < MAX_PATH && WideCharToMultiByte(CP_ACP, 0, sw, -1, out, (int)size, NULL, &lost2) && !lost2) lost = FALSE;
        else WideCharToMultiByte(CP_ACP, 0, w, -1, out, (int)size, NULL, &lost);
    }
    *lossy = lost;
    return 1;
}
static void launch_defaults(void) {
    const char *rom = getenv("SSBU_ROMFS"), *p;
    char buf[16384], probe[MAX_PATH + 16];
    if (rom && rom[0]) return;
    FILE *f = fopen("..\\state.json", "rb");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (!(p = strstr(buf, "\"romfs\""))) return;
    p = json_blank(p + 7);
    if (*p++ != ':') return;
    p = json_blank(p);
    if (*p != '"' || !json_path(p, g_state_romfs, sizeof g_state_romfs, &g_state_romfs_lossy)) { g_state_romfs[0] = 0; return; }     /* not a string (null) */
    snprintf(probe, sizeof probe, "%s\\data.arc", g_state_romfs);
    if (g_state_romfs[0] && !g_state_romfs_lossy && GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) {
        _putenv_s("SSBU_ROMFS", g_state_romfs);
        printf("SSBU_ROMFS not set: taken from ..\\state.json\n");
    }
}
/* The message of a start that cannot work, on the console and on stderr. A box on the screen as well, unless the run is a
 * scripted one or SSBU_ERROR_DIALOG=0 (the rule of nnmisc.c err_dialog: tests never hang on a dialog). Exit code 2. */
static void romfs_fail(const char *msg) {
    const char *off = getenv("SSBU_ERROR_DIALOG");
    int quiet = (off && off[0] == '0') || getenv("SSBU_SEQ") || getenv("SSBU_PLAY") || getenv("SSBU_PRESS") || getenv("SSBU_SCRIPT");
    printf("\n%s\n", msg);
    if (quiet) fprintf(stderr, "%s\n", msg);
    else MessageBoxA(NULL, msg, "SSBU native", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
    exit(2);
}
/* The game cannot run without data.arc: say so instead of crashing later inside the game. Modes that run without game data
 * (the self-tests, all of which exit) skip it. */
static void check_romfs(void) {
    static const char *const no_data[] = {"SSBU_SDK_SELFTEST", "SSBU_VK_SELFTEST", "SSBU_CXXRT_SELFTEST", "SSBU_MISC_SELFTEST", "SSBU_EH_MAIN_TEST"};
    const char *rom = getenv("SSBU_ROMFS");
    char probe[MAX_PATH + 16], msg[2 * MAX_PATH + 800];
    const char *dir = rom && rom[0] ? rom : "..\\romfs";
    for (size_t i = 0; i < sizeof no_data / sizeof no_data[0]; i++) if (getenv(no_data[i])) return;
    snprintf(probe, sizeof probe, "%s\\data.arc", dir);
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
        static const char how[] = "Start the game with  python scripts\\run.py --userdata <folder>  from the repository, "
                                  "or set SSBU_ROMFS to the RomFS folder of your dump (the one that holds data.arc).";
        if (!(rom && rom[0]) && g_state_romfs[0])           /* ..\state.json names a folder that has no data.arc (any more) */
            snprintf(msg, sizeof msg, "data.arc was not found in %s, the RomFS folder that ..\\state.json names%s, nor in %s.\n\n"
                     "The dump was moved or renamed after scripts\\setup.py recorded it. %s", g_state_romfs,
                     g_state_romfs_lossy ? " (some characters of its name do not exist in this Windows code page)" : "", dir, how);
        else snprintf(msg, sizeof msg, "data.arc was not found (%s).\n\n%s", probe, how);
    } else {
        /* .nrr\*.nrr beside data.arc: the lists of the script modules nn::ro may load (the game registers them at start). A
         * RomFS without them runs to the menus, then every module is refused and the game waits for ever on the loading
         * screen before a fight. */
        WIN32_FIND_DATAA fd;
        snprintf(probe, sizeof probe, "%s\\.nrr\\*.nrr", dir);
        HANDLE h = FindFirstFileA(probe, &fd);
        if (h != INVALID_HANDLE_VALUE) { FindClose(h); return; }
        snprintf(msg, sizeof msg, "The RomFS folder %s has data.arc but no .nrr\\*.nrr files.\n\nThe game would refuse to load its script "
                 "modules and stop on the loading screen before the first fight. Copy the whole RomFS folder of your dump "
                 "(the .nrr folder sits beside data.arc).", dir);
    }
    romfs_fail(msg);
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--write-dump")) { int diag_dump_helper(int, char **); return diag_dump_helper(argc, argv); }   /* hostrt.c: minidump of the crashed game, written from outside */
    g_elf = argc > 1 ? argv[1] : "..\\binaries\\13.0.4\\main.elf";
    setvbuf(stdout, NULL, _IONBF, 0);
    print_build_identity();
    launch_defaults();
    {   /* pad tools and their self-test (nnpad_sdl.c): no game is started */
        int nnhid_pad_selftest(void), nnpad_tool(int, char **);
        int tool;
        if (getenv("SSBU_PAD_SELFTEST")) return nnhid_pad_selftest() != 0;
        if ((tool = nnpad_tool(argc, argv)) >= 0) return tool;
    }
    /* track V: `--prepare-shaders [folder]` translates every stage of the shader containers in the folder (default
     * ..\cache\vk_containers, filled by tools_vk/bnsh_extract.py) into ..\cache\vk; no game, no window, no GPU (nvn_vk_m4.c) */
    if (argc > 1 && !strcmp(argv[1], "--prepare-shaders")) { int nvn_vk_prepare_shaders(const char *); return nvn_vk_prepare_shaders(argc > 2 ? argv[2] : NULL); }
    /* data symbols the game imports from the SDK module get their real values (see gen_sdkdata.py) */
    for (int i = 0; i < sdk_data_count; i++) {
        void *copy = _aligned_malloc(sdk_data[i].size, 16);
        memcpy(copy, sdk_data[i].bytes, sdk_data[i].size);
        import_set_address(sdk_data[i].name, copy);
    }
    cxxrt_install_data();
    check_romfs();
    runtime_init(g_elf, getenv("SSBU_TRACE") ? getenv("SSBU_TRACE") : "boot_trace.txt");
    if (getenv("SSBU_CRASH_TEST")) CloseHandle(CreateThread(NULL, 0, crash_test, NULL, 0, NULL));
    code_prefetch_boot();
    register_imports(base_imports, (int)(sizeof base_imports / sizeof base_imports[0]));
    register_sdk_imports();
    register_cxx_imports();
    register_thread_imports();               /* later registrations replace earlier ones */
    register_nvn_imports();
    register_fs_imports();
    register_clib_imports();                 /* full printf engine replaces the integer-only versions */
    register_time_imports();
    register_hid_imports();
    register_display_imports();
    register_pl_imports();
    register_fontll_imports();
    register_movie_imports();
    register_misc_imports();                 /* only fills names nobody else implements */
    register_audio_imports();
    register_ro_imports();
    register_movie_mf_imports();             /* replaces nnmovie.c's failing placeholder unless SSBU_MOVIE=0 */
    register_iostream_imports();             /* libc++ stream base classes */
    register_cxxrt_imports();                /* C / C++ runtime: everything still without a handler (keep last) */
    register_unwind_imports();               /* installs the unwinder the C++ runtime calls on a throw */
    register_sdk2_imports();                 /* SDK families (lifted sdk.elf code + offline service answers): fills only; after the C runtime because its self-test needs every handler */
    { void audit_startup(void); audit_startup(); }   /* hostrt.c: fake audit / SSBU_STRICT (plan\FAKE_AUDIT.md) */
    display_init();
    int seconds = argc > 2 ? atoi(argv[2]) : 0;      /* no number = no time limit (a player's launch); tests pass theirs */
    /* the lifted code uses the host stack for every guest call: give it plenty */
    HANDLE t = CreateThread(NULL, (SIZE_T)1 << 30, guest_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (getenv("SSBU_PROF")) { g_prof_target = t; CloseHandle(CreateThread(NULL, 0, profiler, NULL, 0, NULL)); }
    { void prof15_start(void); prof15_start(); }         /* prof15.c: SSBU_PROF15, every busy thread (test only; nothing without the variable) */
    DWORD wr = WaitForSingleObject(t, seconds > 0 ? (DWORD)seconds * 1000 : INFINITE);
    {   unsigned nvn_gl_held_ms(void);                   /* nvn_gl.c: time the presents were held for shader preparation is not run time */
        for (unsigned done = 0, h; wr == WAIT_TIMEOUT && (h = nvn_gl_held_ms()) > done + 50; done = h) wr = WaitForSingleObject(t, h - done);
    }
    if (wr == WAIT_TIMEOUT) {
        printf("\nstill running after %d s with %d guest thread(s) started: stopping here.\n", seconds, guest_thread_count());
        trace_printf("=== stopped by the time limit after %d s\n", seconds);
        main_stack_report();
        if (g_main_ctx) {
            printf("main thread: last function f_%llx  lr=%llx  %s%s (from %llx)\n", (unsigned long long)g_main_ctx->fn,
                   (unsigned long long)g_main_ctx->x[30], g_main_ctx->in_import ? "in " : "",
                   g_main_ctx->in_import ? g_main_ctx->in_import : "guest code", (unsigned long long)g_main_ctx->import_from);
            for (int i = 19; i < 29; i++) printf("  x%d %016llx%s", i, (unsigned long long)g_main_ctx->x[i], i % 3 == 0 ? "\n" : "");
            printf("\n");
            {   /* the global std::map at 0x710593c3c0 the main thread walks: header and first nodes */
                uint64_t m = GA(0x710593c3c0ULL);
                printf("map header: [+0]=%llx [+8]=%llx [+10]=%llx  size=%llx\n", (unsigned long long)rd64(m),
                       (unsigned long long)rd64(m + 8), (unsigned long long)rd64(m + 16), (unsigned long long)rd64(m + 24));
                uint64_t n = rd64(m + 8);
                for (int k = 0; k < 6 && n > 0x1000 && n != m; k++) {
                    printf("  node %llx: left=%llx right=%llx parent=%llx color=%llx key=%llx\n", (unsigned long long)n,
                           (unsigned long long)rd64(n), (unsigned long long)rd64(n + 8), (unsigned long long)rd64(n + 16),
                           (unsigned long long)rd64(n + 24), (unsigned long long)rd64(n + 32));
                    n = rd64(n);
                }
            }
#ifdef _WIN32
            /* where is the main host thread really? sample its instruction pointer and unwind */
            for (int s = 0; s < 3; s++) {
                Sleep(50);
                CONTEXT ctx;
                memset(&ctx, 0, sizeof ctx);
                ctx.ContextFlags = CONTEXT_FULL;
                SuspendThread(t);
                GetThreadContext(t, &ctx);
                printf("  host rip sample %d: rip=%p -> f_%llx |", s, (void *)ctx.Rip, (unsigned long long)guest_function_of(ctx.Rip));
                uint64_t last = 0;
                for (int depth = 0, shown = 0; depth < 200 && shown < 12; depth++) {
                    uint64_t g = guest_function_of(ctx.Rip);
                    if (g && g != last) { printf(" f_%llx", (unsigned long long)g); shown++; last = g; }
                    else if (!g && depth == 0) printf(" [host %p]", (void *)ctx.Rip);
                    DWORD64 base;
                    RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                    if (!rf) break;
                    void *hd; DWORD64 est;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &est, NULL);
                    if (!ctx.Rip) break;
                }
                printf("\n");
                ResumeThread(t);
            }
#endif
            for (int s = 0; s < 8; s++) {            /* is it moving? sample a few times */
                Sleep(37);
                printf("  sample %d: f_%llx lr=%llx x0=%llx x1=%llx x8=%llx x9=%llx x11=%llx x12=%llx\n", s,
                       (unsigned long long)g_main_ctx->fn, (unsigned long long)g_main_ctx->x[30], (unsigned long long)g_main_ctx->x[0],
                       (unsigned long long)g_main_ctx->x[1], (unsigned long long)g_main_ctx->x[8], (unsigned long long)g_main_ctx->x[9],
                       (unsigned long long)g_main_ctx->x[11], (unsigned long long)g_main_ctx->x[12]);
            }
        }
        dump_guest_threads(stdout);
        icall_dump(stdout, (uint64_t)(uintptr_t)g_main_ctx);
        if (getenv("SSBU_CHAIN")) {
            /* SSBU_CHAIN=<fn>:<off>,<off>,...[;<fn>:...] (hex): start from x0 of the first recorded indirect call to fn
             * (needs SSBU_ICALLS), follow pointers at the given offsets, print 0x40 bytes at every step */
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", getenv("SSBU_CHAIN"));
            for (char *spec = strtok(buf, ";"); spec; spec = strtok(NULL, ";")) {
                char *colon = strchr(spec, ':');
                uint64_t p = icall_first_x0(strtoull(spec, NULL, 16));
                printf("[chain] %s: x0=%llx\n", spec, (unsigned long long)p);
                for (char *o = colon ? colon + 1 : NULL; p; o = o ? strchr(o, ',') : NULL) {
                    MEMORY_BASIC_INFORMATION mi;
                    if (!VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi) || mi.State != MEM_COMMIT) { printf("[chain]   %llx not mapped\n", (unsigned long long)p); break; }
                    printf("[chain]   %llx:", (unsigned long long)p);
                    for (int w = 0; w < 0x130; w += 8) printf("%s%016llx", w % 0x40 ? " " : "\n[chain]     ", (unsigned long long)rd64(p + w));
                    printf("\n");
                    if (!o) break;
                    if (*o == ',') o++;
                    uint64_t off = strtoull(o, NULL, 16);
                    p = rd64(p + off);
                    printf("[chain]   [+%llx] = %llx\n", (unsigned long long)off, (unsigned long long)p);
                }
            }
        }
        report_and_exit(0);
    }
    return 0;
}
