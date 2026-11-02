/* Fighter source: host side. #included by nnro.c (it uses nnro.c's module tables); not compiled on its own
 * (after editing this file, touch nnro.c: build.py looks at nnro.c's date).
 *
 * When the game loads a script module lua2cpp_<name>, fighters_bin\<name>.dll is loaded if it exists
 * (fighter_sdk\tools\build_fighter.py). It lists the script functions it has as C++ source by their offset in the
 * module; nro_call_indirect asks fighter_dispatch first, so the game runs the source instead of the lifted code.
 *
 *   SSBU_FIGHTER_SRC=0            do not load fighter DLLs (everything lifted)
 *   SSBU_FIGHTER_LIFTED=1         load, but run everything lifted;  =name1,name2  only these scripts lifted
 *   SSBU_FIGHTER_DIFF=1           differential test on every call of a script that has source: the lifted function runs
 *                                 for real while its calls into the game are recorded (name, arguments, results); then the
 *                                 source runs from the same arguments with every game call answered from the recording
 *                                 and compared. Equal = same calls in the same order with the same arguments.
 *                                 lib::L2CValue's own value functions (constructors, conversions) are not events: the
 *                                 source builds values natively; what is compared is the VALUE handed to the game.
 *   SSBU_FIGHTER_REPORT=<file>    where the per-function table goes (default fighter_report.txt); rewritten when a
 *                                 module is unloaded and every 2000 dispatched calls
 *   SSBU_FIGHTER_TRACE=<text>     print every game call made by source scripts whose name contains <text>
 *
 * Scripts run on FIBERS (os_threads.c): a script waits inside app::sv_animcmd::frame()/wait() by switching fibers and
 * goes on later, or is ended there for good by an exception (nnunwind.c) when the motion changes. So all state of a
 * running script is per fiber (FLS), a recording can span many game frames, and a recording whose script was ended
 * is compared as a PREFIX ("interrupted"): the source must make the same calls up to the one that never returned.
 */
#include <setjmp.h>
typedef struct { void (*call)(Ctx *, int); void (*log)(const char *); Ctx *(*ctx)(void); void (*call_addr)(Ctx *, uint64_t); } FsHost;
typedef struct { uint32_t off; const char *name; void (*fn)(Ctx *); } FsOverride;
typedef struct { const char *name; const char *spec; } FsSym;
typedef struct { const FsSym *syms; int sym_count; uint64_t *addr; const FsOverride *ovr; int ovr_count; } FsTable;
typedef void (*FsBindFn)(const FsHost *, FsTable *);

typedef struct { long long calls, equal, partial, differ, synth_equal, synth_differ, synth_cut; int lifted; char first[200];
                 long long unreplayable, stale; char why_not[120];
                 int modified, is_new; long long direct; } FsStat;                /* S10: FS_MODIFIED / only in the source; calls run without comparison */    /* S6: recordings the replay cannot answer soundly (see fs_unreplayable) */
typedef struct FsMod {
    NroModule *mod;
    HMODULE dll;
    char loaded_path[MAX_PATH * 2];
    FsTable t;
    ImportFn *fn;                 /* per slot: lifted function to call directly, or NULL */
    unsigned char *pure;          /* per slot: lib::L2CValue value function (not an event) */
    unsigned char *local;         /* per slot: a function defined in this module */
    int *slot_of_sym;             /* module dynamic symbol index -> slot, -1 */
    FsOverride *ovr;              /* sorted by offset */
    FsStat *stat;
    int ovr_count, synth_done;
} FsMod;

#define FS_MAX_EVENTS 8192
typedef struct {
    const char *name; const char *spec;
    uint64_t x[8]; uint32_t f[8]; uint8_t lv[8][16];
    uint64_t r_x0; V128 r_v0; uint8_t sret[16]; uint64_t x8; int done;
    uint64_t target;              /* a call through a function pointer: the address called */
    /* S6: a game function the SDK has no description of (only lifted code of the module calls it): 1 = nothing known
     * about its result; 2 = it changed the 16 bytes at x8 (kept in sret: an L2CValue or small struct result) */
    int unknown; uint8_t x8_before[16];
} FsEvent;

typedef struct {
    FsMod *mod; const char *script; Ctx *ctx;      /* the source script running on this fiber */
    int rec, replay, partial, depth, count, pos, bad, synth, cut, inlifted;
    unsigned synth_n;
    FsMod *rec_mod; int rec_at; uint64_t rec_sp; uintptr_t rec_host; uint64_t entry[32];
    uint64_t res_at; uint8_t res_before[16];       /* S3: result buffer of the function under test */
    uint64_t lr;                                   /* S3: return address the running source function was called with */
    FsEvent *ev;
    /* S6: table elements. look = every element the ORIGINAL looked up (lib::L2CValue::operator[]) and what it held at
     * that moment; touch = every element the replay looked up and what it held when the replay began. */
    struct FsLook { uint64_t addr; uint8_t v[16]; int at, used; } *look;
    struct FsTouch { uint64_t addr; uint8_t post[16], seen[16]; } *touch;      /* seen = what the replay was last given there */
    int look_count, look_pos, look_over, ix_pending, touch_count, stale;
    char why[200];
    jmp_buf jb, synth_jb;
} FsFiber;
#define FS_MAX_LOOKS 4096
#define FS_SYNTH_CAP 400                           /* synthetic test: calls after which a run of the original is cut */
static int g_fs_offline;                           /* SSBU_FIGHTER_OFFLINE: modules are loaded by fighter_offline, the game never starts */

static DWORD g_fs_fls = FLS_OUT_OF_INDEXES;
void *fighter_host_fls_slot(void) { return (void *)&g_fs_fls; }      /* (R3 pub1) r1_state.c: this index's address, without a linker map */
static volatile LONG g_fs_recording;               /* recordings in flight (fast test in nro_import_host) */
static FsMod *g_fs_mods[128];
static int g_fs_mod_count;
static long long g_fs_dispatched, g_fs_interrupted;
static FsMod *g_fs_export_mod;
static int fighter_export_override(Ctx *c, uint64_t addr, ImportFn lifted);

static FsFiber *fs_fiber(void) {
    FsFiber *ff = FlsGetValue(g_fs_fls);
    if (!ff) { ff = calloc(1, sizeof *ff); FlsSetValue(g_fs_fls, ff); }
    return ff;
}

static int fs_is_pure(const char *n) {
    if (strstr(n, "push_variadic")) return 0;          /* hands the script's result to the caller: an event */
    return strncmp(n, "_ZN3lib8L2CValue", 16) == 0 || strncmp(n, "_ZNK3lib8L2CValue", 17) == 0;
}

/* S6: the value functions that touch memory shared with the game: 1 operator[] (returns the address of a table
 * element), 2 operator=, 3 destructor, 4 copy constructor, 5 constructor from a table / inner function pointer.
 * 2-5 count references; in a replay they must not (the original's run already did all the counting). */
static int fs_pure_kind(const char *n) {
    if (strncmp(n, "_ZNK3lib8L2CValueix", 19) == 0) return 1;
    if (strncmp(n, "_ZN3lib8L2CValue", 16) != 0) return 0;
    n += 16;
    if (strcmp(n, "aSERKS0_") == 0) return 2;
    if (strcmp(n, "D1Ev") == 0 || strcmp(n, "D2Ev") == 0) return 3;
    if (strcmp(n, "C1ERKS0_") == 0 || strcmp(n, "C2ERKS0_") == 0) return 4;
    if (strcmp(n + 1, "1EPNS_8L2CTableE") == 0 || strcmp(n + 1, "2EPNS_8L2CTableE") == 0) return 5;
    if (strcmp(n + 1, "1EPNS_20L2CInnerFunctionBaseE") == 0 || strcmp(n + 1, "2EPNS_20L2CInnerFunctionBaseE") == 0) return 6;
    return 0;
}
static int fs_writable16(uint64_t p);
static int fs_readable16(uint64_t p) {                 /* S10: 16 bytes that can be read (a vector argument) */
    MEMORY_BASIC_INFORMATION mi;
    if (p < 0x10000 || (p & 3) || ((p + 15) >> 12) != (p >> 12) || !VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) return 0;
    return mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
}
static long long g_fs_n_look, g_fs_n_match, g_fs_n_putback, g_fs_n_touch, g_fs_n_undone, g_fs_n_inlifted;      /* report */
static int fs_lv_simple(const uint8_t *v) { uint32_t t; memcpy(&t, v, 4); return t <= 4 || t == 7; }
/* recording: the original looked the element at `addr` up; keep what it holds now */
static void fs_look_add(FsFiber *ff, uint64_t addr) {
    if (!ff->look) ff->look = malloc(sizeof *ff->look * FS_MAX_LOOKS);
    if (ff->look_count >= FS_MAX_LOOKS) { ff->look_over = 1; return; }
    if (!fs_writable16(addr)) return;
    struct FsLook *l = &ff->look[ff->look_count++];
    l->addr = addr; l->at = ff->count; l->used = 0; memcpy(l->v, (const void *)(uintptr_t)addr, 16);
    g_fs_n_look++;
}
/* replay: the source (or lifted code of the module run inside the replay) looked the element at `addr` up, for real.
 * The element is given the value it had when the original looked it up at the same point (between the same two
 * game calls), so the source reads what the original read, not what the original left behind; whatever the replay
 * then writes there is compared with what the original left and taken back at the end (fs_touch_end). */
static void fs_replay_look(FsFiber *ff, uint64_t addr) {
    if (!fs_writable16(addr)) return;
    if (!ff->touch) ff->touch = malloc(sizeof *ff->touch * FS_MAX_LOOKS);
    int k = 0;
    while (k < ff->touch_count && ff->touch[k].addr != addr) k++;
    if (k == ff->touch_count) {
        if (k >= FS_MAX_LOOKS) { ff->stale++; return; }
        ff->touch[k].addr = addr; memcpy(ff->touch[k].post, (const void *)(uintptr_t)addr, 16); ff->touch_count++;
        memcpy(ff->touch[k].seen, ff->touch[k].post, 16);
    }
    /* the first lookup of this element the original made between the same two game calls and the replay has not used
     * yet (the source need not look elements up in the original's order) */
    while (ff->look_pos < ff->look_count && ff->look[ff->look_pos].at < ff->pos) ff->look_pos++;
    for (int i = ff->look_pos; i < ff->look_count && ff->look[i].at == ff->pos; i++) {
        if (ff->look[i].used || ff->look[i].addr != addr) continue;
        ff->look[i].used = 1; g_fs_n_match++;
        if (memcmp(ff->look[i].v, (const void *)(uintptr_t)addr, 16) == 0) { memcpy(ff->touch[k].seen, ff->look[i].v, 16); return; }
        /* a table / string the original has replaced since may be freed: it is not put back, and the comparison
         * of this call is not counted as a difference if it fails (stale) */
        if (!fs_lv_simple(ff->look[i].v)) { ff->stale++; return; }
        memcpy((void *)(uintptr_t)addr, ff->look[i].v, 16);
        memcpy(ff->touch[k].seen, ff->look[i].v, 16);
        g_fs_n_putback++;
        return;
    }
}
/* end of a replay: what the source left in every element it looked up against what the original left; then the
 * original's values are put back, so the game goes on from exactly the state the original's run produced. */
static void fs_touch_end(FsFiber *ff, int compare) {
    for (int k = 0; k < ff->touch_count; k++) {
        struct FsTouch *t = &ff->touch[k];
        if (!fs_writable16(t->addr)) continue;
        if (compare && !ff->bad) {
            uint32_t ta, tb; uint64_t ra, rb;
            memcpy(&ta, t->post, 4); memcpy(&ra, t->post + 8, 8);
            memcpy(&tb, (const void *)(uintptr_t)t->addr, 4); memcpy(&rb, (const void *)(uintptr_t)(t->addr + 8), 8);
            uint64_t m = ta == 0 ? 0 : ta == 1 ? 1 : ta == 3 ? 0xffffffffull : (ta == 2 || ta == 4 || ta == 7) ? ~0ull : 0;
            /* the source left the element as it was given it, and the original's state has another value: either the
             * original stored and the source did not (a difference), or a game call the original made changed it
             * (no difference). The test cannot tell: no verdict for this call. */
            if ((ta != tb || ((ra ^ rb) & m)) && memcmp(t->seen, (const void *)(uintptr_t)t->addr, 16) == 0) ff->stale++;
            if (ta != tb || ((ra ^ rb) & m))
                { ff->bad = 1; snprintf(ff->why, sizeof ff->why, "leaves type %u %llx in a table element (looked up as number %d), original type %u %llx", tb, (unsigned long long)rb, k, ta, (unsigned long long)ra); }
        }
        g_fs_n_touch++;
        if (memcmp((const void *)(uintptr_t)t->addr, t->post, 16) != 0) g_fs_n_undone++;
        memcpy((void *)(uintptr_t)t->addr, t->post, 16);
    }
    ff->touch_count = 0;
}
/* S6: a recording the replay cannot answer soundly: the original allocated or freed memory (operator new/delete, a
 * lib::L2CTable). The answer would be a pointer the original may have freed again before it returned, and value
 * functions run for real in the replay would then count references in freed memory (a double free). Such a call
 * is not replayed; it is counted apart, by function. */
static const char *fs_unreplayable(const FsFiber *ff) {
    for (int i = 0; i < ff->count; i++) {
        const char *n = ff->ev[i].name;
        if (strncmp(n, "_Znwm", 5) == 0 || strncmp(n, "_Znam", 5) == 0 || strncmp(n, "_ZdlPv", 6) == 0 || strncmp(n, "_ZdaPv", 6) == 0 || strstr(n, "L2CTable")) return n;
    }
    return ff->look_over ? "more table lookups than the test keeps" : NULL;
}

static void fs_capture(FsEvent *e, const Ctx *c, const char *name, const char *spec) {
    e->name = name; e->spec = spec; e->x8 = c->x[8]; e->done = 0; e->unknown = 0;
    for (int i = 0; i < 8; i++) { e->x[i] = c->x[i]; e->f[i] = c->v[i].w[0]; }
    int xi = 0;
    for (const char *p = spec ? spec : ""; *p; p++) {
        if (*p == 'S' || *p == 'f') continue;
        if (*p == 'V' && xi < 8 && c->x[xi]) memcpy(e->lv[xi], (const void *)(uintptr_t)c->x[xi], 16);
        /* S10: '2' '3' '4' = pointer to a phx::Vector2f / 3f / 4f: the CONTENT is the argument (the vector is built
         * on the caller's stack, so the pointer itself always differs between the original and the source) */
        if (*p >= '2' && *p <= '4' && xi < 8) { memset(e->lv[xi], 0, 16); if (fs_readable16(c->x[xi])) memcpy(e->lv[xi], (const void *)(uintptr_t)c->x[xi], 4 * (size_t)(*p - '0')); else e->lv[xi][15] = 0xee; }
        xi++;
    }
}

/* same arguments? compares what the description says each register carries */
static int fs_same(const FsEvent *a, const FsEvent *b, char *why, size_t n) {
    if (strcmp(a->name, b->name) != 0) { snprintf(why, n, "calls %.60s where the original calls %.60s", b->name, a->name); return 0; }
    int xi = 0, fi = 0, k = 0;
    for (const char *p = a->spec ? a->spec : ""; *p; p++, k++) {
        uint64_t u = a->x[xi & 7], v = b->x[xi & 7], mask = ~0ull;
        if (*p == 'S') { k--; continue; }
        if (*p == 'f') { if (a->f[fi] != b->f[fi]) { snprintf(why, n, "%.60s: float argument %d is %g, original %g", a->name, k, *(const float *)&b->f[fi], *(const float *)&a->f[fi]); return 0; } fi++; continue; }
        if (*p == 'V') {
            uint32_t ta, tb; uint64_t ra, rb;
            memcpy(&ta, a->lv[xi], 4); memcpy(&tb, b->lv[xi], 4); memcpy(&ra, a->lv[xi] + 8, 8); memcpy(&rb, b->lv[xi] + 8, 8);
            uint64_t m = ta == 0 ? 0 : ta == 1 ? 1 : ta == 3 ? 0xffffffffull : ~0ull;
            if (ta != tb || ((ra ^ rb) & m)) { snprintf(why, n, "%.60s: value %d is type %u %llx, original type %u %llx", a->name, k, tb, (unsigned long long)rb, ta, (unsigned long long)ra); return 0; }
            xi++; continue;
        }
        if (*p >= '2' && *p <= '4') {                  /* S10: a vector, by content */
            if (memcmp(a->lv[xi], b->lv[xi], 16) != 0) {
                float fa[4], fb[4]; memcpy(fa, a->lv[xi], 16); memcpy(fb, b->lv[xi], 16);
                snprintf(why, n, "%.60s: vector argument %d is (%g %g %g %g), original (%g %g %g %g)", a->name, k, fb[0], fb[1], fb[2], fb[3], fa[0], fa[1], fa[2], fa[3]); return 0;
            }
            xi++; continue;
        }
        if (*p == 'i') mask = 0xffffffffull; else if (*p == 'b') mask = 1; else if (*p == 'H') mask = 0xffffffffffull;
        if ((u ^ v) & mask) { snprintf(why, n, "%.60s: argument %d is %llx, original %llx", a->name, k, (unsigned long long)(v & mask), (unsigned long long)(u & mask)); return 0; }
        xi++;
    }
    return 1;
}

static void fs_trace_event(const char *script, const char *name, const char *spec, const Ctx *c) {
    char line[600]; int o = snprintf(line, sizeof line, "[fs] %s: %.70s(", script ? script : "?", name);
    int xi = 0, fi = 0;
    for (const char *p = spec ? spec : ""; *p && o < 520; p++) {
        if (*p == 'S') continue;
        if (*p == 'f') { o += snprintf(line + o, sizeof line - o, "%g ", (double)c->v[fi++].f[0]); continue; }
        if (*p == 'V' && c->x[xi]) {
            uint32_t t = rd32(c->x[xi]); uint64_t r = rd64(c->x[xi] + 8); float fl; memcpy(&fl, &r, 4);
            if (t == 3) o += snprintf(line + o, sizeof line - o, "num %g ", (double)fl);
            else o += snprintf(line + o, sizeof line - o, "%s %lld ", t == 0 ? "nil" : t == 1 ? "bool" : t == 2 ? "int" : t == 7 ? "hash" : "other", (long long)r);
        } else if (*p != 'p' && *p != 'T') o += snprintf(line + o, sizeof line - o, "%lld ", (long long)c->x[xi]);
        xi++;
    }
    printf("%s)\n", line);
}

/* replay: the next recorded call must be this one; it is answered from the recording */
static void fs_replay_event(FsFiber *ff, Ctx *c, const char *name, const char *spec) {
    FsEvent now;
    fs_capture(&now, c, name, spec);
    if (ff->pos >= ff->count) {
        if (ff->partial) longjmp(ff->jb, 1);                /* the original was ended here: so is the comparison */
        if (!ff->bad) snprintf(ff->why, sizeof ff->why, "calls %.80s after the original had finished (call %d)", name, ff->pos);
        ff->bad = 1; longjmp(ff->jb, 1);                    /* S2: a difference ends the replay (what follows has no answers) */
    }
    FsEvent *e = &ff->ev[ff->pos++];
    if (!ff->bad && !fs_same(e, &now, ff->why, sizeof ff->why)) ff->bad = 1;
    if (ff->bad || !e->done) longjmp(ff->jb, 1);            /* a difference, or the call the original never came back from */
    c->x[0] = e->r_x0; c->v[0] = e->r_v0;
    if (spec[0] == 'S' && c->x[8]) memcpy((void *)(uintptr_t)c->x[8], e->sret, 16);
    if (e->unknown) {                                   /* S6: answered as well as the test can; a difference after this is no verdict */
        ff->stale++;
        if (e->unknown == 2 && fs_writable16(c->x[8])) memcpy((void *)(uintptr_t)c->x[8], e->sret, 16);
    }
}
static volatile LONG g_fs_replaying;
static void nro_import_host_inner(Ctx *c, void *module, int sym);
/* nro_import_host during a replay: lifted code of the module under test (run by fs_call, see there) calls an import */
static int fighter_replay_import(Ctx *c, NroModule *m, int sym) {
    FsFiber *ff = FlsGetValue(g_fs_fls);
    if (!ff || !ff->replay || !ff->inlifted || ff->rec_mod->mod != m) return 0;
    const char *name = m->syms[sym].name;
    if (fs_is_pure(name)) {
        /* S6: value functions of lifted code inside a replay: no reference is counted (the original's run did), and
         * a table lookup is treated like the source's (fs_replay_look) */
        int kind = fs_pure_kind(name);
        if (kind == 2 || kind == 4) { if (c->x[0] != c->x[1]) memcpy((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], 16); return 1; }
        if (kind == 3) return 1;
        if (kind == 5 || kind == 6) { wr64(c->x[0], kind == 5 ? 5 : 6); wr64(c->x[0] + 8, c->x[1]); return 1; }
        if (kind == 1) { nro_import_host_inner(c, m, sym); fs_replay_look(ff, c->x[0]); return 1; }
        return 0;
    }
    int slot = ff->rec_mod->slot_of_sym[sym];
    fs_replay_event(ff, c, name, slot >= 0 ? ff->rec_mod->t.syms[slot].spec : "");
    return 1;
}

/* a source script calls the game function behind `slot` */
static void fs_call(Ctx *c, int slot) {
    FsFiber *ff = fs_fiber();
    FsMod *fm = ff->mod;
    const FsSym *s = &fm->t.syms[slot];
    static int trace = -1;
    if (trace < 0) trace = getenv("SSBU_FIGHTER_TRACE") != NULL;
    if (trace && !ff->replay && ff->script && strstr(ff->script, getenv("SSBU_FIGHTER_TRACE"))) fs_trace_event(ff->script, s->name, s->spec, c);
    if (ff->replay && !fm->pure[slot]) {
        /* S2: a function of this same module that the original calls DIRECTLY (not through its import table) left no
         * event of its own in the recording, only the calls it made: here its lifted code runs, and those calls are
         * answered from the recording (fighter_replay_import). */
        if (fm->local[slot] && fm->fn[slot] && (ff->pos >= ff->count || strcmp(ff->ev[ff->pos].name, s->name) != 0)) {
            g_fs_n_inlifted++;
            ff->inlifted++; fm->fn[slot](c); ff->inlifted--;
            return;
        }
        fs_replay_event(ff, c, s->name, s->spec);
        return;
    }
    if (ff->replay) {                                   /* S6: a value function in a replay (see fs_pure_kind) */
        int kind = fs_pure_kind(s->name);
        if (kind == 2) { if (c->x[0] != c->x[1]) memcpy((void *)(uintptr_t)c->x[0], (const void *)(uintptr_t)c->x[1], 16); return; }
        if (kind == 1) {
            if (fm->fn[slot]) fm->fn[slot](c); else call_indirect(c, fm->t.addr[slot]);
            fs_replay_look(ff, c->x[0]);
            return;
        }
    }
    if (g_fs_export_mod && !g_fs_hookmode && !ff->replay && fighter_export_override(c, fm->t.addr[slot], fm->fn[slot])) return;   /* source calls source */
    /* S3: a source function has no frame on the game's stack. What it calls must see the return address the source
     * function itself was called with (as after a tail call), not whatever the last lifted call left in x30:
     * the exception unwinder (nnunwind.c) walks the game's frames by these addresses. */
    if (ff->lr) c->x[30] = ff->lr;
    if (fm->fn[slot]) fm->fn[slot](c); else call_indirect(c, fm->t.addr[slot]);
}

static void fs_log(const char *s) { printf("%s\n", s); }
static Ctx *fs_get_ctx(void) { return fs_fiber()->ctx; }

/* S2: a script function called through a pointer taken from a table (global_table's "uniq" functions).
 * It is one event of the differential test: (agent, target address) -> the L2CValue it returns. */
static const char FS_PTR_NAME[] = "(function pointer)";
static void fs_call_addr(Ctx *c, uint64_t addr) {
    FsFiber *ff = fs_fiber();
    if (!ff->replay) { call_indirect(c, addr); return; }
    FsEvent now;
    fs_capture(&now, c, FS_PTR_NAME, "ST");
    if (ff->pos >= ff->count) {
        if (ff->partial) longjmp(ff->jb, 1);
        if (!ff->bad) snprintf(ff->why, sizeof ff->why, "calls a function pointer after the original had finished (call %d)", ff->pos);
        ff->bad = 1; longjmp(ff->jb, 1);
    }
    FsEvent *e = &ff->ev[ff->pos++];
    if (!ff->bad && (!fs_same(e, &now, ff->why, sizeof ff->why) || e->target != addr)) {
        if (!ff->why[0]) snprintf(ff->why, sizeof ff->why, "calls the function at %llx, original %llx", (unsigned long long)addr, (unsigned long long)e->target);
        ff->bad = 1;
    }
    if (ff->bad || !e->done) longjmp(ff->jb, 1);
    c->x[0] = e->r_x0; c->v[0] = e->r_v0;
    if (c->x[8]) memcpy((void *)(uintptr_t)c->x[8], e->sret, 16);
}
static const FsHost g_fs_host = {fs_call, fs_log, fs_get_ctx, fs_call_addr};

static int fighter_synth_answer(Ctx *c, FsEvent *e);
static FsMod *g_fs_export_mod;                     /* lua2cpp_common with source: its functions are also reached by name (imports) */

/* S2: module `m` calls its import `sym` (or source calls address `addr`): if that is a function of lua2cpp_common that
 * exists as source, run the source. Returns 1 if handled. */
static int fighter_export_override(Ctx *c, uint64_t addr, ImportFn lifted) {
    FsMod *xm = g_fs_export_mod;
    if (!xm || !xm->dll || addr - xm->mod->info->base >= xm->mod->info->image_size) return 0;
    return fighter_dispatch(c, xm->mod, addr, lifted);
}

/* recording side of the same: the lifted function being recorded calls a module address through a pointer (nro_call_indirect) */
static int fighter_rec_pointer(Ctx *c, uint64_t addr, ImportFn fn) {
    FsFiber *ff = FlsGetValue(g_fs_fls);
    if (ff && ff->replay && ff->inlifted) { fs_call_addr(c, addr); return 1; }      /* lifted code run inside a replay */
    if (!ff || !ff->rec || ff->depth || c->x[31] >= ff->rec_sp) return 0;
    if (ff->synth && ff->count >= FS_SYNTH_CAP) longjmp(ff->synth_jb, 1);
    if (ff->count >= FS_MAX_EVENTS) return 0;
    FsEvent *e = &ff->ev[ff->count++];
    fs_capture(e, c, FS_PTR_NAME, "ST");
    e->target = addr;
    if (ff->synth) { ff->depth++; fighter_synth_answer(c, e); return 1; }
    ff->depth++;
    fn(c);
    ff = FlsGetValue(g_fs_fls);
    ff->depth--;
    e->r_x0 = c->x[0]; e->r_v0 = c->v[0]; e->done = 1;
    if (e->x8) memcpy(e->sret, (const void *)(uintptr_t)e->x8, 16);
    return 1;
}

/* recording side: the lifted module calls import `sym` */
static FsEvent *fighter_rec_begin(Ctx *c, NroModule *m, int sym) {
    FsFiber *ff = FlsGetValue(g_fs_fls);
    if (!ff || !ff->rec) return NULL;
    if (ff->depth++ || ff->rec_mod->mod != m) return NULL;
    const char *name = m->syms[sym].name;
    if (fs_is_pure(name)) { if (fs_pure_kind(name) == 1) ff->ix_pending = 1; return NULL; }      /* S6: see fighter_rec_end */
    if (ff->synth && ff->count >= FS_SYNTH_CAP) longjmp(ff->synth_jb, 1);     /* a script that loops on made-up answers: compared up to here */
    if (ff->count >= FS_MAX_EVENTS) { ff->bad = 1; snprintf(ff->why, sizeof ff->why, "more than %d calls", FS_MAX_EVENTS); return NULL; }
    int slot = ff->rec_mod->slot_of_sym[sym];
    FsEvent *e = &ff->ev[ff->count++];
    fs_capture(e, c, name, slot >= 0 ? ff->rec_mod->t.syms[slot].spec : "");
    if (slot < 0 && !ff->synth) { e->unknown = 1; if (fs_writable16(e->x8)) { e->unknown = 3; memcpy(e->x8_before, (const void *)(uintptr_t)e->x8, 16); } }
    return e;
}
/* SSBU_FIGHTER_SYNTH: the recorded call is not made; it is answered with a made-up value (see fighter_synth) */
static int fighter_synth_answer(Ctx *c, FsEvent *e) {
    FsFiber *ff = FlsGetValue(g_fs_fls);
    if (!ff || !ff->synth) return 0;
    ff->depth--;
    uint64_t b = ff->synth == 1 ? 0 : ff->synth == 2 ? 1 : (ff->synth_n++ * 2654435761u >> 16) & 1;
    c->x[0] = b; c->v[0].d[0] = 0; c->v[0].d[1] = 0;
    {   /* S2: a float result is negative / positive with the bit, so that both sides of float comparisons run.
         * S10: its size differs from call to call (2.5, 2.625 ... by the number of the call in this run) and is never
         * what x0 holds: a source that uses the float of an EARLIER call, or converts the float where the original
         * reads x0, now hands a different value to the next game call. SSBU_FIGHTER_SYNTH_FLOATS=old: -1 / +1 as before. */
        static int old = -1;
        if (old < 0) old = getenv("SSBU_FIGHTER_SYNTH_FLOATS") != NULL;
        float f = old ? 1.0f : 2.5f + 0.125f * (float)(ff->count % 16);
        if (!b) f = -f;
        memcpy(&c->v[0], &f, 4);
    }
    if (e->spec[0] == 'S' && e->x8) { wr64(e->x8, 1); wr64(e->x8 + 8, b); }      /* an L2CValue: bool b */
    e->r_x0 = c->x[0]; e->r_v0 = c->v[0]; e->done = 1;
    if (e->spec[0] == 'S' && e->x8) memcpy(e->sret, (const void *)(uintptr_t)e->x8, 16);
    return 1;
}
static void fighter_rec_end(Ctx *c, FsEvent *e) {
    FsFiber *ff = FlsGetValue(g_fs_fls);
    if (!ff || !ff->rec) return;
    ff->depth--;
    if (ff->ix_pending) { ff->ix_pending = 0; if (!ff->depth) fs_look_add(ff, c->x[0]); }      /* S6: the element the original just looked up */
    if (!e) return;
    e->r_x0 = c->x[0]; e->r_v0 = c->v[0]; e->done = 1;
    if (e->spec[0] == 'S' && e->x8) memcpy(e->sret, (const void *)(uintptr_t)e->x8, 16);
    if (e->unknown == 3) {                              /* S6: did it write a result through x8? */
        e->unknown = 1;
        if (fs_writable16(e->x8) && memcmp(e->x8_before, (const void *)(uintptr_t)e->x8, 16) != 0) { memcpy(e->sret, (const void *)(uintptr_t)e->x8, 16); e->unknown = 2; }
    }
}

static void fighter_report(void) {
    const char *path = getenv("SSBU_FIGHTER_REPORT");
    FILE *f = fopen(path && path[0] ? path : "fighter_report.txt", "w");
    if (!f) return;
    for (int k = 0; k < g_fs_mod_count; k++) {
        FsMod *fm = g_fs_mods[k];
        long long called = 0, eq = 0, pa = 0, df = 0, calls = 0, fn_eq = 0, fn_df = 0, fn_none = 0;
        for (int i = 0; i < fm->ovr_count; i++) {
            FsStat *s = &fm->stat[i];
            if (s->calls) called++;
            calls += s->calls; eq += s->equal; pa += s->partial; df += s->differ;
            if (s->differ) fn_df++; else if (s->equal || s->partial) fn_eq++; else if (s->calls) fn_none++;
        }
        fprintf(f, "module %s: %d scripts as source; called in this run: %lld scripts, %lld calls\n", fm->mod->info->name, fm->ovr_count, called, calls);
        fprintf(f, "  differential, by call: %lld equal to the end, %lld equal up to where the game ended the script, %lld differing, %lld not compared (still running, or run lifted inside another recording)\n",
                eq, pa, df, calls - eq - pa - df);
        fprintf(f, "  differential, by script: %lld equal on every compared call, %lld differing, %lld called but never compared\n", fn_eq, fn_df, fn_none);
        long long se = 0, sd = 0;
        for (int i = 0; i < fm->ovr_count; i++) { if (fm->stat[i].synth_differ) sd++; else if (fm->stat[i].synth_equal == 3) se++; }
        fprintf(f, "  synthetic differential (SSBU_FIGHTER_SYNTH, 3 passes per script): %lld scripts equal on all passes, %lld differing\n", se, sd);
        if (k == 0) fprintf(f, "  table elements, all modules (S6): %lld looked up by the originals; in the replays %lld lookups matched one of them, %lld were given the value the original read (it had changed), "
                               "%lld elements compared at the end, %lld of them put back to the original's value; lifted functions of the module run inside a replay: %lld\n",
                            g_fs_n_look, g_fs_n_match, g_fs_n_putback, g_fs_n_touch, g_fs_n_undone, g_fs_n_inlifted);
        for (int i = 0; i < fm->ovr_count; i++) {
            FsStat *s = &fm->stat[i];
            /* S6: "at 0x<offset in the module>": 1,424 of the 1,964 names of lua2cpp_common are bound at two addresses */
            if (s->calls || s->synth_differ)fprintf(f, "  %-44s calls %6lld  equal %6lld  equal-interrupted %6lld  differ %6lld  at 0x%x%s%s%s%s\n", fm->ovr[i].name, s->calls, s->equal, s->partial, s->differ,
                                  (unsigned)fm->ovr[i].off, s->lifted ? "  (forced lifted)" : "", s->is_new ? "  (NEW: only in the source, not compared)" : s->modified ? "  (MODIFIED on purpose, not compared)" : "",
                                  s->first[0] ? "  first: " : "", s->first);
            if (s->unreplayable || s->stale) fprintf(f, "      not replayed: %lld (allocates/frees), no verdict: %lld; %s\n", s->unreplayable, s->stale, s->why_not);
        }
    }
    fclose(f);
}

static int fs_cmp_ovr(const void *a, const void *b) { return (int)((const FsOverride *)a)->off - (int)((const FsOverride *)b)->off; }

/* S3: with the hook table (hook_host.c) every function that exists as source is hooked at its own entry, so it is
 * reached however it is called: by pointer, by import, and by the DIRECT calls inside the lifted module. One record
 * per function for the life of the process (the module DLL stays loaded, the fighter DLL is a new copy every match). */
typedef struct FsHookRec { NroModule *m; uint64_t addr; ImportFn orig; int id; struct FsHookRec *next; } FsHookRec;
static FsHookRec *g_fs_hk[1 << 16];
static long long g_fs_hk_count, g_fs_hk_refused;
static FsHookRec *fs_hook_rec(uint64_t addr) {
    FsHookRec *r = g_fs_hk[(addr >> 4) & 0xffff];
    while (r && r->addr != addr) r = r->next;
    return r;
}
static void fs_hook_entry(Ctx *c, void *user) {
    FsHookRec *r = user;
    if (r->m->fighter && fighter_dispatch(c, r->m, r->addr, r->orig)) return;
    r->orig(c);
}
static void fs_hook_function(NroModule *m, uint64_t addr, const char *name) {
    if (fs_hook_rec(addr)) return;
    FsHookRec *r = calloc(1, sizeof *r);
    r->m = m; r->addr = addr;
    r->id = hook_install(addr, fs_hook_entry, r, &r->orig);
    if (!r->id) { if (g_fs_hk_refused++ < 5) printf("fighter source: %s stays lifted: %s\n", name, hook_error()); free(r); return; }
    r->next = g_fs_hk[(addr >> 4) & 0xffff]; g_fs_hk[(addr >> 4) & 0xffff] = r;
    g_fs_hk_count++;
}
/* the lifted original of a module function: behind its hook if it has one */
static ImportFn fs_lifted(NroModule *m, uint64_t addr) {
    FsHookRec *r = fs_hook_rec(addr);
    return r ? r->orig : fn_in_table(m->fns, m->fn_count, addr);
}

static void fighter_attach(NroModule *m) {
    const char *on = getenv("SSBU_FIGHTER_SRC");
    if (on && on[0] == '0') return;
    const char *name = m->info->name;
    if (strncmp(name, "lua2cpp_", 8) == 0) name += 8;
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];
    DWORD n = GetModuleFileNameA(NULL, src, MAX_PATH);
    while (n && src[n - 1] != '\\' && src[n - 1] != '/') n--;
    { const char *fb = getenv("SSBU_FIGHTER_BIN");     /* S3: another folder of fighter DLLs (work in progress), e.g. fighters_bin_s3 */
      snprintf(src + n, sizeof src - n, "%s\\%s.dll", fb && fb[0] ? fb : "fighters_bin", name); }
    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) snprintf(src + n, sizeof src - n, "fighters_bin\\%s.dll", name);
    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) return;
    if (g_fs_fls == FLS_OUT_OF_INDEXES) {
        g_fs_fls = FlsAlloc(NULL);
        /* once per run: remove the DLL copies earlier runs left. S10: ONLY those whose process no longer exists (the
         * name carries the process id: <name>.run<pid>_<n>.dll). It used to try to delete all of them and relied on
         * Windows refusing for a mapped file; a copy that a live game had not loaded YET, or had detached, was lost. */
        char pat[MAX_PATH * 2]; WIN32_FIND_DATAA fd;
        memcpy(pat, src, n); snprintf(pat + n, sizeof pat - n, "fighters_bin\\*.run*.dll");
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const char *r = strstr(fd.cFileName, ".run");
                unsigned long pid = r ? strtoul(r + 4, NULL, 10) : 0;
                if (!pid || pid == GetCurrentProcessId()) continue;
                HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                DWORD code = 0;
                int alive = ph != NULL && GetExitCodeProcess(ph, &code) && code == STILL_ACTIVE;
                if (!ph && GetLastError() == ERROR_ACCESS_DENIED) alive = 1;      /* it exists and is not ours to look at: leave its file */
                if (ph) CloseHandle(ph);
                if (alive) continue;
                char old[MAX_PATH * 2]; memcpy(old, src, n); snprintf(old + n, sizeof old - n, "fighters_bin\\%s", fd.cFileName); DeleteFileA(old);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    /* load a private copy, so that the fighter can be rebuilt while the game runs: the next match loads the new one */
    static volatile LONG serial;
    LARGE_INTEGER qf, q0, q1, q2, q3;                  /* S4: what loading a fighter DLL costs, and on which thread */
    QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&q0);
    memcpy(dst, src, n); snprintf(dst + n, sizeof dst - n, "fighters_bin\\%s.run%lu_%ld.dll", name, (unsigned long)GetCurrentProcessId(), (long)InterlockedIncrement(&serial));
    if (!CopyFileA(src, dst, FALSE)) { printf("fighter source: cannot copy %s (error %lu)\n", src, (unsigned long)GetLastError()); return; }
    QueryPerformanceCounter(&q1);
    HMODULE dll = LoadLibraryA(dst);
    QueryPerformanceCounter(&q2);
    FsBindFn bind = dll ? (FsBindFn)(void *)GetProcAddress(dll, "fighter_bind") : NULL;
    if (!bind) { printf("fighter source: %s is not a fighter DLL\n", src); if (dll) FreeLibrary(dll); DeleteFileA(dst); return; }
    FsMod *fm = calloc(1, sizeof *fm);
    fm->mod = m; fm->dll = dll; strcpy(fm->loaded_path, dst);
    bind(&g_fs_host, &fm->t);
    fm->fn = calloc((size_t)fm->t.sym_count, sizeof *fm->fn);
    fm->pure = calloc((size_t)fm->t.sym_count, 1);
    fm->local = calloc((size_t)fm->t.sym_count, 1);
    fm->slot_of_sym = malloc((size_t)m->sym_count * sizeof(int));
    for (int i = 0; i < m->sym_count; i++) fm->slot_of_sym[i] = -1;
    int missing = 0;
    for (int s = 0; s < fm->t.sym_count; s++) {
        fm->pure[s] = (unsigned char)fs_is_pure(fm->t.syms[s].name);
        int found = -1;
        for (int i = 1; i < m->sym_count; i++) if (strcmp(m->syms[i].name, fm->t.syms[s].name) == 0) { found = i; break; }
        if (found < 0 || !m->bind_addr[found]) {
            /* one SDK serves all modules: a function this module never imports is bound like an import would be (main, then the other modules) */
            int mi = main_find(fm->t.syms[s].name);
            const Export *e = mi >= 0 && dyn_syms[mi].defined ? NULL : exp_find(fm->t.syms[s].name);
            uint64_t a = mi >= 0 && dyn_syms[mi].defined ? GUEST_BASE + dyn_syms[mi].value : e ? e->value : 0;
            if (a) { fm->t.addr[s] = a; fm->fn[s] = function_at(a); } else missing++;
            continue;
        }
        fm->slot_of_sym[found] = s;
        fm->local[s] = (unsigned char)(m->bind_addr[found] - m->info->base < m->info->image_size);
        fm->t.addr[s] = m->bind_addr[found];
        fm->fn[s] = m->bind_fn[found];
        if (!fm->fn[s] && fm->local[s]) fm->fn[s] = fn_in_table(m->fns, m->fn_count, m->bind_addr[found]);   /* the module was not mapped yet when it was bound */
    }
    fm->ovr_count = fm->t.ovr_count;
    fm->ovr = malloc((size_t)fm->ovr_count * sizeof *fm->ovr);
    memcpy(fm->ovr, fm->t.ovr, (size_t)fm->ovr_count * sizeof *fm->ovr);
    qsort(fm->ovr, (size_t)fm->ovr_count, sizeof *fm->ovr, fs_cmp_ovr);
    fm->stat = calloc((size_t)fm->ovr_count + 1, sizeof *fm->stat);
    const char *lifted = getenv("SSBU_FIGHTER_LIFTED");
    int forced = 0;
    for (int i = 0; lifted && i < fm->ovr_count; i++) {
        const char *hit = strcmp(lifted, "1") == 0 ? lifted : strstr(lifted, fm->ovr[i].name);
        size_t len = strlen(fm->ovr[i].name);
        if (hit && (strcmp(lifted, "1") == 0 || ((hit == lifted || hit[-1] == ',') && (hit[len] == 0 || hit[len] == ',')))) { fm->stat[i].lifted = 1; forced++; }
    }
    /* S10 (part D): FS_MODIFIED(name) in the source exports the data symbol fs_modified_<name>; a binding whose
     * address is not a function of the module is a function that exists only in the source */
    int n_mod = 0, n_new = 0;
    for (int i = 0; i < fm->ovr_count; i++) {
        char sym[200]; snprintf(sym, sizeof sym, "fs_modified_%.180s", fm->ovr[i].name);
        if (GetProcAddress(dll, sym)) { fm->stat[i].modified = 1; n_mod++; }
        if (!fn_in_table(m->fns, m->fn_count, m->info->base + fm->ovr[i].off)) { fm->stat[i].is_new = 1; n_new++; }
    }
    if (n_mod || n_new) printf("fighter source: %s: %d functions marked modified (not compared), %d functions that exist only in the source\n", name, n_mod, n_new);
    { const char *hm = getenv("SSBU_FIGHTER_HOOK"); if (hm) g_fs_hookmode = hm[0] != '0'; }
    if (g_fs_hookmode) for (int i = 0; i < fm->ovr_count; i++) fs_hook_function(m, m->info->base + fm->ovr[i].off, fm->ovr[i].name);
    m->fighter = fm;
    if (strcmp(name, "common") == 0) g_fs_export_mod = fm;
    if (g_fs_mod_count < 128) g_fs_mods[g_fs_mod_count++] = fm;
    printf("fighter source: %s: %d scripts run as C++ source (%d forced lifted), %d game symbols bound (%d missing)%s%s\n", name, fm->ovr_count - forced, forced,
           fm->t.sym_count - missing, missing, getenv("SSBU_FIGHTER_DIFF") ? ", differential test ON" : "", g_fs_hookmode ? ", installed as entry hooks" : ", reached by pointer/import only");
    trace_printf("fighter source: %s loaded from %s\n", name, dst);
    QueryPerformanceCounter(&q3);
    { static double total; double k = 1000.0 / (double)qf.QuadPart;
      total += (double)(q3.QuadPart - q0.QuadPart) * k;
      printf("fighter source: %s: load cost %.1f ms on thread %lu (copy %.1f, LoadLibrary %.1f, bind %.1f); all modules so far %.1f ms\n", name, (double)(q3.QuadPart - q0.QuadPart) * k,
             (unsigned long)GetCurrentThreadId(), (double)(q1.QuadPart - q0.QuadPart) * k, (double)(q2.QuadPart - q1.QuadPart) * k, (double)(q3.QuadPart - q2.QuadPart) * k, total); }
}

static void fighter_detach(NroModule *m) {
    FsMod *fm = m->fighter;
    if (!fm) return;
    fighter_report();
    if (g_fs_export_mod == fm) g_fs_export_mod = NULL;
    /* S2 fix (crash 0xc0000005 after "UnloadModule: lua2cpp_mario", found by track V): the DLL copy must STAY loaded.
     * The report keeps reading the script names, which are strings inside the DLL, and a script fiber can still be
     * suspended inside one of its functions. It was freed here; now it is only marked as detached (dll = NULL) and
     * the file is removed by the next run (fs_remove_old_copies). */
    m->fighter = NULL;              /* the FsMod stays (its statistics are in the report) */
    fm->dll = NULL;
}

/* S3, offline test only: the made-up agent's BattleObject (agent+0x38) is 64 KB in which every word is a pointer
 * into the same 64 KB, so that a function that follows pointers from it (object -> member -> field) reads memory
 * instead of address 0x60 and can be compared at all. It is put back before the original and before the source run. */
static uint8_t *g_fs_soup, *g_fs_soup0;
static int g_fs_guard_phase;                       /* defined with the fault guard below */
static void fs_soup_reset(void) {
    if (!g_fs_offline) return;
    if (!g_fs_soup) {
        g_fs_soup = VirtualAlloc(NULL, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        g_fs_soup0 = g_fs_soup + 0x10000;
        for (unsigned i = 0; i < 0x2000; i++) { uint64_t p = (uint64_t)(uintptr_t)g_fs_soup + ((i * 296u + 0x100u) & 0xfff8u); memcpy(g_fs_soup0 + 8 * i, &p, 8); }
    }
    memcpy(g_fs_soup, g_fs_soup0, 0x10000);
}

/* S3: the RESULT of a function of the script classes (the L2CValue written through x8) is part of the comparison:
 * sessions 1-2 compared only the calls a function makes, which says nothing about what it hands back to a lifted
 * caller. Before the original runs the 16 bytes at x8 are kept (fs_result_begin); the source starts from the same
 * bytes and must leave the same value. Only for lua2cpp_common (ACMD scripts have no result buffer). */
static int fs_writable16(uint64_t p) {
    MEMORY_BASIC_INFORMATION mi;
    if (p < 0x10000 || (p & 7) || !VirtualQuery((void *)(uintptr_t)p, &mi, sizeof mi)) return 0;
    return mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mi.Protect & PAGE_GUARD) && ((p + 15) >> 12) == (p >> 12);
}
static void fs_result_begin(FsFiber *ff, FsMod *fm, const Ctx *c) {
    ff->res_at = (fm == g_fs_export_mod && fs_writable16(c->x[8])) ? c->x[8] : 0;
    if (ff->res_at) memcpy(ff->res_before, (const void *)(uintptr_t)ff->res_at, 16);
}

/* run the source of the recorded script against the recording; partial = the original was ended before it returned */
static void __declspec(noinline) fs_compare(Ctx *c, FsFiber *ff, int partial) {
    FsMod *fm = ff->rec_mod;
    FsStat *st = &fm->stat[ff->rec_at];
    ff->rec = 0;
    InterlockedDecrement(&g_fs_recording);
    if (!fm->dll) return;                              /* its module was unloaded meanwhile */
    if (!ff->synth) {                                  /* S6 */
        const char *no = fs_unreplayable(ff);
        if (no) { if (!st->unreplayable++) snprintf(st->why_not, sizeof st->why_not, "%.100s", no); ff->res_at = 0; return; }
    }
    ff->look_pos = 0; ff->touch_count = 0; ff->stale = 0;
    uint8_t res_lifted[16];
    fs_soup_reset();
    if (partial) ff->res_at = 0;                      /* ended from outside: no result, and its buffer may be gone */
    if (ff->res_at) { memcpy(res_lifted, (const void *)(uintptr_t)ff->res_at, 16); memcpy((void *)(uintptr_t)ff->res_at, ff->res_before, 16); }
    Ctx after = *c;
    FsMod *saved_mod = ff->mod; const char *saved_script = ff->script; Ctx *saved_ctx = ff->ctx;
    memcpy(c->x, ff->entry, sizeof ff->entry);
    ff->mod = fm; ff->script = fm->ovr[ff->rec_at].name; ff->ctx = c;
    ff->pos = 0; ff->partial = partial; ff->replay = 1; ff->inlifted = 0;
    InterlockedIncrement(&g_fs_replaying);
    if (setjmp(ff->jb) == 0) { g_fs_guard_phase = g_fs_offline ? 2 : 0; fm->ovr[ff->rec_at].fn(c); }      /* nothing reaches the game: every call is answered from the recording */
    g_fs_guard_phase = 0;
    InterlockedDecrement(&g_fs_replaying);
    ff->replay = 0; ff->inlifted = 0;
    if (!ff->bad && ff->pos != ff->count)
        { ff->bad = 1; snprintf(ff->why, sizeof ff->why, "stops after %d of the original's %d calls (next: %.80s)", ff->pos, ff->count, ff->ev[ff->pos].name); }
    if (ff->res_at) {
        uint32_t ta, tb; uint64_t ra, rb;
        memcpy(&ta, res_lifted, 4); memcpy(&ra, res_lifted + 8, 8);
        memcpy(&tb, (const void *)(uintptr_t)ff->res_at, 4); memcpy(&rb, (const void *)(uintptr_t)(ff->res_at + 8), 8);
        uint64_t m = ta == 0 ? 0 : ta == 1 ? 1 : ta == 3 ? 0xffffffffull : (ta == 2 || ta == 4 || ta == 7) ? ~0ull : 0;      /* tables, strings: the type only */
        if (!ff->bad && !partial && (ta != tb || ((ra ^ rb) & m)))
            { ff->bad = 1; snprintf(ff->why, sizeof ff->why, "returns type %u %llx, original type %u %llx", tb, (unsigned long long)rb, ta, (unsigned long long)ra); }
        memcpy((void *)(uintptr_t)ff->res_at, res_lifted, 16);      /* the original's result stands */
        ff->res_at = 0;
    }
    fs_touch_end(ff, !partial);                        /* S6: table elements: compared, then the original's values stand */
    if (ff->bad && ff->stale && !ff->synth) {          /* S6: the replay could not be given what the original read: no verdict */
        if (!st->stale++) { snprintf(st->why_not, sizeof st->why_not, "replay incomplete, then: %.90s", ff->why); printf("fighter diff: %s: no verdict (the replay could not be given everything the original read), then: %s\n", fm->ovr[ff->rec_at].name, ff->why); }
        ff->mod = saved_mod; ff->script = saved_script; ff->ctx = saved_ctx;
        *c = after;
        return;
    }
    if (ff->synth) {
        if (!ff->bad) st->synth_equal++;
        else if (!st->synth_differ++) { snprintf(st->first, sizeof st->first, "synthetic %d: %s", ff->synth, ff->why); printf("fighter synthetic diff: %s differs: %s\n", fm->ovr[ff->rec_at].name, ff->why); }
    }
    else if (ff->bad) { if (!st->differ++) { snprintf(st->first, sizeof st->first, "%s", ff->why); printf("fighter diff: %s differs: %s\n", fm->ovr[ff->rec_at].name, ff->why); } }
    else if (partial) { st->partial++; g_fs_interrupted++; }
    else st->equal++;
    ff->mod = saved_mod; ff->script = saved_script; ff->ctx = saved_ctx;
    *c = after;
}

/* SSBU_FIGHTER_SYNTH=1: once per module, when the game first calls one of its source scripts, EVERY source script is
 * compared with its lifted original on made-up answers: both run from the same arguments (this call's agent), no call
 * reaches the game, every game function "returns" false (pass 1), true (pass 2), a fixed pseudo-random pattern
 * (pass 3). It covers the scripts a match does not reach and both sides of every condition; it does not prove
 * anything about what the game answers - the recorded real calls (SSBU_FIGHTER_DIFF) do that. */
/* S3, offline test: an access violation inside a function under test does not end the whole test. The handler puts
 * the thread back at the setjmp of the run in progress (registers from the jmp_buf: nothing is unwound, as the
 * abandoned frames are lifted code or source without resources): during the ORIGINAL's run the function cannot be
 * tested on the made-up agent (reported as such), during the SOURCE's run it is a difference. */
static FsFiber *g_fs_guard_ff; static int g_fs_guard_phase;      /* 1 = the original runs, 2 = the source runs */
#ifdef _WIN32      /* Linux build: no fault guard; a fault in the offline test ends the run */
static void fs_guard_resume(CONTEXT *x, jmp_buf jb, int value) {
    _JUMP_BUFFER *j = (_JUMP_BUFFER *)jb;
    x->Rbx = j->Rbx; x->Rsp = j->Rsp; x->Rbp = j->Rbp; x->Rsi = j->Rsi; x->Rdi = j->Rdi;
    x->R12 = j->R12; x->R13 = j->R13; x->R14 = j->R14; x->R15 = j->R15; x->Rip = j->Rip;
    memcpy(&x->Xmm6, &j->Xmm6, 160);
    x->Rax = (DWORD64)value;
}
static LONG CALLBACK fs_guard(EXCEPTION_POINTERS *ep) {
    FsFiber *ff = g_fs_guard_ff;
    if (!ff || !g_fs_guard_phase || ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    int phase = g_fs_guard_phase;
    g_fs_guard_phase = 0;
    if (phase == 1) fs_guard_resume(ep->ContextRecord, ff->synth_jb, 3);
    else { if (!ff->bad) snprintf(ff->why, sizeof ff->why, "the source faults (reads or writes address %llx) where the original does not", (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]); ff->bad = 1; fs_guard_resume(ep->ContextRecord, ff->jb, 1); }
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

static void fighter_synth(Ctx *c, NroModule *m, FsMod *fm, FsFiber *ff) {
    Ctx saved = *c;
#ifdef _WIN32
    if (g_fs_offline && !g_fs_guard_ff) { g_fs_guard_ff = ff; AddVectoredExceptionHandler(1, fs_guard); }
#endif
    if (!ff->ev) ff->ev = malloc(sizeof(FsEvent) * FS_MAX_EVENTS);
    for (int i = 0; i < fm->ovr_count; i++) {
        ImportFn lifted = fs_lifted(m, m->info->base + fm->ovr[i].off);
        if (!lifted) continue;
        if (fm->stat[i].modified) { printf("fighter synthetic diff: %s is marked modified: not compared\n", fm->ovr[i].name); continue; }      /* S10 */
        {   /* S3: SSBU_FIGHTER_OFFLINE_SKIP=<file>: names (one per line) the test leaves out, e.g. those that stopped an earlier run */
            static char *skip; static int skip_read;
            if (!skip_read) {
                skip_read = 1;
                FILE *sf = getenv("SSBU_FIGHTER_OFFLINE_SKIP") ? fopen(getenv("SSBU_FIGHTER_OFFLINE_SKIP"), "rb") : NULL;
                if (sf) { skip = calloc(1, 1 << 20); skip[0] = '\n'; size_t n = fread(skip + 1, 1, (1 << 20) - 3, sf); skip[n + 1] = '\n'; fclose(sf); }
            }
            char key[160]; snprintf(key, sizeof key, "\n%.150s\n", fm->ovr[i].name);
            if (skip && strstr(skip, key)) continue;
        }
        if (g_fs_offline > 1) printf("synth %s\n", fm->ovr[i].name);
        for (int pass = 1; pass <= 3; pass++) {
            *c = saved;
            memcpy(ff->entry, c->x, sizeof ff->entry);
            ff->count = 0; ff->bad = 0; ff->depth = 0; ff->why[0] = 0; ff->synth_n = (unsigned)i * 7919u;
            ff->look_count = 0; ff->look_over = 0; ff->ix_pending = 0;
            ff->rec_mod = fm; ff->rec_at = i; ff->rec_sp = c->x[31];
            ff->synth = pass; ff->rec = 1; ff->cut = 0;
            fs_soup_reset();
            if (g_fs_offline && c->x[0]) wr64(c->x[0] + 0x38, (uint64_t)(uintptr_t)g_fs_soup);
            fs_result_begin(ff, fm, c);
            InterlockedIncrement(&g_fs_recording);
            int sj = setjmp(ff->synth_jb);
            if (sj == 0) { g_fs_guard_phase = g_fs_offline ? 1 : 0; lifted(c); g_fs_guard_phase = 0; }
            else if (sj == 3) {                         /* the original itself faults on the made-up agent: not testable here */
                ff->rec = 0; ff->synth = 0; ff->depth = 0; InterlockedDecrement(&g_fs_recording);
                if (!fm->stat[i].synth_differ++) { snprintf(fm->stat[i].first, sizeof fm->stat[i].first, "the original faults on the test agent"); printf("fighter synthetic diff: %s differs: the original faults on the test agent (not testable offline)\n", fm->ovr[i].name); }
                break;
            }
            else { g_fs_guard_phase = 0; ff->cut = 1; ff->depth = 0; fm->stat[i].synth_cut++; }
            ff->synth_n = (unsigned)i * 7919u;
            fs_compare(c, ff, ff->cut);
            ff->synth = 0;
        }
    }
    *c = saved;
    long long eq = 0, df = 0;
    for (int i = 0; i < fm->ovr_count; i++) { if (fm->stat[i].synth_differ) df++; else if (fm->stat[i].synth_equal == 3) eq++; }
    printf("fighter synthetic differential: %s: %lld scripts equal on all 3 passes, %lld differing, of %d\n", m->info->name, eq, df, fm->ovr_count);
}

/* S3: SSBU_FIGHTER_LOG=1: evidence for edit proofs without looking at the screen. Whenever a function of the shared
 * state machine (so: a fighter's agent in x0) is entered, the fighter's damage percent and status kind are read with
 * the game's own getters; a change is one log line. `[fs] damage` lines give the percent each hit added,
 * `[fs] status` lines the sequence of statuses of each fighter. */
static void fs_state_log(Ctx *c, const char *at) {
    static uint64_t a_damage, a_status, boma_of[8];
    static float last_damage[8]; static int last_status[8], count; static long long line;
    if (!a_damage) {
        a_damage = hk_address_of("_ZN3app8lua_bind25DamageModule__damage_implEPNS_26BattleObjectModuleAccessorEi");
        a_status = hk_address_of("_ZN3app8lua_bind30StatusModule__status_kind_implEPNS_26BattleObjectModuleAccessorE");
        if (!a_damage || !a_status) { printf("[fs] log: getters not found\n"); a_damage = 1; }
    }
    if (a_damage == 1 || !c->x[0]) return;
    uint64_t boma = rd64(c->x[0] + 0x40);
    if (!boma) return;
    int k = 0;
    while (k < count && boma_of[k] != boma) k++;
    if (k == 8) return;
    Ctx saved = *c;
    if (k == count) {
        static uint64_t a_kind;
        if (!a_kind) a_kind = hk_address_of("_ZN3app7utility8get_kindEPKNS_26BattleObjectModuleAccessorE");
        boma_of[k] = boma; last_damage[k] = 0; last_status[k] = -1; count++;
        if (a_kind) { c->x[0] = boma; call_indirect(c, a_kind); printf("[fs] fighter %d is fighter kind %d (0 = Mario)\n", k, (int)c->x[0]); *c = saved; }
    }
    c->x[0] = boma; c->x[1] = 0; call_indirect(c, a_damage);
    float d = c->v[0].f[0];
    c->x[0] = boma; call_indirect(c, a_status);
    int s = (int)c->x[0];
    *c = saved;
    if (d != last_damage[k]) { printf("[fs] damage %lld: fighter %d: %.2f%% -> %.2f%% (%+.2f) seen at %s\n", ++line, k, last_damage[k], d, d - last_damage[k], at); last_damage[k] = d; }
    if (s != last_status[k]) { printf("[fs] status %lld: fighter %d: 0x%x -> 0x%x seen at %s\n", ++line, k, last_status[k], s, at); last_status[k] = s; }
}

/* nro_call_indirect: the game calls module address `addr`. Returns 1 if a source function handled it. */
static int fighter_dispatch(Ctx *c, NroModule *m, uint64_t addr, ImportFn lifted) {
    FsMod *fm = m->fighter;
    uint32_t off = (uint32_t)(addr - m->info->base);
    int lo = 0, hi = fm->ovr_count - 1, at = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (fm->ovr[mid].off == off) { at = mid; break; }
        if (fm->ovr[mid].off < off) lo = mid + 1; else hi = mid - 1;
    }
    if (at < 0 || !fm->dll) return 0;
    if (fm->stat[at].lifted && lifted) return 0;
    static int diff = -1;
    if (diff < 0) diff = getenv("SSBU_FIGHTER_DIFF") != NULL;
    FsFiber *ff = fs_fiber();
    FsStat *st = &fm->stat[at];
    /* S10 (part D): a function that exists only in the source (no original: `lifted` is NULL, its address is one of
     * the module's synthetic addresses, see nro_call_indirect), or one marked FS_MODIFIED in the source ("changed on
     * purpose"): the source runs, nothing is recorded or compared for it; the report says "new" / "modified". */
    if (!lifted || st->modified) {
        if (ff->replay && lifted) return 0;            /* inside a replay the original of a modified function answers from the recording */
        if (ff->rec && lifted) { st->calls++; return 0; }      /* inside another function's recording: the original, as for every nested call */
        st->calls++; st->direct++;
        FsMod *saved_mod = ff->mod; const char *saved_script = ff->script; Ctx *saved_ctx = ff->ctx; uint64_t saved_lr = ff->lr;
        ff->mod = fm; ff->script = fm->ovr[at].name; ff->ctx = c; ff->lr = 0;
        fm->ovr[at].fn(c);
        ff = fs_fiber();
        ff->mod = saved_mod; ff->script = saved_script; ff->ctx = saved_ctx; ff->lr = saved_lr;
        return 1;
    }
    if (ff->replay) return 0;
    static int statelog = -1;
    if (statelog < 0) statelog = getenv("SSBU_FIGHTER_LOG") != NULL && !g_fs_offline;
    if (statelog && fm == g_fs_export_mod && !ff->rec && strncmp(fm->ovr[at].name, "status_", 7) == 0) fs_state_log(c, fm->ovr[at].name);
    if (!fm->synth_done && !ff->rec) {
        fm->synth_done = 1;
        if (getenv("SSBU_FIGHTER_SYNTH")) fighter_synth(c, m, fm, ff);
    }
    if (ff->rec) {
        /* called from inside the function being recorded: runs lifted, uncompared. S3: with entry hooks this is also
         * reached by a TAIL call (same guest sp): then the recording's host frame is still below this one; in the
         * synthetic test nothing ever ends a function from outside. */
        if (c->x[31] < ff->rec_sp || ff->synth || (c->x[31] == ff->rec_sp && (uintptr_t)&at < ff->rec_host)) { st->calls++; return 0; }
        fs_compare(c, ff, 1);                                       /* that script was ended by the game (unwound): compare what it did */
    }
    st->calls++;
    static int every = 0;                               /* S4: SSBU_FIGHTER_REPORT_EVERY=<n> (probe.py reads the report as a progress counter) */
    if (!every) { const char *e = getenv("SSBU_FIGHTER_REPORT_EVERY"); every = e && atoi(e) > 0 ? atoi(e) : diff ? 20 : 2000; }
    if (++g_fs_dispatched % every == 0) fighter_report();
    if (!diff) {
        FsMod *saved_mod = ff->mod; const char *saved_script = ff->script; Ctx *saved_ctx = ff->ctx; uint64_t saved_lr = ff->lr;
        ff->mod = fm; ff->script = fm->ovr[at].name; ff->ctx = c; ff->lr = g_fs_hookmode ? c->x[30] : 0;
        fm->ovr[at].fn(c);
        ff = fs_fiber();
        ff->mod = saved_mod; ff->script = saved_script; ff->ctx = saved_ctx; ff->lr = saved_lr;
        return 1;
    }
    if (!ff->ev) ff->ev = malloc(sizeof(FsEvent) * FS_MAX_EVENTS);
    memcpy(ff->entry, c->x, sizeof ff->entry);
    ff->count = 0; ff->bad = 0; ff->depth = 0; ff->why[0] = 0;
    ff->look_count = 0; ff->look_over = 0; ff->ix_pending = 0;
    ff->rec_mod = fm; ff->rec_at = at; ff->rec_sp = c->x[31]; ff->rec_host = (uintptr_t)&at;
    fs_result_begin(ff, fm, c);
    ff->rec = 1;
    InterlockedIncrement(&g_fs_recording);
    lifted(c);                                      /* the real run; may take many game frames, may never come back */
    ff = fs_fiber();
    if (ff->rec && ff->rec_mod == fm && ff->rec_at == at) fs_compare(c, ff, 0);
    return 1;
}

/* SSBU_FIGHTER_OFFLINE=all | name1,name2: the synthetic differential test of every source script of the listed
 * modules WITHOUT starting the game (called once before the game's entry point, nnro.c module_coverage_start).
 * Each module is read from SSBU_FIGHTER_NRO_DIR (default ro_study\nro), loaded by the normal nn::ro path (its hash
 * is put in a one-entry NRR first), not initialised (no DT_INIT: nothing of the game exists), and every script of
 * fighters_bin\<name>.dll is compared with its lifted original as fighter_synth does. The constant table of
 * lua2cpp_common is filled with a distinct value per index, so a wrong constant index shows. Then the process ends. */
static void ro_LoadModule(Ctx *c);
static void ro_RegisterModuleInfo(Ctx *c);
static void ro_UnregisterModuleInfo(Ctx *c);
static int sha256(const void *data, size_t size, uint8_t out[32]);
static NroModule *module_of_image(uint64_t nro);
static NroModule *fs_offline_load(Ctx *c, const char *dir, const char *name) {
    static uint8_t nrr_page[0x2000];
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s/%s.nro", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("fighter offline: cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *nro = VirtualAlloc(NULL, (size_t)size + 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!nro || fread(nro, 1, (size_t)size, f) != (size_t)size) { fclose(f); printf("fighter offline: cannot read %s\n", path); return NULL; }
    fclose(f);
    uint8_t h[32], info[0x20], out[0x40];
    uint64_t nrr = ((uint64_t)(uintptr_t)nrr_page + 0xfff) & ~(uint64_t)0xfff, lr = c->x[30];
    if (!sha256(nro, rd32((uint64_t)(uintptr_t)nro + 0x18), h)) { printf("fighter offline: no SHA-256\n"); return NULL; }
    memset((void *)(uintptr_t)nrr, 0, 0x1000);
    wr32(nrr, 0x3052524eu); wr32(nrr + 0x338, 0x1000); wr32(nrr + 0x340, 0x350); wr32(nrr + 0x344, 1);
    memcpy((void *)(uintptr_t)(nrr + 0x350), h, 32);
    memset(info, 0, sizeof info); memset(out, 0, sizeof out);
    c->x[0] = (uint64_t)(uintptr_t)info; c->x[1] = nrr; ro_RegisterModuleInfo(c);
    c->x[0] = (uint64_t)(uintptr_t)out; c->x[1] = (uint64_t)(uintptr_t)nro; c->x[2] = 0; c->x[3] = 0; c->x[4] = 2;
    ro_LoadModule(c);
    uint64_t r = c->x[0];
    c->x[0] = (uint64_t)(uintptr_t)info; ro_UnregisterModuleInfo(c);
    c->x[30] = lr;
    if (r != RO_OK) { printf("fighter offline: %s not loaded (0x%llx)\n", name, (unsigned long long)r); return NULL; }
    return module_of_image((uint64_t)(uintptr_t)nro);
}

static void fighter_offline(Ctx *c) {
    const char *list = getenv("SSBU_FIGHTER_OFFLINE");
    if (!list || !list[0]) return;
    const char *dir = getenv("SSBU_FIGHTER_NRO_DIR");
    if (!dir) dir = "ro_study/nro";
    g_fs_offline = getenv("SSBU_FIGHTER_OFFLINE_TRACE") ? 2 : 1;
    ro_init_tables();
    setvbuf(stdout, NULL, _IONBF, 0);
    NroModule *common = fs_offline_load(c, dir, "lua2cpp_common");
    if (!common) ExitProcess(3);
    const Export *tab = exp_find("_ZN7lua2cpp21L2CAgentGeneratedBase19const_value_table__E");
    if (!tab) { printf("fighter offline: no constant table\n"); ExitProcess(3); }
    for (int i = 0; i < 0x4c0d; i++) wr32(tab->value + 4ull * (unsigned)i, 1000u + (unsigned)i);
    static uint8_t agent[0x400], lua[0x400], boma[0x1000], variadic[0x100];
    wr64((uint64_t)(uintptr_t)agent + 8, (uint64_t)(uintptr_t)lua);
    wr64((uint64_t)(uintptr_t)agent + 0x40, (uint64_t)(uintptr_t)boma);
    int all = strcmp(list, "all") == 0;
    for (int i = 0; i < nro_registry_count; i++) {
        const char *name = nro_registry[i].name, *sh = strncmp(name, "lua2cpp_", 8) == 0 ? name + 8 : name;
        size_t len = strlen(sh);
        const char *hit = all ? list : strstr(list, sh);
        while (!all && hit && !((hit == list || hit[-1] == ',') && (hit[len] == 0 || hit[len] == ','))) hit = strstr(hit + 1, sh);
        if (!hit) continue;
        int is_common = strcmp(sh, "common") == 0;
        NroModule *m = is_common ? common : fs_offline_load(c, dir, name);
        if (!m) continue;
        FsMod *fm = m->fighter;
        if (!fm) { if (!is_common || !all) printf("fighter offline: %s has no fighters_bin DLL\n", name); continue; }
        Ctx saved = *c;
        static uint8_t nil_args[8][0x40];              /* L2CValue arguments of functions that take some: nil */
        memset(agent, 0, sizeof agent); memset(nil_args, 0, sizeof nil_args);
        wr64((uint64_t)(uintptr_t)agent + 8, (uint64_t)(uintptr_t)lua);
        wr64((uint64_t)(uintptr_t)agent + 0x40, (uint64_t)(uintptr_t)boma);
        c->x[0] = (uint64_t)(uintptr_t)agent; c->x[1] = (uint64_t)(uintptr_t)variadic;
        for (int k = 2; k < 8; k++) c->x[k] = (uint64_t)(uintptr_t)nil_args[k];
        c->x[8] = (uint64_t)(uintptr_t)nil_args[0];
        fm->synth_done = 1;
        fighter_synth(c, m, fm, fs_fiber());
        *c = saved;
        long long cut = 0;
        for (int k = 0; k < fm->ovr_count; k++) if (fm->stat[k].synth_cut) cut++;
        printf("fighter offline: %s done (%lld scripts were cut after %d calls on some pass)\n", name, cut, FS_SYNTH_CAP);
        m->fighter = NULL; fm->dll = NULL;             /* stays loaded: see fighter_detach */
    }
    fighter_report();
    printf("fighter offline: finished\n");
    ExitProcess(0);
}

/* ---------------------------------------------------------------- S10: SSBU_FIGHTER_DEBUG=<script file>: debug injection
 * TEST ONLY. Without the variable nothing here is installed and nothing runs. The file gives commands that provoke a
 * situation instead of waiting for it to happen in a match; every command is one or two calls of the GAME'S OWN
 * functions (the lua_bind exports the fighter sources call too), made on the game's own thread at one fixed point of
 * the fighter's frame: the entry of lua2cpp::L2CFighterCommon::sys_line_system_control_fighter (the script line the
 * game runs for every fighter once per frame, before that frame's status scripts), reached through a pass-through
 * hook (hook_host.c). At that point no status script of the fighter is running and no recording of the differential
 * test is open (if one is, the command waits for the next frame), so with SSBU_FIGHTER_DIFF=1 the original and the
 * source see exactly the same forced situation.
 *
 *   at <frame> <command> ...        once, when the fighter's own frame counter reaches <frame> (it counts calls of the
 *                                   line above for that fighter since the fight began: "fight frames")
 *   every <n> <command> ...         every n-th frame
 *   # ...                           comment
 * commands (fighter = the entry id, 0 = player 1; a number can be decimal, 0x.., or the NAME of a Lua constant, which
 * is looked up in fighters\sdk_generated\lua_const.h and read from the game's constant table at run time):
 *   status <fighter> <kind> [0|1]   StatusModule::change_status_request_from_script(kind, clear command = 1)
 *   statusreq <fighter> <kind> [0|1] StatusModule::change_status_request(kind, 0)
 *   item <fighter> <item kind> [variation]  ItemModule::have_item(kind, variation, 0, false, false)
 *   noitem <fighter>                ItemModule::remove_all
 *   final <fighter>                 WorkModule::on_flag(FIGHTER_INSTANCE_WORK_ID_FLAG_FINAL): what the fighter has after
 *                                   breaking a Smash Ball (the flag the shared scripts test); then `status .. FINAL`
 *   flag <fighter> <work flag> [0|1] WorkModule::on_flag / off_flag
 *   damage <fighter> <percent>      DamageModule::add_damage(percent, 0)
 *   pos <fighter> <x> <y>           PostureModule::set_pos_2d
 *   air <fighter> [dy]              leave the ground: position y + dy (default 30), StatusModule::set_situation_kind(AIR),
 *                                   GroundModule::set_correct(GROUND_CORRECT_KIND_AIR)
 * Every executed command is one log line `[debug] frame F fighter K: ...` written BEFORE the calls are made (the
 * sweep driver tests\s10_sweep.py reads the last one after a trap). */
enum { FD_STATUS, FD_STATUSREQ, FD_ITEM, FD_NOITEM, FD_FINAL, FD_FLAG, FD_DAMAGE, FD_POS, FD_AIR, FD_TERM, FD_KINDS };
static const char *const g_fd_names[FD_KINDS] = {"status", "statusreq", "item", "noitem", "final", "flag", "damage", "pos", "air", "term"};
typedef struct { int every, cmd, fighter, done, nargs; long long at; char arg[3][72]; } FsDbgCmd;
static FsDbgCmd *g_fd_cmd;
static int g_fd_count, g_fd_n;
static ImportFn g_fd_orig;
static struct { uint64_t boma; int entry; long long frame; } g_fd_f[8];
static long long g_fd_done, g_fd_waited;

/* a number, or the name of a Lua constant (index from the generated header, value from the game's table) */
static int fd_value(const char *tok, double *out) {
    char *end; double v = strtod(tok, &end);
    if (end != tok && *end == 0) { *out = v; return 1; }
    long long iv = strtoll(tok, &end, 0);
    if (end != tok && *end == 0) { *out = (double)iv; return 1; }
    static char *hdr;
    if (!hdr) {
        char path[MAX_PATH * 2]; DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        while (n && path[n - 1] != '\\' && path[n - 1] != '/') n--;
        snprintf(path + n, sizeof path - n, "fighters\\sdk_generated\\lua_const.h");
        FILE *f = fopen(path, "rb");
        if (!f) { printf("[debug] cannot open %s\n", path); hdr = ""; }
        else { fseek(f, 0, SEEK_END); long size = ftell(f); fseek(f, 0, SEEK_SET); hdr = calloc(1, (size_t)size + 1); if (fread(hdr, 1, (size_t)size, f) != (size_t)size) hdr[0] = 0; fclose(f); }
    }
    char key[96]; snprintf(key, sizeof key, " %.80s{", tok);
    const char *hit = strstr(hdr, key);
    const Export *tab = exp_find("_ZN7lua2cpp21L2CAgentGeneratedBase19const_value_table__E");
    if (!hit || !tab) return 0;
    *out = (double)(int32_t)rd32(tab->value + 4ull * (unsigned)atoi(hit + strlen(key)));
    return 1;
}
static uint64_t fd_fn(const char *name) {
    static struct { const char *name; uint64_t addr; } cache[24]; static int n;
    for (int i = 0; i < n; i++) if (cache[i].name == name) return cache[i].addr;
    uint64_t a = hk_address_of(name);
    if (!a) printf("[debug] game function not found: %s\n", name);
    if (n < 24) { cache[n].name = name; cache[n].addr = a; n++; }
    return a;
}
/* one call of a game function: x0 = the module accessor, up to 5 integer arguments, one float; the context is put back */
static uint64_t fd_call(Ctx *c, const char *name, uint64_t boma, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, float f0, float *fret) {
    uint64_t a = fd_fn(name);
    if (!a) return 0;
    Ctx saved = *c;
    c->x[0] = boma; c->x[1] = x1; c->x[2] = x2; c->x[3] = x3; c->x[4] = x4; c->x[5] = x5;
    c->v[0].d[0] = 0; c->v[0].d[1] = 0; memcpy(&c->v[0], &f0, 4);
    call_indirect(c, a);
    uint64_t r = c->x[0];
    if (fret) memcpy(fret, &c->v[0], 4);
    *c = saved;
    return r;
}
#define FD_N(s) "_ZN3app8lua_bind" s "EPNS_26BattleObjectModuleAccessorE"
static void fd_run(Ctx *c, FsDbgCmd *d, uint64_t boma, int k) {
    double a[3] = {0, 0, 0};
    for (int i = 0; i < d->nargs; i++)
        if (!fd_value(d->arg[i], &a[i])) { printf("[debug] frame %lld fighter %d: %s: unknown value %s\n", g_fd_f[k].frame, d->fighter, g_fd_names[d->cmd], d->arg[i]); return; }
    int status = (int)fd_call(c, FD_N("30StatusModule__status_kind_impl"), boma, 0, 0, 0, 0, 0, 0, NULL);
    int situation = (int)fd_call(c, FD_N("33StatusModule__situation_kind_impl"), boma, 0, 0, 0, 0, 0, 0, NULL);
    printf("[debug] frame %lld fighter %d: %s %s %s %s (status now 0x%x, situation %d)\n", g_fd_f[k].frame, d->fighter, g_fd_names[d->cmd],
           d->nargs > 0 ? d->arg[0] : "", d->nargs > 1 ? d->arg[1] : "", d->nargs > 2 ? d->arg[2] : "", status, situation);
    fflush(stdout);
    g_fd_done++;
    switch (d->cmd) {
    case FD_STATUS: fd_call(c, FD_N("52StatusModule__change_status_request_from_script_impl") "ib", boma, (uint64_t)(int64_t)a[0], d->nargs > 1 ? (uint64_t)a[1] : 1, 0, 0, 0, 0, NULL); break;
    case FD_STATUSREQ: fd_call(c, FD_N("40StatusModule__change_status_request_impl") "ib", boma, (uint64_t)(int64_t)a[0], d->nargs > 1 ? (uint64_t)a[1] : 0, 0, 0, 0, 0, NULL); break;
    case FD_ITEM: {
        uint64_t r = fd_call(c, "_ZN3app8lua_bind26ItemModule__have_item_implEPNS_26BattleObjectModuleAccessorENS_8ItemKindEiibb", boma, (uint64_t)(int64_t)a[0], (uint64_t)(int64_t)a[1], 0, 0, 0, 0, NULL);
        uint64_t have = fd_call(c, FD_N("29ItemModule__is_have_item_impl") "i", boma, 0, 0, 0, 0, 0, 0, NULL);
        printf("[debug]   have_item returned %llx, is_have_item(0) = %d\n", (unsigned long long)r, (int)(have & 1));
        break; }
    case FD_NOITEM: fd_call(c, FD_N("27ItemModule__remove_all_impl"), boma, 0, 0, 0, 0, 0, 0, NULL); break;
    case FD_FINAL: {
        double flag;
        if (!fd_value("FIGHTER_INSTANCE_WORK_ID_FLAG_FINAL", &flag)) { printf("[debug]   FIGHTER_INSTANCE_WORK_ID_FLAG_FINAL not found\n"); break; }
        fd_call(c, FD_N("24WorkModule__on_flag_impl") "i", boma, (uint64_t)(int64_t)flag, 0, 0, 0, 0, 0, NULL);
        printf("[debug]   is_flag(FIGHTER_INSTANCE_WORK_ID_FLAG_FINAL) = %d\n", (int)(fd_call(c, FD_N("24WorkModule__is_flag_impl") "i", boma, (uint64_t)(int64_t)flag, 0, 0, 0, 0, 0, NULL) & 1));
        break; }
    case FD_TERM: {      /* S14: term <f> <transition term id> [1]: log WorkModule::is_enable_transition_term; with 1 first enable_transition_term */
        if (d->nargs > 1 && a[1] != 0) fd_call(c, FD_N("39WorkModule__enable_transition_term_impl") "i", boma, (uint64_t)(int64_t)a[0], 0, 0, 0, 0, 0, NULL);
        printf("[debug]   is_enable_transition_term(%s) = %d\n", d->arg[0], (int)(fd_call(c, FD_N("42WorkModule__is_enable_transition_term_impl") "i", boma, (uint64_t)(int64_t)a[0], 0, 0, 0, 0, 0, NULL) & 1));
        break; }
    case FD_FLAG: fd_call(c, d->nargs > 1 && a[1] == 0 ? FD_N("25WorkModule__off_flag_impl") "i" : FD_N("24WorkModule__on_flag_impl") "i", boma, (uint64_t)(int64_t)a[0], 0, 0, 0, 0, 0, NULL); break;
    case FD_DAMAGE: fd_call(c, FD_N("29DamageModule__add_damage_impl") "fi", boma, 0, 0, 0, 0, 0, (float)a[0], NULL); break;
    case FD_POS: case FD_AIR: {
        static float v[4];
        if (d->cmd == FD_POS) { v[0] = (float)a[0]; v[1] = (float)a[1]; }
        else {
            fd_call(c, FD_N("25PostureModule__pos_x_impl"), boma, 0, 0, 0, 0, 0, 0, &v[0]);
            fd_call(c, FD_N("25PostureModule__pos_y_impl"), boma, 0, 0, 0, 0, 0, 0, &v[1]);
            v[1] += d->nargs > 0 ? (float)a[0] : 30.0f;
        }
        v[2] = v[3] = 0;
        fd_call(c, FD_N("30PostureModule__set_pos_2d_impl") "RKN3phx8Vector2fE", boma, (uint64_t)(uintptr_t)v, 0, 0, 0, 0, 0, NULL);
        if (d->cmd == FD_AIR) {
            double air, correct;
            if (fd_value("SITUATION_KIND_AIR", &air)) fd_call(c, FD_N("37StatusModule__set_situation_kind_impl") "NS_13SituationKindEb", boma, (uint64_t)(int64_t)air, 0, 0, 0, 0, 0, NULL);
            if (fd_value("GROUND_CORRECT_KIND_AIR", &correct)) fd_call(c, FD_N("30GroundModule__set_correct_impl") "NS_17GroundCorrectKindE", boma, (uint64_t)(int64_t)correct, 0, 0, 0, 0, 0, NULL);
        }
        break; }
    }
}
static void fd_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t agent = c->x[0], boma = agent ? rd64(agent + 0x40) : 0;
    FsFiber *ff = g_fs_fls != FLS_OUT_OF_INDEXES ? FlsGetValue(g_fs_fls) : NULL;
    if (boma && (rd32(boma + 8) >> 28) == 0) {
        int k = 0;
        while (k < g_fd_n && g_fd_f[k].boma != boma) k++;
        if (k == g_fd_n && g_fd_n < 8) {
            double id = 0;
            g_fd_f[k].boma = boma; g_fd_f[k].frame = 0; g_fd_f[k].entry = -1;
            if (fd_value("FIGHTER_INSTANCE_WORK_ID_INT_ENTRY_ID", &id)) g_fd_f[k].entry = (int)fd_call(c, FD_N("24WorkModule__get_int_impl") "i", boma, (uint64_t)(int64_t)id, 0, 0, 0, 0, 0, NULL);
            printf("[debug] fighter with entry id %d seen (module accessor %d of this run)\n", g_fd_f[k].entry, k);
            g_fd_n++;
        }
        if (k < g_fd_n) {
            if (ff && (ff->rec || ff->replay)) g_fd_waited++;      /* inside a recording of the differential test: next frame */
            else {
                long long f = ++g_fd_f[k].frame;
                for (int i = 0; i < g_fd_count; i++) {
                    FsDbgCmd *d = &g_fd_cmd[i];
                    if (d->fighter != g_fd_f[k].entry) continue;
                    if (d->every ? f % d->every != 0 : (d->done || f < d->at)) continue;
                    d->done = 1;
                    fd_run(c, d, boma, k);
                }
            }
        }
    }
    g_fd_orig(c);
}
/* ---------------------------------------------------------------- S11: SSBU_FIGHT_HASH / SSBU_DETERMINISTIC (test only)
 * SSBU_FIGHT_HASH=<file>|1: one line per fighter per FIGHT FRAME (frame = that fighter's count of calls of the frame
 * line sys_line_system_control_fighter since it was first seen; nothing to do with the time since process start),
 * written at the ENTRY of the frame line: position, status kind, motion hash, motion frame, damage (the game's own
 * lua_bind getters), the script random generators (the 9 xorshift128 generators of 20 bytes that app::sv_math::rand
 * indexes by its Hash40 argument: [[[main 0x52c25b0]]] + 0x14 * i = 4 state words + a call counter; logged: sum of the
 * 9 counters, FNV-1a of the 180 bytes) and a running FNV-1a over everything logged so far.
 * The first frame of the first fighter raises the input mark "fight" (SSBU_SEQ step `@fight`, nnhid.c marks).
 * SSBU_DETERMINISTIC=1: see plan\phases\R0_determinism.md. Here: the 9 generators are set to a fixed state at the
 * first battle frame (SSBU_DET_OFF=1 leaves them alone, for diagnosis). */
void input_mark(const char *what);                      /* nnhid.c */
int rbn_handshake_wanted(void); uint64_t rbn_match_seed(void);   /* rb_net.c (R3: 0 / 0 without SSBU_RB_NET) */
extern long long g_rgs_next; extern int g_ra_phase;   /* the ring simulation pass about to run (pass index goes back after a rollback) and its phase: 0 real, 2 re-run */
static ImportFn g_fh_orig;
static struct { uint64_t boma; int entry; long long frame; } g_fh_f[8];
static int g_fh_n, g_fh_det, g_fh_detoff;
static FILE *g_fh_out;
static uint64_t g_fh_run = 0xcbf29ce484222325ull;
static long long g_fh_poll_hist[4]; static unsigned long g_fh_poll_tid;
static long long g_fh_drop_calls, g_fh_drop_frames, g_fh_frame_ends;
static uint64_t fh_fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; while (n--) { h ^= *b++; h *= 0x100000001b3ull; } return h; }
static uint64_t fh_rng_base(void) {
    uint64_t p = rd64(GA(0x71052c25b0ull));
    if (p) p = rd64(p);
    if (p) p = rd64(p);
    return p;
}
static void fh_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t agent = c->x[0], boma = agent ? rd64(agent + 0x40) : 0;
    { static int dn; if (agent && dn < 12 && g_fh_n) { uint64_t key[2] = {agent, boma}; static uint64_t seen[16][2]; int f = 0; for (int q = 0; q < dn; q++) if (seen[q][0] == key[0]) f = 1; if (!f) { seen[dn][0] = agent; seen[dn][1] = boma; dn++; printf("[fight] S20t: frame line, agent %llx boma %llx category word %x, first fighter frame %lld\n", (unsigned long long)agent, (unsigned long long)boma, boma ? rd32(boma + 8) : 0, g_fh_f[0].frame); fflush(stdout); } } }
    if (boma && (rd32(boma + 8) >> 28) == 0) {
        int k = 0;
        while (k < g_fh_n && g_fh_f[k].boma != boma) k++;
        if (k == g_fh_n && g_fh_n < 8) {
            double id = 0;
            g_fh_f[k].boma = boma; g_fh_f[k].frame = 0; g_fh_f[k].entry = -1;
            if (fd_value("FIGHTER_INSTANCE_WORK_ID_INT_ENTRY_ID", &id)) g_fh_f[k].entry = (int)fd_call(c, FD_N("24WorkModule__get_int_impl") "i", boma, (uint64_t)(int64_t)id, 0, 0, 0, 0, 0, NULL);
            g_fh_n++;
            if (k > 0 || g_fh_n > 1) { printf("[fight] S20t: fighter slot %d: boma %llx entry %d, category word %x, frame line call %lld\n", k, (unsigned long long)boma, g_fh_f[k].entry, rd32(boma + 8), g_fh_f[0].frame); fflush(stdout); }
            if (k == 0) {
                uint64_t rb = fh_rng_base();
                printf("[fight] first battle frame (fighter entry %d); input mark \"fight\" raised; script random generators at %llx\n", g_fh_f[k].entry, (unsigned long long)rb);
                if (rb) { printf("[fight] generators before:"); for (int i = 0; i < 9; i++) printf(" %08x.%u", rd32(rb + 0x14ull * i + 12), rd32(rb + 0x14ull * i + 16)); printf("\n"); }
                int rbn_match_cancelled(void);
                if (g_fh_det && !(g_fh_detoff & 1) && rb && !(rbn_match_cancelled() == 2 && !getenv("SSBU_DETERMINISTIC"))) {      /* (R3 pub2) a room fight with only rollback mode on: the game's own generators */
                    uint64_t ms = rbn_match_seed();    /* R3: the seed of the online match handshake (0 without one: the fixed state below) */
                    if (ms) {
                        for (int i = 0; i < 9; i++) {
                            uint64_t z = ms + 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; z ^= z >> 31;
                            uint64_t y = z * 0xD1342543DE82EF95ull + 1;
                            wr32(rb + 0x14ull * i, (uint32_t)z | 1u); wr32(rb + 0x14ull * i + 4, (uint32_t)(z >> 32)); wr32(rb + 0x14ull * i + 8, (uint32_t)y); wr32(rb + 0x14ull * i + 12, (uint32_t)(y >> 32)); wr32(rb + 0x14ull * i + 16, 0);
                        }
                        printf("[fight] SSBU_RB_NET: the 9 script random generators set from the agreed match seed %016llx, counters 0\n", (unsigned long long)ms);
                        if (getenv("SSBU_RB_NET_WATCH")) { void debug_watch(const uint64_t *a, int n); uint64_t w[2] = {rb, GA(0x71052c41b0ull) + 0x98}; const char *g = getenv("SSBU_RB_NET_WATCH_GEN"); if (g) w[0] = rb + 0x14ull * (uint64_t)atoi(g) + 8; debug_watch(w, 2); }   /* research: who writes the generators / match block + 0x98 next (_GEN=<i>: generator i's state words 2..3, 8-byte aligned) */
                    } else {
                    for (int i = 0; i < 9; i++) {
                        wr32(rb + 0x14ull * i, 0x12345678u + 0x9E3779B9u * (unsigned)i); wr32(rb + 0x14ull * i + 4, 0x2468ACE1u ^ (unsigned)i);
                        wr32(rb + 0x14ull * i + 8, 0x0F1E2D3Cu + (unsigned)i); wr32(rb + 0x14ull * i + 12, 0x89ABCDEFu - (unsigned)i); wr32(rb + 0x14ull * i + 16, 0);
                    }
                    printf("[fight] SSBU_DETERMINISTIC: the 9 script random generators set to a fixed state, counters 0\n");
                    }
                }
                if (getenv("SSBU_WORK_RESEARCH")) {        /* S11 part 4: where the work module keeps its variables */
                    uint64_t wm = rd64(boma + 0x50), vt = wm ? rd64(wm) : 0;
                    printf("[work] module %llx vtable %llx: get_float %llx get_int %llx set_int %llx is_flag %llx\n", (unsigned long long)wm, (unsigned long long)vt,
                           (unsigned long long)rd64(vt + 0x58), (unsigned long long)rd64(vt + 0x98), (unsigned long long)rd64(vt + 0xa0), (unsigned long long)rd64(vt + 0x108));
                    uint64_t areas = rd64(wm + 0x18);
                    for (int a = 0; a < 2 && areas; a++) {
                        uint64_t ar = rd64(areas + 8ull * a);
                        printf("[work] area %d at %llx:", a, (unsigned long long)ar);
                        for (int i = 0; i < 0x50 && ar; i += 8) printf(" %llx", (unsigned long long)rd64(ar + i));
                        printf("\n");
                    }
                    for (int i = 0; i < 0x60; i += 0x20) printf("[work]   +%03x: %016llx %016llx %016llx %016llx\n", i, (unsigned long long)rd64(wm + i), (unsigned long long)rd64(wm + i + 8), (unsigned long long)rd64(wm + i + 16), (unsigned long long)rd64(wm + i + 24));
                }
                fflush(stdout);
                input_mark("fight");
            }
        }
        /* (rb6) the online checksum needs the record even without SSBU_FIGHT_HASH: before, a tester's game hashed nothing
         * and sent no check (every report said "checksums compared 0") */
        int rbn_hash_wanted(void); int want = g_fh_out || rbn_hash_wanted();
        if (k < g_fh_n && !want) ++g_fh_f[k].frame;
        if (k < g_fh_n && want) {
            long long f = ++g_fh_f[k].frame;
            float x = 0, y = 0, mf = 0, dmg = 0;
            int st = (int)fd_call(c, FD_N("30StatusModule__status_kind_impl"), boma, 0, 0, 0, 0, 0, 0, NULL);
            uint64_t mot = fd_call(c, FD_N("30MotionModule__motion_kind_impl"), boma, 0, 0, 0, 0, 0, 0, NULL);
            fd_call(c, FD_N("24MotionModule__frame_impl"), boma, 0, 0, 0, 0, 0, 0, &mf);
            fd_call(c, FD_N("25PostureModule__pos_x_impl"), boma, 0, 0, 0, 0, 0, 0, &x);
            fd_call(c, FD_N("25PostureModule__pos_y_impl"), boma, 0, 0, 0, 0, 0, 0, &y);
            fd_call(c, FD_N("25DamageModule__damage_impl") "i", boma, 0, 0, 0, 0, 0, 0, &dmg);
            uint64_t rb = fh_rng_base(), rh = 0xcbf29ce484222325ull; uint32_t rc = 0, w[45];
            if (rb) { for (int i = 0; i < 45; i++) w[i] = rd32(rb + 4ull * i); for (int i = 0; i < 9; i++) rc += w[i * 5 + 4]; rh = fh_fnv(rh, w, sizeof w); }
            struct { long long f; int k, st; uint64_t mot; float mf, x, y, dmg; uint64_t rh; } rec;
            memset(&rec, 0, sizeof rec);
            rec.f = f; rec.k = g_fh_f[k].entry; rec.st = st; rec.mot = mot; rec.mf = mf; rec.x = x; rec.y = y; rec.dmg = dmg; rec.rh = rh;
            g_fh_run = fh_fnv(g_fh_run, &rec, sizeof rec);
            { void rbn_hash_add(const void *p, size_t n); struct { int k, st; uint64_t mot; float mf, x, y, dmg; uint64_t rh; } r3 = {rec.k, rec.st, rec.mot, rec.mf, rec.x, rec.y, rec.dmg, rec.rh};
              rbn_hash_add(&r3, sizeof r3); }      /* R3 (rb_net.c, SSBU_RB_NET only): the frame's checksum, without the per-process frame count */
            uint32_t bx, by, bm, bd; memcpy(&bx, &x, 4); memcpy(&by, &y, 4); memcpy(&bm, &mf, 4); memcpy(&bd, &dmg, 4);
            /* N12: SSBU_FIGHT_HASH_WORK=<int work id, hex or constant name>: that int variable of each fighter at the end of
             * the line ("w <value>"); not part of H (the hash stays comparable with runs without it) */
            static int wk_on = -1; static double wk_id;
            if (wk_on < 0) { const char *w = getenv("SSBU_FIGHT_HASH_WORK"); wk_on = w && w[0] && (fd_value(w, &wk_id) || ((wk_id = (double)strtoul(w, NULL, 16)), wk_id != 0)); }
            char wtxt[32] = "";
            if (!g_fh_out) goto no_line;
            if (wk_on) snprintf(wtxt, sizeof wtxt, " w %d", (int)fd_call(c, FD_N("24WorkModule__get_int_impl") "i", boma, (uint64_t)(int64_t)wk_id, 0, 0, 0, 0, 0, NULL));
            fprintf(g_fh_out, "F %lld P %lld %d e %d st %x mot %llx mf %08x x %08x y %08x dmg %08x rngcalls %u rng %08x H %016llx  (%.3f %.3f frame %.2f %.2f%%)%s\n",
                    f, g_rgs_next, g_ra_phase, g_fh_f[k].entry, st, (unsigned long long)mot, bm, bx, by, bd, rc, (unsigned)(rh ^ (rh >> 32)), (unsigned long long)g_fh_run, (double)x, (double)y, (double)mf, (double)dmg, wtxt);
            if ((f & 63) == 0) fflush(g_fh_out);
            if (k == 0 && f % 300 == 0 && rb) {            /* which generators are drawn from: the 9 call counters */
                printf("[fight] frame %lld: generator calls", f);
                for (int i = 0; i < 9; i++) printf(" %u", w[i * 5 + 4]);
                printf("; frame line on thread %lu, pad poll on thread %lu; polls that saw 0 / 1 / 2 / 3+ new fight frames: %lld %lld %lld %lld; frame ends %lld, of them %lld reported dropped refreshes (%lld in all)\n", (unsigned long)GetCurrentThreadId(), (unsigned long)g_fh_poll_tid,
                       g_fh_poll_hist[0], g_fh_poll_hist[1], g_fh_poll_hist[2], g_fh_poll_hist[3], g_fh_frame_ends, g_fh_drop_calls, g_fh_drop_frames);
            }
            no_line:;
        }
    }
    g_fh_orig(c);
}
/* SSBU_DETERMINISTIC: main 0x710386fc30 (frame end of the graphics module: acquires the window texture) compares the
 * refresh count of the game's VSync Count Thread (window + 0x138) with the count at the last frame (+ 0x140) and
 * reports count - (last + 1) "dropped" refreshes through its third argument, result code 0xF0044007 / 0xF0044005; the
 * game then runs that many EXTRA logic frames on the same pad sample. The number depends on when the counting thread
 * ran: wall time. Here: the report is cleared after the call (no extra logic frames, ever). SSBU_DET_OFF=4 leaves it. */
static ImportFn g_fh_orig_frame_end;
static void fh_frame_end(Ctx *c, void *user) {
    (void)user;
    uint64_t p1 = c->x[0], p3 = c->x[2];
    g_fh_orig_frame_end(c);
    g_fh_frame_ends++;
    if (p3 && rd64(p3)) {
        g_fh_drop_calls++; g_fh_drop_frames += (long long)rd64(p3);
        if (!(g_fh_detoff & 4)) { wr64(p3, 0); if (p1 && (rd32(p1) == 0xF0044007u || rd32(p1) == 0xF0044005u)) wr32(p1, 0); }
    }
}
/* nnhid.c, once per pad poll of player 1: how many steps the input sequence (SSBU_SEQ) advances. Normally 1 (the
 * sequence counts polls). SSBU_DETERMINISTIC, from the first battle frame on: the number of FIGHT FRAMES of the first
 * fighter since the last poll, so that the sequence position is a function of the fight frame alone (measured S11:
 * the fighters' frame line ran 510 times in one run and 605 times in another during the same 462 polls after the
 * first battle frame). SSBU_DET_OFF=2 keeps poll counting. Nothing after the fight (no fight frames, no steps). */
uint64_t hook_address(const char *name);
int fight_seq_ticks(void) {
    static long long last;
    if (!g_fh_det || (g_fh_detoff & 2) || !g_fh_n) return 1;
    long long f = g_fh_f[0].frame, n = f - last;
    {   /* (R3 hs12, test sequences of two matches) after a fight has ended (the game's ready-go cleared) the sequence counts
         * polls again until the next fight's ready-go; a new fight's frame count starting lower than the last one is a
         * new start, not "no step" (before: nothing after the fight, so a sequence could not reach a second match) */
        static uint64_t inst; static int was_on, ended;
        if (!inst) inst = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
        uint64_t mgr = inst ? rd64(inst) : 0, impl = mgr ? rd64(mgr) : 0;
        int on = impl && rd8(impl + 0xd2);
        if (on) { was_on = 1; ended = 0; } else if (was_on) ended = 1;
        if (n < 0 && was_on) { last = f; return ended ? 1 : 0; }
        if (ended) { last = f; return 1; }
    }
    last = f;
    g_fh_poll_hist[n < 0 ? 0 : n > 3 ? 3 : n]++; g_fh_poll_tid = GetCurrentThreadId();
    return n < 0 ? 0 : n > 100000 ? 100000 : (int)n;
}
/* research (SSBU_RB_LOADWATCH=1, 13.0.5): which threads append to the two load lists of the online-fight main
 * 0x71015d99b0 (base + 0x19528 / + 0x19540, 0x18-byte entries) and change an entry's state (+ 0x14) */
static ImportFn g_lw_orig;
static int g_lw_watch, g_lw_fix;
int rbn_started(void);
static void lw_gate(Ctx *c, void *user) {
    static long long n; static uint64_t last_sig, last_b0, last_b1; static int arms;
    (void)user;
    n++;
    uint64_t p = rd64(0x71052c4000ull + 1176), base = p ? rd64(p) : 0;
    uint64_t q = rd64(0x71052c5000ull + 248), R = q ? rd64(q) : 0;
    if (g_lw_watch && base && R) {
        /* the gate's view: entries (id:state) of both lists, then the ids still waiting in the loader's request queue
         * (R + 1104 count, + 1112 head, + 1096 capacity, + 1128 slots of 16 bytes: + 4 taken, + 8 id) */
        char line[1024]; int k = 0; uint64_t sig = 0xcbf29ce484222325ull;
        for (int l = 0; l < 2; l++) {
            uint64_t b = rd64(base + 0x19528 + 0x18 * l), e = rd64(base + 0x19528 + 0x18 * l + 8);
            k += snprintf(line + k, sizeof line - k, " L%d:", l);
            for (uint64_t a = b; a < e && a < b + 0x18 * 24 && k < 700; a += 0x18) {
                unsigned id = rd16(a + 16), st = rd32(a + 20);
                k += snprintf(line + k, sizeof line - k, " %u:%u", id, st); sig = (sig ^ (id << 8 | st)) * 0x100000001b3ull;
            }
        }
        uint32_t cnt = rd32(R + 1104); uint64_t cap = rd64(R + 1096), head = rd64(R + 1112), buf = rd64(R + 1128);
        k += snprintf(line + k, sizeof line - k, " | queue %u:", cnt);
        for (uint32_t i = 0; i < cnt && i < 24 && cap && k < 980; i++) {
            uint64_t s = buf + ((head + i) % cap) * 16;
            unsigned id = rd16(s + 8), taken = rd32(s + 4);
            k += snprintf(line + k, sizeof line - k, " %u%s", id, taken ? "t" : ""); sig = (sig ^ (id << 1 | (taken != 0))) * 0x100000001b3ull;
        }
        if (sig != last_sig) { last_sig = sig; printf("[loadwatch] call %lld fight frame %d tid %lu:%s\n", n, g_fh_n, GetCurrentThreadId(), line); fflush(stdout); }
        /* hardware watch on the first entry's state of each list and the queue's count / head (re-armed when a list moves) */
        uint64_t b0 = rd64(base + 0x19528), b1 = rd64(base + 0x19540);
        if ((b0 != last_b0 || b1 != last_b1) && arms < 30 && (b0 || b1)) {
            void debug_watch(const uint64_t *a, int n);
            uint64_t w[4] = {b0 ? b0 + 16 : R + 1104, b1 ? b1 + 16 : R + 1104, R + 1104, R + 1112};
            last_b0 = b0; last_b1 = b1; arms++;
            printf("[loadwatch] call %lld: watching L0[0] %llx L1[0] %llx count %llx head %llx\n", n, (unsigned long long)w[0], (unsigned long long)w[1], (unsigned long long)w[2], (unsigned long long)w[3]);
            fflush(stdout);
            debug_watch(w, 4);
        }
    }
    /* the entry adds draw from the script random generators; those draws happen on whatever logic frame the add falls on,
     * which loads and rollback move, so in an online match they leave the generators as they were */
    uint64_t rb = (g_lw_fix || g_lw_watch) ? fh_rng_base() : 0; uint32_t w[45];
    if (rb) for (int i = 0; i < 45; i++) w[i] = rd32(rb + 4ull * i);
    g_lw_orig(c);
    if (rb && rb == fh_rng_base()) {
        int d = 0;
        for (int i = 0; i < 45; i++) d += rd32(rb + 4ull * i) != w[i];
        if (d) {
            static int logged;
            if (logged++ < 20) { printf("[loadfix] call %lld (match %s): the gate changed %d generator words%s\n", n, rbn_started() ? "on" : "not started", d, g_lw_fix ? ", put back" : ""); fflush(stdout); }
            if (g_lw_fix) for (int i = 0; i < 45; i++) wr32(rb + 4ull * i, w[i]);
        }
    }
}
static void fh_exit(void) { if (g_fh_out) { fflush(g_fh_out); if (g_fh_out != stdout) fclose(g_fh_out); g_fh_out = NULL; } }
static void fight_hash_start(void) {
    const char *h = getenv("SSBU_FIGHT_HASH"), *d = getenv("SSBU_DETERMINISTIC"), *o = getenv("SSBU_DET_OFF");
    g_fh_det = (d && d[0] && d[0] != '0') || rbn_handshake_wanted();      /* R3: a match between online players needs it (no extra logic frames; generators from the agreed seed) */
    g_fh_detoff = o ? atoi(o) : 0;
    if ((!h || !h[0] || h[0] == '0') && !g_fh_det) return;
    if (h && h[0] && h[0] != '0') g_fh_out = strcmp(h, "1") == 0 ? stdout : fopen(h, "w");
    int id = hook_install_name("_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv", fh_hook, NULL, &g_fh_orig);
    printf("[fight] SSBU_FIGHT_HASH %s, SSBU_DETERMINISTIC %d (off mask %d); hook on the fighters' frame line: %s\n", g_fh_out ? "on" : "off", g_fh_det, g_fh_detoff, id ? "installed or waiting for lua2cpp_common" : hook_error());
    if (g_fh_det) printf("[fight] SSBU_DETERMINISTIC: hook on the frame end %llx (dropped refreshes -> extra logic frames): %s\n", (unsigned long long)GA(0x710386fc30ull),
                         hook_install(GA(0x710386fc30ull), fh_frame_end, NULL, &g_fh_orig_frame_end) ? "installed" : hook_error());
    {   const char *lf = getenv("SSBU_RB_LOADFIX");
        g_lw_watch = getenv("SSBU_RB_LOADWATCH") != NULL;
        g_lw_fix = rbn_handshake_wanted() && !(lf && lf[0] == '0');
    }
    if ((g_lw_watch || g_lw_fix) && g_game_build == GAME_13_0_5)
        printf("[loadfix] hook on 0x71015d99b0 (generator restore %d, trace %d): %s\n", g_lw_fix, g_lw_watch, hook_install(0x71015d99b0ull, lw_gate, NULL, &g_lw_orig) ? "installed" : hook_error());
    atexit(fh_exit);
}
static void fd_exit(void) { printf("[debug] %lld commands executed, %lld frames skipped because a differential recording was open\n", g_fd_done, g_fd_waited); fflush(stdout); }
static void fighter_debug_start(void) {
    const char *path = getenv("SSBU_FIGHTER_DEBUG");
    fight_hash_start();
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "r");
    if (!f) { printf("[debug] cannot open %s\n", path); return; }
    char line[400]; int cap = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        char when[16], cmd[16], a0[72] = "", a1[72] = "", a2[72] = ""; long long n; int fighter;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        int got = sscanf(line, "%15s %lld %15s %d %71s %71s %71s", when, &n, cmd, &fighter, a0, a1, a2);
        int kind = 0;
        while (kind < FD_KINDS && (got < 3 || strcmp(cmd, g_fd_names[kind]) != 0)) kind++;
        if (got < 4 || kind == FD_KINDS || (strcmp(when, "at") != 0 && strcmp(when, "every") != 0) || n < 1) { if (bad++ < 5) printf("[debug] line not understood: %s", line); continue; }
        if (g_fd_count == cap) { cap = cap ? cap * 2 : 256; g_fd_cmd = realloc(g_fd_cmd, (size_t)cap * sizeof *g_fd_cmd); }
        FsDbgCmd *d = &g_fd_cmd[g_fd_count++];
        memset(d, 0, sizeof *d);
        d->every = strcmp(when, "every") == 0 ? (int)n : 0; d->at = n; d->cmd = kind; d->fighter = fighter; d->nargs = got - 4;
        strcpy(d->arg[0], a0); strcpy(d->arg[1], a1); strcpy(d->arg[2], a2);
    }
    fclose(f);
    int id = hook_install_name("_ZN7lua2cpp16L2CFighterCommon31sys_line_system_control_fighterEv", fd_hook, NULL, &g_fd_orig);
    printf("[debug] SSBU_FIGHTER_DEBUG: %d commands read from %s (%d lines not understood); hook on the fighters' frame line: %s\n", g_fd_count, path, bad, id ? "installed or waiting for lua2cpp_common" : hook_error());
    atexit(fd_exit);
}
