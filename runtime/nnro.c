/* nn::ro: the game's run-time code modules (the lua2cpp_* NROs: fighter, item and common scripts).
 *
 * Every module is lifted ahead of time at a fixed guest address (ro_study\nro_map.json, 2 MB slots from
 * 0x7200000000; table in nro_registry.c) and packed into its own DLL, nro_bin\<module>.dll, by
 * `python build.py nro <module>` (SSBU_NRO_BIN=<folder> selects another folder of DLLs). At LoadModule the module is recognised by the build id in the buffer
 * the game passes, the image is copied from that buffer to the assigned address (nothing of the game is
 * stored in our binaries), relocated, bound and its DT_INIT is run. See ro_study\PLAN.md.
 *
 * Facts read off the real SDK module (binaries\13.0.4\sdk.elf, `python sdkdis.py <name>`):
 *   nn::ro::Module (0x20 bytes; LoadModuleInternal 0x347460, UnloadModule 0x3478a0)
 *     +0x00  RoModule *   the loader's object, which lives INSIDE the module image at
 *                         mod0 + u32[mod0 + 0x18]; LoadModuleInternal links it to itself
 *                         ([obj] = [obj+8] = obj) and RoModule::Initialize stores the load address at
 *                         obj+0x20 and the dynamic section at obj+0x28
 *     +0x08  int state    1 after LoadModule, 0 after UnloadModule
 *     +0x10  void *nro    the buffer passed in
 *     +0x18  void *bss    the bss buffer passed in
 *   LoadModule(module, nro, bss, bssSize, flag): the 5-argument form is LoadModuleInternal(..., false).
 *     Checks the "NRO0" magic at +0x10 (else result 0x816), maps the image through the ro service
 *     (size u32[nro+0x18], bss u32[nro+0x38]), zeroes mod0+u32[mod0+8] .. mod0+u32[mod0+0xc], then
 *     RoModule::Initialize, RoModule::Relocation(flag bit 1), RoModule::CallInit (DT_INIT with no
 *     arguments, RoModule+0x40), then BindVariables of every other module against the new one.
 *     The game passes flag 2; this loader always binds everything at load time.
 *   GetBufferSize(out, nro): magic check (0x816), *out = (u64)u32[nro + 0x38], result 0.
 *   RegisterModuleInfo(info, nrr): checks "NRR0" at +0 (else 0xc16), registers the hash list with the
 *     ro service, then info: +0 u32 = 1, +8 = nrr, +0x10 = list node (two pointers).
 *   Results are module 22; the SDK aborts on anything that is not (module 22, description 1..0x3ff).
 *   __nnmusl_init_dso returns 1 or 2; a module's _init runs its init_array unless the result is 1. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hostrt.h"

#define NRO_BASE 0x7200000000ULL
#define NRO_SLOT_SHIFT 21                      /* 2 MB */
#define NRO_MAX_SLOTS 0x800                    /* up to IMPORT_BASE */

#define RO_OK 0u
#define RO_INVALID_NRO 0x816u                  /* module 22, description 4: the SDK's own magic check */
#define RO_INVALID_NRR 0xc16u                  /* module 22, description 6 */

/* generated table of every module of the game (nro_registry.c, from ro_study\nro_map.json): NroInfo is in game_data.h,
 * the table itself in the game DLL (game_data.c), bound before main() (gamedll.c) */
#define nro_registry (game_data.nro_tab)
#define nro_registry_count (game_data.nro_n)

/* interface of nro_bin\<module>.dll (nro_shim.c) */
typedef struct {
    void (*call_indirect)(Ctx *, uint64_t);
    void (*host_import)(Ctx *, int);
    void (*guest_trap)(const char *, uint64_t);
    void (*guest_unimpl)(uint64_t, uint32_t, const char *);
    void (*guest_svc)(Ctx *, uint32_t);
    void (*stx_diag)(Ctx *, uint64_t, uint64_t, uint64_t, uint64_t, int);
    void (*nro_import)(Ctx *, void *, int);
    void (*catch_call)(Ctx *, void (*)(Ctx *, uint64_t));      /* nnunwind.c: entry of functions with a catch clause */
    void (*site_inline)(Ctx *, uint64_t);                      /* site callouts of a re-lifted module (nro_bin_sky, relift_sites.py) */
    int (*site_midhook)(Ctx *, uint64_t);
} NroHost;
typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;      /* landing pad -> resume entry (nnunwind.c) */
/* a DLL built before the exception support fills only the first four fields: resume stays NULL */
typedef struct { const FnEntry *fns; int fn_count; const DynSym *syms; int sym_count; const NroResume *resume; int resume_count; } NroTables;
typedef void (*NroBindFn)(const NroHost *, void *, NroTables *);

typedef struct NroModule {
    const NroInfo *info;
    HMODULE dll;
    const FnEntry *fns; int fn_count;          /* the module's lifted functions, sorted by guest address */
    const DynSym *syms; int sym_count;         /* its dynamic symbols by index */
    FnEntry *by_host;                          /* fns sorted by host code address, for crash reports */
    volatile LONG loaded;
    ImportFn *bind_fn;                         /* per dynamic symbol: lifted function to call directly */
    uint64_t *bind_addr;                       /* per dynamic symbol: its address (for call_indirect), 0 = unresolved */
    uint64_t dt_init, dt_fini;
    const NroResume *resume; int resume_count; /* landing pads the unwinder can enter, sorted; NULL for an old DLL */
    struct FsMod *fighter;                     /* S1: C++ source of some of its script functions (fighter_sdk\fighter_host.c), or NULL */
    int plugin;                                /* a Skyline plugin (skyline.c: plugin_bin\plugins.txt), not a module of the game */
    uint64_t rtld_obj;                         /* its rtld ModuleObject while loaded (skyline.c: in g_pManualLoadList) */
    uint64_t *got;                             /* per dynamic symbol: guest address of its GOT entry, 0 = none (plugins rewrite GOT entries) */
} NroModule;

void input_mark(const char *what);             /* nnhid.c */
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t));   /* nnunwind.c */
int eh_register_image(uint64_t base, uint64_t size, uint64_t eh_frame_hdr);    /* eh_runtime.h (nncxxrt.c) */
void eh_unregister_image(uint64_t base);
uint64_t import_address(int sym);              /* hostrt.c: what main's relocations use for an imported symbol */

static NroModule *g_mods;                      /* one per registry entry, then one per Skyline plugin */
static int g_mod_count;                        /* nro_registry_count + plugins */
static int sky_registry(void);                 /* skyline.c */
static NroInfo g_sky_info[];
static void sky_module_loaded(NroModule *m, uint64_t obj);
static void sky_module_unloaded(NroModule *m);
static uint64_t sky_data_symbol(const char *name);
static uint64_t sky_sdk_symbol(const char *name);
static const char *sky_bin_dir(void);
static NroModule *volatile g_slot[NRO_MAX_SLOTS];   /* 2 MB address slot -> module, set while loaded */
static SRWLOCK g_ro_lock = SRWLOCK_INIT;

/* ---------------------------------------------------------------- name tables */
static unsigned hash_name(const char *s) { unsigned h = 2166136261u; while (*s) h = (h ^ (unsigned char)*s++) * 16777619u; return h; }

static int *g_main_hash; static unsigned g_main_mask;        /* name -> index in main's dyn_syms, -1 = empty */

static int main_find(const char *name) {
    for (unsigned i = hash_name(name) & g_main_mask; g_main_hash[i] >= 0; i = (i + 1) & g_main_mask)
        if (strcmp(dyn_syms[g_main_hash[i]].name, name) == 0) return g_main_hash[i];
    return -1;
}

typedef struct { const char *name; NroModule *mod; uint64_t value; } Export;
#define EXP_CAP (1u << 16)                     /* the 98 modules export 9,850 symbols in total */
static Export g_exp[EXP_CAP];

static void exp_add(const char *name, NroModule *mod, uint64_t value) {
    unsigned i = hash_name(name) & (EXP_CAP - 1);
    while (g_exp[i].name) {
        if (strcmp(g_exp[i].name, name) == 0) { if (!g_exp[i].mod->loaded) break; return; }   /* an unloaded module's entry is reused */
        i = (i + 1) & (EXP_CAP - 1);
    }
    g_exp[i].value = value; g_exp[i].mod = mod; g_exp[i].name = name;
}

static const Export *exp_find(const char *name) {
    for (unsigned i = hash_name(name) & (EXP_CAP - 1); g_exp[i].name; i = (i + 1) & (EXP_CAP - 1))
        if (strcmp(g_exp[i].name, name) == 0) return g_exp[i].mod->loaded ? &g_exp[i] : NULL;
    return NULL;
}

static ImportFn fn_in_table(const FnEntry *t, int n, uint64_t addr) {
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (t[mid].addr == addr) return t[mid].fn;
        if (t[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

static int g_ro_reserved;                      /* the modules' address range was reserved at start-up (ro_reserve_range) */
static void ro_init_tables(void) {
    static volatile LONG done;
    AcquireSRWLockExclusive(&g_ro_lock);
    if (!done) {
        unsigned cap = 1;
        while (cap < (unsigned)dyn_sym_count * 2u) cap <<= 1;
        g_main_hash = malloc(cap * sizeof *g_main_hash);
        g_main_mask = cap - 1;
        for (unsigned i = 0; i < cap; i++) g_main_hash[i] = -1;
        for (int s = 1; s < dyn_sym_count; s++) {
            if (!dyn_syms[s].name[0]) continue;
            unsigned i = hash_name(dyn_syms[s].name) & g_main_mask;
            while (g_main_hash[i] >= 0 && strcmp(dyn_syms[g_main_hash[i]].name, dyn_syms[s].name) != 0) i = (i + 1) & g_main_mask;
            if (g_main_hash[i] < 0) g_main_hash[i] = s;
        }
        int np = sky_registry();
        g_mods = calloc((size_t)(nro_registry_count + np), sizeof *g_mods);
        for (int i = 0; i < nro_registry_count; i++) g_mods[i].info = &nro_registry[i];
        for (int i = 0; i < np; i++) { g_mods[nro_registry_count + i].info = &g_sky_info[i]; g_mods[nro_registry_count + i].plugin = 1; }
        g_mod_count = nro_registry_count + np;
        done = 1;
    }
    ReleaseSRWLockExclusive(&g_ro_lock);
}

/* ---------------------------------------------------------------- hooks called from hostrt.c */
static NroModule *module_at(uint64_t addr) {
    if (addr < NRO_BASE || addr >= NRO_BASE + ((uint64_t)NRO_MAX_SLOTS << NRO_SLOT_SHIFT)) return NULL;
    return g_slot[(addr - NRO_BASE) >> NRO_SLOT_SHIFT];
}

/* Script module coverage (audit row "script modules: code coverage"), SSBU_MODCOV=1: which lifted module functions were
 * ENTERED in this run. Two sources, both exact for what they see and together a LOWER bound:
 *   (a) every indirect call into a module (the engine calls script functions through tables: this is how a module is
 *       entered at all) marks its target;
 *   (b) a sampler reads, every millisecond, the "function entered last" word (Ctx.fn, written at the entry of every
 *       lifted function) of every guest thread and marks it when it is a module function: this adds functions reached by
 *       direct calls inside a module, but only those a sample happens to see.
 * A function called directly and never seen by a sample is NOT counted. module_coverage_report prints, per loaded module,
 * marked / lifted functions; nothing else is changed by the switch. */
static uint8_t *g_cov[512];                    /* per registry entry: one byte per lifted function */
static int modcov_on(void) { static int on = -1; if (on < 0) on = getenv("SSBU_MODCOV") != NULL; return on; }
static void modcov_mark(NroModule *m, uint64_t addr) {
    int i = (int)(m - g_mods), lo = 0, hi = m->fn_count - 1;
    if (i < 0 || i >= 512) return;
    if (!g_cov[i]) {
        uint8_t *b = calloc((size_t)m->fn_count + 1, 1);
        if (InterlockedCompareExchangePointer((void *volatile *)&g_cov[i], b, NULL)) free(b);
    }
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (m->fns[mid].addr == addr) { g_cov[i][mid] = 1; return; }
        if (m->fns[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
}
int guest_thread_info(int i, Ctx **c, const char **name, HANDLE *h);      /* os_threads.c */
static Ctx *g_cov_main_ctx;                                                /* boot.c's main guest thread */
static DWORD WINAPI modcov_sampler(LPVOID arg) {
    (void)arg;
    for (;;) {
        Sleep(1);
        for (int t = -1; t < 256; t++) {
            Ctx *c = NULL; const char *name; HANDLE h;
            if (t < 0) c = g_cov_main_ctx; else if (!guest_thread_info(t, &c, &name, &h)) break;
            if (!c) continue;
            uint64_t fn = c->fn;
            if (fn < NRO_BASE || fn >= NRO_BASE + ((uint64_t)NRO_MAX_SLOTS << NRO_SLOT_SHIFT)) continue;
            NroModule *m = g_slot[(fn - NRO_BASE) >> NRO_SLOT_SHIFT];
            if (m && m->loaded && m->fns) modcov_mark(m, fn);
        }
    }
}
static void fighter_offline(Ctx *c);           /* S2: fighter_sdk\fighter_host.c */
static void hook_start(Ctx *c);                /* S3: fighter_sdk\hook_host.c */
void module_coverage_start(Ctx *main_ctx) {    /* boot.c, once, before the game starts */
    g_cov_main_ctx = main_ctx;
    hook_start(main_ctx);                      /* S3: the hook table: self-test, SSBU_HOOK_COUNT, mods */
    fighter_offline(main_ctx);                 /* S2: SSBU_FIGHTER_OFFLINE: test fighter source without the game, then exit */
    if (modcov_on()) CloseHandle(CreateThread(NULL, 0, modcov_sampler, NULL, 0, NULL));
}
void module_coverage_report(FILE *out) {
    long long tot_fn = 0, tot_hit = 0;
    int mods = 0;
    if (!modcov_on() || !g_mods) return;
    fprintf(out, "script module coverage (SSBU_MODCOV: functions entered by an indirect call or seen by the 1 ms sampler; a LOWER bound):\n");
    for (int i = 0; i < g_mod_count && i < 512; i++) {
        NroModule *m = &g_mods[i];
        long long hit = 0;
        if (!m->fns || !m->fn_count) continue;                 /* never loaded in this run */
        for (int k = 0; g_cov[i] && k < m->fn_count; k++) hit += g_cov[i][k];
        fprintf(out, "  %-28s %6lld of %6d lifted functions (%.1f%%)%s\n", m->info->name, hit, m->fn_count, 100.0 * (double)hit / m->fn_count, m->loaded ? "" : "  [unloaded again]");
        tot_fn += m->fn_count; tot_hit += hit; mods++;
    }
    fprintf(out, "  modules loaded in this run: %d of %d; functions marked: %lld of %lld in the loaded modules (%.2f%%)\n", mods, g_mod_count, tot_hit, tot_fn, tot_fn ? 100.0 * (double)tot_hit / (double)tot_fn : 0.0);
}

static int fighter_dispatch(Ctx *c, NroModule *m, uint64_t addr, ImportFn lifted);      /* fighter_sdk\fighter_host.c, included below */
static int fighter_rec_pointer(Ctx *c, uint64_t addr, ImportFn fn);                     /* S2 */
static volatile LONG g_fs_recording, g_fs_replaying;                                    /* S2: defined in fighter_host.c */
static int g_fs_hookmode = 0;                                                           /* S3: SSBU_FIGHTER_HOOK=1: source functions are installed as entry hooks (default 0 = the session-2 pointer/import path, until a match has passed with it) */

/* call_indirect: an address in the module area. Returns 0 if the address is not in that area. */
int nro_call_indirect(Ctx *c, uint64_t addr) {
    if (addr < NRO_BASE || addr >= NRO_BASE + ((uint64_t)NRO_MAX_SLOTS << NRO_SLOT_SHIFT)) return 0;
    NroModule *m = g_slot[(addr - NRO_BASE) >> NRO_SLOT_SHIFT];
    ImportFn fn = m ? fn_in_table(m->fns, m->fn_count, addr) : NULL;
    if (fn && modcov_on()) modcov_mark(m, addr);
    if (fn && (g_fs_recording || g_fs_replaying) && fighter_rec_pointer(c, addr, fn)) return 1;  /* S2: differential test: a call through a function pointer is one event */
    if (fn && m->fighter && !g_fs_hookmode && fighter_dispatch(c, m, addr, fn)) return 1;      /* S1: this script function exists as C++ source (S3: with the hook table the function's own entry does this) */
    /* S10 (track S, part D): a function that exists only in a fighter's C++ source has a synthetic address inside its
     * module (an odd offset: never a function entry); fighter_host.c enters the source function */
    if (!fn && m && m->fighter && fighter_dispatch(c, m, addr, NULL)) return 1;
    if (!fn) {
        if (m) {
            printf("\nindirect call to %s+0x%llx, which is not a lifted function entry (called from near 0x%llx, f_%llx)\n",
                   m->info->name, (unsigned long long)(addr - m->info->base), (unsigned long long)c->x[30], (unsigned long long)c->fn);
            guest_trap("indirect call into a module at an address that is not a lifted function", addr);
        }
        guest_trap("indirect call into a module that is not loaded", addr);
    }
    fn(c);
    return 1;
}

/* nnunwind.c: the loaded module that contains pc, with its resume table */
int nro_unwind_module(uint64_t pc, uint64_t *base, uint64_t *size, const NroResume **tab, int *count, const char **name) {
    NroModule *m = module_at(pc);
    if (!m || !m->loaded) return 0;
    *base = m->info->base; *size = m->info->image_size; *tab = m->resume; *count = m->resume_count; *name = m->info->name;
    return 1;
}

/* crash reports: guest function whose host code contains `rip`, or 0 */
uint64_t nro_guest_function_of(uint64_t rip) {
    for (int i = 0; g_mods && i < g_mod_count; i++) {
        NroModule *m = &g_mods[i];
        if (!m->by_host || rip < (uintptr_t)m->by_host[0].fn) continue;
        int lo = 0, hi = m->fn_count - 1, best = -1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if ((uintptr_t)m->by_host[mid].fn <= rip) { best = mid; lo = mid + 1; } else hi = mid - 1;
        }
        if (best >= 0 && rip - (uintptr_t)m->by_host[best].fn < 0x400000) return m->by_host[best].addr;
    }
    return 0;
}

/* ---------------------------------------------------------------- symbol resolution
 * Order: main's exports, the module itself, the other loaded modules, then whatever main itself uses
 * for the same imported name (a host handler behind a placeholder address, or redirected SDK data). */
static uint64_t resolve(NroModule *m, int sym, int *from_main) {
    const DynSym *s = &m->syms[sym];
    if (from_main) *from_main = 0;
    if (!s->name[0]) return s->defined ? m->info->base + s->value : 0;
    int mi = main_find(s->name);
    if (mi >= 0 && dyn_syms[mi].defined) { if (from_main) *from_main = 1; return GUEST_BASE + dyn_syms[mi].value; }
    if (s->defined) return m->info->base + s->value;
    const Export *e = exp_find(s->name);
    if (e) return e->value;
    if (m->plugin) {    /* skyline.c: names the game never imports (only plugins do): host handlers, SDK data, SDK addresses */
        if (mi >= 0 && import_lookup(s->name)) return import_address(mi);
        ImportFn x = extra_import_lookup(s->name);
        if (x) return hostfn_address(s->name, x);
        uint64_t d = sky_data_symbol(s->name);
        if (!d) d = sky_sdk_symbol(s->name);
        if (d) return d;
    }
    if (mi >= 0) return import_address(mi);
    return 0;                                   /* __rel_dyn_start and friends: weak, stay 0 */
}

static ImportFn function_at(uint64_t addr) {
    if (addr >= GUEST_BASE && addr < NRO_BASE) return fn_in_table(fn_table, fn_table_count, addr);
    NroModule *m = module_at(addr);
    return m ? fn_in_table(m->fns, m->fn_count, addr) : NULL;
}

#include "fighter_sdk/hook_host.c"             /* S3: the hook table: replace any lifted function by address or name (track M) */
#include "fighter_sdk/fighter_host.c"          /* S1: fighter C++ source: loading, per-function switch, differential test */

/* PLT stub of a lifted module: `void f_<stub>(Ctx *c) { nro_import(c, &nromod_<name>, <symbol index>); }`,
 * forwarded here by the module DLL with the NroModule it was bound to. */
static void nro_import_host_inner(Ctx *c, void *module, int sym);
static void nro_import_host(Ctx *c, void *module, int sym) {
    if (g_fs_replaying && fighter_replay_import(c, module, sym)) return;      /* S2: lifted code run inside a replay: answered from the recording */
    if (!g_fs_recording) {
        if (g_fs_export_mod && !g_fs_hookmode && fighter_export_override(c, ((NroModule *)module)->bind_addr[sym], ((NroModule *)module)->bind_fn[sym])) return;   /* S2: a function of lua2cpp_common that exists as source */
        nro_import_host_inner(c, module, sym); return;
    }
    FsEvent *e = fighter_rec_begin(c, module, sym);     /* S1 differential test: this call is part of the recording */
    if (e && fighter_synth_answer(c, e)) return;
    nro_import_host_inner(c, module, sym);
    fighter_rec_end(c, e);
}
static void nro_import_host_inner(Ctx *c, void *module, int sym) {
    NroModule *m = module;
    ImportFn fn = m->bind_fn[sym];
    extern volatile LONG g_guest_frozen;        /* hostrt.c: the application is suspended (close4) */
    void guest_freeze_wait(void);
    if (g_guest_frozen) guest_freeze_wait();
    if (m->got && m->got[sym]) {                /* a plugin rewrote the GOT entry (smashline's rtld crate does): call what it holds now */
        uint64_t cur = rd64(m->got[sym]);
        if (cur != m->bind_addr[sym] && cur) { call_indirect(c, cur); return; }
    }
    if (fn) { fn(c); return; }
    uint64_t addr = m->bind_addr[sym];
    if (!addr) {                                /* provider was not loaded when this module was bound: try again */
        addr = resolve(m, sym, NULL);
        if (!addr) {
            printf("\nmodule %s calls %s, which nothing provides\n", m->info->name, m->syms[sym].name);
            guest_trap("call to an unresolved symbol from a module", m->info->base);
        }
        m->bind_addr[sym] = addr;
        if ((fn = function_at(addr)) != NULL) { m->bind_fn[sym] = fn; fn(c); return; }
    }
    call_indirect(c, addr);                     /* SDK function: main's import placeholder -> host handler, traced */
}

static void bind_symbols(NroModule *m, int *unresolved) {
    if (!m->bind_fn) {
        m->bind_fn = calloc((size_t)m->sym_count, sizeof *m->bind_fn);
        m->bind_addr = calloc((size_t)m->sym_count, sizeof *m->bind_addr);
    }
    for (int i = 0; i < m->sym_count; i++) {
        uint64_t a = resolve(m, i, NULL);
        m->bind_addr[i] = a;
        m->bind_fn[i] = a ? function_at(a) : NULL;
        if (!a && !m->syms[i].defined && m->syms[i].name[0] && strncmp(m->syms[i].name, "__rel_", 6) != 0) {
            if ((*unresolved)++ < 8) trace_printf("  %s: unresolved symbol %s\n", m->info->name, m->syms[i].name);
        }
    }
}

static void find_dynamic(NroModule *m, uint64_t *rela, uint64_t *relasz, uint64_t *jmprel, uint64_t *pltrelsz) {
    uint64_t base = m->info->base;
    uint32_t mod0 = rd32(base + 4);
    *rela = *relasz = *jmprel = *pltrelsz = 0;
    m->dt_init = m->dt_fini = 0;
    for (uint64_t dyn = base + mod0 + (int32_t)rd32(base + mod0 + 4); rd64(dyn); dyn += 16) {
        uint64_t tag = rd64(dyn), val = rd64(dyn + 8);
        if (tag == 7) *rela = val; else if (tag == 8) *relasz = val;            /* DT_RELA, DT_RELASZ */
        else if (tag == 23) *jmprel = val; else if (tag == 2) *pltrelsz = val;  /* DT_JMPREL, DT_PLTRELSZ */
        else if (tag == 12) m->dt_init = base + val;
        else if (tag == 13) m->dt_fini = base + val;
    }
}

static int relocate(NroModule *m, int *preempted) {
    uint64_t base = m->info->base, tab[2], size[2];
    find_dynamic(m, &tab[0], &size[0], &tab[1], &size[1]);
    int bad = 0, keep_got = sky_registry() > 0;         /* GOT entries are noted only with plugins: they rewrite them */
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < size[t] / 24; i++) {
            uint64_t e = base + tab[t] + i * 24;
            uint64_t off = rd64(e), info = rd64(e + 8), addend = rd64(e + 16);
            uint32_t type = (uint32_t)info;
            if (type == 0x403) wr64(base + off, base + addend);                             /* R_AARCH64_RELATIVE */
            else if (type == 0x101 || type == 0x401 || type == 0x402) {                     /* ABS64, GLOB_DAT, JUMP_SLOT */
                int from_main, sym = (int)(info >> 32);
                uint64_t s = sym < m->sym_count ? resolve(m, sym, &from_main) : 0;
                if (s && from_main && m->syms[sym].defined) (*preempted)++;
                wr64(base + off, s ? s + (type == 0x101 ? addend : 0) : 0);
                if (type != 0x101 && sym < m->sym_count && keep_got) {
                    if (!m->got) m->got = calloc((size_t)m->sym_count, sizeof *m->got);
                    m->got[sym] = base + off;
                }
            } else if (bad++ < 8) trace_printf("  %s: unsupported relocation type 0x%x at +0x%llx\n", m->info->name, type, (unsigned long long)off);
        }
    return bad;
}

static int cmp_host(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)((const FnEntry *)a)->fn, y = (uintptr_t)((const FnEntry *)b)->fn;
    return x < y ? -1 : x > y;
}

void code_prefetch(HMODULE m);                               /* boot.c: read a module's code in ahead (pf1) */
void sky_site_inline(Ctx *c, uint64_t pc);                   /* skyline.c (included below) */
int sky_site_midhook(Ctx *c, uint64_t pc);
static const NroHost g_host = {call_indirect, host_import, guest_trap, guest_unimpl, guest_svc, stx_diag, nro_import_host, nro_catch_call, sky_site_inline, sky_site_midhook};

/* Load nro_bin\<module>.dll (next to the exe) and take its tables. */
static int attach_dll(NroModule *m) {
    if (m->dll) return 1;
    char path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
    const char *bin = getenv("SSBU_NRO_BIN");               /* folder of the module DLLs next to the exe (default nro_bin) */
    if (m->plugin) bin = sky_bin_dir();                     /* skyline.c: plugin_bin (SSBU_PLUGIN_BIN) */
    snprintf(path + n, sizeof path - n, "%s\\%s.dll", bin && bin[0] ? bin : "nro_bin_eh_o2", m->info->name);      /* P3 (2026-10-09): the optimised modules are the default; SSBU_NRO_BIN=nro_bin_eh = the -O0 ones */
    HMODULE dll = NULL;
    if (!m->plugin && sky_registry() > 0) {                 /* skyline.c: a module re-lifted with the plugins' sites (relift_sites.py --module) */
        char hp[MAX_PATH * 2];
        snprintf(hp, sizeof hp, "%.*snro_bin_sky\\%s.dll", (int)n, path, m->info->name);
        dll = LoadLibraryA(hp);
        if (dll) printf("nn::ro: %s from %s\n", m->info->name, hp);
    }
    if (!dll) dll = LoadLibraryA(path);
    const char *python = getenv("SSBU_NRO_AUTOBUILD");       /* development aid: path of python.exe; lift a missing module on demand */
    if (!dll && python && python[0]) {
        char cmd[MAX_PATH * 4];
        snprintf(cmd, sizeof cmd, "\"\"%s\" \"%.*sbuild.py\" nro %s\"", python, (int)n, path, m->info->name);
        printf("nn::ro::LoadModule: %s has no lifted code yet, building it: %s\n", m->info->name, cmd);
        DWORD t0 = GetTickCount();
        int rc = system(cmd);
        printf("nn::ro::LoadModule: build of %s finished with code %d after %.1f s\n", m->info->name, rc, (GetTickCount() - t0) / 1000.0);
        dll = LoadLibraryA(path);
    }
    if (!dll) return 0;
    NroBindFn bind = (NroBindFn)(void *)GetProcAddress(dll, "nro_bind");
    if (!bind) { FreeLibrary(dll); return 0; }
    NroTables t;
    memset(&t, 0, sizeof t);
    bind(&g_host, m, &t);
    m->fns = t.fns; m->fn_count = t.fn_count; m->syms = t.syms; m->sym_count = t.sym_count;
    m->resume = t.resume; m->resume_count = t.resume_count;
    m->by_host = malloc((size_t)m->fn_count * sizeof *m->by_host);
    memcpy(m->by_host, m->fns, (size_t)m->fn_count * sizeof *m->by_host);
    qsort(m->by_host, (size_t)m->fn_count, sizeof *m->by_host, cmp_host);
    m->dll = dll;
    code_prefetch(dll);
    return 1;
}

static void imp_unloaded(Ctx *c) { guest_trap("call to a function of a module that was unloaded", c->x[30] - 4); }

/* main calls the modules' exports (lua2cpp::create_agent_*) through its own PLT, which the console binds
 * lazily: while a module is loaded its exports are the handlers of main's imports of the same name. */
static int publish_exports(NroModule *m, int on) {
    int n = 0;
    for (int i = 1; i < m->sym_count; i++) {
        const DynSym *s = &m->syms[i];
        if (!s->defined || !s->name[0]) continue;
        int mi = main_find(s->name);
        if (mi < 0 || dyn_syms[mi].defined) continue;
        ImportDef d = { s->name, on ? fn_in_table(m->fns, m->fn_count, m->info->base + s->value) : imp_unloaded };
        if (!d.fn) continue;                    /* a data symbol */
        register_imports(&d, 1);
        n++;
    }
    return n;
}

/* The SDK's LoadModule ends with BindVariables of every other loaded module against the new one: imports that nothing
 * provided when those modules were loaded get their address now (a plugin loaded at boot imports lua2cpp_common's
 * functions, which load with the first fight). Every relocation of such a symbol is rewritten: a symbol can have a PLT
 * slot (calls) and GOT slots (its address taken). Under g_ro_lock. Returns the number of symbols bound. Only with plugins
 * (skyline.c): without them the game's modules keep the binding they had, and an import resolved late is found by
 * nro_import_host_inner when it is called, as before. */
static void find_dynamic(NroModule *m, uint64_t *rela, uint64_t *relasz, uint64_t *jmprel, uint64_t *pltrelsz);
static int rebind_others(NroModule *nm) {
    int n = 0;
    if (sky_registry() <= 0) return 0;
    for (int i = 0; i < g_mod_count; i++) {
        NroModule *m = &g_mods[i];
        if (m == nm || !m->loaded || !m->bind_addr) continue;
        uint8_t *fresh = NULL;
        for (int k = 1; k < m->sym_count; k++) {
            if (m->bind_addr[k] || m->syms[k].defined || !m->syms[k].name[0]) continue;
            const Export *e = exp_find(m->syms[k].name);
            if (!e || e->mod != nm) continue;
            m->bind_addr[k] = e->value;
            m->bind_fn[k] = function_at(e->value);
            if (!fresh) fresh = calloc((size_t)m->sym_count, 1);
            fresh[k] = 1;
            n++;
        }
        if (!fresh) continue;
        uint64_t base = m->info->base, tab[2], size[2];
        find_dynamic(m, &tab[0], &size[0], &tab[1], &size[1]);
        for (int t = 0; t < 2; t++)
            for (uint64_t r = 0; r < size[t] / 24; r++) {
                uint64_t e = base + tab[t] + r * 24, off = rd64(e), info = rd64(e + 8), addend = rd64(e + 16);
                uint32_t type = (uint32_t)info, sym = (uint32_t)(info >> 32);
                if ((type == 0x101 || type == 0x401 || type == 0x402) && sym < (uint32_t)m->sym_count && fresh[sym])
                    wr64(base + off, m->bind_addr[sym] + (type == 0x101 ? addend : 0));
            }
        free(fresh);
    }
    return n;
}

static NroModule *module_of_image(uint64_t nro) {
    for (int i = 0; i < g_mod_count; i++)
        if (memcmp(g_mods[i].info->build_id, (const void *)(uintptr_t)(nro + 0x40), 20) == 0) return &g_mods[i];
    return NULL;
}

/* ---------------------------------------------------------------- nn::ro */
static void ro_Initialize(Ctx *c) { ro_init_tables(); c->x[0] = RO_OK; }

/* Module registration (NRR) and the hash check of every loaded module.
 * [sdk] 0x3471f0 -> 0x347220 RegisterModuleInfo(info, nrr): "NRR0" at +0 or Result 0xc16; then one request to the
 *   ro module (process id, nrr address, size = u32[nrr + 0x338]); a Result of module 22 with description 1..0x3ff
 *   is returned to the caller, any other failure aborts; on success info+0 = 1, info+8 = nrr and the node at
 *   info+0x10 goes into the SDK's own list.
 * [sdk] 0x347370 UnregisterModuleInfo(info): unlinks the node, asks the ro module to forget the NRR at info+8
 *   (failure aborts), info+0 = 0.
 * The ro module's side (not in the dump), after [doc] suyu core/hle/service/ro/ro.cpp + ro_types.h:
 *   RegisterModuleInfo: address not page aligned -> description 1025, size zero or not page aligned -> 1026
 *   (both above 0x3ff: the SDK aborts); no free slot of 256 -> 8 (0x1016); otherwise it keeps the NRR's list of
 *   SHA-256 values: u32 count at nrr+0x344, at offset u32[nrr+0x340]. (The NRR's own RSA signatures are verified
 *   by the console's module with keys that are not in the dump: NOT checked here, as in the reference.)
 *   LoadModule (ValidateHasNroHash): SHA-256 over the NRO image (u32[nro+0x18] bytes as the game hands it over)
 *   must be in the list of a registered NRR, else description 9 (0x1216, "not authorized").
 *   UnregisterModuleInfo: an address that is not registered -> 1029 (the SDK aborts). */
#define RO_TOO_MANY_NRR 0x1016u
#define RO_NOT_AUTHORIZED 0x1216u
#define RO_MAX_NRR 0x100
static struct { uint64_t addr; uint32_t count; uint8_t *hashes; } g_nrr[RO_MAX_NRR];
static SRWLOCK g_nrr_lock = SRWLOCK_INIT;

static int sha256(const void *data, size_t size, uint8_t out[32]) {      /* Windows CNG (bcrypt.dll) */
    typedef LONG (WINAPI *open_fn)(void **, const WCHAR *, const WCHAR *, ULONG);
    typedef LONG (WINAPI *hash_fn)(void *, UCHAR *, ULONG, UCHAR *, ULONG, UCHAR *, ULONG);
    static void *alg;
    static hash_fn hash;
    if (!hash) {
        HMODULE m = LoadLibraryA("bcrypt.dll");
        open_fn open = m ? (open_fn)(void *)GetProcAddress(m, "BCryptOpenAlgorithmProvider") : NULL;
        hash_fn h = m ? (hash_fn)(void *)GetProcAddress(m, "BCryptHash") : NULL;
        if (!open || !h || open(&alg, L"SHA256", NULL, 0) != 0) return 0;
        hash = h;
    }
    if (size > 0x7fffffffu) return 0;                                    /* BCryptHash takes 32-bit lengths; modules are far smaller */
    return hash(alg, NULL, 0, (UCHAR *)(uintptr_t)data, (ULONG)size, out, 32) == 0;
}

static int nro_hash_registered(uint64_t nro, uint8_t hash[32]) {
    int found = 0;
    if (!sha256((const void *)(uintptr_t)nro, rd32(nro + 0x18), hash)) return -1;
    AcquireSRWLockShared(&g_nrr_lock);
    for (int i = 0; i < RO_MAX_NRR && !found; i++)
        for (uint32_t k = 0; g_nrr[i].hashes && k < g_nrr[i].count && !found; k++)
            found = memcmp(g_nrr[i].hashes + (size_t)k * 32, hash, 32) == 0;
    ReleaseSRWLockShared(&g_nrr_lock);
    return found;
}

static void ro_RegisterModuleInfo(Ctx *c) {      /* (RegistrationInfo *info, const void *nrr) */
    uint64_t info = c->x[0], nrr = c->x[1];
    uint32_t size, off, count;
    int slot = -1;
    if (rd32(nrr) != 0x3052524eu) { c->x[0] = RO_INVALID_NRR; return; }      /* "NRR0" */
    size = rd32(nrr + 0x338); off = rd32(nrr + 0x340); count = rd32(nrr + 0x344);
    if ((nrr & 0xfff) || size == 0 || (size & 0xfff))
        guest_trap("nn::ro::RegisterModuleInfo: NRR address or size not page aligned (the ro module refuses it and the SDK aborts, sdk 0x347300)", c->x[30] - 4);
    if ((uint64_t)off + (uint64_t)count * 32 > size)
        guest_trap("nn::ro::RegisterModuleInfo: the NRR's hash list does not fit in its size", c->x[30] - 4);
    AcquireSRWLockExclusive(&g_nrr_lock);
    for (int i = 0; i < RO_MAX_NRR && slot < 0; i++) if (!g_nrr[i].hashes) slot = i;
    if (slot >= 0) {
        g_nrr[slot].hashes = malloc((size_t)count * 32 + 1);
        memcpy(g_nrr[slot].hashes, (const void *)(uintptr_t)(nrr + off), (size_t)count * 32);
        g_nrr[slot].count = count; g_nrr[slot].addr = nrr;
    }
    ReleaseSRWLockExclusive(&g_nrr_lock);
    if (slot < 0) { c->x[0] = RO_TOO_MANY_NRR; return; }
    wr32(info, 1);
    wr64(info + 8, nrr);
    wr64(info + 0x10, info + 0x10);              /* list node (the SDK's own list; nothing else reads it) */
    wr64(info + 0x18, info + 0x10);
    trace_printf("nn::ro::RegisterModuleInfo: NRR at %llx, size 0x%x, %u module hashes\n", (unsigned long long)nrr, size, count);
    c->x[0] = RO_OK;
}

static void ro_UnregisterModuleInfo(Ctx *c) {    /* (RegistrationInfo *info) */
    uint64_t info = c->x[0], nrr = rd64(info + 8);
    int slot = -1;
    AcquireSRWLockExclusive(&g_nrr_lock);
    for (int i = 0; i < RO_MAX_NRR && slot < 0; i++) if (g_nrr[i].hashes && g_nrr[i].addr == nrr) slot = i;
    if (slot >= 0) { free(g_nrr[slot].hashes); memset(&g_nrr[slot], 0, sizeof g_nrr[slot]); }
    ReleaseSRWLockExclusive(&g_nrr_lock);
    if (slot < 0)
        guest_trap("nn::ro::UnregisterModuleInfo: this NRR is not registered (the ro module refuses it and the SDK aborts, sdk 0x34743c)", c->x[30] - 4);
    wr32(info, 0);
    c->x[0] = RO_OK;
}

int nnro_selftest(Ctx *c) {
    static uint8_t page[0x2000], nro[0x1000];
    static const uint8_t abc[32] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
                                    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    uint8_t h[32], info[0x20];
    uint64_t lr = c->x[30], nrr = ((uint64_t)(uintptr_t)page + 0xfff) & ~(uint64_t)0xfff;
    int ok = sha256("abc", 3, h) && !memcmp(h, abc, 32), ok2 = 1;
    printf("  ro: SHA-256 of \"abc\" (FIPS 180 example): %s\n", ok ? "ok" : "FAILED");
    memset(nro, 0x5a, sizeof nro); wr32((uint64_t)(uintptr_t)nro + 0x18, 0x800);
    ok2 &= nro_hash_registered((uint64_t)(uintptr_t)nro, h) == 0;                         /* nothing registered: refused */
    memset((void *)(uintptr_t)nrr, 0, 0x1000);
    wr32(nrr, 0x3052524eu); wr32(nrr + 0x338, 0x1000); wr32(nrr + 0x340, 0x350); wr32(nrr + 0x344, 2);
    memcpy((void *)(uintptr_t)(nrr + 0x350 + 32), h, 32);                                 /* second entry of the list */
    c->x[0] = (uint64_t)(uintptr_t)info; c->x[1] = nrr; ro_RegisterModuleInfo(c);
    ok2 &= c->x[0] == RO_OK && rd32((uint64_t)(uintptr_t)info) == 1 && rd64((uint64_t)(uintptr_t)info + 8) == nrr;
    ok2 &= nro_hash_registered((uint64_t)(uintptr_t)nro, h) == 1;
    nro[0x700] ^= 1; ok2 &= nro_hash_registered((uint64_t)(uintptr_t)nro, h) == 0;       /* one bit changed: refused */
    nro[0x700] ^= 1;
    wr32(nrr, 0); c->x[0] = (uint64_t)(uintptr_t)info; c->x[1] = nrr; ro_RegisterModuleInfo(c); ok2 &= c->x[0] == RO_INVALID_NRR;
    wr32(nrr, 0x3052524eu);
    c->x[0] = (uint64_t)(uintptr_t)info; ro_UnregisterModuleInfo(c);
    ok2 &= c->x[0] == RO_OK && rd32((uint64_t)(uintptr_t)info) == 0 && nro_hash_registered((uint64_t)(uintptr_t)nro, h) == 0;
    c->x[30] = lr;
    printf("  ro: NRR registration: unlisted image refused, listed accepted, changed bit refused, bad magic 0xc16, unregistered refused again: %s\n", ok2 ? "ok" : "FAILED");
    return !(ok && ok2);
}

static void ro_GetBufferSize(Ctx *c) {           /* (size_t *out, const void *nro) */
    uint64_t out = c->x[0], nro = c->x[1];
    if (rd32(nro + 0x10) != 0x304f524eu) { c->x[0] = RO_INVALID_NRO; return; }   /* "NRO0" */
    wr64(out, (uint64_t)rd32(nro + 0x38));
    c->x[0] = RO_OK;
}

static void ro_LoadModule(Ctx *c) {              /* (Module *out, const void *nro, void *bss, size_t bssSize, int flag) */
    uint64_t out = c->x[0], nro = c->x[1], bss = c->x[2], bss_size = c->x[3];
    int flag = (int)c->x[4];
    ro_init_tables();
    if (rd32(nro + 0x10) != 0x304f524eu) { c->x[0] = RO_INVALID_NRO; return; }
    NroModule *pm = module_of_image(nro);
    if (!pm || !pm->plugin) {                    /* Skyline plugins: installed by the user, not checked */
        uint8_t hash[32];
        int listed = nro_hash_registered(nro, hash);
        if (listed < 0) guest_trap("nn::ro::LoadModule: SHA-256 is not available on this Windows (bcrypt.dll): the module cannot be checked", c->x[30] - 4);
        if (!listed) {
            printf("nn::ro::LoadModule: the NRO at %llx (0x%x bytes) is in no registered NRR: refused (0x1216)\n", (unsigned long long)nro, rd32(nro + 0x18));
            trace_printf("nn::ro::LoadModule: NRO hash not in a registered NRR -> 0x1216\n");
            c->x[0] = RO_NOT_AUTHORIZED; return;
        }
    }
    NroModule *m = module_of_image(nro);
    if (!m) {
        printf("nn::ro::LoadModule: build id of the NRO at %llx is not in nro_registry.c\n", (unsigned long long)nro);
        trace_printf("nn::ro::LoadModule: unknown NRO (build id not in the registry)\n");
        c->x[0] = RO_INVALID_NRO; return;
    }
    const NroInfo *in = m->info;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceCounter(&t0);
    AcquireSRWLockExclusive(&g_ro_lock);
    const char *why = NULL;
    size_t span = ((size_t)in->image_size + 0xffff) & ~(size_t)0xffff;
    int unresolved = 0, preempted = 0, published = 0;
    if (m->loaded) why = "already loaded";
    else if (rd32(nro + 0x18) != in->file_size || (uint64_t)rd32(nro + 0x18) + rd32(nro + 0x38) != in->image_size) why = "sizes differ from the registry";
    else if (!attach_dll(m)) why = "no lifted code: run `python build.py nro <module>`";
    else if (VirtualAlloc((void *)(uintptr_t)in->base, span, g_ro_reserved ? MEM_COMMIT : MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) != (void *)(uintptr_t)in->base) {
        /* say why: the Windows error and what is at that address (1455 = commit limit, 487 = address in use) */
        static char msg[200];
        DWORD err = GetLastError();
        MEMORY_BASIC_INFORMATION mi;
        memset(&mi, 0, sizeof mi);
        VirtualQuery((void *)(uintptr_t)in->base, &mi, sizeof mi);
        snprintf(msg, sizeof msg, "cannot map its address range (Windows error %lu; at %llx: allocation base %p, region size 0x%llx, state 0x%lx, type 0x%lx; range %s)",
                 (unsigned long)err, (unsigned long long)in->base, mi.AllocationBase, (unsigned long long)mi.RegionSize, (unsigned long)mi.State, (unsigned long)mi.Type,
                 g_ro_reserved ? "reserved at start-up" : "not reserved");
        why = msg;
    }
    if (why) {
        ReleaseSRWLockExclusive(&g_ro_lock);
        printf("nn::ro::LoadModule: %s NOT loaded: %s\n", in->name, why);
        trace_printf("nn::ro::LoadModule: %s NOT loaded: %s\n", in->name, why);
        c->x[0] = RO_INVALID_NRO; return;
    }
    memcpy((void *)(uintptr_t)in->base, (const void *)(uintptr_t)nro, in->file_size);     /* text + rodata + data */
    uint32_t mod0 = rd32(in->base + 4);
    uint64_t mh = in->base + mod0;
    /* fresh pages are zero, so the bss after the file image already is; the part of the bss range inside the file is cleared like the SDK does */
    uint64_t z0 = mh + (int32_t)rd32(mh + 8), z1 = mh + (int32_t)rd32(mh + 0xc);
    if (z1 > in->base + in->file_size) z1 = in->base + in->file_size;
    if (z0 < z1) memset((void *)(uintptr_t)z0, 0, (size_t)(z1 - z0));
    int bad = relocate(m, &preempted);
    bind_symbols(m, &unresolved);
    for (int i = 1; i < m->sym_count; i++)
        if (m->syms[i].defined && m->syms[i].name[0]) exp_add(m->syms[i].name, m, in->base + m->syms[i].value);
    uint64_t obj = mh + (int32_t)rd32(mh + 0x18);       /* the loader's RoModule object inside the image */
    wr64(obj, obj); wr64(obj + 8, obj);
    wr64(obj + 0x20, in->base);
    if (sky_registry() > 0) sky_module_loaded(m, obj);      /* skyline.c (plugins only): the whole rtld ModuleObject, in g_pManualLoadList */
    /* close5: the module's text and read-only data (NRO header: data segment offset at +0x30) are read-only after relocation,
     * as nn::ro maps them on the console */
    if (!getenv("SSBU_NO_PROTECT")) {
        DWORD oldp; uint32_t data_off = rd32(nro + 0x30) & ~0xfffu;
        if (data_off && data_off <= in->file_size) VirtualProtect((void *)(uintptr_t)in->base, data_off, PAGE_READONLY, &oldp);
    }
    m->loaded = 1;
    for (uint64_t s = 0; s < (span + (1u << NRO_SLOT_SHIFT) - 1) >> NRO_SLOT_SHIFT; s++)
        g_slot[((in->base - NRO_BASE) >> NRO_SLOT_SHIFT) + s] = m;
    published = publish_exports(m, 1);
    int rebound = rebind_others(m);              /* after m->loaded: exp_find skips modules not loaded */
    ReleaseSRWLockExclusive(&g_ro_lock);
    int eh = eh_register_image(in->base, in->image_size, 0);        /* its .eh_frame_hdr, for C++ exceptions (after relocation) */
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&freq);
    char line[400];
    snprintf(line, sizeof line, "nn::ro::LoadModule: %s at %llx (image 0x%x, bss buffer %llx size 0x%llx, flag %d): %d functions, %d symbols, "
             "%d taken from main although defined here, %d unresolved, %d bad relocations, %d exports bound to main's imports, %d landing pads%s, %.0f ms",
             in->name, (unsigned long long)in->base, in->image_size, (unsigned long long)bss, (unsigned long long)bss_size, flag,
             m->fn_count, m->sym_count, preempted, unresolved, bad, published, m->resume_count,
             !eh ? " (NO unwind tables)" : m->resume ? "" : " (DLL built without resume entries: exceptions cannot be caught in it)", 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart);
    printf("%s\n", line);
    trace_printf("%s\n", line);
    if (rebound) printf("nn::ro::LoadModule: %d imports of other modules bound to %s\n", rebound, in->name);
    wr64(out, obj); wr32(out + 8, 1); wr64(out + 0x10, nro); wr64(out + 0x18, bss);
    fighter_attach(m);                           /* S1: fighters_bin\<name>.dll, if this module has C++ source */
    hook_module_loaded(m);                       /* S3: hooks that were waiting for a name this module exports (fighter_sdk\hook_host.c) */
    if (g_hk_selftest) hook_selftest_module(c, m);                                  /* S3: SSBU_HOOK_SELFTEST */
    if (m->dt_init && !g_fs_offline) {         /* the module's _init: __nnmusl_init_dso, then its init_array (S2: not in the offline test) */
        uint64_t saved[31];
        memcpy(saved, c->x, sizeof saved);
        call_guest(c, m->dt_init);
        memcpy(c->x, saved, sizeof saved);
        trace_printf("nn::ro::LoadModule: %s DT_INIT (%llx) returned\n", in->name, (unsigned long long)m->dt_init);
        printf("nn::ro::LoadModule: %s DT_INIT returned\n", in->name);
    }
    { char mark[64]; snprintf(mark, sizeof mark, "module:%s", in->name); input_mark(mark); }      /* scripted input may wait for it */
    c->x[0] = RO_OK;
}

/* (rb5) rollback_controller.c: a lua2cpp_* module unloaded = the results screen of a match was left (its fighters go) */
volatile long g_ro_script_unloads;
static void ro_UnloadModule(Ctx *c) {            /* (Module *) */
    uint64_t g = c->x[0], obj = rd64(g);
    NroModule *m = obj ? module_at(obj) : NULL;
    if (!m || !m->loaded) { trace_printf("nn::ro::UnloadModule: module object %llx is not a loaded module\n", (unsigned long long)obj); return; }
    if (m->dt_fini) {
        uint64_t saved[31];
        memcpy(saved, c->x, sizeof saved);
        call_guest(c, m->dt_fini);
        memcpy(c->x, saved, sizeof saved);
    }
    eh_unregister_image(m->info->base);
    fighter_detach(m);                           /* S1 */
    AcquireSRWLockExclusive(&g_ro_lock);
    sky_module_unloaded(m);
    publish_exports(m, 0);
    m->loaded = 0;
    size_t span = ((size_t)m->info->image_size + 0xffff) & ~(size_t)0xffff;
    for (uint64_t s = 0; s < (span + (1u << NRO_SLOT_SHIFT) - 1) >> NRO_SLOT_SHIFT; s++)
        g_slot[((m->info->base - NRO_BASE) >> NRO_SLOT_SHIFT) + s] = NULL;
    if (g_ro_reserved) VirtualFree((void *)(uintptr_t)m->info->base, span, MEM_DECOMMIT);      /* the range stays reserved for the next load */
    else VirtualFree((void *)(uintptr_t)m->info->base, 0, MEM_RELEASE);
    ReleaseSRWLockExclusive(&g_ro_lock);
    wr32(g + 8, 0);
    printf("nn::ro::UnloadModule: %s\n", m->info->name);
    if (!strncmp(m->info->name, "lua2cpp_", 8)) InterlockedIncrement(&g_ro_script_unloads);
    trace_printf("nn::ro::UnloadModule: %s\n", m->info->name);
}

/* close5 test (SSBU_RO_RELOAD_TEST=1): every module the game loads is at once unloaded (its _fini runs, its range must no longer
 * be committed, the Module object must say "not loaded") and loaded again with the same arguments; the game then goes on with
 * the reloaded image. Counters are printed per module and summed in ro_reload_report (boot.c calls nothing: see the log lines). */
static int g_reload_ok, g_reload_bad;
static void ro_LoadModule_test(Ctx *c) {
    if (!getenv("SSBU_RO_RELOAD_TEST")) { ro_LoadModule(c); return; }
    uint64_t a[5]; memcpy(a, c->x, sizeof a);
    ro_LoadModule(c);
    if (c->x[0] != RO_OK) return;
    uint64_t obj = rd64(a[0]);
    NroModule *m = module_at(obj);
    if (!m) { g_reload_bad++; return; }
    uint64_t base = m->info->base;
    c->x[0] = a[0]; ro_UnloadModule(c);
    MEMORY_BASIC_INFORMATION mi; memset(&mi, 0, sizeof mi);
    VirtualQuery((void *)(uintptr_t)base, &mi, sizeof mi);
    int gone = mi.State != MEM_COMMIT && rd32(a[0] + 8) == 0 && !m->loaded;
    memcpy(c->x, a, sizeof a);
    ro_LoadModule(c);
    MEMORY_BASIC_INFORMATION m2; memset(&m2, 0, sizeof m2);
    VirtualQuery((void *)(uintptr_t)base, &m2, sizeof m2);
    int back = c->x[0] == RO_OK && rd64(a[0]) == obj && m->loaded && rd32(a[0] + 8) == 1 && m2.State == MEM_COMMIT
               && (getenv("SSBU_NO_PROTECT") || m2.Protect == PAGE_READONLY);
    if (gone && back) g_reload_ok++; else g_reload_bad++;
    printf("nn::ro reload test: %s: after unload %s, loaded again %s (text protection 0x%lx); total %d ok, %d FAILED\n", m->info->name,
           gone ? "released" : "STILL MAPPED", back ? "ok" : "FAILED", (unsigned long)m2.Protect, g_reload_ok, g_reload_bad);
}

/* LoadModule(module, nro, bss, size, flag) is LoadModuleInternal(..., false) in the SDK; plugins hook either one (nro_hook the
 * first, smashline the second), so the 5-argument form goes through the host function the plugins see for the second. */
#define RO_LMI "_ZN2nn2ro18LoadModuleInternalEPNS0_6ModuleEPKvPvmib"
static void ro_LoadModuleInternal(Ctx *c) { ro_LoadModule_test(c); }
static void ro_LoadModule_entry(Ctx *c) {
    static uint64_t lmi;
    if (sky_registry() <= 0) { ro_LoadModule_test(c); return; }     /* no plugins: as before */
    if (!lmi) lmi = hostfn_address(RO_LMI, ro_LoadModuleInternal);
    c->x[5] = 0;
    call_indirect(c, lmi);
}

#include "skyline.c"                           /* Skyline plugins (docs/SKYLINE.md): loader, Skyline's exports, rtld lists */

/* ---------------------------------------------------------------- C runtime entries the modules' _init / _fini use */
static void musl_init_dso(Ctx *c) { c->x[0] = 2; }      /* anything but 1: the module then runs its own init_array */
static void musl_fini_dso(Ctx *c) { (void)c; }

static const ImportDef ro_imports[] = {
    {"_ZN2nn2ro10InitializeEv", ro_Initialize},
    {"_ZN2nn2ro18RegisterModuleInfoEPNS0_16RegistrationInfoEPKv", ro_RegisterModuleInfo},
    {"_ZN2nn2ro20UnregisterModuleInfoEPNS0_16RegistrationInfoE", ro_UnregisterModuleInfo},
    {"_ZN2nn2ro13GetBufferSizeEPmPKv", ro_GetBufferSize},
    {"_ZN2nn2ro10LoadModuleEPNS0_6ModuleEPKvPvmi", ro_LoadModule_entry},
    {"_ZN2nn2ro12UnloadModuleEPNS0_6ModuleE", ro_UnloadModule},
    {"__nnmusl_init_dso", musl_init_dso}, {"__nnmusl_fini_dso", musl_fini_dso},
    {RO_LMI, ro_LoadModuleInternal},
    {"_ZN2nn2ro12LookupSymbolEPmPKc", ro_LookupSymbol},
    {"_ZN2nn2ro18LookupModuleSymbolEPmPKNS0_6ModuleEPKc", ro_LookupModuleSymbol},
};

/* The modules have fixed guest addresses (0x7200000000 + their slot). Their whole range is reserved at start-up, before
 * the game allocates anything, so that no other allocation of the process (heaps, thread stacks, the graphics driver)
 * can land inside it; LoadModule commits a module's pages, UnloadModule decommits them. */
static void ro_reserve_range(void) {
    uint64_t end = NRO_BASE;
    for (int i = 0; i < nro_registry_count; i++) {
        uint64_t e = nro_registry[i].base + (((uint64_t)nro_registry[i].image_size + 0xffff) & ~(uint64_t)0xffff);
        if (e > end) end = e;
    }
    for (int i = 0, np = sky_registry(); i < np; i++) {          /* Skyline plugins (0x7280000000 ...) */
        uint64_t e = g_sky_info[i].base + (((uint64_t)g_sky_info[i].image_size + 0xffff) & ~(uint64_t)0xffff);
        if (e > end) end = e;
    }
    if (end == NRO_BASE) return;
    if (VirtualAlloc((void *)(uintptr_t)NRO_BASE, (SIZE_T)(end - NRO_BASE), MEM_RESERVE, PAGE_NOACCESS) == (void *)(uintptr_t)NRO_BASE) g_ro_reserved = 1;
    else printf("nn::ro: could not reserve %llx..%llx for the script modules (Windows error %lu): each module is mapped when it is loaded\n",
                (unsigned long long)NRO_BASE, (unsigned long long)end, (unsigned long)GetLastError());
}

void register_ro_imports(void) {
    ro_reserve_range();
    register_imports(ro_imports, (int)(sizeof ro_imports / sizeof ro_imports[0]));
    sky_register();                              /* skyline.c: Skyline's exports; plugins load at the first MountRom */
}

#include "fighter_sdk/newmodule_host.c"        /* N5: the fighter registry (SSBU_CHARACTERS) and script modules that exist only as our DLL; nothing runs without that variable */
