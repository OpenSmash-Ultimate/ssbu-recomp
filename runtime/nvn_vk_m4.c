/* Track V, session 4 (M4, design section 5): stages, pipelines and the worker threads that make them AHEAD of the draw.
 * Included by nvn_vk_draw.c (not a build unit of its own); replaces that file's "stages" and "pipelines" sections and its
 * start-up workers of session 3.
 *
 *   - nvnProgramSetShaders (guest thread) -> nvn_vk_program_set: a copy of the program's machine code goes to the worker
 *     queue at once. A worker translates each stage (mx_translate -> glslang -> module, or the record on disk), then makes
 *     pipelines for the pair: the keys of ..\cache\vk\pipes.bin were all made at start; for a pair with no recorded key the
 *     keys are PREDICTED = the pair x the pass signatures most used so far (at most 3), triangles.
 *   - a draw looks up; when its stage or pipeline is being made by a worker it WAITS for it, when nobody makes it it makes
 *     it itself. Either way the render thread stands still at that draw: the present is late, no draw is dropped or
 *     reordered. Every such hold is counted (g_hold_*; "holds so far" in the nvn_vk_perf line).
 *   - with VK_EXT_extended_dynamic_state3 blend enable / equation / write mask, polygon mode, alpha to coverage, depth
 *     clamp and the sample mask are dynamic state, so the key is (stages, topology class, pass formats, samples, logic op,
 *     sample shading): few keys per pair, which is what makes the prediction possible.
 * One lock (g_sh_lock) guards the stage table, the pipeline table, the signature / pair tables and the job queue. */
typedef struct { uint8_t unit, dim, shadow; uint16_t binding; } VSmp;
typedef struct VStage {
    uint64_t h, hb;                                /* h: this variant (code, constants, canonical key, forced outputs); hb: code and constants only */
    int stage, code_len, ok; volatile LONG state, cp_state;      /* state: 1 being made, 2 made (ok says how it went); cp_state: compute pipeline 0 none, 1 being made, 2 made */
    uint32_t key[4], fo, in_attr, out_attr, id, cbufs, ssbos, ssbo_w, images, dep, tex_units; int uses_alpha, nsmp, cp_recorded, from_rec, fragonly, temp;      /* fragonly (session 9): handed over by a program without a vertex stage */
    VkPipeline cp; VkPipelineLayout cl; VkDescriptorSetLayout cdl[2];      /* compute stage: its pipeline */
    uint32_t local[3];
    VSmp smp[64];
    VkShaderModule mod; struct VStage *next, *next_h;
    VkPipeline lib, lib2; volatile LONG lib_state, lib2_state;      /* session 9 (SSBU_VK_GPL=2): the stage's pipeline library; lib2: a vertex stage with the empty fragment shader state */
} VStage;
#define MX_VK_VERSION 6u                        /* part of every hash: change it when mx_translate's Vulkan text or these records change */
typedef struct { uint32_t magic; int32_t stage, code_len; uint32_t key[4], fo; uint64_t spv, hb; uint32_t cbufs, ssbos, ssbo_w, images, dep, tex_units, in_attr, out_attr; int32_t uses_alpha, nsmp; VSmp smp[64]; } StageRec;
typedef struct { uint32_t magic, key[4], fo, dep, tex; uint64_t vh; } VarRec;      /* ..\cache\vk\stage\<stage>_<hb>.var: the variants of one code */
#define STAGEREC_MAGIC (0x52545356u + MX_VK_VERSION)
typedef struct { int stage, code_len, serial; const uint8_t *code, *cdata; uint32_t cdata_size, local[3], smem, lmem; } SSrc;
static CRITICAL_SECTION g_sh_lock;
static CONDITION_VARIABLE g_sh_cv, g_jq_cv;        /* g_sh_cv: something was finished; g_jq_cv: a job was queued */
/* ---- Session 12: the freeze watchdog's wait registry (nvn_vk_wd.c writes the report). Every wait of ours that has no
 * time-out, and the long ones, are entered here with the function and line that waits; the report names them. */
typedef struct { volatile LONG tid; const char *fn; int line; LONG64 since; } WdWait;
static WdWait g_wd_wait[64];
static volatile LONG64 g_wd_beat;                  /* QueryPerformanceCounter of the last present (normal or progress bar) */
static volatile LONG g_wd_where;                   /* the render thread's place in a present (1 barrier, 2 acquire, 4 present) */
static volatile LONG g_pcsave_want;                /* see pcache_save_at */
static void pcache_save_at(int where, int marker);
static volatile LONG64 g_wd_try;                 /* present attempts (one that failed or was skipped counts here, not in g_wd_beat) */
static int wd_enter(const char *fn, int line) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    LONG me = (LONG)GetCurrentThreadId();
    for (int i = 0; i < 64; i++) if (!g_wd_wait[i].tid && InterlockedCompareExchange(&g_wd_wait[i].tid, me, 0) == 0) { g_wd_wait[i].fn = fn; g_wd_wait[i].line = line; g_wd_wait[i].since = t.QuadPart; return i; }
    return -1;
}
static void wd_leave(int i) { if (i >= 0) g_wd_wait[i].tid = 0; }
#define WD_SLEEP(cv, lock, ms) do { int wd_i_ = wd_enter(__func__, __LINE__); SleepConditionVariableCS(cv, lock, ms); wd_leave(wd_i_); } while (0)
static VkPipelineCache g_pcache;
static LONG64 g_pw_stages, g_pw_pipes, g_pw_failed, g_pipe_miss;
#include "nvn_vk_prewarm.h"                         /* PR #33: SSBU_VK_PREWARM_PROFILE=1, the prewarm's measurements; pw_graphics / pw_compute */
static VStage *g_vstage[4096], *g_vh[4096];        /* by hb, by h */
static uint32_t g_vstage_ids;
static int (*g_glsl_to_spirv)(int, const char *, uint32_t **, size_t *, char *, size_t);
static void (*g_spirv_free)(uint32_t *);
enum { H_STAGE_DRAW, H_STAGE_WAIT, H_PIPE_DRAW, H_PIPE_WAIT, H_N };
static LONG64 g_hold_n[H_N], g_hold_tk[H_N], g_hold_max;
static LONG64 g_w_progs, g_w_stages, g_w_pipes, g_pred_made, g_pred_used, g_set_held, g_set_held_tk;
static void hold_note(int what, LONG64 ticks) { g_hold_n[what]++; g_hold_tk[what] += ticks; if (ticks > g_hold_max) g_hold_max = ticks; }
static int holdlog_on(void) { static int on = -1; if (on < 0) on = getenv("SSBU_VK_HOLDLOG") && atoi(getenv("SSBU_VK_HOLDLOG")); return on; }      /* session 7: a "nvn_vk_hold:" line for every stage / pipeline made at a draw */
static volatile LONG g_in_fight;
/* Session 7: the last frame that had compute dispatches. g_in_fight is per frame and some frames of a fight have none (run
 * s7fn2, frame 2072: nvnProgramSetShaders held the main thread 45 ms for background work in the middle of the countdown):
 * "in a fight" for the purpose of NOT holding a caller = a fight frame within the last 600 frames. */
static volatile LONG64 g_fight_last = -100000;
static int fight_recent(void) { return g_in_fight || nvn_front_frame_next() - g_fight_last < 600; }
static LONG64 g_var_known;
static LONG64 g_jq_queued, g_jq_done, g_bq_queued, g_bq_done;
/* SSBU_VK_HOLDLOG=1 only: when and by whom a stage's code was first handed to the workers (0 = nvnProgramSetShaders,
 * 1 = the container scan's background queue), so that a hold can say whether anyone knew the code before its draw. */
static struct { uint64_t hb; int32_t frame; int32_t origin; } g_ho[65536];
static void ho_note(uint64_t hb, int origin) {
    unsigned i = (unsigned)(hb * 0x9E3779B97F4A7C15ull >> 48);
    for (int n = 0; n < 16; n++, i = (i + 1) & 65535) { if (g_ho[i].hb == hb) return; if (!g_ho[i].hb) { g_ho[i].hb = hb; g_ho[i].frame = (int32_t)nvn_front_frame_next(); g_ho[i].origin = origin; return; } }
}
static const char *ho_text(uint64_t hb) {
    static char b[96];
    unsigned i = (unsigned)(hb * 0x9E3779B97F4A7C15ull >> 48);
    for (int n = 0; n < 16; n++, i = (i + 1) & 65535) if (g_ho[i].hb == hb) { snprintf(b, sizeof b, "code first handed over at frame %d by %s", g_ho[i].frame, g_ho[i].origin == 2 ? "the preparation" : g_ho[i].origin ? "the container scan" : "nvnProgramSetShaders"); return b; }
    return "code never handed over before this draw";
}
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
/* Session 7: the cache folder. SSBU_VK_CACHE=<folder> (tests: a first run on a cache of one's own, without touching the
 * shared ..\cache\vk); first called on the starting thread, before the workers exist. */
static const char *vkc_dir(void) {
    static char d[200];
    if (!d[0]) { const char *e = getenv("SSBU_VK_CACHE"); snprintf(d, sizeof d, "%s", e && e[0] ? e : "../cache/vk"); }
    return d;
}
static const char *vkc_file(const char *name) { static char p[4][260]; static LONG i; char *b = p[InterlockedIncrement(&i) & 3]; snprintf(b, 260, "%s/%s", vkc_dir(), name); return b; }
static void compute_pipe_make(VStage *v);
static uint64_t g_spv_unused;
static int g_offline;                              /* --prepare-shaders: stages are translated and written to ..\cache\vk, no Vulkan device exists */
static PFN_vkCmdSetColorBlendEnableEXT vkCmdSetColorBlendEnableEXT;
static PFN_vkCmdSetColorBlendEquationEXT vkCmdSetColorBlendEquationEXT;
static PFN_vkCmdSetColorWriteMaskEXT vkCmdSetColorWriteMaskEXT;
static PFN_vkCmdSetPolygonModeEXT vkCmdSetPolygonModeEXT;
static PFN_vkCmdSetAlphaToCoverageEnableEXT vkCmdSetAlphaToCoverageEnableEXT;
static PFN_vkCmdSetDepthClampEnableEXT vkCmdSetDepthClampEnableEXT;
static PFN_vkCmdSetSampleMaskEXT vkCmdSetSampleMaskEXT;

/* ---------------------------------------------------------------- stages: machine code -> Vulkan GLSL -> SPIR-V -> module */
static int ssrc_of(SSrc *o, const Prog *p, int s) {
    const Stage *st = &p->st[s];
    memset(o, 0, sizeof *o);
    if (!st->code || st->code_len <= 0) return 0;
    o->stage = s; o->code = st->code; o->code_len = st->code_len; o->serial = p->serial;
    if (st->ctl[3] && st->ctl[4]) { o->cdata = st->code - (s == 5 ? 0 : 0x30) + st->ctl[4]; o->cdata_size = st->ctl[3]; }      /* a compute stage has no program header */
    memcpy(o->local, st->local, sizeof o->local); o->smem = st->smem_bytes; o->lmem = st->lmem_bytes;
    return 1;
}
static uint64_t ssrc_hb(const SSrc *o) {
    uint32_t ver = MX_VK_VERSION;
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)o->stage, o->code, (size_t)o->code_len);
    if (o->stage == 5) { h = fnv(h, o->local, sizeof o->local); h = fnv(h, &o->smem, 4); h = fnv(h, &o->lmem, 4); }
    if (o->cdata) h = fnv(h ^ o->cdata_size, o->cdata, o->cdata_size);
    if (o->stage == 1 && RS_ON) h = fnv(h, "rs1", 3);      /* track O: the fragment stages of a run with an internal resolution scale are another translation (MxIn.screen_scale); at 1.0 the hash is the one it always was */
    return fnv(h, &ver, 4);
}
static VkShaderModule module_from_spv(uint64_t h) {              /* any thread; NULL when the file is not there */
    char path[128];
    VkShaderModule mod = NULL;
    snprintf(path, sizeof path, "%s/spv/%016llx.spv", vkc_dir(), (unsigned long long)h);
    LONG64 pt = g_pw_profile ? tk() : 0;           /* PR #33: SSBU_VK_PREWARM_PROFILE=1, history workers only */
    FILE *fp = fopen(path, "rb");
    if (!fp) { if (g_pw_profile) { g_pw_profile->spv_ticks += tk() - pt; g_pw_profile->spvs++; } return NULL; }
    if (g_offline) { fclose(fp); return (VkShaderModule)(uintptr_t)1; }      /* --prepare-shaders: no device; the file being there is all that is asked */
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint32_t *words = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (words && fread(words, 1, (size_t)n, fp) == (size_t)n && words[0] == 0x07230203) {
        LONG64 pm = g_pw_profile ? tk() : 0;
        if (g_pw_profile) { g_pw_profile->spv_ticks += pm - pt; g_pw_profile->spvs++; g_pw_profile->bytes += n; }
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = (size_t)n; ci.pCode = words;
        if (vkCreateShaderModule(V.device, &ci, NULL, &mod) != VK_SUCCESS) mod = NULL;
        if (g_pw_profile) { g_pw_profile->module_ticks += tk() - pm; g_pw_profile->modules++; }
    } else if (g_pw_profile) { g_pw_profile->spv_ticks += tk() - pt; g_pw_profile->spvs++; }
    free(words); fclose(fp);
    return mod;
}
static VkShaderModule module_from_glsl(int stage, const char *glsl, const char *what, uint64_t *spv_hash) {      /* any thread */
    char path[128], log[2000];
    uint32_t *words = NULL;
    size_t count = 0;
    VkShaderModule mod = NULL;
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)stage ^ 0x5601, glsl, strlen(glsl));
    *spv_hash = h;
    if ((mod = module_from_spv(h)) != NULL) { InterlockedIncrement64(&R.modules); return mod; }
    if (!g_glsl_to_spirv(stage, glsl, &words, &count, log, sizeof log)) { printf("nvn_vk: %s: glslang rejected the translated text: %s\n", what, log); fflush(stdout); return NULL; }
    snprintf(path, sizeof path, "%s/spv/%016llx.spv", vkc_dir(), (unsigned long long)h);
    FILE *fp = fopen(path, "wb");
    if (fp) { fwrite(words, 4, count, fp); fclose(fp); }
    if (g_offline) { g_spirv_free(words); InterlockedIncrement64(&R.modules); return (VkShaderModule)(uintptr_t)1; }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = count * 4; ci.pCode = words;
    VKC(vkCreateShaderModule(V.device, &ci, NULL, &mod));
    g_spirv_free(words);
    InterlockedIncrement64(&R.modules);
    return mod;
}
static int stage_rec_read(int s, uint64_t vh, StageRec *r) {
    char path[128];
    snprintf(path, sizeof path, "%s/stage/%d_%016llx.st", vkc_dir(), s, (unsigned long long)vh);
    LONG64 pt = g_pw_profile ? tk() : 0;           /* PR #33: SSBU_VK_PREWARM_PROFILE=1, history workers only */
    FILE *fp = fopen(path, "rb");
    if (!fp) { if (g_pw_profile) { g_pw_profile->record_ticks += tk() - pt; g_pw_profile->records++; } return 0; }
    size_t got = fread(r, 1, sizeof *r, fp);
    fclose(fp);
    if (g_pw_profile) { g_pw_profile->record_ticks += tk() - pt; g_pw_profile->records++; }
    return got == sizeof *r && r->magic == STAGEREC_MAGIC && r->stage == s && r->nsmp >= 0 && r->nsmp <= 64;
}
static void vstage_from_rec(VStage *v, const StageRec *r, uint64_t vh, VkShaderModule mod) {
    v->h = vh; v->hb = r->hb; v->code_len = r->code_len; memcpy(v->key, r->key, 16); v->fo = r->fo;
    v->cbufs = r->cbufs; v->ssbos = r->ssbos; v->ssbo_w = r->ssbo_w; v->images = r->images; v->dep = r->dep; v->tex_units = r->tex_units; v->in_attr = r->in_attr; v->out_attr = r->out_attr;
    v->uses_alpha = r->uses_alpha; v->nsmp = r->nsmp; memcpy(v->smp, r->smp, sizeof v->smp);
    v->mod = mod; v->ok = 1;
}
static int key_fits(int s, const uint32_t *want, const uint32_t *have, uint32_t dep, uint32_t tex) {
    uint32_t da = s == 0 ? dep | dep << 16 : 0;      /* 2026-10-20: bit 16 + n of key[0] = attribute n decoded from A2B10G10R10 SNORM (records made before carry no such bits) */
    return !((want[0] ^ have[0]) & da) && !((want[1] ^ have[1]) & da) && !((want[2] ^ have[2]) & tex) && !((want[3] ^ have[3]) & tex);
}
/* g_sh_lock held. The variant of this code that serves `key` (compared under the variant's own masks: a stage that reads no
 * integer attribute and no integer texture has ONE variant whatever the draw's state is). *busy: a variant of the same code
 * still in the making, whose masks are not known yet. */
static VStage *vstage_find(uint64_t hb, int s, int code_len, const uint32_t *key, uint32_t fo, VStage **busy) {
    *busy = NULL;
    for (VStage *v = g_vstage[hb & 4095]; v; v = v->next) {
        if (v->hb != hb || v->stage != s || v->code_len != code_len || v->fo != fo) continue;
        if (v->state < 2) { if (!memcmp(v->key, key, 16)) return v; *busy = v; continue; }
        if (key_fits(s, key, v->key, v->dep, v->tex_units)) return v;
    }
    return NULL;
}
static VStage *vstage_new(uint64_t hb, int s, int code_len, const uint32_t *key, uint32_t fo) {      /* g_sh_lock held */
    VStage *v = calloc(1, sizeof *v);
    v->hb = hb; v->stage = s; v->code_len = code_len; memcpy(v->key, key, 16); v->fo = fo; v->id = ++g_vstage_ids; v->state = 1;
    v->next = g_vstage[hb & 4095]; g_vstage[hb & 4095] = v;
    return v;
}
/* Any thread, no lock: fills v (in the table, state 1) from its record on disk, else with the translator and glslang. */
static void vstage_build(VStage *v, const SSrc *src) {
    int s = v->stage, done = 0;
    char path[128];
    StageRec r;
    snprintf(path, sizeof path, "%s/stage/%d_%016llx.var", vkc_dir(), s, (unsigned long long)v->hb);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        VarRec e;
        while (!done && fread(&e, 1, sizeof e, fp) == sizeof e) {
            VkShaderModule mod;
            if (e.magic != STAGEREC_MAGIC || e.fo != v->fo || !key_fits(s, v->key, e.key, e.dep, e.tex)) continue;
            if (stage_rec_read(s, e.vh, &r) && (mod = module_from_spv(r.spv)) != NULL) { vstage_from_rec(v, &r, e.vh, mod); v->from_rec = 1; InterlockedIncrement64(&R.modules); done = 1; }
        }
        fclose(fp);
    }
    if (!done) {
        MxIn in;
        MxOut out;
        char what[64];
        memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
        in.stage = s; in.code = src->code; in.code_len = src->code_len; in.cdata = src->cdata; in.cdata_size = src->cdata ? src->cdata_size : 0;
        in.int_attrs = v->key[0]; in.sint_attrs = v->key[1]; in.int_tex = v->key[2]; in.sint_tex = v->key[3]; in.force_out = v->fo;
        in.target = 1; in.binding_base = 40u * (unsigned)s; in.ssbo_base = 40u * (unsigned)s + 20; in.tex_base = 40u * (unsigned)s; in.image_base = 40u * (unsigned)s + 32; in.wmask_binding = 40u * (unsigned)s + 38;
        if (s == 5) {                              /* compute has a layout of its own (design, section 4): the bindings of stage 0 */
            in.binding_base = 0; in.ssbo_base = 20; in.tex_base = 0; in.image_base = 32; in.wmask_binding = 38; in.track_writes = 1;
            memcpy(in.local_size, src->local, sizeof in.local_size); in.smem_bytes = src->smem; in.lmem_bytes = src->lmem;
        }
        snprintf(what, sizeof what, "program #%d %s stage", src->serial, s == 5 ? "compute" : s ? "fragment" : "vertex");
        if (s == 1 && RS_ON) in.screen_scale = 1;
        if (!mx_translate(&in, &out)) { printf("nvn_vk: %s: machine code not translated: %s\n", what, out.why); fflush(stdout); }
        else {
            uint64_t spv_hash = 0;
            if (in.screen_scale) { InterlockedIncrement64(&g_rs_stages); if (strstr(out.glsl, "mx_fcs)")) InterlockedIncrement64(&g_rs_fcs); if (strstr(out.glsl, "mx_tf(")) InterlockedIncrement64(&g_rs_tf); }
            v->mod = module_from_glsl(s, out.glsl, what, &spv_hash);
            v->cbufs = out.cbufs; v->ssbos = out.ssbos; v->ssbo_w = out.ssbo_written; v->images = out.images; v->in_attr = out.in_attr; v->out_attr = out.out_attr;
            v->dep = s == 0 ? out.int_dep : 0; v->tex_units = out.tex_units; v->uses_alpha = out.uses_alpha; v->nsmp = out.nsmp;
            for (int i = 0; i < out.nsmp && i < 64; i++) {
                const char *ty = out.smp[i].type;
                int arr = strstr(ty, "Array") != NULL, dim = strstr(ty, "Cube") ? (arr ? DIM_CUBEA : DIM_CUBE) : strstr(ty, "3D") ? DIM_3D : strstr(ty, "1D") ? (arr ? DIM_1DA : DIM_1D) : (arr ? DIM_2DA : DIM_2D);
                if (strstr(ty, "MS") || strstr(ty, "Rect") || strstr(ty, "Buffer")) { static char msg[96]; snprintf(msg, sizeof msg, "a shader sampler of type %s", ty); nvn_vk_missing(msg); }
                v->smp[i].unit = out.smp[i].unit; v->smp[i].dim = (uint8_t)dim; v->smp[i].shadow = out.smp[i].shadow; v->smp[i].binding = out.smp[i].binding;
            }
            free(out.glsl);
            v->ok = v->mod != NULL;
            if (v->ok) {                           /* the canonical key: only the bits the code looks at */
                VarRec e;
                v->key[0] &= v->dep; v->key[1] &= v->dep; v->key[2] &= v->tex_units; v->key[3] &= v->tex_units;
                v->h = fnv(fnv(v->hb, v->key, 16), &v->fo, 4);
                memset(&r, 0, sizeof r);
                r.magic = STAGEREC_MAGIC; r.stage = s; r.code_len = v->code_len; memcpy(r.key, v->key, 16); r.fo = v->fo; r.spv = spv_hash; r.hb = v->hb;
                r.cbufs = v->cbufs; r.ssbos = v->ssbos; r.ssbo_w = v->ssbo_w; r.images = v->images; r.dep = v->dep; r.tex_units = v->tex_units; r.in_attr = v->in_attr; r.out_attr = v->out_attr;
                r.uses_alpha = v->uses_alpha; r.nsmp = v->nsmp; memcpy(r.smp, v->smp, sizeof r.smp);
                snprintf(path, sizeof path, "%s/stage/%d_%016llx.st", vkc_dir(), s, (unsigned long long)v->h);
                if ((fp = fopen(path, "wb")) != NULL) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
                memset(&e, 0, sizeof e);
                e.magic = STAGEREC_MAGIC; memcpy(e.key, v->key, 16); e.fo = v->fo; e.dep = v->dep; e.tex = v->tex_units; e.vh = v->h;
                snprintf(path, sizeof path, "%s/stage/%d_%016llx.var", vkc_dir(), s, (unsigned long long)v->hb);
                if ((fp = fopen(path, "ab")) != NULL) { fwrite(&e, 1, sizeof e, fp); fclose(fp); }
            }
        }
    }
    EnterCriticalSection(&g_sh_lock);
    if (v->ok && !v->temp) { v->next_h = g_vh[v->h & 4095]; g_vh[v->h & 4095] = v; }      /* temp (session 10): a stage of the preparation that is in no table */
    v->state = 2;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
}
/* Any thread: the variant of this code for (key, fo). *how: 0 it was there, 1 made by this call, 2 waited for the thread
 * that was making it. */
static void workers_boost(void);
static VStage *stage_ensure(const SSrc *src, uint64_t hb, const uint32_t *key, uint32_t fo, int *how) {
    VStage *v, *busy;
    *how = 0;
    EnterCriticalSection(&g_sh_lock);
    for (;;) {
        v = vstage_find(hb, src->stage, src->code_len, key, fo, &busy);
        if (v && v->state == 2) break;
        if (!v) v = busy;
        if (v) { if (!*how) { *how = 2; workers_boost(); } while (v->state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); continue; }
        v = vstage_new(hb, src->stage, src->code_len, key, fo);
        LeaveCriticalSection(&g_sh_lock);
        vstage_build(v, src);
        *how = 1;
        EnterCriticalSection(&g_sh_lock);
    }
    LeaveCriticalSection(&g_sh_lock);
    return v;
}
/* Render thread, at a draw or dispatch. */
/* ---- Session 9: shader_wait = skip (graphics.ini; SSBU_VK_NOHOLD=1 / =0 wins). What emulators call asynchronous shader
 * compilation: near a fight (fight_recent) a draw whose stage or pipeline is not ready is NOT waited for: the draw is left
 * out, a worker makes what was missing, and the draw appears in a later frame. Every draw left out is counted
 * ("nvn_vk_skip:" lines, "nvn_vk_skipf:" per frame, total at exit). Compute stages (skinning) are never skipped. Off by
 * default: the picture is wrong for the frames in which something is left out. */
static int skip_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("SSBU_VK_NOHOLD"); on = e ? atoi(e) != 0 : gfx_options()->shader_skip; } return on; }
static int fight_recent(void);
static int skip_active(void) { return skip_on() && fight_recent(); }
static LONG64 g_skip_n, g_skip_frames, g_skip_fr_n, g_skip_stage, g_skip_pipe;
static int g_skip_flag;                            /* set by vstage_get when it returned NULL because the stage is being made */
static void skip_queue_stage(const SSrc *src, const uint32_t *key, uint32_t fo);
static void skip_note(int what, uint64_t vh, uint64_t fh) {
    g_skip_n++; g_skip_fr_n++; if (what) g_skip_pipe++; else g_skip_stage++;
    { static int nl; if (nl++ < 600) printf("nvn_vk_skip: frame %lld: a draw left out, its %s was not ready: vertex code %016llx, fragment code %016llx\n", (long long)nvn_front_frame_next(), what ? "pipeline (stage library or link)" : "stage", (unsigned long long)vh, (unsigned long long)fh); }
}
static VStage *vstage_get(Prog *p, int s, const uint32_t *key, uint32_t fo) {
    Stage *st = &p->st[s];
    VStage *v = st->vk;
    SSrc src;
    int how;
    if (!st->code) return NULL;
    if (v && v->state == 2 && v->fo == fo && key_fits(s, key, v->key, v->dep, v->tex_units)) return v->ok ? v : NULL;
    if (!ssrc_of(&src, p, s)) return NULL;
    if (s != 5 && skip_active()) {                 /* shader_wait = skip: only what is ready */
        VStage *busy = NULL;
        uint64_t hb = ssrc_hb(&src);
        EnterCriticalSection(&g_sh_lock);
        v = vstage_find(hb, src.stage, src.code_len, key, fo, &busy);
        int ready = v && v->state == 2;
        LeaveCriticalSection(&g_sh_lock);
        if (!ready) { if (!v && !busy) skip_queue_stage(&src, key, fo); g_skip_flag = 1; skip_note(0, s ? 0 : hb, s ? hb : 0); return NULL; }
    }
    LONG64 t0 = tk();
    v = stage_ensure(&src, ssrc_hb(&src), key, fo, &how);
    if (how) { LONG64 dt = tk() - t0; hold_note(how == 1 ? H_STAGE_DRAW : H_STAGE_WAIT, dt); g_tm[6] += dt;
        if (holdlog_on()) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            printf("nvn_vk_hold: frame %lld (%s): %s STAGE %s, %.2f ms: program #%d, code %016llx, key asked %x %x %x %x (kept %x %x %x %x), forced outputs %x; %s\n", (long long)nvn_front_frame_next(), g_in_fight ? "fight" : "no fight",
                   s == 5 ? "compute" : s ? "fragment" : "vertex", how == 1 ? "made at the draw" : "waited for (a worker was making it)", (double)dt * 1000.0 / (double)fq.QuadPart, p->serial, (unsigned long long)v->hb,
                   key[0], key[1], key[2], key[3], v->key[0], v->key[1], v->key[2], v->key[3], fo, v->from_rec ? "from its record on disk" : "translated now (no record)");
            EnterCriticalSection(&g_sh_lock);
            printf("nvn_vk_hold:     %s; jobs waiting now: %lld foreground, %lld background (container scan)\n", ho_text(v->hb), (long long)(g_jq_queued - g_jq_done), (long long)(g_bq_queued - g_bq_done));
            LeaveCriticalSection(&g_sh_lock); } }
    st->vk = v; memcpy(v->local, st->local, sizeof v->local);
    return v->ok ? v : NULL;
}

/* ---------------------------------------------------------------- pipelines */
typedef struct { uint32_t vs, fs, chanmask, sample_mask; int32_t cfmt[8], dfmt; uint8_t topo_class, ncolor, samples, poly, a2c, dclamp, logic, sshade, blend_on, pad[3]; uint8_t blend[8][8]; PKEY_VI } PKey;
typedef struct Pipe { PKey k; VkPipeline p, p_fast; /* p_fast: the pipeline linked at once, after the optimised one took its place */ VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; VStage *vs, *fs; volatile LONG state; uint8_t recorded, predicted, used, libbed, lib_how; struct Pipe *next; } Pipe;      /* libbed: linked from libraries (layout and set layouts belong to the pair library); lib_how: 0 the pair library was there, 1 compiled by this build, 2 waited for */      /* state: 0 queued for a worker, 1 being made, 2 made */
static Pipe *g_pipes[1024];
static void pkey_targets(PKey *k);
static int pkey_targets_of(PKey *k, int ncol, const TargetRef *col, const TargetRef *depth);
static VkBlendFactor blend_factor(unsigned f) {
    static const uint8_t m[9] = {VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                 VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    if (f >= 3 && f <= 0xB) return (VkBlendFactor)m[f - 3];
    switch (f) { case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR; case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA;
                 case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA; case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR; case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
                 case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA; }
    return VK_BLEND_FACTOR_ONE;
}
static VkBlendOp blend_op(unsigned e) { static const uint8_t m[6] = {VK_BLEND_OP_ADD, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX}; return (VkBlendOp)m[e < 6 ? e : 1]; }
static void layout_bindings(VStage *v, int s, VkDescriptorSetLayoutBinding *b0, int *n0, VkDescriptorSetLayoutBinding *b1, int *n1) {
    VkFlags sf = s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
    for (unsigned n = 1; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {40u * (unsigned)s + 20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[(*n0)++] = b; }
    for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL}; b1[(*n1)++] = b; }
}
static Pipe *pipe_find(const PKey *k, uint64_t h) { for (Pipe *p = g_pipes[h & 1023]; p; p = p->next) if (!memcmp(&p->k, k, sizeof *k)) return p; return NULL; }      /* g_sh_lock held */
static Pipe *pipe_new(const PKey *k, VStage *vs, VStage *fs, uint64_t h, int state) {                                                                                 /* g_sh_lock held */
    Pipe *p = calloc(1, sizeof *p);
    p->k = *k; p->vs = vs; p->fs = fs; p->state = state;
    p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
    return p;
}
/* ---- Session 9: graphics pipeline libraries (VK_EXT_graphics_pipeline_library; D.has_gpl, SSBU_VK_GPL=1).
 * What a first run did not know at loading is (a) the pass formats a stage pair will be drawn with and (b) the topology
 * class: everything else of the key is dynamic state since session 4. With libraries the expensive part, the two shader
 * stages, is compiled WITHOUT those: one "pair library" per (vertex stage, fragment stage, samples, sample shading) holds
 * the pre-rasterization and the fragment shader state; it is made by the workers when the game hands the program over
 * (job_prog). The formats live in a "fragment output" library per pass signature and the topology class in a "vertex input"
 * library (three of them): both hold no shader and are made in microseconds. A draw LINKS the three (no compilation).
 * Not covered: a pair nobody announced (a fragment stage bound with the vertex stage of another program): its pair
 * library is compiled at the draw as the whole pipeline was before (counted: "pair library compiled at a draw"). */
typedef struct PairLib { VStage *vs, *fs; uint8_t samples, sshade; volatile LONG state; VkPipeline lib; VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; struct PairLib *next; } PairLib;
static PairLib *g_plib[1024];
static CRITICAL_SECTION g_gpl_lock;
static volatile LONG g_in_fight;
/* Session 13: PIPELINE CREATION FEEDBACK (core in Vulkan 1.3): the driver's own answer to "was this compiled now or taken
 * from the pipeline cache". Chained to every stage library and every link. [0] stage libraries, [1] links; hit = the
 * driver set APPLICATION_PIPELINE_CACHE_HIT, miss = valid feedback without it, none = the driver gave no valid feedback.
 * The same split for creations on the render thread in a fight frame ("f"), with the time of the misses. Line
 * "nvn_vk_feedback:" at exit; with SSBU_VK_HOLDLOG=1 one line "nvn_vk_first:" per creation at a draw in a fight frame. */
typedef struct { uint32_t flags; uint64_t duration; } FbRec;
typedef struct { VkStructureType sType; const void *pNext; FbRec *pPipelineCreationFeedback; uint32_t pipelineStageCreationFeedbackCount; FbRec *pPipelineStageCreationFeedbacks; } FbInfo;
static volatile LONG64 g_pc_dirty, g_pc_dirty_saved;      /* session 14: creations the driver's cache did not answer (stage libraries by feedback; whole pipelines by their time, over 3 ms), and the count at the last read-out */
static void pc_dirty_time(LONG64 t0) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); if ((tk() - t0) * 1000 > 3 * fq.QuadPart) InterlockedIncrement64(&g_pc_dirty); }
static LONG64 g_fb_hit[2], g_fb_miss[2], g_fb_none[2], g_fb_fhit[2], g_fb_fmiss[2], g_fb_fmiss_tk[2], g_fb_fhit_tk[2], g_fb_miss_ns[2];
static DWORD g_render_tid;
static void fb_note(int kind, const FbRec *f, LONG64 ticks) {
    int valid = f->flags & 1, hit = valid && (f->flags & 2);
    InterlockedIncrement64(!valid ? &g_fb_none[kind] : hit ? &g_fb_hit[kind] : &g_fb_miss[kind]);
    if (kind == 0 && !hit) InterlockedIncrement64(&g_pc_dirty);      /* session 14: a stage library the pipeline cache did not have: the file on disk is now older than the cache */
    if (valid && !hit) InterlockedAdd64(&g_fb_miss_ns[kind], (LONG64)f->duration);
    if (GetCurrentThreadId() == g_render_tid && g_in_fight) {
        static int nl;
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        if (hit) { g_fb_fhit[kind]++; g_fb_fhit_tk[kind] += ticks; } else { g_fb_fmiss[kind]++; g_fb_fmiss_tk[kind] += ticks; }
        if (holdlog_on() && nl++ < 2000) printf("nvn_vk_first: frame %lld (fight): %s at a draw: %s, %.3f ms (the driver's own time %.3f ms)\n", (long long)nvn_front_frame_next(), kind ? "pipeline LINKED" : "stage LIBRARY made",
                                                !valid ? "no feedback from the driver" : hit ? "pipeline cache HIT" : "COMPILED NOW (not in the pipeline cache)", (double)ticks * 1000.0 / (double)fq.QuadPart, (double)f->duration / 1e6);
    }
}
static struct { PKey k; VkPipeline lib; } g_olib[128];
static int g_nolib;
static VkPipeline g_vilib[3];
static LONG64 g_gpl_made, g_gpl_made_tk, g_gpl_draw, g_gpl_draw_tk, g_gpl_draw_max, g_gpl_wait, g_gpl_wait_tk, g_gpl_link, g_gpl_link_tk, g_gpl_link_max, g_gpl_dlink, g_gpl_dlink_tk, g_gpl_dlink_max, g_gpl_dlink_fight;
static DWORD g_render_tid;
static uint32_t pipe_dyn_list(VkDynamicState *dyn) {
    static const VkDynamicState base[20] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    uint32_t ndyn = 20;
    memcpy(dyn, base, sizeof base);
    if (D.has_eds3) {
        dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
        dyn[ndyn++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
        if (D.f2.features.depthClamp) dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    }
    return ndyn;
}
static void gpl_ms(VkPipelineMultisampleStateCreateInfo *ms, VkSampleMask *smask, int samples, int sshade) {      /* the same in the pair library and in the fragment output library */
    memset(ms, 0, sizeof *ms);
    ms->sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    *smask = 0xffffffffu;
    ms->rasterizationSamples = (VkSampleCountFlagBits)(samples ? samples : 1); ms->pSampleMask = smask;
    if (sshade == 1 && D.f2.features.sampleRateShading) { ms->sampleShadingEnable = VK_TRUE; ms->minSampleShading = 1.0f; }
}
/* Any thread, no lock held: the pair library; *how: 0 it was there, 1 compiled by this call, 2 waited for the thread compiling it. */
static PairLib *pairlib_ensure(VStage *vs, VStage *fs, int samples, int sshade, int *how) {
    unsigned h = (vs->id * 2654435761u ^ (fs ? fs->id * 40503u : 0) ^ (unsigned)(samples * 131 + sshade)) & 1023;
    PairLib *l;
    int mine = 0;
    LONG64 t0 = tk();
    *how = 0;
    EnterCriticalSection(&g_sh_lock);
    for (l = g_plib[h]; l; l = l->next) if (l->vs == vs && l->fs == fs && l->samples == samples && l->sshade == sshade) break;
    if (!l) { l = calloc(1, sizeof *l); l->vs = vs; l->fs = fs; l->samples = (uint8_t)samples; l->sshade = (uint8_t)sshade; l->state = 1; l->next = g_plib[h]; g_plib[h] = l; mine = 1; }
    else if (l->state == 1) { *how = 2; while (l->state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); }
    LeaveCriticalSection(&g_sh_lock);
    if (!mine) { if (*how == 2) { InterlockedIncrement64(&g_gpl_wait); InterlockedAdd64(&g_gpl_wait_tk, tk() - t0); } return l; }
    *how = 1;
    VkDescriptorSetLayoutBinding b0[80], b1[140];
    layout_bindings(vs, 0, b0, &l->nb[0], b1, &l->nb[1]);
    if (fs) layout_bindings(fs, 1, b0, &l->nb[0], b1, &l->nb[1]);
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)l->nb[i]; li.pBindings = i ? b1 : b0;
        if (i == 0 && D.has_push && l->nb[0] <= 32) { li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; l->push0 = 1; }
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &l->dl[i]));
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2; pl.pSetLayouts = l->dl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &l->layout));
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs->mod; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs ? fs->mod : NULL; st[1].pName = "main";
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms; VkSampleMask smask;
    gpl_ms(&ms, &smask, samples, sshade);
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkDynamicState dyn[32];
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = pipe_dyn_list(dyn); dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    gl.pNext = &ri; gl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT; gi.stageCount = fs ? 2 : 1; gi.pStages = st; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds; gi.pDynamicState = &dy; gi.layout = l->layout;
    VkResult r = pw_graphics(g_pcache, &gi, &l->lib);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines, pair library (vertex stage %u, fragment stage %u): %s", vs->id, fs ? fs->id : 0, vk_result_name(r));
    LONG64 dt = tk() - t0;
    EnterCriticalSection(&g_sh_lock);
    l->state = 2;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
    if (GetCurrentThreadId() == g_render_tid) {    /* compiled at a draw: the hold the libraries are there to remove */
        g_gpl_draw++; g_gpl_draw_tk += dt; if (dt > g_gpl_draw_max) g_gpl_draw_max = dt;
        if (holdlog_on()) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); printf("nvn_vk_hold: frame %lld (%s): PAIR LIBRARY compiled at the draw, %.2f ms: vertex code %016llx (forced outputs %x), fragment code %016llx (%s)\n", (long long)nvn_front_frame_next(), g_in_fight ? "fight" : "no fight",
                                   (double)dt * 1000.0 / (double)fq.QuadPart, (unsigned long long)vs->hb, vs->fo, (unsigned long long)(fs ? fs->hb : 0), fs && fs->fragonly ? "a fragment stage of a program without a vertex stage" : "a program with both stages, or a pair the look-ahead met"); }
    } else { InterlockedIncrement64(&g_gpl_made); InterlockedAdd64(&g_gpl_made_tk, dt); }
    return l;
}
static VkPipeline gpl_vilib(int topo_class) {
    EnterCriticalSection(&g_gpl_lock);
    if (!g_vilib[topo_class]) {
        VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkDynamicState dyn[32];
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = pipe_dyn_list(dyn); dy.pDynamicStates = dyn;
        VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
        gl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT; gi.pInputAssemblyState = &ia; gi.pDynamicState = &dy;
        VKC(pw_graphics(NULL, &gi, &g_vilib[topo_class]));
    }
    VkPipeline p = g_vilib[topo_class];
    LeaveCriticalSection(&g_gpl_lock);
    return p;
}
static VkPipeline gpl_olib(const PKey *k0) {
    PKey k = *k0;
    k.vs = k.fs = 0; k.topo_class = 0;
    EnterCriticalSection(&g_gpl_lock);
    int i;
    for (i = 0; i < g_nolib; i++) if (!memcmp(&g_olib[i].k, &k, sizeof k)) break;
    if (i == g_nolib) {
        if (g_nolib == 128) vkd_fail("more than 128 pass signatures (fragment output libraries)");
        VkPipelineMultisampleStateCreateInfo ms; VkSampleMask smask;
        gpl_ms(&ms, &smask, k.samples, k.sshade);
        VkPipelineColorBlendAttachmentState ba[8];
        memset(ba, 0, sizeof ba);
        for (int c = 0; c < k.ncolor; c++) { ba[c].colorWriteMask = 15; ba[c].srcColorBlendFactor = ba[c].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba[c].dstColorBlendFactor = ba[c].dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; }      /* dynamic (extended dynamic state 3) */
        VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = k.ncolor; cb.pAttachments = ba;
        if (k.logic != 3 && D.f2.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k.logic & 15); }
        VkDynamicState dyn[32];
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dy.dynamicStateCount = pipe_dyn_list(dyn); dy.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        VkFormat cf[8];
        for (int c = 0; c < 8; c++) cf[c] = (VkFormat)k.cfmt[c];
        ri.colorAttachmentCount = k.ncolor; ri.pColorAttachmentFormats = cf;
        if (fmt_aspect((VkFormat)k.dfmt) & VK_IMAGE_ASPECT_DEPTH_BIT) ri.depthAttachmentFormat = (VkFormat)k.dfmt;
        if (k.dfmt && (fmt_aspect((VkFormat)k.dfmt) & VK_IMAGE_ASPECT_STENCIL_BIT)) ri.stencilAttachmentFormat = (VkFormat)k.dfmt;
        VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
        gl.pNext = &ri; gl.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT; gi.pMultisampleState = &ms; gi.pColorBlendState = &cb; gi.pDynamicState = &dy;
        VKC(pw_graphics(NULL, &gi, &g_olib[i].lib));
        g_olib[i].k = k; g_nolib++;
    }
    VkPipeline p = g_olib[i].lib;
    LeaveCriticalSection(&g_gpl_lock);
    return p;
}
/* ---- SSBU_VK_GPL=2: one library per STAGE instead of per pair, so that no pair has to be known before its draw (a fragment
 * stage of a program without a vertex stage is drawn with the vertex stage of another program: 24 of the 30 libraries still
 * compiled at a draw in run s9gn2). Two libraries of one pipeline must agree on every descriptor set layout both use, and our
 * sets hold the bindings of both stages; so every stage library is made with ONE layout that declares every binding a vertex
 * or a fragment stage can have (set 0: 66 buffers, set 1: 128 samplers; a declared binding a shader does not use needs no
 * descriptor). The price: set 0 is too large for push descriptors (32), so a draw allocates and writes a set instead.
 * A draw links: vertex input + vertex stage library + fragment stage library + fragment output. A vertex stage drawn with no
 * fragment stage has a second library holding the (empty) fragment shader state as well. Sample shading (never met:
 * 0 of 1,380 recorded keys) takes the pair library path. */
static VkDescriptorSetLayout g_udl[2];
static VkPipelineLayout g_ulayout;
static int gpl_sep(void) { static int m = -1; if (m < 0) m = D.has_gpl && vk_gpl_mode() >= 2; return m; }
static void gpl_universal(void) {
    EnterCriticalSection(&g_gpl_lock);
    if (!g_ulayout) {
        static VkDescriptorSetLayoutBinding b0[66], b1[128];
        int n0 = 0, n1 = 0;
        for (unsigned s = 0; s < 2; s++) {
            VkFlags sf = s ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            for (unsigned n = 1; n < 18; n++) { VkDescriptorSetLayoutBinding b = {40u * s + n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[n0++] = b; }
            for (unsigned k = 0; k < 16; k++) { VkDescriptorSetLayoutBinding b = {40u * s + 20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, sf, NULL}; b0[n0++] = b; }
            for (unsigned u = 0; u < 32; u++) { VkDescriptorSetLayoutBinding b = {40u * s + u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, sf, NULL}; b1[n1++] = b; b.binding += 240; b1[n1++] = b; }
        }
        for (int i = 0; i < 2; i++) {
            VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)(i ? n1 : n0); li.pBindings = i ? b1 : b0;
            VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &g_udl[i]));
        }
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2; pl.pSetLayouts = g_udl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
        VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &g_ulayout));
    }
    LeaveCriticalSection(&g_gpl_lock);
}
static int gpl_opt_on(void);
static int stage_fits_universal(const VStage *v) { for (int i = 0; i < v->nsmp; i++) { unsigned b = v->smp[i].binding; if (b >= 240) b -= 240; if (b >= 80 || b % 40u >= 32) return 0; } return 1; }      /* texture units 0..31 only */
/* Any thread, no lock held: the library of one stage (alone: a vertex stage that is drawn without a fragment stage). */
static VkPipeline stagelib_ensure(VStage *v, int alone, int *how) {
    VkPipeline *lib = alone ? &v->lib2 : &v->lib;
    volatile LONG *state = alone ? &v->lib2_state : &v->lib_state;
    int mine = 0;
    LONG64 t0 = tk();
    *how = 0;
    gpl_universal();
    EnterCriticalSection(&g_sh_lock);
    if (*state == 0) { *state = 1; mine = 1; }
    else if (*state == 1) { *how = 2; workers_boost(); while (*state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); }
    LeaveCriticalSection(&g_sh_lock);
    if (!mine) { if (*how == 2) { InterlockedIncrement64(&g_gpl_wait); InterlockedAdd64(&g_gpl_wait_tk, tk() - t0); } return *lib; }
    *how = 1;
    int isfs = v->stage == 1;
    VkPipelineShaderStageCreateInfo st = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage = isfs ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT; st.module = v->mod; st.pName = "main";
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkDynamicState dyn[32];
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = pipe_dyn_list(dyn); dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkGraphicsPipelineLibraryCreateInfoEXT gl = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    gl.pNext = &ri; gl.flags = isfs ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT : VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT | (alone ? VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT : 0);
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    /* session 10: the link-time-optimisation information is kept only when something will ask for it (SSBU_VK_GPL_OPT=1) */
    gi.pNext = &gl; gi.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | (gpl_opt_on() ? VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT : 0); gi.stageCount = 1; gi.pStages = &st; gi.pDynamicState = &dy; gi.layout = g_ulayout;
    if (!isfs) { gi.pViewportState = &vp; gi.pRasterizationState = &rs; }
    if (isfs || alone) gi.pDepthStencilState = &ds;
    FbRec fb_all = {0, 0}, fb_st = {0, 0};
    FbInfo fbi = {(VkStructureType)1000192000, NULL, &fb_all, 1, &fb_st};
    ri.pNext = &fbi;
    VkResult r = pw_graphics(g_pcache, &gi, lib);
    /* (2026-10-21, tester: AMD RX 6750 XT, VK_ERROR_INITIALIZATION_FAILED during the first-run preparation) tried again, then
     * without the pipeline cache; if the driver still refuses, the next start uses whole pipelines (no stage libraries):
     * the marker file ../cache/vk/no_stage_libraries.txt (vk_gpl_mode), deleted by hand to try the libraries again */
    for (int t = 0; r != VK_SUCCESS && t < 3; t++) {
        printf("nvn_vk: the driver refused the stage library (%s stage %u): %s; trying again%s\n", isfs ? "fragment" : "vertex", v->id, vk_result_name(r), t == 2 ? " without the pipeline cache" : "");
        fflush(stdout);
        Sleep(50 << t);
        r = pw_graphics(t == 2 ? VK_NULL_HANDLE : g_pcache, &gi, lib);
    }
    if (r != VK_SUCCESS) {
        FILE *mk = fopen("../cache/vk/no_stage_libraries.txt", "w");
        if (mk) { fprintf(mk, "The graphics driver refused a stage library (%s): the game uses whole pipelines from the next start. Delete this file to try the libraries again.\n", vk_result_name(r)); fclose(mk); }
        vkd_fail("vkCreateGraphicsPipelines, stage library (%s stage %u): %s. START THE GAME AGAIN: from now on it uses whole pipelines, which this driver accepts", isfs ? "fragment" : "vertex", v->id, vk_result_name(r));
    }
    LONG64 dt = tk() - t0;
    fb_note(0, &fb_all, dt);
    EnterCriticalSection(&g_sh_lock);
    *state = 2;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
    if (GetCurrentThreadId() == g_render_tid) {
        g_gpl_draw++; g_gpl_draw_tk += dt; if (dt > g_gpl_draw_max) g_gpl_draw_max = dt;
        if (holdlog_on()) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); printf("nvn_vk_hold: frame %lld (%s): STAGE LIBRARY compiled at the draw, %.2f ms: %s code %016llx (key %x %x %x %x, forced outputs %x)%s\n", (long long)nvn_front_frame_next(), g_in_fight ? "fight" : "no fight",
                                   (double)dt * 1000.0 / (double)fq.QuadPart, isfs ? "fragment" : "vertex", (unsigned long long)v->hb, v->key[0], v->key[1], v->key[2], v->key[3], v->fo, alone ? " (a vertex stage drawn without a fragment stage)" : ""); }
    } else { InterlockedIncrement64(&g_gpl_made); InterlockedAdd64(&g_gpl_made_tk, dt); }
    return *lib;
}
static void gpl_destroy(void) {
    for (int i = 0; i < 4096; i++) for (VStage *v = g_vstage[i]; v; v = v->next) { if (v->lib) { vkDestroyPipeline(V.device, v->lib, NULL); v->lib = NULL; } if (v->lib2) { vkDestroyPipeline(V.device, v->lib2, NULL); v->lib2 = NULL; } }
    if (g_ulayout) { vkDestroyPipelineLayout(V.device, g_ulayout, NULL); vkDestroyDescriptorSetLayout(V.device, g_udl[0], NULL); vkDestroyDescriptorSetLayout(V.device, g_udl[1], NULL); g_ulayout = NULL; }
    for (int i = 0; i < 1024; i++) for (PairLib *l = g_plib[i], *n; l; l = n) { n = l->next; if (l->lib) vkDestroyPipeline(V.device, l->lib, NULL); if (l->layout) vkDestroyPipelineLayout(V.device, l->layout, NULL); for (int k = 0; k < 2; k++) if (l->dl[k]) vkDestroyDescriptorSetLayout(V.device, l->dl[k], NULL); free(l); g_plib[i] = NULL; }
    for (int i = 0; i < g_nolib; i++) vkDestroyPipeline(V.device, g_olib[i].lib, NULL);
    g_nolib = 0;
    for (int i = 0; i < 3; i++) if (g_vilib[i]) { vkDestroyPipeline(V.device, g_vilib[i], NULL); g_vilib[i] = NULL; }
}
/* A pipeline linked at once from libraries is not optimised across its stages (run s9g2n1, NVIDIA: 3.3 ms of GPU time per
 * fight frame against 1.6 ms). So every pipeline linked from stage libraries is linked AGAIN by a worker with
 * VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION (a full compilation, background queue) and the handle is exchanged; the first
 * one stays alive until shutdown (command buffers in flight use it). SSBU_VK_GPL_OPT=0 turns this off. */
static void gpl_opt_push(struct Pipe *p);          /* g_sh_lock held (defined after the job queue) */
static LONG64 g_gpl_opt_q, g_gpl_opt_n, g_gpl_opt_tk;
static int g_m4_quit;
/* MEASURED (runs s9g3n1 / s9g3i1 against s9g2n1 / s9g2i1, first runs): OFF by default. It cost 195 s (NVIDIA) and 304 s
 * (Intel) of worker time in one run, the GPU time per fight frame did not change (NVIDIA 3.23 against 3.34 ms, Intel 8.60
 * against 8.80) and the holds went UP (16 / 744 ms against 13 / 732 ms; Intel 53 / 2,055 ms against 37 / 1,199 ms). A second
 * run has the normal GPU time without it (s9g2n2: 1.59 ms), so the higher first-run number is not the missing optimisation. */
static int gpl_opt_on(void) { static int on = -1; if (on < 0) on = getenv("SSBU_VK_GPL_OPT") && atoi(getenv("SSBU_VK_GPL_OPT")); return on; }
static void gpl_optimise(Pipe *p) {                /* a worker */
    const PKey *k = &p->k;
    LONG64 t0 = tk();
    int n = 0;
    VkPipeline libs[4], opt = NULL;
    libs[n++] = gpl_vilib(k->topo_class < 3 ? k->topo_class : 2);
    libs[n++] = p->fs ? p->vs->lib : p->vs->lib2;
    if (p->fs) libs[n++] = p->fs->lib;
    libs[n++] = gpl_olib(k);
    for (int i = 0; i < n; i++) if (!libs[i]) return;
    VkPipelineLibraryCreateInfoKHR pli = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    pli.libraryCount = (uint32_t)n; pli.pLibraries = libs;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &pli; gi.layout = g_ulayout; gi.flags = VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
    LONG64 t_dirty = tk();
    if (pw_graphics(g_pcache, &gi, &opt) != VK_SUCCESS || !opt) return;
    pc_dirty_time(t_dirty);
    p->p_fast = InterlockedExchangePointer((void *volatile *)&p->p, opt);
    InterlockedIncrement64(&g_gpl_opt_n); InterlockedAdd64(&g_gpl_opt_tk, tk() - t0);
}
/* Makes the pipeline of p->k (any thread, no lock: the device calls are thread-safe, the pipeline cache is synchronised by
 * the driver), then marks it made. Returns the milliseconds it took. */
static double pipe_build(Pipe *p) {
    const PKey *k = &p->k;
    VStage *vs = p->vs, *fs = p->fs;
    LONG64 t0 = tk();
    if (D.has_gpl && gpl_sep() && !k->sshade && stage_fits_universal(vs) && (!fs || stage_fits_universal(fs))) {      /* session 9, SSBU_VK_GPL=2: linked from one library per stage */
        int how, how2 = 0, n = 0;
        VkPipeline libs[4];
        libs[n++] = gpl_vilib(k->topo_class < 3 ? k->topo_class : 2);
        libs[n++] = stagelib_ensure(vs, !fs, &how);
        if (fs) libs[n++] = stagelib_ensure(fs, 0, &how2);
        libs[n++] = gpl_olib(k);
        if (how2 == 1 || how == 1) how = 1; else if (how2 == 2 || how == 2) how = 2;
        LONG64 t1 = tk();
        VkPipelineLibraryCreateInfoKHR pli = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
        pli.libraryCount = (uint32_t)n; pli.pLibraries = libs;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &pli; gi.layout = g_ulayout;
        FbRec fb_all = {0, 0};
        FbInfo fbi = {(VkStructureType)1000192000, NULL, &fb_all, 0, NULL};
        pli.pNext = &fbi;
        VkResult r = pw_graphics(g_pcache, &gi, &p->p);
        if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines, link of stage libraries (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
        LONG64 dt = tk() - t1;
        fb_note(1, &fb_all, dt);
        p->layout = g_ulayout; p->dl[0] = g_udl[0]; p->dl[1] = g_udl[1]; p->nb[0] = 66; p->nb[1] = 128; p->push0 = 0; p->libbed = 1; p->lib_how = (uint8_t)how;
        InterlockedIncrement64(&R.pipelines);
        if (GetCurrentThreadId() == g_render_tid) { g_gpl_dlink++; g_gpl_dlink_tk += dt; if (dt > g_gpl_dlink_max) g_gpl_dlink_max = dt; if (g_in_fight) g_gpl_dlink_fight++; }
        else { InterlockedIncrement64(&g_gpl_link); InterlockedAdd64(&g_gpl_link_tk, dt); }
        EnterCriticalSection(&g_sh_lock);
        p->state = 2;
        if (gpl_opt_on() && !g_m4_quit) gpl_opt_push(p);      /* the same pipeline again, optimised across its stages, on a worker */
        WakeAllConditionVariable(&g_sh_cv);
        LeaveCriticalSection(&g_sh_lock);
        { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); return (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart; }
    }
    if (D.has_gpl) {                               /* session 9: linked from libraries */
        int how;
        PairLib *l = pairlib_ensure(vs, fs, k->samples, k->sshade, &how);
        VkPipeline libs[3];
        libs[0] = gpl_vilib(k->topo_class < 3 ? k->topo_class : 2); libs[1] = l->lib; libs[2] = gpl_olib(k);
        LONG64 t1 = tk();
        VkPipelineLibraryCreateInfoKHR pli = {VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
        pli.libraryCount = 3; pli.pLibraries = libs;
        VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.pNext = &pli; gi.layout = l->layout;
        VkResult r = pw_graphics(g_pcache, &gi, &p->p);
        if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines, link of libraries (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
        LONG64 dt = tk() - t1;
        p->layout = l->layout; p->dl[0] = l->dl[0]; p->dl[1] = l->dl[1]; p->nb[0] = l->nb[0]; p->nb[1] = l->nb[1]; p->push0 = l->push0; p->libbed = 1; p->lib_how = (uint8_t)how;
        InterlockedIncrement64(&R.pipelines);
        if (GetCurrentThreadId() == g_render_tid) { g_gpl_dlink++; g_gpl_dlink_tk += dt; if (dt > g_gpl_dlink_max) g_gpl_dlink_max = dt; if (g_in_fight) g_gpl_dlink_fight++; }
        else { InterlockedIncrement64(&g_gpl_link); InterlockedAdd64(&g_gpl_link_tk, dt); }
        EnterCriticalSection(&g_sh_lock);
        p->state = 2;
        WakeAllConditionVariable(&g_sh_cv);
        LeaveCriticalSection(&g_sh_lock);
        { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); return (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart; }
    }
    VkDescriptorSetLayoutBinding b0[80], b1[140];
    layout_bindings(vs, 0, b0, &p->nb[0], b1, &p->nb[1]);
    if (fs) layout_bindings(fs, 1, b0, &p->nb[0], b1, &p->nb[1]);
    for (int i = 0; i < 2; i++) {
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = (uint32_t)p->nb[i]; li.pBindings = i ? b1 : b0;
        if (i == 0 && D.has_push && p->nb[0] <= 32) { li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; p->push0 = 1; }      /* 32: the smallest maxPushDescriptors a device may have */
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &p->dl[i]));
    }
    VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 2; pl.pSetLayouts = p->dl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pc;
    VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &p->layout));

    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs->mod; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs ? fs->mod : NULL; st[1].pName = "main";
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = k->topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : k->topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = D.has_eds3 ? VK_POLYGON_MODE_FILL : k->poly == 0 ? VK_POLYGON_MODE_POINT : k->poly == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; rs.depthClampEnable = k->dclamp; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkSampleMask smask = D.has_eds3 ? 0xffffffffu : k->sample_mask;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1); ms.alphaToCoverageEnable = k->a2c; ms.pSampleMask = &smask;
    if (k->sshade == 1 && D.f2.features.sampleRateShading) { ms.sampleShadingEnable = VK_TRUE; ms.minSampleShading = 1.0f; }
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba[8];
    memset(ba, 0, sizeof ba);
    for (int i = 0; i < k->ncolor; i++) {
        const uint8_t *b = k->blend[i];
        ba[i].blendEnable = k->blend_on >> i & 1; ba[i].colorWriteMask = D.has_eds3 ? 15 : (k->chanmask >> (i * 4)) & 15;
        ba[i].srcColorBlendFactor = blend_factor(b[0]); ba[i].dstColorBlendFactor = blend_factor(b[1]); ba[i].srcAlphaBlendFactor = blend_factor(b[2]); ba[i].dstAlphaBlendFactor = blend_factor(b[3]);
        ba[i].colorBlendOp = blend_op(b[4]); ba[i].alphaBlendOp = blend_op(b[5]);
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k->ncolor; cb.pAttachments = ba;
    if (k->logic != 3 && D.f2.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic & 15); }
    VkDynamicState dyn[32] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    uint32_t ndyn = 20;
    if (D.has_eds3) {
        dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
        dyn[ndyn++] = VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT; dyn[ndyn++] = VK_DYNAMIC_STATE_SAMPLE_MASK_EXT;
        if (D.f2.features.depthClamp) dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT;
    }
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = ndyn; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat cf[8];
    for (int i = 0; i < 8; i++) cf[i] = (VkFormat)k->cfmt[i];
    ri.colorAttachmentCount = k->ncolor; ri.pColorAttachmentFormats = cf;
    if (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_DEPTH_BIT) ri.depthAttachmentFormat = (VkFormat)k->dfmt;
    if (k->dfmt && (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_STENCIL_BIT)) ri.stencilAttachmentFormat = (VkFormat)k->dfmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = fs ? 2 : 1; gi.pStages = st; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = p->layout; NVK_VI_PIPE(k, dyn, &gi)
    VkResult r = pw_graphics(g_pcache, &gi, &p->p);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
    InterlockedIncrement64(&R.pipelines);
    EnterCriticalSection(&g_sh_lock);
    p->state = 2;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
    { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); return (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart; }
}
/* ..\cache\vk\pipes.bin: every pipeline key a draw on this PC ever USED, with its stages named by their variant hashes.
 * Read at start by the workers. A predicted pipeline is written here only when a draw uses it. */
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRec;
#define PIPEREC_MAGIC (0x31455049u + MX_VK_VERSION)
static void piperec_append(const PKey *k, VStage *vs, VStage *fs, int compute) {
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.magic = PIPEREC_MAGIC; r.compute = (uint32_t)compute; r.vs_h = vs->h; r.fs_h = fs ? fs->h : 0;
    if (k) { r.k = *k; r.k.vs = r.k.fs = 0; }
    FILE *fp = fopen(vkc_file(VK_PIPES_BIN), "ab");
    if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
}

/* ---- prediction: pass signatures seen so far, and the stage pairs the game handed over */
typedef struct { PKey k; int uses; } Sig;          /* k with vs = fs = 0, triangles, no sample shading */
typedef struct PairRec { VStage *vs, *fs; uint8_t drawn, npred, sshade; int16_t cont; uint64_t sigs, used; struct PairRec *next, *all; } PairRec;      /* cont: 1 + index of the shader container the pair was found in (session 6), used: the pass signatures draws used it with */
static Sig g_sig[64];
static int g_nsig, g_predict;
static PairRec *g_pair_tab[1024], *g_pair_all;
static PairRec *pair_get(VStage *vs, VStage *fs) {                 /* g_sh_lock held */
    unsigned h = (vs->id * 2654435761u ^ (fs ? fs->id * 40503u : 0)) & 1023;
    PairRec *r;
    for (r = g_pair_tab[h]; r; r = r->next) if (r->vs == vs && r->fs == fs) return r;
    r = calloc(1, sizeof *r);
    r->vs = vs; r->fs = fs; r->next = g_pair_tab[h]; g_pair_tab[h] = r; r->all = g_pair_all; g_pair_all = r;
    return r;
}
static int sig_note(const PKey *k, int *idx) {     /* g_sh_lock held: the index of the signature when it is NEW, else -1; *idx: its index either way (-1: table full) */
    PKey s = *k;
    s.vs = s.fs = 0; s.topo_class = 2; s.sshade = 0;
    for (int i = 0; i < g_nsig; i++) if (!memcmp(&g_sig[i].k, &s, sizeof s)) { g_sig[i].uses++; *idx = i; return -1; }
    *idx = -1;
    if (g_nsig == 64) return -1;
    g_sig[g_nsig].k = s; g_sig[g_nsig].uses = 1;
    *idx = g_nsig;
    return g_nsig++;
}
static void cont_learn(PairRec *pr);
static void gpl_shared_vs(VStage *vs);
/* ---- the job queue of the workers */
typedef struct Job { struct Job *next; int kind, sshade, cont, has[3], prep; /* prep (session 10): a variation of the preparation (queue g_pq) */ Pipe *pipe; VStage *lvs, *lfs; /* kind 5 (session 9): the pair library of these two stages */ PKey k; uint32_t key0[4], key1[4]; SSrc src[3]; uint64_t cbase, csize, cobase; /* kind 4 (session 7): a copy of a container to walk, its size, the address it was copied from */ uint8_t data[1]; } Job;      /* kind: 0 a program handed over, 1 one pipeline, 2 the stages and the pipeline of a draw the look-ahead met */
static Job *g_jq_head, *g_jq_tail, *g_bq_head, *g_bq_tail;      /* g_bq: the background queue (container scan, session 6): taken only when g_jq is empty */
static LONG64 g_jq_queued, g_jq_done, g_bq_queued, g_bq_done;
/* Session 10, the PREPARATION queue: every variation of every container of the game's data (prep_main, end of this file).
 * A worker takes from it only when the two queues above are empty, so whatever the game itself hands over or announces
 * (a fight's containers, at its loading screen) always goes first; nobody waits for this queue (not the barrier, not a
 * caller of nvnProgramSetShaders). */
static Job *g_pq_head, *g_pq_tail;
static LONG64 g_pq_queued, g_pq_done, g_pq_running;
static int g_prep_state, g_prep_fight_cap = 0 /* run s10n2: 4 workers at the lowest priority compiling libraries during the fight = 35 frames per second (the render thread in "present": the driver), so the preparation PAUSES near a fight */, g_prep_cap = 8, g_prep_keep = 0;     /* state: 0 not started, 1 running, 2 finished, 3 no container source, 4 done by an earlier run; caps: workers on this queue at a time (in a fight / otherwise: the others stay free, at normal priority, for what the game hands over; run s10n1 had all 16 on it at low priority on a busy PC and the game's own programs waited 101 s); keep: the first n containers are made INTO MEMORY (stage + library stay), the rest into the cache only */
static HANDLE g_prep_thread;
static LONG64 g_pk_queued, g_pk_done, g_bar_keep_n, g_bar_keep_tk;      /* the jobs of the containers made into memory (prep == 1) */
static volatile LONG g_prep_urgent;                /* a loading screen waits for them: every worker, normal priority */
static LONG64 g_prep_t0, g_prep_t1, g_prep_vars, g_prep_fight_frames, g_boosts;
static int g_prep_nfiles, g_prep_read, g_prep_bad;
static void prep_start(void);
static void prep_line(const char *what);
static void job_push_prep(Job *j) {                /* g_sh_lock held; j->prep set by the caller: 1 into memory, 2 into the cache only */
    j->next = NULL;
    if (j->prep == 1) g_pk_queued++;
    if (g_pq_tail) g_pq_tail->next = j; else g_pq_head = j;
    g_pq_tail = j; g_pq_queued++;
    WakeConditionVariable(&g_jq_cv);
}
static void job_push_bg(Job *j) {                  /* g_sh_lock held */
    j->next = NULL;
    if (g_bq_tail) g_bq_tail->next = j; else g_bq_head = j;
    g_bq_tail = j; g_bq_queued++;
    WakeConditionVariable(&g_jq_cv);
}
static void job_push_front(Job *j);
static void job_copy_src(Job *j, uint8_t **d, int i, const SSrc *s);
static void skip_queue_stage(const SSrc *src, const uint32_t *key, uint32_t fo) {      /* render thread, no lock held: the stage a skipped draw needs, to the front of the workers' queue (once) */
    static uint64_t asked[2048]; static int nasked;
    uint64_t id = fnv(fnv(ssrc_hb(src), key, 16), &fo, 4) ^ (uint64_t)src->stage;
    for (int i = 0; i < nasked; i++) if (asked[i] == id) return;
    if (nasked < 2048) asked[nasked++] = id;
    Job *j = calloc(1, sizeof *j + (size_t)src->code_len + src->cdata_size + 16);
    uint8_t *d = j->data;
    j->kind = 7; j->sshade = (int)fo; memcpy(j->key1, key, 16);
    job_copy_src(j, &d, 1, src);
    EnterCriticalSection(&g_sh_lock);
    job_push_front(j);
    LeaveCriticalSection(&g_sh_lock);
}
static void gpl_opt_push(Pipe *p) { Job *j = calloc(1, sizeof *j); j->kind = 6; j->pipe = p; g_gpl_opt_q++; job_push_bg(j); }
static int g_m4_on, g_m4_quit, g_scan_on;
static LONG64 g_scan_jobs, g_scan_pipes, g_cs_pairs;
static volatile LONG g_in_fight;
static struct { int w, h, ncol, cf, df, lv; long long n, fight, first; } g_rtlog[256];      /* SSBU_VK_RTLOG=1: render-target sizes (nvn_vk_draw.c, where a pass begins) */
static int g_rtlog_n;
static int g_ncont, g_cscan, g_cscan_log, g_cpredict, g_game_tables = 1;     /* the container scan (end of this file) */
static LONG64 g_cs_scans, g_cs_vars, g_cs_jobs, g_cs_tk, g_cs_pred, g_cs_bytes;
static LONG64 g_cs_neg_hit, g_cs_neg_ext, g_cs_search, g_cs_gaveup;      /* session 8: the container search's negative intervals and time budget */
static uint64_t g_cscan_reach = 64, g_cscan_step = 0x1000;    /* MB searched below a control section for the header of its container, and the alignment a header is looked for at (SSBU_VK_CSCAN_STEP) */
static void job_push(Job *j) {                     /* g_sh_lock held */
    j->next = NULL;
    if (g_jq_tail) g_jq_tail->next = j; else g_jq_head = j;
    g_jq_tail = j; g_jq_queued++;
    WakeConditionVariable(&g_jq_cv);
}
static void job_push_front(Job *j) {               /* g_sh_lock held: needed by the batch about to be replayed */
    j->next = g_jq_head;
    g_jq_head = j;
    if (!g_jq_tail) g_jq_tail = j;
    g_jq_queued++;
    WakeConditionVariable(&g_jq_cv);
}
static void job_copy_src(Job *j, uint8_t **d, int i, const SSrc *s) {
    j->has[i] = 1; j->src[i] = *s;
    memcpy(*d, s->code, (size_t)s->code_len); j->src[i].code = *d; *d += (s->code_len + 7) & ~7;
    if (s->cdata) { memcpy(*d, s->cdata, s->cdata_size); j->src[i].cdata = *d; *d += (s->cdata_size + 7) & ~7u; }
}
/* g_sh_lock held: queues the pipeline of this pair for signature i, unless the pair was already drawn, has had its three
 * predictions, or the pipeline exists. */
static void predict_pair(PairRec *pr, int i) {
    if (pr->drawn || pr->npred >= 3 || (pr->sigs >> i & 1)) return;
    PKey k = g_sig[i].k;
    k.vs = pr->vs->id; k.fs = pr->fs ? pr->fs->id : 0; k.sshade = pr->fs ? pr->sshade : 0;
    uint64_t h = fnv(0xcbf29ce484222325ULL, &k, sizeof k);
    pr->sigs |= 1ull << i;
    if (pipe_find(&k, h)) return;
    pr->npred++;
    Job *j = calloc(1, sizeof *j);
    j->kind = 1; j->pipe = pipe_new(&k, pr->vs, pr->fs, h, 0); j->pipe->predicted = 1;
    g_pred_made++;
    job_push(j);
}
/* Render thread, at a draw: the pipeline of the key; made here, or waited for, when it is not ready (the frame is late). */
static Pipe *pipe_get(const PKey *k, VStage *vs, VStage *fs) {
    uint64_t h = fnv(0xcbf29ce484222325ULL, k, sizeof *k);
    int mine = 0, waited = 0;
    LONG64 t0 = 0;
    EnterCriticalSection(&g_sh_lock);
    Pipe *p = pipe_find(k, h);
    int was_queued = 0;
    if (skip_active() && !(p && p->state == 2)) {  /* shader_wait = skip: a pipeline that is not there is made by a worker; ready stage libraries are linked here (microseconds) */
        int libs_ready = D.has_gpl && gpl_sep() && !k->sshade && (fs ? vs->lib_state == 2 && fs->lib_state == 2 : vs->lib2_state == 2);
        if (!(libs_ready && (!p || p->state == 0))) {
            if (!p) { Job *j = calloc(1, sizeof *j); j->kind = 1; j->pipe = pipe_new(k, vs, fs, h, 0); job_push_front(j); }
            LeaveCriticalSection(&g_sh_lock);
            skip_note(1, vs->hb, fs ? fs->hb : 0);
            return NULL;
        }
    }
    if (!p) { p = pipe_new(k, vs, fs, h, 1); mine = 1; }
    else if (p->state == 0) { p->state = 1; mine = 1; was_queued = 1; }          /* queued, no worker has begun: made here rather than waited for behind the queue */
    else if (p->state == 1) { waited = 1; t0 = tk(); while (p->state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); }
    if (!p->used) {
        int idx, i = sig_note(k, &idx);
        PairRec *pr0 = pair_get(vs, fs);
        pr0->drawn = 1;
        if (idx >= 0) { pr0->used |= 1ull << idx; cont_learn(pr0); }
        if (i >= 0) g_fu_fr_newsig++;
        if (D.has_gpl && !gpl_sep() && fs && fs->fragonly) gpl_shared_vs(vs);
        if (i >= 0 && g_predict) for (PairRec *pr = g_pair_all; pr; pr = pr->all) predict_pair(pr, i);      /* a pass signature never seen: the pairs not yet drawn get it now, on the workers */
    }
    LeaveCriticalSection(&g_sh_lock);
    if (waited) {
        LONG64 dt = tk() - t0; hold_note(H_PIPE_WAIT, dt); g_tm[5] += dt;
        /* session 11: the line session 10 lacked (its "0 holds in fight frames" could not see these) */
        if (holdlog_on()) { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); printf("nvn_vk_hold: frame %lld (%s): PIPELINE waited for (a worker was making it), %.2f ms: vertex code %016llx (key %x %x %x %x, forced outputs %x), fragment code %016llx, topology class %d, %d colour (format %d), depth %d; made %s\n",
                                 (long long)nvn_front_frame_next(), g_in_fight ? "fight" : "no fight", (double)dt * 1000.0 / (double)fq.QuadPart, (unsigned long long)vs->hb, vs->key[0], vs->key[1], vs->key[2], vs->key[3], vs->fo, (unsigned long long)(fs ? fs->hb : 0),
                                 k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, !D.has_gpl ? "whole (no libraries)" : p->lib_how == 0 ? "by a link of ready stage libraries" : "with a library compiled for it"); }
    }
    if (mine) {
        t0 = tk();
        g_render_tid = GetCurrentThreadId();
        double ms = pipe_build(p);
        LONG64 dt = tk() - t0;
        g_tm[5] += dt;
        if (!(D.has_gpl && p->lib_how == 0)) { hold_note(H_PIPE_DRAW, dt); g_pipe_miss++; }      /* session 9: a link of ready libraries is not a hold (counted on its own: "linked at a draw") */
        if (holdlog_on() && !(D.has_gpl && p->lib_how == 0)) printf("nvn_vk_hold: frame %lld (%s): PIPELINE made at the draw, %.2f ms: vertex code %016llx (variant key %x %x %x %x, forced outputs %x), fragment code %016llx, topology class %d, %d colour (format %d), depth %d, samples %d; it was %s\n",
                                 (long long)nvn_front_frame_next(), g_in_fight ? "fight" : "no fight", ms, (unsigned long long)vs->hb, vs->key[0], vs->key[1], vs->key[2], vs->key[3], vs->fo, (unsigned long long)(fs ? fs->hb : 0),
                                 k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, k->samples, was_queued ? "queued for a worker that had not begun" : "not known to anyone (no record, no prediction)");
        if (D.debug) printf("nvn_vk: pipeline made at a draw (%lld): stages %u/%u, class %d, %d colour (format %d), depth %d, samples %d: %.1f ms\n", (long long)g_pipe_miss, k->vs, k->fs, k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, k->samples, ms);
    }
    if (!p->used) {
        p->used = 1;
        g_fu_fr_n++; if (mine) { if (D.has_gpl && p->lib_how == 0) g_fu_fr_linked++; else g_fu_fr_made++; g_fu_fr_tk += tk() - t0; } if (waited) g_fu_fr_made++;      /* round 16, SSBU_VK_FIRSTLOG: first draws of a pipeline (pair + pass signature) per frame */
        if (p->predicted && !mine) g_pred_used++;
        if (!p->recorded) { piperec_append(k, vs, fs, 0); p->recorded = 1; }
    }
    return p;
}
/* Any thread: the compute pipeline of a stage. Returns 1 when this call made it, 2 when it waited for another thread, 0 when it was there. */
static int compute_pipe_ensure(VStage *v) {
    int how = 0;
    if (v->cp_state == 2) return 0;
    EnterCriticalSection(&g_sh_lock);
    if (v->cp_state == 0) { v->cp_state = 1; how = 1; }
    else if (v->cp_state == 1) { how = 2; while (v->cp_state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); }
    LeaveCriticalSection(&g_sh_lock);
    if (how == 1) {
        compute_pipe_make(v);
        EnterCriticalSection(&g_sh_lock);
        v->cp_state = 2;
        WakeAllConditionVariable(&g_sh_cv);
        LeaveCriticalSection(&g_sh_lock);
    }
    return how;
}

/* ---------------------------------------------------------------- the workers */
static HANDLE g_pw_thread[16], g_pw_gate;
/* Session 10: the render thread is about to wait for something a worker is making, and that worker may be a preparation
 * job at the lowest priority: every worker back to normal priority (each sets its own again at its next job). */
static int g_pw_nthread;
static void workers_boost(void) {
    if (g_offline || GetCurrentThreadId() != g_render_tid) return;
    for (int i = 0; i < g_pw_nthread; i++) if (g_pw_thread[i]) SetThreadPriority(g_pw_thread[i], THREAD_PRIORITY_NORMAL);
    g_boosts++;
}
static int g_pw_nthread, g_pw_n, g_pw_waited;
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRecIn;
static PipeRec *g_pw_rec;
static volatile LONG g_pw_next, g_pw_left;
static LONG64 g_pw_t0, g_pw_t1;
static char g_pcache_path[160];
/* PR #33. A variant named by pipes.bin, from its record: any history worker, NO lock on entry. Before, all of it (stage
 * record, SPIR-V file, vkCreateShaderModule) ran under g_sh_lock: the history workers loaded one stage at a time, and every
 * other user of the lock (the game's main thread in nvnProgramSetShaders, nvn_vk_container_seen, the look-ahead) waited
 * behind the file reads of a cold start. Now the lock is taken only to look the variant up, to register its load
 * (g_pw_loads: one load per stage and variant hash; a second history worker that wants it waits for the first on
 * g_pw_load_cv, not on g_sh_cv, whose waiters are not woken for it) and to publish it. A load that fails (record or SPIR-V
 * file missing or damaged) is remembered as NULL and published nowhere: the game's own path makes the stage when it hands
 * the program over, as before, and the bad file is read once per start instead of once per pipes.bin entry.
 * When the lock is taken again, the game's own path (a phase-2 job of a worker that has already left the history) may have
 * made the same variant meanwhile, or be making it: that one is used (waited for first while it is being made) and the
 * module just made is destroyed. Two VStages of one variant would key the history's pipelines to the one draws do not pick. */
typedef struct PwLoad { struct PwLoad *next; uint64_t hash; int stage, done; VStage *value; } PwLoad;
static PwLoad *g_pw_loads[4096];                   /* g_sh_lock; freed by prewarm_wait, after the gate */
static CONDITION_VARIABLE g_pw_load_cv;            /* a history load is done */
static VStage *pw_stage_find(int s, uint64_t vh) { /* g_sh_lock held: the variant, made and published (g_vh) */
    for (VStage *v = g_vh[vh & 4095]; v; v = v->next_h) if (v->h == vh && v->stage == s) return v;
    return NULL;
}
static VStage *pw_stage(int s, uint64_t vh) {
    LONG64 held = pw_lock(), w;
    VStage *v = pw_stage_find(s, vh);
    if (v) { pw_unlock(held); return v; }
    PwLoad *load;
    for (load = g_pw_loads[vh & 4095]; load; load = load->next) if (load->hash == vh && load->stage == s) break;
    if (load) {                                    /* another history worker is loading it (a waiter holds no load of its own: no cycle) */
        while (!load->done) {
            w = g_pw_profile ? tk() : 0;
            if (g_pw_profile) g_pw_profile->lock_hold += w - held;
            WD_SLEEP(&g_pw_load_cv, &g_sh_lock, INFINITE);
            held = g_pw_profile ? tk() : 0;
            if (g_pw_profile) g_pw_profile->stage_wait += held - w;
        }
        v = load->value;
        pw_unlock(held);
        return v;
    }
    load = calloc(1, sizeof *load);
    if (!load) vkd_fail("out of memory (the table of the prewarm's stage loads)");
    load->hash = vh; load->stage = s;
    load->next = g_pw_loads[vh & 4095]; g_pw_loads[vh & 4095] = load;
    pw_unlock(held);
    StageRec r;
    VkShaderModule mod = NULL;
    if (stage_rec_read(s, vh, &r)) mod = module_from_spv(r.spv);      /* the files and the module: without the lock */
    held = pw_lock();
    while (mod && !(v = pw_stage_find(s, vh))) {   /* not published meanwhile: is the game's own path making or holding it? */
        VStage *busy, *g = vstage_find(r.hb, s, r.code_len, r.key, r.fo, &busy);
        if (g && g->state == 2 && g->ok) { v = g; break; }      /* made, and it serves this key: used */
        if (g && g->state == 2) g = NULL;          /* made, but not translated: this one is published (as before) */
        if (!g) g = busy;
        if (!g) break;
        w = g_pw_profile ? tk() : 0;               /* being made (its key masks are not known yet): waited for, then looked at again */
        if (g_pw_profile) g_pw_profile->lock_hold += w - held;
        while (g->state < 2) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE);      /* vstage_build wakes g_sh_cv; its maker never waits for a history load */
        held = g_pw_profile ? tk() : 0;
        if (g_pw_profile) g_pw_profile->stage_wait += held - w;
    }
    if (!mod) v = pw_stage_find(s, vh);            /* the load failed: the game's path may have made it meanwhile */
    else if (!v) {
        v = vstage_new(r.hb, s, r.code_len, r.key, r.fo);
        vstage_from_rec(v, &r, vh, mod);
        v->next_h = g_vh[vh & 4095]; g_vh[vh & 4095] = v; v->state = 2;      /* published whole: nobody sees it in the making */
        mod = NULL;                                /* the published stage owns it */
        InterlockedIncrement64(&R.modules); InterlockedIncrement64(&g_pw_stages);
    }
    load->value = v; load->done = 1;
    WakeAllConditionVariable(&g_pw_load_cv);
    pw_unlock(held);
    if (mod) vkDestroyShaderModule(V.device, mod, NULL);      /* the game's path made this variant: ours is not needed */
    return v;
}
static void pw_loads_free(void) {                  /* prewarm_wait, after g_pw_gate: no history worker is in pw_stage any more */
    for (int i = 0; i < 4096; i++) {
        PwLoad *p = g_pw_loads[i];
        while (p) { PwLoad *next = p->next; free(p); p = next; }
        g_pw_loads[i] = NULL;
    }
}
/* A worker that is about to make a vertex stage gives the fragment stage of the same program to another worker first
 * (kind 3), so that a program created in the middle of a fight costs the longer of the two stages, not their sum. */
static void spawn_stage(const SSrc *src, const uint32_t *key) {
    Job *j = calloc(1, sizeof *j + (size_t)src->code_len + src->cdata_size + 16);
    uint8_t *d = j->data;
    j->kind = 3; memcpy(j->key1, key, 16);
    job_copy_src(j, &d, 1, src);
    EnterCriticalSection(&g_sh_lock);
    job_push_front(j);
    LeaveCriticalSection(&g_sh_lock);
}
/* ---- Session 9: pairs nobody announces. A program without a vertex stage (session 8, SSBU_VK_FRAGONLY) is drawn with the
 * vertex stage of ANOTHER program, so its pair is only known at the draw. The vertex stages used that way are few (one
 * screen-quad stage made 13 of the 26 unknown pipelines of run s8n1): every vertex stage ever drawn with such a fragment
 * stage is remembered ("shared"), and the pair library of shared vertex stage x fragment stage without a program of its own
 * is made by the workers (background queue) as soon as both are known. */
static VStage *g_fo[8192], *g_shvs[64];
static int g_nfo, g_nshvs;
static LONG64 g_gpl_cross;
/* SSBU_VK_GPL_CROSS: 0 no such pairs are made ahead; 1 (default) only where the fragment stage reads EXACTLY what the vertex
 * stage writes; 2 wherever the draw would take this vertex variant (run s9gn1: 3,906 such fragment stages x 21 shared vertex
 * stages = 37,059 libraries asked for, 17,693 compiled in 1,216 s of worker time: the workers were busy through the whole
 * fight and the run was WORSE than without libraries: 82 holds, 5.8 s). */
static int gpl_cross_mode(void) { static int m = -1; if (m < 0) m = getenv("SSBU_VK_GPL_CROSS") ? atoi(getenv("SSBU_VK_GPL_CROSS")) : 1; return m; }
static int gpl_pair_fits(const VStage *vs, const VStage *fs) {
    if (gpl_cross_mode() == 1) return !vs->fo && fs->in_attr == vs->out_attr;
    return (fs->in_attr & ~(vs->out_attr & ~vs->fo)) == vs->fo;      /* the draw takes the vertex variant whose forced outputs are exactly the fragment inputs the code does not write */
}
static void gpl_cross_push(VStage *vs, VStage *fs) {       /* g_sh_lock held */
    if (!gpl_cross_mode() || !gpl_pair_fits(vs, fs)) return;
    Job *j = calloc(1, sizeof *j);
    j->kind = 5; j->lvs = vs; j->lfs = fs;
    g_gpl_cross++;
    job_push_bg(j);
}
static void gpl_fo_made(VStage *v) {               /* a worker: the fragment stage of a program without a vertex stage is ready */
    if (!D.has_gpl) return;
    EnterCriticalSection(&g_sh_lock);
    if (!v->fragonly && g_nfo < 8192) { v->fragonly = 1; g_fo[g_nfo++] = v; for (int i = 0; i < g_nshvs; i++) gpl_cross_push(g_shvs[i], v); }
    LeaveCriticalSection(&g_sh_lock);
}
static void gpl_shared_vs(VStage *vs) {            /* g_sh_lock held, render thread: vs was drawn with a fragment stage of a program without a vertex stage */
    for (int i = 0; i < g_nshvs; i++) if (g_shvs[i] == vs) return;
    if (g_nshvs == 64) return;
    g_shvs[g_nshvs++] = vs;
    for (int i = 0; i < g_nfo; i++) gpl_cross_push(vs, g_fo[i]);
}
static void spawn_stage_fo(const SSrc *src, const uint32_t *key) {      /* as spawn_stage, marked: the stage of a program without a vertex stage */
    Job *j = calloc(1, sizeof *j + (size_t)src->code_len + src->cdata_size + 16);
    uint8_t *d = j->data;
    j->kind = 3; j->cont = -7; memcpy(j->key1, key, 16);
    job_copy_src(j, &d, 1, src);
    EnterCriticalSection(&g_sh_lock);
    job_push_front(j);
    LeaveCriticalSection(&g_sh_lock);
}
/* The barrier (session 9): a frame that is a loading screen (at most 60 draws) is not presented while the workers still have
 * a queued stage, pair library or pipeline: the game leaves its loading screen only when everything handed over is ready.
 * On by default with pipeline libraries (SSBU_VK_BARRIER=0 / =1 forces it). Time-out 20 s, logged loudly. */
static LONG64 g_bar_n, g_bar_tk, g_bar_max, g_bar_timeout;
static void m4_barrier(LONG64 frame, LONG64 draws) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_VK_BARRIER"); on = e ? atoi(e) != 0 : D.has_gpl; }
    if (!on || draws > 60 || !g_pw_nthread) return;
    int save = 0;
    EnterCriticalSection(&g_sh_lock);
    if (g_jq_queued != g_jq_done || g_bq_queued != g_bq_done) {
        LONG64 t0 = tk();
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        /* session 10: a loading screen that waits for the workers anyway (after the boot: frame 300 on) also waits for the
         * part of the preparation that is made into memory (the largest container: the one a fight hands over at "GO"), and
         * while it waits that part has every worker at normal priority */
        int keep = frame >= 300 && g_pk_queued != g_pk_done;
        if (keep) { g_prep_urgent = 1; g_bar_keep_n++; WakeAllConditionVariable(&g_jq_cv); }
        LONG64 d0 = g_jq_done + g_bq_done, tlast = t0;
        while ((g_jq_queued != g_jq_done || g_bq_queued != g_bq_done || (keep && g_pk_queued != g_pk_done)) && tk() - t0 < 20 * fq.QuadPart) {
            WD_SLEEP(&g_sh_cv, &g_sh_lock, 20);
            if (tk() - t0 > fq.QuadPart * 15 / 100 && tk() - tlast > fq.QuadPart / 10) {      /* session 11: held longer than 150 ms: the same picture again, with the progress bar, ten times a second */
                LONG64 tot = g_jq_queued + g_bq_queued - d0, dn = g_jq_done + g_bq_done - d0;
                tlast = tk();
                LeaveCriticalSection(&g_sh_lock);
                present_progress(tot > 0 ? (float)dn / (float)tot : 0.0f);
                EnterCriticalSection(&g_sh_lock);
            }
        }
        g_prep_urgent = 0;
        LONG64 dt = tk() - t0;
        if (keep) { g_bar_keep_tk += dt; printf("nvn_vk_barrier: frame %lld: the loading screen also waited for the preparation's containers kept in memory (%.0f ms in all for this frame)\n", (long long)frame, (double)dt * 1000.0 / (double)fq.QuadPart); }
        g_bar_n++; g_bar_tk += dt; if (dt > g_bar_max) g_bar_max = dt;
        if (g_jq_queued != g_jq_done || g_bq_queued != g_bq_done) { g_bar_timeout++; printf("nvn_vk_barrier: TIME-OUT at frame %lld: after 20 s the workers still have %lld + %lld jobs: the frame is presented, pipelines may be made at a draw\n", (long long)frame, (long long)(g_jq_queued - g_jq_done), (long long)(g_bq_queued - g_bq_done)); }
        else if (dt * 10 > fq.QuadPart) printf("nvn_vk_barrier: frame %lld (a loading screen: %lld draws) was held %.0f ms until the workers had finished every queued stage and pipeline\n", (long long)frame, (long long)draws, (double)dt * 1000.0 / (double)fq.QuadPart);
        save = g_pcsave_want && frame >= 300 && !fight_recent();
    }
    LeaveCriticalSection(&g_sh_lock);
    /* session 12: the one place where the live pipeline cache is read out while the game runs: this thread, in a loading screen it holds anyway */
    if (save && InterlockedExchange(&g_pcsave_want, 0)) pcache_save_at(1, 1);
}
/* Session 7. The variants of one code that only a draw decides (integer vertex attributes, integer textures, vertex outputs
 * forced by the fragment stage of the draw) are listed in the code's .var file by the run that first needed them. Here,
 * when the program is handed over (loading), each listed variant is made from its record, so the draw finds it. */
static void variants_known(const SSrc *src, uint64_t hb) {
    char path[300];
    VarRec e[32];
    int n, how;
    snprintf(path, sizeof path, "%s/stage/%d_%016llx.var", vkc_dir(), src->stage, (unsigned long long)hb);
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    n = (int)fread(e, sizeof e[0], 32, fp);
    fclose(fp);
    for (int i = 0; i < n; i++) {
        if (e[i].magic != STAGEREC_MAGIC || !(e[i].key[0] | e[i].key[1] | e[i].key[2] | e[i].key[3] | e[i].fo)) continue;
        stage_ensure(src, hb, e[i].key, e[i].fo, &how);
        if (how == 1) { InterlockedIncrement64(&g_w_stages); InterlockedIncrement64(&g_var_known); }
    }
}
static int g_pix_ready;
static LONG64 g_pix_ambiguous;
static uint8_t g_pix_announced[1024];
static int pix_find(uint64_t hb, int *files);
static void pix_announce(int f, int why);
static void job_prog(Job *j) {
    static const uint32_t zero[4];
    int how;
    InterlockedIncrement64(&g_w_progs);
    if (!j->prep && g_pix_ready) {                 /* session 10: a program of a container of the game's data: the whole container is announced */
        uint64_t hk[3] = {0, 0, 0};
        int files[4], nf;
        for (int i = 0; i < 3; i++) if (j->has[i]) hk[i] = ssrc_hb(&j->src[i]);
        nf = pix_find(fnv(0xcbf29ce484222325ULL, hk, sizeof hk), files);
        /* only a variation that ONE file holds names its container (run s10n4: 6,693 of 24,042 index entries are variations
         * several files hold; announcing each holder made the title screen wait 15 s for 4,800 variations of containers nobody had loaded) */
        if (nf == 1 && !g_pix_announced[files[0]]) pix_announce(files[0], 1);
        else if (nf > 1) InterlockedIncrement64(&g_pix_ambiguous);
    }
    if (j->has[0] && j->has[1] && !j->prep) spawn_stage(&j->src[1], zero);      /* the preparation puts nothing on the foreground queue */
    if (j->has[2]) {
        VStage *c = stage_ensure(&j->src[2], ssrc_hb(&j->src[2]), zero, 0, &how);
        if (how == 1) InterlockedIncrement64(&g_w_stages);
        if (!g_offline && c->ok && !c->images && c->nsmp <= 32) compute_pipe_ensure(c);
    }
    if (!j->has[0]) return;
    uint64_t hv = ssrc_hb(&j->src[0]);
    VStage *vs = stage_ensure(&j->src[0], hv, zero, 0, &how), *fs = NULL;
    if (how == 1) InterlockedIncrement64(&g_w_stages);
    if (j->has[1]) { fs = stage_ensure(&j->src[1], ssrc_hb(&j->src[1]), zero, 0, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); }
    if (!vs->ok || (fs && !fs->ok)) return;
    variants_known(&j->src[0], hv);                /* session 7: what earlier runs needed of this code at a draw */
    if (fs) variants_known(&j->src[1], fs->hb);
    if (fs && (fs->in_attr & ~vs->out_attr)) {    /* fragment inputs the vertex code never writes: the variant that declares them */
        vs = stage_ensure(&j->src[0], hv, zero, fs->in_attr & ~vs->out_attr, &how);
        if (how == 1) InterlockedIncrement64(&g_w_stages);
        if (!vs->ok) return;
    }
    if (j->cont) {                                 /* found by the container scan: the pair belongs to that container */
        EnterCriticalSection(&g_sh_lock);
        PairRec *pc = pair_get(vs, fs);
        if (!pc->cont) { pc->cont = (int16_t)j->cont; g_cs_pairs++; }
        cont_learn(pc);
        LeaveCriticalSection(&g_sh_lock);
    }
    if (D.has_gpl && !g_offline && gpl_sep() && !(fs && j->sshade) && stage_fits_universal(vs) && (!fs || stage_fits_universal(fs))) { int lh; stagelib_ensure(vs, 0, &lh); if (fs) stagelib_ensure(fs, 0, &lh); else stagelib_ensure(vs, 1, &lh); }      /* SSBU_VK_GPL=2: a library per stage (a program without a fragment stage: both forms of its vertex stage) */
    else
    if (D.has_gpl && !g_offline && !j->prep) { int lh; pairlib_ensure(vs, fs, 1, fs ? j->sshade : 0, &lh); }      /* session 9: the pair's shaders compiled now, without pass formats; a draw links */
    if (!g_predict || j->prep) return;
    EnterCriticalSection(&g_sh_lock);
    PairRec *pr = pair_get(vs, fs);
    pr->sshade = (uint8_t)j->sshade;
    for (int n = 0; n < 3 && !pr->drawn; n++) {    /* the three signatures most used so far */
        int best = -1;
        for (int i = 0; i < g_nsig; i++) if (!(pr->sigs >> i & 1) && (best < 0 || g_sig[i].uses > g_sig[best].uses)) best = i;
        if (best < 0) break;
        predict_pair(pr, best);
    }
    LeaveCriticalSection(&g_sh_lock);
}
static void job_draw(Job *j) {                     /* a draw the look-ahead met whose stages were not all made yet */
    int how;
    VStage *fs = NULL;
    if (j->has[1]) spawn_stage(&j->src[1], j->key1);
    uint64_t hv = ssrc_hb(&j->src[0]);
    VStage *vs = stage_ensure(&j->src[0], hv, j->key0, 0, &how);
    if (how == 1) InterlockedIncrement64(&g_w_stages);
    if (j->has[1]) { fs = stage_ensure(&j->src[1], ssrc_hb(&j->src[1]), j->key1, 0, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); if (!fs->ok) return; }
    if (!vs->ok) return;
    if (fs && (fs->in_attr & ~vs->out_attr)) { vs = stage_ensure(&j->src[0], hv, j->key0, fs->in_attr & ~vs->out_attr, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); if (!vs->ok) return; }
    if (!D.has_eds3 || !D.has_vids) return;        /* the key holds blend state (or, V_novids, the vertex layout) the look-ahead does not follow */
    PKey k = j->k;
    k.vs = vs->id; k.fs = fs ? fs->id : 0;
    uint64_t h = fnv(0xcbf29ce484222325ULL, &k, sizeof k);
    int mine = 0;
    EnterCriticalSection(&g_sh_lock);
    Pipe *p = pipe_find(&k, h);
    if (!p) { p = pipe_new(&k, vs, fs, h, 1); mine = 1; } else if (p->state == 0) { p->state = 1; mine = 1; }
    LeaveCriticalSection(&g_sh_lock);
    if (mine) { pipe_build(p); InterlockedIncrement64(&g_w_pipes); }
}
static int cont_walk(uint64_t base, uint64_t size, int tag, int *njobs, uint64_t obase);
static LONG64 g_cs_vars, g_cs_jobs, g_cs_tk;
/* ---- Session 10: the INDEX of the game's containers (made by the preparation thread in the first seconds of a run, no
 * translation: hashing only). Every stage code of every container file -> its file; and the first 1024 bytes of each file's
 * lowest graphics code -> its file. With it a container is ANNOUNCED whole (its file read from disk, every variation queued
 * for the workers, the loading barrier waits) as soon as the game shows any sign of it: (a) it lays a pool over the code
 * (loading; nvn_front_pool_made) or (b) it hands over ONE of its programs. Unlike the container scan this does not need the
 * container's header to be found in the game's memory (run s10n2: the searches around the programs handed over in the
 * fight all failed or were given up at their time budget, and those programs came one by one, in the fight). */
typedef struct { uint64_t hb; uint16_t file; } PixEnt;
static PixEnt g_pix[1 << 18];
static uint64_t g_pix_pool[1024], g_pix_lo;
static uint8_t g_pix_announced[1024];
static int g_pix_cur, g_pix_ready;
static LONG64 g_pix_shared, g_cs_skipped;
static LONG64 g_pix_n, g_pix_ann_pool, g_pix_ann_prog, g_pix_ann_vars, g_pix_ann_fight, g_pix_pool_hit, g_pix_pool_miss;
static char g_prep_dir[240];
static void pix_add(uint64_t hb, int file) {       /* the preparation thread only, before g_pix_ready */
    unsigned i = (unsigned)(hb * 0x9E3779B97F4A7C15ull >> 46);
    int same = 0;
    if (!hb) return;
    for (int n = 0; n < 4096; n++, i = (i + 1) & ((1u << 18) - 1)) {      /* the same variation in several files: up to 4 of them are kept */
        if (g_pix[i].hb == hb) { if (g_pix[i].file == file || ++same >= 4) return; continue; }
        if (!g_pix[i].hb) { g_pix[i].hb = hb; g_pix[i].file = (uint16_t)file; g_pix_n++; if (same) g_pix_shared++; return; }
    }
}
static int pix_find(uint64_t hb, int *files) {     /* any thread: the container files (at most 4) that hold this variation; returns their count */
    unsigned i = (unsigned)(hb * 0x9E3779B97F4A7C15ull >> 46);
    int nf = 0;
    if (!g_pix_ready || !hb) return 0;
    for (int n = 0; n < 4096 && nf < 4; n++, i = (i + 1) & ((1u << 18) - 1)) { if (g_pix[i].hb == hb) files[nf++] = g_pix[i].file; else if (!g_pix[i].hb) break; }
    return nf;
}
typedef struct { char name[40]; uint32_t size; uint8_t layout /* a layout archive's container ("...ArchiveShader...") */, late /* its pool was laid inside a fight (this run or an earlier one) */; } PrepFile;
static PrepFile g_prep_file[1024];
/* A worker: container file `f` is read and every variation goes to the background queue (the barrier and the callers of
 * nvnProgramSetShaders outside a fight wait for that queue). why: 0 a pool was laid over its code, 1 one of its programs was handed over. */
static void pix_announce(int f, int why) {
    char path[400];
    if (f < 0 || f >= 1024) return;
    EnterCriticalSection(&g_sh_lock);
    int was = g_pix_announced[f];
    g_pix_announced[f] = 1;
    LeaveCriticalSection(&g_sh_lock);
    if (was) return;
    snprintf(path, sizeof path, "%s/%s", g_prep_dir, g_prep_file[f].name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return;
    size_t n = g_prep_file[f].size;
    uint8_t *raw = malloc(n + 128), *buf = raw ? raw + 16 : NULL;
    int nj = 0, nv = -1;
    if (buf && fread(buf, 1, n, fp) == n) nv = cont_walk((uint64_t)(uintptr_t)buf, (uint64_t)n, -4, &nj, 0);
    free(raw);
    fclose(fp);
    InterlockedIncrement64(why ? &g_pix_ann_prog : &g_pix_ann_pool); InterlockedAdd64(&g_pix_ann_vars, nv > 0 ? nv : 0);
    if (g_in_fight) InterlockedIncrement64(&g_pix_ann_fight);
    if (g_in_fight && !g_prep_file[f].late) {       /* session 11: a pool laid inside a fight: remembered, so that the next start prepares this file first */
        g_prep_file[f].late = 1;
        EnterCriticalSection(&g_sh_lock);
        FILE *lf = fopen(vkc_file("late_containers.txt"), "ab");
        if (lf) { fprintf(lf, "%s\n", g_prep_file[f].name); fclose(lf); }
        LeaveCriticalSection(&g_sh_lock);
    }
    printf("nvn_vk_prep: container file %s (%u KB, %d variations) ANNOUNCED at frame %lld (%s) because %s: every variation queued for the workers\n", g_prep_file[f].name, (unsigned)(n >> 10), nv, (long long)nvn_front_frame_next(),
           g_in_fight ? "IN A FIGHT" : "no fight", why ? "the game handed over one of its programs" : "the game laid a pool over its code");
    fflush(stdout);
}
static void job_scan(Job *j) {                     /* session 7: a container met by the game's thread, walked here on its copy */
    LONG64 t0 = tk();
    int njobs = 0, nvar = cont_walk(j->cbase, j->csize, j->cont, &njobs, j->cobase);
    InterlockedAdd64(&g_cs_vars, nvar > 0 ? nvar : 0); InterlockedAdd64(&g_cs_jobs, njobs); InterlockedAdd64(&g_cs_tk, tk() - t0);
    if (nvar < 0) { printf("nvn_vk_cscan: container #%d (%llu KB, copied from %llx): NOT UNDERSTOOD (no grsc block or variation array out of the file)\n", j->cont - 1, (unsigned long long)(j->csize >> 10), (unsigned long long)j->cobase); fflush(stdout); }
    free((void *)(uintptr_t)j->cbase);
}
/* Session 10, the preparation's variation job in its normal form (prep == 2, "to the cache"): each stage is translated
 * (record + SPIR-V on disk), its library is compiled (into the driver's pipeline cache) and BOTH are destroyed again: nothing
 * stays in memory (measured, run s10n1: 9,324 stage libraries kept = 4.5 GB more private memory on NVIDIA; the game has about
 * 25,000 stage variants). When the game later hands the container over, each stage is a record read and its library a hit
 * of the pipeline cache. A code the game has handed over itself is left to that path. Compute stages are few: made and kept. */
typedef struct { uint64_t key; uint32_t in_attr, out_attr; int ok, done; } PrepSt;
static PrepSt g_prep_st[1 << 17];
static LONG64 g_prep_stages, g_prep_fromrec, g_prep_dup, g_prep_had, g_prep_libs, g_prep_tr_tk, g_prep_lib_tk;
static PrepSt prep_stage(const SSrc *src, uint32_t fo, int alone) {
    static const uint32_t zero[4];
    PrepSt r;
    uint64_t hb = ssrc_hb(src), key = fnv(fnv(hb, &fo, 4), &src->stage, 4) * 2 + (uint64_t)(alone != 0);
    VStage *busy = NULL, *v;
    unsigned i = (unsigned)(key * 0x9E3779B97F4A7C15ull >> 47);
    int how;
    memset(&r, 0, sizeof r);
    if (!key) key = 2;
    EnterCriticalSection(&g_sh_lock);
    v = vstage_find(hb, src->stage, src->code_len, zero, fo, &busy);
    if (!v && !busy) {
        for (;; i = (i + 1) & ((1u << 17) - 1)) {
            if (g_prep_st[i].key == key) { while (!g_prep_st[i].done) WD_SLEEP(&g_sh_cv, &g_sh_lock, INFINITE); r = g_prep_st[i]; g_prep_dup++; LeaveCriticalSection(&g_sh_lock); return r; }
            if (!g_prep_st[i].key) { g_prep_st[i].key = key; break; }
        }
    }
    LeaveCriticalSection(&g_sh_lock);
    if (v || busy) {                               /* the game handed this code over itself */
        v = stage_ensure(src, hb, zero, fo, &how);
        r.ok = v->ok; r.in_attr = v->in_attr; r.out_attr = v->out_attr; r.done = 1;
        InterlockedIncrement64(&g_prep_had);
        return r;
    }
    VStage *t = calloc(1, sizeof *t);
    LONG64 t0 = tk();
    t->hb = hb; t->stage = src->stage; t->code_len = src->code_len; t->fo = fo; t->state = 1; t->temp = 1;
    vstage_build(t, src);
    LONG64 t1 = tk();
    InterlockedIncrement64(&g_prep_stages); if (t->from_rec) InterlockedIncrement64(&g_prep_fromrec);
    InterlockedAdd64(&g_prep_tr_tk, t1 - t0);
    if (t->ok && D.has_gpl && gpl_sep() && stage_fits_universal(t)) {
        int lh;
        stagelib_ensure(t, 0, &lh); InterlockedIncrement64(&g_prep_libs);
        if (alone) { stagelib_ensure(t, 1, &lh); InterlockedIncrement64(&g_prep_libs); }
        InterlockedAdd64(&g_prep_lib_tk, tk() - t1);
    }
    if (t->lib) vkDestroyPipeline(V.device, t->lib, NULL);
    if (t->lib2) vkDestroyPipeline(V.device, t->lib2, NULL);
    if (t->mod) vkDestroyShaderModule(V.device, t->mod, NULL);
    r.key = key; r.ok = t->ok; r.in_attr = t->in_attr; r.out_attr = t->out_attr; r.done = 1;
    free(t);
    EnterCriticalSection(&g_sh_lock);
    g_prep_st[i] = r;
    WakeAllConditionVariable(&g_sh_cv);
    LeaveCriticalSection(&g_sh_lock);
    return r;
}
static void job_prep(Job *j) {
    static const uint32_t zero[4];
    if (j->has[2]) {
        int how;
        VStage *c = stage_ensure(&j->src[2], ssrc_hb(&j->src[2]), zero, 0, &how);
        if (c->ok && !c->images && c->nsmp <= 32) compute_pipe_ensure(c);
    }
    if (!j->has[0]) return;
    PrepSt vs = prep_stage(&j->src[0], 0, !j->has[1]), fs;
    memset(&fs, 0, sizeof fs);
    if (j->has[1]) fs = prep_stage(&j->src[1], 0, 0);
    if (vs.ok && fs.ok && (fs.in_attr & ~vs.out_attr)) prep_stage(&j->src[0], fs.in_attr & ~vs.out_attr, 0);      /* fragment inputs the vertex code never writes: the variant that declares them */
}
static DWORD WINAPI pw_main(void *arg) {           /* arg: the worker's number (its SSBU_VK_PREWARM_PROFILE record) */
    g_pw_profile = g_pw_profile_on ? &g_pw_profiles[(uintptr_t)arg & 15] : NULL;
    for (;;) {                                     /* 1. the keys of earlier runs */
        LONG i = InterlockedIncrement(&g_pw_next) - 1;
        if (i >= g_pw_n) break;
        PipeRec *r = &g_pw_rec[i];
        VStage *vs = pw_stage(r->compute ? 5 : 0, r->vs_h), *fs = r->fs_h ? pw_stage(1, r->fs_h) : NULL;      /* PR #33: each takes g_sh_lock itself, not across its files */
        LONG64 held = pw_lock();
        Pipe *p = NULL;
        if (vs && !r->compute && (!r->fs_h || fs)) {
            r->k.vs = vs->id; r->k.fs = fs ? fs->id : 0;
            uint64_t h = fnv(0xcbf29ce484222325ULL, &r->k, sizeof r->k);
            if (!pipe_find(&r->k, h)) { p = pipe_new(&r->k, vs, fs, h, 1); p->recorded = 1; }
            int idx;
            sig_note(&r->k, &idx);
            PairRec *pr0 = pair_get(vs, fs);
            pr0->drawn = 1;                        /* a pair with recorded keys needs no prediction */
            if (idx >= 0) pr0->used |= 1ull << idx;
        }
        pw_unlock(held);
        if (!vs || (r->fs_h && !fs)) { InterlockedIncrement64(&g_pw_failed); continue; }      /* a stage record or SPIR-V file is gone: made when the game hands the program over, as on a first run */
        if (r->compute) { vs->cp_recorded = 1; if (compute_pipe_ensure(vs) == 1) InterlockedIncrement64(&g_pw_pipes); continue; }
        if (p) { pipe_build(p); InterlockedIncrement64(&g_pw_pipes); }
    }
    g_pw_profile = NULL;                           /* phase 2 is not measured */
    InterlockedExchange64(&g_pw_t1, tk());
    if (InterlockedDecrement(&g_pw_left) == 0) SetEvent(g_pw_gate);
    int prio = THREAD_PRIORITY_NORMAL;
    for (;;) {                                     /* 2. what the game hands over while it runs */
        {   /* Session 7: in a fight the workers run BELOW the game's threads (measured, run s7fn1 frames 2101-2118: the 16
             * workers making the container scan's pipelines kept the CPU 80% busy and the main thread's frames took 32-40 ms).
             * SSBU_VK_WORKER_PRIO=0 keeps them at normal priority. */
            static int on = -1;
            if (on < 0) on = !(getenv("SSBU_VK_WORKER_PRIO") && !atoi(getenv("SSBU_VK_WORKER_PRIO")));
            int want = on && g_in_fight ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
            if (want != prio) { SetThreadPriority(GetCurrentThread(), want); prio = want; }
        }
        EnterCriticalSection(&g_sh_lock);
        {   /* online rollback, in a fight: only the first SSBU_VK_FIGHT_WORKERS (default 4) workers take jobs. Two games on one PC
             * (run lo14): 14 workers each starved game h's main thread at the match start (frames of 200-800 ms, thread holds of
             * 200 ms for the ring captures); with 4 workers (lo15) those frames were gone. Rollback needs the spare cores. */
            static int cap = -2;
            if (cap == -2) { const char *e = getenv("SSBU_RB_NET"), *w = getenv("SSBU_VK_FIGHT_WORKERS"); cap = !(e && e[0] && e[0] != '0') ? -1 : w ? atoi(w) : 4; if (cap == 0) cap = -1; }
            while (cap > 0 && (int)(uintptr_t)arg >= cap && fight_recent() && !g_m4_quit) SleepConditionVariableCS(&g_sh_cv, &g_sh_lock, 100);      /* not g_jq_cv: its single wakes must reach a worker that takes the job */
        }
        /* session 10: a preparation job may be taken when nothing else waits; in a fight by at most g_prep_fight_cap workers */
#define PQ_OK (g_pq_head && g_pq_running < (g_prep_urgent ? 64 : fight_recent() ? g_prep_fight_cap : g_prep_cap))
        while (!g_jq_head && !g_bq_head && !PQ_OK && !g_m4_quit) SleepConditionVariableCS(&g_jq_cv, &g_sh_lock, g_pq_head ? 50 : INFINITE);
        Job *j = g_jq_head;
        int bg = 0;
        if (j) { g_jq_head = j->next; if (!g_jq_head) g_jq_tail = NULL; }
        else if (!g_m4_quit && (j = g_bq_head) != NULL) { g_bq_head = j->next; if (!g_bq_head) g_bq_tail = NULL; bg = 1; }      /* at exit the background work is left undone */
        else if (!g_m4_quit && PQ_OK) { j = g_pq_head; g_pq_head = j->next; if (!g_pq_head) g_pq_tail = NULL; bg = 2; g_pq_running++; }
        Pipe *p = NULL;
        if (j && j->kind == 1 && j->pipe->state == 0) { p = j->pipe; p->state = 1; }
        LeaveCriticalSection(&g_sh_lock);
        if (!j) break;
        if (bg == 2) {                             /* the preparation runs BELOW everything of the game: below normal in menus, lowest in a fight */
            int want = g_prep_urgent ? THREAD_PRIORITY_NORMAL : g_in_fight ? THREAD_PRIORITY_LOWEST : THREAD_PRIORITY_BELOW_NORMAL;
            SetThreadPriority(GetCurrentThread(), want); prio = -99;      /* set again at the next job (workers_boost may have changed it meanwhile) */
        }
        if (j->kind == 0) { if (j->prep == 2) job_prep(j); else job_prog(j); }
        else if (j->kind == 2) job_draw(j);
        else if (j->kind == 3) { int how; VStage *v = stage_ensure(&j->src[1], ssrc_hb(&j->src[1]), j->key1, 0, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); if (v && v->ok && D.has_gpl && gpl_sep() && !g_offline && stage_fits_universal(v)) { int lh; stagelib_ensure(v, 0, &lh); } else if (j->cont == -7 && v && v->ok) gpl_fo_made(v); }
        else if (j->kind == 5) { int lh; pairlib_ensure(j->lvs, j->lfs, 1, 0, &lh); }
        else if (j->kind == 6) gpl_optimise(j->pipe);
        else if (j->kind == 7) { int how, lh; VStage *v = stage_ensure(&j->src[1], ssrc_hb(&j->src[1]), j->key1, (uint32_t)j->sshade, &how); if (how == 1) InterlockedIncrement64(&g_w_stages); if (v && v->ok && D.has_gpl && gpl_sep() && stage_fits_universal(v)) stagelib_ensure(v, 0, &lh); }      /* the stage a skipped draw waits for */
        else if (j->kind == 4) job_scan(j);
        else if (j->kind == 8) pix_announce(j->cont, 0);
        else if (p) { pipe_build(p); InterlockedIncrement64(&g_w_pipes); }
        int pk = j->prep == 1;
        free(j);
        EnterCriticalSection(&g_sh_lock);
        if (bg == 2) { g_pq_done++; g_pq_running--; if (pk) g_pk_done++; } else if (bg) g_bq_done++; else g_jq_done++;
        WakeAllConditionVariable(&g_sh_cv);
        LeaveCriticalSection(&g_sh_lock);
    }
    return 0;
}
/* nvnProgramSetShaders, GUEST thread (called by the front end when the renderer is Vulkan): the program's stages go to
 * the workers now. The caller is held only while more jobs wait than there are workers (a burst of a loading thread),
 * so that loading ends when its shaders are ready; a single program created during a fight does not hold its caller. */
/* Session 7 (SSBU_VK_STAGECOPY=1; OFF by default until it has a whole verification). Measured in a first run (s7gn1, hold
 * log): 32 of the 34 stages made or waited for at a draw had a code hash that NOBODY had handed over, although their programs
 * were set long before: Stage.code points into the GAME's memory and that memory no longer held at the draw what it held at
 * nvnProgramSetShaders (a console's driver compiles at that call and never reads the memory again). With the switch the
 * stage's bytes (program header, code, constant-buffer-1 data, in their relative places) are copied at the call and the
 * stage points at the copy; the copy of an earlier call for the same stage is freed. */
static struct { Stage *st; void *blk; } g_scopy[65536];
static LONG64 g_scopy_n, g_scopy_bytes;
static void stage_copy(Stage *st, int s) {
    if (!st->code || st->code_len <= 0) return;
    const uint8_t *lo = st->code - (s == 5 ? 0 : 0x30), *hi = st->code + st->code_len;
    if (st->ctl[3] && st->ctl[4] && lo + st->ctl[4] + st->ctl[3] > hi) hi = lo + st->ctl[4] + st->ctl[3];
    size_t n = (size_t)(hi - lo);
    if (n > (16u << 20)) return;
    unsigned i = (unsigned)(((uintptr_t)st * 0x9E3779B97F4A7C15ull) >> 48);
    int k;
    for (k = 0; k < 64; k++, i = (i + 1) & 65535) if (g_scopy[i].st == st || !g_scopy[i].st) break;
    if (k == 64) return;                           /* table neighbourhood full: the stage keeps pointing at the game's memory */
    if (g_scopy[i].st == st && st->code >= (const uint8_t *)g_scopy[i].blk && st->code < (const uint8_t *)g_scopy[i].blk + (16u << 20) && lo == (const uint8_t *)g_scopy[i].blk) return;      /* already the copy */
    uint8_t *blk = malloc(n);
    if (!blk) return;
    memcpy(blk, lo, n);
    free(g_scopy[i].blk);
    g_scopy[i].st = st; g_scopy[i].blk = blk;
    st->code = blk + (st->code - lo);
    g_scopy_n++; g_scopy_bytes += (LONG64)n;
}
void nvn_vk_program_set(void *prog) {
    Prog *p = prog;
    SSrc s[3];
    if (!g_m4_on || p->st[2].state || p->st[3].state || p->st[4].state) return;
    {   static int on = -1;
        if (on < 0) on = getenv("SSBU_VK_STAGECOPY") && atoi(getenv("SSBU_VK_STAGECOPY"));
        if (on) { EnterCriticalSection(&g_sh_lock); stage_copy(&p->st[0], 0); stage_copy(&p->st[1], 1); stage_copy(&p->st[5], 5); LeaveCriticalSection(&g_sh_lock); }
    }
    int has[3] = {ssrc_of(&s[0], p, 0), ssrc_of(&s[1], p, 1), ssrc_of(&s[2], p, 5)};
    if (!has[0] && !has[2] && has[1]) {
        /* Session 7, SSBU_VK_FRAGONLY=1 (written at the end of the session, compiled, NOT RUN: off by default). A program
         * with a fragment stage and no vertex stage (the game binds stages of different programs) was never handed over:
         * its fragment stage was translated at its first draw. Believed to be the "code never handed over" stages of the
         * hold log (28-32 of the 29-34 stage holds of a first run: all fragment stages of programs set at start). */
        /* Session 8: RUN, and ON by default (SSBU_VK_FRAGONLY=0 = as before). First run on an empty cache, same exe, quiet PC:
         * off (s8e0) 24 stages made at a draw (297 ms) + 13 waited for (127 ms), 123 holds / 730 ms in all, 0 of 6 fight
         * windows clean; on (s8e1) 0 made + 4 waited for (29 ms), 81 holds / 250 ms, 6 of 8 clean. The price is at loading:
         * 7,740 stages made by the workers instead of 3,872, callers of nvnProgramSetShaders held 17.5 s instead of 7.4 s
         * over the whole first run (a second run reads them from the cache). */
        static int on = -1;
        if (on < 0) on = !(getenv("SSBU_VK_FRAGONLY") && !atoi(getenv("SSBU_VK_FRAGONLY")));
        if (on) { static const uint32_t zero[4]; if (holdlog_on()) { EnterCriticalSection(&g_sh_lock); ho_note(ssrc_hb(&s[1]), 0); LeaveCriticalSection(&g_sh_lock); } spawn_stage_fo(&s[1], zero); }
        return;
    }
    if (!has[0] && !has[2]) return;
    LONG64 ta = tk();
    size_t need = 0;
    for (int i = 0; i < 3; i++) if (has[i]) need += (size_t)s[i].code_len + s[i].cdata_size + 16;
    Job *j = calloc(1, sizeof *j + need);
    uint8_t *d = j->data;
    j->sshade = p->sample_shading;
    for (int i = 0; i < 3; i++) if (has[i]) job_copy_src(j, &d, i, &s[i]);
    uint64_t hn[2] = {0, 0};
    if (holdlog_on()) for (int i = 0; i < 2; i++) if (has[i]) hn[i] = ssrc_hb(&s[i]);      /* session 8: hashed before the lock is taken */
    LONG64 tb = tk();
    nvn_setsh_part[5] += tb - ta;
    EnterCriticalSection(&g_sh_lock);
    nvn_setsh_part[6] += tk() - tb;
    if (holdlog_on()) for (int i = 0; i < 2; i++) if (has[i]) ho_note(hn[i], 0);
    job_push(j);
    /* session 6: outside a fight (no compute dispatch in the last frame) the caller is also held while the container scan
     * still has work: loading ends when everything the container holds is ready */
    int fr = fight_recent();
    /* session 7: in a fight the caller (the game's main thread) is not held at all: a draw that needs the program waits for it
     * on the render thread, a program not drawn in this frame costs nothing */
    if (!fr && (g_jq_queued - g_jq_done > g_pw_nthread || g_bq_queued > g_bq_done)) {
        LONG64 t0 = tk();
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        while ((g_jq_queued - g_jq_done > g_pw_nthread || g_bq_queued > g_bq_done) && tk() - t0 < 30 * fq.QuadPart) WD_SLEEP(&g_sh_cv, &g_sh_lock, 100);
        g_set_held++; g_set_held_tk += tk() - t0;
        nvn_setsh_part[7] += tk() - t0;
    }
    LeaveCriticalSection(&g_sh_lock);
}
static int piperec_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(PipeRec)); }
static void prewarm_start(void) {
    InitializeCriticalSection(&g_sh_lock); InitializeCriticalSection(&g_gpl_lock);
    InitializeConditionVariable(&g_sh_cv); InitializeConditionVariable(&g_jq_cv);
    CreateDirectoryA(vkc_dir(), NULL); CreateDirectoryA(vkc_file("spv"), NULL); CreateDirectoryA(vkc_file("stage"), NULL);
    if (getenv("SSBU_VK_CACHE")) printf("nvn_vk: cache folder %s (SSBU_VK_CACHE)\n", vkc_dir());
    {   HMODULE m = LoadLibraryA("ssbu_glslang.dll");
        g_glsl_to_spirv = m ? (void *)GetProcAddress(m, "ssbu_glsl_to_spirv") : NULL; g_spirv_free = m ? (void *)GetProcAddress(m, "ssbu_spirv_free") : NULL;
        if (!g_glsl_to_spirv || !g_spirv_free) vkd_fail("ssbu_glslang.dll cannot be loaded (build_glslang.bat makes it)");
    }
    {   /* the driver's pipeline cache, ours on disk: keyed by GPU and driver version (the driver checks its own header too) */
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        void *data = NULL;
        snprintf(g_pcache_path, sizeof g_pcache_path, "%s/pipelines_%04x_%04x_%08x.bin", vkc_dir(), (unsigned)V.props.vendorID, (unsigned)V.props.deviceID, (unsigned)V.props.driverVersion);
        LONG64 tp0 = tk(), tp1;
        LARGE_INTEGER pfq; QueryPerformanceFrequency(&pfq);
        FILE *fp = fopen(g_pcache_path, "rb");
        if (fp) { _fseeki64(fp, 0, SEEK_END); long long n = _ftelli64(fp); _fseeki64(fp, 0, SEEK_SET); if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, fp) == (size_t)n) { ci.initialDataSize = (size_t)n; ci.pInitialData = data; } fclose(fp); }
        tp1 = tk();
        if (vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; VKC(vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache)); }
        /* session 11: what the whole-file read costs at every start (file read, then the driver taking the data) */
        printf("nvn_vk: pipeline cache %s: %llu KB read (file read %.0f ms, driver took it in %.0f ms: start-up time on the thread that creates the device)\n", g_pcache_path, (unsigned long long)(ci.initialDataSize >> 10), (double)(tp1 - tp0) * 1000.0 / (double)pfq.QuadPart, (double)(tk() - tp1) * 1000.0 / (double)pfq.QuadPart);
        free(data);
    }
    /* Blind prediction (pair x most used pass signatures) is OFF unless SSBU_VK_PREDICT=1: measured on a first run it made
     * 8,418 pipelines of which draws used 324, and kept the workers from the programs being handed over. The look-ahead
     * (scan_list below) names exactly what a batch needs instead. */
    g_predict = D.has_eds3 && D.has_vids && getenv("SSBU_VK_PREDICT") && atoi(getenv("SSBU_VK_PREDICT"));
    g_scan_on = D.has_eds3 && !(getenv("SSBU_VK_LOOKAHEAD") && !atoi(getenv("SSBU_VK_LOOKAHEAD")));
    g_cscan = !(getenv("SSBU_VK_CSCAN") && !atoi(getenv("SSBU_VK_CSCAN")));                              /* session 6: the container scan */
    g_cpredict = D.has_eds3 && D.has_vids && !(getenv("SSBU_VK_CPREDICT") && !atoi(getenv("SSBU_VK_CPREDICT")));      /* and the pipelines of a container's pass signatures for all its pairs */
    g_cscan_log = getenv("SSBU_VK_CSCAN_LOG") && atoi(getenv("SSBU_VK_CSCAN_LOG"));
    if (getenv("SSBU_VK_CSCAN_STEP") && atoi(getenv("SSBU_VK_CSCAN_STEP")) >= 8) g_cscan_step = (uint64_t)atoi(getenv("SSBU_VK_CSCAN_STEP")) & ~7ull;
    if (getenv("SSBU_VK_CSCAN_REACH") && atoi(getenv("SSBU_VK_CSCAN_REACH")) > 0) g_cscan_reach = (uint64_t)atoi(getenv("SSBU_VK_CSCAN_REACH"));
    int ahead = !(getenv("SSBU_VK_PREWARM") && !atoi(getenv("SSBU_VK_PREWARM")));
    FILE *fp = ahead ? fopen(vkc_file(VK_PIPES_BIN), "rb") : NULL;
    if (fp) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        g_pw_rec = malloc((size_t)n + 1);
        g_pw_n = (int)(fread(g_pw_rec, sizeof(PipeRec), (size_t)n / sizeof(PipeRec), fp));
        fclose(fp);
        int kept = 0;
        qsort(g_pw_rec, (size_t)g_pw_n, sizeof(PipeRec), piperec_cmp);
        for (int i = 0; i < g_pw_n; i++) if (g_pw_rec[i].magic == PIPEREC_MAGIC && nvk_vl_rec_ok(&g_pw_rec[i].k) && (!kept || memcmp(&g_pw_rec[kept - 1], &g_pw_rec[i], sizeof(PipeRec)))) g_pw_rec[kept++] = g_pw_rec[i];
        g_pw_n = kept;
    } else printf(ahead ? "nvn_vk: no ../cache/vk/pipes.bin: a first run on this PC; stages and pipelines are made when the game hands its programs over\n" : "nvn_vk: SSBU_VK_PREWARM=0: the pipeline list of earlier runs is not read\n");
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_pw_nthread = (int)si.dwNumberOfProcessors - 2;
    if (g_pw_nthread < 1) g_pw_nthread = 1;
    if (g_pw_nthread > 16) g_pw_nthread = 16;
    if (getenv("SSBU_VK_WORKERS") && atoi(getenv("SSBU_VK_WORKERS")) > 0 && atoi(getenv("SSBU_VK_WORKERS")) <= 16) g_pw_nthread = atoi(getenv("SSBU_VK_WORKERS"));
    g_pw_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_pw_left = g_pw_nthread;
    InitializeConditionVariable(&g_pw_load_cv);
    g_pw_profile_on = getenv("SSBU_VK_PREWARM_PROFILE") && atoi(getenv("SSBU_VK_PREWARM_PROFILE"));      /* PR #33: nvn_vk_prewarm.h */
    g_pw_t0 = tk();
    for (int i = 0; i < g_pw_nthread; i++) g_pw_thread[i] = CreateThread(NULL, 0, pw_main, (void *)(uintptr_t)i, 0, NULL);
    g_m4_on = !(getenv("SSBU_VK_HANDOVER") && !atoi(getenv("SSBU_VK_HANDOVER")));
    printf("nvn_vk: %d pipelines listed in ../cache/vk/pipes.bin; %d worker threads; programs are handed to them when the game sets their shaders: %s; key prediction: %s\n", g_pw_n, g_pw_nthread,
           g_m4_on ? "yes" : "no (SSBU_VK_HANDOVER=0)", g_predict ? "blind (SSBU_VK_PREDICT=1)" : g_scan_on ? "look-ahead of every batch" : D.has_eds3 ? "none (SSBU_VK_LOOKAHEAD=0)" : "none (blend state is part of the key on this GPU)");
    if (!(getenv("SSBU_VK_PREWARM_BENCH") && atoi(getenv("SSBU_VK_PREWARM_BENCH")))) prep_start();      /* PR #33: the bench measures the prewarm alone (vkd_init) */
}
/* ---------------------------------------------------------------- look-ahead: what a batch will draw, before it is replayed
 * The lists of a submitted batch are read once for the (stage pair, pass formats, topology class) of every draw. A
 * combination not met before whose stages or pipeline do not exist goes to the FRONT of the worker queue; the replay that
 * follows then finds them made, waits for the one being made, or makes a queued one itself: the work of a frame that needs
 * many new pipelines at once (the first frame of a fight) is spread over all cores instead of done one by one at the draws.
 * Not followed: integer texture formats (taken as bound now) and, without extended dynamic state 3, anything. */
typedef struct { Prog *prog[2]; int ncol, nattr, sig_ok, attr_new; TargetRef col[8], depth; uint32_t vattr[32], ints, sints, sig_tag; uint8_t logic; PKey sig; } Scan;
static uint64_t g_scan_seen[8192];
static void scan_sig(Scan *s) {
    memset(&s->sig, 0, sizeof s->sig);
    s->sig_ok = pkey_targets_of(&s->sig, s->ncol, s->col, &s->depth) && (s->ncol || s->depth.tex);
    s->sig_tag = (uint32_t)fnv(0xcbf29ce484222325ULL, &s->sig, sizeof s->sig);
}
static void scan_begin(Scan *s) {
    s->prog[0] = R.prog[0]; s->prog[1] = R.prog[1]; s->ncol = R.ncol; memcpy(s->col, R.col, sizeof s->col); s->depth = R.depth;
    s->nattr = R.nattr; memcpy(s->vattr, R.vattr, sizeof s->vattr); s->attr_new = 1; s->logic = R.logic;
    scan_sig(s);
}
static void scan_draw(Scan *s, unsigned prim) {
    static const uint8_t tclass[7] = {0, 1, 1, 1, 2, 2, 2};
    Prog *p0 = s->prog[0], *p1 = s->prog[1];
    if (!p0 || prim > 6 || !s->sig_ok || !p0->st[0].code || (p1 && !p1->st[1].code) || p0->dead || (p1 && p1->dead)) return;
    if (s->attr_new) {                             /* which attributes are integers (as draw_prepare) */
        s->ints = s->sints = 0; s->attr_new = 0;
        for (int i = 0; i < 16 && i < s->nattr; i++) {
            int nf = (int)(s->vattr[i] & 0xff), k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
            if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) { s->ints |= 1u << i; if (k == 3 || k == 8 || k == 11) s->sints |= 1u << i; }
            if (nf == 0x3E) s->ints |= 1u << i;
            if (nf == 0x6A) { s->ints |= 1u << i; s->sints |= 1u << i; }
        }
    }
    uint64_t w[6] = {(uint64_t)(uintptr_t)p0->st[0].code ^ ((uint64_t)p0->serial << 44), p1 ? (uint64_t)(uintptr_t)p1->st[1].code ^ ((uint64_t)p1->serial << 44) : 0, s->sig_tag, tclass[prim] | (uint64_t)s->logic << 8 | (uint64_t)(p1 ? p1->sample_shading & 1 : 0) << 16,
                     (uint64_t)s->ints << 32 | s->sints, (uint64_t)R.tex_int[0] << 32 | R.tex_int[1]};
    uint64_t tag = fnv(0xcbf29ce484222325ULL, w, sizeof w) | 1;
    unsigned slot = (unsigned)(tag >> 20) & 8191;
    if (g_scan_seen[slot] == tag) return;
    g_scan_seen[slot] = tag;
    uint32_t key0[4] = {s->ints, s->sints, R.tex_int[0], R.tex_sint[0]}, key1[4] = {0, 0, R.tex_int[1], R.tex_sint[1]};
    VStage *vs = p0->st[0].vk, *fs = p1 ? p1->st[1].vk : NULL;
    int fs_made = !p1 || (fs && fs->state == 2 && fs->fo == 0 && key_fits(1, key1, fs->key, fs->dep, fs->tex_units));
    int vs_made = fs_made && vs && vs->state == 2 && key_fits(0, key0, vs->key, vs->dep, vs->tex_units) && vs->fo == (fs && fs->ok ? fs->in_attr & ~(vs->out_attr & ~vs->fo) : 0);
    PKey k = s->sig;
    k.topo_class = tclass[prim]; k.logic = s->logic; k.sshade = p1 ? (uint8_t)p1->sample_shading : 0;
    if (vs_made && fs_made) {
        if (!vs->ok || (fs && !fs->ok) || !D.has_vids) return;      /* V_novids: the look-ahead does not know the vertex layout */
        k.vs = vs->id; k.fs = fs ? fs->id : 0;
        uint64_t h = fnv(0xcbf29ce484222325ULL, &k, sizeof k);
        EnterCriticalSection(&g_sh_lock);
        Pipe *pf = pipe_find(&k, h);
        if (!pf || pf->state == 0) {               /* not there, or waiting in the background queue: to the front */
            Job *j = calloc(1, sizeof *j);
            j->kind = 1; j->pipe = pf ? pf : pipe_new(&k, vs, fs, h, 0);
            job_push_front(j);
            g_scan_pipes++;
        }
        LeaveCriticalSection(&g_sh_lock);
        return;
    }
    SSrc a, b;
    if (!ssrc_of(&a, p0, 0)) return;
    int has_f = p1 && ssrc_of(&b, p1, 1);
    Job *j = calloc(1, sizeof *j + (size_t)a.code_len + a.cdata_size + 16 + (has_f ? (size_t)b.code_len + b.cdata_size + 16 : 0));
    uint8_t *d = j->data;
    j->kind = 2; j->k = k; memcpy(j->key0, key0, 16); memcpy(j->key1, key1, 16);
    job_copy_src(j, &d, 0, &a);
    if (has_f) job_copy_src(j, &d, 1, &b);
    EnterCriticalSection(&g_sh_lock);
    job_push_front(j);
    g_scan_jobs++;
    LeaveCriticalSection(&g_sh_lock);
}
static void scan_list(uint64_t handle, int depth, Scan *s) {
    size_t len = 0;
    uint8_t *data = nvn_front_list(handle, &len);
    if (!data) return;
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        const uint64_t *a = m->a;
        if (m->size < sizeof(Cmd)) break;
        switch (m->op) {
        case OP_PROGRAM: for (int st = 0; st < 2; st++) if (a[1] & (1u << st)) { Prog *p = P(a[0]); s->prog[st] = p && !p->dead && p->st[st].state ? p : NULL; } break;
        case OP_TARGETS: {
            const TargetRef *col = (const TargetRef *)BLOB(m), *dep = col + 8;
            int n = (int)a[0] < 8 ? (int)a[0] : 8;
            memset(s->col, 0, sizeof s->col); memset(&s->depth, 0, sizeof s->depth); s->ncol = 0;
            for (int i = 0; i < n; i++) if (col[i].tex && !col[i].tex->dead) { s->col[i] = col[i]; s->ncol = i + 1; }
            if (dep->tex && !dep->tex->dead) s->depth = *dep;
            scan_sig(s);
            break;
        }
        case OP_VATTR: s->nattr = (int)a[0] < 32 ? (int)a[0] : 32; memcpy(s->vattr, BLOB(m), (size_t)s->nattr * 4); s->attr_new = 1; break;
        case OP_COLOR: s->logic = ((const uint8_t *)&a[0])[1]; break;
        case OP_DRAW_ARRAYS: case OP_DRAW_ELEMENTS: scan_draw(s, (unsigned)a[0]); break;
        case OP_CALL: for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, BLOB(m) + i * 8, 8); if (depth < 8) scan_list(h, depth + 1, s); } break;
        default: break;
        }
        off += m->size;
    }
}
static void prewarm_wait(void) {                   /* render thread, before its first batch */
    if (g_pw_waited) return;
    g_pw_waited = 1;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONG64 t0 = tk();
    WaitForSingleObject(g_pw_gate, INFINITE);
    printf("nvn_vk: pipelines made ahead: %lld of %d listed (%lld stages from their records, %lld entries without their stage files) in %.0f ms on %d threads; the first batch waited %.0f ms for them\n",
           (long long)g_pw_pipes, g_pw_n, (long long)g_pw_stages, (long long)g_pw_failed, (double)(g_pw_t1 - g_pw_t0) * 1000.0 / (double)fq.QuadPart, g_pw_nthread, (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart);
    fflush(stdout);
    pw_profile_report(g_pw_nthread);               /* PR #33: SSBU_VK_PREWARM_PROFILE=1 */
    pw_loads_free();
    free(g_pw_rec); g_pw_rec = NULL;
}
static void m4_report(void) {
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    if (D.has_gpl) {
        double q = 1000.0 / (double)fq.QuadPart;
        printf("nvn_vk_gpl: pair libraries compiled by the workers: %lld (%.0f ms of worker time; %lld of them asked for as shared vertex stage x fragment stage without a program of its own: %d such fragment stages, %d shared vertex stages); "
               "PAIR LIBRARY COMPILED AT A DRAW (a hold): %lld, %.1f ms, longest %.2f ms; waited for at a draw or by a worker: %lld, %.1f ms | pipelines LINKED at a draw from ready libraries: %lld (%lld in fight frames), %.1f ms in all, longest %.2f ms; linked by workers: %lld, %.0f ms; %d fragment output libraries\n",
               (long long)g_gpl_made, (double)g_gpl_made_tk * q, (long long)g_gpl_cross, g_nfo, g_nshvs, (long long)g_gpl_draw, (double)g_gpl_draw_tk * q, (double)g_gpl_draw_max * q, (long long)g_gpl_wait, (double)g_gpl_wait_tk * q,
               (long long)g_gpl_dlink, (long long)g_gpl_dlink_fight, (double)g_gpl_dlink_tk * q, (double)g_gpl_dlink_max * q, (long long)g_gpl_link, (double)g_gpl_link_tk * q, g_nolib);
    }
    if (D.has_gpl) {
        double q2 = 1000.0 / (double)fq.QuadPart;
        printf("nvn_vk_feedback: the driver's creation feedback: stage libraries: %lld pipeline-cache HITS, %lld COMPILED NOW (%.0f ms of driver time), %lld without feedback | links: %lld hits, %lld compiled now (%.0f ms), %lld without feedback | "
               "on the render thread in FIGHT frames: libraries %lld hits (%.1f ms) / %lld compiled (%.1f ms); links %lld hits (%.1f ms) / %lld not hits (%.1f ms)\n",
               (long long)g_fb_hit[0], (long long)g_fb_miss[0], (double)g_fb_miss_ns[0] / 1e6, (long long)g_fb_none[0], (long long)g_fb_hit[1], (long long)g_fb_miss[1], (double)g_fb_miss_ns[1] / 1e6, (long long)g_fb_none[1],
               (long long)g_fb_fhit[0], (double)g_fb_fhit_tk[0] * q2, (long long)g_fb_fmiss[0], (double)g_fb_fmiss_tk[0] * q2, (long long)g_fb_fhit[1], (double)g_fb_fhit_tk[1] * q2, (long long)g_fb_fmiss[1], (double)g_fb_fmiss_tk[1] * q2);
    }
    if (D.has_gpl) printf("nvn_vk_gpl: mode %s; pipelines linked again with optimisation by the workers: %lld of %lld queued, %.0f ms of worker time\n", gpl_sep() ? "2 (a library per stage)" : "1 (a library per stage pair)", (long long)g_gpl_opt_n, (long long)g_gpl_opt_q, (double)g_gpl_opt_tk * 1000.0 / (double)fq.QuadPart);
    printf("nvn_vk_skip: shader_wait = %s; draws left out because a stage or pipeline was not ready: %lld in %lld frames (stage not ready %lld, pipeline not ready %lld)\n", skip_on() ? "skip (near a fight)" : "hold (the default)", (long long)g_skip_n, (long long)g_skip_frames, (long long)g_skip_stage, (long long)g_skip_pipe);
    if (g_pix_ready) printf("nvn_vk_prep: containers announced whole from the index: %lld because a pool was laid over their code, %lld because one of their programs was handed over (%lld of all these while IN A FIGHT), %lld variations queued; pools looked up: %lld found in the index, %lld not; programs handed over whose variation several files hold (no container named): %lld; header searches not made near a fight: %lld\n",
                            (long long)g_pix_ann_pool, (long long)g_pix_ann_prog, (long long)g_pix_ann_fight, (long long)g_pix_ann_vars, (long long)g_pix_pool_hit, (long long)g_pix_pool_miss, (long long)g_pix_ambiguous, (long long)g_cs_skipped);
    if (g_prep_state == 1 || g_prep_state == 2) { prep_line(g_prep_state == 2 ? "report (finished)" : "report (NOT finished)"); printf("nvn_vk_prep: fight frames that passed while it ran: %lld; times the render thread raised the workers' priority because it had to wait for one: %lld; loading-screen frames that also waited for the containers kept in memory: %lld, %.0f ms in all\n", (long long)g_prep_fight_frames, (long long)g_boosts, (long long)g_bar_keep_n, (double)g_bar_keep_tk * 1000.0 / (double)fq.QuadPart); }
    printf("nvn_vk_barrier: loading-screen frames held for the workers: %lld, %.0f ms in all, longest %.0f ms, time-outs %lld | progress bar: %lld extra presents of a held loading screen, %lld normal frames with the thin bar (store / preparation running, no fight near)\n", (long long)g_bar_n, (double)g_bar_tk * 1000.0 / (double)fq.QuadPart, (double)g_bar_max * 1000.0 / (double)fq.QuadPart, (long long)g_bar_timeout, (long long)g_ovl_presents, (long long)g_ovl_frames);
    for (int i = 0; i < g_rtlog_n; i++) printf("nvn_vk_rt: %d x %d (mip level %d), %d colour targets (first format 0x%x), depth format 0x%x: %lld passes, %lld of them in fight frames, first in frame %lld\n", g_rtlog[i].w, g_rtlog[i].h, g_rtlog[i].lv, g_rtlog[i].ncol, g_rtlog[i].cf, g_rtlog[i].df, g_rtlog[i].n, g_rtlog[i].fight, g_rtlog[i].first);
    double k = 1000.0 / (double)fq.QuadPart;
    LONG64 n = 0, t = 0;
    for (int i = 0; i < H_N; i++) { n += g_hold_n[i]; t += g_hold_tk[i]; }
    printf("nvn_vk: texture descriptor sets: %lld reused after a full comparison of their lists, %lld hash hits with a different list (given a set of their own); samplers given a dummy because the texture is of another kind than the shader declares: %lld\n", (long long)R.sets_reused, (long long)R.tset_collisions, (long long)R.dummy_kind);
    printf("nvn_vk_m4: holds of the render thread at a draw: %lld, %.0f ms in all, longest %.1f ms (stage made at the draw %lld / %.0f ms, stage waited for %lld / %.0f ms, pipeline made at the draw %lld / %.0f ms, pipeline waited for %lld / %.0f ms)\n",
           (long long)n, (double)t * k, (double)g_hold_max * k, (long long)g_hold_n[0], (double)g_hold_tk[0] * k, (long long)g_hold_n[1], (double)g_hold_tk[1] * k, (long long)g_hold_n[2], (double)g_hold_tk[2] * k, (long long)g_hold_n[3], (double)g_hold_tk[3] * k);
    printf("nvn_vk_m4: workers: %lld programs handed over, %lld stages made by them, %lld pipelines made by them; look-ahead: %lld pipelines and %lld draws with stages still to make sent ahead, %.0f ms of reading in all; blind prediction: %lld keys, %lld used; pass signatures %d; nvnProgramSetShaders held its caller %lld times, %.0f ms in all\n",
           (long long)g_w_progs, (long long)g_w_stages, (long long)g_w_pipes, (long long)g_scan_pipes, (long long)g_scan_jobs, (double)g_tm[9] * k, (long long)g_pred_made, (long long)g_pred_used, g_nsig, (long long)g_set_held, (double)g_set_held_tk * k);
    printf("nvn_vk_cscan: searches for a container header: %lld made (%lld given up at their time budget); answered by an interval a failed search had proved empty: %lld at once, %lld after looking at the steps above it\n", (long long)g_cs_search, (long long)g_cs_gaveup, (long long)g_cs_neg_hit, (long long)g_cs_neg_ext);
    printf("nvn_vk_cscan: container scan %s: %lld containers scanned (%d known), %lld variations, %lld handed to the workers (%lld KB of code copied), %lld pairs found, %.0f ms of the guest threads' time; pipelines made for a container's pass signatures: %lld (counted in the keys above), background jobs %lld queued / %lld done\n",
           g_cscan ? "on" : "off (SSBU_VK_CSCAN=0)", (long long)g_cs_scans, g_ncont, (long long)g_cs_vars, (long long)g_cs_jobs, (long long)(g_cs_bytes >> 10), (long long)g_cs_pairs, (double)g_cs_tk * k, (long long)g_cs_pred, (long long)g_bq_queued, (long long)g_bq_done);
}
static void m4_stop(void) {
    if (!g_pw_nthread) return;
    prewarm_wait();
    EnterCriticalSection(&g_sh_lock);
    g_m4_on = 0; g_m4_quit = 1;
    WakeAllConditionVariable(&g_jq_cv);
    LeaveCriticalSection(&g_sh_lock);
    WaitForMultipleObjects((DWORD)g_pw_nthread, g_pw_thread, TRUE, 60000);
    for (int i = 0; i < g_pw_nthread; i++) CloseHandle(g_pw_thread[i]);
    g_pw_nthread = 0;
    if (g_prep_thread) { WaitForSingleObject(g_prep_thread, 30000); CloseHandle(g_prep_thread); g_prep_thread = NULL; }
    m4_report();
}
/* ---- Session 12: WHERE the driver's pipeline cache is written. The owner's freeze (2026-10-12): the preparation thread, at
 * its end, called vkGetPipelineCacheData on the ONE live cache (0.9-1 GB; twice: size, then data) while the game was being
 * played. The driver serialises under the cache's own lock; every pipeline creation and every library link of a draw takes
 * the same lock, so the render thread stood still for as long as the serialisation took, and the serialising thread ran BELOW
 * normal priority: on a PC whose CPUs are busy it is hardly scheduled, so seconds became a minute (a priority inversion
 * through the driver's lock). Now the live cache is serialised only (1) by the render thread itself while the loading barrier
 * holds a loading screen (no draw is waiting, the thread that holds the lock is the one that would wait for it, the file is
 * written by a thread of its own), and (2) on the exit path. The preparation's marker is written with the file, not before:
 * a run that dies before either leaves no marker and the next start prepares again (record reads and cache hits).
 * One call with a buffer sized from the file on disk instead of two (the size query serialises too); the two-call form is
 * the fallback. SSBU_VK_PCSAVE=prep = the old behaviour (the preparation thread saves at once): for the comparison only. */
static char g_prep_marker[240];
static volatile LONG g_pcsave_want;                /* the preparation finished: cache + marker to be written at the next safe point */
static volatile LONG64 g_gap_prev, g_gap_max;      /* nvn_vk_draw.c vk_present2: the longest time between two frames that reached the window since g_gap_max was cleared */
static int g_prep_limited;                         /* SSBU_VK_PREP_LIMIT (test): not the whole game: no marker */
static HANDLE g_pcw_thread;
typedef struct { void *data; size_t n; int marker, where, one_call, prio; LONG64 t_get, t_q; } PcWrite;
static void pcache_write(PcWrite *w) {
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    char tmp[176];
    LONG64 t0 = tk();
    int ok = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path);
    FILE *fp = fopen(tmp, "wb");
    if (fp) { ok = fwrite(w->data, 1, w->n, fp) == w->n; fclose(fp); if (ok) ok = MoveFileExA(tmp, g_pcache_path, MOVEFILE_REPLACE_EXISTING) != 0; }
    LONG64 t1 = tk();
    if (w->where == 2) Sleep(300);                 /* the frame that was held behind the driver's lock has been presented by now */
    printf("nvn_vk: pipeline cache %s: %llu KB, %s; vkGetPipelineCacheData %.0f ms (%s), on a thread of priority %d; file written in %.0f ms%s; longest time between two presented frames while the cache was read out: %.0f ms (frame %lld, %s)\n",
           ok ? "saved" : "NOT SAVED (the file could not be written)", (unsigned long long)(w->n >> 10),
           w->where == 0 ? "on the exit path" : w->where == 1 ? "by the render thread inside a held loading screen" : "BY THE PREPARATION THREAD DURING PLAY (SSBU_VK_PCSAVE=prep: the behaviour before session 12)",
           (double)w->t_get * 1000.0 / (double)fq.QuadPart, w->one_call ? "one call" : "two calls: size, then data", w->prio, (double)(t1 - t0) * 1000.0 / (double)fq.QuadPart, w->where == 1 ? " by a thread of its own" : "",
           (double)g_gap_max * 1000.0 / (double)fq.QuadPart, (long long)nvn_front_frame_next(), g_in_fight ? "IN A FIGHT" : "no fight");
    if (ok && w->marker && !g_prep_limited && g_prep_marker[0]) {
        FILE *mf = fopen(g_prep_marker, "wb");
        if (mf) { fprintf(mf, "%d containers, %lld variations\n", g_prep_nfiles, (long long)g_prep_vars); fclose(mf); printf("nvn_vk_prep: marker %s written with the pipeline cache: the next start skips the preparation\n", g_prep_marker); }
    }
    fflush(stdout);
    free(w->data);
}
static DWORD WINAPI pcw_main(void *arg) { PcWrite *w = arg; pcache_write(w); free(w); return 0; }
/* TEST ONLY (with SSBU_VK_PCSAVE=prep): while the preparation thread reads the cache out, this thread (normal priority) makes
 * the library of a stage that already has one, again and again: a hit of the same pipeline cache, what a draw's link or a
 * worker does. Its longest call says whether the driver makes pipeline creation wait behind vkGetPipelineCacheData. */
static volatile LONG g_pcprobe_stop;
static LONG64 g_pcprobe_max, g_pcprobe_n, g_pcprobe_before;
static DWORD WINAPI pcprobe_main(void *arg) {
    VStage *src = NULL;
    (void)arg;
    EnterCriticalSection(&g_sh_lock);
    for (int i = 0; i < 4096 && !src; i++) for (VStage *v = g_vstage[i]; v; v = v->next) if (v->ok && v->stage == 1 && v->lib_state == 2 && v->mod && !v->temp) { src = v; break; }
    LeaveCriticalSection(&g_sh_lock);
    if (!src) return 0;
    for (int round = 0; !g_pcprobe_stop || round < 20; round++) {
        static VStage t;
        int how;
        t = *src; t.lib = NULL; t.lib2 = NULL; t.lib_state = 0; t.lib2_state = 0; t.temp = 1;
        LONG64 t0 = tk();
        stagelib_ensure(&t, 0, &how);
        LONG64 dt = tk() - t0;
        if (t.lib) vkDestroyPipeline(V.device, t.lib, NULL);
        if (round < 20) { if (dt > g_pcprobe_before) g_pcprobe_before = dt; if (round == 19) SetEvent((HANDLE)arg); }      /* 20 calls before the read-out begins */
        else { g_pcprobe_n++; if (dt > g_pcprobe_max) g_pcprobe_max = dt; }
        Sleep(2);
    }
    return 0;
}
/* where: 0 the exit path, 1 the render thread inside a held loading screen, 2 the preparation thread at once (old behaviour) */
static void pcache_save_at(int where, int marker) {
    size_t n = 0;
    VkResult r = VK_INCOMPLETE;
    void *data = NULL;
    int one = 0;
    if (!g_pcache) return;
    if (where == 0) {       /* session 14: the exit path rewrote the whole file (0.9 GB: 5-11 s with the window still there) even when nothing was added */
        const char *e = getenv("SSBU_VK_PCSAVE");
        WIN32_FILE_ATTRIBUTE_DATA fa0;
        LONG64 added = g_pc_dirty - g_pc_dirty_saved;
        int have_file = GetFileAttributesExA(g_pcache_path, GetFileExInfoStandard, &fa0) && (fa0.nFileSizeLow || fa0.nFileSizeHigh);
        if (have_file && !marker && added < 8 && !(e && !strcmp(e, "always"))) {      /* under 8 new entries: not worth 0.9 GB of writing (run s14prep1: 1 entry = 11 s of exit); they are made again next time, a few ms */
            printf("nvn_vk: pipeline cache NOT rewritten at exit: %lld creations were added since %s (under 8: not worth rewriting the file; SSBU_VK_PCSAVE=always writes it anyway)\n", (long long)added, g_pc_dirty_saved ? "the last save of this run" : "it was read at start");
            fflush(stdout);
            return;
        }
        printf("nvn_vk: pipeline cache: %lld creations since %s were not in it%s: written at exit\n", (long long)added, g_pc_dirty_saved ? "the last save" : "the start", marker ? " and the preparation's marker is due" : !have_file ? " and there is no file yet" : "");
    }
    g_pc_dirty_saved = g_pc_dirty;
    if (g_pcw_thread) { WaitForSingleObject(g_pcw_thread, where ? 0 : 60000); if (where && WaitForSingleObject(g_pcw_thread, 0) != WAIT_OBJECT_0) return; CloseHandle(g_pcw_thread); g_pcw_thread = NULL; }
    HANDLE probe = NULL, ready = NULL;
    if (where == 2) { ready = CreateEventA(NULL, TRUE, FALSE, NULL); g_pcprobe_stop = 0; probe = CreateThread(NULL, 0, pcprobe_main, ready, 0, NULL); WaitForSingleObject(ready, 3000); }
    g_gap_max = 0;
    LONG64 t0 = tk();
    if (where != 2) {                             /* one call: the buffer is sized from the file of the last save */
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExA(g_pcache_path, GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh)) {
            n = (size_t)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
            n += n / 8 + (64u << 20);
            if ((data = malloc(n)) != NULL) { r = vkGetPipelineCacheData(V.device, g_pcache, &n, data); if (r == VK_SUCCESS) one = 1; else { free(data); data = NULL; } }
        }
    }
    if (!data) {
        n = 0;
        if (vkGetPipelineCacheData(V.device, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
        if (!(data = malloc(n))) return;
        if (vkGetPipelineCacheData(V.device, g_pcache, &n, data) != VK_SUCCESS) { free(data); return; }
    }
    if (probe) {
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        g_pcprobe_stop = 1; WaitForSingleObject(probe, 30000); CloseHandle(probe); CloseHandle(ready);
        printf("nvn_vk: TEST probe: a stage library made again from the same cache by another thread (normal priority): longest call %.1f ms in the 20 calls BEFORE the read-out, %.1f ms in the %lld calls DURING it (read-out %.0f ms)\n",
               (double)g_pcprobe_before * 1000.0 / (double)fq.QuadPart, (double)g_pcprobe_max * 1000.0 / (double)fq.QuadPart, (long long)g_pcprobe_n, (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart);
    }
    PcWrite *w = calloc(1, sizeof *w);
    w->data = data; w->n = n; w->marker = marker; w->where = where; w->one_call = one; w->prio = GetThreadPriority(GetCurrentThread()); w->t_get = tk() - t0;
    if (where == 1 && (g_pcw_thread = CreateThread(NULL, 0, pcw_main, w, 0, NULL)) != NULL) return;
    pcache_write(w); free(w);
}
static void pcache_save(void) { pcache_save_at(0, InterlockedExchange(&g_pcsave_want, 0)); }      /* the exit path (vkd_shutdown) */

/* ---------------------------------------------------------------- session 6: the shader container scan
 * The game keeps its Maxwell stages in "BNSH" containers (a file of their own, or embedded in an effect / shader-database
 * file) and creates a program from a variation only when something first draws with it: FUN_71000382a0 (13.0.4) takes the
 * six stage-code pointers of one variation's program and calls nvnProgramInitialize / nvnProgramSetShaders; its caller
 * FUN_710009c9e0 sets a container up either eagerly (every variation at once) or lazily (flag bit 0).
 * Layout, read from that code and checked against the memory of the running game (SSBU_VK_CSCAN_LOG=1 prints it):
 *   file header: "BNSH", byte-order mark 0xFEFF at +0x0C, offset of the file name at +0x10, offset of the first block at
 *                +0x16 (u16), file size at +0x1C;
 *   block "grsc": variation count at +0x1C (s32), pointer to the variation array at +0x20;
 *   variation (0x40 bytes): three program pointers (source / intermediate / binary) at the offsets the table the game keeps
 *                at *0x71052a32d8 names; a program holds its six stage-code pointers at the offsets of the table 0x7104470aa8;
 *   stage code: among its first words a pointer to the control section (magic 0x98761234) and one to the code section
 *                (magic 0x12345678), the two blocks nvnProgramSetShaders is given.
 * The pointers are absolute once the game has relocated the file; before that they are offsets from the file start: both
 * are accepted. The scan runs on the GUEST thread that hands over the first program of a container not seen before: every
 * variation becomes a background job (stages translated -> SPIR-V -> modules, as for a program handed over), and every
 * pair found gets the pipelines of the pass signatures other pairs of the SAME container were drawn with (now or in
 * earlier runs: pipes.bin), so a container's unseen variations are ready before the game first draws with them. */
typedef struct { uint64_t base, size, id; uint8_t head[32]; int nvar; uint64_t sigs; } Cont;
static Cont g_cont[1024];
static uint64_t g_cont_neg[1024];
static uint32_t g32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static uint64_t g64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static void cont_predict(PairRec *pr, int i) {     /* g_sh_lock held */
    if ((pr->sigs >> i & 1) || !D.has_vids) return;      /* V_novids: a signature holds the vertex layout of another pair */
    pr->sigs |= 1ull << i;
    PKey k = g_sig[i].k;
    k.vs = pr->vs->id; k.fs = pr->fs ? pr->fs->id : 0; k.sshade = pr->fs ? pr->sshade : 0;
    uint64_t h = fnv(0xcbf29ce484222325ULL, &k, sizeof k);
    if (pipe_find(&k, h)) return;
    Job *j = calloc(1, sizeof *j);
    j->kind = 1; j->pipe = pipe_new(&k, pr->vs, pr->fs, h, 0); j->pipe->predicted = 1;
    g_pred_made++; g_cs_pred++;
    job_push_bg(j);
}
static void cont_learn(PairRec *pr) {              /* g_sh_lock held: a pair of a container was drawn, or was just found by the scan */
    if (!pr->cont || !g_cpredict) return;
    Cont *c = &g_cont[pr->cont - 1];
    uint64_t fresh = pr->used & ~c->sigs;
    if (fresh) {
        c->sigs |= fresh;
        for (PairRec *q = g_pair_all; q; q = q->all) if (q->cont == pr->cont) for (int i = 0; i < g_nsig; i++) if (fresh >> i & 1) cont_predict(q, i);
    }
    for (int i = 0; i < g_nsig; i++) if (c->sigs >> i & 1) cont_predict(pr, i);
}
/* The container around a control section: the nearest "BNSH" header below it whose file size reaches it; 0 when none. */
/* Session 8: *neg_lo (when the search fails) = the lowest address for which this failure also holds: no header lies at a
 * step between it and `control`, and what lies below does not reach it. */
/* `budget` (ticks, 0 = none): the search gives up after that long, as a failure. Measured without it (runs s8a..s8d): the
 * search below ONE pool of the loading's last frames, where no header is found, took 346-407 ms on the render thread
 * (VirtualQuery over an area cut into very many regions, memory not touched before) = the frame of 362-441 ms at the
 * start of the fight. A search that finds its container takes 0.0-0.6 ms, 5.4 ms for the largest container seen. */
static LONG64 g_cs_gaveup;
static uint64_t cont_find(uint64_t control, uint64_t *size, uint64_t *neg_lo, LONG64 budget) {
    uint64_t step = g_cscan_step, a = control & ~(step - 1), stop = control > (g_cscan_reach << 20) ? control - (g_cscan_reach << 20) : 0x10000;
    LONG64 t0 = tk();
    unsigned nstep = 0;
    *neg_lo = 0;
    if ((control >> 28) == 0x710) return 0;        /* inside the game's own image (0x7100000000 + 256 MB): a program built into the exe, no container; the image is thousands of small regions for VirtualQuery (40 ms a search, measured) */
    for (int regions = 0; a >= stop && regions < 256; regions++) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((const void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        uint64_t lo = (uint64_t)(uintptr_t)mi.BaseAddress;
        if (lo < stop) lo = stop;
        if (budget && tk() - t0 > budget) { InterlockedIncrement64(&g_cs_gaveup); *neg_lo = a + step; return 0; }
        for (; a >= lo; a -= step) {
            if (budget && (++nstep & 255) == 0 && tk() - t0 > budget) { InterlockedIncrement64(&g_cs_gaveup); *neg_lo = a + step; return 0; }
            if (g32(a) == 0x48534E42u && (g32(a + 0xC) & 0xffff) == 0xFEFFu) {
                uint64_t sz = g32(a + 0x1C);
                if (sz < 0x60 || control >= a + sz) { *neg_lo = a + (sz < step ? step : sz); return 0; }
                *size = sz;
                return a;
            }
        }
    }
    *neg_lo = a + step;
    return 0;
}
/* Session 8. A search that finds nothing reads up to 64 MB of the game's memory, one word per 4 KB: 5-8 ms, and 346 ms
 * once on cold memory (measured, run s8a). When the fighters enter, the game makes ten and more programs in ONE frame whose
 * variation lies in no container the search can find (frame 2029 of s8a: 10 searches of 7-8 ms = the 70 ms hitch), and the
 * old negative cache knew only the exact address. A failed search now leaves the INTERVAL it proved empty; an address in it
 * fails at once, an address up to 1 MB above it only has the steps in between looked at. The intervals are dropped when the
 * game lays a pool over a container's code (nvn_front_pool_made: new containers are announced that way) and after 600 frames. */
typedef struct { uint64_t lo, hi; long long frame; } NegIv;
static NegIv g_negiv[16];
static int g_negiv_i;
static LONG64 g_cs_neg_hit, g_cs_neg_ext, g_cs_search;
static int cont_range_empty(uint64_t above, uint64_t control) {      /* 1: readable and no header at a step in (above, control] */
    uint64_t step = g_cscan_step, a = control & ~(step - 1);
    while (a > above) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((const void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return 0;
        uint64_t lo = (uint64_t)(uintptr_t)mi.BaseAddress;
        for (; a > above && a >= lo; a -= step) if (g32(a) == 0x48534E42u) return 0;
    }
    return 1;
}
static int cont_stage(uint64_t control, uint64_t data, SSrc *o) {      /* as nvnProgramSetShaders reads a stage (nvn_gl.c prog_set_shaders): 0 vertex, 1 fragment, 2 compute, -1 none, -2 a stage this back end does not take */
    static const int8_t stage_of_type[16] = {-1, 0, 3, 4, 2, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    uint32_t ctl[8];
    memset(o, 0, sizeof *o);
    int len = (int)g32(control + 0x6f8);
    if (len <= 0 || len > (4 << 20)) return -1;
    memcpy(ctl, (const void *)(uintptr_t)(control + 0x6f0), 32);
    if (g32(control + 0x714) == 5) {
        o->stage = 5; o->code = (const uint8_t *)(uintptr_t)data; o->code_len = len;
        for (int k = 0; k < 3; k++) o->local[k] = g32(control + 0x748 + (uint64_t)k * 4);
        o->smem = g32(control + 0x754); o->lmem = g32(control + 0x758);
        if (ctl[3] && ctl[4]) { o->cdata = o->code + ctl[4]; o->cdata_size = ctl[3]; }
        return 2;
    }
    if (g32(data) != 0x12345678u) return -1;
    int s = stage_of_type[(g32(data + 0x30) >> 10) & 15];
    if (s < 0) return -1;
    if (s > 1) return -2;
    o->stage = s; o->code = (const uint8_t *)(uintptr_t)(data + 0x30); o->code_len = len;
    if (ctl[3] && ctl[4]) { o->cdata = o->code - 0x30 + ctl[4]; o->cdata_size = ctl[3]; }
    return s;
}
/* Walks the container at base (size bytes, all readable): one background job per variation. Returns the variation count;
 * `tag` = 1 + the container's index, for the pairs. */
/* obase (session 7): the container is a COPY at `base` of what lies at obase in the game's memory (walked on a worker, not
 * on the game's thread): a pointer into the original is moved into the copy. 0 = walked in place / a file. */
#define CW_RB(p) ((obase && (p) >= obase && (p) < obase + size) ? (p) - obase + base : (p))
#include "hostrt.h"                               /* GA(): the game's offset tables below are named by their 13.0.4 address */
static int cont_walk(uint64_t base, uint64_t size, int tag, int *njobs, uint64_t obase) {
    uint64_t end = base + size, blk = base + (g32(base + 0x14) >> 16), voff[3] = {0, 8, 0x10}, soff[6] = {8, 0x10, 0x18, 0x20, 0x28, 0x30};
    *njobs = 0;
    if (blk + 0x28 > end || g32(blk) != 0x63737267u) return -1;      /* "grsc" */
    int nvar = (int)g32(blk + 0x1C);
    uint64_t vars = g64(blk + 0x20);
    vars = CW_RB(vars);
    if (vars < size) vars += base;                 /* not relocated yet */
    if (nvar <= 0 || nvar > 200000 || vars < base || vars + (uint64_t)nvar * 0x40 > end) return -1;
    if (g_game_tables) {                           /* the game's own offset tables (13.0.4); the defaults above are what they held when read */
        uint64_t t = g64(GA(0x71052a32d8ull));
        for (int i = 0; i < 3 && t; i++) { uint64_t v = g64(t + (uint64_t)i * 8); if (v < 0x40) voff[i] = v; }
        for (int i = 0; i < 6; i++) { uint64_t v = g64(GA(0x7104470aa8ull) + (uint64_t)i * 8); if (v && v < 0x70) soff[i] = v; }
    }
    for (int v = 0; v < nvar; v++) {
        SSrc s[3];
        int has[3] = {0, 0, 0}, bad = 0;
        for (int pi = 2; pi >= 0 && !has[0] && !has[2]; pi--) {      /* binary program first */
            uint64_t prog = g64(vars + (uint64_t)v * 0x40 + voff[pi]);
            prog = CW_RB(prog);
            if (prog && prog < size) prog += base;
            if (prog < base || prog + 0x70 > end) continue;
            for (int st = 0; st < 6; st++) {
                uint64_t code = g64(prog + soff[st]), control = 0, data = 0;
                code = CW_RB(code);
                if (code && code < size) code += base;
                if (code < base || code + 0x40 > end) continue;
                /* stage code: {u64 GPU address (the game fills it), control pointer, code pointer, u32 code size, u32 control size} */
                control = g64(code + 8); data = g64(code + 16);
                control = CW_RB(control); data = CW_RB(data);
                if (control && control < size) control += base;
                if (data && data < size) data += base;
                if (control < base || control + 0x760 > end || g32(control) != 0x98761234u) control = 0;
                if (data < base || data + 0x40 > end) data = 0;
                if (g_cscan_log && v == 0) { printf("nvn_vk_cscan:   variation 0 program %d stage slot %d: code struct at +%llx: %016llx %016llx %016llx %016llx | control +%llx, data +%llx\n", pi, st, (unsigned long long)(code - base),
                                                    (unsigned long long)g64(code), (unsigned long long)g64(code + 8), (unsigned long long)g64(code + 16), (unsigned long long)g64(code + 24), (unsigned long long)(control ? control - base : 0), (unsigned long long)(data ? data - base : 0)); }
                if (!control || !data) continue;
                SSrc one;
                int k = cont_stage(control, data, &one);
                if (k == -2) bad = 1;
                if (k < 0) continue;
                if ((uint64_t)(uintptr_t)one.code + (uint64_t)one.code_len > end || (one.cdata && ((uint64_t)(uintptr_t)one.cdata < base || (uint64_t)(uintptr_t)one.cdata + one.cdata_size > end))) continue;
                one.serial = -(v + 1);
                s[k] = one; has[k] = 1;
            }
        }
        if (bad || (!has[0] && !has[2])) continue;
        if (tag == -3) {                           /* session 10: the index only (which code belongs to which container file) */
            uint64_t hk[3] = {0, 0, 0};
            for (int i = 0; i < 3; i++) if (has[i]) { hk[i] = ssrc_hb(&s[i]); if (i < 2 && (!g_pix_lo || (uint64_t)(uintptr_t)s[i].code < g_pix_lo)) g_pix_lo = (uint64_t)(uintptr_t)s[i].code; }
            pix_add(fnv(0xcbf29ce484222325ULL, hk, sizeof hk), g_pix_cur);      /* the key is the VARIATION (all its stages): single stage codes are shared by many containers (first attempt, run s10n3: one program announced a container, whose variations announced others: 40 files in four frames) */
            (*njobs)++;
            continue;
        }
        size_t need = 0;
        for (int i = 0; i < 3; i++) if (has[i]) need += (size_t)s[i].code_len + s[i].cdata_size + 16;
        Job *j = calloc(1, sizeof *j + need);
        uint8_t *d = j->data;
        j->cont = tag < 0 ? 0 : tag;               /* tag -1 / -2 (session 10): a container of the preparation, read from the game's data: no pair bookkeeping, its own queue (-1 made into memory, -2 into the cache only); -4: a container file announced from the index (background queue, as the scan's) */
        j->prep = tag == -4 ? 3 : tag < 0 ? -tag : 0;
        for (int i = 0; i < 3; i++) if (has[i]) job_copy_src(j, &d, i, &s[i]);
        if (tag >= 0) InterlockedAdd64(&g_cs_bytes, (LONG64)need);
        EnterCriticalSection(&g_sh_lock);
        if (holdlog_on() && !g_offline) for (int i = 0; i < 2; i++) if (has[i]) ho_note(ssrc_hb(&s[i]), tag < 0 ? 2 : 1);
        if (tag < 0 && tag != -4) job_push_prep(j); else job_push_bg(j);
        LeaveCriticalSection(&g_sh_lock);
        (*njobs)++;
    }
    return nvar;
}
/* GUEST thread, nvnProgramSetShaders (front end), once per stage handed over: `control` = the guest address of its control
 * section. A container not met before is scanned whole. */
void nvn_vk_container_seen(uint64_t control, uint64_t prog) {
    if (!g_cscan || !g_m4_on || !control || !prog) return;
    if (g_cscan_log) { static LONG nl; if (InterlockedIncrement(&nl) <= 6) { printf("nvn_vk_cscan: control %llx, variation program %llx:", (unsigned long long)control, (unsigned long long)prog); for (int i = 0; i < 16; i++) printf(" %llx", (unsigned long long)g64(prog + (uint64_t)i * 8)); printf("\n"); } }
    int from_pool = control == prog;               /* nvn_front_pool_made: a container's code section was given a pool */
    control = prog;                                /* the address the container is looked for around */
    if (from_pool && g_pix_ready) {                /* session 10: the pool's first code, looked up in the index of the game's containers: no header search needed */
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery((const void *)(uintptr_t)control, &mi, sizeof mi) && mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && control + 0x30 + 1024 <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize) {
            uint64_t k = fnv(0xcbf29ce484222325ULL, (const void *)(uintptr_t)(control + 0x30), 1024);
            int hit = 0;
            for (int f = 0; f < 1024; f++) if (g_pix_pool[f] == k && k) {
                hit = 1;
                if (g_pix_announced[f]) continue;
                Job *j = calloc(1, sizeof *j);
                j->kind = 8; j->cont = f;
                EnterCriticalSection(&g_sh_lock);
                job_push(j);
                LeaveCriticalSection(&g_sh_lock);
            }
            InterlockedIncrement64(hit ? &g_pix_pool_hit : &g_pix_pool_miss);
            if (hit) return;
            {   /* session 11: which pools the index does not know (first 12) */
                static LONG nl;
                if (InterlockedIncrement(&nl) <= 12) {
                    const uint8_t *b = (const uint8_t *)(uintptr_t)control;
                    printf("nvn_vk_prep: pool at %llx NOT in the index (frame %lld, %s): region of %llu KB from %llx; first words %02x%02x%02x%02x %02x%02x%02x%02x, at +0x30: %02x%02x%02x%02x %02x%02x%02x%02x\n", (unsigned long long)control, (long long)nvn_front_frame_next(), g_in_fight ? "IN A FIGHT" : "no fight",
                           (unsigned long long)(mi.RegionSize >> 10), (unsigned long long)(uintptr_t)mi.BaseAddress, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[0x30], b[0x31], b[0x32], b[0x33], b[0x34], b[0x35], b[0x36], b[0x37]);
                }
            }
        }
    }
    unsigned ns = (unsigned)((control * 0x9E3779B97F4A7C15ull) >> 54);
    static int negiv_on = -1;
    if (negiv_on < 0) negiv_on = !(getenv("SSBU_VK_CSCAN_NEGIV") && !atoi(getenv("SSBU_VK_CSCAN_NEGIV")));      /* =0: every address searched, as before session 8 */
    long long frame = nvn_front_frame_next();
    int ext = -1;
    uint64_t ext_hi = 0;
    /* PR #29 (the fault fix only): a remembered container whose range holds `control` is still there when its first 32 bytes
     * are. The game may have given that memory back since (nn::os::FreeMemoryPages): the plain memcmp of its head then
     * faulted on a game thread (TaskWorker2, after about 10 minutes of play). The head is now read with ReadProcessMemory
     * (memory given back = a failed call, not a fault; as sweep_main) and OUTSIDE g_sh_lock: no VirtualQuery and no system
     * call while the workers and the render thread may be waiting on the lock. An entry is dropped only if it was not changed
     * meanwhile. */
    for (int from = 0;;) {
        int hit = -1;
        uint64_t cbase = 0, csize = 0;
        uint8_t head[32], now[32];
        SIZE_T got = 0;
        EnterCriticalSection(&g_sh_lock);
        for (int i = from; i < g_ncont; i++) if (control - g_cont[i].base < g_cont[i].size) { hit = i; cbase = g_cont[i].base; csize = g_cont[i].size; memcpy(head, g_cont[i].head, 32); break; }
        LeaveCriticalSection(&g_sh_lock);
        if (hit < 0) break;
        if (ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)cbase, now, 32, &got) && got == 32 && !memcmp(head, now, 32)) return;
        EnterCriticalSection(&g_sh_lock);
        if (g_cont[hit].base == cbase && g_cont[hit].size == csize && !memcmp(g_cont[hit].head, head, 32)) g_cont[hit].size = 0;      /* the memory holds something else now, or was given back */
        LeaveCriticalSection(&g_sh_lock);
        from = hit + 1;
    }
    EnterCriticalSection(&g_sh_lock);
    if (g_cont_neg[ns] == control) { LeaveCriticalSection(&g_sh_lock); return; }
    /* Session 10: near a fight a program handed over is NOT searched for its container when the index of the game's
     * containers exists. PINNED (runs s10ip Intel, s10n5 NVIDIA): the main thread's 100-300 ms inside nvnProgramSetShadersExt
     * in fight frames were this search (27-272 ms a call, "search" part of the nvn_setsh line; its 10 ms budget is only
     * looked at every 256 steps and a step can be a page fault on memory the system had trimmed). The index names the
     * container from the program's variation instead (job_prog), and its stages are in the cache from the preparation. */
    if (!from_pool && g_pix_ready && fight_recent()) { g_cs_skipped++; LeaveCriticalSection(&g_sh_lock); return; }
    if (from_pool) memset(g_negiv, 0, sizeof g_negiv);
    else if (negiv_on) for (int i = 0; i < 16; i++) {
        NegIv *v = &g_negiv[i];
        if (!v->hi || frame - v->frame >= 600) continue;
        if (control >= v->lo && control <= v->hi) { g_cs_neg_hit++; LeaveCriticalSection(&g_sh_lock); return; }
        if (control > v->hi && control - v->hi <= (1u << 20)) { ext = i; ext_hi = v->hi; }
    }
    LeaveCriticalSection(&g_sh_lock);
    LONG64 t0 = tk();
    if (ext >= 0 && cont_range_empty(ext_hi, control)) {      /* nothing new between the proved interval and this address: the same failure */
        EnterCriticalSection(&g_sh_lock);
        if (g_negiv[ext].hi == ext_hi) g_negiv[ext].hi = control;
        g_cs_neg_ext++;
        LeaveCriticalSection(&g_sh_lock);
        nvn_setsh_part[2] += tk() - t0; InterlockedAdd64(&g_cs_tk, tk() - t0);
        return;
    }
    uint64_t size = 0, neg_lo = 0, base = 0;
    {   /* the budget: 10 ms; 20 ms for a pool outside a fight (loading). SSBU_VK_CSCAN_BUDGET=<ms> (0 = none, as before session 8) */
        static int bms = -1; static LONG64 fqq;
        if (bms < 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fqq = f.QuadPart; bms = getenv("SSBU_VK_CSCAN_BUDGET") ? atoi(getenv("SSBU_VK_CSCAN_BUDGET")) : 10; }
        base = cont_find(control, &size, &neg_lo, (LONG64)bms * (from_pool && !fight_recent() ? 2 : 1) * fqq / 1000);
    }
    InterlockedIncrement64(&g_cs_search);
    nvn_setsh_part[2] += tk() - t0;
    if (g_cscan_log && (control >> 28) != 0x710) {
        static LONG nl, nl2;
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double ms = (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart;
        if (InterlockedIncrement(&nl) <= 40 || (ms > 2.0 && InterlockedIncrement(&nl2) <= 200)) { printf("nvn_vk_cscan: frame %lld: control %llx -> container %llx (%llu KB), %.1f ms%s\n", frame, (unsigned long long)control, (unsigned long long)base, (unsigned long long)(size >> 10), ms, from_pool ? " (pool)" : ""); fflush(stdout); }
    }
    if (!base) {
        EnterCriticalSection(&g_sh_lock);
        g_cont_neg[ns] = control;
        if (neg_lo && !from_pool) { NegIv *v = &g_negiv[g_negiv_i++ & 15]; v->lo = neg_lo < control ? neg_lo : control; v->hi = control; v->frame = frame; }
        LeaveCriticalSection(&g_sh_lock);
        InterlockedAdd64(&g_cs_tk, tk() - t0);
        return;
    }
    uint64_t id = fnv(0xcbf29ce484222325ULL, (const void *)(uintptr_t)base, 32);
    {   uint32_t no = g32(base + 0x10);            /* the file name */
        if (no && no + 2 < size) id = fnv(id, (const void *)(uintptr_t)(base + no), strnlen((const char *)(uintptr_t)(base + no), (size_t)(size - no) < 96 ? (size_t)(size - no) : 96)); }
    int idx = -1, known = 0;
    EnterCriticalSection(&g_sh_lock);
    for (int i = 0; i < g_ncont; i++) if (g_cont[i].id == id) { idx = i; known = g_cont[i].size != 0 || g_cont[i].nvar > 0; if (g_cont[i].base == base && g_cont[i].size) { LeaveCriticalSection(&g_sh_lock); return; } }
    if (idx < 0 && g_ncont < 1024) { idx = g_ncont++; memset(&g_cont[idx], 0, sizeof g_cont[idx]); g_cont[idx].id = id; }
    if (idx >= 0) { g_cont[idx].base = base; g_cont[idx].size = size; memcpy(g_cont[idx].head, (const void *)(uintptr_t)base, 32); }
    LeaveCriticalSection(&g_sh_lock);
    if (idx < 0 || known) return;                  /* the same file loaded again at another address: its pairs are known */
    if (!(getenv("SSBU_VK_CSCAN_INLINE") && atoi(getenv("SSBU_VK_CSCAN_INLINE")))) {
        /* Session 7: the walk (hashing and copying every variation: 20-70 ms for a fighter's effect container, measured as
         * "main: graphics call: nvnProgramSetShadersExt" in the frames where the fighters enter, runs s7fn2 / s7fi2 / s7gn2)
         * is done by a worker on a COPY of the container; the game's thread only pays the copy. SSBU_VK_CSCAN_INLINE=1 = as before. */
        Job *j = calloc(1, sizeof *j);
        void *copy = malloc((size_t)size);
        if (j && copy) {
            LONG64 tc = tk();
            memcpy(copy, (const void *)(uintptr_t)base, (size_t)size);
            nvn_setsh_part[3] += tk() - tc;
            j->kind = 4; j->cont = idx + 1; j->cbase = (uint64_t)(uintptr_t)copy; j->csize = size; j->cobase = base;
            g_cont[idx].nvar = 1;
            InterlockedIncrement64(&g_cs_scans); InterlockedAdd64(&g_cs_tk, tk() - t0);
            EnterCriticalSection(&g_sh_lock);
            job_push(j);
            LeaveCriticalSection(&g_sh_lock);
            return;
        }
        free(j); free(copy);
    }
    int njobs = 0, nvar = cont_walk(base, size, idx + 1, &njobs, 0);
    g_cont[idx].nvar = nvar > 0 ? nvar : 1;
    InterlockedIncrement64(&g_cs_scans); InterlockedAdd64(&g_cs_vars, nvar > 0 ? nvar : 0); InterlockedAdd64(&g_cs_jobs, njobs); InterlockedAdd64(&g_cs_tk, tk() - t0);
    if (g_cscan_log || nvar < 0) {
        uint32_t no = g32(base + 0x10);
        char name[64] = "";
        if (no && no + 2 < size) snprintf(name, sizeof name, "%.60s", (const char *)(uintptr_t)(base + no));
        printf("nvn_vk_cscan: container #%d \"%s\" at %llx, %llu KB, %d variations, %d queued (frame %lld, %s)%s\n", idx, name, (unsigned long long)base, (unsigned long long)(size >> 10), nvar, njobs, nvn_front_frame_next(),
               g_in_fight ? "IN A FIGHT" : "no fight", nvar < 0 ? ": NOT UNDERSTOOD (no grsc block or variation array out of the file)" : "");
        fflush(stdout);
    }
}
/* ---------------------------------------------------------------- Session 10: the PREPARATION, in the game, in the background
 * Whether a stage is ready must not depend on WHEN the game hands its container over (session 9: every hold left in a fight
 * came from one container handed over about 300 frames into the fight). With a library per stage (SSBU_VK_GPL=2) a stage
 * needs no partner and no pass, so every stage of every container of the game's data can be made ahead: this thread reads the
 * containers (the folder tools_vk/bnsh_extract.py writes from data.arc: ..\cache\vk_containers, or SSBU_VK_CONTAINERS), largest
 * first (the largest is the effect container every fight of the test hands over at "GO"), and queues each variation on g_pq.
 * The workers make stage + module + library exactly as for a program the game hands over, keyed by the same code hash, so
 * when the game does hand the container over every stage is found made. Workers on this queue run below normal priority
 * (lowest, and at most g_prep_fight_cap of them, in a fight); the game's own work always goes first; nobody waits for it.
 * At the end the driver's pipeline cache is written, so a later start pays file reads and cache hits only.
 * SSBU_VK_PREP=0 off, =1 on also without libraries (stages only); SSBU_VK_PREP_FIGHT=<n> workers in a fight (0 = pause). */
#include <psapi.h>
static int g_prep_nfiles, g_prep_read, g_prep_bad;
static LONG64 g_prep_t0, g_prep_t1, g_prep_vars, g_prep_fight_frames;
static HANDLE g_prep_thread;
static char g_prep_marker[240];
static int g_prep_marked;
static int g_prep_loadall;                          /* shader_load = start on a marked start: every container into memory before the first frame */
static void pcache_save(void);
static volatile int g_store_pct;
#include "nvn_vk_store.c"                          /* session 11: the container store made by the host from data.arc */
static int prepfile_cmp(const void *a, const void *b) { const PrepFile *x = a, *y = b; return x->size < y->size ? 1 : x->size > y->size ? -1 : strcmp(x->name, y->name); }
static void prep_line(const char *what) {
    PROCESS_MEMORY_COUNTERS_EX pm;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    long long made = 0, libs = 0;
    memset(&pm, 0, sizeof pm); pm.cb = sizeof pm;
    K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pm, sizeof pm);
    EnterCriticalSection(&g_sh_lock);
    for (int i = 0; i < 4096; i++) for (VStage *v = g_vstage[i]; v; v = v->next) { if (v->ok) made++; if (v->lib) libs++; if (v->lib2) libs++; }
    long long q = (long long)g_pq_queued, d = (long long)g_pq_done;
    LeaveCriticalSection(&g_sh_lock);
    printf("nvn_vk_prep: %s: %d of %d containers read, %lld variations, %lld of %lld done by the workers, %.1f s after its start (frame %lld, %s); stage variants in memory %lld, stage libraries %lld; process memory: %llu MB private, %llu MB working set\n",
           what, g_prep_read, g_prep_nfiles, (long long)g_prep_vars, d, q, (double)((g_prep_t1 ? g_prep_t1 : tk()) - g_prep_t0) / (double)fq.QuadPart, (long long)nvn_front_frame_next(), g_in_fight ? "IN A FIGHT" : "no fight",
           made, libs, (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(pm.WorkingSetSize >> 20));
    fflush(stdout); { void display_title_note(const char *); char tn[200]; if (nvn_front_frame_next() <= 2 && g_prep_nfiles && g_prep_read < g_prep_nfiles) { snprintf(tn, sizeof tn, "first start: preparing the shaders, %d of %d (once; can take 10-30 min on a laptop GPU, the window stays black)", g_prep_read, g_prep_nfiles); display_title_note(tn); } else display_title_note(NULL); }      /* (issue #57 / testers: a black window for minutes) */
}
static int prep_list(const char *dir) {
    char pat[300];
    WIN32_FIND_DATAA fd;
    g_prep_nfiles = 0;
    snprintf(pat, sizeof pat, "%s/*.bnsh", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do { if (g_prep_nfiles < 1024 && strlen(fd.cFileName) < 40) { strcpy(g_prep_file[g_prep_nfiles].name, fd.cFileName); g_prep_file[g_prep_nfiles++].size = fd.nFileSizeLow; } } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return g_prep_nfiles;
}
static DWORD WINAPI prep_main(void *arg) {
    /* session 11: the default store is the one the HOST writes (nvn_vk_store.c: 393 containers of effect / container files
     * + the layout archives' ones); the folder of the Python tool (..\cache\vk_containers) is used only when named. */
    const char *dir = getenv("SSBU_VK_CONTAINERS") ? getenv("SSBU_VK_CONTAINERS") : "../cache/vk_containers2";
    char path[400];
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    (void)arg;
    /* Session 12: the store belongs to ONE data.arc. `store_source.txt` in the store's folder holds the archive's size and
     * modification time; an archive that differs (a game update, another dump) makes the store stale: its *.bnsh files and
     * the preparation's marker are removed and the store is built again. A store without the file (made before session 12)
     * is taken as belonging to the archive that is there now and gets the file. */
    char stamp[96] = "", spath[400];
    int stale = 0;
    {   const char *rom = getenv("SSBU_ROMFS");
        char arc[400];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        snprintf(arc, sizeof arc, "%s\\data.arc", rom && rom[0] ? rom : "..\\romfs");
        if (GetFileAttributesExA(arc, GetFileExInfoStandard, &fa)) snprintf(stamp, sizeof stamp, "data.arc size %llu time %08lx%08lx\n", (unsigned long long)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow), (unsigned long)fa.ftLastWriteTime.dwHighDateTime, (unsigned long)fa.ftLastWriteTime.dwLowDateTime);
        snprintf(spath, sizeof spath, "%s/store_source.txt", dir);
        FILE *sf = fopen(spath, "rb");
        char was[96] = "";
        if (sf) { if (!fgets(was, sizeof was, sf)) was[0] = 0; fclose(sf); }
        if (stamp[0] && was[0] && strcmp(stamp, was)) {
            stale = 1;
            printf("nvn_vk_store: STALE: the container store in %s was made from another data.arc (%.60s now: %.60s): its files and the preparation's marker are removed, the store is built again\n", dir, was, stamp);
            if (prep_list(dir)) for (int i = 0; i < g_prep_nfiles; i++) { snprintf(path, sizeof path, "%s/%s", dir, g_prep_file[i].name); DeleteFileA(path); }
            DeleteFileA(g_prep_marker); g_prep_marked = 0;
            DeleteFileA(spath);
        }
    }
    snprintf(path, sizeof path, "%s/store_building.txt", dir);
    if (!prep_list(dir) || GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        g_prep_state = 5;                          /* the store is being built: the progress overlay says so */
        vks_build(dir);
        prep_list(dir);
        stale = 1;
    }
    if (stamp[0] && g_prep_nfiles && (stale ||GetFileAttributesA(spath) == INVALID_FILE_ATTRIBUTES)) { FILE *sf = fopen(spath, "wb"); if (sf) { fputs(stamp, sf); fclose(sf); } }
    if (!g_prep_nfiles) {
        g_prep_state = 3;
        printf("nvn_vk_prep: NO PREPARATION: no *.bnsh file in %s and none could be made from data.arc. A container the game hands over inside a fight will be translated and compiled at its draws.\n", dir);
        return 0;
    }
    g_prep_state = 0;
    qsort(g_prep_file, (size_t)g_prep_nfiles, sizeof g_prep_file[0], prepfile_cmp);
    snprintf(g_prep_dir, sizeof g_prep_dir, "%s", dir);
    {   /* the index: every stage code of every file -> its file (hashing only) */
        LONG64 ti = tk();
        long long bytes = 0;
        for (int i = 0; i < g_prep_nfiles && !g_m4_quit; i++) {
            snprintf(path, sizeof path, "%s/%s", dir, g_prep_file[i].name);
            FILE *fp = fopen(path, "rb");
            if (!fp) continue;
            size_t n = g_prep_file[i].size;
            uint8_t *raw = n >= 0x60 ? malloc(n + 128) : NULL, *buf = raw ? raw + 16 : NULL;
            if (buf && fread(buf, 1, n, fp) == n && g32((uint64_t)(uintptr_t)buf) == 0x48534E42u) {
                int nj = 0;
                g_pix_cur = i; g_pix_lo = 0;
                cont_walk((uint64_t)(uintptr_t)buf, (uint64_t)n, -3, &nj, 0);
                if (g_pix_lo && g_pix_lo + 1024 <= (uint64_t)(uintptr_t)buf + n) g_pix_pool[i] = fnv(0xcbf29ce484222325ULL, (const void *)(uintptr_t)g_pix_lo, 1024);
                bytes += (long long)n;
                {   uint32_t no = g32((uint64_t)(uintptr_t)buf + 0x10);
                    char nm[48] = "";
                    if (no && no + 40 < n) snprintf(nm, sizeof nm, "%.40s", (const char *)buf + no);
                    g_prep_file[i].layout = strstr(nm, "ArchiveShader") != NULL;
                }
            }
            free(raw);
            fclose(fp);
        }
        for (int i = 0; i < g_prep_keep && i < 1024; i++) g_pix_announced[i] = 1;      /* made into memory by this thread anyway */
        g_pix_ready = 1;
        printf("nvn_vk_prep: index of the game's containers ready %.1f s after the start of the preparation (frame %lld): %d files, %lld MB, %lld (variation, file) entries, %lld of them a variation that another file holds too\n", (double)(tk() - ti) / (double)fq.QuadPart, (long long)nvn_front_frame_next(), g_prep_nfiles, bytes >> 20, (long long)g_pix_n, (long long)g_pix_shared);
        fflush(stdout);
    }
    int todo = g_prep_marked && g_prep_nfiles > g_prep_keep ? g_prep_keep : g_prep_nfiles;      /* marked: an earlier run prepared everything; only the containers kept in memory */
    if (getenv("SSBU_VK_PREP_LIMIT") && atoi(getenv("SSBU_VK_PREP_LIMIT")) > 0 && atoi(getenv("SSBU_VK_PREP_LIMIT")) < todo) { todo = atoi(getenv("SSBU_VK_PREP_LIMIT")); g_prep_limited = 1; printf("nvn_vk_prep: TEST: only the first %d containers are prepared (SSBU_VK_PREP_LIMIT): no marker will be written\n", todo); }
    SetThreadPriority(GetCurrentThread(), g_prep_loadall ? THREAD_PRIORITY_NORMAL : THREAD_PRIORITY_BELOW_NORMAL);
    if (g_prep_loadall) { g_prep_urgent = 1; WakeAllConditionVariable(&g_jq_cv); }    /* the start: no fight, every worker at normal priority */
    g_prep_t0 = tk();
    g_prep_state = 1;
    prep_line("start");
    LONG64 last = tk();
    /* session 11, the ORDER: (1) the files whose pool an earlier run saw laid INSIDE a fight (late_containers.txt), (2) the
     * layout archives' containers, smallest first (124 files, 9.6 MB: 12 of the 13 pools laid inside the first fight of run
     * s11n1 were of this kind: the fight's own screen layouts), (3) the others, largest first as before. Containers kept in
     * memory (SSBU_VK_PREP_KEEP, off) keep their old place: the first g_prep_keep of the size order. */
    static int ord[1024];
    int no = 0, nlate = 0, nlay = 0;
    {   FILE *lf = fopen(vkc_file("late_containers.txt"), "rb");
        char ln[80];
        while (lf && fgets(ln, sizeof ln, lf)) { ln[strcspn(ln, "\r\n")] = 0; for (int i = 0; i < g_prep_nfiles; i++) if (!strcmp(ln, g_prep_file[i].name)) g_prep_file[i].late = 1; }
        if (lf) fclose(lf);
    }
    for (int i = 0; i < g_prep_keep && i < g_prep_nfiles; i++) ord[no++] = i;
    for (int i = g_prep_keep > 0 ? g_prep_keep : 0; i < g_prep_nfiles; i++) if (g_prep_file[i].late) { ord[no++] = i; nlate++; }
    for (int i = g_prep_nfiles - 1; i >= (g_prep_keep > 0 ? g_prep_keep : 0); i--) if (!g_prep_file[i].late && g_prep_file[i].layout) { ord[no++] = i; nlay++; }
    for (int i = g_prep_keep > 0 ? g_prep_keep : 0; i < g_prep_nfiles; i++) if (!g_prep_file[i].late && !g_prep_file[i].layout) ord[no++] = i;
    printf("nvn_vk_prep: order: %d file(s) whose pool an earlier run saw laid inside a fight, then %d layout-archive containers (smallest first), then %d others (largest first)\n", nlate, nlay, no - nlate - nlay);
    for (int oi = 0; oi < todo && !g_m4_quit; oi++) {
        int i = ord[oi];
        snprintf(path, sizeof path, "%s/%s", dir, g_prep_file[i].name);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        size_t n = g_prep_file[i].size;
        uint8_t *raw = n >= 0x60 ? malloc(n + 128) : NULL, *buf = raw ? raw + 16 : NULL;
        if (buf && fread(buf, 1, n, fp) == n && g32((uint64_t)(uintptr_t)buf) == 0x48534E42u) {
            int nj = 0, nv = cont_walk((uint64_t)(uintptr_t)buf, (uint64_t)n, i < g_prep_keep ? -1 : -2, &nj, 0);
            if (nv < 0) g_prep_bad++; else g_prep_vars += nv;
        }
        free(raw);                                  /* the jobs hold their own copy of the code */
        fclose(fp);
        g_prep_read = oi + 1;
        if (i == g_prep_keep - 1) {                /* the containers made into memory alone, so that their end is in the log */
            EnterCriticalSection(&g_sh_lock);
            while (g_pq_done < g_pq_queued && !g_m4_quit) SleepConditionVariableCS(&g_sh_cv, &g_sh_lock, 100);
            LeaveCriticalSection(&g_sh_lock);
            prep_line("the containers made into memory (the largest first) are done");
        }
        EnterCriticalSection(&g_sh_lock);          /* not more than 600 copies of code waiting */
        while (g_pq_queued - g_pq_done > 600 && !g_m4_quit) {
            SleepConditionVariableCS(&g_sh_cv, &g_sh_lock, 200);
            if (tk() - last > 10 * fq.QuadPart) { last = tk(); LeaveCriticalSection(&g_sh_lock); prep_line("progress"); EnterCriticalSection(&g_sh_lock); }
        }
        LeaveCriticalSection(&g_sh_lock);
    }
    EnterCriticalSection(&g_sh_lock);
    while (g_pq_done < g_pq_queued && !g_m4_quit) {
        SleepConditionVariableCS(&g_sh_cv, &g_sh_lock, 200);
        if (tk() - last > 10 * fq.QuadPart) { last = tk(); LeaveCriticalSection(&g_sh_lock); prep_line("progress"); EnterCriticalSection(&g_sh_lock); }
    }
    int whole = g_pq_done == g_pq_queued && g_prep_read == todo;
    LeaveCriticalSection(&g_sh_lock);
    {   /* TEST ONLY, SSBU_VK_PREP_END_AT=<frame>: the end below is held back until that game frame (to place it in a fight or a loading screen) */
        LONG64 at = getenv("SSBU_VK_PREP_END_AT") ? atoll(getenv("SSBU_VK_PREP_END_AT")) : 0;
        while (whole && at > 0 && nvn_front_frame_next() < at && !g_m4_quit) Sleep(5);
        while (whole && at < 0 && g_battle_frames < -at && !g_m4_quit) Sleep(1);      /* a negative value: that many battle frames after the first one */
        if (g_m4_quit && at) whole = 0;
    }
    g_prep_t1 = tk();
    printf("nvn_vk_prep: into the cache only: %lld stage variants (%lld read from a record of an earlier run, the others translated now), %lld stage libraries compiled and released, %lld met again in a later variation, %lld left to the game's own path (already handed over); worker time: %.0f s stages, %.0f s libraries; at most %d workers at a time (%d in a fight)\n",
           (long long)g_prep_stages, (long long)g_prep_fromrec, (long long)g_prep_libs, (long long)g_prep_dup, (long long)g_prep_had, (double)g_prep_tr_tk / (double)fq.QuadPart, (double)g_prep_lib_tk / (double)fq.QuadPart, g_prep_cap, g_prep_fight_cap);
    if (g_prep_loadall) g_prep_urgent = 0;
    if (whole && g_prep_marked) { g_prep_state = 2; prep_line(g_prep_loadall ? "FINISHED: every container of the game's data is in memory (shader_load = start)" : "FINISHED (the containers kept in memory; the rest was prepared by an earlier run)"); }
    else if (whole) {
        g_prep_state = 2;
        prep_line("FINISHED: every stage of every container of the game's data is made");
        /* session 12: NOT saved here any more (see pcache_save_at: this thread serialising the live cache during play was the
         * owner's freeze). The render thread writes cache and marker inside the next held loading screen, or the exit path does. */
        if (getenv("SSBU_VK_PCSAVE") && !strcmp(getenv("SSBU_VK_PCSAVE"), "prep")) pcache_save_at(2, 1);
        else InterlockedExchange(&g_pcsave_want, 1);
        printf("nvn_vk_prep: %d containers not understood; frames of a fight that passed while the preparation ran: %lld; %s\n", g_prep_bad, (long long)g_prep_fight_frames,
               g_pcsave_want ? "the pipeline cache and the marker are written inside the next held loading screen or at exit (not by this thread during play)" : "pipeline cache written by this thread at once (SSBU_VK_PCSAVE=prep)");
        fflush(stdout);
    } else prep_line("STOPPED before its end (the game is closing)");
    return 0;
}
/* SSBU_VK_SWEEP_LOG=1 (a measurement, off by default): WHEN do a container's bytes arrive in the game's memory? Every 2 s a
 * thread looks at the first words of every committed, writable 4 KB step of the process (ReadProcessMemory: memory given
 * back meanwhile is a failed call, not a fault) and prints each container header the first time it is seen at an address. */
static DWORD WINAPI sweep_main(void *arg) {
    static struct { uint64_t a; uint32_t size; } seen[4096];
    int nseen = 0;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    HANDLE me = GetCurrentProcess();
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    while (!g_m4_quit) {
        Sleep(2000);
        LONG64 t0 = tk();
        long long steps = 0, frame0 = nvn_front_frame_next();
        int fresh = 0;
        uint64_t a = 0x10000;
        MEMORY_BASIC_INFORMATION mi;
        while (a < 0x7ffffffe0000ull && VirtualQuery((const void *)(uintptr_t)a, &mi, sizeof mi) && !g_m4_quit) {
            uint64_t lo = (uint64_t)(uintptr_t)mi.BaseAddress, hi = lo + mi.RegionSize;
            if (mi.State == MEM_COMMIT && mi.Protect == PAGE_READWRITE && mi.Type != MEM_IMAGE && (lo >> 28) != 0x710)
                for (uint64_t p = (lo + 0xfff) & ~0xfffull; p + 0x40 <= hi; p += 0x1000) {
                    uint32_t hd[8];
                    SIZE_T got = 0;
                    steps++;
                    if (!ReadProcessMemory(me, (const void *)(uintptr_t)p, hd, 32, &got) || got != 32) break;
                    if (hd[0] != 0x48534E42u || (hd[3] & 0xffff) != 0xFEFFu || hd[7] < 0x60 || hd[7] > (64u << 20)) continue;
                    int k;
                    for (k = 0; k < nseen; k++) if (seen[k].a == p && seen[k].size == hd[7]) break;
                    if (k < nseen || nseen == 4096) continue;
                    seen[nseen].a = p; seen[nseen++].size = hd[7];
                    fresh++;
                    if (hd[7] >= (256u << 10)) printf("nvn_vk_sweep: a container of %u KB is in memory at %llx, first seen by the sweep that began at frame %lld (%s)\n", hd[7] >> 10, (unsigned long long)p, frame0, fight_recent() ? "near a fight" : "no fight");
                }
            a = hi;
        }
        if (fresh) { printf("nvn_vk_sweep: sweep from frame %lld: %d new container headers (%d so far), %lld steps of 4 KB read in %.0f ms\n", frame0, fresh, nseen, steps, (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart); fflush(stdout); }
    }
    return 0;
}
static void prep_start(void) {
    const char *e = getenv("SSBU_VK_PREP");
    if (getenv("SSBU_VK_SWEEP_LOG") && atoi(getenv("SSBU_VK_SWEEP_LOG"))) CloseHandle(CreateThread(NULL, 0, sweep_main, NULL, 0, NULL));
    int on = e ? atoi(e) != 0 : (D.has_gpl && gpl_sep());
    if (getenv("SSBU_VK_PREP_FIGHT")) g_prep_fight_cap = atoi(getenv("SSBU_VK_PREP_FIGHT"));
    g_prep_cap = g_pw_nthread > 1 ? g_pw_nthread / 2 : 1;
    if (getenv("SSBU_VK_PREP_WORKERS") && atoi(getenv("SSBU_VK_PREP_WORKERS")) > 0) g_prep_cap = atoi(getenv("SSBU_VK_PREP_WORKERS"));
    if (getenv("SSBU_VK_PREP_KEEP")) g_prep_keep = atoi(getenv("SSBU_VK_PREP_KEEP"));
    /* session 14: VK_KEYS_VERSION = the version of what feeds the driver's keys besides the shader text: the library layout
     * (the one-layout sets: 66 buffers, 128 samplers) and the dynamic-state list. RAISE IT when either changes: every stage
     * library then misses in the driver's cache, and without a new marker name the old marker would suppress the preparation.
     * Version 1 keeps the marker name of sessions 10-13 (no "_k" part), so a PC prepared before today is not prepared again. */
#define VK_KEYS_VERSION 1
    char kv[16] = "";
    if (VK_KEYS_VERSION > 1) snprintf(kv, sizeof kv, "_k%d", VK_KEYS_VERSION);
    snprintf(g_prep_marker, sizeof g_prep_marker, "%s/prepared_v%u%s_s%d_%s_%04x_%04x_%08x.txt", vkc_dir(), MX_VK_VERSION, kv, RS_ON ? 1 : 0, D.has_gpl && gpl_sep() ? "lib" : D.has_vids ? "nolib" : "novi" /* a pass without libraries (SSBU_VK_PREP=1, run s10n0) must not make a later run with libraries skip theirs */, (unsigned)V.props.vendorID, (unsigned)V.props.deviceID, (unsigned)V.props.driverVersion);
    if (on && GetFileAttributesA(g_prep_marker) != INVALID_FILE_ATTRIBUTES && !(e && atoi(e) == 2)) {
        /* 2026-10-16, the owner's order ("all of them during the white screen at the start, with a small loading screen"):
         * graphics.ini shader_load = start (default; SSBU_VK_LOAD_AT_START=0/1 wins, SSBU_VK_PREP_KEEP=<n> too): EVERY
         * container is made into memory from the records of the earlier run (stage + library kept, the libraries are
         * pipeline-cache hits), all of them marked announced, so a pool laid at a loading screen queues nothing; the game's
         * first present waits for the end behind the "LOADING SHADERS" screen (prep_first_hold). */
        const char *la = getenv("SSBU_VK_LOAD_AT_START");
        g_prep_loadall = la ? atoi(la) != 0 : gfx_options()->shader_load == 0;
        if (g_prep_loadall && !getenv("SSBU_VK_PREP_KEEP")) g_prep_keep = 1024; else g_prep_loadall = 0;
        printf("nvn_vk_prep: every container of the game's data was prepared for this GPU and driver by an earlier run (%s): %s (SSBU_VK_PREP=2 does everything again)\n", g_prep_marker,
               g_prep_loadall ? "shader_load = start: ALL of them are made into memory now, from their records, before the game's first frame" : g_prep_keep > 0 ? "only the container(s) kept in memory (SSBU_VK_PREP_KEEP) are made again, from their records" : "shader_load = loading: each is made at its loading screen");
        g_prep_marked = 1;
        /* session 11: the thread still runs: it makes the INDEX (hashing, under a second), so that a container is announced
         * whole at its loading screen on every start; it queues nothing (before: no thread, so no index, on a marked start) */
    }
    if (!on || !g_m4_on) { printf("nvn_vk_prep: the preparation of every container is off (%s)\n", e ? "SSBU_VK_PREP=0" : "no stage libraries: SSBU_VK_GPL is not 2 or the driver lacks VK_EXT_graphics_pipeline_library"); return; }
    g_prep_thread = CreateThread(NULL, 0, prep_main, NULL, 0, NULL);
}
/* `ssbu_vk1.exe --prepare-shaders [folder]` (boot.c): no game, no window, no Vulkan device. Every *.bnsh file of the folder
 * (tools_vk/bnsh_extract.py writes them from data.arc) is walked as the scan above walks a container in the game's memory
 * (file offsets instead of pointers) and its stages are translated on the workers: ..\cache\vk\stage and \spv are then
 * what a run finds when the game loads the container. Pipelines are NOT made here: they need the GPU and the pass a pair
 * is drawn in, which only a run knows (pipes.bin). */
int nvn_vk_prepare_shaders(const char *dir) {
    char pat[300], path[400];
    WIN32_FIND_DATAA fd;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONG64 t0 = tk(), last = t0;
    int files = 0, bad = 0;
    long long vars = 0, jobs = 0;
    if (!dir) dir = "../cache/vk_containers";
    g_offline = 1; g_game_tables = 0; g_cpredict = 0;
    InitializeCriticalSection(&g_sh_lock); InitializeCriticalSection(&g_gpl_lock);
    InitializeConditionVariable(&g_sh_cv); InitializeConditionVariable(&g_jq_cv);
    CreateDirectoryA("../cache", NULL); CreateDirectoryA(vkc_dir(), NULL); CreateDirectoryA(vkc_file("spv"), NULL); CreateDirectoryA(vkc_file("stage"), NULL);
    {   HMODULE m = LoadLibraryA("ssbu_glslang.dll");
        g_glsl_to_spirv = m ? (void *)GetProcAddress(m, "ssbu_glsl_to_spirv") : NULL; g_spirv_free = m ? (void *)GetProcAddress(m, "ssbu_spirv_free") : NULL;
        if (!g_glsl_to_spirv || !g_spirv_free) { printf("prepare-shaders: ssbu_glslang.dll cannot be loaded (build_glslang.bat makes it)\n"); return 2; }
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_pw_nthread = (int)si.dwNumberOfProcessors > 16 ? 16 : (int)si.dwNumberOfProcessors;
    g_pw_gate = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_pw_left = g_pw_nthread;
    for (int i = 0; i < g_pw_nthread; i++) g_pw_thread[i] = CreateThread(NULL, 0, pw_main, NULL, 0, NULL);
    snprintf(pat, sizeof pat, "%s/*.bnsh", dir);
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("prepare-shaders: no *.bnsh file in %s (run: python tools_vk/bnsh_extract.py)\n", dir); return 2; }
    do {
        snprintf(path, sizeof path, "%s/%s", dir, fd.cFileName);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        uint8_t *buf = n >= 0x60 ? _aligned_malloc((size_t)n + 64, 4096) : NULL;
        if (buf && fread(buf, 1, (size_t)n, fp) == (size_t)n && g32((uint64_t)(uintptr_t)buf) == 0x48534E42u) {
            int nj = 0, nv = cont_walk((uint64_t)(uintptr_t)buf, (uint64_t)n, 0, &nj, 0);
            if (nv < 0) { bad++; printf("prepare-shaders: %s: not understood (no grsc block, or its variation array lies outside the file)\n", fd.cFileName); }
            else { vars += nv; jobs += nj; }
            files++;
        }
        if (buf) _aligned_free(buf);                /* the jobs hold their own copy of the code */
        fclose(fp);
        if (tk() - last > 2 * fq.QuadPart) { last = tk(); printf("prepare-shaders: %d containers read, %lld variations, %lld of %lld translated (%.0f s)\n", files, vars, (long long)g_bq_done, (long long)g_bq_queued, (double)(last - t0) / (double)fq.QuadPart); }
        EnterCriticalSection(&g_sh_lock);          /* not more than 4,000 copies of code waiting */
        while (g_bq_queued - g_bq_done > 4000) SleepConditionVariableCS(&g_sh_cv, &g_sh_lock, 200);
        LeaveCriticalSection(&g_sh_lock);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    EnterCriticalSection(&g_sh_lock);
    while (g_bq_done < g_bq_queued) {
        SleepConditionVariableCS(&g_sh_cv, &g_sh_lock, 500);
        if (tk() - last > 2 * fq.QuadPart) { last = tk(); printf("prepare-shaders: %d containers read, %lld variations, %lld of %lld translated (%.0f s)\n", files, vars, (long long)g_bq_done, (long long)g_bq_queued, (double)(last - t0) / (double)fq.QuadPart); }
    }
    g_m4_quit = 1;
    WakeAllConditionVariable(&g_jq_cv);
    LeaveCriticalSection(&g_sh_lock);
    WaitForMultipleObjects((DWORD)g_pw_nthread, g_pw_thread, TRUE, 60000);
    long long made = 0, failed = 0;
    for (int i = 0; i < 4096; i++) for (VStage *v = g_vstage[i]; v; v = v->next) { if (v->ok) made++; else failed++; }
    printf("prepare-shaders: done in %.0f s on %d threads: %d containers (%d not understood), %lld variations, %lld handed to the workers, %lld distinct stage variants ready in ../cache/vk (%lld could not be translated)\n",
           (double)(tk() - t0) / (double)fq.QuadPart, g_pw_nthread, files, bad, vars, jobs, made, failed);
    return bad || failed ? 1 : 0;
}
