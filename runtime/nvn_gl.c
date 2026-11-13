/* NVN on OpenGL: the game's graphics calls drawn for real.
 *
 * How it fits together
 *   - One render thread owns the OpenGL context (4.6 compatibility, NVIDIA). Guest threads hand it work with
 *     gl_run() and wait for it.
 *   - Shaders: only about 20 of the game's ~8200 shaders carry NV_gpu_program5 assembly text in their control
 *     section; all of them carry Maxwell machine code. The machine code is translated to GLSL (mx_translate.c,
 *     tested offline by mxtest.exe on every dumped stage) and used as separable programs in one pipeline. The assembly-text path
 *     (glProgramStringARB) is kept as a fallback for pairs where both stages have text.
 *   - Hardware constant buffers as the machine code sees them: 1 = constants stored after the shader's code
 *     (also jump tables), 2 = texture handles (8 bytes per unit from byte 32), 3 + n = uniform buffer n bound
 *     by the game. They are bound as storage blocks (binding 20 * stage + index); the game's storage buffers
 *     (LDG / STG in machine code) as storage blocks at 40 + 16 * stage + index. Alpha test is done by the
 *     translated fragment shader (uniform at location 0), not by fixed function.
 *   - Command buffers are recorded into host lists and replayed at nvnQueueSubmitCommands.
 *   - Buffers are guest memory: at draw time the bound ranges are copied into a streaming GL buffer.
 *   - Textures are created lazily; texels come from the pool memory (Tegra block-linear layout) and are
 *     re-read when that memory changes. Samplers + textures become bindless handles.
 *   - Window origin is upper-left and depth is 0..1 (glClipControl), so row 0 of every GL texture is the
 *     top row, as in NVN; only the final blit to the window is flipped.
 *
 * Debug aids (environment): SSBU_SHOT=<n> / SSBU_SHOT_EVERY=<k> save presented frames as frame_<n>.bmp (in SSBU_SHOT_DIR if set, from frame SSBU_SHOT_FROM on);
 * SSBU_RTDUMP=<n> saves every colour target at frame n; SSBU_DRAWSHOT=<n> the target after each draw
 * (with SSBU_DRAWSHOT_CLEAR, cleared before each); SSBU_DRAWLOG=<n> prints state, uniforms and textures of
 * every draw of frame n and writes the textures as tex_*.rgba; SSBU_SHADERDUMP=<dir> writes shader text,
 * machine code and generated GLSL; SSBU_NOSCISSOR, SSBU_SKIPDRAW=<i>, SSBU_FRAGDEBUG (magenta fragments),
 * SSBU_GLDEBUG (driver messages, error check after every command); SSBU_NOCOMPUTE skips every compute dispatch (fighters then
 * stay in their bind pose); SSBU_DRAWLOG_MINDISP=<n> makes SSBU_DRAWLOG count frames with at least n dispatches (the draw log
 * lists each dispatch with its buffers and marks vertex buffers written by compute); every 300 frames a line with the frame
 * rate. SSBU_NVN_MISSING=warn (nvn_stub.c): a call of an NVN function without an implementation does not stop the game.
 *
 * Compute (nvnCommandBufferDispatchCompute): see the block above compute_dispatch. The five compute programs built into the
 * executable carry NV assembly text and run through GL_NV_compute_program5; the others are Maxwell machine code without a
 * program header (control +0x714 = 5, work group size at control +0x748) translated by mx_translate.c. Storage buffers a
 * dispatch writes are copied back to guest memory before the next draw / copy and at the end of the batch.
 * The whole API surface is audited in plan\phases\G1_nvn_audit.md (python nvn_audit.py).
 *
 * NVN enum values and argument orders: public nvn.h (open-ead/nnheaders), checked against the trace.
 * Maxwell encodings: envytools envydis/gm107.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <GL/gl.h>
#include "runtime.h"
#include "hostrt.h"
#include "mx_translate.h"
#include "gfx_options.h"
#include "nvn_front.h"
#include "tex_decode.h"
#include "nvn_sdk.h"
#include "nvn_c4.h"

#pragma comment(lib, "opengl32.lib")

/* ask laptop drivers for the discrete GPU */
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define A6 (c->x[6])
#define A7 (c->x[7])
#define F0 (c->v[0].f[0])
#define F1 (c->v[1].f[0])
#define F2 (c->v[2].f[0])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(a) ((void *)(uintptr_t)(a))

int nvn_pool_range(uint64_t pool, uint64_t *mem, uint64_t *size);     /* nvn_stub.c */
void input_frame_draws(int draws);                                    /* nnhid.c */
void display_pace(void);                                              /* display.c */
int display_wait_refresh(void);                                       /* display.c, P3 round 3: one refresh clock */
void display_before_present(void);
void display_clock_sample(uint64_t index_ptr);                        /* SSBU_CLOCKSTAT */
static void *volatile g_window_sync;                                  /* the sync object of the last nvnWindowAcquireTexture */
void display_set_interval(int n);
/* V1 (plan\phases\V1_vulkan.md, seam step S0): SSBU_RENDERER=vk. The render thread then starts nvn_vk.c instead of an
 * OpenGL context and its jobs go through vk_route (end of this file); nothing else in this file changes behaviour. */
int nvn_vk_init(void *hwnd);
void nvn_vk_present_black(void);
void nvn_vk_flush(int finish);
void nvn_vk_shutdown(void);
void nvn_vk_missing(const char *what);
void nvn_vk_report(FILE *out);
static int g_vk;
const RBackend *g_rb = &rb_gl;                 /* seam step S2: chosen once in nvn_gl_start (SSBU_RENDERER) */
static void vk_shutdown_at_exit(void);

/* ---------------------------------------------------------------- OpenGL beyond 1.1 */
#define GLF(X) \
    X(void, GenProgramsARB, (GLsizei, GLuint *)) \
    X(void, DeleteProgramsARB, (GLsizei, const GLuint *)) \
    X(void, BindProgramARB, (GLenum, GLuint)) \
    X(void, ProgramStringARB, (GLenum, GLenum, GLsizei, const void *)) \
    X(void, GenBuffers, (GLsizei, GLuint *)) \
    X(void, BindBuffer, (GLenum, GLuint)) \
    X(void, BufferData, (GLenum, ptrdiff_t, const void *, GLenum)) \
    X(void, BufferSubData, (GLenum, ptrdiff_t, ptrdiff_t, const void *)) \
    X(void, BindBufferRange, (GLenum, GLuint, GLuint, ptrdiff_t, ptrdiff_t)) \
    X(void, GenFramebuffers, (GLsizei, GLuint *)) \
    X(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, BindFramebuffer, (GLenum, GLuint)) \
    X(void, FramebufferTexture, (GLenum, GLenum, GLuint, GLint)) \
    X(GLenum, CheckFramebufferStatus, (GLenum)) \
    X(void, DrawBuffers, (GLsizei, const GLenum *)) \
    X(void, BlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
    X(void, TexStorage1D, (GLenum, GLsizei, GLenum, GLsizei)) \
    X(void, TexStorage2D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, TexStorage3D, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei)) \
    X(void, TexStorage2DMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLboolean)) \
    X(void, TexStorage3DMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei, GLsizei, GLboolean)) \
    X(void, TexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, CompressedTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *)) \
    X(void, CompressedTexSubImage3D, (GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLsizei, const void *)) \
    X(void, TextureView, (GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint)) \
    X(void, GenSamplers, (GLsizei, GLuint *)) \
    X(void, SamplerParameteri, (GLuint, GLenum, GLint)) \
    X(void, SamplerParameterf, (GLuint, GLenum, GLfloat)) \
    X(void, SamplerParameterfv, (GLuint, GLenum, const GLfloat *)) \
    X(uint64_t, GetTextureSamplerHandleARB, (GLuint, GLuint)) \
    X(uint64_t, GetTextureHandleARB, (GLuint)) \
    X(void, MakeTextureHandleResidentARB, (uint64_t)) \
    X(void, MakeTextureHandleNonResidentARB, (uint64_t)) \
    X(void, VertexAttribFormat, (GLuint, GLint, GLenum, GLboolean, GLuint)) \
    X(void, VertexAttribIFormat, (GLuint, GLint, GLenum, GLuint)) \
    X(void, VertexAttribBinding, (GLuint, GLuint)) \
    X(void, BindVertexBuffer, (GLuint, GLuint, ptrdiff_t, GLsizei)) \
    X(void, VertexBindingDivisor, (GLuint, GLuint)) \
    X(void, EnableVertexAttribArray, (GLuint)) \
    X(void, DisableVertexAttribArray, (GLuint)) \
    X(void, DrawElementsInstancedBaseVertexBaseInstance, (GLenum, GLsizei, GLenum, const void *, GLsizei, GLint, GLuint)) \
    X(void, DrawArraysInstancedBaseInstance, (GLenum, GLint, GLsizei, GLsizei, GLuint)) \
    X(void, BlendFuncSeparatei, (GLuint, GLenum, GLenum, GLenum, GLenum)) \
    X(void, BlendEquationSeparatei, (GLuint, GLenum, GLenum)) \
    X(void, Enablei, (GLenum, GLuint)) \
    X(void, Disablei, (GLenum, GLuint)) \
    X(void, ColorMaski, (GLuint, GLboolean, GLboolean, GLboolean, GLboolean)) \
    X(void, BlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, StencilFuncSeparate, (GLenum, GLenum, GLint, GLuint)) \
    X(void, StencilOpSeparate, (GLenum, GLenum, GLenum, GLenum)) \
    X(void, StencilMaskSeparate, (GLenum, GLuint)) \
    X(void, ClearBufferfv, (GLenum, GLint, const GLfloat *)) \
    X(void, ViewportIndexedf, (GLuint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, ScissorIndexed, (GLuint, GLint, GLint, GLsizei, GLsizei)) \
    X(void, DepthRangeIndexed, (GLuint, GLdouble, GLdouble)) \
    X(void, ClipControl, (GLenum, GLenum)) \
    X(void, PrimitiveRestartIndex, (GLuint)) \
    X(void, PatchParameteri, (GLenum, GLint)) \
    X(void, DispatchCompute, (GLuint, GLuint, GLuint)) \
    X(void, CreateBuffers, (GLsizei, GLuint *)) \
    X(void, DeleteBuffers, (GLsizei, const GLuint *)) \
    X(void, NamedBufferData, (GLuint, ptrdiff_t, const void *, GLenum)) \
    X(void, NamedBufferSubData, (GLuint, ptrdiff_t, ptrdiff_t, const void *)) \
    X(void, GetNamedBufferSubData, (GLuint, ptrdiff_t, ptrdiff_t, void *)) \
    X(void, MemoryBarrier, (GLbitfield)) \
    X(void, BindImageTexture, (GLuint, GLuint, GLint, GLboolean, GLint, GLenum, GLenum)) \
    X(void, DebugMessageCallback, (void *, const void *))     X(GLuint, CreateShaderProgramv, (GLenum, GLsizei, const char *const *))     X(void, GenProgramPipelines, (GLsizei, GLuint *))     X(void, BindProgramPipeline, (GLuint))     X(void, UseProgramStages, (GLuint, GLbitfield, GLuint))     X(void, GetProgramiv, (GLuint, GLenum, GLint *))     X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei *, char *))     X(void, DeleteProgram, (GLuint))     X(void, ProgramUniform2ui, (GLuint, GLint, GLuint, GLuint))     X(void, PolygonOffsetClamp, (GLfloat, GLfloat, GLfloat))     X(void, ClearBufferiv, (GLenum, GLint, const GLint *))     X(void, ClearBufferuiv, (GLenum, GLint, const GLuint *))     X(void, SampleMaski, (GLuint, GLbitfield))     X(void, GetTextureSubImage, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, GLsizei, void *))     X(void, GetCompressedTextureSubImage, (GLuint, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLsizei, void *))     X(void, GenQueries, (GLsizei, GLuint *))     X(void, BeginQuery, (GLenum, GLuint))     X(void, EndQuery, (GLenum))     X(void, GetQueryObjectuiv, (GLuint, GLenum, GLuint *))
#define X(ret, name, args) static ret (APIENTRY *gl##name) args;
GLF(X)
#undef X
/* OpenGL entry points that only some NVN features need: looked up without stopping when one is absent (gl_has reports a use). */
#define GLO(X) \
    X(GLuint, CreateShader, (GLenum)) \
    X(void, ShaderSource, (GLuint, GLsizei, const char *const *, const GLint *)) \
    X(void, CompileShader, (GLuint)) \
    X(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, AttachShader, (GLuint, GLuint)) \
    X(void, DetachShader, (GLuint, GLuint)) \
    X(void, DeleteShader, (GLuint)) \
    X(GLuint, CreateProgram, (void)) \
    X(void, LinkProgram, (GLuint)) \
    X(void, ProgramParameteri, (GLuint, GLenum, GLint)) \
    X(void, GetProgramBinary, (GLuint, GLsizei, GLsizei *, GLenum *, void *)) \
    X(void, ProgramBinary, (GLuint, GLenum, const void *, GLsizei)) \
    X(void, UseProgram, (GLuint)) \
    X(void, DepthBoundsEXT, (GLdouble, GLdouble)) \
    X(void, ConservativeRasterParameterfNV, (GLenum, GLfloat)) \
    X(void, SubpixelPrecisionBiasNV, (GLuint, GLuint)) \
    X(void, ViewportSwizzleNV, (GLuint, GLenum, GLenum, GLenum, GLenum)) \
    X(void, PatchParameterfv, (GLenum, const GLfloat *)) \
    X(void, TextureBarrier, (void)) \
    X(void, InvalidateFramebuffer, (GLenum, GLsizei, const GLenum *)) \
    X(void, FramebufferTextureLayer, (GLenum, GLenum, GLuint, GLint, GLint)) \
    X(void *, FenceSync, (GLenum, GLbitfield)) \
    X(GLenum, ClientWaitSync, (void *, GLbitfield, uint64_t)) \
    X(void, WaitSync, (void *, GLbitfield, uint64_t)) \
    X(void, DeleteSync, (void *)) \
    X(void, PushDebugGroup, (GLenum, GLuint, GLsizei, const char *)) \
    X(void, PopDebugGroup, (void)) \
    X(void, DebugMessageInsert, (GLenum, GLenum, GLuint, GLenum, GLsizei, const char *)) \
    X(void, ObjectLabel, (GLenum, GLuint, GLsizei, const char *)) \
    X(void, DebugMessageControl, (GLenum, GLenum, GLenum, GLsizei, const GLuint *, GLboolean)) \
    X(void, CoverageModulationNV, (GLenum)) \
    X(void, CoverageModulationTableNV, (GLsizei, const GLfloat *)) \
    X(void, FragmentCoverageColorNV, (GLuint)) \
    X(void, RasterSamplesEXT, (GLuint, GLboolean)) \
    X(void, FramebufferParameteri, (GLenum, GLenum, GLint)) \
    X(void, FramebufferSampleLocationsfvNV, (GLenum, GLuint, GLsizei, const GLfloat *)) \
    X(void, MinSampleShading, (GLfloat)) \
    X(void, GetQueryObjectui64v, (GLuint, GLenum, uint64_t *)) \
    X(GLenum, GetGraphicsResetStatus, (void)) \
    X(void, SamplerParameterIiv, (GLuint, GLenum, const GLint *)) \
    X(void, SamplerParameterIuiv, (GLuint, GLenum, const GLuint *)) \
    X(void, BlendParameteriNV, (GLenum, GLint)) \
    X(void, BlendEquationi, (GLuint, GLenum))
#define X(ret, name, args) static ret (APIENTRY *gl##name) args;
GLO(X)
#undef X
static int gl_has_(const void *fn, const char *name) {
    static int nlog;
    if (fn) return 1;
    if (nlog < 30) { nlog++; printf("nvn_gl: this OpenGL driver lacks %s: the NVN feature that needs it has no effect\n", name); fflush(stdout); }
    return 0;
}
#define gl_has(fn, name) gl_has_((const void *)(uintptr_t)(fn), name)

enum {
    GL_VERTEX_PROGRAM_ARB_ = 0x8620, GL_FRAGMENT_PROGRAM_ARB_ = 0x8804, GL_GEOMETRY_PROGRAM_NV_ = 0x8C26,
    GL_TESS_CONTROL_PROGRAM_NV_ = 0x891E, GL_TESS_EVALUATION_PROGRAM_NV_ = 0x891F, GL_COMPUTE_PROGRAM_NV_ = 0x90FB,
    GL_PROGRAM_FORMAT_ASCII_ARB_ = 0x8875, GL_PROGRAM_ERROR_POSITION_ARB_ = 0x864B, GL_PROGRAM_ERROR_STRING_ARB_ = 0x8874,
    GL_MAX_PROGRAM_PARAMETER_BUFFER_BINDINGS_NV_ = 0x8DA0,
    GL_ARRAY_BUFFER_ = 0x8892, GL_ELEMENT_ARRAY_BUFFER_ = 0x8893, GL_STREAM_DRAW_ = 0x88E0, GL_SHADER_STORAGE_BUFFER_ = 0x90D2,
    GL_FRAMEBUFFER_ = 0x8D40, GL_READ_FRAMEBUFFER_ = 0x8CA8, GL_DRAW_FRAMEBUFFER_ = 0x8CA9, GL_COLOR_ATTACHMENT0_ = 0x8CE0,
    GL_DEPTH_ATTACHMENT_ = 0x8D00, GL_STENCIL_ATTACHMENT_ = 0x8D20, GL_DEPTH_STENCIL_ATTACHMENT_ = 0x821A,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8CD5, GL_FRAMEBUFFER_SRGB_ = 0x8DB9,
    GL_TEXTURE_3D_ = 0x806F, GL_TEXTURE_1D_ARRAY_ = 0x8C18, GL_TEXTURE_2D_ARRAY_ = 0x8C1A, GL_TEXTURE_2D_MULTISAMPLE_ = 0x9100,
    GL_TEXTURE_2D_MULTISAMPLE_ARRAY_ = 0x9102, GL_TEXTURE_RECTANGLE_ = 0x84F5, GL_TEXTURE_CUBE_MAP_ = 0x8513,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X_ = 0x8515, GL_TEXTURE_CUBE_MAP_ARRAY_ = 0x9009, GL_TEXTURE_BUFFER_ = 0x8C2A,
    GL_TEXTURE_MAX_LEVEL_ = 0x813D, GL_TEXTURE_SWIZZLE_RGBA_ = 0x8E46, GL_DEPTH_STENCIL_TEXTURE_MODE_ = 0x90EA,
    GL_TEXTURE_WRAP_R_ = 0x8072, GL_TEXTURE_MIN_LOD_ = 0x813A, GL_TEXTURE_MAX_LOD_ = 0x813B, GL_TEXTURE_LOD_BIAS_ = 0x8501,
    GL_TEXTURE_COMPARE_MODE_ = 0x884C, GL_TEXTURE_COMPARE_FUNC_ = 0x884D, GL_COMPARE_REF_TO_TEXTURE_ = 0x884E,
    GL_TEXTURE_MAX_ANISOTROPY_ = 0x84FE, GL_TEXTURE_CUBE_MAP_SEAMLESS_ = 0x884F,
    GL_UPPER_LEFT_ = 0x8CA2, GL_LOWER_LEFT_ = 0x8CA1, GL_ZERO_TO_ONE_ = 0x935F, GL_NEGATIVE_ONE_TO_ONE_ = 0x935E,
    GL_MULTISAMPLE_ = 0x809D, GL_SAMPLE_ALPHA_TO_COVERAGE_ = 0x809E, GL_PRIMITIVE_RESTART_ = 0x8F9D, GL_PATCH_VERTICES_ = 0x8E72,
    GL_DEPTH_CLAMP_ = 0x864F, GL_RASTERIZER_DISCARD_ = 0x8C89, GL_DEBUG_OUTPUT_ = 0x92E0, GL_DEBUG_OUTPUT_SYNCHRONOUS_ = 0x8242,
    GL_BGR_ = 0x80E0, GL_BGRA_ = 0x80E1, GL_UNPACK_ROW_LENGTH_ = 0x0CF2, GL_DEPTH_STENCIL_ = 0x84F9, GL_STENCIL_INDEX_ = 0x1901,
};

/* ---------------------------------------------------------------- the render thread */
typedef struct Job { void (*fn)(void *); void *arg; HANDLE done; struct Job *next; } Job;
static CRITICAL_SECTION g_job_lock;
static Job *g_job_head, *g_job_tail;
static HANDLE g_job_event, g_ready_event;
static DWORD g_render_tid;
static HWND g_hwnd;
static HDC g_dc;
static __declspec(thread) HANDLE t_done;
static void gl_housekeeping(void);
static void (*pf_job_submit)(void *), (*pf_job_present)(void *);
static LONG64 g_joblog_frame, g_joblog_cur;
static void joblog(void (*fn)(void *), LONG64 t);
static inline LONG64 pf_now(void);
static inline void pf_add(int k, LONG64 t0);
enum { PF_WAIT_SUBMIT_ = 13, PF_WAIT_PRESENT_ = 14, PF_WAIT_OTHER_ = 15 };
/* Hitch log (hitch.c, SSBU_HITCH=<ms>): what the render thread is doing right now, for the watchdog. Two stores per scope. */
extern int g_hitch_ms;
extern volatile const char *g_hitch_rt;
extern volatile uint64_t g_hitch_rt_arg;
void hitch_wait_render(int on); void hitch_frame(long long frame, double ms, int battle); void hitch_report(FILE *out); void hitch_set_render(void);
#define HS_IN(name, arg) const char *hs_p_ = (const char *)g_hitch_rt; uint64_t hs_a_ = g_hitch_rt_arg; g_hitch_rt_arg = (uint64_t)(arg); g_hitch_rt = (name)
#define HS_OUT() (g_hitch_rt = hs_p_, g_hitch_rt_arg = hs_a_)

static void gl_run(void (*fn)(void *), void *arg) {
    if (GetCurrentThreadId() == g_render_tid) { fn(arg); return; }
    if (!t_done) t_done = CreateEventA(NULL, FALSE, FALSE, NULL);
    Job j = {fn, arg, t_done, NULL};
    LONG64 pf0 = pf_now();
    if (g_joblog_frame > 0 && g_joblog_cur >= g_joblog_frame && g_joblog_cur < g_joblog_frame + 3) joblog(fn, pf0);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = &j; else g_job_head = &j;
    g_job_tail = &j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
    hitch_wait_render(1);
    WaitForSingleObject(j.done, INFINITE);
    hitch_wait_render(0);
    pf_add(fn == pf_job_submit ? PF_WAIT_SUBMIT_ : fn == pf_job_present ? PF_WAIT_PRESENT_ : PF_WAIT_OTHER_, pf0);
}

/* P3: nvnQueueSubmitCommands / FenceSync / Flush do not wait for the render thread (SSBU_SYNC_SUBMIT=1: they wait, as before).
 * Measured in a battle: the game's main thread calls, per frame, SyncWait x2, SubmitCommands, FenceSync, Flush, then works
 * for about 8 ms and calls PresentTexture; the replay of the submitted commands takes 6-9 ms, during which the main
 * thread used to stand still. NVN itself executes submitted commands later, on the GPU, and the game waits for its
 * fence (SyncWait) before reusing what the commands refer to, so this is the order of events the game was written for.
 * What keeps the result the same as with a waiting submit:
 *   - the jobs run in the order they were issued (one queue); every other job (PresentTexture, SyncWait, texel
 *     reads and writes, Finish, ...) still waits, and so runs after everything issued before it;
 *   - the handles given to SubmitCommands are copied; the command lists themselves were copied when recorded;
 *   - entry points that change or release a host record a queued command may name (programs, textures, pools, samplers,
 *     windows, queues) first wait until the queue is empty (gl_barrier); FinalizeCommandHandle is queued in order;
 *   - guest memory the commands refer to (vertex / uniform / storage buffers, texture storage) is read when the commands
 *     run, as on the console's GPU: the game must not change it before the fence signals, and that rule is the game's own. */
static volatile LONG g_async_pending;
static int g_sync_submit = -1;
static void job_nop(void *arg) { (void)arg; }
static int gl_async_on(void) {
    if (g_sync_submit < 0) g_sync_submit = getenv("SSBU_SYNC_SUBMIT") != NULL;
    return !g_sync_submit;
}
static void gl_post(void (*fn)(void *), const void *arg, size_t size) {       /* run fn(copy of arg) on the render thread, in order, without waiting */
    if (GetCurrentThreadId() == g_render_tid) { fn((void *)arg); return; }
    Job *j = malloc(sizeof *j + size + 8);
    if (size) memcpy(j + 1, arg, size);
    j->fn = fn; j->arg = j + 1; j->done = NULL; j->next = NULL;
    InterlockedIncrement(&g_async_pending);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = j; else g_job_head = j;
    g_job_tail = j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
}
static void gl_barrier(void) { if (g_async_pending) gl_run(job_nop, NULL); }   /* everything queued so far has run when this returns */

/* ---------------------------------------------------------------- statistics and diagnostics */
static struct {
    LONG64 frames, submits, lists, draws, draws_skipped, clears, stale, tex_created, tex_uploads, tex_bytes, views, handles,
           prog_ok, prog_fail, binary_used, glsl_ok, glsl_fail, glsl_draws, fbo_made, fbo_bad, gl_errors, copies, unknown_fmt, arena_bytes,
           dispatches, dispatch_run, dispatch_skipped, compute_bytes, draws_from_compute;
} g_st;
static LONG64 g_frame_draws, g_frame_skipped, g_frame_clears;
static int g_debug, g_logn;
static const char *g_cur_op = "";
static volatile LONG g_setshaders_calls, g_with_text, g_without_text;
static LONG g_bufhist[6][32], g_storhist[32];

/* ---- timing scopes (plan\phases\P3_performance.md). Always on: two QueryPerformanceCounter calls per scope.
 * Printed with the frame-rate line every 300 frames as milliseconds per frame. Scopes nest (PREP contains ARENA and
 * TEXSYNC; SUBMIT contains everything the render thread does for a batch); WAIT_* are measured on the calling guest
 * thread (how long it was blocked in gl_run), PACE is the sleep of display_pace. */
enum { PF_SUBMIT, PF_PREP, PF_DRAWCALL, PF_ARENA, PF_TEXHASH, PF_TEXUP, PF_HANDLE, PF_DISPATCH, PF_CFLUSH, PF_COPY, PF_PRESENT, PF_SWAP,
       PF_OTHERJOB, PF_WAIT_SUBMIT, PF_WAIT_PRESENT, PF_WAIT_OTHER, PF_PACE, PF_XLATE, PF_GLCOMPILE, PF_N };
static const char *const k_pf_name[PF_N] = {"submit", "prep", "drawcall", "arena", "texhash", "texupload", "handle", "dispatch", "cflush", "copy", "present", "swap",
                                            "otherjob", "wait-submit", "wait-present", "wait-other", "pace", "shader-translate", "shader-compile"};
static volatile LONG64 g_pf[PF_N], g_pfn[PF_N];
static LONG64 g_pf_arena_bytes, g_pf_cflush_bytes, g_pf_hash_bytes;
/* SSBU_GLSTATS=1: one more line every 300 frames with per-frame counts (render-thread waits of guest threads, compute
 * read-backs, objects created, shader work, and how many per-draw GL settings repeat the value of the previous draw). */
static LONG64 g_gs_draws, g_gs_prog_same, g_gs_attr_set, g_gs_attr_same, g_gs_vbuf_bind, g_gs_buf_bind;
static GLuint g_gs_last_prog[2];
static uint32_t g_gs_last_attr[16];
static inline LONG64 pf_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline void pf_add(int k, LONG64 t0) { InterlockedExchangeAdd64(&g_pf[k], pf_now() - t0); InterlockedIncrement64(&g_pfn[k]); }
void nvn_pf_pace(LONG64 ticks) { InterlockedExchangeAdd64(&g_pf[PF_PACE], ticks); InterlockedIncrement64(&g_pfn[PF_PACE]); }     /* display.c */

static void logf_limited(int *counter, int limit, const char *fmt, ...) {
    if (*counter >= limit) return;
    (*counter)++;
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* P3 round 4, SSBU_FDLOG=1 (diagnostics): every message of the driver is kept with its time (ring of 64) and counted
 * per id; hitch_draw prints the ones that arrived during a slow draw call, nvn_gl_report the table. */
static int g_fdlog;
static unsigned g_fd_prim, g_fd_poly, g_fd_dclamp, g_fd_rdisc, g_fd_count;
static struct { LONG64 t; GLuint id; GLenum type, sev; char text[232]; } g_fd_ring[64];
static unsigned g_fd_ring_n;
static struct { GLuint id; GLenum type, sev; LONG64 n; char text[200]; } g_fd_ids[64];
static void fd_note(GLenum type, GLuint id, GLenum severity, const char *message) {
    unsigned k = g_fd_ring_n++ & 63;
    g_fd_ring[k].t = pf_now(); g_fd_ring[k].id = id; g_fd_ring[k].type = type; g_fd_ring[k].sev = severity;
    snprintf(g_fd_ring[k].text, sizeof g_fd_ring[k].text, "%s", message ? message : "");
    for (int i = 0; i < 64; i++) {
        if (g_fd_ids[i].n && g_fd_ids[i].id != id) continue;
        if (!g_fd_ids[i].n) { g_fd_ids[i].id = id; g_fd_ids[i].type = type; g_fd_ids[i].sev = severity; snprintf(g_fd_ids[i].text, sizeof g_fd_ids[i].text, "%s", message ? message : ""); }
        g_fd_ids[i].n++;
        break;
    }
}
static void APIENTRY debug_cb(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const char *message, const void *user) {
    static int n;
    (void)source; (void)length; (void)user;
    if (g_fdlog) fd_note(type, id, severity, message);
    if (severity == 0x826B) return;                       /* notifications */
    if (type != 0x824C && !g_debug) return;               /* errors always; the rest only with SSBU_GLDEBUG */
    logf_limited(&n, g_debug ? 3000 : 60, "GL debug (type %x id %u) during %s / %s: %s\n", type, id, g_cur_op, (const char *)g_hitch_rt, message);
}

/* ---------------------------------------------------------------- formats */
static const uint16_t k_int12[4][12] = {
    {0x8229, 0x8F94, 0x8232, 0x8231, 0x822D, 0x822A, 0x8F98, 0x8234, 0x8233, 0x822E, 0x8236, 0x8235},
    {0x822B, 0x8F95, 0x8238, 0x8237, 0x822F, 0x822C, 0x8F99, 0x823A, 0x8239, 0x8230, 0x823C, 0x823B},
    {0x8051, 0x8F96, 0x8D7D, 0x8D8F, 0x881B, 0x8054, 0x8F9A, 0x8D77, 0x8D89, 0x8815, 0x8D71, 0x8D83},
    {0x8058, 0x8F97, 0x8D7C, 0x8D8E, 0x881A, 0x805B, 0x8F9B, 0x8D76, 0x8D88, 0x8814, 0x8D70, 0x8D82}};
static const uint16_t k_type12[12] = {0x1401, 0x1400, 0x1401, 0x1400, 0x140B, 0x1403, 0x1402, 0x1403, 0x1402, 0x1406, 0x1405, 0x1404};
static const uint8_t k_size12[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4};
static const uint8_t k_isint12[12] = {0, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 1};
static const uint8_t k_norm12[12] = {1, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0};
static const uint16_t k_pix[4] = {0x1903, 0x8227, 0x1907, 0x1908}, k_pixint[4] = {0x8D94, 0x8228, 0x8D98, 0x8D99};
static const uint8_t k_astc[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

static int fmt_get(int nf, Fmt *f) {
    memset(f, 0, sizeof *f);
    f->bw = f->bh = 1;
#define SET(i, fo, ty, b, k) (f->internal = (i), f->format = (fo), f->type = (ty), f->bpb = (b), f->kind = (k), 1)
#define BC(i, b) (f->bw = f->bh = 4, SET(i, 0, 0, b, 4))
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int comps = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        return SET(k_int12[comps - 1][i], k_isint12[i] ? k_pixint[comps - 1] : k_pix[comps - 1], k_type12[i], comps * k_size12[i], 0);
    }
    if (nf >= 0x79 && nf <= 0x94) {
        int i = (nf - 0x79) % 14;
        f->bw = k_astc[i][0]; f->bh = k_astc[i][1];
        return SET((nf >= 0x87 ? 0x93D0 : 0x93B0) + i, 0, 0, 16, 4);
    }
    switch (nf) {
    case 0x31: return SET(0x8D48, GL_STENCIL_INDEX_, 0x1401, 1, 2);
    case 0x32: return SET(0x81A5, 0x1902, 0x1403, 2, 1);
    case 0x33: return SET(0x81A6, 0x1902, 0x1405, 4, 1);
    case 0x34: return SET(0x8CAC, 0x1902, 0x1406, 4, 1);
    case 0x35: return SET(0x88F0, GL_DEPTH_STENCIL_, 0x84FA, 4, 3);
    case 0x36: return SET(0x8CAD, GL_DEPTH_STENCIL_, 0x8DAD, 8, 3);
    case 0x37: case 0x38: return SET(0x8C43, 0x1908, 0x1401, 4, 0);
    case 0x39: return SET(0x8056, 0x1908, 0x8033, 2, 0);
    case 0x3A: return SET(0x8050, 0x1908, 0x8034, 2, 0);
    case 0x3B: return SET(0x8057, 0x1908, 0x8034, 2, 0);
    case 0x3C: return SET(0x8D62, 0x1907, 0x8363, 2, 0);
    case 0x3D: return SET(0x8059, 0x1908, 0x8368, 4, 0);
    case 0x3E: return SET(0x906F, 0x8D99, 0x8368, 4, 0);
    case 0x3F: return SET(0x8C3A, 0x1907, 0x8C3B, 4, 0);
    case 0x40: return SET(0x8C3D, 0x1907, 0x8C3E, 4, 0);
    case 0x41: return BC(0x83F0, 8);
    case 0x42: return BC(0x83F1, 8);
    case 0x43: return BC(0x83F2, 16);
    case 0x44: return BC(0x83F3, 16);
    case 0x45: return BC(0x8C4C, 8);
    case 0x46: return BC(0x8C4D, 8);
    case 0x47: return BC(0x8C4E, 16);
    case 0x48: return BC(0x8C4F, 16);
    case 0x49: return BC(0x8DBB, 8);
    case 0x4A: return BC(0x8DBC, 8);
    case 0x4B: return BC(0x8DBD, 16);
    case 0x4C: return BC(0x8DBE, 16);
    case 0x4D: return BC(0x8E8C, 16);
    case 0x4E: return BC(0x8E8D, 16);
    case 0x4F: return BC(0x8E8E, 16);
    case 0x50: return BC(0x8E8F, 16);
    case 0x95: return SET(0x8D62, GL_BGR_, 0x8363, 2, 0);
    case 0x99: case 0x9A: return SET(0x8058, GL_BGRA_, 0x1401, 4, 0);
    case 0x9B: case 0x9C: return SET(0x8C43, GL_BGRA_, 0x1401, 4, 0);
    }
    return 0;
#undef SET
#undef BC
}

/* Vertex attribute format: component count, GL type, normalised, integer. */
static int vfmt_get(int nf, int *size, GLenum *type, int *norm, int *integer) {
    static const uint16_t t6[6] = {0x1401, 0x1400, 0x1403, 0x1402, 0x1405, 0x1404};
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        *size = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1; *type = k_type12[i]; *norm = k_norm12[i]; *integer = k_isint12[i];
        return 1;
    }
    if (nf >= 0x51 && nf <= 0x68) { *size = (nf - 0x51) / 6 + 1; *type = t6[(nf - 0x51) % 6]; *norm = 0; *integer = 0; return 1; }
    switch (nf) {
    case 0x3D: *size = 4; *type = 0x8368; *norm = 1; *integer = 0; return 1;
    case 0x3E: *size = 4; *type = 0x8368; *norm = 0; *integer = 1; return 1;
    case 0x69: *size = 4; *type = 0x8D9F; *norm = 1; *integer = 0; return 1;
    case 0x6A: *size = 4; *type = 0x8D9F; *norm = 0; *integer = 1; return 1;
    case 0x6B: *size = 4; *type = 0x8368; *norm = 0; *integer = 0; return 1;
    case 0x6C: *size = 4; *type = 0x8D9F; *norm = 0; *integer = 0; return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- guest objects
 * Small state objects and builders keep their contents in the guest's own memory (the game copies them
 * around freely); textures, programs, command buffers, pools and windows keep a pointer to a host struct
 * at +0 and a tag at +8. Host structs of textures and programs are never freed (recorded command lists
 * and pool entries may still name them); they are only marked dead. */
static void *hobj(uint64_t g, uint32_t kind) { return g && rd32(g + 8) == kind ? P(rd64(g)) : NULL; }
static void *hnew(uint64_t g, uint32_t kind, size_t size) {
    void *p = calloc(1, size);
    wr64(g, (uint64_t)(uintptr_t)p);
    wr32(g + 8, kind);
    return p;
}

/* SamplerParams, View, TexBuilder, TexObj, EView, VNode, Tex, TexPoolEnt, TexPool, SamplerPool, Stage, Prog, Cmd, CmdBuf, Window:
 * nvn_front.h (seam step S1) */

static SRWLOCK g_pool_lock = SRWLOCK_INIT;
static Tex *volatile g_all_tex;            /* every texture, newest first (never unlinked: host records of textures are not freed) */
/* PR #30 (v1Ta44): flush notifications query only live texture ranges. History remains intact for renderer references and
 * prewarming. Sorted starts plus prefix maxima include overlapping/aliased textures without scanning every historical record. */
typedef struct { Tex *tex; uint64_t begin, end, prefix_end; } TexRange;
static SRWLOCK g_tex_range_lock = SRWLOCK_INIT;
static TexRange *g_tex_ranges;
static size_t g_tex_range_count, g_tex_range_cap;
static int g_tex_range_fallback;
static uint64_t tex_range_end(uint64_t begin, uint64_t size) { return size > UINT64_MAX - begin ? UINT64_MAX : begin + size; }
static size_t tex_range_lower(uint64_t addr) {
    size_t lo = 0, hi = g_tex_range_count;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (g_tex_ranges[mid].begin < addr) lo = mid + 1; else hi = mid; }
    return lo;
}
static void tex_range_prefix(size_t first) {
    uint64_t end = first ? g_tex_ranges[first - 1].prefix_end : 0;
    for (size_t i = first; i < g_tex_range_count; i++) {
        if (g_tex_ranges[i].end > end) end = g_tex_ranges[i].end;
        g_tex_ranges[i].prefix_end = end;
    }
}
static void tex_range_add(Tex *t) {
    if (!t->mem || !t->storage) return;
    AcquireSRWLockExclusive(&g_tex_range_lock);
    if (g_tex_range_fallback) { ReleaseSRWLockExclusive(&g_tex_range_lock); return; }
    if (g_tex_range_count == g_tex_range_cap) {
        size_t cap = g_tex_range_cap ? g_tex_range_cap * 2 : 256;
        TexRange *r = realloc(g_tex_ranges, cap * sizeof *r);
        if (!r) {                         /* never drop invalidations because the optional index could not grow */
            free(g_tex_ranges); g_tex_ranges = NULL; g_tex_range_count = g_tex_range_cap = 0; g_tex_range_fallback = 1;
            ReleaseSRWLockExclusive(&g_tex_range_lock);
            printf("nvn_gl: the live-texture range index could not grow: flush notifications walk every texture again\n");
            return;
        }
        g_tex_ranges = r; g_tex_range_cap = cap;
    }
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    size_t i = tex_range_lower(begin);
    memmove(g_tex_ranges + i + 1, g_tex_ranges + i, (g_tex_range_count - i) * sizeof *g_tex_ranges);
    g_tex_ranges[i].tex = t; g_tex_ranges[i].begin = begin; g_tex_ranges[i].end = tex_range_end(begin, t->storage);
    g_tex_range_count++; tex_range_prefix(i);
    ReleaseSRWLockExclusive(&g_tex_range_lock);
}
static void tex_range_remove(Tex *t) {
    if (!t->mem || !t->storage) return;
    AcquireSRWLockExclusive(&g_tex_range_lock);
    uint64_t begin = (uint64_t)(uintptr_t)t->mem;
    for (size_t i = tex_range_lower(begin); i < g_tex_range_count && g_tex_ranges[i].begin == begin; i++) {
        if (g_tex_ranges[i].tex != t) continue;
        g_tex_range_count--;
        memmove(g_tex_ranges + i, g_tex_ranges + i + 1, (g_tex_range_count - i) * sizeof *g_tex_ranges);
        tex_range_prefix(i);
        break;
    }
    if (!g_tex_range_count) { free(g_tex_ranges); g_tex_ranges = NULL; g_tex_range_cap = 0; }
    ReleaseSRWLockExclusive(&g_tex_range_lock);
}
void nvn_label_set(uint64_t obj, const char *text);
const char *nvn_label_get(uint64_t obj);
void nvn_label_drop(uint64_t obj);
static int g_origin_mode = 1, g_depth_mode = 1;        /* what the game asks for; applied by the render thread */

/* ---------------------------------------------------------------- Tegra block-linear layout
 * Rules, storage sizes and de-swizzling come from tex_decode.c (tex_layout / tex_deswizzle_level), which was
 * checked against the game's packaged textures; in particular 3D textures use block height 1 and a block depth. */
static void tex_lay(const TexBuilder *b, TexLayout *L) {
    tex_layout(L, b->target, b->format, b->width, b->height, b->depth, b->levels, b->flags & ~0x50, 0, b->has_layout && b->layout[0] <= 4 ? b->layout[0] : -1);
}
static int tex_layers(const TexBuilder *b) {
    switch (b->target) {
    case 3: case 4: case 6: case 9: return b->depth > 0 ? b->depth : 1;     /* arrays; a cube array counts faces */
    case 8: return 6;
    }
    return 1;
}
static int lvl(int v, int l) { v >>= l; return v > 0 ? v : 1; }
/* Size of the whole texture in pool memory (reserved) and of the part that holds texels (for bounds checks). */
static size_t tex_storage_size2(const TexBuilder *b, size_t *data_size) {
    Fmt f;
    TexLayout L;
    size_t s;
    if (!fmt_get(b->format, &f)) { if (data_size) *data_size = 0x1000; return 0x1000; }
    if (b->flags & 0x10) {
        s = (size_t)(b->stride ? b->stride : (int64_t)b->width * f.bpb) * (size_t)(b->height > 0 ? b->height : 1);
        if (data_size) *data_size = s;
        return s;
    }
    tex_lay(b, &L);
    s = L.size;
    if (data_size) *data_size = L.data_size;
    if (b->samples > 1) { s *= (size_t)b->samples; if (data_size) *data_size *= (size_t)b->samples; }
    return s;
}
static size_t tex_storage_size(const TexBuilder *b) { return tex_storage_size2(b, NULL); }

/* ---------------------------------------------------------------- textures on the render thread */
static uint32_t g_serial = 1;              /* one per submitted batch: texture memory is re-checked once per batch */
static GLuint g_fbo_read, g_dummy_tex;
static uint64_t g_dummy_handle;
static void fbo_purge(GLuint tex);

static GLenum gl_target(int t, int samples) {
    static const GLenum m[11] = {GL_TEXTURE_1D, GL_TEXTURE_2D, GL_TEXTURE_3D_, GL_TEXTURE_1D_ARRAY_, GL_TEXTURE_2D_ARRAY_, GL_TEXTURE_2D_MULTISAMPLE_,
                                 GL_TEXTURE_2D_MULTISAMPLE_ARRAY_, GL_TEXTURE_RECTANGLE_, GL_TEXTURE_CUBE_MAP_, GL_TEXTURE_CUBE_MAP_ARRAY_, GL_TEXTURE_BUFFER_};
    if (samples > 1 && t == 1) return GL_TEXTURE_2D_MULTISAMPLE_;
    if (samples > 1 && t == 4) return GL_TEXTURE_2D_MULTISAMPLE_ARRAY_;
    return t >= 0 && t < 11 ? m[t] : GL_TEXTURE_2D;
}
static void set_swizzle(GLenum tg, const uint8_t *swz) {
    static const GLint m[6] = {GL_ZERO, GL_ONE, GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
    GLint v[4];
    if (swz[0] == 2 && swz[1] == 3 && swz[2] == 4 && swz[3] == 5) return;
    for (int i = 0; i < 4; i++) v[i] = m[swz[i] < 6 ? swz[i] : 0];
    glTexParameteriv(tg, GL_TEXTURE_SWIZZLE_RGBA_, v);
}

static void tex_realize(Tex *t) {
    static int nlog, nbad;
    Fmt f;
    TexBuilder *b = &t->b;
    if (!fmt_get(b->format, &f)) {
        g_st.unknown_fmt++;
        logf_limited(&nbad, 20, "nvn_gl: texture format 0x%x is not mapped; using RGBA8\n", b->format);
        fmt_get(0x25, &f);
    }
    { const TexFormat *tf = tex_format(b->format); if (tf && tf->sw_decode && tf->gl_sw_internal) f.internal = tf->gl_sw_internal; }
    GLenum tg = gl_target(b->target, b->samples);
    int w = b->width > 0 ? b->width : 1, h = b->height > 0 ? b->height : 1, d = b->depth > 0 ? b->depth : 1, levels = b->levels > 0 ? b->levels : 1, maxdim = w > h ? w : h, maxl = 1;
    if (b->target == 2 && d > maxdim) maxdim = d;
    while ((maxdim >> maxl) > 0) maxl++;
    if (levels > maxl) levels = maxl;
    t->glevels = levels;
    while (glGetError()) {}
    glGenTextures(1, &t->o.gl);
    glBindTexture(tg, t->o.gl);
    switch (tg) {
    case GL_TEXTURE_1D: glTexStorage1D(tg, levels, f.internal, w); break;
    case GL_TEXTURE_2D: case GL_TEXTURE_RECTANGLE_: case GL_TEXTURE_CUBE_MAP_: case GL_TEXTURE_1D_ARRAY_:
        glTexStorage2D(tg, levels, f.internal, w, tg == GL_TEXTURE_1D_ARRAY_ ? d : h); break;
    case GL_TEXTURE_3D_: case GL_TEXTURE_2D_ARRAY_: case GL_TEXTURE_CUBE_MAP_ARRAY_: glTexStorage3D(tg, levels, f.internal, w, h, d); break;
    case GL_TEXTURE_2D_MULTISAMPLE_: glTexStorage2DMultisample(tg, b->samples, f.internal, w, h, GL_TRUE); t->glevels = 1; break;
    case GL_TEXTURE_2D_MULTISAMPLE_ARRAY_: glTexStorage3DMultisample(tg, b->samples, f.internal, w, h, d, GL_TRUE); t->glevels = 1; break;
    default: glTexStorage2D(GL_TEXTURE_2D, levels, f.internal, w, h); break;
    }
    if (tg != GL_TEXTURE_2D_MULTISAMPLE_ && tg != GL_TEXTURE_2D_MULTISAMPLE_ARRAY_) {
        glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        set_swizzle(tg, b->swz);
        if (b->dsmode == 1) glTexParameteri(tg, GL_DEPTH_STENCIL_TEXTURE_MODE_, GL_STENCIL_INDEX_);
    }
    GLenum e = glGetError();
    g_st.tex_created++;
    if (b->format >= 0x01 && b->format <= 0x0c && !(b->flags & 0x10)) {       /* candidates for a glyph cache: block-linear single-channel textures */
        static int n1;
        logf_limited(&n1, 12, "nvn_gl: single-channel texture %dx%dx%d format 0x%x levels %d flags 0x%x swizzle %d%d%d%d -> gl %u\n", b->width, b->height, b->depth, b->format, b->levels, b->flags,
                     b->swz[0], b->swz[1], b->swz[2], b->swz[3], t->o.gl);
    }
    if (e || g_debug) logf_limited(&nlog, 80, "nvn_gl: texture target %d format 0x%x %dx%dx%d levels %d samples %d flags 0x%x -> gl %u%s (error 0x%x)\n", b->target, b->format,
                                   b->width, b->height, b->depth, b->levels, b->samples, b->flags, t->o.gl, e ? " FAILED" : "", e);
}

/* One level (one layer / face, or all slices of a 3D level) of tightly packed texels into the GL texture. */
static void tex_put(Tex *t, const Fmt *f, int level, int layer, int x, int y, int z, int w, int h, int d, const void *data) {
    GLenum tg = gl_target(t->b.target, t->b.samples);
    GLsizei size = (GLsizei)(((size_t)(w + f->bw - 1) / f->bw) * ((size_t)(h + f->bh - 1) / f->bh) * f->bpb * (size_t)d);
    glBindTexture(tg, t->o.gl);
    switch (tg) {
    case GL_TEXTURE_1D: glTexSubImage1D(tg, level, x, w, f->format, f->type, data); break;
    case GL_TEXTURE_CUBE_MAP_: tg = GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + (GLenum)(layer % 6);    /* fall through */
    case GL_TEXTURE_2D: case GL_TEXTURE_RECTANGLE_: case GL_TEXTURE_1D_ARRAY_:
        if (f->kind == 4) glCompressedTexSubImage2D(tg, level, x, y, w, h, f->internal, size, data);
        else glTexSubImage2D(tg, level, x, y, w, h, f->format, f->type, data);
        break;
    case GL_TEXTURE_3D_: case GL_TEXTURE_2D_ARRAY_: case GL_TEXTURE_CUBE_MAP_ARRAY_:
        if (tg != GL_TEXTURE_3D_) z = layer;
        if (f->kind == 4) glCompressedTexSubImage3D(tg, level, x, y, z, w, h, d, f->internal, size, data);
        else glTexSubImage3D(tg, level, x, y, z, w, h, d, f->format, f->type, data);
        break;
    }
}

/* PR #41: this renderer consumes packaged texels once, at the deferred first upload. Capture its source before publishing
 * the texture so that later hashing and upload do not depend on the caller keeping the original storage mapped. */
static int tex_capture_packaged(Tex *t) {
    if (!t->b.packaged || !t->data_size) return 1;
    t->packaged_copy = malloc(t->data_size);
    if (!t->packaged_copy) return 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), P(t->b.packaged), t->packaged_copy, t->data_size, &got) || got != t->data_size) {
        free(t->packaged_copy); t->packaged_copy = NULL; return 0;
    }
    return 1;
}
static const uint8_t *tex_source(const Tex *t) {
    return t->packaged_copy ? t->packaged_copy : t->b.packaged ? (const uint8_t *)P(t->b.packaged) : t->mem;
}
static void tex_release_packaged(Tex *t) {
    free(t->packaged_copy); t->packaged_copy = NULL;
}

static uint64_t mem_hash(const uint8_t *p, size_t n, int full) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ n, w;
    size_t words = n / 8, step = full || words <= 8192 ? 1 : words / 4096;
    for (size_t i = 0; i < words; i += step) { memcpy(&w, p + i * 8, 8); h = (h ^ w) * 0x100000001B3ULL; h ^= h >> 29; }
    return h;
}

/* ---- which texture memory did the game write? (P3)
 * A texture's texels are guest memory, and NVN has no call that says "I changed them", so every texture bound in a batch
 * used to be hashed in every batch (35 MB per frame in a battle; large block-linear ones were only sampled, so a small
 * change could be missed). The game's heap is reserved with MEM_WRITE_WATCH (nnsdk.c, nn::os::AllocateAddressRegion):
 * GetWriteWatch names the pages written since the previous question. Rule:
 *   - no page of the texture's range written since this texture was last looked at -> the GL copy is current, nothing to do;
 *   - otherwise the whole range is hashed (every word) and uploaded if the hash changed, as before.
 * Asking resets the answer for those pages, and two textures can share a page (small textures) or the whole range (two
 * NVN textures on the same storage), so every written page gets a stamp in a table and a texture compares the newest stamp
 * of its pages with the stamp it saw last. Memory that is not write-watched (the image's own data, host heap) is hashed
 * as before. SSBU_NO_WRITEWATCH=1 restores the old behaviour everywhere. Render thread only. */
#define WW_SLOTS (1u << 20)
static struct { uint64_t page; uint64_t stamp; } *g_ww_tab;
static uint64_t g_ww_stamp, g_ww_used, g_ww_floor;       /* g_ww_floor: stamp given to every page when the table had to be emptied */
static int g_ww_off = -1;
static LONG64 g_ww_clean, g_ww_dirty, g_ww_unwatched;
static uint64_t *ww_slot(uint64_t page, int insert) {
    for (unsigned h = (unsigned)((page * 0x9E3779B97F4A7C15ULL) >> 44);; h = (h + 1) & (WW_SLOTS - 1)) {
        if (g_ww_tab[h].page == page) return &g_ww_tab[h].stamp;
        if (!g_ww_tab[h].page) {
            if (!insert) return NULL;
            g_ww_tab[h].page = page; g_ww_used++;
            return &g_ww_tab[h].stamp;
        }
    }
}
/* Newest write stamp of the pages of [p, p+n), or 0 when the memory is not write-watched. Never returns less than 1 otherwise. */
static uint64_t ww_newest(const uint8_t *p, size_t n) {
    static void *pages[16384];
    ULONG_PTR count = 16384;
    DWORD gran = 0;
    if (g_ww_off < 0) { g_ww_off = getenv("SSBU_NO_WRITEWATCH") != NULL; g_ww_tab = calloc(WW_SLOTS, sizeof *g_ww_tab); g_ww_stamp = 1; }
    if (g_ww_off || !n || GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)p, n, pages, &count, &gran)) return 0;
    uint64_t first = (uint64_t)(uintptr_t)p >> 12, last = ((uint64_t)(uintptr_t)p + n - 1) >> 12, newest = g_ww_floor ? g_ww_floor : 1;
    if (g_ww_used > WW_SLOTS / 2) { memset(g_ww_tab, 0, WW_SLOTS * sizeof *g_ww_tab); g_ww_used = 0; g_ww_floor = ++g_ww_stamp; newest = g_ww_floor; }      /* forget: everything counts as written once */
    if (count) {
        uint64_t stamp = ++g_ww_stamp;
        if (count == 16384) {                      /* more written pages than the list holds: stamp the whole range */
            ResetWriteWatch((void *)p, n);
            for (uint64_t pg = first; pg <= last; pg++) *ww_slot(pg, 1) = stamp;
        } else for (ULONG_PTR i = 0; i < count; i++) *ww_slot((uint64_t)(uintptr_t)pages[i] >> 12, 1) = stamp;
    }
    for (uint64_t pg = first; pg <= last; pg++) { uint64_t *s = ww_slot(pg, 0); if (s && *s > newest) newest = *s; }
    return newest;
}
/* Round 16 (nvn_vk_draw.c, SSBU_VK_CBKEEP): the same question for any guest range: has the CPU written a page of it since
 * the stamp the caller remembers? 0 = the memory is not write-watched. Render thread only. */
uint64_t nvn_front_mem_newest(const void *p, size_t n) { return ww_newest(p, n); }

static void tex_upload_memory(Tex *t, const Fmt *f) {
    static uint8_t *tmp;
    static size_t tmp_cap;
    static int nlog;
    TexBuilder *b = &t->b;
    const uint8_t *src = tex_source(t);
    if (b->flags & 0x10) {                         /* linear: plain rows with a stride */
        int64_t stride = b->stride ? b->stride : (int64_t)b->width * f->bpb;
        if (f->kind == 4) return;
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)(stride / f->bpb));
        tex_put(t, f, 0, 0, 0, 0, 0, b->width, b->height, 1, src);
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
        g_st.tex_bytes += stride * b->height;
        return;
    }
    TexLayout L;
    const TexFormat *tf = tex_format(b->format);
    Fmt sw = *f;
    tex_lay(b, &L);
    if (tf && tf->sw_decode) { sw.kind = 0; sw.format = 0x1908; sw.type = 0x1401; sw.bw = sw.bh = 1; sw.bpb = 4; }     /* decoded in software to RGBA8 */
    for (int layer = 0; layer < L.layers; layer++) {
        for (int l = 0; l < t->glevels && l < L.levels; l++) {
            const uint8_t *p = tex_level_ptr(&L, src, l, layer);
            size_t lin = L.lvl_linear_size[l], need = lin + (tf && tf->sw_decode ? (size_t)L.lvl_w[l] * L.lvl_h[l] * 4 : 0);
            if (!b->packaged && t->limit && p + L.lvl_size[l] > t->limit) {
                logf_limited(&nlog, 20, "nvn_gl: texture %dx%d format 0x%x level %d layer %d runs past its memory pool; not uploaded\n", b->width, b->height, b->format, l, layer);
                return;
            }
            if (need > tmp_cap) { tmp_cap = need * 2; tmp = realloc(tmp, tmp_cap); }
            tex_deswizzle_level(&L, l, p, tmp);
            if (tf && tf->sw_decode) {
                tex_decode_rgba8(b->format, tmp, L.lvl_w[l], L.lvl_h[l], tmp + lin);
                tex_put(t, &sw, l, layer, 0, 0, 0, L.lvl_w[l], L.lvl_h[l], 1, tmp + lin);
            } else tex_put(t, f, l, layer, 0, 0, 0, L.lvl_w[l], L.lvl_h[l], L.lvl_d[l], tmp);
            g_st.tex_bytes += (LONG64)lin;
        }
    }
}

/* Make the GL texture exist and hold what the guest memory holds. */
static void tex_sync(Tex *t) {
    Fmt f;
    if (!t->o.gl) tex_realize(t);
    if (t->label_dirty && t->o.gl) { t->label_dirty = 0; if (glObjectLabel) glObjectLabel(0x1702 /* TEXTURE */, t->o.gl, -1, nvn_label_get(t->guest)); }
    if (t->checked == g_serial) return;
    t->checked = g_serial;
    const uint8_t *src = tex_source(t);
    if (t->gpu_written || !src || t->b.samples > 1 || !fmt_get(t->b.format, &f) || (f.kind >= 1 && f.kind <= 3)) return;
    if (!t->b.packaged && t->limit && src + t->data_size > t->limit) return;
    if (t->b.packaged && t->uploaded) return;      /* packaged data is consumed once at creation */
    LONG64 pf0 = pf_now();
    HS_IN("texture check (write watch, hash)", 0);
    uint64_t newest = ww_newest(src, t->data_size);
    if (newest) {
        if (t->uploaded && !t->dirty && t->ww_hashed && newest <= t->ww_seen) { g_ww_clean++; pf_add(PF_TEXHASH, pf0); HS_OUT(); return; }      /* not written since the last look */
        t->ww_seen = newest;
        g_ww_dirty++;
    } else g_ww_unwatched++;
    uint64_t h = mem_hash(src, t->data_size, newest != 0 || (t->b.flags & 0x10) != 0);
    int same = t->uploaded && !t->dirty && h == t->hash && (!newest || t->ww_hashed);
    t->ww_hashed = newest != 0;                    /* t->hash is the hash of every word (needed to compare the next time) */
    pf_add(PF_TEXHASH, pf0); g_pf_hash_bytes += (LONG64)t->data_size;
    HS_OUT();
    if (same) return;
    while (glGetError()) {}
    pf0 = pf_now();
    { HS_IN("texture upload (deswizzle, decode, glTexSubImage)", 0); tex_upload_memory(t, &f); HS_OUT(); }
    pf_add(PF_TEXUP, pf0);
    GLenum e = glGetError();
    static int nlog;
    if (e) logf_limited(&nlog, 20, "nvn_gl: upload of texture %dx%d format 0x%x target %d failed with GL error 0x%x\n", t->b.width, t->b.height, t->b.format, t->b.target, e);
    t->hash = h; t->uploaded = 1; t->dirty = 0;
    tex_release_packaged(t);
    g_st.tex_uploads++;
}

static void eview_base(Tex *t, EView *e) {
    memset(e, 0, sizeof *e);
    e->target = t->b.target; e->format = t->b.format; e->nlvl = t->glevels; e->nlay = tex_layers(&t->b); e->dsmode = t->b.dsmode;
    memcpy(e->swz, t->b.swz, 4);
}
/* The GL texture object that stands for `t` seen through `v` (the texture itself when the view changes nothing). */
static TexObj *tex_view_obj(Tex *t, const View *v) {
    EView base, e;
    eview_base(t, &base);
    e = base;
    if (!v || !(v->set & 0x80)) return &t->o;
    if (v->set & 1) { e.lvl0 = v->lvl0; e.nlvl = v->nlvl; }
    if (v->set & 2) { e.lay0 = v->lay0; e.nlay = v->nlay; }
    if ((v->set & 4) && v->format) e.format = v->format;
    if (v->set & 8) memcpy(e.swz, v->swz, 4);
    if (v->set & 16) e.dsmode = v->dsmode;
    if (v->set & 32) e.target = v->target;
    if (e.lvl0 < 0 || e.lvl0 >= base.nlvl) e.lvl0 = 0;
    if (e.nlvl <= 0 || e.lvl0 + e.nlvl > base.nlvl) e.nlvl = base.nlvl - e.lvl0;
    if (e.lay0 < 0 || e.lay0 >= base.nlay) e.lay0 = 0;
    if (e.nlay <= 0 || e.lay0 + e.nlay > base.nlay) e.nlay = base.nlay - e.lay0;
    if (t->b.target == 2 || t->b.samples > 1) { e.target = base.target; e.lay0 = 0; e.nlay = base.nlay; }      /* no layer views of 3D / multisample */
    if ((e.target == 1 || e.target == 7 || e.target == 0) && e.nlay > 1) e.nlay = 1;
    if (e.target == 8 && e.nlay != 6) e.target = e.nlay == 1 ? 1 : 4;
    if (e.target == 9 && e.nlay % 6) e.target = 4;
    if (!memcmp(&e, &base, sizeof e)) return &t->o;
    for (VNode *n = t->views; n; n = n->next) if (!memcmp(&n->e, &e, sizeof e)) return &n->o;
    VNode *n = calloc(1, sizeof *n);
    Fmt f;
    static int nlog;
    n->e = e; n->next = t->views; t->views = n;
    if (!fmt_get(e.format, &f)) fmt_get(t->b.format, &f);
    GLenum tg = gl_target(e.target, t->b.samples);
    while (glGetError()) {}
    glGenTextures(1, &n->o.gl);
    glTextureView(n->o.gl, tg, t->o.gl, f.internal, (GLuint)e.lvl0, (GLuint)e.nlvl, (GLuint)e.lay0, (GLuint)e.nlay);
    GLenum err = glGetError();
    if (err) {
        logf_limited(&nlog, 30, "nvn_gl: view (target %d format 0x%x levels %d+%d layers %d+%d) of texture (target %d format 0x%x %dx%d) refused: GL error 0x%x\n",
                     e.target, e.format, e.lvl0, e.nlvl, e.lay0, e.nlay, t->b.target, t->b.format, t->b.width, t->b.height, err);
        glDeleteTextures(1, &n->o.gl);
        n->o.gl = t->o.gl; n->o.alias = 1;
        return &n->o;
    }
    glBindTexture(tg, n->o.gl);
    if (tg != GL_TEXTURE_2D_MULTISAMPLE_ && tg != GL_TEXTURE_2D_MULTISAMPLE_ARRAY_) {
        glTexParameteri(tg, GL_TEXTURE_MIN_FILTER, e.nlvl > 1 ? GL_NEAREST_MIPMAP_NEAREST : GL_NEAREST);
        glTexParameteri(tg, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        set_swizzle(tg, e.swz);
        if (e.dsmode == 1) glTexParameteri(tg, GL_DEPTH_STENCIL_TEXTURE_MODE_, GL_STENCIL_INDEX_);
    }
    g_st.views++;
    return &n->o;
}

static void texobj_free(TexObj *o) {
    for (int i = 0; i < o->nh; i++) glMakeTextureHandleNonResidentARB(o->h[i].h);
    free(o->h);
    o->h = NULL; o->nh = 0;
    if (o->gl && !o->alias) { fbo_purge(o->gl); glDeleteTextures(1, &o->gl); }
    o->gl = 0;
}
static void tex_free_gl(Tex *t) {
    for (VNode *n = t->views, *next; n; n = next) { next = n->next; texobj_free(&n->o); free(n); }
    t->views = NULL;
    texobj_free(&t->o);
    tex_release_packaged(t);
}

/* GL sampler objects are shared by value and never deleted. */
static struct { SamplerParams p; GLuint gl; } g_samplers[512];
static int g_sampler_count;
static GLuint sampler_gl(const SamplerParams *p) {
    static const GLenum minf[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR, GL_LINEAR_MIPMAP_LINEAR};
    static const GLenum wrap[8] = {GL_CLAMP, GL_REPEAT, 0x8742, 0x8743, 0x8912, 0x812D, 0x8370, 0x812F};
    for (int i = 0; i < g_sampler_count; i++) if (!memcmp(&g_samplers[i].p, p, sizeof *p)) return g_samplers[i].gl;
    GLuint s = 0;
    glGenSamplers(1, &s);
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, (GLint)minf[p->min < 6 ? p->min : 1]);
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, p->mag ? GL_LINEAR : GL_NEAREST);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (GLint)wrap[p->ws & 7]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (GLint)wrap[p->wt & 7]);
    glSamplerParameteri(s, GL_TEXTURE_WRAP_R_, (GLint)wrap[p->wr & 7]);
    glSamplerParameterf(s, GL_TEXTURE_MIN_LOD_, p->lodmin);
    glSamplerParameterf(s, GL_TEXTURE_MAX_LOD_, p->lodmax);
    glSamplerParameterf(s, GL_TEXTURE_LOD_BIAS_, p->bias);
    {   /* ARB_bindless_texture: a sampler used for a texture handle may only have one of four border colours,
         * (0,0,0,0) (0,0,0,1) (1,1,1,0) (1,1,1,1); anything else makes glGetTextureSamplerHandleARB fail with
         * INVALID_OPERATION and the texture is then not bound at all. The nearest allowed colour is used instead. */
        float b[4] = {p->border[0], p->border[1], p->border[2], p->border[3]};
        float mx = b[0] > b[1] ? b[0] : b[1];         /* a depth or one-channel texture only looks at red: (1,0,0,0) must stay 1 */
        if (b[2] > mx) mx = b[2];
        float rgb = mx >= 0.5f ? 1.0f : 0.0f, al = b[3] >= 0.5f ? 1.0f : 0.0f;
        if (b[0] != rgb || b[1] != rgb || b[2] != rgb || b[3] != al) {
            static int said;
            if (said++ < 4) printf("nvn_gl: sampler border colour (%g, %g, %g, %g) is not usable with bindless handles: using (%g, %g, %g, %g)\n", b[0], b[1], b[2], b[3], rgb, rgb, rgb, al);
            b[0] = b[1] = b[2] = rgb; b[3] = al;
        }
        if (p->border_kind && gl_has(glSamplerParameterIiv, "glSamplerParameterIiv")) {       /* integer border colour (SetBorderColori / ui): the same four values only */
            GLint bi[4];
            memcpy(bi, p->border, 16);
            GLint rgbi = bi[0] || bi[1] || bi[2], ali = bi[3] != 0;
            bi[0] = bi[1] = bi[2] = rgbi; bi[3] = ali;
            if (p->border_kind == 1) glSamplerParameterIuiv(s, GL_TEXTURE_BORDER_COLOR, (const GLuint *)bi); else glSamplerParameterIiv(s, GL_TEXTURE_BORDER_COLOR, bi);
        } else glSamplerParameterfv(s, GL_TEXTURE_BORDER_COLOR, b);
    }
    if (p->aniso > 1.0f) glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_, p->aniso);
    if (p->reduction == 1 || p->reduction == 2) glSamplerParameteri(s, 0x9366 /* TEXTURE_REDUCTION_MODE_ARB */, p->reduction == 1 ? 0x8007 : 0x8008);   /* min / max instead of average */
    if (p->cmode == 1) {
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_MODE_, GL_COMPARE_REF_TO_TEXTURE_);
        glSamplerParameteri(s, GL_TEXTURE_COMPARE_FUNC_, GL_NEVER + (p->cfunc ? p->cfunc - 1 : 7));
    }
    if (g_sampler_count < 512) { g_samplers[g_sampler_count].p = *p; g_samplers[g_sampler_count++].gl = s; }
    return s;
}

/* NVN texture handle (texture ID | sampler ID << 20, bit 32 set: dev_texture_handle, as [sdk 0x41bc80]) -> resident GL bindless handle. */
static uint64_t resolve_handle(TexPool *tp, SamplerPool *sp, uint64_t nh, uint32_t *dims, int *isint) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;   /* real layout [sdk 0x41bc80]; DrawTexture splits it the same way [0x4382f0] */
    Tex *t = NULL;
    View v;
    SamplerParams p;
    int have_sampler = 0;
    if (!nh || !tp) return g_dummy_handle;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { t = tp->e[tid].tex; v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { p = sp->e[sid]; have_sampler = 1; }
    ReleaseSRWLockShared(&g_pool_lock);
    if (!t || t->dead) return g_dummy_handle;
    tex_sync(t);
    TexObj *o = tex_view_obj(t, &v);
    {   /* what TXQ reports, and whether the shader must use an integer sampler */
        Fmt f;
        int l0 = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, nl = (v.set & 0x81) == 0x81 && v.nlvl > 0 ? v.nlvl : t->glevels - l0, is3d = t->b.target == 2;
        int layers = (v.set & 0x82) == 0x82 && v.nlay > 0 ? v.nlay : tex_layers(&t->b);
        dims[0] = (uint32_t)lvl(t->b.width, l0); dims[1] = (uint32_t)lvl(t->b.height, l0);
        dims[2] = is3d ? (uint32_t)lvl(t->b.depth, l0) : (uint32_t)(t->b.target == 9 ? layers / 6 : layers);
        dims[3] = (uint32_t)(nl > 0 ? nl : 1) | ((uint32_t)is3d << 16);
        if (fmt_get((v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, &f) && f.kind == 0 && (f.format == 0x8D94 || f.format == 0x8228 || f.format == 0x8D98 || f.format == 0x8D99))
            *isint = f.type == 0x1400 || f.type == 0x1402 || f.type == 0x1404 ? 2 : 1;
    }
    GLuint smp = have_sampler ? sampler_gl(&p) : 0;
    for (int i = 0; i < o->nh; i++) if (o->h[i].smp == smp) return o->h[i].h;
    uint64_t h = smp ? glGetTextureSamplerHandleARB(o->gl, smp) : glGetTextureHandleARB(o->gl);
    if (!h) return g_dummy_handle;
    glMakeTextureHandleResidentARB(h);
    o->h = realloc(o->h, sizeof *o->h * (size_t)(o->nh + 1));
    o->h[o->nh].smp = smp; o->h[o->nh++].h = h;
    g_st.handles++;
    return h;
}
/* The same lookup for nvnCommandBufferDrawTexture, which needs the OpenGL names: 0 = no live texture under that ID (resolve_handle
 * gives the 1x1 dummy there; a DrawTexture of it is reported instead). */
static int resolve_names(TexPool *tp, SamplerPool *sp, uint64_t nh, GLuint *tex, GLuint *smp) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;   /* real layout [sdk 0x41bc80]; DrawTexture splits it the same way [0x4382f0] */
    Tex *t = NULL;
    View v;
    SamplerParams p;
    int have_sampler = 0;
    if (!tp) return 0;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { t = tp->e[tid].tex; v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { p = sp->e[sid]; have_sampler = 1; }
    ReleaseSRWLockShared(&g_pool_lock);
    if (!t || t->dead) return 0;
    tex_sync(t);
    *tex = tex_view_obj(t, &v)->gl;
    *smp = have_sampler ? sampler_gl(&p) : 0;
    return 1;
}

/* ---------------------------------------------------------------- framebuffers */
typedef struct { GLuint color[8], depth; int n, dkind; } FboKey;
static struct { FboKey k; GLuint fbo; int w, h, used; } g_fbos[256];
static int g_fbo_next;
static void fbo_purge(GLuint tex) {
    for (int i = 0; i < 256; i++) {
        if (!g_fbos[i].used) continue;
        int hit = g_fbos[i].k.depth == tex;
        for (int j = 0; j < 8; j++) hit |= g_fbos[i].k.color[j] == tex;
        if (hit) { glDeleteFramebuffers(1, &g_fbos[i].fbo); g_fbos[i].used = 0; }
    }
}
static GLuint fbo_get(const FboKey *k) {
    static int nlog;
    for (int i = 0; i < 256; i++) if (g_fbos[i].used && !memcmp(&g_fbos[i].k, k, sizeof *k)) return g_fbos[i].fbo;
    int slot = -1;
    for (int i = 0; i < 256 && slot < 0; i++) if (!g_fbos[i].used) slot = i;
    if (slot < 0) { slot = g_fbo_next++ & 255; glDeleteFramebuffers(1, &g_fbos[slot].fbo); }
    GLuint fbo = 0;
    GLenum bufs[8];
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER_, fbo);
    for (int i = 0; i < 8; i++) {
        bufs[i] = i < k->n && k->color[i] ? GL_COLOR_ATTACHMENT0_ + (GLenum)i : GL_NONE;
        if (i < k->n && k->color[i]) glFramebufferTexture(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_ + (GLenum)i, k->color[i], 0);
    }
    if (k->depth) glFramebufferTexture(GL_FRAMEBUFFER_, k->dkind == 3 ? GL_DEPTH_STENCIL_ATTACHMENT_ : k->dkind == 2 ? GL_STENCIL_ATTACHMENT_ : GL_DEPTH_ATTACHMENT_, k->depth, 0);
    glDrawBuffers(8, bufs);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER_);
    g_st.fbo_made++;
    if (st != GL_FRAMEBUFFER_COMPLETE_) {
        g_st.fbo_bad++;
        logf_limited(&nlog, 20, "nvn_gl: framebuffer incomplete (status 0x%x): %d colour targets (first gl %u), depth gl %u kind %d\n", st, k->n, k->color[0], k->depth, k->dkind);
    }
    g_fbos[slot].k = *k; g_fbos[slot].fbo = fbo; g_fbos[slot].used = 1;
    return fbo;
}

/* ---------------------------------------------------------------- streaming buffer: guest memory -> GL */
#define ARENA_SIZE ((size_t)96 << 20)
static GLuint g_arena;
static size_t g_arena_pos;
static struct { uint64_t addr, size; size_t off; uint32_t gen; } g_acache[8192];
static uint32_t g_arena_gen = 1;

static size_t arena_put_raw(const void *p, size_t n) {
    g_arena_pos = (g_arena_pos + 255) & ~(size_t)255;
    if (g_arena_pos + n > ARENA_SIZE) { HS_IN("buffer streaming: new 96 MB store (glBufferData)", 0); glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)ARENA_SIZE, NULL, GL_STREAM_DRAW_); HS_OUT(); g_arena_pos = 0; g_arena_gen++; }
    size_t off = g_arena_pos;
    LONG64 pf0 = pf_now();
    HS_IN("buffer streaming (glBufferSubData)", 0);
    glBufferSubData(GL_ARRAY_BUFFER_, (ptrdiff_t)off, (ptrdiff_t)n, p);
    HS_OUT();
    pf_add(PF_ARENA, pf0); g_pf_arena_bytes += (LONG64)n;
    g_arena_pos += n;
    g_st.arena_bytes += (LONG64)n;
    return off;
}
/* Offset in the streaming buffer of a copy of guest memory [addr, addr+size), made at most once per batch. */
static size_t arena_put(uint64_t addr, uint64_t size) {
    static int nlog;
    if (size > ARENA_SIZE / 4) { logf_limited(&nlog, 10, "nvn_gl: buffer range of %llu bytes clamped to %llu\n", (unsigned long long)size, (unsigned long long)(ARENA_SIZE / 4)); size = ARENA_SIZE / 4; }
    unsigned h = (unsigned)(((addr >> 4) ^ (size * 0x9E3779B1u)) * 2654435761u) >> 19;
    for (int probe = 0; probe < 8; probe++) {
        unsigned i = (h + (unsigned)probe) & 8191;
        if (g_acache[i].gen == g_arena_gen && g_acache[i].addr == addr && g_acache[i].size == size) return g_acache[i].off;
        if (g_acache[i].gen != g_arena_gen) { h = i; break; }
    }
    size_t off = arena_put_raw(P(addr), (size_t)size);      /* may start a new generation */
    h &= 8191;
    g_acache[h].addr = addr; g_acache[h].size = size; g_acache[h].off = off; g_acache[h].gen = g_arena_gen;
    return off;
}

/* ---------------------------------------------------------------- programs */
static const GLenum k_prog_target[6] = {GL_VERTEX_PROGRAM_ARB_, GL_FRAGMENT_PROGRAM_ARB_, GL_GEOMETRY_PROGRAM_NV_, GL_TESS_CONTROL_PROGRAM_NV_, GL_TESS_EVALUATION_PROGRAM_NV_, GL_COMPUTE_PROGRAM_NV_};
static const GLenum k_pbuf_target[6] = {0x8DA2, 0x8DA4, 0x8DA3, 0x8C74, 0x8C75, 0x90FC};
static const char *const k_stage_name[6] = {"vertex", "fragment", "geometry", "tess-control", "tess-eval", "compute"};
#define TEXTURE_HANDLE_BUFFER 14           /* program.buffer[14] of a stage holds the 64-bit handles set with BindTexture, 8 bytes per unit */
static int g_max_pbuf;

static int stage_of(const char *text, int len) {
    static const char *const tag[6] = {"!!NVvp", "!!NVfp", "!!NVgp", "!!NVtcp", "!!NVtep", "!!NVcp"};
    for (int i = 0; i < 6; i++) if (len > 8 && !memcmp(text, tag[i], strlen(tag[i]))) return i;
    return -1;
}
/* Keeps a copy of the program text with program.buffer[n] renumbered to slots 0,1,2.. in order of appearance. */
static void prog_add_stage(Prog *p, const char *text, int len, int serial) {
    static int nlog;
    len = (int)strnlen(text, (size_t)len);
    int s = stage_of(text, len);
    if (s < 0) { logf_limited(&nlog, 10, "nvn_gl: program #%d: shader text of unknown kind '%.12s'\n", serial, text); return; }
    Stage *st = &p->st[s];
    char *out = malloc((size_t)len + 64);
    int o = 0;
    st->nbuf = 0; st->storage = 0;
    for (int i = 0; i < len;) {
        if (len - i > 16 && !memcmp(text + i, "program.buffer[", 15)) {
            int n = 0, slot;
            i += 15;
            while (i < len && text[i] >= '0' && text[i] <= '9') n = n * 10 + (text[i++] - '0');
            for (slot = 0; slot < st->nbuf && st->map[slot] != n; slot++) {}
            if (slot == st->nbuf && st->nbuf < 16) st->map[st->nbuf++] = (uint8_t)n;
            o += sprintf(out + o, "program.buffer[%d", slot);
            InterlockedIncrement(&g_bufhist[s][n & 31]);
            continue;
        }
        if (len - i > 17 && !memcmp(text + i, "program.storage[", 16)) {
            int n = atoi(text + i + 16);
            st->storage |= 1u << (n & 31);
            InterlockedIncrement(&g_storhist[n & 31]);
        }
        out[o++] = text[i++];
    }
    out[o] = 0;
    st->storage_w = 0;
    {   /* storage buffers the program writes: STB / ATOMB / CSWAPB lines name "sbo_bufK", declared as STORAGE sbo_bufK[] = { program.storage[n] } */
        int8_t decl[32];
        memset(decl, -1, sizeof decl);
        for (const char *q = out; (q = strstr(q, "STORAGE sbo_buf")) != NULL; q += 15) {
            int k = atoi(q + 15);
            const char *r = strstr(q, "program.storage[");
            if (r && k >= 0 && k < 32) decl[k] = (int8_t)(atoi(r + 16) & 31);
        }
        for (const char *q = out; q && *q; q = strchr(q, 10), q = q ? q + 1 : NULL) {
            if (strncmp(q, "STB", 3) && strncmp(q, "ATOM", 4) && strncmp(q, "CSWAP", 5)) continue;
            const char *eol = strchr(q, 10), *r = strstr(q, "sbo_buf");
            if (r && (!eol || r < eol)) { int k = atoi(r + 7); if (k >= 0 && k < 32 && decl[k] >= 0) st->storage_w |= 1u << decl[k]; }
        }
    }
    free(st->text);
    st->text = out; st->len = o; st->state = 1;
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/p%05d_%s.txt", dir, serial, k_stage_name[s]);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(text, 1, (size_t)len, f); fclose(f); }
    }
}
static int stage_ready(Prog *p, int s) {
    static int nlog;
    Stage *st = &p->st[s];
    if (st->state == 2) return 1;
    if (st->state != 1) return 0;
    GLint pos = -1;
    glGenProgramsARB(1, &st->gl);
    glBindProgramARB(k_prog_target[s], st->gl);
    while (glGetError()) {}
    glProgramStringARB(k_prog_target[s], GL_PROGRAM_FORMAT_ASCII_ARB_, st->len, st->text);
    glGetIntegerv(GL_PROGRAM_ERROR_POSITION_ARB_, &pos);
    if (pos != -1 || glGetError()) {
        st->state = 3;
        g_st.prog_fail++;
        if (nlog < 12) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_%02d.txt", nlog);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(st->text, 1, (size_t)st->len, f); fclose(f); }
        }
        logf_limited(&nlog, 40, "nvn_gl: program #%d %s stage REJECTED at %d (%d buffers): %s\n", p->serial, k_stage_name[s], pos, st->nbuf, (const char *)glGetString(GL_PROGRAM_ERROR_STRING_ARB_));
        return 0;
    }
    st->state = 2;
    g_st.prog_ok++;
    free(st->text);
    st->text = NULL;
    return 1;
}

/* A shader that came without assembly text: Maxwell machine code (program header + 64-bit instruction words). */
static void dump_binary(Prog *p, int s) {
    const char *dir = getenv("SSBU_SHADERDUMP");
    char path[260];
    if (!dir) return;
    snprintf(path, sizeof path, "%s/b%05d_%s.bin", dir, p->serial, k_stage_name[s]);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(p->st[s].ctl, 1, 32, f); fwrite(p->st[s].code, 1, (size_t)p->st[s].code_len, f); fclose(f); }
}
static GLuint g_pipeline;
/* The GLSL program for a stage given as machine code; 0 when it cannot be translated or the driver refuses it. */
/* key: integer vertex inputs (all, signed) and integer textures (all, signed) as seen at this draw; a stage is translated
 * again when the bits it depends on change. */
/* P3: translated vertex / fragment stages are shared and kept (SSBU_NO_SHADERCACHE=1 turns this off).
 *   level 1: (stage, machine code, constants, the four key words) -> the translation's result and its GL program:
 *            a program object the game creates again from the same shader binary, or a stage that alternates between two
 *            keys, costs a table look-up instead of a translation and a driver compile;
 *   level 2: generated GLSL text -> GL program: two keys that lead to the same text share the program.
 * Shared GL programs are never deleted (a few thousand at most: the game has about 6,900 distinct stages). Per-draw state
 * of a program (the alpha-test uniform) is set at every draw that uses it, so sharing does not change what is drawn.
 * Entries are found by 64-bit hashes of the contents (plus length, stage and key compared exactly). */
typedef struct ShEnt { uint64_t h; int stage, code_len; uint32_t key[4]; GLuint prog; uint32_t cbufs, ssbos, in_attr, tex_units, ssbo_written; int uses_alpha; struct ShEnt *next; } ShEnt;
typedef struct ShSrc { uint64_t h; size_t len; GLuint prog; struct ShSrc *next; char *text; } ShSrc;
static ShSrc *g_prog_src[65536];                 /* P3 round 3: GL stage program -> its text record (pair programs) */
static GLuint g_pair_cur;                        /* the linked pair program in use (glUseProgram), 0 = none */
static void pair_off(void);
static ShEnt *g_shc[4096];
static ShSrc *g_shs[4096];
static int g_shc_off = -1;
static LONG64 g_shc_hit, g_shc_src_hit, g_shc_compiled;
static uint64_t sh_hash(uint64_t h, const void *data, size_t n) {
    const uint8_t *b = data;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x100000001B3ULL; h ^= h >> 31; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL;
    return h;
}
/* P3: the driver's compiled form of a translated stage is kept on disk. The driver needs 100-300 ms to compile one of
 * these stages (measured: about 500 new stages in a 200 s run with a battle, 8-60 ms per frame on average while they
 * arrive), which is what makes the frame time jump whenever something new comes on screen. With the binary on disk the
 * second and later runs load it in about a millisecond.
 *   file: <dir>/<hash of the GLSL text>_<text length>_<hash of GL_VENDOR, GL_RENDERER, GL_VERSION>.bin = format, size, bytes
 *   The key is the generated TEXT, so a change of the translator can never pick up an old binary; another driver or GPU
 *   gets other file names, and a binary the driver refuses (glProgramBinary leaves the program unlinked) is compiled again
 *   and rewritten. Files are written under a temporary name and renamed, so several running copies can share the folder.
 *   SSBU_SHADER_CACHE=<dir> chooses the folder (default: ..\\cache\\shaders, i.e. Documents\\SSBU\\cache\\shaders), SSBU_SHADER_CACHE=0
 *   turns it off (every stage is then compiled with glCreateShaderProgramv, as before). */
static LONG64 g_shd_loaded, g_shd_saved, g_shd_refused;
static volatile LONG64 g_shw_queued, g_shw_done, g_shw_compiled, g_shw_present, g_shw_failed;
static int g_shd_mode = -1;
static char g_shd_dir[200];
static uint64_t g_shd_drv;
static void shd_workers_start(void);
static void prec_load(void);
/* Render thread, once, after the context exists. */
static void shd_init(void) {
    const char *e = getenv("SSBU_SHADER_CACHE");
    g_shd_mode = !(e && !strcmp(e, "0")) && glCreateShader && glShaderSource && glCompileShader && glGetShaderiv && glAttachShader && glDetachShader && glDeleteShader &&
                 glCreateProgram && glLinkProgram && glProgramParameteri && glGetProgramBinary && glProgramBinary;
    if (!g_shd_mode) return;
    const char *v[3] = {(const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION)};
    snprintf(g_shd_dir, sizeof g_shd_dir, "%s", e && e[0] ? e : "../cache/shaders");      /* Documents\\SSBU\\cache\\shaders when run from recomp-study */
    if (!(e && e[0])) CreateDirectoryA("../cache", NULL);
    CreateDirectoryA(g_shd_dir, NULL);
    g_shd_drv = 0x243F6A8885A308D3ULL;
    for (int i = 0; i < 3; i++) if (v[i]) g_shd_drv = sh_hash(g_shd_drv, v[i], strlen(v[i]));
    printf("nvn_gl: shader binaries are kept in %s (SSBU_SHADER_CACHE=0 turns this off)%c", g_shd_dir, 10);
    shd_workers_start();
    prec_load();
}
static void shd_path(char *path, size_t size, uint64_t h, size_t len) {
    snprintf(path, size, "%s/%016llx_%zx_%016llx.bin", g_shd_dir, (unsigned long long)h, len, (unsigned long long)g_shd_drv);
}
/* Compile + link as a separable program (what glCreateShaderProgramv does, plus the hint that the binary will be asked
 * for) and write the binary to `path`. Returns the program, 0 when the shader does not compile. Any thread with a context. */
static GLuint shd_compile_save(GLenum type, const char *src, const char *path, GLint *ok, int *saved) {
    char tmp[360];
    GLint cs = 0, n = 0;
    GLuint sh = glCreateShader(type), q;
    *ok = 0; *saved = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, 0x8B81 /* COMPILE_STATUS */, &cs);
    if (!cs) { glDeleteShader(sh); return 0; }
    q = glCreateProgram();
    glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
    glProgramParameteri(q, 0x8257 /* PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
    glAttachShader(q, sh);
    glLinkProgram(q);
    glDetachShader(q, sh);
    glDeleteShader(sh);
    glGetProgramiv(q, 0x8B82, ok);
    if (!*ok) return q;
    glGetProgramiv(q, 0x8741 /* PROGRAM_BINARY_LENGTH */, &n);
    if (n > 0) {
        uint32_t head[2] = {0, 0};
        GLsizei got = 0;
        GLenum fmt = 0;
        void *data = malloc((size_t)n);
        while (glGetError()) {}
        glGetProgramBinary(q, n, &got, &fmt, data);
        if (!glGetError() && got > 0) {
            snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
            FILE *f = fopen(tmp, "wb");
            if (f) {
                head[0] = fmt; head[1] = (uint32_t)got;
                int good = fwrite(head, 4, 2, f) == 2 && fwrite(data, 1, (size_t)got, f) == (size_t)got;
                fclose(f);
                if (good && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) *saved = 1; else DeleteFileA(tmp);
            }
        }
        free(data);
    }
    return q;
}
/* Render thread: the program for this text, from disk when its binary is there. */
static GLuint shader_build(GLenum type, const char *src, size_t len, uint64_t h, GLint *ok) {
    char path[320];
    int saved = 0;
    *ok = 0;
    if (g_shd_mode <= 0) { GLuint q = glCreateShaderProgramv(type, 1, &src); glGetProgramiv(q, 0x8B82, ok); return q; }
    shd_path(path, sizeof path, h, len);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                GLuint q = glCreateProgram();
                while (glGetError()) {}
                HS_IN("shader binary from disk (glProgramBinary)", 0);
                glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, ok);
                HS_OUT();
                while (glGetError()) {}
                if (*ok) { free(data); fclose(f); g_shd_loaded++; return q; }
                glDeleteProgram(q);
                g_shd_refused++;
            }
            free(data);
        }
        fclose(f);
    }
    GLuint q;
    { HS_IN(type == 0x8B31 ? "shader compile + link, vertex stage (driver)" : type == 0x91B9 ? "shader compile + link, compute stage (driver)" : "shader compile + link, fragment stage (driver)", 0); q = shd_compile_save(type, src, path, ok, &saved); HS_OUT(); }
    if (!q) { q = glCreateShaderProgramv(type, 1, &src); glGetProgramiv(q, 0x8B82, ok); return q; }     /* does not compile: the old path reports it (its log holds the compiler's messages) */
    g_shd_saved += saved;
    return q;
}

/* ---- compiling ahead of the first draw (P3)
 * The game hands over a program's shaders (nvnProgramSetShaders) when it loads a fighter, a stage or an effect, usually
 * long before the first draw that uses it. Worker threads, each with a small OpenGL context of its own (nothing shared
 * with the render thread's context), translate those stages at once and put the driver's binary into the disk cache; when
 * the first draw comes, the render thread finds the binary there. Nothing is drawn differently: a stage whose binary is
 * not ready is compiled by the render thread exactly as before. The workers assume the common case for what a stage's
 * text can depend on (no integer vertex inputs or textures); a draw with another combination compiles its own text.
 * SSBU_SHADER_WORKERS=<n> (default 3, 0 = none). */
typedef struct ShJob { struct ShJob *next; int stage, code_len; uint32_t cdata_size; uint32_t key[4]; uint8_t data[1]; } ShJob;
/* P3 round 2: which text variants of a stage were needed. A stage's GLSL depends on which vertex inputs and textures hold
 * integers at the draw (the four key words); the workers used to compile the variant with none only, so a stage first
 * drawn with another combination was compiled by the render thread in the middle of a frame (0.3-1.3 s for a vertex
 * stage, hitch log). Every key the render thread had to compile for is appended to <cache>/variants.txt as
 * "<hash of code and constants> <stage> <key words>"; at the next start the workers compile those variants as well, as
 * soon as the game hands the stage over. Nothing is drawn differently: this only decides what is compiled ahead. */
typedef struct ShVar { uint64_t h; int stage; uint32_t key[4]; struct ShVar *next; } ShVar;
static ShVar *g_shv[4096];
static CRITICAL_SECTION g_shw_lock;
static LONG64 g_shv_loaded, g_shv_added, g_shv_queued;
static volatile LONG64 g_shw_held, g_shw_held_ticks;
static void shv_put(uint64_t h, int stage, const uint32_t *key) {
    ShVar *e = calloc(1, sizeof *e);
    e->h = h; e->stage = stage; memcpy(e->key, key, 16); e->next = g_shv[h & 4095]; g_shv[h & 4095] = e;
}
static int shv_has(uint64_t h, int stage, const uint32_t *key) {
    for (ShVar *e = g_shv[h & 4095]; e; e = e->next) if (e->h == h && e->stage == stage && !memcmp(e->key, key, 16)) return 1;
    return 0;
}
static void shv_load(void) {                       /* render thread, before the workers start */
    char path[260], line[200];
    snprintf(path, sizeof path, "%s/variants.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long h; int stage; uint32_t k[4];
        if (sscanf(line, "%llx %d %x %x %x %x", &h, &stage, &k[0], &k[1], &k[2], &k[3]) == 6 && stage >= 0 && stage < 2 && !shv_has(h, stage, k)) { shv_put(h, stage, k); g_shv_loaded++; }
    }
    fclose(f);
}
static void shv_record(uint64_t h, int stage, const uint32_t *key) {      /* render thread: this key had to be compiled here */
    char path[260];
    if (g_shd_mode <= 0 || !(key[0] | key[1] | key[2] | key[3])) return;
    EnterCriticalSection(&g_shw_lock);
    int known = shv_has(h, stage, key);
    if (!known) shv_put(h, stage, key);
    LeaveCriticalSection(&g_shw_lock);
    if (known) return;
    snprintf(path, sizeof path, "%s/variants.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %d %x %x %x %x\n", (unsigned long long)h, stage, key[0], key[1], key[2], key[3]); fclose(f); g_shv_added++; }
}
static CRITICAL_SECTION g_shw_lock;
static ShJob *g_shw_head, *g_shw_tail;
static HANDLE g_shw_sem;
static uint64_t g_shw_seen[16384];
static int g_shw_n;
/* ---- P3 round 4: the driver's work at the first draw of a shader pair, done ahead by the workers
 * What the driver does (measured with tools_p3/p4lab.c, P3_performance.md section 13): a program loaded with
 * glProgramBinary (or linked) is not ready to run; at the FIRST draw call that uses a (vertex, fragment) combination the
 * driver generates its code, 40-190 ms, without any debug message, under a lock that also stops the draw calls of every
 * other context of the process. It keeps the result in its own cache: in memory for the process (keyed by the CONTENT
 * of the two programs: other program objects, other contexts and other threads get it), and on disk in a file per
 * executable NAME (a copy of the exe under another name starts cold, the same name in another folder or relinked is
 * warm). Our program binaries do not contain it (read back after the draw and loaded under a new name: cold again).
 * So: a worker thread, in its small context of its own, loads the two programs and draws ONE point with the rasterizer
 * off into its own 8x8 window. Nothing of the game's frame is involved; when the render thread later draws with its
 * own program objects of the same binaries, the driver finds the pair in its memory cache (1 ms instead of 65).
 * Which pairs: (1) every pair a draw used in an earlier run (<cache>/pairs.txt), queued at start; (2) the vertex and
 * fragment stage of a program the game hands over (nvnProgramSetShaders) after start-up, or always with
 * SSBU_PAIR_WARM=2; the caller is held until its program is ready, as the stage compile already did.
 * When: the driver's lock means this work cannot hide behind the game (a 65 ms stall per cold pair on the render
 * thread), so while COLD work is pending (a warm draw or a compile took more than 8 ms) the render thread holds the
 * next present until the workers are done: the cost is paid as loading time, before the frames that need it.
 * With a warm driver cache a pair costs the workers about 2 ms and nothing is held. */
typedef struct PwSeen { uint64_t vh, fh; uint32_t vl, fl, mask; struct PwSeen *next; } PwSeen;
static PwSeen *g_pw_seen[4096];
static uint64_t g_pw_qseen[16384];
static CRITICAL_SECTION g_pw_stripe[64];
static volatile LONG g_pw_cold, g_shw_ctx_failed;
static volatile LONG64 g_pw_named, g_pw_progs, g_pw_warmed, g_pw_cold_n, g_pw_ticks, g_pw_missing, g_pw_dup, g_pw_gate_ticks, g_pw_gates, g_pw_compiled;
static int pw_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SSBU_PAIR_WARM"); m = e ? atoi(e) : 1; } return m; }
static int pw_seen(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask) {
    unsigned k = (unsigned)((vh ^ fh * 0x9E3779B97F4A7C15ULL) >> 20) & 4095;
    int found = 0;
    EnterCriticalSection(&g_shw_lock);
    for (PwSeen *e = g_pw_seen[k]; e && !found; e = e->next) found = e->vh == vh && e->fh == fh && e->vl == vl && e->fl == fl && e->mask == mask;
    if (!found) { PwSeen *e = calloc(1, sizeof *e); e->vh = vh; e->vl = vl; e->fh = fh; e->fl = fl; e->mask = mask; e->next = g_pw_seen[k]; g_pw_seen[k] = e; }
    LeaveCriticalSection(&g_shw_lock);
    return found;
}
static void pw_cold_mark(LONG64 t0) {
    static LONG64 freq;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    if ((pf_now() - t0) * 125 > freq) InterlockedExchange(&g_pw_cold, 1);          /* more than 8 ms */
}
/* Worker: the program for a stage text, from its binary; compiled (and saved) when there is none and the text is given. */
static GLuint pw_load(int stage, uint64_t th, size_t tl, const char *src) {
    char path[320];
    GLuint q = 0;
    GLint ok = 0;
    shd_path(path, sizeof path, th, tl);
    EnterCriticalSection(&g_pw_stripe[th & 63]);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                q = glCreateProgram();
                glProgramParameteri(q, 0x8258 /* PROGRAM_SEPARABLE */, 1);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, &ok);
                if (!ok) { glDeleteProgram(q); q = 0; }
            }
            free(data);
        }
        fclose(f);
    }
    if (!q && src) {
        int saved = 0;
        LONG64 t0 = pf_now();
        q = shd_compile_save(stage == 0 ? 0x8B31 : 0x8B30, src, path, &ok, &saved);
        if (q && !ok) { glDeleteProgram(q); q = 0; }
        if (saved) { InterlockedIncrement64(&g_shw_compiled); InterlockedIncrement64(&g_pw_compiled); }
        pw_cold_mark(t0);
    }
    LeaveCriticalSection(&g_pw_stripe[th & 63]);
    while (glGetError()) {}
    return q;
}
static char *pw_translate(int stage, const uint8_t *code, int code_len, const uint8_t *cdata, uint32_t cdata_size, uint32_t *in_attr) {
    MxIn in;
    MxOut out;
    memset(&in, 0, sizeof in);
    in.stage = stage; in.code = code; in.code_len = code_len;
    if (cdata_size) { in.cdata = cdata; in.cdata_size = cdata_size; }
    in.binding_base = (unsigned)stage * 20; in.ssbo_base = 40 + (unsigned)stage * 16;
    in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL;
    if (!mx_translate(&in, &out)) return NULL;
    if (in_attr) *in_attr = out.in_attr;
    return out.glsl;
}
/* <cache>/progs.txt: one line per program (hash of the machine code of its stages, as pw_queue_prog computes it) whose
 * pair a worker has made ready and written to pairs.txt. Read at start into the table pw_queue_prog looks at. */
static void pw_prog_known(const ShJob *j) {
    char path[260];
    uint64_t h;
    if (j->stage != 3 || getenv("SSBU_NO_PAIR_LIST")) return;
    memcpy(&h, &j->key[2], 8);
    snprintf(path, sizeof path, "%s/progs.txt", g_shd_dir);
    EnterCriticalSection(&g_shw_lock);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx\n", (unsigned long long)h); fclose(f); }
    LeaveCriticalSection(&g_shw_lock);
}
static LONG64 g_pw_progs_known;
static void pw_progs_load(void) {                  /* render thread, start, only when pairs.txt was read */
    char path[260], line[64];
    snprintf(path, sizeof path, "%s/progs.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    EnterCriticalSection(&g_shw_lock);
    while (fgets(line, sizeof line, f)) {
        unsigned long long h = 0;
        if (sscanf(line, "%llx", &h) != 1 || !h) continue;
        unsigned k = (unsigned)(h >> 20) & 16383;
        for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_pw_qseen[k] == h) break; if (!g_pw_qseen[k]) { g_pw_qseen[k] = h; g_pw_progs_known++; break; } }
    }
    LeaveCriticalSection(&g_shw_lock);
    fclose(f);
}
/* Worker: one pair job. stage 2 = named by the two text hashes (data: vh, fh, vl, fl); stage 3 = the machine code of a
 * program's vertex stage (code_len, cdata_size) and fragment stage (key[0], key[1]; 0 = none: depth-only). */
static void pw_job(ShJob *j, GLuint pipe, GLuint empty_fs) {
    uint64_t vh = 0, fh = 0;
    uint32_t vl = 0, fl = 0, mask = 0;             /* mask: the vertex arrays that are enabled for the warm draw */
    char *vt = NULL, *ft = NULL;
    if (j->stage == 2) { memcpy(&vh, j->data, 8); memcpy(&fh, j->data + 8, 8); memcpy(&vl, j->data + 16, 4); memcpy(&fl, j->data + 20, 4); memcpy(&mask, j->data + 24, 4); }
    else {
        const uint8_t *vc = j->data, *vd = vc + j->code_len, *fc = vd + j->cdata_size, *fd = fc + j->key[0];
        vt = pw_translate(0, vc, j->code_len, vd, j->cdata_size, &mask);      /* a program handed over: every input the vertex stage reads comes from an array */
        mask &= 0xffff;
        if (j->key[0]) ft = pw_translate(1, fc, (int)j->key[0], fd, j->key[1], NULL);
        if (!vt || (j->key[0] && !ft)) { free(vt); free(ft); InterlockedIncrement64(&g_pw_missing); return; }
        vl = (uint32_t)strlen(vt); vh = sh_hash(0x9E3779B97F4A7C15ULL ^ 0, vt, vl);
        if (ft) { fl = (uint32_t)strlen(ft); fh = sh_hash(0x9E3779B97F4A7C15ULL ^ 1, ft, fl); }
    }
    if (pw_seen(vh, vl, fh, fl, mask)) { free(vt); free(ft); InterlockedIncrement64(&g_pw_dup); pw_prog_known(j); return; }
    if (mask & 0x10000) {                          /* a compute program: one work group with no buffer and no image bound (reads give 0, writes go nowhere) */
        GLuint cs = pw_load(5, vh, vl, NULL);
        if (cs) {
            static LONG64 freq;
            if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
            glUseProgramStages(pipe, 0x20 /* COMPUTE_SHADER_BIT */, cs);
            LONG64 t0 = pf_now();
            glDispatchCompute(1, 1, 1);
            glFinish();
            LONG64 dt = pf_now() - t0;
            InterlockedExchangeAdd64(&g_pw_ticks, dt); InterlockedIncrement64(&g_pw_warmed);
            if (dt * 125 > freq) { InterlockedIncrement64(&g_pw_cold_n); InterlockedExchange(&g_pw_cold, 1); }
            if (g_fdlog) printf("fd-warm: %016llx %x %016llx %x mask %x kind %d %.1f ms frame %lld%c", (unsigned long long)vh, vl, 0ull, 0u, mask, j->stage, (double)dt * 1000.0 / (double)freq, (long long)g_st.frames, 10);
            glUseProgramStages(pipe, 0x20, 0);
            glDeleteProgram(cs);
        } else InterlockedIncrement64(&g_pw_missing);
        while (glGetError()) {}
        return;
    }
    GLuint vs = pw_load(0, vh, vl, vt), fs = fl ? pw_load(1, fh, fl, ft) : empty_fs;
    free(vt); free(ft);
    if (vs && fs) {
        glUseProgramStages(pipe, 1, vs);
        glUseProgramStages(pipe, 2, fs);
        for (GLuint k = 0; k < 16; k++) { if (mask >> k & 1) glEnableVertexAttribArray(k); else glDisableVertexAttribArray(k); }
        LONG64 t0 = pf_now();
        glDrawArrays(GL_POINTS, 0, 1);
        glFinish();
        LONG64 dt = pf_now() - t0;
        static LONG64 freq;
        if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
        InterlockedExchangeAdd64(&g_pw_ticks, dt); InterlockedIncrement64(&g_pw_warmed);
        if (dt * 125 > freq) { InterlockedIncrement64(&g_pw_cold_n); InterlockedExchange(&g_pw_cold, 1); }
        if (g_fdlog) printf("fd-warm: %016llx %x %016llx %x mask %x kind %d %.1f ms frame %lld%c", (unsigned long long)vh, vl, (unsigned long long)fh, fl, mask, j->stage, (double)dt * 1000.0 / (double)freq, (long long)g_st.frames, 10);
        glUseProgramStages(pipe, 1, 0);
        glUseProgramStages(pipe, 2, 0);
        if (j->stage == 3 && !getenv("SSBU_NO_PAIR_LIST")) {          /* into <cache>/pairs.txt (same line format as prec_note) */
            char path[260];
            snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
            EnterCriticalSection(&g_shw_lock);
            FILE *f = fopen(path, "a");
            if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)vh, (unsigned)vl, (unsigned long long)fh, (unsigned)fl, (unsigned)mask); fclose(f); }
            LeaveCriticalSection(&g_shw_lock);
            pw_prog_known(j);
        }
    } else InterlockedIncrement64(&g_pw_missing);
    if (vs) glDeleteProgram(vs);
    if (fs && fs != empty_fs) glDeleteProgram(fs);
    while (glGetError()) {}
}
static void pw_enqueue(ShJob *j) {
    j->next = NULL;
    EnterCriticalSection(&g_shw_lock);
    if (g_shw_tail) g_shw_tail->next = j; else g_shw_head = j;
    g_shw_tail = j;
    LeaveCriticalSection(&g_shw_lock);
    InterlockedIncrement64(&g_shw_queued);
    ReleaseSemaphore(g_shw_sem, 1, NULL);
}
/* Start-up (prec_load): a pair that a draw used in an earlier run. */
static void pw_queue_named(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask) {
    if (!pw_mode() || g_shd_mode <= 0 || !g_shw_n) return;
    ShJob *j = calloc(1, sizeof *j + 28);
    j->stage = 2;
    memcpy(j->data, &vh, 8); memcpy(j->data + 8, &fh, 8); memcpy(j->data + 16, &vl, 4); memcpy(j->data + 20, &fl, 4); memcpy(j->data + 24, &mask, 4);
    g_pw_named++;
    pw_enqueue(j);
}
static volatile LONG64 g_pw_gate_t0;
/* boot.c: milliseconds presents were held for shader preparation so far (not counted by the run-time limit). */
unsigned nvn_gl_held_ms(void) {
    LARGE_INTEGER f;
    LONG64 t0 = g_pw_gate_t0, ticks = g_pw_gate_ticks + (t0 ? pf_now() - t0 : 0);
    QueryPerformanceFrequency(&f);
    return (unsigned)(ticks * 1000 / f.QuadPart);
}
/* Render thread, before a present: hold it while cold shader work is pending (see above). */
static void pw_gate(void) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("SSBU_PAIR_GATE"); off = (e && !strcmp(e, "0")) || !pw_mode(); }
    if (off || !g_pw_cold) return;
    if (g_shw_queued == g_shw_done || g_shw_ctx_failed) { InterlockedExchange(&g_pw_cold, 0); return; }
    LONG64 t0 = pf_now(), last = t0, freq, start_done = g_shw_done;
    char title[200] = "", text[300];
    g_pw_gate_t0 = t0;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    HS_IN("shader preparation (present held while the workers prepare programs)", 0);
    while (g_shw_queued != g_shw_done) {
        Sleep(2);
        if (pf_now() - last > freq) {            /* once per second while it lasts: the log and the window title */
            if (!title[0] && !GetWindowTextA(g_hwnd, title, sizeof title)) strcpy(title, " ");
            last = pf_now();
            snprintf(text, sizeof text, "%s - preparing shaders: %lld to go", title, (long long)(g_shw_queued - g_shw_done));
            SetWindowTextA(g_hwnd, text);
            printf("nvn_gl: preparing shaders, present held for %.0f s so far: %lld jobs to go (%lld pairs made ready, %lld of them not in the driver's cache)%c", (double)(last - t0) / (double)freq,
                   (long long)(g_shw_queued - g_shw_done), (long long)g_pw_warmed, (long long)g_pw_cold_n, 10);
            fflush(stdout);
        }
    }
    HS_OUT();
    if (title[0]) SetWindowTextA(g_hwnd, title);
    InterlockedExchange(&g_pw_cold, 0);
    g_pw_gate_ticks += pf_now() - t0; g_pw_gates++;
    g_pw_gate_t0 = 0;
    printf("nvn_gl: shader preparation held the present of frame %lld for %.2f s (%lld jobs)%c", (long long)g_st.frames + 1, (double)(pf_now() - t0) / (double)freq, (long long)(g_shw_done - start_done), 10);
}
static DWORD WINAPI shd_worker(LPVOID arg) {
#ifndef _WIN32
    (void)arg; InterlockedIncrement(&g_shw_ctx_failed); return 0;      /* Linux build: no OpenGL renderer */
#else
    PIXELFORMATDESCRIPTOR pfd;
    (void)arg;
    HWND w = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 8, 8, NULL, NULL, NULL, NULL);
    HDC dc = w ? GetDC(w) : NULL;
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    if (!dc || !SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd)) return 0;
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { InterlockedIncrement(&g_shw_ctx_failed); return 0; }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    GLuint pw_pipe = 0, pw_empty = 0;                  /* P3 round 4: the pair jobs draw one discarded point through this pipeline */
    if (!getenv("SSBU_GL_DRIVER_THREAD")) { glEnable(GL_DEBUG_OUTPUT_); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_); }
    {   static const char *const src[1] = {"#version 450\nvoid main() {}\n"};
        static const float zero[64];
        GLuint vb = 0;
        glGenProgramPipelines(1, &pw_pipe);
        glBindProgramPipeline(pw_pipe);
        glEnable(GL_RASTERIZER_DISCARD_);
        pw_empty = glCreateShaderProgramv(0x8B30, 1, src);
        glGenBuffers(1, &vb);                          /* every vertex array reads this small buffer of zeros */
        glBindBuffer(GL_ARRAY_BUFFER_, vb);
        glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)sizeof zero, zero, GL_STREAM_DRAW_);
        glBindVertexBuffer(0, vb, 0, 16);
        for (GLuint k = 0; k < 16; k++) { glVertexAttribFormat(k, 4, 0x1406 /* FLOAT */, 0, 0); glVertexAttribBinding(k, 0); }
    }
    for (;;) {
        WaitForSingleObject(g_shw_sem, INFINITE);
        EnterCriticalSection(&g_shw_lock);
        ShJob *j = g_shw_head;
        if (j) { g_shw_head = j->next; if (!g_shw_head) g_shw_tail = NULL; }
        LeaveCriticalSection(&g_shw_lock);
        if (!j) continue;
        if (j->stage >= 2) { pw_job(j, pw_pipe, pw_empty); InterlockedIncrement64(&g_shw_done); free(j); continue; }
        MxIn in;
        MxOut out;
        memset(&in, 0, sizeof in);
        in.stage = j->stage; in.code = j->data; in.code_len = j->code_len;
        if (j->cdata_size) { in.cdata = j->data + j->code_len; in.cdata_size = j->cdata_size; }
        in.binding_base = (unsigned)j->stage * 20; in.ssbo_base = 40 + (unsigned)j->stage * 16;
        in.int_attrs = j->key[0]; in.sint_attrs = j->key[1]; in.int_tex = j->key[2]; in.sint_tex = j->key[3];
        in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL;
        if (mx_translate(&in, &out)) {
            char path[320];
            size_t len = strlen(out.glsl);
            shd_path(path, sizeof path, sh_hash(0x9E3779B97F4A7C15ULL ^ (uint64_t)j->stage, out.glsl, len), len);
            if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) InterlockedIncrement64(&g_shw_present);
            else {
                GLint ok = 0;
                int saved = 0;
                GLuint q = shd_compile_save(j->stage == 0 ? 0x8B31 : 0x8B30, out.glsl, path, &ok, &saved);
                if (q) glDeleteProgram(q);
                InterlockedExchange(&g_pw_cold, 1);            /* a compile: presents wait for the workers (pw_gate) */
                if (saved) InterlockedIncrement64(&g_shw_compiled); else InterlockedIncrement64(&g_shw_failed);
                while (glGetError()) {}
            }
            free(out.glsl);
        } else InterlockedIncrement64(&g_shw_failed);
        InterlockedIncrement64(&g_shw_done);
        free(j);
    }
#endif
}
static void shd_workers_start(void) {
    const char *e = getenv("SSBU_SHADER_WORKERS");
    g_shw_n = e ? atoi(e) : 3;
    if (g_shw_n < 0) g_shw_n = 0;
    if (g_shw_n > 8) g_shw_n = 8;
    InitializeCriticalSection(&g_shw_lock);
    for (int i = 0; i < 64; i++) InitializeCriticalSection(&g_pw_stripe[i]);
    if (!getenv("SSBU_NO_SHADER_VARIANTS")) shv_load();
    g_shw_sem = CreateSemaphoreA(NULL, 0, 0x7fffffff, NULL);
    for (int i = 0; i < g_shw_n; i++) CloseHandle(CreateThread(NULL, 0, shd_worker, NULL, 0, NULL));
}
/* Guest thread, nvnProgramSetShaders: a vertex or fragment stage given as machine code. */
static void shd_prewarm(int stage, const uint8_t *code, int code_len, const uint8_t *cdata, uint32_t cdata_size) {
    if (g_shd_mode <= 0 || !g_shw_n || stage > 1 || code_len <= 0) return;
    uint64_t h = sh_hash(0xcbf29ce484222325ULL ^ (uint64_t)stage, code, (size_t)code_len);
    if (cdata) h = sh_hash(h ^ cdata_size, cdata, cdata_size);
    if (!h) h = 1;
    ShJob *j = malloc(sizeof *j + (size_t)code_len + cdata_size);
    j->next = NULL; j->stage = stage; j->code_len = code_len; j->cdata_size = cdata ? cdata_size : 0;
    memset(j->key, 0, sizeof j->key);
    memcpy(j->data, code, (size_t)code_len);
    if (cdata) memcpy(j->data + code_len, cdata, cdata_size);
    EnterCriticalSection(&g_shw_lock);
    unsigned k = (unsigned)(h >> 20) & 16383;
    int seen = 0, extra = 0;
    for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_shw_seen[k] == h) { seen = 1; break; } if (!g_shw_seen[k]) { g_shw_seen[k] = h; break; } }
    if (!seen) {
        if (g_shw_tail) g_shw_tail->next = j; else g_shw_head = j;
        g_shw_tail = j;
        for (ShVar *e = g_shv[h & 4095]; e; e = e->next) {         /* the variants recorded in earlier runs (variants.txt) */
            if (e->h != h || e->stage != stage) continue;
            ShJob *v = malloc(sizeof *v + (size_t)code_len + cdata_size);
            memcpy(v, j, sizeof *v + (size_t)code_len + (cdata ? cdata_size : 0));
            v->next = NULL; memcpy(v->key, e->key, 16);
            g_shw_tail->next = v; g_shw_tail = v;
            extra++;
        }
    }
    LeaveCriticalSection(&g_shw_lock);
    if (seen) { free(j); return; }
    InterlockedExchangeAdd64(&g_shw_queued, 1 + extra);
    g_shv_queued += extra;
    ReleaseSemaphore(g_shw_sem, 1 + extra, NULL);
    /* P3 round 2: nvnProgramSetShaders holds its caller while every worker is busy compiling (not during start-up, where
     * the game hands over 6,800 stages at once and nothing of them is drawn yet). A stage or fighter loaded later brings
     * stages that were never compiled on this PC; without this the game finished loading while the workers were still
     * compiling, and the first draws compiled the same stages again on the render thread, in the middle of the battle
     * (7 vertex stages, 5.3 s, tests/logs/p3b_r2a.log). On the console the program is ready when this call returns; here
     * the loading takes as long as the compiler needs, once per stage per PC. SSBU_SHADER_NOWAIT=1 = never hold. */
    {
        static int nowait = -1;
        if (nowait < 0) nowait = getenv("SSBU_SHADER_NOWAIT") != NULL;
        if (!nowait && g_st.frames >= 300 && GetCurrentThreadId() != g_render_tid && g_shw_queued - g_shw_done >= g_shw_n) {
            LONG64 t0 = pf_now(), freq = 0;
            { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
            while (g_shw_queued - g_shw_done >= g_shw_n && pf_now() - t0 < 30 * freq) Sleep(1);
            InterlockedExchangeAdd64(&g_shw_held_ticks, pf_now() - t0); InterlockedIncrement64(&g_shw_held);
        }
    }
}
/* ---- P3 round 3: linked program pairs (SSBU_PAIR=1; default off until measured)
 * The two stages of a draw are separable programs in one pipeline object, and this driver does its last piece of work
 * for a (vertex, fragment) combination inside the first draw call that uses it (50-90 ms; it keeps the result in a
 * cache of its own that depends on the name of the executable). Here the same two TEXTS are also linked into one
 * program; its binary goes into our cache folder under the hash of both texts (file <hash>_<total length>_<driver>.bin,
 * same folder and driver key as the stage binaries, nothing in it depends on the executable). A draw uses the linked
 * program when it is there (same shaders, same bindings: every binding is fixed by a layout qualifier in the text; the
 * only uniform, the alpha test at location 0, is set on whichever program is in use); when the binary is not on disk
 * yet the draw goes through the pipeline exactly as before and the pair is linked for the next time. */
static int pair_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("SSBU_PAIR"); m = e ? atoi(e) : 0; } return m; }
typedef struct { uint64_t key; GLuint prog; int state; } PairEnt;      /* state 0 = free, 1 = ready, 2 = no linked program (pipeline) */
static PairEnt g_pairs[16384];
static LONG64 g_pair_loaded, g_pair_linked, g_pair_failed, g_pair_draws, g_pair_refused;
static void pair_off(void) { if (g_pair_cur) { glUseProgram(0); g_pair_cur = 0; } }
static GLuint pair_link(const char *vs, const char *fs, const char *path, int *saved) {
    GLint ok = 0, n = 0;
    GLuint sh[2] = {glCreateShader(0x8B31), glCreateShader(0x8B30)}, q = glCreateProgram();
    const char *src[2] = {vs, fs};
    *saved = 0;
    for (int i = 0; i < 2; i++) { glShaderSource(sh[i], 1, &src[i], NULL); glCompileShader(sh[i]); glAttachShader(q, sh[i]); }
    glProgramParameteri(q, 0x8257 /* PROGRAM_BINARY_RETRIEVABLE_HINT */, 1);
    glLinkProgram(q);
    for (int i = 0; i < 2; i++) { glDetachShader(q, sh[i]); glDeleteShader(sh[i]); }
    glGetProgramiv(q, 0x8B82, &ok);
    if (!ok) {
        static int nlog;
        char log[600] = "";
        glGetProgramInfoLog(q, sizeof log - 1, NULL, log);
        logf_limited(&nlog, 6, "nvn_gl: pair program not linked (the pipeline is used for this pair): %s\n", log);
        glDeleteProgram(q);
        return 0;
    }
    glGetProgramiv(q, 0x8741 /* PROGRAM_BINARY_LENGTH */, &n);
    if (n > 0) {
        char tmp[360];
        uint32_t head[2] = {0, 0};
        GLsizei got = 0;
        GLenum fmt = 0;
        void *data = malloc((size_t)n);
        while (glGetError()) {}
        glGetProgramBinary(q, n, &got, &fmt, data);
        if (!glGetError() && got > 0) {
            snprintf(tmp, sizeof tmp, "%s.%lu.%lu.tmp", path, (unsigned long)GetCurrentProcessId(), (unsigned long)GetCurrentThreadId());
            FILE *f = fopen(tmp, "wb");
            if (f) {
                head[0] = fmt; head[1] = (uint32_t)got;
                int good = fwrite(head, 4, 2, f) == 2 && fwrite(data, 1, (size_t)got, f) == (size_t)got;
                fclose(f);
                if (good && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) *saved = 1; else DeleteFileA(tmp);
            }
        }
        free(data);
    }
    return q;
}
/* Render thread, at a draw: the linked program for these two stage programs, 0 = use the pipeline. */
static GLuint pair_get(GLuint vs, GLuint fs, const char *fs_text_fixed) {
    uint64_t key = ((uint64_t)vs << 32) | fs;
    unsigned h = (unsigned)((key * 0x9E3779B97F4A7C15ULL) >> 50);
    PairEnt *e = NULL;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 16383) { if (g_pairs[h].state && g_pairs[h].key == key) return g_pairs[h].prog; if (!g_pairs[h].state) { e = &g_pairs[h]; break; } }
    if (!e || g_shd_mode <= 0) return 0;
    e->key = key; e->state = 2; e->prog = 0;
    ShSrc *a = vs < 65536 ? g_prog_src[vs] : NULL, *b = fs_text_fixed ? NULL : fs < 65536 ? g_prog_src[fs] : NULL;
    const char *ft = fs_text_fixed ? fs_text_fixed : b ? b->text : NULL;
    if (!a || !a->text || !ft) return 0;
    size_t flen = strlen(ft);
    char path[320];
    uint64_t ph = sh_hash(sh_hash(0x50414953ULL, a->text, a->len) ^ a->len, ft, flen);
    shd_path(path, sizeof path, ph, a->len + flen);
    FILE *f = fopen(path, "rb");
    if (f) {
        uint32_t head[2] = {0, 0};
        if (fread(head, 4, 2, f) == 2 && head[1] > 0 && head[1] < (64u << 20)) {
            void *data = malloc(head[1]);
            if (fread(data, 1, head[1], f) == head[1]) {
                GLint ok = 0;
                GLuint q = glCreateProgram();
                while (glGetError()) {}
                HS_IN("pair program binary from disk (glProgramBinary)", 0);
                glProgramBinary(q, head[0], data, (GLsizei)head[1]);
                glGetProgramiv(q, 0x8B82, &ok);
                HS_OUT();
                while (glGetError()) {}
                if (ok) { e->prog = q; e->state = 1; g_pair_loaded++; } else { glDeleteProgram(q); g_pair_refused++; }
            }
            free(data);
        }
        fclose(f);
        if (e->state == 1) return e->prog;
    }
    if (pair_mode() == 1) {            /* 1: link here, at the first draw (measurement of the idea); 2: only use what is on disk */
        int saved = 0;
        /* Both stage texts name their buffer blocks CB<n> / SB<n> (mx_translate.c) with different bindings; in ONE program
         * blocks of the same name are the same block, so the fragment stage's are renamed FB<n> / TB<n> for the link
         * (block names are not used anywhere else in the text; the members c<n> / s<n> belong to each stage's own block). */
        char *ft2 = _strdup(ft);
        for (char *p = ft2; (p = strstr(p, " buffer CB")) != NULL; p += 9) p[8] = 'F';
        for (char *p = ft2; (p = strstr(p, " buffer SB")) != NULL; p += 9) p[8] = 'T';
        /* ... and so are their members: the translator writes them only as c<n>[ and s<n>[ (whole identifiers), and no
         * identifier of the text starts with K or Q; an unnamed block's member is a global name of the whole program. */
        for (char *p = ft2 + 1; *p; p++) {
            if ((*p != 'c' && *p != 's') || p[1] < '0' || p[1] > '9') continue;
            char b = p[-1];
            if ((b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_' || b == '.') continue;
            char *e = p + 1;
            while (*e >= '0' && *e <= '9') e++;
            if (*e == '[') *p = *p == 'c' ? 'K' : 'Q';
        }
        HS_IN("pair program compile + link (driver)", 0);
        GLuint q = pair_link(a->text, ft2, path, &saved);
        HS_OUT();
        free(ft2);
        while (glGetError()) {}
        if (q) { e->prog = q; e->state = 1; g_pair_linked++; } else g_pair_failed++;
    }
    return e->prog;
}
/* ---- P3 round 4: the list of shader pairs that were drawn (<cache>/pairs.txt)
 * One line per (vertex text, fragment text, enabled arrays) combination a draw used: "<vs hash> <vs length> <fs hash>
 * <fs length> <mask>": the names the two stage binaries have in the cache folder (fragment 0 0 = the empty fragment
 * shader of a depth-only pass) and which of the vertex attribute arrays the vertex stage reads were enabled (the
 * driver's code for a pair depends on exactly that, p4lab mode M). Read at start (prec_load), appended to when a draw
 * uses a combination that is not in it. */
typedef struct PairRec { uint64_t vh, fh; uint32_t vl, fl, mask; struct PairRec *next; } PairRec;
static PairRec *g_prec[4096];
static LONG64 g_prec_loaded, g_prec_added;
static int g_prec_off = -1;
static int prec_has(uint64_t vh, uint32_t vl, uint64_t fh, uint32_t fl, uint32_t mask, int add) {
    unsigned k = (unsigned)((vh ^ fh * 0x9E3779B97F4A7C15ULL) >> 20) & 4095;
    for (PairRec *e = g_prec[k]; e; e = e->next) if (e->vh == vh && e->fh == fh && e->vl == vl && e->fl == fl && e->mask == mask) return 1;
    if (add) { PairRec *e = calloc(1, sizeof *e); e->vh = vh; e->vl = vl; e->fh = fh; e->fl = fl; e->mask = mask; e->next = g_prec[k]; g_prec[k] = e; }
    return 0;
}
/* Render thread, at a draw through the pipeline: note the pair of GL stage programs (fs_empty: depth-only pass). */
static void prec_note(GLuint vs, GLuint fs, int fs_empty, uint32_t mask) {
    static uint64_t seen[1 << 15];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off || fs >= 65536) return;
    uint64_t pair = ((uint64_t)vs << 32) | (uint64_t)(mask & 0xffff) << 16 | fs | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 49);
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0x7fff) { if (seen[h] == pair) return; if (!seen[h]) { seen[h] = pair; break; } }
    ShSrc *a = vs < 65536 ? g_prog_src[vs] : NULL, *b = fs_empty ? NULL : fs < 65536 ? g_prog_src[fs] : NULL;
    if (!a || (!b && !fs_empty)) return;
    if (prec_has(a->h, (uint32_t)a->len, b ? b->h : 0, b ? (uint32_t)b->len : 0, mask, 1)) return;
    char path[260];
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)a->h, (unsigned)a->len, (unsigned long long)(b ? b->h : 0), b ? (unsigned)b->len : 0, (unsigned)mask); fclose(f); g_prec_added++; }
}
/* Render thread: a compute program was built (its first dispatch follows). List line: <hash> <length> 0 0 10000. */
static void prec_note_compute(uint64_t h, size_t len) {
    char path[260];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off || prec_has(h, (uint32_t)len, 0, 0, 0x10000, 1)) return;
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "a");
    if (f) { fprintf(f, "%016llx %x %016llx %x %x\n", (unsigned long long)h, (unsigned)len, 0ull, 0u, 0x10000u); fclose(f); g_prec_added++; }
}
/* Render thread, start: the stage program of a listed pair is loaded from its binary now, not at its first draw. */
static LONG64 g_pre_n, g_pre_ticks;
static void prec_preload(int stage, uint64_t h, uint32_t len) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("SSBU_PAIR_PRELOAD"); off = (e && !strcmp(e, "0")) || g_shc_off > 0 || getenv("SSBU_NO_SHADERCACHE") != NULL; }
    if (off || !len) return;
    for (ShSrc *e = g_shs[h & 4095]; e; e = e->next) if (e->h == h && e->len == len) return;
    LONG64 t0 = pf_now();
    GLuint q = pw_load(stage, h, len, NULL);
    g_pre_ticks += pf_now() - t0;
    if (!q) return;
    ShSrc *e = calloc(1, sizeof *e);
    e->h = h; e->len = len; e->prog = q; e->next = g_shs[h & 4095]; g_shs[h & 4095] = e;
    if (q < 65536) g_prog_src[q] = e;
    g_pre_n++; g_shd_loaded++;
}
static void prec_load(void) {                      /* render thread, once (shd_init) */
    char path[260], line[200];
    if (g_prec_off < 0) g_prec_off = getenv("SSBU_NO_PAIR_LIST") != NULL || g_shd_mode <= 0;
    if (g_prec_off) return;
    snprintf(path, sizeof path, "%s/pairs.txt", g_shd_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        unsigned long long vh, fh; unsigned vl, fl, mask;
        if (sscanf(line, "%llx %x %llx %x %x", &vh, &vl, &fh, &fl, &mask) == 5 && !prec_has(vh, vl, fh, fl, mask, 1)) {
            g_prec_loaded++;
            pw_queue_named(vh, vl, fh, fl, mask);
            if (!(mask & 0x10000)) { prec_preload(0, vh, vl); prec_preload(1, fh, fl); }
        }
    }
    fclose(f);
    pw_progs_load();
    { LARGE_INTEGER q; QueryPerformanceFrequency(&q); printf("nvn_gl: %lld programs known by their machine code (progs.txt)%c", (long long)g_pw_progs_known, 10); }
    { LARGE_INTEGER q; QueryPerformanceFrequency(&q); printf("nvn_gl: %lld listed shader pairs queued for the workers; %lld stage binaries loaded ahead in %.0f ms%c", (long long)g_prec_loaded, (long long)g_pre_n, (double)g_pre_ticks * 1000.0 / (double)q.QuadPart, 10); }
}
static int stage_glsl(Prog *p, int s, const uint32_t *key) {
    static int nfail, nlink;
    Stage *st = &p->st[s];
    MxIn in;
    MxOut out;
    if (!st->code || s > 1) return 0;
    if (!st->dumped) { st->dumped = 1; g_st.binary_used++; dump_binary(p, s); }
    if (st->glsl_state == 2) return 0;
    uint32_t dep_a = s == 0 ? st->glsl_dep : 0;
    if (st->glsl_state == 1 && ((key[0] ^ st->glsl_key[0]) & dep_a) == 0 && ((key[1] ^ st->glsl_key[1]) & dep_a) == 0 &&
        ((key[2] ^ st->glsl_key[2]) & st->glsl_tex) == 0 && ((key[3] ^ st->glsl_key[3]) & st->glsl_tex) == 0) return 1;
    if (g_shc_off < 0) g_shc_off = getenv("SSBU_NO_SHADERCACHE") != NULL;
    if (st->glsl && g_shc_off) glDeleteProgram(st->glsl);
    st->glsl = 0;
    memset(&in, 0, sizeof in);
    in.stage = s; in.code = st->code; in.code_len = st->code_len;
    if (st->ctl[3] && st->ctl[4]) { in.cdata = st->code - 0x30 + st->ctl[4]; in.cdata_size = st->ctl[3]; }
    uint64_t h1 = 0, h0 = 0;
    if (!g_shc_off) {
        h1 = sh_hash(0xcbf29ce484222325ULL ^ (uint64_t)s, st->code, (size_t)st->code_len);
        if (in.cdata) h1 = sh_hash(h1 ^ in.cdata_size, in.cdata, in.cdata_size);
        h0 = h1 ? h1 : 1;                           /* the workers' name for this stage (shd_prewarm) */
        h1 = sh_hash(h1, key, 16);
        for (ShEnt *e = g_shc[h1 & 4095]; e; e = e->next)
            if (e->h == h1 && e->stage == s && e->code_len == st->code_len && !memcmp(e->key, key, 16)) {
                st->glsl = e->prog; st->glsl_state = 1;
                memcpy(st->glsl_key, key, sizeof st->glsl_key);
                st->glsl_cbufs = e->cbufs; st->glsl_ssbos = e->ssbos; st->glsl_dep = e->in_attr; st->glsl_tex = e->tex_units; st->glsl_alpha = e->uses_alpha; st->glsl_ssbo_w = e->ssbo_written;
                g_shc_hit++;
                return 1;
            }
    }
    in.int_attrs = key[0]; in.sint_attrs = key[1]; in.int_tex = key[2]; in.sint_tex = key[3];
    in.binding_base = (unsigned)s * 20; in.ssbo_base = 40 + (unsigned)s * 16;
    in.fragdebug = getenv("SSBU_FRAGDEBUG") != NULL;
    LONG64 pf0 = pf_now();
    int xl;
    { HS_IN("shader translate (machine code to GLSL)", 0); xl = mx_translate(&in, &out); HS_OUT(); }
    pf_add(PF_XLATE, pf0);
    if (!xl) {
        st->glsl_state = 2;
        g_st.glsl_fail++;
        logf_limited(&nfail, 60, "nvn_gl: program #%d %s stage: machine code not translated: %s\n", p->serial, k_stage_name[s], out.why);
        return 0;
    }
    char *src = out.glsl;
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/b%05d_%s.glsl", dir, p->serial, k_stage_name[s]);
        FILE *f = fopen(path, "wb");
        if (f) { fputs(src, f); fclose(f); }
    }
    const char *srcs[1] = {src};
    GLint ok = 0;
    size_t src_len = strlen(src);
    uint64_t h2 = g_shc_off ? 0 : sh_hash(0x9E3779B97F4A7C15ULL ^ (uint64_t)s, src, src_len);
    ShSrc *hit = NULL;
    for (ShSrc *e = g_shc_off ? NULL : g_shs[h2 & 4095]; e && !hit; e = e->next) if (e->h == h2 && e->len == src_len) hit = e;
    if (hit) { st->glsl = hit->prog; ok = 1; g_shc_src_hit++; }
    else {
        pf0 = pf_now();
        LONG64 saved0 = g_shd_saved;
        st->glsl = g_shc_off ? glCreateShaderProgramv(s == 0 ? 0x8B31 : 0x8B30, 1, srcs) : shader_build(s == 0 ? 0x8B31 : 0x8B30, src, src_len, h2, &ok);
        if (g_shc_off) glGetProgramiv(st->glsl, 0x8B82, &ok);
        pf_add(PF_GLCOMPILE, pf0);
        if (g_shd_saved != saved0 && h0) shv_record(h0, s, key);
        g_shc_compiled++;
        if (ok && !g_shc_off) { ShSrc *e = calloc(1, sizeof *e); e->h = h2; e->len = src_len; e->prog = st->glsl; e->next = g_shs[h2 & 4095]; g_shs[h2 & 4095] = e;
            if (st->glsl < 65536) { if (pair_mode()) e->text = _strdup(src); g_prog_src[st->glsl] = e; } }
    }
    if (!ok) {
        char log[2000] = "";
        glGetProgramInfoLog(st->glsl, sizeof log - 1, NULL, log);
        if (nlink < 8) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_glsl_%02d.txt", nlink);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(src, f); fclose(f); }
        }
        logf_limited(&nlink, 20, "nvn_gl: program #%d %s stage: the driver refuses the translated GLSL:\n%s\n", p->serial, k_stage_name[s], log);
        glDeleteProgram(st->glsl);
        st->glsl = 0; st->glsl_state = 2;
        g_st.glsl_fail++;
        free(src);
        return 0;
    }
    free(src);
    st->glsl_state = 1;
    memcpy(st->glsl_key, key, sizeof st->glsl_key);
    st->glsl_cbufs = out.cbufs; st->glsl_ssbos = out.ssbos; st->glsl_dep = out.in_attr; st->glsl_tex = out.tex_units; st->glsl_alpha = out.uses_alpha; st->glsl_ssbo_w = out.ssbo_written;
    if (out.ssbo_written) { static int nw; logf_limited(&nw, 5, "nvn_gl: program #%d %s stage writes storage buffers (mask 0x%x): results are not copied back to guest memory\n", p->serial, k_stage_name[s], out.ssbo_written); }
    if (!g_shc_off) {
        ShEnt *e = calloc(1, sizeof *e);
        e->h = h1; e->stage = s; e->code_len = st->code_len; memcpy(e->key, key, 16); e->prog = st->glsl;
        e->cbufs = out.cbufs; e->ssbos = out.ssbos; e->in_attr = out.in_attr; e->tex_units = out.tex_units; e->ssbo_written = out.ssbo_written; e->uses_alpha = out.uses_alpha;
        e->next = g_shc[h1 & 4095]; g_shc[h1 & 4095] = e;
    }
    g_st.glsl_ok++;
    return 1;
}

/* ---------------------------------------------------------------- deferred deletion (objects finalized on guest threads) */
static CRITICAL_SECTION g_dead_lock;
static struct { int kind; void *p; } *g_dead;
static int g_dead_n, g_dead_cap;
static void defer_free(int kind, void *p) {
    EnterCriticalSection(&g_dead_lock);
    if (g_dead_n == g_dead_cap) { g_dead_cap = g_dead_cap ? g_dead_cap * 2 : 256; g_dead = realloc(g_dead, sizeof *g_dead * (size_t)g_dead_cap); }
    g_dead[g_dead_n].kind = kind; g_dead[g_dead_n++].p = p;
    LeaveCriticalSection(&g_dead_lock);
}
static void gl_housekeeping(void) {
    if (!g_dead_n) return;
    EnterCriticalSection(&g_dead_lock);
    for (int i = 0; i < g_dead_n; i++) {
        if (g_dead[i].kind == K_TEX) tex_free_gl(g_dead[i].p);
        else if (g_dead[i].kind == K_PROG) {
            Prog *p = g_dead[i].p;
            for (int s = 0; s < 6; s++) { if (p->st[s].gl) glDeleteProgramsARB(1, &p->st[s].gl); if (p->st[s].glsl && (s > 1 || g_shc_off > 0)) glDeleteProgram(p->st[s].glsl); free(p->st[s].text); memset(&p->st[s], 0, sizeof p->st[s]); }
        }
    }
    g_dead_n = 0;
    LeaveCriticalSection(&g_dead_lock);
}

/* ---------------------------------------------------------------- command lists */
/* the OP_* list: nvn_front.h */
static const char *const k_op_name[OP_COUNT] = {
    "nop", "SetTexturePool", "SetSamplerPool", "BindProgram", "BindUniformBuffer", "BindStorageBuffer", "BindTexture", "BindVertexBuffer", "BindVertexAttribState",
    "BindVertexStreamState", "BindBlendState", "BindColorState", "BindChannelMaskState", "BindMultisampleState", "BindPolygonState", "BindDepthStencilState",
    "SetViewports", "SetScissors", "SetDepthRanges", "SetRenderTargets", "SetStencilRef", "SetStencilValueMask", "SetStencilMask", "SetBlendColor", "SetAlphaRef",
    "SetPointSize", "SetLineWidth", "SetPolygonOffsetClamp", "SetSampleMask", "SetRasterizerDiscard", "SetDepthClamp", "SetPrimitiveRestart", "SetPatchSize",
    "ClearColor", "ClearDepthStencil", "DrawArrays", "DrawElements", "CopyBufferToTexture", "CopyTextureToTexture", "Downsample", "CopyBufferToBuffer",
    "UpdateUniformBuffer", "ClearBuffer", "CallCommands", "SetCopyRowStride", "CopyTextureToBuffer", "DispatchCompute", "BindImage", "SetCopyImageStride", "SetDepthBounds", "SetConservativeRasterEnable", "SetConservativeRasterDilate",
    "SetSubpixelPrecisionBias", "SetViewportSwizzles", "SetTessellationLevels", "Barrier", "Discard", "ClearTexture", "DownsampleTextureView", "DrawIndirect",
    "DispatchComputeIndirect", "SetRenderEnable", "ResetCounter", "ReportCounter", "ReportValue", "FenceSync", "WaitSync", "SignalEvent", "WaitEvent",
    "BindCoverageModulationTable", "DebugGroup", "DrawTexture"};

typedef struct { uint8_t *data; size_t len; uint32_t gen, model_cmd, model_ctrl, dispatches; int finalized; LONG64 created, used; } List;
static List *g_lists;
static int g_list_count, g_list_cap, *g_list_free, g_list_free_n;
static SRWLOCK g_list_lock = SRWLOCK_INIT;

/* Track R1 round 5 (run-ahead, r1_state.c): every command buffer's host record is listed, so that a restore of the game's
 * state can put the recordings in progress back as they were at the save (the commands recorded so far are host data:
 * without this a speculative pass's commands would stay in the buffer and be submitted again by the real pass, or the
 * commands recorded before the save would be gone after the speculative pass ended the buffer). */
static CmdBuf *g_all_cb[512]; static int g_all_cb_n; static SRWLOCK g_all_cb_lock = SRWLOCK_INIT;
static CmdBuf *cb_new(uint64_t g) {
    CmdBuf *cb = hnew(g, K_CMD, sizeof(CmdBuf));
    AcquireSRWLockExclusive(&g_all_cb_lock);
    int i; for (i = 0; i < g_all_cb_n && g_all_cb[i]; i++) {}
    if (i < 512) { g_all_cb[i] = cb; if (i == g_all_cb_n) g_all_cb_n++; }
    ReleaseSRWLockExclusive(&g_all_cb_lock);
    return cb;
}
static void cb_forget(CmdBuf *cb) { AcquireSRWLockExclusive(&g_all_cb_lock); for (int i = 0; i < g_all_cb_n; i++) if (g_all_cb[i] == cb) g_all_cb[i] = NULL; ReleaseSRWLockExclusive(&g_all_cb_lock); }
typedef struct { CmdBuf *cb, copy; uint8_t *data; size_t cap; } CbSave;
static CbSave g_cbs[512]; static int g_ncbs;
/* (R3 pub1) r1_ring.c: the save area's addresses, without a linker map (the prebuilt runtime package has none) */
void nvn_gl_r1_cb_area(void **cbs, void **ncbs, void **all) { *cbs = g_cbs; *ncbs = &g_ncbs; *all = g_all_cb; }
size_t nvn_gl_r1_cb_size(void) { return sizeof(CbSave); }      /* (R3 pub1) the entry size r1_ring.c expects (RG_CBS_SIZE) */
size_t nvn_r1_cb_save(void) {                      /* the guest threads are held */
    size_t bytes = 0;
    AcquireSRWLockShared(&g_all_cb_lock);
    g_ncbs = g_all_cb_n;
    for (int i = 0; i < g_ncbs; i++) {
        CbSave *s = &g_cbs[i]; CmdBuf *cb = g_all_cb[i];
        s->cb = cb;
        if (!cb) continue;
        s->copy = *cb;
        if (cb->len) { if (s->cap < cb->len) { s->cap = cb->len + 65536; s->data = realloc(s->data, s->cap); } memcpy(s->data, cb->data, cb->len); bytes += cb->len; }
    }
    ReleaseSRWLockShared(&g_all_cb_lock);
    return bytes;
}
void nvn_r1_cb_restore(void) {
    AcquireSRWLockShared(&g_all_cb_lock);
    for (int i = 0; i < g_ncbs; i++) {
        CbSave *s = &g_cbs[i]; CmdBuf *cb = s->cb;
        if (!cb || g_all_cb[i] != cb) continue;           /* finalized meanwhile */
        uint8_t *d = cb->data; size_t cap = cb->cap;       /* (the saved record's buffer may belong to a list now: never reused) */
        *cb = s->copy;
        if (s->copy.len && cap < s->copy.len) { cap = s->copy.len + 65536; d = realloc(d, cap); }
        if (s->copy.len) memcpy(d, s->data, s->copy.len);
        cb->data = d; cb->cap = cap;
    }
    ReleaseSRWLockShared(&g_all_cb_lock);
}
static CmdBuf *cb_get(uint64_t g) {
    CmdBuf *cb = hobj(g, K_CMD);
    return cb ? cb : cb_new(g);
}
/* Appends a command with x1..x7 as its arguments and room for `blob` more bytes; returns it (valid until the next append). */
static Cmd *rec(Ctx *c, uint32_t op, size_t blob) {
    CmdBuf *cb = cb_get(A0);
    size_t need = sizeof(Cmd) + ((blob + 7) & ~(size_t)7);
    if (cb->len + need > cb->cap) { cb->cap = (cb->cap + need) * 2 + 4096; cb->data = realloc(cb->data, cb->cap); }
    {   /* Command memory as the game sees it. The real encoding is the driver's own; the model is 8 bytes per command plus its
         * array payload (control memory: 32 bytes per recording, charged by EndRecording). When the memory the game added is
         * used up, its callback is asked for more, as NVN does (NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY = 0). */
        uint64_t use = 8 + ((blob + 7) & ~(size_t)7);
        if (cb->cmd_size && cb->cmd_used + use > cb->cmd_size && cb->callback && !cb->callback_off && !cb->in_callback) {
            uint64_t sx[19];
            V128 sv[8];
            memcpy(sx, c->x, sizeof sx); memcpy(sv, c->v, sizeof sv);
            cb->in_callback = 1;
            c->x[1] = 0; c->x[2] = (use + 0xff) & ~0xffull; c->x[3] = cb->callback_data;
            call_guest(c, cb->callback);
            cb->in_callback = 0;
            memcpy(c->x, sx, sizeof sx); memcpy(c->v, sv, sizeof sv);
            if (cb->len + need > cb->cap) { cb->cap = (cb->cap + need) * 2 + 4096; cb->data = realloc(cb->data, cb->cap); }
        }
        cb->cmd_used += use; cb->rec_cmd += use;
    }
    Cmd *m = (Cmd *)(cb->data + cb->len);
    cb->len += need;
    m->op = op; m->size = (uint32_t)need;
    for (int i = 0; i < 7; i++) m->a[i] = c->x[i + 1];
    return m;
}

static uint64_t list_register(uint8_t *data, size_t len, uint32_t model_cmd, uint32_t model_ctrl, uint32_t dispatches) {
    AcquireSRWLockExclusive(&g_list_lock);
    int i;
    if (g_list_free_n) i = g_list_free[--g_list_free_n];
    else {
        if (g_list_count == g_list_cap) {
            g_list_cap = g_list_cap ? g_list_cap * 2 : 1024;
            g_lists = realloc(g_lists, sizeof *g_lists * (size_t)g_list_cap);
            g_list_free = realloc(g_list_free, sizeof *g_list_free * (size_t)g_list_cap);
            memset(g_lists + g_list_count, 0, sizeof *g_lists * (size_t)(g_list_cap - g_list_count));
        }
        i = g_list_count++;
    }
    g_lists[i].data = data; g_lists[i].len = len; g_lists[i].created = g_st.frames; g_lists[i].used = -1;
    g_lists[i].model_cmd = model_cmd; g_lists[i].model_ctrl = model_ctrl; g_lists[i].dispatches = dispatches; g_lists[i].finalized = 0;
    uint64_t h = ((uint64_t)(g_lists[i].gen & 0x7fffffff) << 32) | (uint32_t)(i + 1);
    ReleaseSRWLockExclusive(&g_list_lock);
    InterlockedIncrement64(&g_st.lists);
    return h;
}
static uint8_t *list_lookup(uint64_t h, size_t *len) {
    uint8_t *data = NULL;
    uint32_t i = (uint32_t)h - 1;
    AcquireSRWLockShared(&g_list_lock);
    if (h && i < (uint32_t)g_list_count && g_lists[i].data && (g_lists[i].gen & 0x7fffffff) == (uint32_t)(h >> 32)) { data = g_lists[i].data; *len = g_lists[i].len; g_lists[i].used = g_st.frames; }
    ReleaseSRWLockShared(&g_list_lock);
    return data;
}
/* Render thread, once per frame: drop lists not submitted for a while (NVN has no call that frees a command handle). */
static void list_collect(void) {
    AcquireSRWLockExclusive(&g_list_lock);
    for (int i = 0; i < g_list_count; i++) {
        List *l = &g_lists[i];
        if (!l->data) continue;
        LONG64 last = l->used >= 0 ? l->used : l->created;
        if (l->finalized || g_st.frames - last > 120) { free(l->data); l->data = NULL; l->gen++; g_list_free[g_list_free_n++] = i; }      /* finalized by the game, or (a handle the game never finalized) unused for two seconds */
    }
    ReleaseSRWLockExclusive(&g_list_lock);
}

/* ---------------------------------------------------------------- GPU state as left by the commands executed so far */
static struct {
    TexPool *tpool; SamplerPool *spool;
    Prog *prog[6];
    Range ubo[6][16], ssbo[6][32], vbuf[32];
    uint64_t nvn_img[6][8];                       /* image handles (nvnCommandBufferBindImage) */
    uint64_t nvn_tex[6][32], gl_tex[6][112];      /* constant buffer 2 as the shaders see it (mx_translate.h): 32 bytes reserved, one 64-bit handle per unit,
                                                     8 image handle slots (bytes 288..351, not read: images go to image units), then from byte 384
                                                     four words per unit: width, height, depth or layers, levels | is3d << 16 */
    uint32_t tex_int[6], tex_sint[6];             /* units holding integer formats (and which are signed) */
    int tex_dirty[6]; size_t tex_off[6]; uint32_t tex_gen[6];
    uint32_t vattr[32]; int nattr;
    struct { uint32_t stride, divisor; } vstream[32];
    uint64_t ds;
    uint32_t chanmask;
    int sref[2], svmask[2], swmask[2], stencil_dirty;
    float alpha_ref; int alpha_func;
    int have_fbo, fbo_w, fbo_h; GLuint fbo;
    Tex *color0, *color[8];
    int64_t row_stride, image_stride;
    int attr_enabled[16];
    uint8_t ms[24]; unsigned ms_ext;              /* the bound multisample state, and which of its extensions are on in OpenGL */
    int sc0[4], render_off, debug_depth, sample_shading;
} S;

static GLenum nvn_func(unsigned f) { return GL_NEVER + (f >= 1 && f <= 8 ? f - 1 : 7); }
static GLenum blend_func(unsigned f) {
    if (f >= 3 && f <= 0xB) return 0x300 + f - 3;
    switch (f) { case 1: return GL_ZERO; case 2: return GL_ONE; case 0x10: return 0x88F9; case 0x11: return 0x88FA; case 0x12: return 0x8589; case 0x13: return 0x88FB;
                 case 0x61: return 0x8001; case 0x62: return 0x8002; case 0x63: return 0x8003; case 0x64: return 0x8004; }
    return GL_ONE;
}
/* NVNdrawPrimitive has the OpenGL values for 0..9 (points .. polygon); adjacency 10..13 and patches 14 map to the GL enums.
 * Winding: glClipControl(GL_UPPER_LEFT) keeps OpenGL's rule (facing is decided as with a lower-left origin), and NVN is taken to
 * behave the same; SSBU_FLIPFACE inverts it for experiments (SSBU_DRAWLOG shows the samples each draw produces). */
static GLenum nvn_prim(unsigned p) {
    static const GLenum m[15] = {GL_POINTS, GL_LINES, GL_LINE_LOOP, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN, GL_QUADS, GL_QUAD_STRIP, GL_POLYGON,
                                 0x000A, 0x000B, 0x000C, 0x000D, 0x000E};
    return p < 15 ? m[p] : GL_TRIANGLES;
}
static GLenum blend_eq(unsigned e) { static const GLenum m[6] = {0x8006, 0x8006, 0x800A, 0x800B, 0x8007, 0x8008}; return m[e < 6 ? e : 1]; }
static GLenum stencil_op(unsigned o) { static const GLenum m[9] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, 0x8507, 0x8508}; return m[o < 9 ? o : 1]; }

static void apply_stencil(void) {
    if (!S.stencil_dirty) return;
    S.stencil_dirty = 0;
    glStencilFuncSeparate(GL_FRONT, nvn_func((unsigned)(S.ds >> 8) & 15), S.sref[0], (GLuint)S.svmask[0]);
    glStencilFuncSeparate(GL_BACK, nvn_func((unsigned)(S.ds >> 12) & 15), S.sref[1], (GLuint)S.svmask[1]);
    glStencilMaskSeparate(GL_FRONT, (GLuint)S.swmask[0]);
    glStencilMaskSeparate(GL_BACK, (GLuint)S.swmask[1]);
}
static void apply_chanmask(void) {
    for (unsigned i = 0; i < 8; i++) { unsigned m = S.chanmask >> (i * 4); glColorMaski(i, m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1); }
}

static void set_targets(int n, const TargetRef *col, const TargetRef *depth) {
    FboKey k;
    Fmt f;
    memset(&k, 0, sizeof k);
    S.have_fbo = 0; S.color0 = NULL; S.fbo_w = S.fbo_h = 0;
    memset(S.color, 0, sizeof S.color);
    for (int i = 0; i < n && i < 8; i++) {
        Tex *t = col[i].tex;
        if (!t || t->dead) continue;
        tex_sync(t);
        t->gpu_written = 1;
        k.color[i] = tex_view_obj(t, &col[i].view)->gl;
        k.n = i + 1;
        if (!S.fbo_w) { int l = (col[i].view.set & 0x81) == 0x81 ? col[i].view.lvl0 : 0; S.fbo_w = lvl(t->b.width, l); S.fbo_h = lvl(t->b.height, l); }
        if (i == 0) S.color0 = t;
        S.color[i] = t;
    }
    if (depth && depth->tex && !depth->tex->dead) {
        Tex *t = depth->tex;
        tex_sync(t);
        t->gpu_written = 1;
        k.depth = tex_view_obj(t, &depth->view)->gl;
        k.dkind = fmt_get(t->b.format, &f) ? f.kind : 1;
        if (!S.fbo_w) { S.fbo_w = t->b.width; S.fbo_h = t->b.height; }
    }
    if (!k.n && !k.depth) { glBindFramebuffer(GL_FRAMEBUFFER_, 0); return; }
    S.fbo = fbo_get(&k);
    S.have_fbo = 1;
    glBindFramebuffer(GL_FRAMEBUFFER_, S.fbo);
}

/* Everything a draw needs that is not plain GL state: programs, their buffers, texture handles, vertex streams. */
static void bind_handle_table(int s, GLenum target, GLuint index, int skip) {
    if (S.tex_dirty[s] || S.tex_gen[s] != g_arena_gen) {
        S.tex_off[s] = arena_put_raw(S.gl_tex[s], sizeof S.gl_tex[s]);
        S.tex_gen[s] = g_arena_gen; S.tex_dirty[s] = 0;
    }
    glBindBufferRange(target, index, g_arena, (ptrdiff_t)S.tex_off[s] + skip, skip ? (ptrdiff_t)256 : (ptrdiff_t)sizeof S.gl_tex[s]);
}
/* Debug aid SSBU_DRAWLOG=<frame>: state of every draw of that frame, and how many samples each draw produced. */
static LONG64 g_prev_frame_draws, g_drawlog_frame = -1, g_disp_frames, g_frame_dispatch;
static int g_drawlog_mindisp_n;     /* with SSBU_DRAWLOG_MIN=<n>: the first frame from <frame> on that follows a frame of at least n draws */
static int drawlog_now(void) {
    static int drawlog = -2, mindraws, mindisp;
    if (drawlog == -2) {
        drawlog = getenv("SSBU_DRAWLOG") ? atoi(getenv("SSBU_DRAWLOG")) : -1; mindraws = getenv("SSBU_DRAWLOG_MIN") ? atoi(getenv("SSBU_DRAWLOG_MIN")) : 0;
        mindisp = getenv("SSBU_DRAWLOG_MINDISP") ? atoi(getenv("SSBU_DRAWLOG_MINDISP")) : 0;
    }
    if (drawlog < 0) return 0;
    if (mindisp) {                             /* SSBU_DRAWLOG_MINDISP=<n>: SSBU_DRAWLOG then counts the frames that had at least n compute dispatches */
        if (g_drawlog_frame < 0 && g_disp_frames >= drawlog) g_drawlog_frame = g_st.frames;
        return g_st.frames == g_drawlog_frame;
    }
    if (!mindraws) return g_st.frames == drawlog;
    if (g_drawlog_frame < 0 && g_st.frames >= drawlog && g_prev_frame_draws >= mindraws) g_drawlog_frame = g_st.frames;
    return g_st.frames == g_drawlog_frame;
}
static GLuint g_dbg_query;
static struct { GLuint q; int active; uint64_t acc; } g_counter[17];
static void drawlog_begin(void) { if (!drawlog_now() || g_counter[1].active) return; if (!g_dbg_query) glGenQueries(1, &g_dbg_query); glBeginQuery(0x8914, g_dbg_query); }
static void drawlog_end(void) {
    GLuint n = 0;
    if (!drawlog_now() || g_counter[1].active) return;
    glEndQuery(0x8914);
    glGetQueryObjectuiv(g_dbg_query, 0x8866, &n);
    printf("  -> %u samples passed\n", n);
}
static int g_dbg_flipface;
static struct { uint64_t addr, size; int prog, n; } g_crange[1024];       /* storage ranges written by this frame's compute dispatches */
static int g_ncrange;
static int crange_hit(uint64_t addr, uint64_t size);
static void compute_flush(void);
static int g_dbg_noscissor, g_dbg_skipdraw = -1, g_frame_draw_index;      /* debug aids: SSBU_NOSCISSOR=1, SSBU_SKIPDRAW=<n-th draw of every frame> */
/* Audit measurement (fake audit, row "unbound or dead texture slots sample a 1x1 dummy texture"): draws whose bound
 * programs reference a texture unit (MxOut.tex_units of the translated stage) that holds the dummy texture; split by cause:
 * the game bound handle 0 to the unit / never bound it, or the handle names a texture that is gone or was never registered. */
static long long g_dummy_draws, g_dummy_unbound, g_dummy_dead;
static void count_dummy_samples(void) {
    int hit = 0;
    for (int s = 0; s < 6; s++) {
        uint32_t m = S.prog[s] ? S.prog[s]->st[s].glsl_tex : 0;
        for (int u = 0; m && u < 32; u++)
            if ((m >> u & 1) && S.gl_tex[s][4 + u] == g_dummy_handle) { hit = 1; if (S.nvn_tex[s][u]) g_dummy_dead++; else g_dummy_unbound++; }
    }
    g_dummy_draws += hit;
}
static int draw_prepare(void) {
    static int nlog;
    uint32_t ints = 0, sints = 0;
    GLuint pn_vs = 0, pn_fs = 0;                /* the pair of GLSL stage programs of this draw (prec_note) */
    int pn_empty = 0;
    compute_flush();
    if (!S.have_fbo || S.render_off) return 0;
    if (g_frame_draw_index++ == g_dbg_skipdraw) return 0;
    for (int i = 0; i < 16 && i < S.nattr; i++) {
        int size, norm, integer = 0;
        GLenum type;
        if ((S.vattr[i] & 0xff) && vfmt_get((int)(S.vattr[i] & 0xff), &size, &type, &norm, &integer) && integer) {
            ints |= 1u << i;
            if (type == 0x1400 || type == 0x1402 || type == 0x1404 || type == 0x8D9F) sints |= 1u << i;
        }
    }
    uint32_t key0[4] = {ints, sints, S.tex_int[0], S.tex_sint[0]}, key1[4] = {0, 0, S.tex_int[1], S.tex_sint[1]};
    /* preferred: both stages translated from machine code to GLSL (separable programs in one pipeline) */
    /* A program with a vertex stage only is a depth-only pass (shadow maps, depth pre-pass): NVN then writes depth and
     * stencil and no colour. Here it runs with an empty fragment shader. */
    int depth_only = S.prog[0] && !S.prog[1];
    if (S.prog[0] && !S.prog[2] && !S.prog[3] && !S.prog[4] && stage_glsl(S.prog[0], 0, key0) && (depth_only || stage_glsl(S.prog[1], 1, key1))) {
        static GLuint empty_fs;
        if (depth_only && !empty_fs) {
            static const char *const src[1] = {"#version 450\nvoid main() {}\n"};
            empty_fs = glCreateShaderProgramv(0x8B30, 1, src);
            printf("nvn_gl: first draw with a vertex stage only (program #%d): depth-only pass, empty fragment shader %u\n", S.prog[0]->serial, empty_fs);
        }
        for (int s = 0; s < 5; s++) glDisable(k_prog_target[s]);
        GLuint pair = pair_mode() ? pair_get(S.prog[0]->st[0].glsl, depth_only ? empty_fs : S.prog[1]->st[1].glsl, depth_only ? "#version 450\nvoid main() {}\n" : NULL) : 0;
        if (pair) { if (g_pair_cur != pair) { glUseProgram(pair); g_pair_cur = pair; } g_pair_draws++; }
        else {
        pair_off();
        glBindProgramPipeline(g_pipeline);
        glUseProgramStages(g_pipeline, 1, S.prog[0]->st[0].glsl);
        glUseProgramStages(g_pipeline, 2, depth_only ? empty_fs : S.prog[1]->st[1].glsl);
        }
        {   GLuint now[2] = {S.prog[0]->st[0].glsl, depth_only ? empty_fs : S.prog[1]->st[1].glsl};
            g_gs_draws++; g_gs_prog_same += now[0] == g_gs_last_prog[0] && now[1] == g_gs_last_prog[1];
            g_gs_last_prog[0] = now[0]; g_gs_last_prog[1] = now[1];
            pn_vs = now[0]; pn_fs = now[1]; pn_empty = depth_only; }
        glDisable(GL_ALPHA_TEST);
        for (int s = 0; s < (depth_only ? 1 : 2); s++)
            if (S.prog[s]->label_dirty) {          /* nvnProgramSetDebugLabel: the label goes to the OpenGL programs of its stages */
                Prog *lp = S.prog[s];
                lp->label_dirty = 0;
                for (int q = 0; q < 6; q++) if (lp->st[q].glsl && glObjectLabel) glObjectLabel(0x82E2 /* PROGRAM */, lp->st[q].glsl, -1, nvn_label_get(lp->guest));
            }
        {   /* nvnProgramSetSampleShading of the fragment program: 1 = run the fragment shader per sample, 2 = never, 0 = as the shader says */
            int ss = depth_only ? 0 : S.prog[1]->sample_shading;
            if (ss != S.sample_shading) { S.sample_shading = ss; if (ss == 1 && gl_has(glMinSampleShading, "glMinSampleShading")) { glEnable(0x8C36 /* SAMPLE_SHADING */); glMinSampleShading(1.0f); } else glDisable(0x8C36); }
        }
        if (!depth_only && S.prog[1]->st[1].glsl_alpha) { uint32_t ref; memcpy(&ref, &S.alpha_ref, 4); glProgramUniform2ui(pair ? pair : S.prog[1]->st[1].glsl, 0, (GLuint)S.alpha_func, ref); }
        for (int s = 0; s < (depth_only ? 1 : 2); s++) {
            Stage *st = &S.prog[s]->st[s];
            for (unsigned n = 0; n < 16; n++)
                if ((st->glsl_ssbos >> n & 1) && S.ssbo[s][n].addr && S.ssbo[s][n].size) {
                    size_t off = arena_put(S.ssbo[s][n].addr, S.ssbo[s][n].size);
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, 40 + (GLuint)s * 16 + n, g_arena, (ptrdiff_t)off, (ptrdiff_t)S.ssbo[s][n].size);
                }
            /* hardware constant buffers: 1 = constants stored after the shader's code, 2 = texture handles, 3.. = the uniform buffers bound by the game */
            for (unsigned n = 1; n < 18; n++) {
                Range r = {0, 0};
                if (!(st->glsl_cbufs >> n & 1)) continue;
                if (n == 2) { bind_handle_table(s, GL_SHADER_STORAGE_BUFFER_, (GLuint)s * 20 + n, 0); continue; }
                if (n == 1) { r.addr = (uint64_t)(uintptr_t)(st->code - 0x30 + st->ctl[4]); r.size = st->ctl[3]; } else r = S.ubo[s][n - 3];
                if (r.addr && r.size) {
                    size_t off = arena_put(r.addr, r.size);
                    glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, (GLuint)s * 20 + n, g_arena, (ptrdiff_t)off, (ptrdiff_t)r.size);
                    g_gs_buf_bind++;
                }
            }
        }
        g_st.glsl_draws++;
    } else {
    pair_off();
    glBindProgramPipeline(0);
    if (S.alpha_func && S.alpha_func != 8) { glEnable(GL_ALPHA_TEST); glAlphaFunc(nvn_func((unsigned)S.alpha_func), S.alpha_ref); } else glDisable(GL_ALPHA_TEST);
    if (!S.prog[0] || !stage_ready(S.prog[0], 0) || !S.prog[1] || !stage_ready(S.prog[1], 1)) {
        logf_limited(&nlog, 10, "nvn_gl: draw skipped: no usable vertex/fragment program (vertex #%d state %d, fragment #%d state %d)\n", S.prog[0] ? S.prog[0]->serial : -1,
                     S.prog[0] ? S.prog[0]->st[0].state : -1, S.prog[1] ? S.prog[1]->serial : -1, S.prog[1] ? S.prog[1]->st[1].state : -1);
        return 0;
    }
    uint32_t storage = 0;
    for (int s = 0; s < 5; s++) {
        Prog *p = S.prog[s];
        if (!p || !stage_ready(p, s)) { glDisable(k_prog_target[s]); continue; }
        Stage *st = &p->st[s];
        glEnable(k_prog_target[s]);
        glBindProgramARB(k_prog_target[s], st->gl);
        for (int i = 0; i < st->nbuf; i++) {
            int n = st->map[i];
            if (n == TEXTURE_HANDLE_BUFFER) {
                bind_handle_table(s, k_pbuf_target[s], (GLuint)i, 32);
            } else if (n < 16 && S.ubo[s][n].addr && S.ubo[s][n].size) {
                size_t off = arena_put(S.ubo[s][n].addr, S.ubo[s][n].size);
                glBindBufferRange(k_pbuf_target[s], (GLuint)i, g_arena, (ptrdiff_t)off, (ptrdiff_t)S.ubo[s][n].size);
            }
        }
        for (uint32_t m = st->storage & ~storage, n = 0; m; m >>= 1, n++)
            if ((m & 1) && S.ssbo[s][n].addr && S.ssbo[s][n].size) {
                size_t off = arena_put(S.ssbo[s][n].addr, S.ssbo[s][n].size);
                glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, n, g_arena, (ptrdiff_t)off, (ptrdiff_t)S.ssbo[s][n].size);
                storage |= 1u << n;
            }
    }
    }
    for (int i = 0; i < 16; i++) {
        uint32_t a = i < S.nattr ? S.vattr[i] : 0;
        int fmt = (int)(a & 0xff), stream = (int)((a >> 8) & 31), size, norm, integer, on = 0;
        GLenum type;
        if (fmt && S.vbuf[stream].addr && S.vbuf[stream].size && vfmt_get(fmt, &size, &type, &norm, &integer)) {
            if (integer) glVertexAttribIFormat((GLuint)i, size, type, a >> 14);
            else glVertexAttribFormat((GLuint)i, size, type, (GLboolean)norm, a >> 14);
            glVertexAttribBinding((GLuint)i, (GLuint)stream);
            g_gs_attr_set++; g_gs_attr_same += g_gs_last_attr[i] == a; g_gs_last_attr[i] = a;
            on = 1;
        }
        if (on != S.attr_enabled[i]) { if (on) glEnableVertexAttribArray((GLuint)i); else glDisableVertexAttribArray((GLuint)i); S.attr_enabled[i] = on; }
    }
    if (pn_vs) {
        uint32_t mask = 0;
        for (int i = 0; i < 16; i++) mask |= (uint32_t)(S.attr_enabled[i] != 0) << i;
        prec_note(pn_vs, pn_fs, pn_empty, mask & S.prog[0]->st[0].glsl_dep);
    }
    for (int i = 0; i < 16; i++)
        if (S.vbuf[i].addr && S.vbuf[i].size) {
            size_t off = arena_put(S.vbuf[i].addr, S.vbuf[i].size);
            glBindVertexBuffer((GLuint)i, g_arena, (ptrdiff_t)off, (GLsizei)S.vstream[i].stride);
            g_gs_vbuf_bind++;
            glVertexBindingDivisor((GLuint)i, S.vstream[i].divisor);
        }
    apply_stencil();
    if (g_ncrange) {
        int hit = 0;
        for (int i = 0; i < 16 && i < S.nattr; i++) {
            int stream = (int)((S.vattr[i] >> 8) & 31);
            if ((S.vattr[i] & 0xff) && S.vbuf[stream].addr && crange_hit(S.vbuf[stream].addr, S.vbuf[stream].size) >= 0) hit = 1;
        }
        g_st.draws_from_compute += hit;
    }
    if (drawlog_now()) {                    /* debug: what the draws of one frame look like */
        GLfloat vp[4];
        GLint sc[4], dfunc = 0;
        GLboolean cm[4];
        glGetFloatv(GL_VIEWPORT, vp); glGetIntegerv(GL_SCISSOR_BOX, sc); glGetBooleanv(GL_COLOR_WRITEMASK, cm); glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
        printf("draw: fbo %u (%dx%d) viewport %.0f,%.0f %.0fx%.0f scissor %d,%d %dx%d vs #%d fs #%d depth %d(func %x) stencil %d blend %d cull %d alpha %d mask %d%d%d%d\n", S.fbo, S.fbo_w, S.fbo_h,
               vp[0], vp[1], vp[2], vp[3], sc[0], sc[1], sc[2], sc[3], S.prog[0] ? S.prog[0]->serial : -1, S.prog[1] ? S.prog[1]->serial : -1, glIsEnabled(GL_DEPTH_TEST), dfunc,
               glIsEnabled(GL_STENCIL_TEST), glIsEnabled(GL_BLEND), glIsEnabled(GL_CULL_FACE), S.alpha_func, cm[0], cm[1], cm[2], cm[3]);
        for (int s = 0; s < 2; s++) {
            printf("  %s buffers:", k_stage_name[s]);
            for (int n = 0; n < 16; n++) if (S.ubo[s][n].addr) printf(" c%d=%llx+%llu", n, (unsigned long long)S.ubo[s][n].addr, (unsigned long long)S.ubo[s][n].size);
            for (int n = 0; n < 16; n++)
                if (S.ssbo[s][n].addr) {
                    const float *f = P(S.ssbo[s][n].addr);
                    printf(" storage%d=%llx+%llu%s [", n, (unsigned long long)S.ssbo[s][n].addr, (unsigned long long)S.ssbo[s][n].size, S.prog[s] && (S.prog[s]->st[s].glsl_ssbos >> n & 1) ? "(used)" : "");
                    for (unsigned k = 0; k < 16 && k * 4 < S.ssbo[s][n].size; k++) printf(" %g", f[k]);
                    printf(" ]");
                }
            printf("  textures:");
            for (int n = 0; n < 8; n++) if (S.nvn_tex[s][n]) printf(" %d:%llx%s", n, (unsigned long long)S.nvn_tex[s][n], S.gl_tex[s][4 + n] == g_dummy_handle ? "(dummy)" : "");
            printf("\n");
            for (int n = 0; n < 16; n++)
                if (S.ubo[s][n].addr && S.prog[s] && (S.prog[s]->st[s].glsl_cbufs >> (n + 3) & 1)) {
                    const float *f = P(S.ubo[s][n].addr);
                    printf("    c%d:", n);
                    static int full = -1;              /* SSBU_DRAWLOG_FULL=1: every float of the buffer, not the first 24 */
                    if (full < 0) full = getenv("SSBU_DRAWLOG_FULL") != NULL;
                    for (unsigned k = 0; k < (full ? 1024u : 24u) && k * 4 < S.ubo[s][n].size; k++) printf(" %g", f[k]);
                    printf("\n");
                }
            for (int n = 0; n < 8; n++) {
                uint32_t tid = (uint32_t)S.nvn_tex[s][n] & 0xfffff;
                Tex *t = S.tpool && tid && tid < (uint32_t)S.tpool->count ? S.tpool->e[tid].tex : NULL;
                static uint8_t seen[0x4000];
                if (!t) continue;
                printf("    texture %d: id 0x%x target %d format 0x%x %dx%dx%d levels %d flags 0x%x gl %u uploaded %d rendered %d view set 0x%x format 0x%x\n", n, tid, t->b.target, t->b.format, t->b.width,
                       t->b.height, t->b.depth, t->b.levels, t->b.flags, t->o.gl, t->uploaded, t->gpu_written, S.tpool->e[tid].view.set, S.tpool->e[tid].view.format);
                {   uint32_t sid = (uint32_t)(S.nvn_tex[s][n] >> 20) & 0xfff;
                    if (S.spool && sid < (uint32_t)S.spool->count) {
                        const SamplerParams *sp = &S.spool->e[sid];
                        printf("      sampler 0x%x: min %d mag %d wrap %d %d %d lod %g..%g border %g %g %g %g (kind %d)\n", sid, sp->min, sp->mag, sp->ws, sp->wt, sp->wr, sp->lodmin, sp->lodmax, sp->border[0], sp->border[1], sp->border[2], sp->border[3], sp->border_kind);
                    }
                }
                if (tid < sizeof seen && !seen[tid] && t->o.gl && t->b.target == 1 && t->b.samples <= 1 && t->b.width <= 4096) {
                    char path[64];
                    uint8_t *px = malloc((size_t)t->b.width * t->b.height * 4);
                    seen[tid] = 1;
                    glBindTexture(GL_TEXTURE_2D, t->o.gl);
                    glPixelStorei(GL_PACK_ALIGNMENT, 1);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
                    while (glGetError()) {}
                    snprintf(path, sizeof path, "tex_%04x_%dx%d.rgba", tid, t->b.width, t->b.height);
                    FILE *fp = fopen(path, "wb");
                    if (fp) { fwrite(px, 1, (size_t)t->b.width * t->b.height * 4, fp); fclose(fp); }
                    free(px);
                }
            }
        }
        for (int i = 0; i < 16 && i < S.nattr; i++) {
            uint32_t a = S.vattr[i];
            int stream = (int)((a >> 8) & 31);
            if (!(a & 0xff)) continue;
            printf("  attr %d: format 0x%x stream %d offset %u stride %u buffer %llx+%llu", i, a & 0xff, stream, a >> 14, S.vstream[stream].stride, (unsigned long long)S.vbuf[stream].addr, (unsigned long long)S.vbuf[stream].size);
            if (S.vbuf[stream].addr && S.vbuf[stream].size >= (a >> 14) + 16) { const float *f = (const float *)((const uint8_t *)P(S.vbuf[stream].addr) + (a >> 14)); printf("  first: %g %g %g %g", f[0], f[1], f[2], f[3]); }
            { int k = S.vbuf[stream].addr ? crange_hit(S.vbuf[stream].addr, S.vbuf[stream].size) : -1; if (k >= 0) printf("  <- storage %d written by compute program #%d (%llx+%llu)", g_crange[k].n, g_crange[k].prog, (unsigned long long)g_crange[k].addr, (unsigned long long)g_crange[k].size); }
            printf("\n");
        }
    }
    return 1;
}

/* ---------------------------------------------------------------- compute
 * nvnCommandBufferDispatchCompute(cmdbuf, groupsX, groupsY, groupsZ) runs the program bound to the compute stage with the
 * uniform buffers, storage buffers, textures and images bound to that stage. The game skins its fighter meshes this way:
 * per mesh one dispatch reads bind-pose vertices and bone matrices and writes posed vertices into a storage buffer that later
 * draws bind as a vertex buffer. Storage buffers are guest memory: a buffer the program writes is copied into a GL buffer,
 * the dispatch runs, and the result is copied back to guest memory, where the CPU and later draws read it.
 * Debug aids: SSBU_NOCOMPUTE=1 skips every dispatch (the state before this was implemented); the draw log (SSBU_DRAWLOG)
 * also lists every dispatch of the frame with its buffers, and marks vertex buffers that a dispatch of the frame wrote. */
static int g_dbg_nocompute;
static struct { int serial; LONG64 calls, run, groups; } g_chist[64];
/* Storage buffers written by dispatches live in GL buffers of their own from the first dispatch that writes them until the next
 * command that reads guest memory (a draw, a copy, the end of the batch): compute_flush() then copies the written ones back to
 * guest memory, once, however many dispatches wrote into them (a fighter's meshes are skinned by ~10 dispatches into one buffer). */
typedef struct { uint64_t addr, size; GLuint buf; int dirty, live; } CBuf;
static CBuf g_cbuf[64];
static int g_cbuf_live;
static CBuf *cbuf_find(uint64_t addr, uint64_t size) {
    for (int k = 0; k < 64; k++) if (g_cbuf[k].live && g_cbuf[k].addr == addr && g_cbuf[k].size == size) return &g_cbuf[k];
    return NULL;
}
static int cbuf_conflict(uint64_t addr, uint64_t size) {      /* overlaps a buffer held in GL without being that buffer */
    for (int k = 0; k < 64; k++)
        if (g_cbuf[k].live && addr < g_cbuf[k].addr + g_cbuf[k].size && g_cbuf[k].addr < addr + size && !(g_cbuf[k].addr == addr && g_cbuf[k].size == size)) return 1;
    return 0;
}
static void compute_flush(void) {
    int any = 0;
    if (!g_cbuf_live) return;
    for (int k = 0; k < 64; k++) {
        CBuf *b = &g_cbuf[k];
        if (!b->live) continue;
        if (b->dirty) { LONG64 pf0 = pf_now(); HS_IN("compute read-back (glGetNamedBufferSubData)", 0); glGetNamedBufferSubData(b->buf, 0, (ptrdiff_t)b->size, P(b->addr)); HS_OUT(); pf_add(PF_CFLUSH, pf0); g_pf_cflush_bytes += (LONG64)b->size; g_st.compute_bytes += (LONG64)b->size; any = 1; }
        b->live = b->dirty = 0;
    }
    g_cbuf_live = 0;
    nvn_front_ccmp_landed();
    if (any) g_arena_gen++;                      /* guest memory changed: later draws must stream it again */
}
static CBuf *cbuf_get(uint64_t addr, uint64_t size) {
    CBuf *b = cbuf_find(addr, size);
    if (b) return b;
    for (int k = 0; k < 64 && !b; k++) if (!g_cbuf[k].live) b = &g_cbuf[k];
    if (!b) { compute_flush(); b = &g_cbuf[0]; }
    if (!b->buf) glCreateBuffers(1, &b->buf);
    glNamedBufferData(b->buf, (ptrdiff_t)size, P(addr), 0x88EA /* DYNAMIC_COPY */);
    b->addr = addr; b->size = size; b->live = 1; b->dirty = 0;
    g_cbuf_live++;
    return b;
}
static int crange_hit(uint64_t addr, uint64_t size) {
    for (int k = 0; k < g_ncrange; k++) if (addr < g_crange[k].addr + g_crange[k].size && g_crange[k].addr < addr + size) return k;
    return -1;
}
static void mem_stats(const char *what, uint64_t addr, uint64_t size) {
    const uint8_t *p = P(addr);
    const float *f = P(addr);
    size_t nz = 0, n = size > (8u << 20) ? (8u << 20) : (size_t)size;
    for (size_t q = 0; q < n; q++) nz += p[q] != 0;
    printf("    %s %llx+%llu: %zu of %zu bytes non-zero, hash %016llx, first:", what, (unsigned long long)addr, (unsigned long long)size, nz, n, (unsigned long long)mem_hash(p, n, 1));
    for (unsigned k = 0; k < 12 && k * 4 + 4 <= size; k++) printf(" %g", f[k]);
    printf("\n");
}
#define WMASK_BINDING 90                   /* storage block that collects which storage buffers a translated compute program wrote */
static Tex *image_tex(int stage, int k, View *v) {            /* NVN image handle = texture ID in the texture pool (nvnDeviceGetImageHandle) */
    uint32_t tid = (uint32_t)S.nvn_img[stage][k];
    Tex *t = NULL;
    if (!S.tpool || !tid) return NULL;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)S.tpool->count) { t = S.tpool->e[tid].tex; *v = S.tpool->e[tid].view; }
    ReleaseSRWLockShared(&g_pool_lock);
    return t && !t->dead ? t : NULL;
}
/* The GLSL program of a compute stage given as machine code (translated again when the integer-ness of a texture it samples or
 * the format of an image it loads changes); 0 when it cannot be translated or the driver refuses it. */
static int stage_glsl_compute(Prog *p) {
    static int nfail, nlink;
    Stage *st = &p->st[5];
    MxIn in;
    MxOut out;
    uint16_t ifmt[8];
    if (!st->code || st->glsl_state == 2) return 0;
    for (int k = 0; k < 8; k++) {
        View v;
        Fmt f;
        Tex *t = image_tex(5, k, &v);
        ifmt[k] = t && fmt_get((v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, &f) ? f.internal : 0;
    }
    if (st->glsl_state == 1 && ((S.tex_int[5] ^ st->glsl_key[2]) & st->glsl_tex) == 0 && ((S.tex_sint[5] ^ st->glsl_key[3]) & st->glsl_tex) == 0) {
        int same = 1;
        for (int k = 0; k < 8; k++) if ((st->glsl_images >> k & 1) && st->glsl_img_key[k] != ifmt[k]) same = 0;
        if (same) return 1;
    }
    if (st->glsl) { glDeleteProgram(st->glsl); st->glsl = 0; }
    memset(&in, 0, sizeof in);
    in.stage = MX_COMPUTE; in.code = st->code; in.code_len = st->code_len;
    if (st->ctl[3] && st->ctl[4]) { in.cdata = st->code + st->ctl[4]; in.cdata_size = st->ctl[3]; }
    in.int_tex = S.tex_int[5]; in.sint_tex = S.tex_sint[5];
    in.binding_base = 0; in.ssbo_base = 40;        /* the bindings of the vertex stage: every draw and every dispatch binds what it uses */
    memcpy(in.local_size, st->local, sizeof in.local_size);
    in.smem_bytes = st->smem_bytes; in.lmem_bytes = st->lmem_bytes;
    in.image_base = 0; memcpy(in.image_format, ifmt, sizeof ifmt);
    in.track_writes = 1; in.wmask_binding = WMASK_BINDING;
    if (!mx_translate(&in, &out)) {
        st->glsl_state = 2;
        g_st.glsl_fail++;
        logf_limited(&nfail, 60, "nvn_gl: program #%d compute stage: machine code not translated: %s\n", p->serial, out.why);
        return 0;
    }
    const char *dir = getenv("SSBU_SHADERDUMP");
    if (dir) {
        char path[260];
        snprintf(path, sizeof path, "%s/b%05d_compute.glsl", dir, p->serial);
        FILE *f = fopen(path, "wb");
        if (f) { fputs(out.glsl, f); fclose(f); }
    }
    const char *srcs[1] = {out.glsl};
    GLint ok = 0;
    {   /* P3 round 2: compute stages go through the binary cache as well (a first dispatch cost 0.1-3.3 s, hitch log) */
        size_t clen = strlen(out.glsl);
        HS_IN("shader compile or load, compute stage", 0);
        if (g_shd_mode > 0) st->glsl = shader_build(0x91B9 /* COMPUTE_SHADER */, out.glsl, clen, sh_hash(0x9E3779B97F4A7C15ULL ^ 5u, out.glsl, clen), &ok);
        if (g_shd_mode > 0 && ok) prec_note_compute(sh_hash(0x9E3779B97F4A7C15ULL ^ 5u, out.glsl, clen), clen);
        else { st->glsl = glCreateShaderProgramv(0x91B9, 1, srcs); glGetProgramiv(st->glsl, 0x8B82, &ok); }
        HS_OUT();
    }
    if (!ok) {
        char log[2000] = "";
        glGetProgramInfoLog(st->glsl, sizeof log - 1, NULL, log);
        if (nlink < 4) {
            char path[64];
            snprintf(path, sizeof path, "shader_fail_glsl_c%d.txt", nlink);
            FILE *f = fopen(path, "wb");
            if (f) { fputs(out.glsl, f); fclose(f); }
        }
        logf_limited(&nlink, 20, "nvn_gl: program #%d compute stage: the driver refuses the translated GLSL:\n%s\n", p->serial, log);
        glDeleteProgram(st->glsl);
        st->glsl = 0; st->glsl_state = 2;
        g_st.glsl_fail++;
        free(out.glsl);
        return 0;
    }
    free(out.glsl);
    st->glsl_state = 1;
    st->glsl_key[2] = S.tex_int[5]; st->glsl_key[3] = S.tex_sint[5];
    memcpy(st->glsl_img_key, ifmt, sizeof ifmt); memcpy(st->glsl_img_fmt, out.image_gl_format, sizeof st->glsl_img_fmt);
    st->glsl_cbufs = out.cbufs; st->glsl_ssbos = out.ssbos; st->glsl_ssbo_w = out.ssbo_written; st->glsl_tex = out.tex_units;
    st->glsl_images = out.images; st->glsl_images_w = out.images_written; st->glsl_images_r = out.images_read;
    g_st.glsl_ok++;
    return 1;
}
static GLuint g_wmask_buf;
static void compute_bind_glsl(Prog *p) {
    static int nlog;
    Stage *st = &p->st[5];
    pair_off();
    glBindProgramPipeline(g_pipeline);
    glUseProgramStages(g_pipeline, 0x20 /* COMPUTE_SHADER_BIT */, st->glsl);
    for (unsigned n = 0; n < 18; n++) {
        Range r = {0, 0};
        if (!(st->glsl_cbufs >> n & 1)) continue;
        if (n == 0) { logf_limited(&nlog, 10, "nvn_gl: compute program #%d reads the driver's constant buffer 0 outside the storage buffer descriptors: not provided\n", p->serial); continue; }
        if (n == 2) { bind_handle_table(5, GL_SHADER_STORAGE_BUFFER_, n, 0); continue; }
        if (n == 1) { r.addr = (uint64_t)(uintptr_t)(st->code + st->ctl[4]); r.size = st->ctl[3]; } else r = S.ubo[5][n - 3];
        if (r.addr && r.size) {
            size_t off = arena_put(r.addr, r.size);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, n, g_arena, (ptrdiff_t)off, (ptrdiff_t)r.size);
        }
    }
    for (int k = 0; k < 8; k++) {
        View v;
        Fmt f;
        Tex *t;
        if (!(st->glsl_images >> k & 1)) continue;
        if (!(t = image_tex(5, k, &v))) { logf_limited(&nlog, 10, "nvn_gl: compute program #%d uses image %d but none is bound\n", p->serial, k); continue; }
        tex_sync(t);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, w = (st->glsl_images_w >> k & 1) != 0;
        GLenum fmt = st->glsl_img_fmt[k] ? st->glsl_img_fmt[k] : fmt_get(t->b.format, &f) ? f.internal : 0x8058;
        glBindImageTexture((GLuint)k, tex_view_obj(t, &v)->gl, level, GL_TRUE, 0, w ? ((st->glsl_images_r >> k & 1) ? 0x88BA : 0x88B9) : 0x88B8, fmt);
        if (w) { t->gpu_written = 1; t->uploaded = 1; }
    }
    if (st->glsl_ssbo_w) {
        static const uint32_t zero;
        if (!g_wmask_buf) glCreateBuffers(1, &g_wmask_buf);
        glNamedBufferData(g_wmask_buf, 4, &zero, 0x88EA);
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, WMASK_BINDING, g_wmask_buf, 0, 4);
    }
}
static void compute_dispatch(unsigned gx, unsigned gy, unsigned gz) {
    static int nlog;
    Prog *p = S.prog[5];
    int log = drawlog_now(), h;
    g_st.dispatches++; g_frame_dispatch++;
    for (h = 0; h < 63 && g_chist[h].serial && g_chist[h].serial != (p ? p->serial : -1); h++) {}
    g_chist[h].serial = p ? p->serial : -1; g_chist[h].calls++; g_chist[h].groups += (LONG64)gx * gy * gz;
    if (!p) { logf_limited(&nlog, 10, "nvn_gl: DispatchCompute with no compute program bound\n"); return; }
    Stage *st = &p->st[5];
    int text = st->state == 1 || st->state == 2, glsl = 0;
    if (st->state == 4 && g_chist[h].calls <= 2) log = 1;         /* the first dispatches of every machine-code program are always described */
    uint32_t used = 0, written = 0;
    { static int mc = -1; if (mc < 0) mc = getenv("SSBU_COMPUTE_MC") != NULL; if (mc && text && st->code) text = 0; }      /* run the translated machine code instead of the text */
    if (text && !g_dbg_nocompute && !stage_ready(p, 5)) text = 0;
    if (text) { used = st->storage; written = st->storage_w; }
    else if (!g_dbg_nocompute && stage_glsl_compute(p)) { glsl = 1; used = st->glsl_ssbos; written = st->glsl_ssbo_w; }
    else if (st->state == 1 || st->state == 2) { used = st->storage; written = st->storage_w; }
    if (log) {
        printf("dispatch %lld: compute program #%d (%s) groups %u x %u x %u, storage used 0x%x written 0x%x\n", (long long)g_frame_dispatch - 1, p->serial,
               st->state == 1 || st->state == 2 ? "assembly text" : st->code ? "machine code" : "nothing usable", gx, gy, gz, used, written);
        for (int n = 0; n < 16; n++)
            if (S.ubo[5][n].addr) {
                const uint32_t *w = P(S.ubo[5][n].addr);
                printf("    uniform %d %llx+%llu:", n, (unsigned long long)S.ubo[5][n].addr, (unsigned long long)S.ubo[5][n].size);
                for (unsigned k = 0; k < 20 && k * 4 + 4 <= S.ubo[5][n].size; k++) printf(w[k] < 0x100000 ? " %u" : " %08x", w[k]);
                printf("\n");
            }
        for (int n = 0; n < 32; n++) if (S.ssbo[5][n].addr) { char tag[40]; snprintf(tag, sizeof tag, "storage %d%s before", n, written >> n & 1 ? " (written)" : ""); mem_stats(tag, S.ssbo[5][n].addr, S.ssbo[5][n].size); }
        for (int n = 0; n < 8; n++) if (S.nvn_tex[5][n]) printf("    texture %d: %llx\n", n, (unsigned long long)S.nvn_tex[5][n]);
        for (int n = 0; n < 8; n++) if (S.nvn_img[5][n]) printf("    image %d: %llx\n", n, (unsigned long long)S.nvn_img[5][n]);
    }
    for (int n = 0; n < 32; n++)
        if ((written >> n & 1) && S.ssbo[5][n].addr && g_ncrange < 1024) { g_crange[g_ncrange].addr = S.ssbo[5][n].addr; g_crange[g_ncrange].size = S.ssbo[5][n].size; g_crange[g_ncrange].prog = p->serial; g_crange[g_ncrange++].n = n; }
    if (g_dbg_nocompute) return;
    if (!text && !glsl) { logf_limited(&nlog, 20, "nvn_gl: DispatchCompute skipped: compute program #%d cannot be used (state %d, machine code %d bytes)\n", p->serial, st->state, st->code_len); g_st.dispatch_skipped++; return; }
    if (!gx || !gy || !gz) return;
    /* storage buffers: read-only ones are streamed like any other buffer (unless an earlier dispatch wrote them and they are still
     * held in GL); written ones get a GL buffer of their own (see compute_flush) */
    CBuf *held[32];
    memset(held, 0, sizeof held);
    for (int n = 0; n < 32; n++) {
        Range r = S.ssbo[5][n];
        if ((used >> n & 1) && r.addr && r.size && cbuf_conflict(r.addr, r.size)) { compute_flush(); break; }
    }
    for (int n = 0; n < 16; n++) if (S.ubo[5][n].addr && cbuf_conflict(S.ubo[5][n].addr, S.ubo[5][n].size)) { compute_flush(); break; }
    for (int n = 0; n < 16; n++) if (S.ubo[5][n].addr && cbuf_find(S.ubo[5][n].addr, S.ubo[5][n].size)) { compute_flush(); break; }
    for (int n = 0; n < 32; n++) {
        Range r = S.ssbo[5][n];
        GLuint binding = text ? (GLuint)n : 40 + (GLuint)n;
        if (!(used >> n & 1) || !r.addr || !r.size) continue;
        held[n] = written >> n & 1 ? cbuf_get(r.addr, r.size) : cbuf_find(r.addr, r.size);
        if (held[n]) glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, binding, held[n]->buf, 0, (ptrdiff_t)r.size);
        else {
            size_t off = arena_put(r.addr, r.size);
            glBindBufferRange(GL_SHADER_STORAGE_BUFFER_, binding, g_arena, (ptrdiff_t)off, (ptrdiff_t)r.size);
        }
    }
    if (text) {
        pair_off();
        glBindProgramPipeline(0);
        glEnable(GL_COMPUTE_PROGRAM_NV_);
        glBindProgramARB(GL_COMPUTE_PROGRAM_NV_, st->gl);
        for (int i = 0; i < st->nbuf; i++) {
            int n = st->map[i];
            /* a parameter buffer binding of an assembly program must start at offset 0 of its buffer object (the driver answers
             * INVALID_VALUE "<offset> must be zero" otherwise), so each one gets a buffer of its own instead of a range of the stream */
            static GLuint param[16];
            if (!param[i]) glCreateBuffers(1, &param[i]);
            if (n == TEXTURE_HANDLE_BUFFER) glNamedBufferData(param[i], 256, (const uint8_t *)S.gl_tex[5] + 32, GL_STREAM_DRAW_);
            else if (n < 16 && S.ubo[5][n].addr && S.ubo[5][n].size) glNamedBufferData(param[i], (ptrdiff_t)S.ubo[5][n].size, P(S.ubo[5][n].addr), GL_STREAM_DRAW_);
            else continue;
            glBindBufferRange(k_pbuf_target[5], (GLuint)i, param[i], 0, n == TEXTURE_HANDLE_BUFFER ? 256 : (ptrdiff_t)S.ubo[5][n].size);
        }
    } else {
        glDisable(GL_COMPUTE_PROGRAM_NV_);
        compute_bind_glsl(p);
    }
    nvn_front_ccmp_dispatch(S.ubo[5], S.ssbo[5], written, st->code, st->code_len, gx, gy, gz);      /* SSBU_COMPUTE_CMP */
    glDispatchCompute(gx, gy, gz);
    if (glsl && st->glsl_images_w) glMemoryBarrier(0xffffffffu);
    if (written) {
        glMemoryBarrier(0x200 /* BUFFER_UPDATE_BARRIER_BIT */);
        if (glsl) {                              /* a translated program reports which storage buffers it really wrote */
            uint32_t wm = 0;
            glGetNamedBufferSubData(g_wmask_buf, 0, 4, &wm);
            written &= wm;
            if (log) printf("    storage buffers written by this dispatch: 0x%x\n", wm);
        }
        for (int n = 0; n < 32; n++) if ((written >> n & 1) && held[n]) held[n]->dirty = 1;
        if (log) {
            compute_flush();
            for (int n = 0; n < 32; n++) if ((written >> n & 1) && held[n]) { char tag[40]; snprintf(tag, sizeof tag, "storage %d after ", n); mem_stats(tag, S.ssbo[5][n].addr, S.ssbo[5][n].size); }
        }
    }
    if (text) glDisable(GL_COMPUTE_PROGRAM_NV_);
    g_chist[h].run++;
    g_st.dispatch_run++;
}



typedef struct { uint64_t cmd_size, compute_size, control_size, flush_threshold; int flags, priority; volatile LONG64 cmd_used, control_used, compute_used; } QueueObj;
static uint64_t now_ns(void);
static void fence_now(SyncObj *o, int flags);
static void event_signal_at(uint64_t a, int mode, uint32_t value);
/* ---- counters (nvnCommandBufferResetCounter / ReportCounter): OpenGL queries that run from one report to the next */
static GLenum counter_target(int type) {
    static const GLenum m[16] = {0, 0x8914 /* SAMPLES_PASSED */, 0x82EE /* VERTICES_SUBMITTED */, 0x82EF /* PRIMITIVES_SUBMITTED */, 0x82F0 /* VERTEX_SHADER_INVOCATIONS */,
                                 0x82F1 /* TESS_CONTROL_SHADER_PATCHES */, 0x82F2 /* TESS_EVALUATION_SHADER_INVOCATIONS */, 0x887F /* GEOMETRY_SHADER_INVOCATIONS */,
                                 0x82F4 /* FRAGMENT_SHADER_INVOCATIONS */, 0, 0x82F3 /* GEOMETRY_SHADER_PRIMITIVES_EMITTED */, 0x82F6 /* CLIPPING_INPUT_PRIMITIVES */,
                                 0x82F7 /* CLIPPING_OUTPUT_PRIMITIVES */, 0x8C87 /* PRIMITIVES_GENERATED */, 0x8C88 /* TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN */, 0};
    return type >= 0 && type < 16 ? m[type] : 0;
}
static void counter_stop(int type) {
    uint64_t r = 0;
    if (type < 0 || type >= 17 || !g_counter[type].active) return;
    glEndQuery(counter_target(type));
    if (gl_has(glGetQueryObjectui64v, "glGetQueryObjectui64v")) glGetQueryObjectui64v(g_counter[type].q, 0x8866 /* QUERY_RESULT */, &r);
    g_counter[type].acc += r; g_counter[type].active = 0;
}
static void counter_start(int type) {
    if (!counter_target(type)) return;
    if (!g_counter[type].q) glGenQueries(1, &g_counter[type].q);
    glBeginQuery(counter_target(type), g_counter[type].q);
    g_counter[type].active = 1;
}
/* What a multisample state asks for beyond the two enables; some of it is state of the bound framebuffer. */
static void apply_multisample(void) {
    const uint8_t *b = S.ms;
    static int warned;
    unsigned fl = b[3];
    if (b[5]) { if (gl_has(glRasterSamplesEXT, "glRasterSamplesEXT")) { glEnable(0x9327 /* RASTER_MULTISAMPLE_EXT */); glRasterSamplesEXT(b[5], GL_TRUE); } } else if (S.ms_ext & 1) glDisable(0x9327);
    if (((fl >> 3) & 3) || (S.ms_ext & 2)) { static const GLenum m[4] = {GL_NONE, GL_RGB, GL_ALPHA, GL_RGBA}; if (gl_has(glCoverageModulationNV, "glCoverageModulationNV")) glCoverageModulationNV(m[(fl >> 3) & 3]); }
    if (fl & 1) { if (gl_has(glFragmentCoverageColorNV, "glFragmentCoverageColorNV")) { glEnable(0x92DD /* FRAGMENT_COVERAGE_TO_COLOR_NV */); glFragmentCoverageColorNV(fl >> 5); } } else if (S.ms_ext & 4) glDisable(0x92DD);
    if ((fl & 2) && S.have_fbo) {
        GLfloat loc[32];
        for (int i = 0; i < 16; i++) { loc[2 * i] = (float)(b[8 + i] & 15) / 16.0f; loc[2 * i + 1] = (float)(b[8 + i] >> 4) / 16.0f; }
        if (gl_has(glFramebufferSampleLocationsfvNV, "glFramebufferSampleLocationsfvNV") && gl_has(glFramebufferParameteri, "glFramebufferParameteri")) {
            glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9342 /* FRAMEBUFFER_PROGRAMMABLE_SAMPLE_LOCATIONS_NV */, 1);
            glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9343 /* FRAMEBUFFER_SAMPLE_LOCATION_PIXEL_GRID_NV */, (fl >> 2) & 1);
            glFramebufferSampleLocationsfvNV(GL_FRAMEBUFFER_, 0, b[4] ? b[4] : 1, loc);
        }
    } else if ((S.ms_ext & 8) && S.have_fbo && glFramebufferParameteri) glFramebufferParameteri(GL_FRAMEBUFFER_, 0x9342, 0);
    S.ms_ext = (b[5] ? 1u : 0) | (((fl >> 3) & 3) ? 2u : 0) | ((fl & 1) ? 4u : 0) | ((fl & 2) ? 8u : 0);
    (void)warned;
}

static void copy_region(const uint8_t *p, int *r) { memcpy(r, p, 24); }
static GLuint temp_fbo(GLenum target, GLuint slot_fbo, Tex *t, const View *v) {
    Fmt f;
    int kind = fmt_get(t->b.format, &f) ? f.kind : 0;
    glBindFramebuffer(target, slot_fbo);
    glFramebufferTexture(target, GL_COLOR_ATTACHMENT0_, 0, 0);
    glFramebufferTexture(target, GL_DEPTH_STENCIL_ATTACHMENT_, 0, 0);
    glFramebufferTexture(target, kind == 3 ? GL_DEPTH_STENCIL_ATTACHMENT_ : kind == 1 ? GL_DEPTH_ATTACHMENT_ : kind == 2 ? GL_STENCIL_ATTACHMENT_ : GL_COLOR_ATTACHMENT0_, tex_view_obj(t, v)->gl, 0);
    return (GLuint)(kind == 3 ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : kind == 1 ? GL_DEPTH_BUFFER_BIT : kind == 2 ? GL_STENCIL_BUFFER_BIT : GL_COLOR_BUFFER_BIT);
}
static GLuint g_fbo_tmp[2];
static void blit_tex(Tex *src, const View *sv, const int *sr, Tex *dst, const View *dv, const int *dr, int linear) {
    tex_sync(src); tex_sync(dst);
    dst->gpu_written = 1;
    GLuint mask = temp_fbo(GL_READ_FRAMEBUFFER_, g_fbo_tmp[0], src, sv);
    temp_fbo(GL_DRAW_FRAMEBUFFER_, g_fbo_tmp[1], dst, dv);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB_);
    glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[3], sr[1] + sr[4], dr[0], dr[1], dr[0] + dr[3], dr[1] + dr[4], mask, linear && mask == GL_COLOR_BUFFER_BIT ? GL_LINEAR : GL_NEAREST);
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
    g_st.copies++;
}

static void replay(uint64_t handle, int depth);
static void save_bmp(const char *path, Tex *t);
/* Round 17, TEST ONLY (nvn_dt.h): SSBU_DRAWTAG=1 with SSBU_SHOT_FIGHT: the submissions the game queues between the pad poll
 * that reached a wanted fight frame and the present that is saved are "tagged": g_draw_tag = that fight frame while the
 * render thread replays them (set by a job queued before the submission, cleared by the back end at the present). */
#include "nvn_dt.h"
static int g_draw_tag, g_dt_idx;                 /* render thread */
volatile LONG g_drawtag_next;                    /* game thread: the fight frame the next submission belongs to, 0 = not tagged */
static void *dt_host(uint64_t a) { return P(a); }
static int drawtag_env(void) { static int on = -1; if (on < 0) on = getenv("SSBU_DRAWTAG") != NULL; return on; }
static void job_drawtag(void *arg) { int n = *(int *)arg; if (g_draw_tag != n) { g_draw_tag = n; g_dt_idx = 0; printf("DT begin: the commands of fight frame %d (%s)\n", n, g_vk ? "Vulkan" : "OpenGL"); } }
int nvn_front_draw_tag(void) { return g_draw_tag; }
int nvn_front_draw_tag_next(void) { return g_dt_idx++; }
void nvn_front_draw_tag_clear(void) { if (g_draw_tag) { printf("DT end: fight frame %d, %d draw commands\n", g_draw_tag, g_dt_idx); g_draw_tag = 0; } }
/* Debug aid SSBU_DRAWSHOT=<frame>: the colour target after every draw of that frame, as draw_<n>.bmp. */
static void draw_shot(void) {
    static int want = -2;
    Tex fake;
    char path[64];
    if (want == -2) want = getenv("SSBU_DRAWSHOT") ? atoi(getenv("SSBU_DRAWSHOT")) : -1;
    if ((g_st.frames != want && !(g_draw_tag && getenv("SSBU_DRAWSHOT_BOX"))) || !S.fbo_w) return;      /* round 17: with SSBU_DRAWTAG the box aid follows the tagged fight frame */
    {   /* SSBU_DRAWSHOT_BOX=x,y,w,h (in 1920x1080 picture coordinates): no files; one line per draw that CHANGED the mean
         * colour of that box of its colour target (finds the draws that paint a given place of the screen) */
        static int box[4] = {-1, 0, 0, 0};
        static unsigned last_fbo; static double last[4];
        if (box[0] == -1) { const char *e = getenv("SSBU_DRAWSHOT_BOX"); box[0] = -2; if (e && sscanf(e, "%d,%d,%d,%d", &box[0], &box[1], &box[2], &box[3]) != 4) box[0] = -2; }
        if (box[0] >= 0) {
            int x = box[0] * S.fbo_w / 1920, y = box[1] * S.fbo_h / 1080, w = box[2] * S.fbo_w / 1920, h = box[3] * S.fbo_h / 1080;
            if (w < 1 || h < 1 || x + w > S.fbo_w || y + h > S.fbo_h) return;
            uint8_t *px = malloc((size_t)w * h * 4);
            double m[4] = {0, 0, 0, 0};
            glBindFramebuffer(GL_READ_FRAMEBUFFER_, S.fbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(x, y, w, h, 0x1908 /* GL_RGBA */, GL_UNSIGNED_BYTE, px);
            for (int i = 0; i < w * h; i++) for (int k = 0; k < 4; k++) m[k] += px[i * 4 + k];
            for (int k = 0; k < 4; k++) m[k] /= (double)(w * h);
            free(px);
            if (last_fbo != S.fbo || m[0] != last[0] || m[1] != last[1] || m[2] != last[2] || m[3] != last[3])
                printf("  BOX: draw %d (DT %d) fbo %u (%dx%d): mean RGBA of the box %.2f %.2f %.2f %.2f%s\n", g_frame_draw_index - 1, g_dt_idx - 1, S.fbo, S.fbo_w, S.fbo_h, m[0], m[1], m[2], m[3], last_fbo != S.fbo ? "  (first draw seen on this target)" : "  CHANGED");
            last_fbo = S.fbo; memcpy(last, m, sizeof last);
            return;
        }
    }
    memset(&fake, 0, sizeof fake);
    fake.b.width = S.fbo_w; fake.b.height = S.fbo_h;
    snprintf(path, sizeof path, "draw_%02d_fbo%u.bmp", g_frame_draw_index - 1, S.fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, S.fbo);
    save_bmp(path, &fake);
}
/* P3 round 2, EXPERIMENT, off by default (SSBU_FIRSTDRAW_WARM=1 turns it on): the first draw with a pair of GLSL
 * programs is preceded by one point drawn with the rasterizer off. Result: it does NOT remove the cost, it only moves it.
 * The driver spends 50-90 ms inside the first draw call of 48-93 of the 420-520 program pairs of a run (3.4-6.2 s per
 * run); with the point, that time is spent in the point instead (tests/logs/p3b_r2c.log: 48 points over 4 ms, 3,979 ms).
 * One run (p3b_warm.log) had only 5 slow pairs, with or without the point: the cost belongs to pairs the DRIVER has
 * never linked on this PC (its own disk cache), not to the first draw of a run. Kept as a measuring aid: the end
 * report counts the slow first draws. What the point does: the vertex stage runs once on vertex 0 of the bound
 * buffers; nothing reaches the framebuffer. It is not done while a counter other than "samples passed" is running or
 * when the vertex stage writes storage buffers. */
static LONG64 g_fdw_n, g_fdw_slow, g_fdw_skipped, g_fdw_ticks;
static void first_draw_warm(void) {
    static uint64_t seen[1 << 16];
    static int off = -1;
    if (off < 0) off = getenv("SSBU_FIRSTDRAW_WARM") == NULL;
    if (off || !g_gs_last_prog[0]) return;
    uint64_t pair = ((uint64_t)g_gs_last_prog[0] << 32) | g_gs_last_prog[1] | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 48);
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0xffff) { if (seen[h] == pair) return; if (!seen[h]) { seen[h] = pair; break; } }
    if (!S.prog[0] || S.prog[0]->st[0].glsl != g_gs_last_prog[0] || S.prog[0]->st[0].glsl_ssbo_w) { g_fdw_skipped++; return; }      /* not the GLSL path, or a vertex stage with side effects */
    for (int t = 0; t < 17; t++) if (t != 1 && g_counter[t].active) { g_fdw_skipped++; return; }
    LONG64 t0 = pf_now(), freq;
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    HS_IN("first draw of a program pair: discarded point (driver)", 0);
    glEnable(GL_RASTERIZER_DISCARD_);
    glDrawArrays(GL_POINTS, 0, 1);
    glDisable(GL_RASTERIZER_DISCARD_);
    HS_OUT();
    LONG64 dt = pf_now() - t0;
    g_fdw_n++; g_fdw_ticks += dt; g_fdw_slow += dt * 250 >= freq;
}
/* SSBU_HITCH: a draw call that took more than 4 ms, and whether it was the first draw with this pair of programs. */
static void hitch_draw(LONG64 t0) {
    static uint64_t seen[1 << 16];
    static LONG64 freq, n_first, n_first_slow, n_old_slow;
    static double ms_first, ms_old;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    double ms = (double)(pf_now() - t0) * 1000.0 / (double)freq;
    uint64_t pair = ((uint64_t)g_gs_last_prog[0] << 32) | g_gs_last_prog[1] | 1ull << 63;
    unsigned h = (unsigned)((pair * 0x9E3779B97F4A7C15ULL) >> 48);
    int first = 1;
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & 0xffff) { if (seen[h] == pair) { first = 0; break; } if (!seen[h]) { seen[h] = pair; break; } }
    n_first += first;
    if (ms < 4.0) return;
    if (first) { n_first_slow++; ms_first += ms; } else { n_old_slow++; ms_old += ms; }
    if (g_fdlog) {
        int any = 0;
        uint32_t mask = 0;
        ShSrc *a = g_gs_last_prog[0] < 65536 ? g_prog_src[g_gs_last_prog[0]] : NULL, *b = g_gs_last_prog[1] < 65536 ? g_prog_src[g_gs_last_prog[1]] : NULL;
        for (int i = 0; i < 16; i++) mask |= (uint32_t)(S.attr_enabled[i] != 0) << i;
        printf("fd-slow: %016llx %x %016llx %x mask %x (enabled %x) %.1f ms frame %lld %s%c", (unsigned long long)(a ? a->h : 0), a ? (unsigned)a->len : 0, (unsigned long long)(b ? b->h : 0), b ? (unsigned)b->len : 0,
               S.prog[0] ? mask & S.prog[0]->st[0].glsl_dep : 0, mask, ms, (long long)g_st.frames + 1, first ? "first" : "later", 10);
        {   int nc = 0; for (int i = 0; i < 8; i++) nc += S.color[i] != NULL;
            printf("fd-state: prim %u count %u poly %x depth clamp %u discard %u sample shading %d alpha %d func %d ms %02x %02x %02x %02x %02x %02x colour targets %d (format %x) %dx%d ds %llx dep %x key ints %x texint %x %x nattr %d%c",
                   g_fd_prim, g_fd_count, g_fd_poly, g_fd_dclamp, g_fd_rdisc, S.sample_shading, S.prog[1] ? S.prog[1]->st[1].glsl_alpha : -1, S.alpha_func, S.ms[0], S.ms[1], S.ms[2], S.ms[3], S.ms[4], S.ms[5], nc,
                   S.color0 ? S.color0->b.format : 0, S.fbo_w, S.fbo_h, (unsigned long long)S.ds, S.prog[0] ? S.prog[0]->st[0].glsl_dep : 0, S.prog[0] ? S.prog[0]->st[0].glsl_key[0] : 0, S.tex_int[0], S.tex_int[1], S.nattr, 10); }
        for (unsigned i = 0; i < 64; i++) if (g_fd_ring[i].t >= t0 && g_fd_ring[i].text[0]) { any++; printf("fdlog: frame %lld draw %.1f ms: id %u type %x severity %x: %s%c", (long long)g_st.frames + 1, ms, g_fd_ring[i].id, g_fd_ring[i].type, g_fd_ring[i].sev, g_fd_ring[i].text, 10); }
        if (!any) printf("fdlog: frame %lld draw %.1f ms: NO driver message during this draw call%c", (long long)g_st.frames + 1, ms, 10);
    }
    printf("hitch-draw: frame %lld %.1f ms, programs vs %u fs %u, %s | so far: %lld pairs, slow first draws %lld (%.0f ms), slow later draws %lld (%.0f ms)%c", (long long)g_st.frames + 1, ms, g_gs_last_prog[0],
           g_gs_last_prog[1], first ? "FIRST draw with this pair" : "pair drawn before", (long long)n_first, (long long)n_first_slow, ms_first, (long long)n_old_slow, ms_old, 10);
}
static void exec(const Cmd *m, int depth) {
    const uint8_t *blob = BLOB(m);
    const uint64_t *a = m->a;
    g_cur_op = k_op_name[m->op < OP_COUNT ? m->op : 0];
    if (g_cbuf_live && (m->op == OP_COPY_BUF_TEX || m->op == OP_COPY_BUF_BUF || m->op == OP_UPDATE_UBO || m->op == OP_CLEAR_BUF || m->op == OP_COPY_TEX_BUF)) compute_flush();
    switch (m->op) {
    case OP_TEXPOOL: S.tpool = P(a[0]); break;
    case OP_SMPPOOL: S.spool = P(a[0]); break;
    case OP_PROGRAM:
        for (int s = 0; s < 6; s++) if (a[1] & (1u << s)) { Prog *p = P(a[0]); S.prog[s] = p && !p->dead && p->st[s].state ? p : NULL; }
        break;
    case OP_UBO: case OP_SSBO: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2], max = m->op == OP_UBO ? 16 : 32;
        Range *dst = m->op == OP_UBO ? S.ubo[stage] : S.ssbo[stage];
        for (int i = 0; i < count && first + i < max; i++) memcpy(&dst[first + i], blob + i * 16, 16);
        break;
    }
    case OP_TEXTURES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 32; i++) {
            uint64_t h;
            memcpy(&h, blob + i * 8, 8);
            S.nvn_tex[stage][first + i] = h;
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            unsigned u = (unsigned)(first + i);
            { LONG64 pf0 = pf_now(); HS_IN("texture handle (view, sampler, bindless handle)", 0); S.gl_tex[stage][4 + u] = resolve_handle(S.tpool, S.spool, h, dims, &isint); HS_OUT(); pf_add(PF_HANDLE, pf0); }
            memcpy(&S.gl_tex[stage][48 + 2 * u], dims, 16);
            S.tex_int[stage] = (S.tex_int[stage] & ~(1u << u)) | ((uint32_t)(isint != 0) << u);
            S.tex_sint[stage] = (S.tex_sint[stage] & ~(1u << u)) | ((uint32_t)(isint == 2) << u);
        }
        S.tex_dirty[stage] = 1;
        break;
    }
    case OP_VBUF:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 32; i++) memcpy(&S.vbuf[a[0] + i], blob + i * 16, 16);
        break;
    case OP_VATTR:
        S.nattr = (int)a[0] < 32 ? (int)a[0] : 32;
        memcpy(S.vattr, blob, (size_t)S.nattr * 4);
        break;
    case OP_VSTREAM:
        for (int i = 0; i < (int)a[0] && i < 32; i++) memcpy(&S.vstream[i], blob + i * 8, 8);
        break;
    case OP_BLEND: {
        const uint8_t *b = (const uint8_t *)&a[0];
        glBlendFuncSeparatei(b[0] & 7, blend_func(b[1]), blend_func(b[2]), blend_func(b[3]), blend_func(b[4]));
        glBlendEquationSeparatei(b[0] & 7, blend_eq(b[5]), blend_eq(b[6]));
        if (b[7]) {                            /* advanced blend equation (NV_blend_equation_advanced) instead of the functions above */
            static const uint16_t adv[0x33] = {0, 0, 0, 0, 0, 0 /* ZERO */, 0x9286, 0x9287, 0x9288, 0x9289, 0x928A, 0x928B, 0x928C, 0x928D, 0x928E, 0x928F, 0x1506, 0x9291, 0x92B1, 0x92B2, 0x9292,
                                               0x9294, 0x9295, 0x9296, 0x9297, 0x9298, 0x9299, 0x929A, 0x929B, 0x929C, 0x929E, 0x929F, 0x92B3, 0x92A0, 0x92A1, 0x150A, 0x92A3, 0x92B4, 0x92A4,
                                               0x92A5, 0x92A6, 0x92A7, 0x92A8, 0x92A9, 0x1903, 0x1904, 0x1905, 0x92AD, 0x92AE, 0x92AF, 0x92B0};
            if (b[7] < 0x33 && gl_has(glBlendEquationi, "glBlendEquationi") && gl_has(glBlendParameteriNV, "glBlendParameteriNV")) {
                glBlendParameteriNV(0x9280 /* BLEND_PREMULTIPLIED_SRC_NV */, (b[0] >> 5) & 1);
                glBlendParameteriNV(0x9281 /* BLEND_OVERLAP_NV */, 0x9282 + ((b[0] >> 3) & 3));
                glBlendEquationi(b[0] & 7, adv[b[7]]);
            }
        }
        break;
    }
    case OP_COLOR: {
        const uint8_t *b = (const uint8_t *)&a[0];
        for (unsigned i = 0; i < 8; i++) { if (b[0] & (1u << i)) glEnablei(GL_BLEND, i); else glDisablei(GL_BLEND, i); }
        if (b[1] != 3) { glEnable(GL_COLOR_LOGIC_OP); glLogicOp(GL_CLEAR + (b[1] & 15)); } else glDisable(GL_COLOR_LOGIC_OP);
        S.alpha_func = b[2];
        break;
    }
    case OP_CHANMASK: S.chanmask = (uint32_t)a[0]; apply_chanmask(); break;
    case OP_MULTISAMPLE: {
        const uint8_t *b = (const uint8_t *)&a[0];
        if (b[0]) glEnable(GL_MULTISAMPLE_); else glDisable(GL_MULTISAMPLE_);
        if (b[1]) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE_); else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE_);
        memcpy(S.ms, b, 24);
        if (b[3] || b[5] || S.ms_ext) apply_multisample();
        break;
    }
    case OP_POLYGON: {
        const uint8_t *b = (const uint8_t *)&a[0];
        static const GLenum face[4] = {0, GL_FRONT, GL_BACK, GL_FRONT_AND_BACK};
        if (b[0] & 3) { glEnable(GL_CULL_FACE); glCullFace(face[b[0] & 3]); } else glDisable(GL_CULL_FACE);
        glFrontFace((b[1] != 0) != (g_dbg_flipface != 0) ? GL_CCW : GL_CW);       /* see the note on winding at nvn_prim */
        glPolygonMode(GL_FRONT_AND_BACK, GL_POINT + (b[2] < 3 ? b[2] : 2));
        g_fd_poly = b[0] | (unsigned)b[2] << 8;
        if (b[3] & 1) glEnable(GL_POLYGON_OFFSET_POINT); else glDisable(GL_POLYGON_OFFSET_POINT);
        if (b[3] & 2) glEnable(GL_POLYGON_OFFSET_LINE); else glDisable(GL_POLYGON_OFFSET_LINE);
        if (b[3] & 4) glEnable(GL_POLYGON_OFFSET_FILL); else glDisable(GL_POLYGON_OFFSET_FILL);
        break;
    }
    case OP_DEPTHSTENCIL: {
        uint64_t d = a[0];
        S.ds = d;
        if (d & 1) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
        glDepthMask((d >> 1) & 1);
        glDepthFunc(nvn_func((unsigned)(d >> 4) & 15));
        if (d & 4) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        glStencilOpSeparate(GL_FRONT, stencil_op((unsigned)(d >> 16) & 15), stencil_op((unsigned)(d >> 20) & 15), stencil_op((unsigned)(d >> 24) & 15));
        glStencilOpSeparate(GL_BACK, stencil_op((unsigned)(d >> 28) & 15), stencil_op((unsigned)(d >> 32) & 15), stencil_op((unsigned)(d >> 36) & 15));
        S.stencil_dirty = 1;
        break;
    }
    case OP_VIEWPORTS:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { float v[4]; memcpy(v, blob + i * 16, 16); glViewportIndexedf((GLuint)a[0] + (GLuint)i, v[0], v[1], v[2], v[3]); }
        break;
    case OP_SCISSORS:
        if (g_dbg_noscissor) break;
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) {
            int v[4];
            memcpy(v, blob + i * 16, 16);
            glScissorIndexed((GLuint)a[0] + (GLuint)i, v[0], v[1], v[2] < 0 ? 0 : v[2], v[3] < 0 ? 0 : v[3]);
            if ((int)a[0] + i == 0) { S.sc0[0] = v[0]; S.sc0[1] = v[1]; S.sc0[2] = v[2] < 0 ? 0 : v[2]; S.sc0[3] = v[3] < 0 ? 0 : v[3]; }
        }
        break;
    case OP_DEPTHRANGES:
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { float v[2]; memcpy(v, blob + i * 8, 8); glDepthRangeIndexed((GLuint)a[0] + (GLuint)i, v[0], v[1]); }
        break;
    case OP_TARGETS: set_targets((int)a[0], (const TargetRef *)blob, (const TargetRef *)blob + 8); if (S.ms_ext & 8) apply_multisample(); break;
    case OP_STENCIL_REF: case OP_STENCIL_VMASK: case OP_STENCIL_MASK: {
        int *dst = m->op == OP_STENCIL_REF ? S.sref : m->op == OP_STENCIL_VMASK ? S.svmask : S.swmask;
        if (a[0] & 1) dst[0] = (int)a[1];
        if (a[0] & 2) dst[1] = (int)a[1];
        S.stencil_dirty = 1;
        break;
    }
    case OP_BLENDCOLOR: { float v[4]; memcpy(v, blob, 16); glBlendColor(v[0], v[1], v[2], v[3]); break; }
    case OP_ALPHAREF: memcpy(&S.alpha_ref, &a[0], 4); break;                       /* applied per draw (draw_prepare) */
    case OP_POINTSIZE: { float v; memcpy(&v, &a[0], 4); glPointSize(v); break; }
    case OP_LINEWIDTH: { float v; memcpy(&v, &a[0], 4); glLineWidth(v); break; }
    case OP_POLYOFFSET: { float v[3]; memcpy(v, &a[0], 12); glPolygonOffsetClamp(v[0], v[1], v[2]); break; }      /* (factor, units, clamp) */
    case OP_SAMPLEMASK: if ((uint32_t)a[0] == 0xffffffffu) glDisable(0x8E51); else { glEnable(0x8E51); glSampleMaski(0, (GLbitfield)a[0]); } break;
    case OP_RASTDISCARD: g_fd_rdisc = a[0] & 1; if (a[0] & 1) glEnable(GL_RASTERIZER_DISCARD_); else glDisable(GL_RASTERIZER_DISCARD_); break;
    case OP_DEPTHCLAMP: g_fd_dclamp = a[0] & 1; if (a[0] & 1) glEnable(GL_DEPTH_CLAMP_); else glDisable(GL_DEPTH_CLAMP_); break;
    case OP_PRIMRESTART: if (a[0] & 1) { glEnable(GL_PRIMITIVE_RESTART_); glPrimitiveRestartIndex((GLuint)a[1]); } else glDisable(GL_PRIMITIVE_RESTART_); break;
    case OP_PATCHSIZE: glPatchParameteri(GL_PATCH_VERTICES_, (GLint)a[0]); break;
    case OP_CLEARCOLOR: {
        float v[4];
        unsigned i = (unsigned)a[0] & 7, mask = (unsigned)a[2];
        if (!S.have_fbo || S.render_off) break;
        memcpy(v, blob, 16);
        glColorMaski(i, mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
        if (a[5] == 1) glClearBufferiv(GL_COLOR, (GLint)i, (const GLint *)v);              /* ClearColori / ClearColorui say what the four words are */
        else if (a[5] == 2) glClearBufferuiv(GL_COLOR, (GLint)i, (const GLuint *)v);
        else
        {
            Fmt f;
            int isint = S.color[i] && fmt_get(S.color[i]->b.format, &f) && f.kind == 0 && (f.format == 0x8D94 || f.format == 0x8228 || f.format == 0x8D98 || f.format == 0x8D99);
            if (!isint) glClearBufferfv(GL_COLOR, (GLint)i, v);      /* NVN passes the same four words for ClearColori / ClearColorui */
            else if (f.type == 0x1400 || f.type == 0x1402 || f.type == 0x1404) glClearBufferiv(GL_COLOR, (GLint)i, (const GLint *)v);
            else glClearBufferuiv(GL_COLOR, (GLint)i, (const GLuint *)v);
        }
        apply_chanmask();
        g_st.clears++; g_frame_clears++;
        break;
    }
    case OP_CLEARDS: {
        float depth;
        GLbitfield bits = (a[0] & 1 ? GL_DEPTH_BUFFER_BIT : 0) | ((uint32_t)a[2] ? GL_STENCIL_BUFFER_BIT : 0);
        if (!S.have_fbo || !bits || S.render_off) break;
        memcpy(&depth, &a[3], 4);
        glDepthMask(GL_TRUE);
        glStencilMaskSeparate(GL_FRONT_AND_BACK, (GLuint)a[2]);
        glClearDepth(depth);
        glClearStencil((GLint)a[1]);
        glClear(bits);
        glDepthMask((S.ds >> 1) & 1);
        S.stencil_dirty = 1;
        g_st.clears++; g_frame_clears++;
        break;
    }
    case OP_DRAW_ARRAYS: {
        LONG64 pf0 = pf_now();
        int ok;
        { HS_IN("draw state (pipeline, bindings)", 0); ok = draw_prepare(); HS_OUT(); }
        pf_add(PF_PREP, pf0);
        if (g_draw_tag) dt_line("gl", g_draw_tag, g_dt_idx++, ok, (unsigned)a[0], -1, (int)a[2], 0, (long long)a[1], (unsigned)a[3], (unsigned)a[4], S.prog, S.nattr, S.vattr, S.vbuf, (const uint32_t *)S.vstream, S.ubo, S.ssbo, dt_host);
        if (!ok) { g_st.draws_skipped++; g_frame_skipped++; break; }
        if (drawlog_now()) printf("  DrawArrays mode %d first %d count %d base instance %d instances %d%c", (int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], 10);
        drawlog_begin();
        first_draw_warm();
        pf0 = pf_now();
        g_fd_prim = (unsigned)a[0]; g_fd_count = (unsigned)a[2];
        { HS_IN("draw call (driver)", 0); glDrawArraysInstancedBaseInstance(nvn_prim((unsigned)a[0]), (GLint)a[1], (GLsizei)a[2], (GLsizei)a[4], (GLuint)a[3]); HS_OUT(); }
        pf_add(PF_DRAWCALL, pf0);
        if (g_hitch_ms) hitch_draw(pf0);
        drawlog_end();
        draw_shot();
        g_st.draws++; g_frame_draws++; count_dummy_samples();
        break;
    }
    case OP_DRAW_ELEMENTS: {
        static const GLenum itype[3] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT};
        unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1;
        int count = (int)a[2];
        LONG64 pf0 = pf_now();
        int ok;
        { HS_IN("draw state (pipeline, bindings)", 0); ok = count > 0 && a[3] && draw_prepare(); HS_OUT(); }
        if (g_draw_tag) dt_line("gl", g_draw_tag, g_dt_idx++, ok, (unsigned)a[0], (int)t, count, a[3], (long long)(int32_t)a[4], (unsigned)a[5], (unsigned)a[6], S.prog, S.nattr, S.vattr, S.vbuf, (const uint32_t *)S.vstream, S.ubo, S.ssbo, dt_host);
        if (!ok) { pf_add(PF_PREP, pf0); g_st.draws_skipped++; g_frame_skipped++; break; }
        size_t off = arena_put(a[3], (uint64_t)count << t);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, g_arena);
        pf_add(PF_PREP, pf0);
        if (drawlog_now()) printf("  DrawElements mode %d type %d count %d base vertex %d base instance %d instances %d%c", (int)a[0], (int)t, count, (int)a[4], (int)a[5], (int)a[6], 10);
        if (getenv("SSBU_DRAWSHOT_CLEAR") && getenv("SSBU_DRAWSHOT") && g_st.frames == atoi(getenv("SSBU_DRAWSHOT"))) { glDisable(GL_SCISSOR_TEST); glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT); glEnable(GL_SCISSOR_TEST); }
        drawlog_begin();
        first_draw_warm();
        pf0 = pf_now();
        g_fd_prim = (unsigned)a[0]; g_fd_count = (unsigned)count;
        { HS_IN("draw call (driver)", 0); glDrawElementsInstancedBaseVertexBaseInstance(nvn_prim((unsigned)a[0]), count, itype[t], (const void *)off, (GLsizei)a[6], (GLint)a[4], (GLuint)a[5]); HS_OUT(); }
        pf_add(PF_DRAWCALL, pf0);
        if (g_hitch_ms) hitch_draw(pf0);
        drawlog_end();
        draw_shot();
        g_st.draws++; g_frame_draws++; count_dummy_samples();
        break;
    }
    case OP_COPY_BUF_TEX: {                    /* a0 buffer address, a1 texture, a4 flags, a5 / a6 row / image stride of its command buffer; blob: view, region */
        /* PR #14 (xtlg511): replay with the strides recorded in the command (a5/a6), not the running S.row_stride/S.image_stride */
        Tex *t = P(a[1]);
        View v;
        Fmt f;
        int r[6];
        if (!t || t->dead || !a[0]) break;
        memcpy(&v, blob, sizeof v);
        copy_region(blob + 24, r);
        if (!t->o.gl) tex_realize(t);
        if (!fmt_get(t->b.format, &f)) break;
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
        {   /* one line per distinct destination (format, size): shows what is updated this way (movie frames, glyph caches, ...) */
            static struct { int fmt, w, h; } seen[32];
            static int nseen;
            int k;
            for (k = 0; k < nseen && !(seen[k].fmt == t->b.format && seen[k].w == t->b.width && seen[k].h == t->b.height); k++) {}
            if (k == nseen && nseen < 32) {
                size_t nz = 0, bytes = (size_t)r[3] * (size_t)r[4] * f.bpb / (size_t)(f.bw * f.bh);
                for (size_t q = 0; q < bytes; q++) nz += ((const uint8_t *)P(a[0]))[q] != 0;
                seen[nseen].fmt = t->b.format; seen[nseen].w = t->b.width; seen[nseen++].h = t->b.height;
                printf("nvn_gl: CopyBufferToTexture into %dx%dx%d format 0x%x target %d levels %d flags 0x%x swizzle %d%d%d%d: region %d,%d,%d %dx%dx%d level %d layer %d, row stride %lld, %zu of %zu source bytes non-zero\n",
                       t->b.width, t->b.height, t->b.depth, t->b.format, t->b.target, t->b.levels, t->b.flags, t->b.swz[0], t->b.swz[1], t->b.swz[2], t->b.swz[3],
                       r[0], r[1], r[2], r[3], r[4], r[5], level, layer, (long long)a[5], nz, bytes);     /* PR #14 (xtlg511): the command's own row stride */
            }
        }
        /* PR #14 (xtlg511): the strides of the command buffer that recorded this copy (a5 / a6), not the running S state */
        if (a[5] && f.kind != 4) glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)((int64_t)a[5] / f.bpb));      /* nvnCommandBufferSetCopyRowStride */
        if (a[6] && a[5] && f.kind != 4) glPixelStorei(0x806E /* UNPACK_IMAGE_HEIGHT */, (GLint)((int64_t)a[6] / (int64_t)a[5]));   /* SetCopyImageStride */
        if (t->b.target != 2 && t->b.target != 4 && t->b.target != 9 && t->b.target != 8) layer = 0;
        else if (t->b.target != 2) layer += r[2];
        tex_put(t, &f, level, layer, r[0], r[1], r[2], r[3], r[4], r[5] > 0 ? r[5] : 1, P(a[0]));
        glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0); glPixelStorei(0x806E, 0);
        t->gpu_written = 1; t->uploaded = 1;
        g_st.copies++;
        break;
    }
    case OP_COPY_TEX_TEX: {                    /* a0 src, a3 dst, a6 flags; blob: src view, src region, dst view, dst region */
        Tex *s = P(a[0]), *d = P(a[3]);
        View sv, dv;
        int sr[6], dr[6];
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); copy_region(blob + 24, sr);
        memcpy(&dv, blob + 48, sizeof dv); copy_region(blob + 72, dr);
        if (a[6] & 4) { dr[0] += dr[3]; dr[3] = -dr[3]; }      /* NVN_COPY_FLAGS_MIRROR_X / MIRROR_Y */
        if (a[6] & 8) { dr[1] += dr[4]; dr[4] = -dr[4]; }
        { LONG64 pf0 = pf_now(); blit_tex(s, &sv, sr, d, &dv, dr, (int)(a[6] & 1)); pf_add(PF_COPY, pf0); }
        break;
    }
    case OP_DOWNSAMPLE: {
        Tex *s = P(a[0]), *d = P(a[1]);
        if (!s || !d || s->dead || d->dead) break;
        int sr[6] = {0, 0, 0, s->b.width, s->b.height, 1}, dr[6] = {0, 0, 0, d->b.width, d->b.height, 1};
        blit_tex(s, NULL, sr, d, NULL, dr, 0);
        break;
    }
    case OP_COPY_TEX_BUF: {                    /* a0 texture, a3 buffer address, a5 / a6 row / image stride of its command buffer; blob: view, region */
        Tex *t = P(a[0]);
        View v;
        Fmt f;
        int r[6];
        if (!t || t->dead || !a[3] || !t->o.gl || !fmt_get(t->b.format, &f)) break;
        memcpy(&v, blob, sizeof v);
        copy_region(blob + 24, r);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, d = r[5] > 0 ? r[5] : 1;
        int z = t->b.target == 2 ? r[2] : layer + r[2];
        size_t bytes = ((size_t)(r[3] + f.bw - 1) / f.bw) * ((size_t)(r[4] + f.bh - 1) / f.bh) * f.bpb * (size_t)d;
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        compute_flush();
        /* PR #14 (xtlg511): the strides of the command buffer that recorded this copy (a5 / a6), not the running S state */
        if (a[5] && f.kind != 4) { glPixelStorei(0x0D02 /* PACK_ROW_LENGTH */, (GLint)((int64_t)a[5] / f.bpb)); bytes = (size_t)a[5] * (size_t)r[4] * (size_t)d; }
        if (a[6] && a[5] && f.kind != 4) { glPixelStorei(0x806C /* PACK_IMAGE_HEIGHT */, (GLint)((int64_t)a[6] / (int64_t)a[5])); bytes = (size_t)a[6] * (size_t)d; }
        if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, (GLsizei)bytes, P(a[3]));
        else glGetTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, f.format, f.type, (GLsizei)bytes, P(a[3]));
        glPixelStorei(0x0D02, 0); glPixelStorei(0x806C, 0);
        g_arena_gen++;
        g_st.copies++;
        break;
    }
    case OP_COPY_BUF_BUF: if (a[0] && a[1]) memmove(P(a[1]), P(a[0]), (size_t)a[2]); g_arena_gen++; break;
    case OP_UPDATE_UBO: if (a[0]) memcpy((uint8_t *)P(a[0]) + a[2], blob, (size_t)a[3]); g_arena_gen++; break;
    case OP_CLEAR_BUF: if (a[0]) { uint32_t v = (uint32_t)a[2]; for (uint64_t i = 0; i + 4 <= a[1]; i += 4) memcpy((uint8_t *)P(a[0]) + i, &v, 4); } g_arena_gen++; break;
    case OP_ROWSTRIDE: S.row_stride = (int64_t)a[0]; break;      /* PR #14 (xtlg511): no longer read by the copies (they carry their own strides); kept as state */
    case OP_IMAGESTRIDE: S.image_stride = (int64_t)a[0]; break;
    case OP_DEPTHBOUNDS: {                     /* depth bounds test: fragments pass when the depth already in the buffer is inside [near, far] */
        float v[2];
        memcpy(v, &a[1], 8);
        if (!gl_has(glDepthBoundsEXT, "glDepthBoundsEXT")) break;
        if (a[0] & 1) { glEnable(0x8890 /* DEPTH_BOUNDS_TEST_EXT */); glDepthBoundsEXT(v[0], v[1]); } else glDisable(0x8890);
        break;
    }
    case OP_CONSRASTER: if (a[0] & 1) glEnable(0x9346 /* CONSERVATIVE_RASTERIZATION_NV */); else glDisable(0x9346); break;
    case OP_CONSDILATE: { float v; memcpy(&v, &a[0], 4); if (gl_has(glConservativeRasterParameterfNV, "glConservativeRasterParameterfNV")) glConservativeRasterParameterfNV(0x9379 /* CONSERVATIVE_RASTER_DILATE_NV */, v); break; }
    case OP_SUBPIXEL: if (gl_has(glSubpixelPrecisionBiasNV, "glSubpixelPrecisionBiasNV")) glSubpixelPrecisionBiasNV((GLuint)a[0], (GLuint)a[1]); break;
    case OP_VPSWIZZLE:
        if (!gl_has(glViewportSwizzleNV, "glViewportSwizzleNV")) break;
        for (int i = 0; i < (int)a[1] && (int)a[0] + i < 16; i++) { int v[4]; memcpy(v, blob + i * 16, 16); glViewportSwizzleNV((GLuint)a[0] + (GLuint)i, 0x9350 + (GLenum)(v[0] & 7), 0x9350 + (GLenum)(v[1] & 7), 0x9350 + (GLenum)(v[2] & 7), 0x9350 + (GLenum)(v[3] & 7)); }
        break;
    case OP_TESSLEVELS: if (gl_has(glPatchParameterfv, "glPatchParameterfv")) glPatchParameterfv(a[0] ? 0x8E74 /* PATCH_DEFAULT_OUTER_LEVEL */ : 0x8E73, (const GLfloat *)blob); break;
    case OP_BARRIER:                           /* NVNbarrierBits: 2 / 4 order fragments (rendering to a texture that is being sampled), 8 indirect data, 0x10 texture memory changed */
        if ((a[0] & 6) && gl_has(glTextureBarrier, "glTextureBarrier")) glTextureBarrier();
        if (a[0] & 8) compute_flush();
        if (a[0] & 0x10) g_serial++;
        break;
    case OP_DISCARD: {                         /* the contents of an attachment are no longer needed */
        GLenum att[2] = {GL_DEPTH_ATTACHMENT_, GL_STENCIL_ATTACHMENT_};
        if (!S.have_fbo || !gl_has(glInvalidateFramebuffer, "glInvalidateFramebuffer")) break;
        if (a[0] == ~0ull) glInvalidateFramebuffer(GL_FRAMEBUFFER_, 2, att);
        else { att[0] = GL_COLOR_ATTACHMENT0_ + (GLenum)(a[0] & 7); if (S.color[a[0] & 7]) glInvalidateFramebuffer(GL_FRAMEBUFFER_, 1, att); }
        break;
    }
    case OP_CLEARTEX: {                        /* a0 texture, a4 channel mask, a5 0 float / 1 int / 2 uint, a6 whole level; blob: view, region, colour */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        unsigned mask = (unsigned)a[4];
        if (!t || t->dead || !gl_has(glFramebufferTextureLayer, "glFramebufferTextureLayer")) break;
        memcpy(&v, blob, sizeof v); copy_region(blob + 24, r);
        tex_sync(t);
        t->gpu_written = 1; t->uploaded = 1;
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, layered = t->b.target == 2 || t->b.target == 3 || t->b.target == 4 || t->b.target == 8 || t->b.target == 9;
        if (a[6]) { r[0] = r[1] = r[2] = 0; r[3] = lvl(t->b.width, level); r[4] = lvl(t->b.height, level); r[5] = t->b.target == 2 ? lvl(t->b.depth, level) : 1; }
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, g_fbo_tmp[1]);
        glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_DEPTH_STENCIL_ATTACHMENT_, 0, 0);
        glScissorIndexed(0, r[0], r[1], r[3], r[4]);
        glColorMaski(0, mask & 1, (mask >> 1) & 1, (mask >> 2) & 1, (mask >> 3) & 1);
        for (int z = 0; z < (r[5] > 0 ? r[5] : 1); z++) {
            if (layered) glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, level, (t->b.target == 2 ? 0 : layer) + r[2] + z);
            else glFramebufferTexture(GL_DRAW_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, level);
            if (a[5] == 1) glClearBufferiv(GL_COLOR, 0, (const GLint *)(blob + 48)); else if (a[5] == 2) glClearBufferuiv(GL_COLOR, 0, (const GLuint *)(blob + 48)); else glClearBufferfv(GL_COLOR, 0, (const GLfloat *)(blob + 48));
        }
        glScissorIndexed(0, S.sc0[0], S.sc0[1], S.sc0[2], S.sc0[3]);
        apply_chanmask();
        glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
        g_st.clears++;
        break;
    }
    case OP_DOWNSAMPLE_VIEW: {
        Tex *s = P(a[0]), *d = P(a[1]);
        View sv, dv;
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); memcpy(&dv, blob + 24, sizeof dv);
        int sl = (sv.set & 0x81) == 0x81 ? sv.lvl0 : 0, dl = (dv.set & 0x81) == 0x81 ? dv.lvl0 : 0;
        int sr[6] = {0, 0, 0, lvl(s->b.width, sl), lvl(s->b.height, sl), 1}, dr[6] = {0, 0, 0, lvl(d->b.width, dl), lvl(d->b.height, dl), 1};
        blit_tex(s, &sv, sr, d, &dv, dr, 0);
        break;
    }
    case OP_DRAW_INDIRECT: {                   /* a0 mode, a1 index type, a2 index buffer, a3 indirect data, a4 draws (or the maximum), a5 stride, a6 bit 0 elements, bit 1 count read from memory */
        static const GLenum itype[3] = {GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT, GL_UNSIGNED_INT};
        unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1, n = (unsigned)a[4], stride = (unsigned)a[5];
        int elements = (int)(a[6] & 1);
        compute_flush();                       /* the indirect data may have been written by a dispatch */
        if (a[6] & 2) { uint64_t param; memcpy(&param, blob, 8); uint32_t cnt = param ? rd32(param) : 0; if (cnt < n) n = cnt; }
        if (!stride) stride = elements ? 20 : 16;
        for (unsigned k = 0; k < n && a[3]; k++) {
            uint32_t d[5];                     /* arrays: count, instances, first, base instance; elements: count, instances, first index, base vertex, base instance */
            memcpy(d, P(a[3] + (uint64_t)k * stride), elements ? 20 : 16);
            if (!d[0] || !d[1]) continue;
            if (!draw_prepare()) { g_st.draws_skipped++; g_frame_skipped++; continue; }
            if (elements) {
                if (!a[2]) continue;
                size_t off = arena_put(a[2] + ((uint64_t)d[2] << t), (uint64_t)d[0] << t);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER_, g_arena);
                glDrawElementsInstancedBaseVertexBaseInstance(nvn_prim((unsigned)a[0]), (GLsizei)d[0], itype[t], (const void *)off, (GLsizei)d[1], (GLint)d[3], d[4]);
            } else glDrawArraysInstancedBaseInstance(nvn_prim((unsigned)a[0]), (GLint)d[2], (GLsizei)d[0], (GLsizei)d[1], d[3]);
            g_st.draws++; g_frame_draws++; count_dummy_samples();
        }
        break;
    }
    case OP_DISPATCH_INDIRECT: {
        uint32_t g[3] = {0, 0, 0};
        compute_flush();
        if (a[0]) memcpy(g, P(a[0]), 12);
        compute_dispatch(g[0], g[1], g[2]);
        break;
    }
    case OP_RENDER_ENABLE:                     /* a1 = 0: a0 is the switch; otherwise mode + 1 and a2 the address of two counters (16 bytes each) to compare */
        if (!a[1]) S.render_off = !(a[0] & 1);
        else { compute_flush(); int eq = a[2] && rd64(a[2]) == rd64(a[2] + 16); S.render_off = a[1] == 1 ? !eq : eq; }
        break;
    case OP_COUNTER_RESET: counter_stop((int)a[0]); if (a[0] < 17) g_counter[a[0]].acc = 0; counter_start((int)a[0]); break;
    case OP_COUNTER_REPORT: {                  /* (type, address of an NVNcounterData {counter, timestamp}) */
        uint64_t d[2] = {0, now_ns()};
        int type = (int)a[0];
        if (type > 0 && type < 16 && counter_target(type)) { counter_stop(type); d[0] = g_counter[type].acc; counter_start(type); }
        if (a[1]) { compute_flush(); memcpy(P(a[1]), d, 16); g_arena_gen++; }
        break;
    }
    case OP_REPORT_VALUE: if (a[1]) { uint64_t d[2] = {(uint32_t)a[0], now_ns()}; compute_flush(); memcpy(P(a[1]), d, 16); g_arena_gen++; } break;
    case OP_FENCE: if (a[0]) fence_now(P(a[0]), (int)a[2]); break;
    case OP_WAITSYNC: { SyncObj *o = P(a[0]); if (o && o->s && !o->dead) glWaitSync(o->s, 0, 0xFFFFFFFFFFFFFFFFull); break; }
    case OP_EVENT_SIGNAL: event_signal_at(a[0], (int)a[1], (uint32_t)a[4]); break;
    case OP_EVENT_WAIT: {                      /* the queue stops until the event's value equals (mode 0) or has reached (mode 1, wrapping) the value */
        static int nlog;
        uint32_t want = (uint32_t)a[2];
        DWORD t0 = GetTickCount();
        while (a[0]) {
            uint32_t v = rd32(a[0]);
            if (a[1] == 0 ? v == want : (int32_t)(v - want) >= 0) break;
            if (GetTickCount() - t0 > 5000) { logf_limited(&nlog, 10, "nvn_gl: WaitEvent: the event at %llx still holds %u after 5 s (waiting for %s %u); continuing\n", (unsigned long long)a[0], v, a[1] ? ">=" : "==", want); break; }
            Sleep(0);
        }
        break;
    }
    case OP_DRAW_TEXTURE: {                    /* nvn_c4.c: glDrawTextureNV with the depth, stencil and depth bounds tests off */
        GLuint tex = 0, smp = 0;
        static int nlog;
        if (!S.have_fbo || S.render_off) break;
        if (!resolve_names(S.tpool, S.spool, a[0], &tex, &smp)) { logf_limited(&nlog, 20, "nvn_gl: DrawTexture with handle 0x%llx: no such texture in the bound texture pool; nothing to draw\n", (unsigned long long)a[0]); break; }
        if (!nvn_c4_draw_texture_gl(tex, smp, (const NvnC4DrawTexture *)blob, S.alpha_func, S.alpha_ref)) {
            printf("nvn: stopping: nvnCommandBufferDrawTexture with a texture view that is not 2D or rectangle: what the GPU does is not established\n");
            fflush(stdout);
            report_and_exit(6);
        }
        g_st.draws++; g_frame_draws++;
        break;
    }
    case OP_COVMODTABLE:
        if (!gl_has(glCoverageModulationTableNV, "glCoverageModulationTableNV")) break;
        if (a[0]) { glEnable(0x9331 /* COVERAGE_MODULATION_TABLE_NV */); glCoverageModulationTableNV(16, (const GLfloat *)blob); } else glDisable(0x9331);
        break;
    case OP_DEBUG:
        if (!gl_has(glPushDebugGroup, "glPushDebugGroup")) break;
        if (a[0] == 0) { if (S.debug_depth < 60) { glPushDebugGroup(0x824A /* DEBUG_SOURCE_APPLICATION */, (GLuint)a[1], -1, (const char *)blob); S.debug_depth++; } }
        else if (a[0] == 1) { if (S.debug_depth > 0) { glPopDebugGroup(); S.debug_depth--; } }
        else glDebugMessageInsert(0x824A, 0x8268 /* DEBUG_TYPE_MARKER */, (GLuint)a[1], 0x826B /* DEBUG_SEVERITY_NOTIFICATION */, -1, (const char *)blob);
        break;
    case OP_DISPATCH: { LONG64 pf0 = pf_now(); HS_IN("compute dispatch", 0); compute_dispatch((unsigned)a[0], (unsigned)a[1], (unsigned)a[2]); HS_OUT(); pf_add(PF_DISPATCH, pf0); break; }
    case OP_IMAGES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 8; i++) memcpy(&S.nvn_img[stage][first + i], blob + i * 8, 8);
        break;
    }
    case OP_CALL:
        for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); if (depth < 8) replay(h, depth + 1); }
        break;
    }
    if (g_debug) {
        static int nlog;
        GLenum e = glGetError();
        if (e) { g_st.gl_errors++; logf_limited(&nlog, 60, "nvn_gl: GL error 0x%x after %s\n", e, g_cur_op); }
    }
}
static void replay(uint64_t handle, int depth) {
    size_t len = 0;
    uint8_t *data = list_lookup(handle, &len);
    static int nlog;
    if (!data) { g_st.stale++; logf_limited(&nlog, 10, "nvn_gl: command handle %llx is unknown or expired\n", (unsigned long long)handle); return; }
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        exec(m, depth);
        off += m->size;
    }
}

/* ---------------------------------------------------------------- render-thread jobs */
static void job_submit(void *arg) {
    SubmitJob *j = arg;
    static int nlog;
    g_serial++;
    g_arena_gen++;                               /* guest memory may have changed since the last batch */
    LONG64 pf0 = pf_now();
    for (int i = 0; i < j->count; i++) replay(j->handles[i], 0);
    compute_flush();                             /* the guest may read what compute wrote as soon as the batch has been submitted */
    pf_add(PF_SUBMIT, pf0);
    g_cur_op = "";
    GLenum e = glGetError();
    if (e) { g_st.gl_errors++; logf_limited(&nlog, 30, "nvn_gl: GL error 0x%x in submitted batch %lld\n", e, (long long)g_st.submits); while (glGetError()) {} }
    g_st.submits++;
}

static void job_submit_async(void *arg) { SubmitJob *j = arg; j->handles = (const uint64_t *)(j + 1); job_submit(j); }

static void save_bmp(const char *path, Tex *t) {
    int w = t->b.width, h = t->b.height;
    uint8_t *px = malloc((size_t)w * h * 3), head[54] = {'B', 'M'};
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    static const uint8_t pad[4];
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_BGR_, GL_UNSIGNED_BYTE, px);
    FILE *f = fopen(path, "wb");
    if (!f) { free(px); return; }
    memcpy(head + 2, &size, 4);
    v = 54; memcpy(head + 10, &v, 4);
    v = 40; memcpy(head + 14, &v, 4);
    memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4);
    head[26] = 1; head[28] = 24;
    fwrite(head, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) { fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f); fwrite(pad, 1, row - (uint32_t)w * 3, f); }   /* GL row 0 is the top row */
    fclose(f);
    free(px);
    printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);
    fflush(stdout);
}

/* Frame times (present to present) of the last 300 frames and CPU time per thread, printed with the frame-rate line. */
#include <tlhelp32.h>
#ifndef _WIN32
#include <dirent.h>
#endif
static double g_pf_ft[300];
static struct { DWORD tid; ULONGLONG cpu; } g_pf_thr[512];
static int g_pf_nthr;
static DWORD g_pf_main_tid;
const char *guest_thread_name_of(DWORD tid);                             /* os_threads.c */
static int pf_cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
#ifdef _WIN32
static void pf_threads(double frames) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    DWORD pid = GetCurrentProcessId();
    struct { DWORD tid; double ms; } top[512];
    int n = 0;
    double total = 0;
    if (snap == INVALID_HANDLE_VALUE) return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(0x0800 /* THREAD_QUERY_LIMITED_INFORMATION */, FALSE, te.th32ThreadID);
        FILETIME c, e, k, u;
        if (!h) continue;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            ULONGLONG cpu = (((ULONGLONG)k.dwHighDateTime << 32) | k.dwLowDateTime) + (((ULONGLONG)u.dwHighDateTime << 32) | u.dwLowDateTime), prev = 0;
            int i;
            for (i = 0; i < g_pf_nthr && g_pf_thr[i].tid != te.th32ThreadID; i++) {}
            if (i < g_pf_nthr) prev = g_pf_thr[i].cpu; else if (g_pf_nthr < 512) { g_pf_thr[g_pf_nthr].tid = te.th32ThreadID; i = g_pf_nthr++; }
            if (i < 512) g_pf_thr[i].cpu = cpu;
            if (n < 512) { top[n].tid = te.th32ThreadID; top[n].ms = (double)(cpu - prev) / 10000.0 / frames; total += top[n].ms; n++; }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    printf("nvn_perf: cpu ms/frame by thread (all %.1f):", total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (top[i].ms > 0.05 && (best < 0 || top[i].ms > top[best].ms)) best = i;
        if (best < 0) break;
        const char *name = top[best].tid == g_render_tid ? "render(host)" : top[best].tid == g_pf_main_tid ? "main" : guest_thread_name_of(top[best].tid);
        printf(" %s[%lu] %.1f", name ? name : "host", (unsigned long)top[best].tid, top[best].ms);
        top[best].ms = 0;
    }
    printf("\n");
}
#else
static void pf_threads(double frames) {                 /* Linux build: /proc/self/task/<tid>/stat, utime + stime */
    struct { DWORD tid; double ms; } top[512];
    int n = 0;
    double total = 0;
    long hz = sysconf(_SC_CLK_TCK);
    DIR *d = opendir("/proc/self/task");
    struct dirent *de;
    if (!d) return;
    while ((de = readdir(d))) {
        DWORD tid = (DWORD)strtoul(de->d_name, NULL, 10);
        char sp[96], buf[512];
        if (!tid) continue;
        snprintf(sp, sizeof sp, "/proc/self/task/%lu/stat", (unsigned long)tid);
        FILE *f = fopen(sp, "r");
        if (!f) continue;
        size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
        const char *rp = strrchr(buf, ')');
        unsigned long long ut = 0, st = 0;
        if (!rp || sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        ULONGLONG cpu = (ut + st) * 10000000ull / (ULONGLONG)(hz > 0 ? hz : 100), prev = 0;
        int i;
        for (i = 0; i < g_pf_nthr && g_pf_thr[i].tid != tid; i++) {}
        if (i < g_pf_nthr) prev = g_pf_thr[i].cpu; else if (g_pf_nthr < 512) { g_pf_thr[g_pf_nthr].tid = tid; i = g_pf_nthr++; }
        if (i < 512) g_pf_thr[i].cpu = cpu;
        if (n < 512) { top[n].tid = tid; top[n].ms = (double)(cpu - prev) / 10000.0 / frames; total += top[n].ms; n++; }
    }
    closedir(d);
    printf("nvn_perf: cpu ms/frame by thread (all %.1f):", total);
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < n; i++) if (top[i].ms > 0 && (best < 0 || top[i].ms > top[best].ms)) best = i;
        if (best < 0 || top[best].ms < 0.05) break;
        const char *g = guest_thread_name_of(top[best].tid);
        printf(" %s %.1f", g ? g : top[best].tid == g_render_tid ? "render" : "host", top[best].ms);
        top[best].ms = 0;
    }
    printf("\n");
}
#endif
static DWORD WINAPI pf_threads_thread(LPVOID arg) { for (;;) { WaitForSingleObject((HANDLE)arg, INFINITE); pf_threads(300.0); } }
static void pf_frame_mark(LONG64 frame) {
    static LONG64 last, freq, snap[PF_N], snapn[PF_N], b0[3];
    LONG64 now = pf_now();
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    if (last) g_pf_ft[frame % 300] = (double)(now - last) * 1000.0 / (double)freq;
    if (last && g_hitch_ms) hitch_frame((long long)frame, (double)(now - last) * 1000.0 / (double)freq, g_frame_dispatch > 0);
    last = now;
    if (frame % 300) return;
    double ft[300], sum = 0;
    memcpy(ft, g_pf_ft, sizeof ft);
    qsort(ft, 300, sizeof ft[0], pf_cmp_d);
    for (int i = 0; i < 300; i++) sum += ft[i];
    printf("nvn_perf: frames %lld..%lld frame time ms: avg %.2f min %.2f median %.2f p99 %.2f max %.2f | fps avg %.1f, 1%% low %.1f\n", (long long)frame - 300, (long long)frame,
           sum / 300, ft[0], ft[150], ft[296], ft[299], sum > 0 ? 300000.0 / sum : 0.0, ft[296] > 0 ? 1000.0 / ft[296] : 0.0);
    printf("nvn_perf: ms/frame (calls/frame):");
    for (int k = 0; k < PF_N; k++) {
        LONG64 t = g_pf[k], c = g_pfn[k];
        printf(" %s %.2f (%.1f)", k_pf_name[k], (double)(t - snap[k]) * 1000.0 / (double)freq / 300.0, (double)(c - snapn[k]) / 300.0);
        snap[k] = t; snapn[k] = c;
    }
    printf(" | MB/frame: streamed %.2f, read back %.2f, hashed %.2f\n", (double)(g_pf_arena_bytes - b0[0]) / 300.0 / 1048576.0, (double)(g_pf_cflush_bytes - b0[1]) / 300.0 / 1048576.0,
           (double)(g_pf_hash_bytes - b0[2]) / 300.0 / 1048576.0);
    b0[0] = g_pf_arena_bytes; b0[1] = g_pf_cflush_bytes; b0[2] = g_pf_hash_bytes;
    {
        static int on = -1;
        static LONG64 z[16];
        if (on < 0) on = getenv("SSBU_GLSTATS") != NULL;
        if (on) {
            LONG64 v[16] = {g_gs_draws, g_gs_prog_same, g_gs_attr_set, g_gs_attr_same, g_gs_vbuf_bind, g_gs_buf_bind, g_st.tex_created, g_st.tex_uploads, g_st.fbo_made, g_st.views,
                            g_st.handles, g_shc_compiled, g_shd_loaded, g_shc_hit, g_pfn[PF_CFLUSH], g_pfn[PF_WAIT_SUBMIT] + g_pfn[PF_WAIT_PRESENT] + g_pfn[PF_WAIT_OTHER]};
            double d[16];
            for (int k = 0; k < 16; k++) { d[k] = (double)(v[k] - z[k]) / 300.0; z[k] = v[k]; }
            printf("nvn_glstats: per frame: guest waits for the render thread %.1f | compute read-backs (glGetNamedBufferSubData) %.1f | created: textures %.3f, uploads %.3f, framebuffers %.3f, views %.3f, bindless handles %.3f | "
                   "shader stages: built %.3f (from disk %.3f), reused %.3f | draws %.1f: same two programs as the previous draw %.1f; vertex attribute formats set %.1f, unchanged since last set %.1f; vertex buffer binds %.1f; uniform/constant buffer binds %.1f\n",
                   d[15], d[14], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[0], d[1], d[2], d[3], d[4], d[5]);
        }
    }
    {   /* P3 round 2: the snapshot of all threads (CreateToolhelp32Snapshot) took 30-70 ms on the render thread: one long
         * frame in every 300 (hitch log, "present" scope). It is taken by a helper thread now. */
        static HANDLE ev;
        if (!ev) { ev = CreateEventA(NULL, FALSE, FALSE, NULL); CloseHandle(CreateThread(NULL, 0, pf_threads_thread, ev, 0, NULL)); }
        SetEvent(ev);
    }
}

static void job_present(void *arg);
static void job_fence(void *arg);
static void job_sync_wait(void *arg);
static void job_flush(void *arg);
static void job_finish(void *arg);
static void joblog(void (*fn)(void *), LONG64 t) {
    static LONG64 freq;
    if (!freq) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); freq = f.QuadPart; }
    printf("joblog: frame %lld t=%.3f ms thread %lu %s%c", (long long)g_joblog_cur, (double)(t % (freq * 100)) * 1000.0 / (double)freq, (unsigned long)GetCurrentThreadId(),
           fn == job_submit ? "SubmitCommands" : fn == job_present ? "PresentTexture" : fn == job_fence ? "FenceSync" : fn == job_sync_wait ? "SyncWait" : fn == job_flush ? "Flush" :
           fn == job_finish ? "Finish" : "other", 10);
}
static void job_present(void *arg) {
    pair_off();
    pw_gate();
    PresentJob *j = arg;
    static int shot = -2, every, shot_from;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    RECT r;
    LONG64 frame = g_st.frames + 1;
    LONG64 pf0 = pf_now();
    if (shot == -2) { shot = getenv("SSBU_SHOT") ? atoi(getenv("SSBU_SHOT")) : -1; every = getenv("SSBU_SHOT_EVERY") ? atoi(getenv("SSBU_SHOT_EVERY")) : 0; shot_from = getenv("SSBU_SHOT_FROM") ? atoi(getenv("SSBU_SHOT_FROM")) : 0; }
    GetClientRect(g_hwnd, &r);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB_);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER_, 0);
    if (t && !t->dead && t->o.gl) {
        int sr[4] = {0, 0, t->b.width, t->b.height};
        if (j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(sr, j->win->crop, sizeof sr);
        glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
        glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
        void display_fit(int sw, int sh, int dw, int dh, int out[4]);      /* PR #24, display.c: the picture keeps its 16:9 shape */
        int d4[4];
        display_fit(16, 9, (int)r.right, (int)r.bottom, d4);
        if (d4[2] != r.right || d4[3] != r.bottom) { glColorMaski(0, 1, 1, 1, 1); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT); }      /* the game's next draw restores its own mask */
        glBlitFramebuffer(sr[0], sr[1], sr[0] + sr[2], sr[1] + sr[3], d4[0], r.bottom - d4[1], d4[0] + d4[2], r.bottom - d4[1] - d4[3], GL_COLOR_BUFFER_BIT, GL_LINEAR);
        if (g_drawlog_frame >= 0 && frame == g_drawlog_frame + 1) save_bmp("frame_drawlog.bmp", t);      /* the frame the draw log describes */
        { int step = nvn_front_take_step_shot(); if (step) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sstep_%03d.bmp", dir ? dir : "", dir ? "/" : "", step - 1); save_bmp(path, t); } }
        nvn_front_draw_tag_clear();
        if (j->shot_fight) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sfight_%05d.bmp", dir ? dir : "", dir ? "/" : "", j->shot_fight); save_bmp(path, t); printf("shot: fight frame %d saved at presented frame %lld (OpenGL)\n", j->shot_fight, (long long)frame); }
        if (frame == shot || (every > 0 && frame >= shot_from && frame % every == 0)) {
            char path[300];
            const char *dir = getenv("SSBU_SHOT_DIR");                /* default: the current directory */
            snprintf(path, sizeof path, "%s%sframe_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)frame);
            save_bmp(path, t);
        }
        if (getenv("SSBU_RTDUMP") && frame == atoi(getenv("SSBU_RTDUMP"))) {       /* debug: every colour target in use, as rt_<n>.bmp */
            for (int k = 0; k < 256; k++) {
                Tex fake;
                char path[64];
                GLint w = 0, h = 0;
                if (!g_fbos[k].used || !g_fbos[k].k.color[0]) continue;
                glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbos[k].fbo);
                glBindTexture(GL_TEXTURE_2D, g_fbos[k].k.color[0]);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
                glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
                if (w <= 0 || h <= 0) continue;
                memset(&fake, 0, sizeof fake);
                fake.b.width = w; fake.b.height = h;
                snprintf(path, sizeof path, "rt_%03d_gl%u.bmp", k, g_fbos[k].k.color[0]);
                save_bmp(path, &fake);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
        }
    } else {
        glClearColor(0, 0, 0, 1);                 /* nothing has been drawn into the presented texture yet */
        glClear(GL_COLOR_BUFFER_BIT);
    }
#ifdef _WIN32
    { LONG64 pf1 = pf_now(); HS_IN("SwapBuffers", 0); SwapBuffers(g_dc); HS_OUT(); pf_add(PF_SWAP, pf1); }
#endif
    pf_add(PF_PRESENT, pf0);
    pf_frame_mark(frame);
    g_joblog_cur = frame;
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
    g_st.frames = frame;
    if (frame <= 3 || frame % 600 == 0 || g_frame_draws != 0 && g_logn < 40 && (g_logn++, 1))
        printf("nvn_gl: frame %lld: %lld draws (%lld skipped), %lld clears | totals: %lld lists, %lld textures, %lld uploads (%lld MB), %lld programs ok, %lld rejected, %lld GL errors\n",
               (long long)frame, (long long)g_frame_draws, (long long)g_frame_skipped, (long long)g_frame_clears, (long long)g_st.lists, (long long)g_st.tex_created,
               (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20), (long long)g_st.prog_ok, (long long)g_st.prog_fail, (long long)g_st.gl_errors);
    fflush(stdout);
    {   /* frame rate over the last 300 presented frames */
        static ULONGLONG t0;
        static LONG64 d0, c0;
        if (frame % 300 == 0) {
            ULONGLONG now = GetTickCount64();
            if (t0 && now > t0) printf("nvn_gl: frames %lld..%lld: %.1f fps, %lld draws and %lld dispatches per frame\n", (long long)frame - 300, (long long)frame, 300000.0 / (double)(now - t0),
                                       (long long)(g_st.draws - d0) / 300, (long long)(g_st.dispatches - c0) / 300);
            t0 = now; d0 = g_st.draws; c0 = g_st.dispatches;
            if (frame % 3000 == 0 || g_dummy_draws) { static long long last = -1; if (g_dummy_draws != last) { last = g_dummy_draws; printf("nvn_gl: dummy texture: %lld of %lld draws reference a unit that holds it (%lld units unbound, %lld units with a dead or unknown handle)\n", g_dummy_draws, (long long)g_st.draws, g_dummy_unbound, g_dummy_dead); } }
        }
    }
    g_prev_frame_draws = g_frame_draws;
    if (g_frame_dispatch >= (g_drawlog_mindisp_n > 0 ? g_drawlog_mindisp_n : 1)) g_disp_frames++;
    g_frame_dispatch = 0; g_ncrange = 0;
    input_frame_draws((int)g_frame_draws);        /* nnhid.c: scripted input can wait for a screen by its draw count */
    g_frame_draws = g_frame_skipped = g_frame_clears = 0;
    g_frame_draw_index = 0;
    list_collect();
}

static void job_write_texels(void *arg) {
    WriteJob *j = arg;
    Tex *t = j->tex;
    Fmt f;
    static int nlog;
    if (!t->o.gl) tex_realize(t);
    if (!fmt_get(t->b.format, &f)) return;
    int level = j->has_view && (j->view.set & 0x81) == 0x81 ? j->view.lvl0 : 0, layer = j->has_view && (j->view.set & 0x82) == 0x82 ? j->view.lay0 : 0;
    const int *r = j->region;
    while (glGetError()) {}
    if (j->row_stride && f.kind != 4) glPixelStorei(GL_UNPACK_ROW_LENGTH_, (GLint)(j->row_stride / f.bpb));
    if (t->b.target == 4 || t->b.target == 9 || t->b.target == 8) layer += r[2];
    tex_put(t, &f, level, layer, r[0], r[1], r[2], r[3], r[4], r[5] > 0 ? r[5] : 1, j->data);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_, 0);
    GLenum e = glGetError();
    if (e) logf_limited(&nlog, 10, "nvn_gl: WriteTexels on texture %dx%dx%d format 0x%x region %d,%d,%d %dx%dx%d failed: GL error 0x%x\n", t->b.width, t->b.height, t->b.depth, t->b.format,
                        r[0], r[1], r[2], r[3], r[4], r[5], e);
    t->gpu_written = 1; t->uploaded = 1;
    g_st.copies++;
}

/* The render thread's job loop (both back ends). */
static void render_loop(void) {
    for (;;) {
        WaitForSingleObject(g_job_event, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_job_lock);
            Job *j = g_job_head;
            if (j) { g_job_head = j->next; if (!g_job_head) g_job_tail = NULL; }
            LeaveCriticalSection(&g_job_lock);
            if (!j) break;
            HANDLE done = j->done;
            if (g_rb->idle) g_rb->idle();
            g_hitch_rt = j->fn == g_rb->submit || j->fn == g_rb->submit_async ? "submitted commands (outside the named scopes)" : j->fn == g_rb->present ? "present (blit)" : j->fn == g_rb->sync_wait ? "SyncWait job" : "other job (texel read/write, finish, barrier, ...)";
            j->fn(j->arg);                       /* a member of g_rb (seam step S2), or a job of the front end / of the back end itself */
            g_hitch_rt = "idle";
            if (done) SetEvent(done); else { free(j); InterlockedDecrement(&g_async_pending); }      /* gl_post: nobody waits */
        }
    }
}

static DWORD WINAPI render_thread(LPVOID arg) {
    (void)arg;
    pf_job_submit = g_rb->submit; pf_job_present = g_rb->present;
    g_joblog_frame = getenv("SSBU_JOBLOG") ? atoi(getenv("SSBU_JOBLOG")) : 0;
#ifdef _WIN32
    PIXELFORMATDESCRIPTOR pfd;
#else
    int pfd = 0;
#endif
    g_render_tid = GetCurrentThreadId();
    {   /* Round 17, MEASUREMENT: SSBU_RENDER_PRIO=1 above normal, 2 highest (default: normal, as before). The game's own
         * threads run above normal when the game asked for a priority under 16 (os_threads.c host_priority). */
        const char *e = getenv("SSBU_RENDER_PRIO");
        int p = e ? atoi(e) : 0;
        if (p > 0) { SetThreadPriority(GetCurrentThread(), p >= 2 ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_ABOVE_NORMAL); printf("nvn: render thread priority %s (SSBU_RENDER_PRIO)\n", p >= 2 ? "highest" : "above normal"); }
    }
    if (g_vk) {                                 /* V1 step S0: no OpenGL context, no shader workers; the jobs go to nvn_vk.c (vk_route) */
        nvn_vk_init(g_hwnd);
        hitch_set_render();
        SetEvent(g_ready_event);
        render_loop();
        return 0;
    }
#ifndef _WIN32
    (void)pfd;
    printf("nvn_gl: the OpenGL renderer is not part of the Linux build\n"); fflush(stdout); ExitProcess(5);
#else
    g_dc = GetDC(g_hwnd);
    memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd; pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32;
    SetPixelFormat(g_dc, ChoosePixelFormat(g_dc, &pfd), &pfd);
    HGLRC rc = wglCreateContext(g_dc);
    if (!rc || !wglMakeCurrent(g_dc, rc)) { printf("nvn_gl: cannot create an OpenGL context (error %lu)\n", GetLastError()); fflush(stdout); ExitProcess(5); }
    int missing = 0;
#define X(ret, name, args) gl##name = (void *)wglGetProcAddress("gl" #name); if (!gl##name) { printf("nvn_gl: the driver lacks gl" #name "\n"); missing++; }
    GLF(X)
#undef X
#define X(ret, name, args) gl##name = (void *)wglGetProcAddress("gl" #name);
    GLO(X)
#undef X
    printf("nvn_gl: %s | %s | OpenGL %s\n", (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    if (missing) { printf("nvn_gl: %d required OpenGL functions are missing; this backend needs an NVIDIA driver\n", missing); fflush(stdout); ExitProcess(5); }
    BOOL (WINAPI *swap_interval)(int) = (void *)wglGetProcAddress("wglSwapIntervalEXT");
    if (swap_interval) swap_interval(0);
    g_debug = getenv("SSBU_GLDEBUG") != NULL;
    g_dbg_noscissor = getenv("SSBU_NOSCISSOR") != NULL;
    g_dbg_flipface = getenv("SSBU_FLIPFACE") != NULL;
    g_dbg_nocompute = getenv("SSBU_NOCOMPUTE") != NULL;
    g_drawlog_mindisp_n = getenv("SSBU_DRAWLOG_MINDISP") ? atoi(getenv("SSBU_DRAWLOG_MINDISP")) : 0;
    if (getenv("SSBU_SKIPDRAW")) g_dbg_skipdraw = atoi(getenv("SSBU_SKIPDRAW"));
    glEnable(GL_DEBUG_OUTPUT_);
    /* P3 (hitch log, tests/logs/p3b_hitch0.log against p3b_hitch_dbg.log): without synchronous debug output this driver
     * hands the GL calls of the render thread to a thread of its own, and every call that returns something (texture
     * handles, program status, glGetError, buffer read-back) then waits for everything queued before it: 0.1-2 s frames
     * whenever a new shader or texture arrives (65 s of frames over 25 ms in a 170 s run; 13 s with it on). Synchronous
     * debug output is a debugging state of the context: it changes when the callback is called, not what is drawn; the
     * callback prints errors only unless SSBU_GLDEBUG is set, as before. SSBU_GL_DRIVER_THREAD=1 = the old behaviour. */
    if (g_debug || !getenv("SSBU_GL_DRIVER_THREAD")) glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_);
    g_fdlog = getenv("SSBU_FDLOG") != NULL;
    if (g_fdlog && glDebugMessageControl) glDebugMessageControl(0x1100 /* DONT_CARE */, 0x1100, 0x1100, 0, NULL, 1);     /* every severity, low ones too */
    glDebugMessageCallback((void *)debug_cb, NULL);
    glGetIntegerv(GL_MAX_PROGRAM_PARAMETER_BUFFER_BINDINGS_NV_, &g_max_pbuf);
    printf("nvn_gl: parameter buffer bindings per program: %d; window origin mode %d, depth mode %d\n", g_max_pbuf, g_origin_mode, g_depth_mode);
    glClipControl(g_origin_mode ? GL_UPPER_LEFT_ : GL_LOWER_LEFT_, g_depth_mode ? GL_ZERO_TO_ONE_ : GL_NEGATIVE_ONE_TO_ONE_);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glEnable(GL_FRAMEBUFFER_SRGB_);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS_);
    glEnable(GL_SCISSOR_TEST);
    for (GLuint i = 0; i < 16; i++) glScissorIndexed(i, 0, 0, 16384, 16384);
    glGenBuffers(1, &g_arena);
    glBindBuffer(GL_ARRAY_BUFFER_, g_arena);
    glBufferData(GL_ARRAY_BUFFER_, (ptrdiff_t)ARENA_SIZE, NULL, GL_STREAM_DRAW_);
    glGenFramebuffers(1, &g_fbo_read);
    glGenFramebuffers(2, g_fbo_tmp);
    glGenProgramPipelines(1, &g_pipeline);
    static const uint8_t black[4] = {0, 0, 0, 255};
    { void album_movie_frame_source_attach(void); album_movie_frame_source_attach(); }   /* close4: queue_present feeds the movie maker (job_present_movie) */
    glGenTextures(1, &g_dummy_tex);
    glBindTexture(GL_TEXTURE_2D, g_dummy_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, 0x8058, 1, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    g_dummy_handle = glGetTextureHandleARB(g_dummy_tex);
    glMakeTextureHandleResidentARB(g_dummy_handle);
    for (int s = 0; s < 6; s++) { for (int i = 4; i < 36; i++) S.gl_tex[s][i] = g_dummy_handle; for (int i = 0; i < 32; i++) { uint32_t d[4] = {1, 1, 1, 1}; memcpy(&S.gl_tex[s][48 + 2 * i], d, 16); } S.tex_dirty[s] = 1; }
    S.chanmask = 0xffffffffu; S.svmask[0] = S.svmask[1] = 0xff; S.swmask[0] = S.swmask[1] = 0xff;
    S.sc0[2] = S.sc0[3] = 16384;
    S.ds = 0x1111118822ULL;                     /* depth func LESS, stencil funcs ALWAYS, ops KEEP */
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers(g_dc);
#endif
    fflush(stdout);
    shd_init();
    hitch_set_render();
    nvn_c4_gl_selftest_hook();                  /* SSBU_NVN_GLTEST: OpenGL self-test of nvn_c4.c, then exit */
    SetEvent(g_ready_event);
    render_loop();
    return 0;
}

/* Called by display.c once the window exists. */
void nvn_gl_start(void *hwnd) {
    g_hwnd = hwnd;
    {   /* V1: which back end. SSBU_RENDERER=gl (default) | vk */
        const char *e = getenv("SSBU_RENDERER");
        if (e && e[0] && strcmp(e, "gl") && strcmp(e, "vk")) { printf("nvn: SSBU_RENDERER=%s is not a renderer (gl, vk)\n", e); fflush(stdout); ExitProcess(5); }
        /* Track V session 4: VULKAN IS THE DEFAULT. SSBU_RENDERER=gl|vk wins; else renderer = vulkan | opengl of config\graphics.ini. */
        g_vk = e && e[0] ? !strcmp(e, "vk") : !gfx_options()->renderer;
#ifndef _WIN32
        if (!g_vk) { printf("nvn: the OpenGL renderer is not part of the Linux build: Vulkan instead\n"); g_vk = 1; }
#endif
        g_rb = g_vk ? &rb_vk : &rb_gl;
        printf("nvn: renderer: %s (%s)\n", g_vk ? "Vulkan (nvn_vk.c)" : "OpenGL (nvn_gl.c)", e && e[0] ? "SSBU_RENDERER" : "graphics.ini; the default is Vulkan");
    }
    InitializeCriticalSection(&g_job_lock);
    InitializeCriticalSection(&g_dead_lock);
    g_job_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_ready_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    { const char *e = getenv("SSBU_STACKS_AT"); void nvn_vk_stacks_at(int seconds); if (e && atoi(e) > 0) nvn_vk_stacks_at(atoi(e)); }      /* session 13, TEST: every thread's call stack once, that many seconds from here, with either renderer (nvn_vk_wd.c) */
    CreateThread(NULL, 16 << 20, render_thread, NULL, 0, NULL);
    WaitForSingleObject(g_ready_event, INFINITE);
}

void nvn_gl_report(FILE *out) {
    fprintf(out, "\nnvn_gl: %lld frames, %lld batches, %lld command lists (%lld unknown handles), %lld draws (+%lld skipped), %lld clears, %lld copies\n", (long long)g_st.frames,
            (long long)g_st.submits, (long long)g_st.lists, (long long)g_st.stale, (long long)g_st.draws, (long long)g_st.draws_skipped, (long long)g_st.clears, (long long)g_st.copies);
    fprintf(out, "nvn_gl: textures %lld (uploads %lld, %lld MB, views %lld, bindless handles %lld, unmapped formats %lld), framebuffers %lld (%lld incomplete)\n", (long long)g_st.tex_created,
            (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20), (long long)g_st.views, (long long)g_st.handles, (long long)g_st.unknown_fmt, (long long)g_st.fbo_made, (long long)g_st.fbo_bad);
    fprintf(out, "nvn_gl: nvnProgramSetShaders calls %ld, stages compiled %lld, rejected %lld, GL errors %lld, streamed %lld MB\n", (long)g_setshaders_calls, (long long)g_st.prog_ok,
            (long long)g_st.prog_fail, (long long)g_st.gl_errors, (long long)(g_st.arena_bytes >> 20));
    fprintf(out, "nvn_gl: shader cache: %lld stages compiled by the driver, %lld taken from the cache by machine code, %lld by identical GLSL text; texture memory checks: %lld unwritten (write watch), %lld written and hashed, %lld not watched (hashed)\n",
            (long long)g_shc_compiled, (long long)g_shc_hit, (long long)g_shc_src_hit, (long long)g_ww_clean, (long long)g_ww_dirty, (long long)g_ww_unwatched);
    fprintf(out, "nvn_gl: shader binaries on disk: %lld loaded, %lld compiled and saved by the render thread, %lld refused by the driver (compiled again)\n", (long long)g_shd_loaded, (long long)g_shd_saved, (long long)g_shd_refused);
    for (int i = 0; g_fdlog && i < 64 && g_fd_ids[i].n; i++) fprintf(out, "fdlog-id: %u type %x severity %x: %lld messages, first: %s\n", g_fd_ids[i].id, g_fd_ids[i].type, g_fd_ids[i].sev, (long long)g_fd_ids[i].n, g_fd_ids[i].text);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f);
      fprintf(out, "nvn_gl: shader pairs made ready by the workers (SSBU_PAIR_WARM=%d): %lld from pairs.txt + %lld programs handed over; %lld warm draws, %lld of them over 8 ms (not in the driver's cache), %.1f s in all; %lld known already, %lld without binaries or not translated, %lld stages compiled for it\n",
              pw_mode(), (long long)g_pw_named, (long long)g_pw_progs, (long long)g_pw_warmed, (long long)g_pw_cold_n, (double)g_pw_ticks / (double)f.QuadPart, (long long)g_pw_dup, (long long)g_pw_missing, (long long)g_pw_compiled);
      fprintf(out, "nvn_gl: presents held for shader preparation: %lld times, %.2f s in all\n", (long long)g_pw_gates, (double)g_pw_gate_ticks / (double)f.QuadPart); }
    fprintf(out, "nvn_gl: shader pairs drawn (pairs.txt): %lld known at start, %lld added in this run\n", (long long)g_prec_loaded, (long long)g_prec_added);
    if (pair_mode()) fprintf(out, "nvn_gl: linked pair programs (SSBU_PAIR=%d): %lld loaded from disk, %lld linked here, %lld not linkable, %lld binaries refused, %lld draws used one\n", pair_mode(), (long long)g_pair_loaded, (long long)g_pair_linked, (long long)g_pair_failed, (long long)g_pair_refused, (long long)g_pair_draws);
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fprintf(out, "nvn_gl: first draws of a program pair preceded by a discarded point: %lld (%lld over 4 ms, %.0f ms in all), not done %lld (counter running or vertex stage with side effects)\n", (long long)g_fdw_n, (long long)g_fdw_slow, (double)g_fdw_ticks * 1000.0 / (double)f.QuadPart, (long long)g_fdw_skipped); }
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fprintf(out, "nvn_gl: nvnProgramSetShaders held its caller %lld times, %.1f s in all, while all workers were compiling\n", (long long)g_shw_held, (double)g_shw_held_ticks / (double)f.QuadPart); }
    fprintf(out, "nvn_gl: shader variants (variants.txt): %lld known at start, %lld of them queued for the workers, %lld added in this run\n", (long long)g_shv_loaded, (long long)g_shv_queued, (long long)g_shv_added);
    fprintf(out, "nvn_gl: shader workers (%d): %lld stages queued, %lld done (%lld compiled and saved, %lld already on disk, %lld not translated or not saved)\n", g_shw_n, (long long)g_shw_queued, (long long)g_shw_done,
            (long long)g_shw_compiled, (long long)g_shw_present, (long long)g_shw_failed);
    fprintf(out, "nvn_gl: shaders given with NV assembly text: %ld; with Maxwell machine code only: %ld (translated to GLSL: %lld stages, not translatable yet: %lld; draws through GLSL: %lld)\n", (long)g_with_text,
            (long)g_without_text, (long long)g_st.glsl_ok, (long long)g_st.glsl_fail, (long long)g_st.glsl_draws);
    fprintf(out, "nvn_gl: compute dispatches %lld (run %lld, skipped %lld), %lld MB copied back to guest memory, draws using a vertex buffer written by compute in the same frame: %lld\n",
            (long long)g_st.dispatches, (long long)g_st.dispatch_run, (long long)g_st.dispatch_skipped, (long long)(g_st.compute_bytes >> 20), (long long)g_st.draws_from_compute);
    for (int h = 0; h < 64 && g_chist[h].serial; h++)
        fprintf(out, "nvn_gl:   compute program #%d: %lld dispatches (%lld run), %lld groups\n", g_chist[h].serial, (long long)g_chist[h].calls, (long long)g_chist[h].run, (long long)g_chist[h].groups);
    for (int s = 0; s < 6; s++) {
        int any = 0;
        for (int i = 0; i < 32; i++) any |= g_bufhist[s][i] != 0;
        if (!any) continue;
        fprintf(out, "nvn_gl: program.buffer[n] uses in %s programs (n:count):", k_stage_name[s]);
        for (int i = 0; i < 32; i++) if (g_bufhist[s][i]) fprintf(out, " %d:%ld", i, (long)g_bufhist[s][i]);
        fprintf(out, "\n");
    }
    fprintf(out, "nvn_gl: program.storage[n] uses (n:count):");
    for (int i = 0; i < 32; i++) if (g_storhist[i]) fprintf(out, " %d:%ld", i, (long)g_storhist[i]);
    fprintf(out, "\n");
    hitch_report(out);
    if (g_vk) { nvn_vk_report(out); vk_shutdown_at_exit(); }
}

/* ================================================================ the NVN entry points */
static void n_nop(Ctx *c) { (void)c; }
static void n_true(Ctx *c) { RET(1); }
static void n_zero(Ctx *c) { RET(0); }

/* ---- device */
static void dev_origin_mode(Ctx *c) { g_origin_mode = (int)A1; }
static void dev_depth_mode(Ctx *c) { g_depth_mode = (int)A1; }
/* The real values (close5; until then this port gave textureID | samplerID << 32 and the bare ID for the other three):
 *   nvnDeviceGetTextureHandle [sdk 0x41bc80]         sign-extended 32-bit (textureID | samplerID << 20), then bit 32 set
 *   nvnDeviceGetTexelFetchHandle [0x41bc70]          sign-extended textureID, bit 32 set
 *   nvnDeviceGetImageHandle [0x41bc00]               sign-extended textureID */
static void dev_texture_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)((uint32_t)A1 | (uint32_t)A2 << 20) | 0x100000000ull); }   /* (device, textureID, samplerID) */
static void dev_texel_fetch_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1 | 0x100000000ull); }
static void dev_image_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1); }

/* ---- programs */
static volatile LONG g_prog_serial;
static void prog_initialize(Ctx *c) {
    Prog *p = hnew(A0, K_PROG, sizeof(Prog));
    p->serial = (int)InterlockedIncrement(&g_prog_serial);
    p->guest = A0;
    RET(1);
}
static void prog_finalize(Ctx *c) {
    gl_barrier();
    Prog *p = hobj(A0, K_PROG);
    if (!p) return;
    InterlockedExchange(&p->dead, 1);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
    defer_free(K_PROG, p);
}
/* Debug aid SSBU_SHADERDUMP_ALL=<dir>: every shader stage handed to nvnProgramSetShaders, whole, as <dir>/s<program>_<i>.mxd for the
 * offline harness (mxtest.exe). File: "MXD1", u32 program serial, u32 index in the call, u32 control bytes, u32 data bytes, then the
 * control section (at least up to +0x720, plus the assembly text if any) and the code section (header + code + constants). */
static void dump_stage_all(int serial, int index, uint64_t control, uint64_t data) {
    static const char *dir;
    static volatile LONG init;
    if (!init) { dir = getenv("SSBU_SHADERDUMP_ALL"); InterlockedExchange(&init, 1); }
    if (!dir || !control) return;
    uint32_t text_off = rd32(control + 0x14), text_len = rd32(control + 0x18), ctl_len = 0x720, data_len = 0;
    if (text_len && text_len < (1u << 20) && text_off < (1u << 20) && text_off + text_len > ctl_len) ctl_len = text_off + text_len;
    { uint32_t tail = rd32(control + 0x20); if (tail && tail < (1u << 20) && tail + 0x400 > ctl_len) ctl_len = tail + 0x400; }      /* the block after the text */
    if (data) {
        uint32_t code_len = rd32(control + 0x6f8), c_size = rd32(control + 0x6fc), c_off = rd32(control + 0x700), total = rd32(control + 0x704);
        if (code_len < (4u << 20)) data_len = 0x30 + code_len;
        if (c_off < (4u << 20) && c_size < (1u << 20) && c_off + c_size > data_len) data_len = c_off + c_size;
        if (total > data_len && total < (8u << 20)) data_len = total;
    }
    char path[300];
    snprintf(path, sizeof path, "%s/s%05d_%d.mxd", dir, serial, index);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint32_t head[5] = {0x3144584du, (uint32_t)serial, (uint32_t)index, ctl_len, data_len};
    fwrite(head, 4, 5, f);
    fwrite(P(control), 1, ctl_len, f);
    if (data_len) fwrite(P(data), 1, data_len, f);
    fclose(f);
}

/* nvnProgramSetShaders, guest thread (P3 round 4): one job that makes the program's vertex and fragment stage ready (binaries
 * compiled when missing) and has the driver do its first-draw work for the pair. Returns 0 when it does not apply (the
 * stages are then handed to the workers one by one, as before). After start-up the caller is held until the workers
 * have nothing left, so a fighter or stage that is loaded is ready when its loading ends. */
static int pw_queue_prog(Prog *p) {
    int m = pw_mode();
    Stage *v = &p->st[0], *f = &p->st[1];
    if (!m || g_shd_mode <= 0 || !g_shw_n || g_shw_ctx_failed || (m < 2 && g_st.frames < 300) || GetCurrentThreadId() == g_render_tid) return 0;
    if (!(v->code && v->code_len > 0 && v->glsl_state == 0) || p->st[2].state || p->st[3].state || p->st[4].state) return 0;
    int has_f = f->code && f->code_len > 0 && f->glsl_state == 0;
    if (f->state && !has_f) return 0;
    uint32_t vc = v->ctl[3] && v->ctl[4] ? v->ctl[3] : 0, fc = has_f && f->ctl[3] && f->ctl[4] ? f->ctl[3] : 0;
    size_t need = (size_t)v->code_len + vc + (has_f ? (size_t)f->code_len + fc : 0);
    ShJob *j = calloc(1, sizeof *j + need);
    uint8_t *d = j->data;
    j->stage = 3; j->code_len = v->code_len; j->cdata_size = vc; j->key[0] = has_f ? (uint32_t)f->code_len : 0; j->key[1] = fc;
    memcpy(d, v->code, (size_t)v->code_len); d += v->code_len;
    if (vc) { memcpy(d, v->code - 0x30 + v->ctl[4], vc); d += vc; }
    if (has_f) { memcpy(d, f->code, (size_t)f->code_len); d += f->code_len; if (fc) memcpy(d, f->code - 0x30 + f->ctl[4], fc); }
    uint64_t h = sh_hash(0x5041495257ULL ^ (uint64_t)j->code_len ^ ((uint64_t)j->key[0] << 32), j->data, need);
    if (!h) h = 1;
    memcpy(&j->key[2], &h, 8);
    int seen = 0;
    EnterCriticalSection(&g_shw_lock);
    unsigned k = (unsigned)(h >> 20) & 16383;
    for (int probe = 0; probe < 64; probe++, k = (k + 1) & 16383) { if (g_pw_qseen[k] == h) { seen = 1; break; } if (!g_pw_qseen[k]) { g_pw_qseen[k] = h; break; } }
    LeaveCriticalSection(&g_shw_lock);
    if (seen) free(j); else { InterlockedIncrement64(&g_pw_progs); pw_enqueue(j); }
    static int nowait = -1;
    if (nowait < 0) nowait = getenv("SSBU_SHADER_NOWAIT") != NULL;
    if (!nowait && g_st.frames >= 300 && g_shw_queued != g_shw_done) {
        LONG64 t0 = pf_now(), freq = 0;
        { LARGE_INTEGER q; QueryPerformanceFrequency(&q); freq = q.QuadPart; }
        while (g_shw_queued != g_shw_done && pf_now() - t0 < 30 * freq) Sleep(1);
        InterlockedExchangeAdd64(&g_shw_held_ticks, pf_now() - t0); InterlockedIncrement64(&g_shw_held);
    }
    return 1;
}
/* nvnMemoryPoolInitialize (nvn_stub.c), guest thread. The game lays a pool over the code section of each "BNSH" shader
 * container when it sets the container up (loading), long before it makes the first program of it: the Vulkan back end
 * scans the container then (nvn_vk_m4.c). */
void nvn_front_pool_made(uint64_t memory, uint64_t size) { if (g_rb == &rb_vk && memory && size >= 0x40 && (memory >> 28) != 0x710 && rd32(memory) == 0x12345678u) nvn_vk_container_seen(memory, memory); }      /* such a pool begins with the code section of a stage (its magic); a search below every other pool cost 0.2 s each (measured) */
/* nvnProgramSetShaders(program, count, const NVNshaderData *): { u64 data (GPU address), const void *control }.
 * control: magic 0x98761234, offset of the NV_gpu_program5 text at +0x14, its length at +0x18. */
__declspec(thread) long long nvn_setsh_part[8];
static void prog_set_shaders_(Ctx *c);
static void prog_set_shaders(Ctx *c) {             /* session 8: the call timed part by part; a line for every call over 2 ms (SSBU_VK_SETSH_LOG=<ms> changes the limit, 0 = no lines) */
    static int lim = -1; static LONG nl; static LONG64 fq;
    if (lim < 0) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); fq = f.QuadPart; lim = getenv("SSBU_VK_SETSH_LOG") ? atoi(getenv("SSBU_VK_SETSH_LOG")) : 2; }
    if (!lim || g_rb != &rb_vk) { prog_set_shaders_(c); return; }
    memset(nvn_setsh_part, 0, sizeof nvn_setsh_part);
    uint64_t guest = A0, ra = c->x[30];
    LONG64 t0 = pf_now();
    prog_set_shaders_(c);
    LONG64 t = pf_now() - t0;
    if (t * 1000 > (LONG64)lim * fq && (InterlockedIncrement(&nl) <= 400 || (t * 1000 > 20 * fq && nl <= 4000))) {      /* session 10: a call over 20 ms is logged after the first 400 lines too (Intel: the lines were used up at loading) */
        long long *q = nvn_setsh_part;
#define SETSH_MS(x) ((double)(x) * 1000.0 / (double)fq)
        printf("nvn_setsh: frame %lld, program object %llx (called from %llx): %.1f ms | barrier %.1f | container %.1f (search %.1f, copy or walk %.1f) | program_set %.1f (hash + job copy %.1f, lock %.1f, HELD %.1f) | rest %.1f\n",
               nvn_front_frame_next(), (unsigned long long)guest, (unsigned long long)ra, SETSH_MS(t), SETSH_MS(q[0]), SETSH_MS(q[1]), SETSH_MS(q[2]), SETSH_MS(q[3]), SETSH_MS(q[4]), SETSH_MS(q[5]), SETSH_MS(q[6]), SETSH_MS(q[7]), SETSH_MS(t - q[0] - q[1] - q[4]));
        fflush(stdout);
    }
}
static void prog_set_shaders_(Ctx *c) {
    static int nlog, nlog2;
    { LONG64 tb = pf_now(); gl_barrier(); nvn_setsh_part[0] += pf_now() - tb; }
    Prog *p = hobj(A0, K_PROG);
    int count = (int)A1;
    uint64_t stride = strstr(hostfn_current_name(), "Ext") ? 24 : 16;      /* the Ext variant has a third pointer per shader */
    InterlockedIncrement(&g_setshaders_calls);
    if (!p) { p = hnew(A0, K_PROG, sizeof(Prog)); p->serial = (int)InterlockedIncrement(&g_prog_serial); p->guest = A0; }
    for (int s = 0; s < 6; s++) if (p->st[s].state == 1 || p->st[s].state == 4) { free(p->st[s].text); p->st[s].text = NULL; p->st[s].state = 0; }
    for (int i = 0; i < count && i < 6 && A2; i++) {
        uint64_t control = rd64(A2 + (uint64_t)i * stride + 8);
        if (!control || rd32(control) != 0x98761234u) { logf_limited(&nlog, 10, "nvn_gl: %s: shader %d of program #%d has no recognisable control section\n", hostfn_current_name(), i, p->serial); continue; }
        uint64_t data = rd64(A2 + (uint64_t)i * stride);
        dump_stage_all(p->serial, i, control, data);
        /* V1 session 6: a program made by FUN_71000382a0 (13.0.4; the caller's return address lies in it) comes from a variation
         * of a "BNSH" container; that function keeps its third argument, the variation's program inside the file, in x20 */
        if (g_rb == &rb_vk && i == 0 && c->x[30] - GA(0x71000382a0ull) < 2884) { LONG64 tb = pf_now(); nvn_vk_container_seen(control, c->x[20]); nvn_setsh_part[1] += pf_now() - tb; }
        {   static int diag = -1; static LONG nd;      /* SSBU_SHADER_SRC_LOG=<n>: where the first n heap-held stages come from (guest return addresses, the memory around the control section) */
            if (diag < 0) diag = getenv("SSBU_SHADER_SRC_LOG") ? atoi(getenv("SSBU_SHADER_SRC_LOG")) : 0;
            if (diag && (control >> 28) != 0x710 && InterlockedIncrement(&nd) <= diag) {
                uint64_t fp = c->x[29], ra1 = fp ? rd64(fp + 8) : 0, fp2 = fp ? rd64(fp) : 0, ra2 = fp2 ? rd64(fp2 + 8) : 0, fp3 = fp2 ? rd64(fp2) : 0, ra3 = fp3 ? rd64(fp3 + 8) : 0, pg = control & ~0xffffull;
                printf("shader-src: frame %lld program #%d stage %d of %d: control %llx data %llx (third pointer %llx) | called from %llx <- %llx <- %llx <- %llx | 64K base: %08x %08x %08x %08x %08x %08x %08x %08x | before control: %016llx %016llx %016llx %016llx | data: %08x %08x %08x %08x\n", nvn_front_frame_next(), p->serial, i, count,
                       (unsigned long long)control, (unsigned long long)data, (unsigned long long)(stride == 24 ? rd64(A2 + (uint64_t)i * stride + 16) : 0), (unsigned long long)c->x[30], (unsigned long long)ra1, (unsigned long long)ra2, (unsigned long long)ra3,
                       rd32(pg), rd32(pg + 4), rd32(pg + 8), rd32(pg + 12), rd32(pg + 16), rd32(pg + 20), rd32(pg + 24), rd32(pg + 28),
                       (unsigned long long)rd64(control - 32), (unsigned long long)rd64(control - 24), (unsigned long long)rd64(control - 16), (unsigned long long)rd64(control - 8), data ? rd32(data) : 0, data ? rd32(data + 4) : 0, data ? rd32(data + 8) : 0, data ? rd32(data + 12) : 0);
                fflush(stdout);
            }
        }
        static const int8_t stage_of_type[16] = {-1, 0, 3, 4, 2, 1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
        if (rd32(control + 0x18)) {
            InterlockedIncrement(&g_with_text);
            prog_add_stage(p, P(control + rd32(control + 0x14)), (int)rd32(control + 0x18), p->serial);
            if (data && rd32(data) == 0x12345678u) {                  /* the machine code is there as well */
                int s2 = stage_of_type[(rd32(data + 0x30) >> 10) & 15], len2 = (int)rd32(control + 0x6f8);
                if (s2 >= 0 && s2 < 2 && p->st[s2].state == 1 && len2 > 0 && len2 < (4 << 20)) { p->st[s2].code = P(data + 0x30); p->st[s2].code_len = len2; p->st[s2].glsl_state = 0; memcpy(p->st[s2].ctl, P(control + 0x6f0), 32); }
            }
            /* V1 M3: a compute program with assembly text (the five built into the exe) also carries its machine code; the
             * Vulkan back end runs that (no NV assembly there), OpenGL only with SSBU_COMPUTE_MC=1 (translator check). */
            if (rd32(control + 0x714) == 5 && data && p->st[5].state == 1) {
                static int ncl;
                int len = (int)rd32(control + 0x6f8);
                logf_limited(&ncl, 8, "nvn_gl: program #%d: compute stage with assembly text and %d bytes of machine code: %08x %08x %08x %08x, work group %u x %u x %u\n", p->serial, len,
                             rd32(data), rd32(data + 4), rd32(data + 8), rd32(data + 12), rd32(control + 0x748), rd32(control + 0x74c), rd32(control + 0x750));
                for (int k = 0; k < 3; k++) p->st[5].local[k] = rd32(control + 0x748 + (uint64_t)k * 4);
                p->st[5].smem_bytes = rd32(control + 0x754); p->st[5].lmem_bytes = rd32(control + 0x758);
                if (len > 0 && len < (4 << 20)) { p->st[5].code = P(data); p->st[5].code_len = len; p->st[5].glsl_state = 0; memcpy(p->st[5].ctl, P(control + 0x6f0), 32); }
            }
            continue;
        }
        /* No assembly text: only Maxwell machine code. data: magic 0x12345678, program header (0x50 bytes) at +0x30, then 64-bit
         * instruction words; control +0x6f8 = size of the instructions in bytes. Header word 0 bits 10..13 = stage (1 vertex,
         * 2 tess control, 3 tess evaluation, 4 geometry, 5 fragment). */
        InterlockedIncrement(&g_without_text);
        if (rd32(control + 0x714) == 5 && data) {                     /* compute stage (control +0x714 = NVN shader stage) */
            static int ncl;
            int len = (int)rd32(control + 0x6f8);
            logf_limited(&ncl, 40, "nvn_gl: program #%d: compute stage as machine code, %d bytes, data %llx: %08x %08x %08x %08x | +0x30: %08x %08x %08x %08x\n", p->serial, len, (unsigned long long)data,
                         rd32(data), rd32(data + 4), rd32(data + 8), rd32(data + 12), rd32(data + 0x30), rd32(data + 0x34), rd32(data + 0x38), rd32(data + 0x3c));
            for (int k = 0; k < 3; k++) p->st[5].local[k] = rd32(control + 0x748 + (uint64_t)k * 4);      /* work group size */
            p->st[5].smem_bytes = rd32(control + 0x754); p->st[5].lmem_bytes = rd32(control + 0x758);
            if (len > 0 && len < (4 << 20)) { p->st[5].code = P(data); p->st[5].code_len = len; p->st[5].state = 4; p->st[5].dumped = 0; p->st[5].glsl_state = 0; memcpy(p->st[5].ctl, P(control + 0x6f0), 32); }
            continue;
        }
        if (!data || rd32(data) != 0x12345678u) { logf_limited(&nlog2, 10, "nvn_gl: program #%d shader %d: no text and no recognisable code section\n", p->serial, i); continue; }
        int s = stage_of_type[(rd32(data + 0x30) >> 10) & 15], len = (int)rd32(control + 0x6f8);
        if (s < 0 || len <= 0 || len > (4 << 20)) { logf_limited(&nlog2, 10, "nvn_gl: program #%d shader %d: unexpected program header %08x, code size %d\n", p->serial, i, rd32(data + 0x30), len); continue; }
        p->st[s].code = P(data + 0x30); p->st[s].code_len = len; p->st[s].state = 4; p->st[s].dumped = 0; p->st[s].glsl_state = 0; memcpy(p->st[s].ctl, P(control + 0x6f0), 32);
    }
    if (g_rb == &rb_vk) { LONG64 tb = pf_now(); nvn_vk_program_set(p); nvn_setsh_part[4] += pf_now() - tb; RET(1); return; }       /* V1 M4: the stages go to the Vulkan workers at once (nvn_vk_m4.c) */
    if (!pw_queue_prog(p))
    for (int s = 0; s < 2; s++) {              /* P3: have the driver's binary ready before the first draw */
        Stage *st = &p->st[s];
        if (st->code && st->code_len > 0 && st->glsl_state == 0)
            shd_prewarm(s, st->code, st->code_len, st->ctl[3] && st->ctl[4] ? st->code - 0x30 + st->ctl[4] : NULL, st->ctl[3]);
    }
    RET(1);
}

/* ---- samplers */
static void smpb_defaults(Ctx *c) {
    SamplerParams p;
    memset(&p, 0, sizeof p);
    p.min = 1; p.mag = 1; p.ws = p.wt = p.wr = 7; p.lodmax = 1000.0f; p.aniso = 1.0f; p.cfunc = 2;
    memcpy(P(A0), &p, sizeof p);
}
#define SMP ((SamplerParams *)P(A0))
static void smpb_minmag(Ctx *c) { SMP->min = (uint8_t)A1; SMP->mag = (uint8_t)A2; }
static void smpb_wrap(Ctx *c) { SMP->ws = (uint8_t)A1; SMP->wt = (uint8_t)A2; SMP->wr = (uint8_t)A3; }
static void smpb_lodclamp(Ctx *c) { SMP->lodmin = F0; SMP->lodmax = F1; }
static void smpb_lodbias(Ctx *c) { SMP->bias = F0; }
static void smpb_compare(Ctx *c) { SMP->cmode = (uint8_t)A1; SMP->cfunc = (uint8_t)A2; }
static void smpb_border(Ctx *c) { if (A1) memcpy(SMP->border, P(A1), 16); }
static void smpb_aniso(Ctx *c) { SMP->aniso = F0; }
static void smpb_reduction(Ctx *c) { SMP->reduction = (uint8_t)A1; }
static void smp_initialize(Ctx *c) { memcpy(P(A0), P(A1), sizeof(SamplerParams)); RET(1); }
static void spool_initialize(Ctx *c) {                 /* (pool, memoryPool, offset, numDescriptors) */
    SamplerPool *sp = hnew(A0, K_SPOOL, sizeof *sp);
    sp->count = (int)A3 > 0 ? (int)A3 : 0; sp->mempool = A1; sp->offset = A2;
    sp->e = calloc((size_t)sp->count + 1, sizeof *sp->e);
    sp->used = calloc((size_t)sp->count + 1, 1);
    RET(1);
}
static void spool_register(Ctx *c) {                   /* (pool, id, sampler) or (pool, id, samplerBuilder): same contents */
    gl_barrier();
    SamplerPool *sp = hobj(A0, K_SPOOL);
    if (!sp || (uint32_t)A1 >= (uint32_t)sp->count || !A2) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    memcpy(&sp->e[(uint32_t)A1], P(A2), sizeof(SamplerParams));
    sp->used[(uint32_t)A1] = 1;
    ReleaseSRWLockExclusive(&g_pool_lock);
}

/* ---- textures */
#define TB ((TexBuilder *)P(A0))
static void texb_defaults(Ctx *c) {
    TexBuilder b;
    memset(&b, 0, sizeof b);
    b.target = 1; b.levels = 1;                        /* [sdk 0x427610]: target 1, size 0 x 0 x 0, 1 level, format 0, 0 samples, swizzle 2 3 4 5 */
    b.swz[0] = 2; b.swz[1] = 3; b.swz[2] = 4; b.swz[3] = 5;
    memcpy(P(A0), &b, sizeof b);
}
static void texb_flags(Ctx *c) { TB->flags = (int)A1; }
static void texb_target(Ctx *c) { TB->target = (int)A1; }
static void texb_format(Ctx *c) { TB->format = (int)A1; }
static void texb_width(Ctx *c) { TB->width = (int)A1; }
static void texb_height(Ctx *c) { TB->height = (int)A1; }
static void texb_depth(Ctx *c) { TB->depth = (int)A1; }
static void texb_size1d(Ctx *c) { TB->width = (int)A1; TB->height = 1; TB->depth = 1; }     /* [sdk 0x4276a0] */
static void texb_size2d(Ctx *c) { TB->width = (int)A1; TB->height = (int)A2; TB->depth = 1; }     /* [sdk 0x4276b0] */
static void texb_size3d(Ctx *c) { TB->width = (int)A1; TB->height = (int)A2; TB->depth = (int)A3; }
static void texb_levels(Ctx *c) { TB->levels = (int)A1; }
static void texb_samples(Ctx *c) { TB->samples = (int)A1; }
static void texb_swizzle(Ctx *c) { TB->swz[0] = (uint8_t)A1; TB->swz[1] = (uint8_t)A2; TB->swz[2] = (uint8_t)A3; TB->swz[3] = (uint8_t)A4; }
static void texb_dsmode(Ctx *c) { TB->dsmode = (int)A1; }
static void texb_storage(Ctx *c) { TB->pool = A1; TB->offset = A2; }
static void texb_packaged(Ctx *c) { TB->packaged = A1; }
static void texb_stride(Ctx *c) { TB->stride = (int64_t)A1; }
static void texb_layout(Ctx *c) {
    static int nlog;
    if (!A1) return;
    memcpy(TB->layout, P(A1), 8);
    TB->has_layout = 1;
    Fmt f;
    int hb = fmt_get(TB->format, &f) ? (TB->height + f.bh - 1) / f.bh : TB->height;
    logf_limited(&nlog, 40, "nvn_gl: packaged texture layout %02x %02x %02x %02x %02x %02x %02x %02x for %dx%dx%d format 0x%x target %d levels %d (block height by rule: %d)\n", TB->layout[0], TB->layout[1],
                 TB->layout[2], TB->layout[3], TB->layout[4], TB->layout[5], TB->layout[6], TB->layout[7], TB->width, TB->height, TB->depth, TB->format, TB->target, TB->levels, 1 << tex_rule_block_height_log2(hb));
}
/* Storage size and alignment are the SDK's own code (nvn_sdk.c: [sdk 0x428760] and [sdk 0x4278b0] lifted, with the GPU
 * memory-kind choice they end in). The size this file's own layout needs (tex_storage_size) is only compared with it. */
static void texb_sdk(const TexBuilder *b, NvnSdkTexBuilder *r) {
    memset(r, 0, sizeof *r);
    r->flags = (uint32_t)b->flags; r->target = (uint32_t)b->target; r->width = (uint32_t)b->width; r->height = (uint32_t)b->height; r->depth = (uint32_t)b->depth;
    r->levels = (uint32_t)b->levels; r->format = (uint32_t)b->format; r->samples = (uint32_t)b->samples; r->stride = (uint32_t)b->stride; r->glname = b->glname;
    r->has_layout = (uint32_t)b->has_layout; memcpy(&r->layout, b->layout, 4);
}
static void texb_storage_size(Ctx *c) {
    static int nlog;
    TexBuilder b = *TB;
    NvnSdkTexBuilder r;
    texb_sdk(&b, &r);
    uint64_t real = nvn_sdk_texture_storage_size(c, &r);
    size_t own = nvn_sdk_format_known(r.format) && !b.glname ? tex_storage_size(&b) : 0;
    if (own > real) logf_limited(&nlog, 40, "nvn_gl: texture layout of this file needs 0x%llx bytes, the SDK's storage size is 0x%llx (%dx%dx%d format 0x%x target %d levels %d samples %d flags 0x%x)\n",
                                 (unsigned long long)own, (unsigned long long)real, b.width, b.height, b.depth, b.format, b.target, b.levels, b.samples, b.flags);
    RET(real);
}
static void texb_storage_align(Ctx *c) {
    NvnSdkTexBuilder r;
    texb_sdk(TB, &r);
    RET(nvn_sdk_texture_storage_alignment(c, &r));
}
static void texb_get_flags(Ctx *c) { RET(TB->flags); }
static void texb_get_target(Ctx *c) { RET(TB->target); }
static void texb_get_format(Ctx *c) { RET(TB->format); }
static void texb_get_width(Ctx *c) { RET(TB->width); }
static void texb_get_height(Ctx *c) { RET(TB->height); }
static void texb_get_depth(Ctx *c) { RET(TB->depth); }
static void texb_get_levels(Ctx *c) { RET(TB->levels); }
static void texb_get_samples(Ctx *c) { RET(TB->samples); }

static void tex_initialize(Ctx *c) {                   /* (texture, builder) */
    Tex *t = hnew(A0, K_TEX, sizeof *t);
    uint64_t mem = 0, size = 0;
    memcpy(&t->b, P(A1), sizeof t->b);
    if (t->b.pool && nvn_pool_range(t->b.pool, &mem, &size) && mem) { t->mem = (uint8_t *)P(mem) + t->b.offset; t->limit = (uint8_t *)P(mem) + size; }
    t->storage = tex_storage_size2(&t->b, &t->data_size);
    if (!tex_capture_packaged(t)) {                /* PR #41, changed: no copy = the old behaviour (the game's own storage is read) */
        static int said;
        if (said++ < 5) printf("nvn: packaged texture %dx%d format 0x%x: its source %llx + %llu could not be copied: read in place as before\n",
                               t->b.width, t->b.height, t->b.format, (unsigned long long)t->b.packaged, (unsigned long long)t->data_size);
    }
    t->guest = A0;
    tex_range_add(t);
    do { t->all_next = g_all_tex; } while (InterlockedCompareExchangePointer((void *volatile *)&g_all_tex, t, t->all_next) != t->all_next);
    RET(1);
}
static void tex_finalize(Ctx *c) {
    gl_barrier();
    Tex *t = hobj(A0, K_TEX);
    if (!t) return;
    if (!t->dead) tex_range_remove(t);
    InterlockedExchange(&t->dead, 1);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
    defer_free(K_TEX, t);
}
#define TEXGET(name, expr) static void name(Ctx *c) { Tex *t = hobj(A0, K_TEX); RET(t ? (expr) : 0); }
TEXGET(tex_get_flags, t->b.flags)
TEXGET(tex_get_target, t->b.target)
TEXGET(tex_get_format, t->b.format)
TEXGET(tex_get_width, t->b.width)
TEXGET(tex_get_height, t->b.height)
TEXGET(tex_get_depth, t->b.depth)
TEXGET(tex_get_levels, t->b.levels)
TEXGET(tex_get_samples, t->b.samples)
TEXGET(tex_get_stride, t->b.stride)
static void tex_get_storage_size(Ctx *c);       /* below: the SDK's value [sdk 0x429720], not this file's own layout size (t->storage) */
TEXGET(tex_get_memory_offset, t->b.offset)
TEXGET(tex_get_address, (uint64_t)(uintptr_t)t->mem)
TEXGET(tex_get_dsmode, t->b.dsmode)
/* Storage class (raw, and 0x100 | GPU memory kind) and sparse tile layout: the SDK's own code, lifted (nvn_sdk.c: [sdk 0x427a10],
 * [0x427b30], [0x428c00], [0x429870], [0x4290b0], [0x429150]). NVNtextureSparseTileLayout = the 16 bytes [sdk 0x411090] writes.
 * The texture forms run on the NVNtexture that nvnTextureInitialize [sdk 0x4287d0] would have built from this texture's builder. */
static void texb_storage_class(Ctx *c) { NvnSdkTexBuilder r; texb_sdk(TB, &r); RET(nvn_sdk_texture_storage_class(c, &r)); }
static void texb_raw_storage_class(Ctx *c) { NvnSdkTexBuilder r; texb_sdk(TB, &r); RET(nvn_sdk_texture_raw_storage_class(c, &r)); }
static void texb_sparse_layout(Ctx *c) { uint64_t out = A1; uint32_t v[4]; NvnSdkTexBuilder r; texb_sdk(TB, &r); nvn_sdk_texture_sparse_layout(c, &r, v); if (out) memcpy(P(out), v, 16); }
static int tex_sdk(Ctx *c, NvnSdkTexBuilder *r, const char *fn) {
    Tex *t = hobj(A0, K_TEX);
    if (t && !t->b.glname) { texb_sdk(&t->b, r); return 1; }
    printf("nvn: stopping: %s on %s: the SDK object cannot be modelled\n", fn, t ? "a texture made from an OpenGL texture name" : "a texture that was never initialized");
    fflush(stdout);
    report_and_exit(6);
    return 0;
}
static void tex_storage_class(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (tex_sdk(c, &r, "nvnTextureGetStorageClass")) nvn_sdk_tex_storage_class(c, &r, &v); RET(v); }
static void tex_raw_storage_class(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (tex_sdk(c, &r, "nvnTextureGetRawStorageClass")) nvn_sdk_tex_raw_storage_class(c, &r, &v); RET(v); }
/* nvnTextureGetStorageSize [sdk 0x429720] = the 32-bit word +0x78 of the NVNtexture = the SDK layout's size (nvn_sdk.c). Until close5
 * this answered t->storage, the size of this file's own layout, which is not rounded to the GPU page as the SDK's is. */
static void tex_get_storage_size(Ctx *c) { NvnSdkTexBuilder r; uint32_t v = 0; if (tex_sdk(c, &r, "nvnTextureGetStorageSize")) nvn_sdk_tex_storage_size(c, &r, &v); RET(v); }
static void tex_sparse_layout(Ctx *c) { uint64_t out = A1; uint32_t v[4] = {0, 0, 0, 0}; NvnSdkTexBuilder r; if (tex_sdk(c, &r, "nvnTextureGetSparseTileLayout")) nvn_sdk_tex_sparse_layout(c, &r, v); if (out) memcpy(P(out), v, 16); }

static void tex_write_texels(Ctx *c) {                 /* (texture, view, region, data) [, rowStride, imageStride] */
    Tex *t = hobj(A0, K_TEX);
    WriteJob j;
    if (!t || !A2 || !A3) return;
    memset(&j, 0, sizeof j);
    j.tex = t; j.data = P(A3);
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    memcpy(j.region, P(A2), 24);
    if (strstr(hostfn_current_name(), "Strided")) j.row_stride = (int64_t)A4;
    gl_run(g_rb->write_texels, &j);
}
static void tex_flush_texels(Ctx *c) { Tex *t = hobj(A0, K_TEX); if (t) t->dirty = 1; }

#define TV ((View *)P(A0))
static void view_defaults(Ctx *c) { memset(P(A0), 0, sizeof(View)); TV->set = 0x80; }
static void view_levels(Ctx *c) { TV->lvl0 = (int16_t)A1; TV->nlvl = (int16_t)A2; TV->set |= 0x81; }
static void view_layers(Ctx *c) { TV->lay0 = (int16_t)A1; TV->nlay = (int16_t)A2; TV->set |= 0x82; }
static void view_format(Ctx *c) { TV->format = (uint16_t)A1; TV->set |= 0x84; }
static void view_swizzle(Ctx *c) { TV->swz[0] = (uint8_t)A1; TV->swz[1] = (uint8_t)A2; TV->swz[2] = (uint8_t)A3; TV->swz[3] = (uint8_t)A4; TV->set |= 0x88; }
static void view_dsmode(Ctx *c) { TV->dsmode = (uint8_t)A1; TV->set |= 0x90; }
static void view_target(Ctx *c) { TV->target = (uint8_t)A1; TV->set |= 0xa0; }

static void tpool_initialize(Ctx *c) {                 /* (pool, memoryPool, offset, numDescriptors) */
    TexPool *tp = hnew(A0, K_TPOOL, sizeof *tp);
    tp->count = (int)A3 > 0 ? (int)A3 : 0; tp->mempool = A1; tp->offset = A2;
    tp->e = calloc((size_t)tp->count + 1, sizeof *tp->e);
    RET(1);
}
static void tpool_register(Ctx *c) {                   /* (pool, id, texture, view) */
    gl_barrier();
    TexPool *tp = hobj(A0, K_TPOOL);
    TexPoolEnt e;
    if (!tp || (uint32_t)A1 >= (uint32_t)tp->count) return;
    memset(&e, 0, sizeof e);
    e.tex = hobj(A2, K_TEX);
    if (A3) memcpy(&e.view, P(A3), sizeof e.view);
    AcquireSRWLockExclusive(&g_pool_lock);
    tp->e[(uint32_t)A1] = e;
    ReleaseSRWLockExclusive(&g_pool_lock);
}

/* ---- small state objects: contents live in the guest object */
static void blend_func_set(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); wr8(A0 + 2, (uint8_t)A2); wr8(A0 + 3, (uint8_t)A3); wr8(A0 + 4, (uint8_t)A4); }
static void blend_equation(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); wr8(A0 + 6, (uint8_t)A2); }
static void color_defaults(Ctx *c) { static const uint8_t d[4] = {0, 3, 8, 0}; memcpy(P(A0), d, 4); }
static void color_blend_enable(Ctx *c) { uint8_t v = rd8(A0), bit = (uint8_t)(1u << (A1 & 7)); wr8(A0, (uint8_t)((A2 & 1) ? v | bit : v & ~bit)); }
static void color_logic_op(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); }
static void color_alpha_test(Ctx *c) { wr8(A0 + 2, (uint8_t)A1); }
static void chanmask_defaults(Ctx *c) { wr32(A0, 0xffffffffu); }
static void chanmask_set(Ctx *c) {                     /* (state, index, r, g, b, a) */
    uint32_t v = rd32(A0), sh = ((uint32_t)A1 & 7) * 4;
    v = (v & ~(15u << sh)) | ((uint32_t)((A2 & 1) | (A3 & 1) << 1 | (A4 & 1) << 2 | (A5 & 1) << 3) << sh);
    wr32(A0, v);
}
static void poly_defaults(Ctx *c) { static const uint8_t d[4] = {0, 1, 2, 0}; memcpy(P(A0), d, 4); }
static void poly_cull(Ctx *c) { wr8(A0, (uint8_t)A1); }
static void poly_front(Ctx *c) { wr8(A0 + 1, (uint8_t)A1); }
static void poly_mode(Ctx *c) { wr8(A0 + 2, (uint8_t)A1); }
static void poly_offset(Ctx *c) { wr8(A0 + 3, (uint8_t)A1); }
static void poly_get_cull(Ctx *c) { RET(rd8(A0)); }
static void poly_get_front(Ctx *c) { RET(rd8(A0 + 1)); }
static void poly_get_mode(Ctx *c) { RET(rd8(A0 + 2)); }
static void poly_get_offset(Ctx *c) { RET(rd8(A0 + 3)); }
/* depth-stencil: bit0 depth test, bit1 depth write, bit2 stencil test, 4..7 depth func, 8..11 / 12..15 stencil func front / back,
 * 16..27 stencil ops front (stencil fail, depth fail, depth pass), 28..39 the same for back */
static void ds_defaults(Ctx *c) { wr64(A0, 0x1111118822ULL); }
static void ds_bits(uint64_t a, int shift, uint64_t mask, uint64_t v) { wr64(a, (rd64(a) & ~(mask << shift)) | ((v & mask) << shift)); }
static void ds_depth_test(Ctx *c) { ds_bits(A0, 0, 1, A1); }
static void ds_depth_write(Ctx *c) { ds_bits(A0, 1, 1, A1); }
static void ds_stencil_test(Ctx *c) { ds_bits(A0, 2, 1, A1); }
static void ds_depth_func(Ctx *c) { ds_bits(A0, 4, 15, A1); }
static void ds_stencil_func(Ctx *c) { if (A1 & 1) ds_bits(A0, 8, 15, A2); if (A1 & 2) ds_bits(A0, 12, 15, A2); }
static void ds_stencil_op(Ctx *c) {                    /* (state, faces, stencilFail, depthFail, depthPass) */
    uint64_t v = (A2 & 15) | (A3 & 15) << 4 | (A4 & 15) << 8;
    if (A1 & 1) ds_bits(A0, 16, 0xfff, v);
    if (A1 & 2) ds_bits(A0, 28, 0xfff, v);
}
static void ds_get_depth_test(Ctx *c) { RET(rd64(A0) & 1); }
static void ds_get_depth_write(Ctx *c) { RET((rd64(A0) >> 1) & 1); }
static void ds_get_stencil_test(Ctx *c) { RET((rd64(A0) >> 2) & 1); }
static void ds_get_depth_func(Ctx *c) { RET((rd64(A0) >> 4) & 15); }
/* vertex attribute: bits 0..7 format (0 = disabled), 8..12 stream, 14..31 offset */
static void vattr_defaults(Ctx *c) { wr32(A0, 0); }
static void vattr_format(Ctx *c) { wr32(A0, (rd32(A0) & 0x1f00u) | ((uint32_t)A1 & 0xff) | ((uint32_t)A2 << 14)); }
static void vattr_stream(Ctx *c) { wr32(A0, (rd32(A0) & ~0x1f00u) | (((uint32_t)A1 & 31) << 8)); }
static void vstream_defaults(Ctx *c) { wr64(A0, 0); }
static void vstream_stride(Ctx *c) { wr32(A0, (uint32_t)A1); }
static void vstream_divisor(Ctx *c) { wr32(A0 + 4, (uint32_t)A1); }

/* ---- command buffers: recording */
static void cmd_initialize(Ctx *c) { cb_new(A0); RET(1); }
static void cmd_finalize(Ctx *c) { CmdBuf *cb = hobj(A0, K_CMD); if (cb) { cb_forget(cb); free(cb->data); free(cb); wr32(A0 + 8, 0); } }
static void cmd_add_command_memory(Ctx *c) { CmdBuf *cb = cb_get(A0); cb->cmd_size = A3; cb->cmd_used = 0; }       /* (cmdbuf, pool, offset, size): a new block replaces the current one */
static void cmd_add_control_memory(Ctx *c) { CmdBuf *cb = cb_get(A0); cb->ctrl_size = A2; cb->ctrl_used = 0; }     /* (cmdbuf, memory, size) */
static void cmd_get_command_size(Ctx *c) { RET(cb_get(A0)->cmd_size); }
static void cmd_get_control_size(Ctx *c) { RET(cb_get(A0)->ctrl_size); }
static void cmd_is_recording(Ctx *c) { RET(cb_get(A0)->recording); }
/* L1 round 2, TEST ONLY (SSBU_L1_TRACE=1 with SSBU_L1=1): for the 5 presents after the first press, every command-buffer
 * recording, submission, fence, sync wait, window acquire and present, with the guest thread name and the guest return address. */
#ifdef _WIN32
int display_l1_trace(void); double display_l1_ms(void); const char *guest_thread_name_of(unsigned long tid);
#else
int display_l1_trace(void); double display_l1_ms(void); const char *guest_thread_name_of(DWORD tid);
#endif
#define L1T(...) do { if (display_l1_trace()) { char l1b_[240]; snprintf(l1b_, sizeof l1b_, __VA_ARGS__); printf("l1-trace: %+8.2f %-26s lr %09llx %s%c", display_l1_ms(), guest_thread_name_of(GetCurrentThreadId()) ? guest_thread_name_of(GetCurrentThreadId()) : "?", (unsigned long long)c->x[30], l1b_, 10); } } while (0)
static void cmd_begin(Ctx *c) { L1T("begin recording cb %llx", (unsigned long long)A0); CmdBuf *cb = cb_get(A0); cb->len = 0; cb->recording = 1; cb->rec_cmd = cb->rec_ctrl = 0; cb->dispatches = 0; }
static void cmd_end(Ctx *c) {
    CmdBuf *cb = cb_get(A0);
    cb->ctrl_used += 32; cb->rec_ctrl += 32;
    uint64_t h = list_register(cb->data, cb->len, (uint32_t)cb->rec_cmd, (uint32_t)cb->rec_ctrl, (uint32_t)cb->dispatches);
    L1T("end recording cb %llx -> list %llx (%u bytes, %u dispatches)", (unsigned long long)A0, (unsigned long long)h, (unsigned)cb->len, (unsigned)cb->dispatches);
    cb->data = NULL; cb->len = cb->cap = 0; cb->recording = 0;
    RET(h);
}
static void cmd_call(Ctx *c) { uint64_t src = A2; int n = (int)A1; Cmd *m = rec(c, OP_CALL, (size_t)n * 8); memcpy(BLOB(m), P(src), (size_t)n * 8); }
static void cmd_texpool(Ctx *c) { rec(c, OP_TEXPOOL, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_TPOOL); }
static void cmd_smppool(Ctx *c) { rec(c, OP_SMPPOOL, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_SPOOL); }
static void cmd_program(Ctx *c) { rec(c, OP_PROGRAM, 0)->a[0] = (uint64_t)(uintptr_t)hobj(A1, K_PROG); }
static void rec_ranges(Ctx *c, uint32_t op, int single) {      /* (cmdbuf, stage, index, address, size) or (cmdbuf, stage, first, count, ranges) */
    int n = single ? 1 : (int)A3;
    uint64_t src = A4, addr = A3, size = A4;
    Cmd *m = rec(c, op, (size_t)n * 16);
    if (single) { m->a[2] = 1; memcpy(BLOB(m), &addr, 8); memcpy(BLOB(m) + 8, &size, 8); }
    else if (src) memcpy(BLOB(m), P(src), (size_t)n * 16);
    else memset(BLOB(m), 0, (size_t)n * 16);
}
static void cmd_ubo(Ctx *c) { rec_ranges(c, OP_UBO, 1); }
static void cmd_ubos(Ctx *c) { rec_ranges(c, OP_UBO, 0); }
static void cmd_ssbo(Ctx *c) { rec_ranges(c, OP_SSBO, 1); }
static void cmd_ssbos(Ctx *c) { rec_ranges(c, OP_SSBO, 0); }
static void cmd_texture(Ctx *c) { uint64_t h = A3; Cmd *m = rec(c, OP_TEXTURES, 8); m->a[2] = 1; memcpy(BLOB(m), &h, 8); }   /* (cmdbuf, stage, index, handle) */
static void cmd_textures(Ctx *c) {                      /* (cmdbuf, stage, first, count, handles) */
    int n = (int)A3;
    uint64_t src = A4;
    Cmd *m = rec(c, OP_TEXTURES, (size_t)n * 8);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 8); else memset(BLOB(m), 0, (size_t)n * 8);
}
static void cmd_vbuf(Ctx *c) { uint64_t addr = A2, size = A3; Cmd *m = rec(c, OP_VBUF, 16); m->a[1] = 1; memcpy(BLOB(m), &addr, 8); memcpy(BLOB(m) + 8, &size, 8); }   /* (cmdbuf, index, address, size) */
static void cmd_vbufs(Ctx *c) {                         /* (cmdbuf, first, count, ranges) */
    int n = (int)A2;
    uint64_t src = A3;
    Cmd *m = rec(c, OP_VBUF, (size_t)n * 16);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 16); else memset(BLOB(m), 0, (size_t)n * 16);
}
static void cmd_vattr(Ctx *c) { int n = (int)A1; uint64_t src = A2; Cmd *m = rec(c, OP_VATTR, (size_t)n * 4); if (n) memcpy(BLOB(m), P(src), (size_t)n * 4); }       /* (cmdbuf, count, states) */
static void cmd_vstream(Ctx *c) { int n = (int)A1; uint64_t src = A2; Cmd *m = rec(c, OP_VSTREAM, (size_t)n * 8); if (n) memcpy(BLOB(m), P(src), (size_t)n * 8); }   /* (cmdbuf, count, states) */
static void rec_state(Ctx *c, uint32_t op, size_t size) { uint64_t src = A1; Cmd *m = rec(c, op, 0); m->a[0] = m->a[1] = m->a[2] = 0; if (src) memcpy(m->a, P(src), size); }
static void cmd_blend(Ctx *c) { rec_state(c, OP_BLEND, 8); }
static void cmd_color(Ctx *c) { rec_state(c, OP_COLOR, 4); }
static void cmd_chanmask(Ctx *c) { rec_state(c, OP_CHANMASK, 4); }
static void cmd_multisample(Ctx *c) { rec_state(c, OP_MULTISAMPLE, 0x18); }
static void cmd_polygon(Ctx *c) { rec_state(c, OP_POLYGON, 4); }
static void cmd_depthstencil(Ctx *c) { rec_state(c, OP_DEPTHSTENCIL, 8); }
static void cmd_viewport(Ctx *c) {                      /* (cmdbuf, x, y, w, h) */
    float v[4] = {(float)(int)A1, (float)(int)A2, (float)(int)A3, (float)(int)A4};
    Cmd *m = rec(c, OP_VIEWPORTS, 16);
    m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 16);
}
static void cmd_viewports(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_VIEWPORTS, (size_t)n * 16); memcpy(BLOB(m), P(src), (size_t)n * 16); }   /* (cmdbuf, first, count, float[4] each) */
static void cmd_scissor(Ctx *c) { int v[4] = {(int)A1, (int)A2, (int)A3, (int)A4}; Cmd *m = rec(c, OP_SCISSORS, 16); m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 16); }
static void cmd_scissors(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_SCISSORS, (size_t)n * 16); memcpy(BLOB(m), P(src), (size_t)n * 16); }
static void cmd_depth_range(Ctx *c) { float v[2] = {F0, F1}; Cmd *m = rec(c, OP_DEPTHRANGES, 8); m->a[0] = 0; m->a[1] = 1; memcpy(BLOB(m), v, 8); }
static void cmd_depth_ranges(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_DEPTHRANGES, (size_t)n * 8); memcpy(BLOB(m), P(src), (size_t)n * 8); }
static void cmd_targets(Ctx *c) {                       /* (cmdbuf, count, textures[], views[], depth, depthView) */
    int n = (int)A1 < 8 ? (int)A1 : 8;
    uint64_t texs = A2, views = A3, depth = A4, dview = A5;
    TargetRef r[9];
    memset(r, 0, sizeof r);
    for (int i = 0; i < n; i++) {
        uint64_t t = texs ? rd64(texs + (uint64_t)i * 8) : 0, v = views ? rd64(views + (uint64_t)i * 8) : 0;
        r[i].tex = hobj(t, K_TEX);
        if (v) memcpy(&r[i].view, P(v), sizeof(View));
    }
    r[8].tex = hobj(depth, K_TEX);
    if (dview) memcpy(&r[8].view, P(dview), sizeof(View));
    Cmd *m = rec(c, OP_TARGETS, sizeof r);
    m->a[0] = (uint64_t)n;
    memcpy(BLOB(m), r, sizeof r);
}
static void cmd_stencil_ref(Ctx *c) { rec(c, OP_STENCIL_REF, 0); }
static void cmd_stencil_vmask(Ctx *c) { rec(c, OP_STENCIL_VMASK, 0); }
static void cmd_stencil_mask(Ctx *c) { rec(c, OP_STENCIL_MASK, 0); }
static void cmd_blend_color(Ctx *c) { uint64_t src = A1; Cmd *m = rec(c, OP_BLENDCOLOR, 16); memcpy(BLOB(m), P(src), 16); }
static void rec_float(Ctx *c, uint32_t op) { float v[3] = {F0, F1, F2}; Cmd *m = rec(c, op, 0); memcpy(m->a, v, 12); }
static void cmd_alpha_ref(Ctx *c) { rec_float(c, OP_ALPHAREF); }
static void cmd_point_size(Ctx *c) { rec_float(c, OP_POINTSIZE); }
static void cmd_line_width(Ctx *c) { rec_float(c, OP_LINEWIDTH); }
static void cmd_poly_offset(Ctx *c) { rec_float(c, OP_POLYOFFSET); }
static void cmd_rast_discard(Ctx *c) { rec(c, OP_RASTDISCARD, 0); }
static void cmd_depth_clamp(Ctx *c) { rec(c, OP_DEPTHCLAMP, 0); }
static void cmd_prim_restart(Ctx *c) { rec(c, OP_PRIMRESTART, 0); }
static void cmd_patch_size(Ctx *c) { rec(c, OP_PATCHSIZE, 0); }
static void cmd_clear_color(Ctx *c) { uint64_t src = A2; Cmd *m = rec(c, OP_CLEARCOLOR, 16); if (src) memcpy(BLOB(m), P(src), 16); else memset(BLOB(m), 0, 16); }   /* (cmdbuf, index, rgba, mask) */
static void cmd_clear_ds(Ctx *c) { float d = F0; Cmd *m = rec(c, OP_CLEARDS, 0); m->a[3] = 0; memcpy(&m->a[3], &d, 4); }   /* (cmdbuf, depth [v0], depthMask, stencilValue, stencilMask) */
static void cmd_draw_arrays(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ARRAYS, 0); m->a[3] = 0; m->a[4] = 1; }              /* (cmdbuf, mode, first, count) */
static void cmd_draw_arrays_inst(Ctx *c) { rec(c, OP_DRAW_ARRAYS, 0); }                                            /* (cmdbuf, mode, first, count, baseInstance, instanceCount) */
static void cmd_draw_elements(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ELEMENTS, 0); m->a[4] = 0; m->a[5] = 0; m->a[6] = 1; }   /* (cmdbuf, mode, type, count, indexBuffer) */
static void cmd_draw_elements_bv(Ctx *c) { Cmd *m = rec(c, OP_DRAW_ELEMENTS, 0); m->a[5] = 0; m->a[6] = 1; }             /* (.., baseVertex) */
static void cmd_draw_elements_inst(Ctx *c) {            /* (cmdbuf, mode, type, count, indexBuffer, baseVertex, baseInstance, instanceCount) */
    static int nlog;
    logf_limited(&nlog, 6, "nvn_gl: DrawElementsInstanced mode %d type %d count %d index %llx x5=%d x6=%d x7=%d\n", (int)A1, (int)A2, (int)A3, (unsigned long long)A4, (int)A5, (int)A6, (int)A7);
    rec(c, OP_DRAW_ELEMENTS, 0);
}
static void cmd_copy_buf_tex(Ctx *c) {                  /* (cmdbuf, buffer, texture, view, region, flags) */
    uint64_t view = A3, region = A4;
    Tex *t = hobj(A2, K_TEX);
    Cmd *m = rec(c, OP_COPY_BUF_TEX, 48);
    CmdBuf *cb = cb_get(A0);
    /* PR #14 (xtlg511): the copy takes the strides this command buffer has now (SetCopyRowStride / SetCopyImageStride), as
     * NVN does: at replay the running stride state may come from another command buffer's copies (a 320x320 upload read with
     * the 832-texel rows of a stage preview read past its buffer: the crash after a random stage choice) */
    m->a[1] = (uint64_t)(uintptr_t)t; m->a[5] = (uint64_t)cb->row_stride; m->a[6] = (uint64_t)cb->image_stride;
    memset(BLOB(m), 0, 48);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24);
}
static void cmd_copy_tex_tex(Ctx *c) {                  /* (cmdbuf, src, srcView, srcRegion, dst, dstView, dstRegion, flags) */
    uint64_t sv = A2, sr = A3, dv = A5, dr = A6;
    Tex *s = hobj(A1, K_TEX), *d = hobj(A4, K_TEX);
    Cmd *m = rec(c, OP_COPY_TEX_TEX, 96);
    m->a[0] = (uint64_t)(uintptr_t)s; m->a[3] = (uint64_t)(uintptr_t)d;
    memset(BLOB(m), 0, 96);
    if (sv) memcpy(BLOB(m), P(sv), sizeof(View));
    if (sr) memcpy(BLOB(m) + 24, P(sr), 24);
    if (dv) memcpy(BLOB(m) + 48, P(dv), sizeof(View));
    if (dr) memcpy(BLOB(m) + 72, P(dr), 24);
}
static void cmd_copy_tex_buf(Ctx *c) {                  /* (cmdbuf, texture, view, region, buffer, flags) */
    uint64_t view = A2, region = A3;
    Tex *t = hobj(A1, K_TEX);
    Cmd *m = rec(c, OP_COPY_TEX_BUF, 48);
    CmdBuf *cb = cb_get(A0);
    m->a[0] = (uint64_t)(uintptr_t)t; m->a[3] = c->x[4]; m->a[5] = (uint64_t)cb->row_stride; m->a[6] = (uint64_t)cb->image_stride;   /* PR #14 (xtlg511): as cmd_copy_buf_tex */
    memset(BLOB(m), 0, 48);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24);
}
static void cmd_sample_mask(Ctx *c) { rec(c, OP_SAMPLEMASK, 0); }                     /* (cmdbuf, mask) */
static void cmd_downsample(Ctx *c) { Tex *s = hobj(A1, K_TEX), *d = hobj(A2, K_TEX); Cmd *m = rec(c, OP_DOWNSAMPLE, 0); m->a[0] = (uint64_t)(uintptr_t)s; m->a[1] = (uint64_t)(uintptr_t)d; }
static void cmd_copy_buf_buf(Ctx *c) { rec(c, OP_COPY_BUF_BUF, 0); }                  /* (cmdbuf, src, dst, size, flags) */
static void cmd_update_ubo(Ctx *c) {                    /* (cmdbuf, buffer, bufferSize, offset, size, data) */
    uint64_t size = A4, src = A5;
    Cmd *m = rec(c, OP_UPDATE_UBO, (size_t)size);
    if (src) memcpy(BLOB(m), P(src), (size_t)size); else m->a[3] = 0;
}
static void cmd_clear_buffer(Ctx *c) { rec(c, OP_CLEAR_BUF, 0); }                     /* (cmdbuf, buffer, size, value) */
static void cmd_row_stride(Ctx *c) { cb_get(A0)->row_stride = (int64_t)A1; rec(c, OP_ROWSTRIDE, 0); }
static void cmd_dispatch(Ctx *c) { cb_get(A0)->dispatches++; rec(c, OP_DISPATCH, 0); }                          /* (cmdbuf, groupsX, groupsY, groupsZ) */
static void cmd_image(Ctx *c) { uint64_t h = A3; Cmd *m = rec(c, OP_IMAGES, 8); m->a[2] = 1; memcpy(BLOB(m), &h, 8); }   /* (cmdbuf, stage, index, imageHandle) */
static void cmd_images(Ctx *c) {                        /* (cmdbuf, stage, first, count, handles) */
    int n = (int)A3;
    uint64_t src = A4;
    Cmd *m = rec(c, OP_IMAGES, (size_t)n * 8);
    if (src) memcpy(BLOB(m), P(src), (size_t)n * 8); else memset(BLOB(m), 0, (size_t)n * 8);
}

/* Round 18, TEST ONLY (SSBU_RACECHK=1): does guest memory that submitted commands NAME (uniform / storage / vertex
 * ranges) change between nvnQueueSubmitCommands on the game's thread and the replay on the render thread? The submit
 * walks the lists and keeps a hash of every range (and the first 512 bytes of a uniform range); the back end calls
 * nvn_front_racechk at the start (phase 1) and at the end (phase 2) of the replay of the same batch, which walks them
 * again and compares. Lines "racechk:". Nothing runs without the variable. */
typedef struct RaceRec { struct RaceRec *next; int n, cap, ncs, capcs; LONG64 frame; uint64_t *h; uint8_t *copy; Range *cs; } RaceRec;      /* cs: every storage range of the batch (what a dispatch may write) */
static int race_is_storage(const RaceRec *r, const Range *g) { for (int i = 0; i < r->ncs; i++) if (g->addr < r->cs[i].addr + r->cs[i].size && r->cs[i].addr < g->addr + g->size) return 1; return 0; }
static RaceRec *g_race_head, *g_race_tail;
static SRWLOCK g_race_lock = SRWLOCK_INIT;
static LONG64 g_race_stat[3][3], g_race_batches, g_race_bad_batches[3];      /* [uniform, storage, vertex][ranges seen, changed before the replay, changed during it] */
static int racechk_env(void) { static int on = -1; if (on < 0) on = getenv("SSBU_RACECHK") != NULL; return on; }
static uint64_t race_fnv(const uint8_t *b, size_t n) { uint64_t h = 0xcbf29ce484222325ULL; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
static void race_walk(uint64_t handle, int depth, RaceRec *r, int phase, int *pos, int *bad) {
    size_t len = 0;
    uint8_t *data = list_lookup(handle, &len);
    if (!data) return;
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        off += m->size;
        if (m->op == OP_CALL) { for (int i = 0; i < (int)m->a[0]; i++) { uint64_t h; memcpy(&h, BLOB(m) + i * 8, 8); if (depth < 8) race_walk(h, depth + 1, r, phase, pos, bad); } continue; }
        if (m->op != OP_UBO && m->op != OP_SSBO && m->op != OP_VBUF) continue;
        int kind = m->op == OP_UBO ? 0 : m->op == OP_SSBO ? 1 : 2, count = m->op == OP_VBUF ? (int)m->a[1] : (int)m->a[2];
        size_t cap = kind == 0 ? 65536 : kind == 1 ? 16384 : 4096;
        for (int i = 0; i < count; i++) {
            Range g;
            memcpy(&g, BLOB(m) + i * 16, 16);
            if (!g.addr || !g.size || g.addr < 0x1000) continue;
            size_t n = g.size > cap ? cap : (size_t)g.size, nc = n > 512 ? 512 : n;
            const uint8_t *p = P(g.addr);
            uint64_t h = race_fnv(p, n);
            if (phase == 0) {
                if (r->n == r->cap) { r->cap = r->cap ? r->cap * 2 : 1024; r->h = realloc(r->h, (size_t)r->cap * 8); r->copy = realloc(r->copy, (size_t)r->cap * 512); }
                r->h[r->n] = h;
                if (kind == 0) memcpy(r->copy + (size_t)r->n * 512, p, nc);
                r->n++;
                if (kind == 1) { if (r->ncs == r->capcs) { r->capcs = r->capcs ? r->capcs * 2 : 256; r->cs = realloc(r->cs, (size_t)r->capcs * sizeof(Range)); } r->cs[r->ncs++] = g; }
                continue;
            }
            int k = (*pos)++;
            if (k >= r->n) continue;
            if (phase == 1) g_race_stat[kind][0]++;
            if (r->h[k] == h) continue;
            r->h[k] = h;                                 /* phase 2 then reports only what changed during the replay */
            g_race_stat[kind][phase]++; bad[kind]++;
            static int shown, shown_cs;
            static LONG64 plain[3];
            int is_cs = kind != 0 && race_is_storage(r, &g);      /* a storage range of this batch: a dispatch's target, written by our own copy-back */
            if (kind == 2 && !is_cs) { plain[phase]++; if (plain[phase] % 200 == 1) printf("racechk: vertex ranges that are NOT a storage range of their batch and changed: %lld before the replay, %lld during it (so far)\n", (long long)plain[1], (long long)plain[2]); }
            if (is_cs ? shown_cs++ < 12 : shown < 400) {
                if (!is_cs) shown++;
                if (is_cs) printf("racechk: (a storage range of this batch) ");
                printf("racechk: frame %lld %s range %d of the batch (%s, stage/first %d, slot %d) guest %llx+%llu CHANGED %s", (long long)g_st.frames, kind == 0 ? "UNIFORM" : kind == 1 ? "storage" : "vertex", k,
                       k_op_name[m->op], (int)m->a[0], (int)(m->op == OP_VBUF ? m->a[0] : m->a[1]) + i, (unsigned long long)g.addr, (unsigned long long)g.size, phase == 1 ? "between the submit and the start of its replay" : "DURING its replay");
                if (kind == 0) {
                    const uint8_t *o = r->copy + (size_t)k * 512;
                    size_t d = 0;
                    while (d < nc && o[d] == p[d]) d++;
                    if (d < nc) { d &= ~(size_t)3; printf("; first difference at +%llu: at submit %08x %08x %08x %08x, now %08x %08x %08x %08x", (unsigned long long)d, *(const uint32_t *)(o + d), d + 8 <= nc ? *(const uint32_t *)(o + d + 4) : 0, d + 12 <= nc ? *(const uint32_t *)(o + d + 8) : 0, d + 16 <= nc ? *(const uint32_t *)(o + d + 12) : 0,
                                                       *(const uint32_t *)(p + d), d + 8 <= nc ? *(const uint32_t *)(p + d + 4) : 0, d + 12 <= nc ? *(const uint32_t *)(p + d + 8) : 0, d + 16 <= nc ? *(const uint32_t *)(p + d + 12) : 0); }
                    else printf("; the difference is after the first 512 bytes");
                    memcpy(r->copy + (size_t)k * 512, p, nc);
                }
                printf("\n");
            }
        }
    }
}
static void racechk_submit(const uint64_t *handles, int count) {      /* the game's thread */
    RaceRec *r = calloc(1, sizeof *r);
    int pos = 0, bad[3] = {0, 0, 0};
    r->frame = g_st.frames;
    for (int i = 0; i < count; i++) race_walk(handles[i], 0, r, 0, &pos, bad);
    AcquireSRWLockExclusive(&g_race_lock);
    if (g_race_tail) g_race_tail->next = r; else g_race_head = r;
    g_race_tail = r;
    ReleaseSRWLockExclusive(&g_race_lock);
}
void nvn_front_racechk(const uint64_t *handles, int count, int phase) {      /* render thread: 1 = before the replay of the oldest recorded batch, 2 = after it */
    if (!racechk_env()) return;
    AcquireSRWLockExclusive(&g_race_lock);
    RaceRec *r = g_race_head;
    if (r && phase == 2) { g_race_head = r->next; if (!g_race_head) g_race_tail = NULL; }
    ReleaseSRWLockExclusive(&g_race_lock);
    if (!r) return;
    int pos = 0, bad[3] = {0, 0, 0};
    for (int i = 0; i < count; i++) race_walk(handles[i], 0, r, phase, &pos, bad);
    for (int k = 0; k < 3; k++) if (bad[k]) g_race_bad_batches[k]++;
    if (phase == 1) g_race_batches++;
    if (phase == 2) {
        static LONG64 last;
        if (g_st.frames - last >= 300) {
            last = g_st.frames;
            printf("racechk: totals at frame %lld: %lld batches | uniform ranges %lld, changed before the replay %lld, during %lld | storage %lld / %lld / %lld | vertex (first 4 KB) %lld / %lld / %lld | batches with a changed uniform / storage / vertex range: %lld / %lld / %lld\n",
                   (long long)g_st.frames, (long long)g_race_batches, (long long)g_race_stat[0][0], (long long)g_race_stat[0][1], (long long)g_race_stat[0][2], (long long)g_race_stat[1][0], (long long)g_race_stat[1][1], (long long)g_race_stat[1][2],
                   (long long)g_race_stat[2][0], (long long)g_race_stat[2][1], (long long)g_race_stat[2][2], (long long)g_race_bad_batches[0], (long long)g_race_bad_batches[1], (long long)g_race_bad_batches[2]);
        }
        free(r->h); free(r->copy); free(r->cs); free(r);
    }
}

/* ---- queue and window */
static void l1_pipe_after_submit(void); static int l1_pipe_wait(void);
static void queue_submit(Ctx *c) {                      /* (queue, count, handles) */
    SubmitJob j = {(int)A1, P(A2)};
    L1T("SUBMIT %d lists: %llx %llx %llx %llx", (int)A1, A2 && A1 > 0 ? (unsigned long long)rd64(A2) : 0ull, A2 && A1 > 1 ? (unsigned long long)rd64(A2 + 8) : 0ull, A2 && A1 > 2 ? (unsigned long long)rd64(A2 + 16) : 0ull, A2 && A1 > 3 ? (unsigned long long)rd64(A2 + 24) : 0ull);
    { void display_l1_submit(void); display_l1_submit(); }      /* L1: submits between the game's wait for the window texture and its present */
    if (j.count > 0 && A2 && racechk_env()) racechk_submit(j.handles, j.count);
    { int r1_ra_phase(void), r1_on(void); int ph = r1_ra_phase();      /* R1 round 5 (diagnosis): a submitted handle the host no longer knows, by run-ahead phase */
      if (r1_on()) { static LONG64 bad[3], ok[3]; size_t ln; for (int i = 0; i < j.count; i++) { if (list_lookup(j.handles[i], &ln)) ok[ph]++; else { bad[ph]++; if (bad[ph] <= 5 || !(bad[ph] & (bad[ph] - 1))) { uint32_t si = (uint32_t)j.handles[i] - 1; int have = si < (uint32_t)g_list_count; printf("r1-ra: submitted handle %llx unknown (phase %d: %lld bad / %lld good so far; slot gen %u data %d finalized %d created frame %lld used %lld, frame now %lld)\n", (unsigned long long)j.handles[i], ph, (long long)bad[ph], (long long)ok[ph],
                have ? g_lists[si].gen : 0, have && g_lists[si].data != NULL, have ? g_lists[si].finalized : -1, have ? (long long)g_lists[si].created : -1, have ? (long long)g_lists[si].used : -1, (long long)g_st.frames); } } } } }
    QueueObj *q = hobj(A0, K_QUEUE);
    if (q && A2) {                              /* what the submitted commands took of the queue's memory (model: see rec; a dispatch's launch descriptor is 0x100 bytes) */
        AcquireSRWLockShared(&g_list_lock);
        for (int i = 0; i < j.count; i++) {
            uint64_t h = j.handles[i];
            uint32_t k = (uint32_t)h - 1;
            if (h && k < (uint32_t)g_list_count && g_lists[k].data) { q->cmd_used += g_lists[k].model_cmd; q->control_used += g_lists[k].model_ctrl; q->compute_used += (LONG64)g_lists[k].dispatches * 0x100; }
        }
        ReleaseSRWLockShared(&g_list_lock);
    }
    if (j.count > 0 && A2 && drawtag_env()) {      /* round 17, TEST (SSBU_DRAWTAG): the frame SSBU_SHOT_FIGHT will save: its commands are tagged */
        /* the game polls the pad for frame n AFTER it has submitted the commands whose picture is saved as fight_<n>
         * (submit, next frame's logic and poll, present): the tag is armed by the present that saved fight_<n-1>, so
         * SSBU_SHOT_FIGHT must name n-1 as well (e.g. 699,700 tags the commands of 700). First form (the pending
         * shot at the submit) tagged nothing: run p17_zvk. */
        extern volatile LONG g_drawtag_next;
        int n = (int)InterlockedExchange(&g_drawtag_next, 0);
        if (n) { if (gl_async_on()) gl_post(job_drawtag, &n, sizeof n); else gl_run(job_drawtag, &n); }
    }
    int r1_ra_quiet(void);
    if (j.count > 0 && A2 && r1_ra_quiet()) { static LONG64 n; n++; }      /* R1 round 5, run-ahead: the real pass's submission = the lists the speculative pass already ran on the GPU: not run again */
    else if (j.count > 0 && A2) {
        if (gl_async_on() && j.count <= 4096) {
            size_t size = sizeof j + (size_t)j.count * 8;
            SubmitJob *copy = malloc(size);
            *copy = j;
            memcpy(copy + 1, j.handles, (size_t)j.count * 8);
            gl_post(g_rb->submit_async, copy, size);
            free(copy);
        } else gl_run(g_rb->submit, &j);
        l1_pipe_after_submit();      /* L1 round 2 (SSBU_L1_PIPE=1): the picture is complete after this submission: present it now, hold the game here */
    }
}
static void wb_set_textures(Ctx *c) {                   /* (builder, count, textures[]): kept in the builder (0x40 bytes) */
    int n = (int)A1 < 6 ? (int)A1 : 6;
    wr8(A0, (uint8_t)n);
    for (int i = 0; i < n; i++) wr64(A0 + 8 + (uint64_t)i * 8, rd64(A2 + (uint64_t)i * 8));
}
/* Track V/O session 9: the textures of every window the game made (for the Vulkan back end's resolution scale: nvn_vk_draw.c wintex_is). */
static Tex *g_window_tex[16];
static volatile int g_window_tex_n;
int nvn_front_tex_is_window(const Tex *t) { for (int i = 0; i < g_window_tex_n; i++) if (g_window_tex[i] == t) return 1; return 0; }
static void win_initialize(Ctx *c) {                    /* (window, builder) */
    Window *w = hnew(A0, K_WIN, sizeof *w);
    w->count = (int)(rd32(A1) & 0xff) <= 6 ? (int)(rd32(A1) & 0xff) : 0;
    w->interval = rd8(A1 + 4); w->nactive = rd8(A1 + 5); w->native = rd64(A1 + 56);
    display_set_interval(w->interval);
    for (int i = 0; i < w->count; i++) w->tex[i] = hobj(rd64(A1 + 8 + (uint64_t)i * 8), K_TEX);
    for (int i = 0; i < w->count; i++) if (g_window_tex_n < 16) g_window_tex[g_window_tex_n++] = w->tex[i];
    printf("nvn_gl: window with %d textures", w->count);
    if (w->count && w->tex[0]) printf(" (%dx%d format 0x%x)", w->tex[0]->b.width, w->tex[0]->b.height, w->tex[0]->b.format);
    printf("\n");
    RET(1);
}
static void win_acquire(Ctx *c) {                       /* (window, sync, int *textureIndex) -> 0 = success */
    static volatile LONG next;
    Window *w = hobj(A0, K_WIN);
    L1T("window acquire (index %d)", (int)((next + 1) % 3));
    if (A2) wr32(A2, (uint32_t)(InterlockedIncrement(&next) % (w && w->count ? (w->nactive ? w->nactive : w->count) : 2)));
    InterlockedExchangePointer(&g_window_sync, A1 ? hobj(A1, K_SYNC) : NULL);        /* P3 round 3: nvnSyncWait on it = wait for the refresh that frees the texture */
    if (w && A2 && A1) { extern Window *g_l1_acq_win; extern volatile LONG g_l1_acq_idx, g_l1_acq_new; g_l1_acq_win = w; g_l1_acq_idx = (LONG)rd32(A2); InterlockedExchange(&g_l1_acq_new, 1); }      /* L1: early present */
    display_clock_sample(A2);
    RET(0);
}
static void queue_acquire(Ctx *c) { c->x[0] = c->x[1]; c->x[1] = 0; win_acquire(c); }      /* nvnQueueAcquireTexture(queue, window, int *textureIndex) */
static void win_set_crop(Ctx *c) { Window *w = hobj(A0, K_WIN); if (w) { w->crop[0] = (int)A1; w->crop[1] = (int)A2; w->crop[2] = (int)A3; w->crop[3] = (int)A4; } }
/* close4: a window built on the movie maker's native window (nn::album::GetMovieMakerNativeWindow, nnalbum.c) is not the screen:
 * what the game presents to it is one picture of the video being recorded. The picture is read back and handed to the encoder;
 * nothing is shown and the display is not paced by it. */
uint64_t album_movie_native_window(void);
int album_movie_push_frame(const uint8_t *rgba, int w, int h, int stride, int bottom_up);
void album_movie_frame_source_attach(void);
static void job_present_movie(void *arg) {
    PresentJob *j = arg;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    if (!t || t->dead || !t->o.gl || t->b.width <= 0 || t->b.height <= 0) return;
    int w = t->b.width, h = t->b.height;
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    glBindFramebuffer(GL_READ_FRAMEBUFFER_, g_fbo_read);
    glFramebufferTexture(GL_READ_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, t->o.gl, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);                /* GL row 0 is the top row of an NVN texture (see save_bmp) */
    album_movie_push_frame(px, w, h, w * 4, 0);
    free(px);
    glBindFramebuffer(GL_FRAMEBUFFER_, S.have_fbo ? S.fbo : 0);
}
/* Session 13, TEST ONLY: SSBU_SHOT_FIGHT=<n>[,<n>...] (nnhid.c counts the fight frames at the pad poll): the present the game
 * queues next after the poll that reached fight frame n is saved as fight_<n>.bmp in SSBU_SHOT_DIR by whichever back end runs. */
static volatile LONG g_fight_shot;
void nvn_front_shot_fight(int n) { InterlockedExchange(&g_fight_shot, n); }
int nvn_front_shot_pending(void) { return (int)g_fight_shot; }      /* round 17 (SSBU_DRAWTAG): the fight frame the next queued present will be saved as, 0 = none */
unsigned long nvn_front_present_tid(void) { return g_pf_main_tid; }      /* the game thread that queues the presents (the stall log of nvn_vk_wd.c) */
/* Session 14 (display.c, the vsync thread at a tick): the present the Vulkan back end kept for this tick, as a job of the
 * render thread (in order with the other jobs; nobody waits for it). */
void nvn_vk_present_flush_job(void *arg);              /* nvn_vk_draw.c */
void nvn_front_post_present_flush(void) { if (g_vk) gl_post(nvn_vk_present_flush_job, NULL, 0); }
void nvn_vk_hfr_present_job(void *arg);                /* nvn_vk_draw.c */
int display_hfr_try_queue(void);                       /* display.c */
void nvn_front_post_hfr_vblank(void) { if (g_vk && display_hfr_try_queue()) gl_post(nvn_vk_hfr_present_job, NULL, 0); }
/* osk.c, the on-screen keyboard: drawn by the Vulkan renderer only (the OpenGL path keeps the old text dialog) */
void nvn_vk_osk_redraw_job(void *arg);                 /* nvn_vk_draw.c */
long long nvn_vk_presents(void);                       /* nvn_vk_draw.c */
int nvn_front_osk_ready(void) { return g_vk != 0; }
void nvn_front_osk_redraw(void) { if (g_vk) gl_post(nvn_vk_osk_redraw_job, NULL, 0); }
long long nvn_front_presents(void) { return g_vk ? nvn_vk_presents() : 0; }
/* ---- Track L1 (input latency), early present. The game submits the commands of a frame's picture BEFORE it waits for the
 * window texture (nvnSyncWait on the acquire sync = display_wait_refresh), then reads the pad, runs that frame's logic and
 * only then calls nvnQueuePresentTexture for the picture it had finished before the wait (measured: tests\logs\vk13_l1t_*,
 * `l1-submit:` 0 submits between the wait and the present in every fight frame). With SSBU_L1_EARLY=1 (graphics.ini
 * latency = early | lowest) that picture is handed to the window when the wait BEGINS, for the tick the game waits for (the
 * hold of session 14 keeps it until then), instead of one tick later; the game's own present of the same texture then only
 * does the pacing bookkeeping. If the game does not present that texture next, the early present is counted as a miss. */
Window *g_l1_acq_win;
volatile LONG g_l1_acq_idx, g_l1_acq_new;
static Window *volatile g_l1_early_win;
static volatile LONG g_l1_early_idx;
static LONG64 g_l1_early_n, g_l1_early_miss;
static void l1_early_present(void) {
    int display_l1_early(void);
    if (!g_vk || !display_l1_early() || !InterlockedExchange(&g_l1_acq_new, 0)) return;
    Window *w = g_l1_acq_win;
    if (!w || (w->native && w->native == album_movie_native_window())) return;
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    PresentJob j = {w, (int)g_l1_acq_idx};
    /* the picture of THIS acquire is the one rendered since the last present: the index the game acquired at the end of its
     * previous frame was the target of the commands it submitted since; present it */
    g_l1_early_win = w; g_l1_early_idx = j.index;
    g_l1_early_n++;
    gl_post(g_rb->present, &j, sizeof j);
}
/* L1 round 2, PIPE (SSBU_L1_PIPE=1): measured (SSBU_L1_TRACE, tests\logs\vk13_l1t_tr1.log): after its present the game
 * acquires the next window texture, waits for it (nvnSyncWait on the acquire sync, 0x710386fce0) and for the previous
 * frame's fence (0x710386fcf0), then ends the command buffers its "render dispatch" thread recorded during the previous
 * frame plus one of its own and submits them in ONE call (0x71037f0b40, list of 6), then runs the next frame (task workers,
 * pad poll, logic) and only then presents that picture (0x710386fc84). The picture is complete after that submission
 * (l1-submit: no other submission before the present). Pipe: the window wait returns at once; right after that submission
 * the picture goes to the render thread for the next tick and the GAME is held there until that tick (display_wait_refresh),
 * i.e. before its next pad poll as before; the game's later present of the same texture only does the bookkeeping.
 * Saves one whole frame of the game's two. */
static int g_l1_pipe = -1;
static volatile LONG g_l1_pipe_pending;
static int l1_pipe_on(void) { if (g_l1_pipe < 0) { const char *e = getenv("SSBU_L1_PIPE"); int display_l1_pipe_default(void); g_l1_pipe = e ? atoi(e) != 0 : display_l1_pipe_default(); printf("nvn: pipe (picture presented right after the game's frame submission, game held there) %s%c", g_l1_pipe ? "ON" : "off", 10); } return g_l1_pipe && g_vk; }
static volatile LONG g_l1_sh_pending;
static int l1_pipe_wait(void) {
    InterlockedExchange(&g_l1_sh_pending, 1);
    if (!l1_pipe_on() || !g_l1_acq_win) return 0;
    InterlockedExchange(&g_l1_pipe_pending, 1);
    return 1;
}
static void l1_nop_job(void *arg) { (void)arg; }
static void l1_pipe_after_submit(void) {
    { static int sh = -1; if (sh < 0) sh = getenv("SSBU_L1_SUBMIT_HOLD") && atoi(getenv("SSBU_L1_SUBMIT_HOLD")); if (sh && !g_l1_pipe && InterlockedExchange(&g_l1_sh_pending, 0)) gl_run(l1_nop_job, NULL); }      /* TEST: old path, but the game waits until the render thread has replayed this submission (is the old picture a race with the game's next frame?) */
    if (!InterlockedExchange(&g_l1_pipe_pending, 0)) return;
    { int r1_ra_quiet(void); if (r1_ra_quiet()) return; }      /* R1 round 5, run-ahead: the real pass's submission repeats the picture the speculative pass presented: not presented, the game not held */
    Window *w = g_l1_acq_win;
    if (!w || (w->native && w->native == album_movie_native_window())) { display_wait_refresh(); return; }
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    PresentJob j = {w, (int)g_l1_acq_idx};
    g_l1_early_win = w; g_l1_early_idx = j.index;
    g_l1_early_n++;
    { void display_present_plan(int vulkan); display_present_plan(g_vk); }
    gl_post(g_rb->present, &j, sizeof j);
    display_wait_refresh();
}
void nvn_l1_report(FILE *out) { if (g_l1_early_n || g_l1_early_miss) fprintf(out, "l1: early presents %lld, game presents that did not match an early present %lld%c", (long long)g_l1_early_n, (long long)g_l1_early_miss, 10); }
static void queue_present(Ctx *c) {                     /* (queue, window, textureIndex) */
    PresentJob j = {hobj(A1, K_WIN), (int)A2};
    L1T("PRESENT texture %d", (int)A2);
    if (j.win && j.win->native && j.win->native == album_movie_native_window()) { gl_run(g_rb->present_movie, &j); return; }
    g_pf_main_tid = GetCurrentThreadId();
    int ra_mode; { int r1_ra_present_mode(void); ra_mode = g_l1_pipe > 0 ? r1_ra_present_mode() : 0; }      /* R1 round 5, run-ahead: 2 = the real pass's present (nothing), 1 = presented at the speculative submission already */
    if (ra_mode == 2) return;
    if (ra_mode == 1 && g_l1_early_win == j.win) j.index = (int)g_l1_early_idx;      /* (the restore rolled the game's texture index back, the host's acquire count is not: the texture presented is the host's) */
    if (g_l1_early_win && g_l1_early_win == j.win && g_l1_early_idx == j.index) {      /* L1: this picture was presented at the game's wait already: only the pacing bookkeeping */
        g_l1_early_win = NULL;
        if (g_l1_pipe) {      /* round 2: planned and held at the submission already; a picture wanted by SSBU_SHOT_FIGHT is saved by presenting the same texture again (test only) */
            if (g_fight_shot) { void nvn_vk_shot_job(void *arg); j.shot_fight = (int)InterlockedExchange(&g_fight_shot, 0); gl_run(nvn_vk_shot_job, &j); }
        } else {
        display_before_present();
        { void display_present_plan(int vulkan); display_present_plan(g_vk); }
        }
        { void display_l1_early_skip(void); display_l1_early_skip(); }
        display_pace();
        return;
    }
    if (g_l1_early_win) { g_l1_early_win = NULL; g_l1_early_miss++; }
    if (g_fight_shot) j.shot_fight = (int)InterlockedExchange(&g_fight_shot, 0);      /* session 13 (test, SSBU_SHOT_FIGHT): bound here, on the game's thread, so that both renderers save the same game frame */
    if (j.shot_fight && drawtag_env()) InterlockedExchange(&g_drawtag_next, j.shot_fight + 1);      /* round 17 (SSBU_DRAWTAG): the next submission is the next fight frame's */
    display_before_present();
    { void display_present_plan(int vulkan); display_present_plan(g_vk); }      /* session 14: the tick this frame is shown at, decided before the present (display.c) */
    gl_run(g_rb->present, &j);
    display_pace();
}


/* ================================================================ the rest of the NVN API (plan\phases\G1_nvn_audit.md)
 * Signatures and enum values: the public nvn.h (open-ead/nnheaders, MIT). Every entry point the game obtains has a handler
 * here or in nvn_stub.c; the ones that have no effect on this platform by the API's own meaning say why in nvn_audit_notes.tsv. */

/* ---- debug labels: kept for every object (nvn_label_get), and given to the OpenGL object where there is one */
typedef struct LabelEnt { uint64_t key; char text[96]; struct LabelEnt *next; } LabelEnt;
static LabelEnt *g_labels[1024];
static SRWLOCK g_label_lock = SRWLOCK_INIT;
void nvn_label_set(uint64_t obj, const char *text) {
    unsigned h = (unsigned)((obj >> 4) * 2654435761u) & 1023;
    AcquireSRWLockExclusive(&g_label_lock);
    LabelEnt *e = g_labels[h];
    while (e && e->key != obj) e = e->next;
    if (!e) { e = calloc(1, sizeof *e); e->key = obj; e->next = g_labels[h]; g_labels[h] = e; }
    snprintf(e->text, sizeof e->text, "%s", text ? text : "");
    ReleaseSRWLockExclusive(&g_label_lock);
}
const char *nvn_label_get(uint64_t obj) {
    unsigned h = (unsigned)((obj >> 4) * 2654435761u) & 1023;
    const char *r = "";
    AcquireSRWLockShared(&g_label_lock);
    for (LabelEnt *e = g_labels[h]; e; e = e->next) if (e->key == obj) r = e->text;
    ReleaseSRWLockShared(&g_label_lock);
    return r;
}
void nvn_label_drop(uint64_t obj) { if (nvn_label_get(obj)[0]) nvn_label_set(obj, ""); }
static void any_set_label(Ctx *c) {                     /* nvn<Object>SetDebugLabel(object, const char *label) */
    nvn_label_set(A0, A1 ? (const char *)P(A1) : "");
    Tex *t = hobj(A0, K_TEX);
    Prog *p = hobj(A0, K_PROG);
    if (t) t->label_dirty = 1;                          /* applied by tex_sync on the render thread */
    if (p) p->label_dirty = 1;                          /* applied when its GLSL programs are next used */
}
static void any_debug_id(Ctx *c) { RET(A0); }           /* nvn<Object>GetDebugID: a number unique to the object: its address */

/* ---- device */
static uint64_t g_nvn_device;
static int g_device_flags;
static LARGE_INTEGER g_qpf;
static uint64_t now_ns(void) {
    LARGE_INTEGER t;
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    QueryPerformanceCounter(&t);
    return (uint64_t)(t.QuadPart / g_qpf.QuadPart) * 1000000000ull + (uint64_t)(t.QuadPart % g_qpf.QuadPart) * 1000000000ull / (uint64_t)g_qpf.QuadPart;
}
static void devb_defaults(Ctx *c) { memset(P(A0), 0, 0x40); }
static void devb_set_flags(Ctx *c) { wr32(A0, (uint32_t)A1); }
static void devb_get_flags(Ctx *c) { RET(rd32(A0)); }
static void dev_initialize(Ctx *c) { g_nvn_device = A0; g_device_flags = A1 ? (int)rd32(A1) : 0; RET(1); }      /* (device, builder) */
static void dev_finalize(Ctx *c) { if (g_nvn_device == A0) g_nvn_device = 0; }
static void dev_get_origin_mode(Ctx *c) { RET(g_origin_mode); }
static void dev_get_depth_mode(Ctx *c) { RET(g_depth_mode); }
static void dev_timestamp_now(Ctx *c) { RET(now_ns()); }
static void dev_timestamp_of(Ctx *c) { RET(A1 ? rd64(A1 + 8) : 0); }      /* (device, const NVNcounterData *): counter timestamps are written in nanoseconds */
static void dev_debug_domain(Ctx *c) { static volatile LONG next; RET(InterlockedIncrement(&next)); }
static void dev_separate_handle(Ctx *c) { RET((uint64_t)(int64_t)(int32_t)(uint32_t)A1 | 0x100000000ull); }   /* nvnDeviceGetSeparateTextureHandle [sdk 0x41bc60] */
static void dev_separate_sampler_handle(Ctx *c) { RET((uint64_t)((int64_t)(int32_t)(uint32_t)A1 << 20)); }                 /* nvnDeviceGetSeparateSamplerHandle [sdk 0x41bc50]: sbfiz x0, x1, #20, #32 */
static void dev_debugger_attached(Ctx *c) { RET(0); }   /* no external NVN debugger exists for this process: the true answer */
/* nvnDeviceInstallDebugCallback: in this SDK build the function is a bare `ret` [sdk 0x41b890] (the entry the name resolves to in
 * the table nvnDeviceGetProcAddress reads, [0x41d6a0] -> table [0xc28398]): nothing is stored, nothing can ever be reported.
 * Bound to n_nop in the table below. */
static void job_housekeeping(void *arg) { (void)arg; gl_housekeeping(); }
static void dev_apply_deferred(Ctx *c) { gl_run(g_rb->housekeeping, NULL); }      /* (device, age): objects finalized so far are released now */
static void any_get_device(Ctx *c) { RET(g_nvn_device); }                       /* nvn<Builder>GetDevice: there is one device */
static void any_set_device(Ctx *c) { if (A1) g_nvn_device = A1; }               /* nvn<Builder>SetDevice(builder, device) */

/* ---- sync objects: OpenGL fences */
static void sync_initialize(Ctx *c) { hnew(A0, K_SYNC, sizeof(SyncObj)); RET(1); }
static void job_sync_drop(void *arg) { SyncObj *o = arg; if (o->s) glDeleteSync(o->s); o->s = NULL; }
static void sync_finalize(Ctx *c) {                     /* the small host record stays: a recorded FenceSync / WaitSync may still name it */
    SyncObj *o = hobj(A0, K_SYNC);
    if (!o) return;
    InterlockedExchange(&o->dead, 1);
    wr32(A0 + 8, 0);
    gl_run(g_rb->sync_drop, o);
}
static void fence_now(SyncObj *o, int flags) {
    if (o->dead) return;
    if (o->s) glDeleteSync(o->s);
    o->s = glFenceSync(0x9117 /* SYNC_GPU_COMMANDS_COMPLETE */, 0);
    if (flags & 1) glFlush();                           /* NVN_SYNC_FLAG_FLUSH_FOR_CPU */
}
static void job_fence(void *arg) { SyncJob *j = arg; fence_now(j->o, j->flags); }
static void queue_fence_sync(Ctx *c) {                  /* (queue, sync, condition, flags) */
    L1T("fence sync %llx", (unsigned long long)A1);
    SyncObj *o = hobj(A1, K_SYNC);
    SyncJob j = {o, (int)A3, 0, 0};
    if (o) { if (gl_async_on()) gl_post(g_rb->fence, &j, sizeof j); else gl_run(g_rb->fence, &j); }
}
static void job_sync_wait(void *arg) {
    SyncJob *j = arg;
    if (!j->o->s) { j->result = 3; return; }            /* never fenced: NVN_SYNC_WAIT_RESULT_FAILED */
    GLenum r = glClientWaitSync(j->o->s, 1 /* SYNC_FLUSH_COMMANDS_BIT */, j->timeout);
    j->result = r == 0x911A ? 0 : r == 0x911C ? 1 : r == 0x911B ? 2 : 3;       /* already signaled, condition satisfied, timeout expired, failed */
}
static void sync_wait(Ctx *c) {                         /* (sync, timeoutNs) -> NVNsyncWaitResult */
    L1T("sync wait %llx%s", (unsigned long long)A0, hobj(A0, K_SYNC) == g_window_sync ? " (WINDOW texture)" : "");
    SyncObj *o = hobj(A0, K_SYNC);
    SyncJob j = {o, 0, A1, 3};
    if (o) gl_run(g_rb->sync_wait, &j);
    if (o && o == g_window_sync && A1 && j.result == 3) {      /* P3 round 3: the window texture's sync (never fenced by us): it is free at the refresh that shows the queued frame (display.c) */
        if (l1_pipe_wait()) j.result = 1; else {      /* L1 round 2: pipe mode does not hold here */
        l1_early_present();                                    /* L1 (latency = early / SSBU_L1_EARLY=1): the picture the game finished before this wait goes to the window now (at its tick) */
        if (display_wait_refresh()) j.result = 1;             /* NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED; otherwise as before */
        }
    }
    RET(j.result);
}
static void job_server_wait(void *arg) { SyncJob *j = arg; if (j->o->s) glWaitSync(j->o->s, 0, 0xFFFFFFFFFFFFFFFFull); j->result = j->o->s != NULL; }
static void queue_wait_sync(Ctx *c) {                   /* (queue, sync) -> NVNboolean */
    SyncObj *o = hobj(A1, K_SYNC);
    SyncJob j = {o, 0, 0, 0};
    if (o) gl_run(g_rb->server_wait, &j);
    RET(j.result);
}
static void sync_no_gl_interop(Ctx *c) { RET(0); }

/* ---- events: a 32-bit value in memory pool storage that the CPU and queued commands signal and wait for.
 * NVNeventBuilder: +0 pool, +8 offset. NVNevent: +0 pool, +8 offset, +16 address of the value. */
static void evb_defaults(Ctx *c) { memset(P(A0), 0, 0x20); }
static void evb_storage(Ctx *c) { wr64(A0, A1); wr64(A0 + 8, A2); }
static void evb_get_storage(Ctx *c) { if (A1) wr64(A1, rd64(A0 + 8)); RET(rd64(A0)); }
static void ev_get_pool(Ctx *c) { RET(rd64(A0)); }
static void ev_get_offset(Ctx *c) { RET(rd64(A0 + 8)); }
static void ev_initialize(Ctx *c) {                     /* (event, builder) */
    uint64_t mem = 0, size = 0, pool = rd64(A1), off = rd64(A1 + 8);
    memset(P(A0), 0, 0x40);
    wr64(A0, pool); wr64(A0 + 8, off);
    if (!pool || !nvn_pool_range(pool, &mem, &size) || !mem || off + 4 > size) { RET(0); return; }
    wr64(A0 + 16, mem + off);
    RET(1);
}
static void ev_finalize(Ctx *c) { wr64(A0 + 16, 0); }
static void ev_get_value(Ctx *c) { uint64_t a = rd64(A0 + 16); RET(a ? rd32(a) : 0); }
static void event_signal_at(uint64_t a, int mode, uint32_t value) {
    if (!a) return;
    if (mode == 1) InterlockedExchangeAdd((volatile LONG *)P(a), (LONG)value); else InterlockedExchange((volatile LONG *)P(a), (LONG)value);
}
static void ev_signal(Ctx *c) { event_signal_at(rd64(A0 + 16), (int)A1, (uint32_t)A2); }      /* (event, mode, value) */

/* ---- queue. NVNqueueBuilder (0x40): +0 queue memory, +8 its size, +0x10 command, +0x18 compute, +0x20 control memory size,
 * +0x28 command flush threshold, +0x30 flags, +0x34 priority. NVNqueue: +0 host record. */
static void qb_defaults(Ctx *c) {                       /* sizes: the defaults of the device (nvnDeviceGetInteger 0x3a / 0x3d / 0x3b in this table), priority normal */
    memset(P(A0), 0, 0x40);
    wr64(A0 + 0x10, 65536); wr64(A0 + 0x18, 262144); wr64(A0 + 0x20, 16384); wr64(A0 + 0x28, 0); wr32(A0 + 0x34, 100);
}
static void qb_set_flags(Ctx *c) { wr32(A0 + 0x30, (uint32_t)A1); }
static void qb_set_cmd(Ctx *c) { wr64(A0 + 0x10, A1); }
static void qb_set_compute(Ctx *c) { wr64(A0 + 0x18, A1); }
static void qb_set_control(Ctx *c) { wr64(A0 + 0x20, A1); }
static void qb_set_threshold(Ctx *c) { wr64(A0 + 0x28, A1); }
static void qb_set_priority(Ctx *c) { wr32(A0 + 0x34, (uint32_t)A1); }
static void qb_set_memory(Ctx *c) { wr64(A0, A1); wr64(A0 + 8, A2); }
static void qb_get_flags(Ctx *c) { RET(rd32(A0 + 0x30)); }
static void qb_get_cmd(Ctx *c) { RET(rd64(A0 + 0x10)); }
static void qb_get_compute(Ctx *c) { RET(rd64(A0 + 0x18)); }
static void qb_get_control(Ctx *c) { RET(rd64(A0 + 0x20)); }
static void qb_get_threshold(Ctx *c) { RET(rd64(A0 + 0x28)); }
static void qb_get_priority(Ctx *c) { RET(rd32(A0 + 0x34)); }
static void qb_get_memory(Ctx *c) { RET(rd64(A0)); }
static void qb_get_memory_size(Ctx *c) { RET(rd64(A0 + 8)); }
static void qb_get_queue_memory_size(Ctx *c) { RET((rd64(A0 + 0x10) + rd64(A0 + 0x18) + rd64(A0 + 0x20) + 0xfff) & ~0xfffull); }   /* what a queue with these sizes needs */
static void queue_initialize(Ctx *c) {                  /* (queue, builder) */
    QueueObj *q = hnew(A0, K_QUEUE, sizeof *q);
    q->cmd_size = rd64(A1 + 0x10); q->compute_size = rd64(A1 + 0x18); q->control_size = rd64(A1 + 0x20); q->flush_threshold = rd64(A1 + 0x28);
    q->flags = (int)rd32(A1 + 0x30); q->priority = (int)rd32(A1 + 0x34);
    RET(1);
}
static void queue_finalize(Ctx *c) { gl_barrier(); QueueObj *q = hobj(A0, K_QUEUE); if (q) { wr32(A0 + 8, 0); free(q); } nvn_label_drop(A0); }
static void queue_total_cmd(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->cmd_used : 0); }
static void queue_total_control(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->control_used : 0); }
static void queue_total_compute(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); RET(q ? q->compute_used : 0); }
static void queue_reset_usage(Ctx *c) { QueueObj *q = hobj(A0, K_QUEUE); if (q) q->cmd_used = q->control_used = q->compute_used = 0; }
static void job_flush(void *arg) { (void)arg; glFlush(); }
static void job_finish(void *arg) { (void)arg; glFinish(); }
static void queue_flush(Ctx *c) { (void)c; if (gl_async_on()) gl_post(g_rb->flush, NULL, 0); else gl_run(g_rb->flush, NULL); }
static void queue_finish(Ctx *c) { (void)c; gl_run(g_rb->finish, NULL); }
static void job_reset_status(void *arg) { *(int *)arg = glGetGraphicsResetStatus ? (int)glGetGraphicsResetStatus() : 0; }
static void queue_get_error(Ctx *c) {                   /* (queue, NVNqueueErrorInfo *) -> NVNqueueGetErrorResult: the state of the real GPU context */
    int st = 0;
    gl_run(g_rb->reset_status, &st);
    RET(st ? 1 : 0);                                    /* GPU_NO_ERROR, or GPU_ERROR_UNKNOWN after a context reset */
}

/* ---- multisample state (0x18 bytes): +0 multisample enable, +1 alpha to coverage, +2 its dither, +3 bit 0 coverage to colour,
 * bit 1 sample locations, bit 2 sample location grid, bits 3..4 coverage modulation mode, bits 5..7 coverage-to-colour output;
 * +4 samples, +5 raster samples; +8..+23 sixteen sample locations, x in the low and y in the high four bits (sixteenths). */
static void ms_defaults(Ctx *c) { memset(P(A0), 0, 0x18); wr8(A0, 1); wr8(A0 + 2, 1); memset(P(A0 + 8), 0x88, 16); }
static void ms_enable(Ctx *c) { wr8(A0, (uint8_t)(A1 & 1)); }
static void ms_a2c(Ctx *c) { wr8(A0 + 1, (uint8_t)(A1 & 1)); }
static void ms_a2c_dither(Ctx *c) { wr8(A0 + 2, (uint8_t)(A1 & 1)); }
static void ms_samples(Ctx *c) { wr8(A0 + 4, (uint8_t)A1); }
static void ms_raster_samples(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); }
static void ms_flag(uint64_t a, unsigned shift, unsigned mask, uint64_t v) { wr8(a + 3, (uint8_t)((rd8(a + 3) & ~(mask << shift)) | ((v & mask) << shift))); }
static void ms_c2c(Ctx *c) { ms_flag(A0, 0, 1, A1); }
static void ms_locations_on(Ctx *c) { ms_flag(A0, 1, 1, A1); }
static void ms_grid_on(Ctx *c) { ms_flag(A0, 2, 1, A1); }
static void ms_covmod(Ctx *c) { ms_flag(A0, 3, 3, A1); }
static void ms_c2c_output(Ctx *c) { ms_flag(A0, 5, 7, A1); }
static void ms_set_locations(Ctx *c) {                  /* (state, start, count, const float *xy pairs in [0, 1)) */
    for (int i = 0; i < (int)A2 && (int)A1 + i < 16; i++) {
        float x, y;
        memcpy(&x, P(A3 + (uint64_t)i * 8), 4); memcpy(&y, P(A3 + (uint64_t)i * 8 + 4), 4);
        int xi = (int)(x * 16.0f), yi = (int)(y * 16.0f);
        wr8(A0 + 8 + A1 + (uint64_t)i, (uint8_t)((xi < 0 ? 0 : xi > 15 ? 15 : xi) | (yi < 0 ? 0 : yi > 15 ? 15 : yi) << 4));
    }
}
static void ms_get_enable(Ctx *c) { RET(rd8(A0)); }
static void ms_get_a2c(Ctx *c) { RET(rd8(A0 + 1)); }
static void ms_get_a2c_dither(Ctx *c) { RET(rd8(A0 + 2)); }
static void ms_get_samples(Ctx *c) { RET(rd8(A0 + 4)); }
static void ms_get_raster_samples(Ctx *c) { RET(rd8(A0 + 5)); }
static void ms_get_c2c(Ctx *c) { RET(rd8(A0 + 3) & 1); }
static void ms_get_locations_on(Ctx *c) { RET((rd8(A0 + 3) >> 1) & 1); }
static void ms_get_grid_on(Ctx *c) { RET((rd8(A0 + 3) >> 2) & 1); }
static void ms_get_covmod(Ctx *c) { RET((rd8(A0 + 3) >> 3) & 3); }
static void ms_get_c2c_output(Ctx *c) { RET(rd8(A0 + 3) >> 5); }
static void job_grid(void *arg) { GLint *wh = arg; wh[0] = wh[1] = 1; glGetIntegerv(0x933E /* SAMPLE_LOCATION_PIXEL_GRID_WIDTH_NV */, &wh[0]); glGetIntegerv(0x933F, &wh[1]); while (glGetError()) {} }
static void ms_get_grid(Ctx *c) {                       /* (state, int *width, int *height): the grid this GPU repeats sample locations over */
    GLint wh[2] = {1, 1};
    if ((rd8(A0 + 3) >> 2) & 1) gl_run(g_rb->sample_grid, wh);
    if (A1) wr32(A1, (uint32_t)wh[0]);
    if (A2) wr32(A2, (uint32_t)wh[1]);
}

/* ---- the other small state objects: getters of what the setters stored */
/* blend state (8 bytes): +0 bits 0..2 target, 3..4 advanced overlap, 5 premultiplied source, 6 normalised destination;
 * +1..+4 functions, +5 +6 equations, +7 advanced mode */
static void blend_defaults(Ctx *c) { static const uint8_t d[8] = {0x20, 2, 1, 2, 1, 1, 1, 0}; memcpy(P(A0), d, 8); }
static void blend_target(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~7) | (A1 & 7))); }
static void blend_adv_mode(Ctx *c) { wr8(A0 + 7, (uint8_t)A1); }
static void blend_adv_overlap(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x18) | ((A1 & 3) << 3))); }
static void blend_adv_premult(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x20) | ((A1 & 1) << 5))); }
static void blend_adv_normdst(Ctx *c) { wr8(A0, (uint8_t)((rd8(A0) & ~0x40) | ((A1 & 1) << 6))); }
static void blend_get_target(Ctx *c) { RET(rd8(A0) & 7); }
static void blend_get_func(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], rd8(A0 + 1 + (uint64_t)i)); }
static void blend_get_equation(Ctx *c) { if (A1) wr32(A1, rd8(A0 + 5)); if (A2) wr32(A2, rd8(A0 + 6)); }
static void blend_get_adv_mode(Ctx *c) { RET(rd8(A0 + 7)); }
static void blend_get_adv_overlap(Ctx *c) { RET((rd8(A0) >> 3) & 3); }
static void blend_get_adv_premult(Ctx *c) { RET((rd8(A0) >> 5) & 1); }
static void blend_get_adv_normdst(Ctx *c) { RET((rd8(A0) >> 6) & 1); }
static void color_get_blend_enable(Ctx *c) { RET((rd8(A0) >> (A1 & 7)) & 1); }
static void color_get_logic_op(Ctx *c) { RET(rd8(A0 + 1)); }
static void color_get_alpha_test(Ctx *c) { RET(rd8(A0 + 2)); }
static void chanmask_get(Ctx *c) {                      /* (state, index, NVNboolean *r, *g, *b, *a) */
    uint32_t m = rd32(A0) >> (((uint32_t)A1 & 7) * 4);
    for (int i = 0; i < 4; i++) if (c->x[2 + i]) wr8(c->x[2 + i], (uint8_t)((m >> i) & 1));
}
static void ds_get_stencil_func(Ctx *c) { RET((rd64(A0) >> (A1 & 2 && !(A1 & 1) ? 12 : 8)) & 15); }       /* (state, face) */
static void ds_get_stencil_op(Ctx *c) {                 /* (state, face, *stencilFail, *depthFail, *depthPass) */
    uint64_t v = rd64(A0) >> (A1 & 2 && !(A1 & 1) ? 28 : 16);
    for (int i = 0; i < 3; i++) if (c->x[2 + i]) wr32(c->x[2 + i], (uint32_t)((v >> (i * 4)) & 15));
}
static void vattr_get_format(Ctx *c) { if (A1) wr32(A1, rd32(A0) & 0xff); if (A2) wr64(A2, rd32(A0) >> 14); }
static void vattr_get_stream(Ctx *c) { RET((rd32(A0) >> 8) & 31); }
static void vstream_get_stride(Ctx *c) { RET(rd32(A0)); }
static void vstream_get_divisor(Ctx *c) { RET(rd32(A0 + 4)); }

/* ---- samplers: NVNsamplerBuilder and NVNsampler both hold a SamplerParams */
static void smpb_border_i(Ctx *c) { if (A1) { memcpy(SMP->border, P(A1), 16); SMP->border_kind = strstr(hostfn_current_name(), "ui") ? 1 : 2; } }
static void smpb_border_f(Ctx *c) { if (A1) { memcpy(SMP->border, P(A1), 16); SMP->border_kind = 0; } }
static void smpb_lod_snap(Ctx *c) { SMP->lod_snap = F0; }
static void smp_get_minmag(Ctx *c) { if (A1) wr32(A1, SMP->min); if (A2) wr32(A2, SMP->mag); }
static void smp_get_wrap(Ctx *c) { if (A1) wr32(A1, SMP->ws); if (A2) wr32(A2, SMP->wt); if (A3) wr32(A3, SMP->wr); }
static void smp_get_lodclamp(Ctx *c) { if (A1) memcpy(P(A1), &SMP->lodmin, 4); if (A2) memcpy(P(A2), &SMP->lodmax, 4); }
static void smp_get_lodbias(Ctx *c) { F0 = SMP->bias; }
static void smp_get_compare(Ctx *c) { if (A1) wr32(A1, SMP->cmode); if (A2) wr32(A2, SMP->cfunc); }
static void smp_get_border(Ctx *c) { if (A1) memcpy(P(A1), SMP->border, 16); }
static void smp_get_aniso(Ctx *c) { F0 = SMP->aniso; }
static void smp_get_reduction(Ctx *c) { RET(SMP->reduction); }
static void smp_get_lod_snap(Ctx *c) { F0 = SMP->lod_snap; }
static void smp_compare(Ctx *c) { RET(A0 && A1 && !memcmp(P(A0), P(A1), sizeof(SamplerParams))); }
static void smp_finalize(Ctx *c) { nvn_label_drop(A0); }      /* the parameters live in the guest object; OpenGL sampler objects are shared by value */
static void spool_finalize(Ctx *c) {
    gl_barrier();
    SamplerPool *sp = hobj(A0, K_SPOOL);
    if (!sp) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    sp->count = 0;                                      /* the record itself stays: recorded SetSamplerPool commands may still name it */
    ReleaseSRWLockExclusive(&g_pool_lock);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
}
static void spool_get_pool(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->mempool : 0); }
static void spool_get_offset(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->offset : 0); }
static void spool_get_size(Ctx *c) { SamplerPool *sp = hobj(A0, K_SPOOL); RET(sp ? sp->count : 0); }

/* ---- textures */
static void texb_get_swizzle(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], TB->swz[i]); }
static void texb_get_dsmode(Ctx *c) { RET(TB->dsmode); }
static void texb_get_pool(Ctx *c) { RET(TB->pool); }
static void texb_get_offset(Ctx *c) { RET(TB->offset); }
static void texb_get_packaged(Ctx *c) { RET(TB->packaged); }
static void texb_get_layout(Ctx *c) { if (A1) memcpy(P(A1), TB->layout, 8); }
static void texb_get_stride(Ctx *c) { RET(TB->stride); }
static void texb_set_glname(Ctx *c) { TB->glname = (uint32_t)A1; }
static void texb_get_glname(Ctx *c) { RET(TB->glname); }
static void tex_get_swizzle(Ctx *c) { Tex *t = hobj(A0, K_TEX); for (int i = 0; t && i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], t->b.swz[i]); }
TEXGET(tex_get_pool, t->b.pool)
static void tex_compare(Ctx *c) {                       /* two textures are the same when they describe the same storage the same way */
    Tex *a = hobj(A0, K_TEX), *b = hobj(A1, K_TEX);
    RET(a && b && (a == b || !memcmp(&a->b, &b->b, sizeof a->b)));
}
static void tex_view_offset(Ctx *c) {                   /* (texture, view): where the view's first level and layer start in the texture's storage */
    Tex *t = hobj(A0, K_TEX);
    View v;
    TexLayout L;
    if (!t || !A1 || (t->b.flags & 0x10)) { RET(0); return; }
    memcpy(&v, P(A1), sizeof v);
    tex_lay(&t->b, &L);
    int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
    if (level < 0 || level >= L.levels || layer < 0 || layer >= L.layers) { RET(0); return; }
    RET((uint64_t)(tex_level_ptr(&L, (const uint8_t *)0x1000, level, layer) - (const uint8_t *)0x1000));
}
/* ReadJob: nvn_front.h (V1 session 5: the Vulkan back end reads texels too) */
/* nvnTextureReadTexels: texels of a region as tightly packed rows (or with the given strides). nvnTextureInvalidateTexels: what
 * the GPU rendered or copied into the texture becomes readable in its pool memory, in the texture's own (block-linear) layout. */
static void job_read_texels(void *arg) {
    ReadJob *j = arg;
    Tex *t = j->tex;
    Fmt f;
    static uint8_t *tmp;
    static size_t tmp_cap;
    static int nlog;
    if (!fmt_get(t->b.format, &f)) return;
    tex_sync(t);
    while (glGetError()) {}
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    int level = j->has_view && (j->view.set & 0x81) == 0x81 ? j->view.lvl0 : 0, layer0 = j->has_view && (j->view.set & 0x82) == 0x82 ? j->view.lay0 : 0;
    GLenum fo = f.kind == 1 ? 0x1902 : f.kind == 2 ? GL_STENCIL_INDEX_ : f.format;
    if (!j->to_pool) {
        const int *r = j->region;
        int d = r[5] > 0 ? r[5] : 1, z = t->b.target == 2 ? r[2] : layer0 + r[2];
        size_t rows = (size_t)(r[4] + f.bh - 1) / f.bh, rowb = j->row_stride ? (size_t)j->row_stride : ((size_t)(r[3] + f.bw - 1) / f.bw) * f.bpb;
        size_t bytes = (j->image_stride ? (size_t)j->image_stride : rowb * rows) * (size_t)d;
        if (j->row_stride && f.kind != 4) glPixelStorei(0x0D02 /* PACK_ROW_LENGTH */, (GLint)(j->row_stride / f.bpb));
        if (j->image_stride && j->row_stride && f.kind != 4) glPixelStorei(0x806C /* PACK_IMAGE_HEIGHT */, (GLint)(j->image_stride / j->row_stride));
        if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, (GLsizei)bytes, j->out);
        else glGetTextureSubImage(t->o.gl, level, r[0], r[1], z, r[3], r[4], d, fo, f.type, (GLsizei)bytes, j->out);
        glPixelStorei(0x0D02, 0); glPixelStorei(0x806C, 0);
    } else if (t->mem && t->gpu_written && !t->b.packaged) {
        const TexFormat *tf = tex_format(t->b.format);
        TexLayout L;
        if (tf && tf->sw_decode) { logf_limited(&nlog, 5, "nvn_gl: InvalidateTexels on a texture of format 0x%x that is kept decoded: its pool memory is not rewritten\n", t->b.format); return; }
        if (t->b.samples > 1) { logf_limited(&nlog, 5, "nvn_gl: InvalidateTexels on a multisample texture: its pool memory is not rewritten\n"); return; }
        if (t->b.flags & 0x10) {
            int64_t stride = t->b.stride ? t->b.stride : (int64_t)t->b.width * f.bpb;
            if (f.kind == 4 || t->mem + stride * t->b.height > t->limit) return;
            glPixelStorei(0x0D02, (GLint)(stride / f.bpb));
            glGetTextureSubImage(t->o.gl, 0, 0, 0, 0, t->b.width, t->b.height, 1, fo, f.type, (GLsizei)(stride * t->b.height), t->mem);
            glPixelStorei(0x0D02, 0);
        } else {
            tex_lay(&t->b, &L);
            if (level >= L.levels || level >= t->glevels) return;
            for (int layer = 0; layer < L.layers; layer++) {
                uint8_t *dst = (uint8_t *)(uintptr_t)tex_level_ptr(&L, t->mem, level, layer);
                size_t lin = L.lvl_linear_size[level];
                if (t->limit && dst + L.lvl_size[level] > t->limit) return;
                if (lin > tmp_cap) { tmp_cap = lin * 2; tmp = realloc(tmp, tmp_cap); }
                int is3d = t->b.target == 2;
                if (f.kind == 4) glGetCompressedTextureSubImage(t->o.gl, level, 0, 0, is3d ? 0 : layer, L.lvl_w[level], L.lvl_h[level], is3d ? L.lvl_d[level] : 1, (GLsizei)lin, tmp);
                else glGetTextureSubImage(t->o.gl, level, 0, 0, is3d ? 0 : layer, L.lvl_w[level], L.lvl_h[level], is3d ? L.lvl_d[level] : 1, fo, f.type, (GLsizei)lin, tmp);
                tex_swizzle_level(&L, level, tmp, dst);
            }
        }
        t->hash = mem_hash(t->mem, t->data_size, t->ww_hashed || (t->b.flags & 0x10) != 0);
    }
    GLenum e = glGetError();
    if (e) logf_limited(&nlog, 10, "nvn_gl: reading texels of texture %dx%dx%d format 0x%x failed: GL error 0x%x\n", t->b.width, t->b.height, t->b.depth, t->b.format, e);
}
static void tex_read_texels(Ctx *c) {                   /* (texture, view, region, out) [, rowStride, imageStride] */
    ReadJob j;
    memset(&j, 0, sizeof j);
    j.tex = hobj(A0, K_TEX); j.out = P(A3);
    if (!j.tex || !A2 || !A3) return;
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    memcpy(j.region, P(A2), 24);
    if (strstr(hostfn_current_name(), "Strided")) { j.row_stride = (int64_t)A4; j.image_stride = (int64_t)A5; }
    gl_run(g_rb->read_texels, &j);
}
static void tex_invalidate_texels(Ctx *c) {             /* (texture, view, region) */
    ReadJob j;
    memset(&j, 0, sizeof j);
    j.tex = hobj(A0, K_TEX); j.to_pool = 1;
    if (!j.tex) return;
    if (A1) { memcpy(&j.view, P(A1), sizeof j.view); j.has_view = 1; }
    gl_run(g_rb->read_texels, &j);
}
static void view_get_levels(Ctx *c) { if (A1) wr32(A1, (uint32_t)TV->lvl0); if (A2) wr32(A2, (uint32_t)TV->nlvl); RET((TV->set & 0x81) == 0x81); }
static void view_get_layers(Ctx *c) { if (A1) wr32(A1, (uint32_t)TV->lay0); if (A2) wr32(A2, (uint32_t)TV->nlay); RET((TV->set & 0x82) == 0x82); }
static void view_get_format(Ctx *c) { if (A1) wr32(A1, TV->format); RET((TV->set & 0x84) == 0x84); }
static void view_get_swizzle(Ctx *c) { for (int i = 0; i < 4; i++) if (c->x[1 + i]) wr32(c->x[1 + i], TV->swz[i]); RET((TV->set & 0x88) == 0x88); }
static void view_get_dsmode(Ctx *c) { if (A1) wr32(A1, TV->dsmode); RET((TV->set & 0x90) == 0x90); }
static void view_get_target(Ctx *c) { if (A1) wr32(A1, TV->target); RET((TV->set & 0xa0) == 0xa0); }
static void view_compare(Ctx *c) { RET(A0 && A1 && !memcmp(P(A0), P(A1), sizeof(View))); }
static void tpool_finalize(Ctx *c) {
    gl_barrier();
    TexPool *tp = hobj(A0, K_TPOOL);
    if (!tp) return;
    AcquireSRWLockExclusive(&g_pool_lock);
    tp->count = 0;                                      /* the record itself stays: recorded SetTexturePool commands may still name it */
    ReleaseSRWLockExclusive(&g_pool_lock);
    wr32(A0 + 8, 0);
    nvn_label_drop(A0);
}
static void tpool_get_pool(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->mempool : 0); }
static void tpool_get_offset(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->offset : 0); }
static void tpool_get_size(Ctx *c) { TexPool *tp = hobj(A0, K_TPOOL); RET(tp ? tp->count : 0); }

/* ---- programs */
static void prog_sample_shading(Ctx *c) { Prog *p = hobj(A0, K_PROG); if (p) p->sample_shading = (int)A1; }    /* 0 as the shader says, 1 forced on, 2 forced off */

/* ---- window */
static void wb_defaults(Ctx *c) { memset(P(A0), 0, 0x40); wr8(A0 + 4, 1); }
static void wb_native(Ctx *c) { wr64(A0 + 56, A1); }
static void wb_interval(Ctx *c) { wr8(A0 + 4, (uint8_t)A1); }
static void wb_num_active(Ctx *c) { wr8(A0 + 5, (uint8_t)A1); }
static void wb_get_native(Ctx *c) { RET(rd64(A0 + 56)); }
static void wb_get_interval(Ctx *c) { RET(rd8(A0 + 4)); }
static void wb_get_num_textures(Ctx *c) { RET(rd32(A0) & 0xff); }
static void wb_get_num_active(Ctx *c) { RET(rd8(A0 + 5) ? rd8(A0 + 5) : (rd32(A0) & 0xff)); }
static void wb_get_texture(Ctx *c) { RET((uint32_t)A1 < 6 ? rd64(A0 + 8 + (A1 & 7) * 8) : 0); }
static void win_finalize(Ctx *c) { gl_barrier(); Window *w = hobj(A0, K_WIN); if (w) { wr32(A0 + 8, 0); w->count = 0; } nvn_label_drop(A0); }
static void win_set_interval(Ctx *c) { Window *w = hobj(A0, K_WIN); if (w) { w->interval = (int)A1; display_set_interval((int)A1); } }
static void win_get_interval(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->interval : 1); }
static void win_get_native(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->native : 0); }
static void win_get_num_textures(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? w->count : 0); }
static void win_get_num_active(Ctx *c) { Window *w = hobj(A0, K_WIN); RET(w ? (w->nactive ? w->nactive : w->count) : 0); }
static void win_set_num_active(Ctx *c) { Window *w = hobj(A0, K_WIN); if (w && (int)A1 >= 1 && (int)A1 <= w->count) w->nactive = (int)A1; }
static void win_get_crop(Ctx *c) {                      /* (window, NVNrectangle *): the crop set, or the whole texture */
    Window *w = hobj(A0, K_WIN);
    if (!w || !A1) return;
    if (w->crop[2] > 0 && w->crop[3] > 0) memcpy(P(A1), w->crop, 16);
    else { int r[4] = {0, 0, w->count && w->tex[0] ? w->tex[0]->b.width : 0, w->count && w->tex[0] ? w->tex[0]->b.height : 0}; memcpy(P(A1), r, 16); }
}

/* ---- command buffers: state kept while recording */
static void cmd_set_callback(Ctx *c) { cb_get(A0)->callback = A1; }
static void cmd_set_callback_data(Ctx *c) { cb_get(A0)->callback_data = A1; }
static void cmd_get_callback(Ctx *c) { RET(cb_get(A0)->callback); }
static void cmd_get_callback_data(Ctx *c) { RET(cb_get(A0)->callback_data); }
static void cmd_callback_enabled(Ctx *c) { cb_get(A0)->callback_off = !(A1 & 1); }
static void cmd_get_command_used(Ctx *c) { RET(cb_get(A0)->cmd_used); }
static void cmd_get_control_used(Ctx *c) { RET(cb_get(A0)->ctrl_used); }
static void cmd_get_command_free(Ctx *c) { CmdBuf *cb = cb_get(A0); RET(cb->cmd_size > cb->cmd_used ? cb->cmd_size - cb->cmd_used : 0); }
static void cmd_get_control_free(Ctx *c) { CmdBuf *cb = cb_get(A0); RET(cb->ctrl_size > cb->ctrl_used ? cb->ctrl_size - cb->ctrl_used : 0); }
static void cmd_get_row_stride(Ctx *c) { RET(cb_get(A0)->row_stride); }
static void cmd_get_image_stride(Ctx *c) { RET(cb_get(A0)->image_stride); }
static void cmd_image_stride(Ctx *c) { cb_get(A0)->image_stride = (int64_t)A1; rec(c, OP_IMAGESTRIDE, 0); }
static void cmd_depth_bounds(Ctx *c) { float v[2] = {F0, F1}; Cmd *m = rec(c, OP_DEPTHBOUNDS, 0); memcpy(&m->a[1], v, 8); }       /* (cmdbuf, enable, near [v0], far [v1]) */
static void cmd_cons_raster(Ctx *c) { rec(c, OP_CONSRASTER, 0); }
static void cmd_cons_dilate(Ctx *c) { float v = F0; Cmd *m = rec(c, OP_CONSDILATE, 0); memcpy(&m->a[0], &v, 4); }
static void cmd_subpixel_bias(Ctx *c) { rec(c, OP_SUBPIXEL, 0); }
static void cmd_viewport_swizzles(Ctx *c) { int n = (int)A2; uint64_t src = A3; Cmd *m = rec(c, OP_VPSWIZZLE, (size_t)n * 16); if (n > 0 && src) memcpy(BLOB(m), P(src), (size_t)n * 16); }
static void cmd_tess_levels(Ctx *c) {                   /* (cmdbuf, const float *levels): 2 inner or 4 outer */
    int outer = strstr(hostfn_current_name(), "Outer") != NULL;
    uint64_t src = A1;
    Cmd *m = rec(c, OP_TESSLEVELS, 16);
    m->a[0] = (uint64_t)outer;
    memset(BLOB(m), 0, 16);
    if (src) memcpy(BLOB(m), P(src), outer ? 16 : 8);
}
static void cmd_barrier(Ctx *c) { rec(c, OP_BARRIER, 0); }
static void cmd_discard_color(Ctx *c) { rec(c, OP_DISCARD, 0); }
static void cmd_discard_ds(Ctx *c) { rec(c, OP_DISCARD, 0)->a[0] = ~0ull; }
static void cmd_clear_color_typed(Ctx *c) {             /* ClearColori / ClearColorui (cmdbuf, index, const int *, mask) */
    uint64_t src = A2;
    int kind = strstr(hostfn_current_name(), "ui") ? 2 : 1;
    Cmd *m = rec(c, OP_CLEARCOLOR, 16);
    m->a[5] = (uint64_t)kind;
    if (src) memcpy(BLOB(m), P(src), 16); else memset(BLOB(m), 0, 16);
}
static void cmd_clear_texture(Ctx *c) {                 /* (cmdbuf, texture, view, region, color, mask); the i / ui variants pass integers */
    uint64_t view = A2, region = A3, color = A4, mask = A5;
    const char *name = hostfn_current_name();
    int kind = strstr(name, "Textureui") ? 2 : strstr(name, "Texturei") ? 1 : 0;
    Tex *t = hobj(A1, K_TEX);
    Cmd *m = rec(c, OP_CLEARTEX, 64);
    m->a[0] = (uint64_t)(uintptr_t)t; m->a[4] = mask; m->a[5] = (uint64_t)kind;
    memset(BLOB(m), 0, 64);
    if (view) memcpy(BLOB(m), P(view), sizeof(View));
    if (region) memcpy(BLOB(m) + 24, P(region), 24); else m->a[6] = 1;
    if (color) memcpy(BLOB(m) + 48, P(color), 16);
}
static void cmd_downsample_view(Ctx *c) {               /* (cmdbuf, src, srcView, dst, dstView) */
    uint64_t sv = A2, dv = A4;
    Tex *s = hobj(A1, K_TEX), *d = hobj(A3, K_TEX);
    Cmd *m = rec(c, OP_DOWNSAMPLE_VIEW, 48);
    m->a[0] = (uint64_t)(uintptr_t)s; m->a[1] = (uint64_t)(uintptr_t)d;
    memset(BLOB(m), 0, 48);
    if (sv) memcpy(BLOB(m), P(sv), sizeof(View));
    if (dv) memcpy(BLOB(m) + 24, P(dv), sizeof(View));
}
static void cmd_draw_arrays_indirect(Ctx *c) { Cmd *m = rec(c, OP_DRAW_INDIRECT, 0); m->a[6] = 0; m->a[4] = 1; m->a[5] = 0; m->a[3] = m->a[1]; m->a[1] = 0; m->a[2] = 0; }      /* (cmdbuf, mode, indirect) */
static void cmd_draw_elements_indirect(Ctx *c) { Cmd *m = rec(c, OP_DRAW_INDIRECT, 0); m->a[6] = 1; m->a[4] = 1; m->a[5] = 0; }   /* (cmdbuf, mode, type, indexBuffer, indirect) */
static void cmd_multi_draw_arrays_indirect(Ctx *c) {    /* (cmdbuf, mode, indirect, parameter, maxDrawCount, stride) */
    uint64_t indirect = A2, param = A3, maxn = A4, stride = A5;
    Cmd *m = rec(c, OP_DRAW_INDIRECT, 8);
    m->a[1] = 0; m->a[2] = 0; m->a[3] = indirect; m->a[4] = maxn; m->a[5] = stride; m->a[6] = 2;
    memcpy(BLOB(m), &param, 8);
}
static void cmd_multi_draw_elements_indirect(Ctx *c) {  /* (cmdbuf, mode, type, indexBuffer, indirect, parameter, maxDrawCount, stride) */
    uint64_t indirect = A4, param = A5, maxn = A6, stride = A7, index = A3;
    Cmd *m = rec(c, OP_DRAW_INDIRECT, 8);
    m->a[2] = index; m->a[3] = indirect; m->a[4] = maxn; m->a[5] = stride; m->a[6] = 3;
    memcpy(BLOB(m), &param, 8);
}
static void cmd_dispatch_indirect(Ctx *c) { cb_get(A0)->dispatches++; rec(c, OP_DISPATCH_INDIRECT, 0); }       /* (cmdbuf, address of three group counts) */
static void cmd_render_enable(Ctx *c) { Cmd *m = rec(c, OP_RENDER_ENABLE, 0); m->a[1] = 0; m->a[2] = 0; }                      /* (cmdbuf, enable) */
static void cmd_render_enable_cond(Ctx *c) { Cmd *m = rec(c, OP_RENDER_ENABLE, 0); m->a[2] = m->a[1]; m->a[1] = m->a[0] + 1; }   /* (cmdbuf, mode, counter address) */
static void cmd_reset_counter(Ctx *c) { rec(c, OP_COUNTER_RESET, 0); }
static void cmd_report_counter(Ctx *c) { rec(c, OP_COUNTER_REPORT, 0); }
static void cmd_report_value(Ctx *c) { rec(c, OP_REPORT_VALUE, 0); }
static void cmd_fence_sync(Ctx *c) { SyncObj *o = hobj(A1, K_SYNC); rec(c, OP_FENCE, 0)->a[0] = (uint64_t)(uintptr_t)o; }      /* (cmdbuf, sync, condition, flags) */
static void cmd_wait_sync(Ctx *c) { SyncObj *o = hobj(A1, K_SYNC); rec(c, OP_WAITSYNC, 0)->a[0] = (uint64_t)(uintptr_t)o; }
static void cmd_signal_event(Ctx *c) { uint64_t a = A1 ? rd64(A1 + 16) : 0; rec(c, OP_EVENT_SIGNAL, 0)->a[0] = a; }            /* (cmdbuf, event, mode, location, flags, value) */
static void cmd_wait_event(Ctx *c) { uint64_t a = A1 ? rd64(A1 + 16) : 0; rec(c, OP_EVENT_WAIT, 0)->a[0] = a; }                /* (cmdbuf, event, waitMode, value) */
/* nvnCommandBufferDrawTexture(cmdbuf, textureHandle, dstRegion, srcRegion) [sdk 0x438210]: see nvn_c4.c. The handle is the one
 * nvnDeviceGetTextureHandle gives (texture ID | sampler ID << 20, bit 32 set). An empty destination records nothing, as the
 * real encoder writes nothing [0x43827c]. */
static void cmd_draw_texture(Ctx *c) {
    uint64_t h = A1, dst = A2, src = A3;
    float d[4], s[4];
    NvnC4DrawTexture w;
    memcpy(d, P(dst), 16); memcpy(s, P(src), 16);
    if (!nvn_c4_draw_texture_words(d, s, &w)) return;
    w.src_texture = (uint32_t)h & 0xfffff; w.src_sampler = (uint32_t)h >> 20;   /* [sdk 0x4382f0, 0x4382f4] */
    Cmd *m = rec(c, OP_DRAW_TEXTURE, sizeof w);
    m->a[0] = h;
    memcpy(BLOB(m), &w, sizeof w);
}
static void cmd_covmod_table(Ctx *c) { uint64_t src = A1; Cmd *m = rec(c, OP_COVMODTABLE, 64); m->a[0] = src != 0; if (src) memcpy(BLOB(m), P(src), 64); }
static void rec_debug(Ctx *c, int kind, uint64_t id, uint64_t text) {
    size_t n = text ? strnlen(P(text), 255) : 0;
    Cmd *m = rec(c, OP_DEBUG, n + 1);
    m->a[0] = (uint64_t)kind; m->a[1] = id;
    if (n) memcpy(BLOB(m), P(text), n);
    BLOB(m)[n] = 0;
}
static void cmd_push_group(Ctx *c) { rec_debug(c, 0, A1, A2); }               /* (cmdbuf, domain, description) */
static void cmd_pop_group(Ctx *c) { rec_debug(c, 1, 0, 0); }
static void cmd_marker(Ctx *c) { rec_debug(c, 2, 0, A1); }                    /* (cmdbuf, description) */
static void cmd_marker_id(Ctx *c) { rec_debug(c, 2, A1, A2); }                /* (cmdbuf, domain, description) */
static void cmd_set_label(Ctx *c) { cb_get(A0); any_set_label(c); }

static void job_finalize_handle(void *arg) {
    uint64_t h = *(uint64_t *)arg;
    uint32_t i = (uint32_t)h - 1;
    AcquireSRWLockExclusive(&g_list_lock);
    if (h && i < (uint32_t)g_list_count && g_lists[i].data && (g_lists[i].gen & 0x7fffffff) == (uint32_t)(h >> 32)) g_lists[i].finalized = 1;
    ReleaseSRWLockExclusive(&g_list_lock);
}
static void dev_finalize_command_handle(Ctx *c) {       /* (device, handle): the recorded commands of that handle are released (by the render thread, at the next present) */
    uint64_t h = A1;
    { int r1_ra_phase(void); if (r1_ra_phase() == 1) return; }      /* R1 round 5, run-ahead: a speculative pass's release is undone by the restore (the real pass asks again); its own lists expire unused */
    if (gl_async_on() && g_async_pending) gl_post(job_finalize_handle, &h, sizeof h);     /* after the submits already queued, which may still use it */
    else job_finalize_handle(&h);
}
/* ================================================================ V1 step S0: the render thread's jobs in Vulkan mode
 * (plan\phases\V1_vulkan.md, section 1). Every job the entry points above hand to the render thread arrives here when
 * SSBU_RENDERER=vk. A job nvn_vk.c implements is done there; one it does not implement yet stops the game with the name
 * of the NVN operation (nvn_vk_missing; SSBU_VK_MISSING=warn: reported once, counted, skipped). A job that is not one
 * of this file's (nvn_vk.c's own, through nvn_render_run) runs as it is. Step S2 replaces this table by the RBackend
 * interface. */
void nvn_render_run(void (*fn)(void *), void *arg) { gl_run(fn, arg); }
/* PR #41: pool / buffer finalization may be followed at once by a decommit or reuse of the guest's storage. A queue barrier
 * alone is not enough on Vulkan: kept compute buffers and asynchronous landings outlive the jobs that used them. */
void nvn_front_memory_retire(uint64_t addr, uint64_t size) {
    if (!addr || !size || !g_render_tid) return;
    if (g_rb == &rb_vk) { Range range = {addr, size}; gl_run(nvn_vk_memory_retire, &range); }
    else gl_barrier();
}
/* Seam step S2: the OpenGL back end as an RBackend (nvn_front.h); rb_vk is in nvn_vk_draw.c. */
static void rb_gl_idle(void) { gl_housekeeping(); }
const RBackend rb_gl = {"gl", job_submit, job_submit_async, job_present, job_present_movie, job_fence, job_sync_wait, job_server_wait, job_sync_drop, job_flush, job_finish,
                        job_write_texels, job_read_texels, job_housekeeping, job_reset_status, job_grid, rb_gl_idle};

/* ---- front-end services for the Vulkan back end (nvn_front.h). Until step S3 splits this file they are thin doors to
 * the statics above; nothing here calls OpenGL. Render thread only, except where the callee has its own lock. */
int nvn_front_fmt(int nvn_format, Fmt *f) { return fmt_get(nvn_format, f); }
uint8_t *nvn_front_list(uint64_t handle, size_t *len) { return list_lookup(handle, len); }
/* Track V session 4, step-aligned comparison of the two renderers: with SSBU_SHOT_STEPS=1 the scripted input (nnhid.c)
 * asks for a screenshot when a key step of SSBU_SEQ begins; the next presented frame is saved as step_<n>.bmp in
 * SSBU_SHOT_DIR by whichever back end runs. Counted in input polls, so both renderers save the same game frame. */
static volatile LONG g_step_shot;
void nvn_front_shot_step(int step) { static int on = -1; if (on < 0) on = getenv("SSBU_SHOT_STEPS") != NULL; if (on) InterlockedExchange(&g_step_shot, step + 1); }
int nvn_front_take_step_shot(void) { return g_step_shot ? (int)InterlockedExchange(&g_step_shot, 0) : 0; }
void nvn_front_batch_begin(void) { g_serial++; }
void nvn_front_barrier_texmem(void) { g_serial++; }
int nvn_front_tex_layers(const Tex *t) { return tex_layers(&t->b); }
int nvn_front_tex_levels(const Tex *t) {
    const TexBuilder *b = &t->b;
    int w = b->width > 0 ? b->width : 1, h = b->height > 0 ? b->height : 1, d = b->depth > 0 ? b->depth : 1, levels = b->levels > 0 ? b->levels : 1, maxdim = w > h ? w : h, maxl = 1;
    if (b->target == 2 && d > maxdim) maxdim = d;
    while ((maxdim >> maxl) > 0) maxl++;
    return b->samples > 1 ? 1 : levels > maxl ? maxl : levels;
}
/* The decision of tex_sync without its OpenGL calls (the same rule, the same fields of Tex). */
int nvn_front_tex_changed(Tex *t) {
    Fmt f;
    if (t->checked == g_serial) return 0;
    t->checked = g_serial;
    const uint8_t *src = tex_source(t);
    if (t->gpu_written || !src || t->b.samples > 1 || !fmt_get(t->b.format, &f) || (f.kind >= 1 && f.kind <= 3)) return 0;
    if (!t->b.packaged && t->limit && src + t->data_size > t->limit) return 0;
    if (t->b.packaged && t->uploaded) return 0;
    LONG64 pf0 = pf_now();
    uint64_t newest = ww_newest(src, t->data_size);
    if (newest) {
        if (t->uploaded && !t->dirty && t->ww_hashed && newest <= t->ww_seen) { g_ww_clean++; pf_add(PF_TEXHASH, pf0); return 0; }
        t->ww_seen = newest;
        g_ww_dirty++;
    } else g_ww_unwatched++;
    uint64_t h = mem_hash(src, t->data_size, newest != 0 || (t->b.flags & 0x10) != 0);
    int same = t->uploaded && !t->dirty && h == t->hash && (!newest || t->ww_hashed);
    t->ww_hashed = newest != 0;
    pf_add(PF_TEXHASH, pf0); g_pf_hash_bytes += (LONG64)t->data_size;
    if (same) return 0;
    t->hash = h; t->uploaded = 1; t->dirty = 0;
    g_st.tex_uploads++;
    return 1;
}
/* tex_upload_memory with the texels handed to the caller instead of glTexSubImage. */
int nvn_front_tex_read(Tex *t, int decode, nvn_front_put_fn *put) {
    static uint8_t *tmp;
    static size_t tmp_cap;
    Fmt f;
    TexBuilder *b = &t->b;
    const uint8_t *src = tex_source(t);
    if (!src || !fmt_get(b->format, &f)) return 0;
    if (b->flags & 0x10) {                         /* linear: plain rows with a stride */
        int64_t stride = b->stride ? b->stride : (int64_t)b->width * f.bpb;
        if (f.kind == 4) return 0;
        put(t, 0, 0, b->width, b->height, 1, src, (size_t)stride * (size_t)b->height, 0, stride);
        g_st.tex_bytes += stride * b->height;
        tex_release_packaged(t);
        return 1;
    }
    TexLayout L;
    tex_lay(b, &L);
    int levels = nvn_front_tex_levels(t);
    for (int layer = 0; layer < L.layers; layer++) {
        for (int l = 0; l < levels && l < L.levels; l++) {
            const uint8_t *p = tex_level_ptr(&L, src, l, layer);
            size_t lin = L.lvl_linear_size[l], need = lin + (decode ? (size_t)L.lvl_w[l] * L.lvl_h[l] * 4 : 0);
            if (!b->packaged && t->limit && p + L.lvl_size[l] > t->limit) return 0;
            if (need > tmp_cap) { tmp_cap = need * 2; tmp = realloc(tmp, tmp_cap); }
            tex_deswizzle_level(&L, l, p, tmp);
            if (decode) {
                tex_decode_rgba8(b->format, tmp, L.lvl_w[l], L.lvl_h[l], tmp + lin);
                put(t, l, layer, L.lvl_w[l], L.lvl_h[l], 1, tmp + lin, (size_t)L.lvl_w[l] * L.lvl_h[l] * 4, 1, 0);
            } else put(t, l, layer, L.lvl_w[l], L.lvl_h[l], L.lvl_d[l], tmp, lin, 0, 0);
            g_st.tex_bytes += (LONG64)lin;
        }
    }
    tex_release_packaged(t);
    return 1;
}
int nvn_front_handle(TexPool *tp, SamplerPool *sp, uint64_t nh, Tex **t, View *v, SamplerParams *p) {
    uint32_t tid = (uint32_t)nh & 0xfffff, sid = (uint32_t)(nh >> 20) & 0xfff;
    int r = 0;
    *t = NULL;
    if (!nh || !tp) return 0;
    AcquireSRWLockShared(&g_pool_lock);
    if (tid < (uint32_t)tp->count) { *t = tp->e[tid].tex; *v = tp->e[tid].view; }
    if (sp && sid < (uint32_t)sp->count && sp->used[sid]) { *p = sp->e[sid]; r |= 2; }
    ReleaseSRWLockShared(&g_pool_lock);
    if (*t && !(*t)->dead) r |= 1; else *t = NULL;
    return r;
}
long long nvn_front_frame_next(void) { return (long long)g_st.frames + 1; }
/* V1 session 5: compute outputs of the two back ends compared OFFLINE (tests/vk5_ccmp.py). SSBU_COMPUTE_CMP=<folder>: every
 * dispatch is named by a hash of what it reads from guest memory (machine code, group counts, every bound uniform buffer,
 * every bound storage buffer it does not write; of the written ones the size); when what it wrote has landed in guest memory
 * the written ranges go to <folder>/<hash>_<storage index>.bin (the first SSBU_COMPUTE_CMP_N distinct hashes, default 600).
 * Two runs (vk, gl) that met the same inputs wrote files of the same name. Render thread only. */
static struct { uint64_t h, addr, size; int n; } g_ccmp_pend[512];
static int g_ccmp_npend, g_ccmp_on = -1, g_ccmp_left;
static uint64_t g_ccmp_seen[4096];
static const char *g_ccmp_dir;
static uint64_t ccmp_hash(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ULL; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL;
    return h;
}
void nvn_front_ccmp_dispatch(const Range *ubo, const Range *ssbo, uint32_t written, const uint8_t *code, int code_len, unsigned gx, unsigned gy, unsigned gz) {
    if (g_ccmp_on < 0) { g_ccmp_dir = getenv("SSBU_COMPUTE_CMP"); g_ccmp_on = g_ccmp_dir != NULL; g_ccmp_left = getenv("SSBU_COMPUTE_CMP_N") ? atoi(getenv("SSBU_COMPUTE_CMP_N")) : 600; if (g_ccmp_on) CreateDirectoryA(g_ccmp_dir, NULL); }
    if (!g_ccmp_on || g_ccmp_left <= 0 || !code || code_len <= 0) return;
    uint32_t g[3] = {gx, gy, gz};
    uint64_t h = ccmp_hash(0xcbf29ce484222325ULL, code, (size_t)code_len);
    h = ccmp_hash(h, g, sizeof g);
    for (int n = 0; n < 16; n++) if (ubo[n].addr && ubo[n].size) { h = ccmp_hash(h ^ (uint64_t)(n + 1), P(ubo[n].addr), (size_t)ubo[n].size); }
    for (int n = 0; n < 32; n++) {
        if (!ssbo[n].addr || !ssbo[n].size) continue;
        /* SSBU_COMPUTE_CMP_MODE=1: storage buffers enter by SIZE only, written or not. Needed for skinning program #16: its
         * assembly text (OpenGL) declares storage 2 written, the translated machine code (Vulkan) 0, 1 and 2, so the
         * default naming never matches between the two. A false match (same sizes, other contents) shows as a difference. */
        static int mode = -1;
        if (mode < 0) mode = getenv("SSBU_COMPUTE_CMP_MODE") ? atoi(getenv("SSBU_COMPUTE_CMP_MODE")) : 0;
        if (mode == 1 || (written >> n & 1)) h = ccmp_hash(h ^ (uint64_t)(n + 101), &ssbo[n].size, 8);
        else h = ccmp_hash(h ^ (uint64_t)(n + 201), P(ssbo[n].addr), (size_t)ssbo[n].size);
    }
    unsigned slot = (unsigned)(h >> 17) & 4095;
    if (g_ccmp_seen[slot] == h) return;
    g_ccmp_seen[slot] = h;
    g_ccmp_left--;
    for (int n = 0; n < 32; n++) if ((written >> n & 1) && ssbo[n].addr && ssbo[n].size && g_ccmp_npend < 512) { g_ccmp_pend[g_ccmp_npend].h = h; g_ccmp_pend[g_ccmp_npend].addr = ssbo[n].addr; g_ccmp_pend[g_ccmp_npend].size = ssbo[n].size; g_ccmp_pend[g_ccmp_npend++].n = n; }
}
void nvn_front_ccmp_landed(void) {
    for (int i = 0; i < g_ccmp_npend; i++) {
        char path[400];
        snprintf(path, sizeof path, "%s/%016llx_%d.bin", g_ccmp_dir, (unsigned long long)g_ccmp_pend[i].h, g_ccmp_pend[i].n);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(P(g_ccmp_pend[i].addr), 1, (size_t)g_ccmp_pend[i].size, fp); fclose(fp); }
    }
    g_ccmp_npend = 0;
}
long long nvn_front_frame_end(long long draws, long long skipped, long long clears) {
    LONG64 frame = g_st.frames + 1;
    pf_frame_mark(frame);
    g_joblog_cur = frame;
    g_st.frames = frame;
    if (frame <= 3 || frame % 600 == 0 || draws != 0 && g_logn < 40 && (g_logn++, 1))
        printf("nvn_vk: frame %lld: %lld draws (%lld skipped), %lld clears | totals: %lld lists, %lld textures, %lld uploads (%lld MB)\n", (long long)frame, draws, skipped, clears,
               (long long)g_st.lists, (long long)g_st.tex_created, (long long)g_st.tex_uploads, (long long)(g_st.tex_bytes >> 20));
    {   static ULONGLONG t0;
        static LONG64 d0;
        if (frame % 300 == 0) {
            ULONGLONG now = GetTickCount64();
            if (t0 && now > t0) printf("nvn_vk: frames %lld..%lld: %.1f fps, %lld draws per frame\n", (long long)frame - 300, (long long)frame, 300000.0 / (double)(now - t0), (long long)(g_st.draws - d0) / 300);
            t0 = now; d0 = g_st.draws;
        }
    }
    fflush(stdout);
    g_prev_frame_draws = draws;
    input_frame_draws((int)draws);
    list_collect();
    return (long long)frame;
}
void nvn_front_take_dead(void (*drop)(int kind, void *host)) {
    if (!g_dead_n) return;
    EnterCriticalSection(&g_dead_lock);
    for (int i = 0; i < g_dead_n; i++) {
        drop(g_dead[i].kind, g_dead[i].p);
        if (g_dead[i].kind == K_TEX) tex_release_packaged(g_dead[i].p);
        if (g_dead[i].kind == K_PROG) { Prog *p = g_dead[i].p; for (int s = 0; s < 6; s++) { free(p->st[s].text); memset(&p->st[s], 0, sizeof p->st[s]); } }
    }
    g_dead_n = 0;
    LeaveCriticalSection(&g_dead_lock);
}
void nvn_front_count(int what, long long n) {
    switch (what) { case 0: g_st.draws += n; break; case 1: g_st.draws_skipped += n; break; case 2: g_st.clears += n; break; case 3: g_st.submits += n; break; case 5: g_st.tex_created += n; break; }
}
int nvn_front_clip_modes(void) { return g_origin_mode | g_depth_mode << 8; }
const char *nvn_front_op_name(unsigned op) { return k_op_name[op < OP_COUNT ? op : 0]; }
void *nvn_front_hwnd(void) { return g_hwnd; }
static void job_vk_shutdown(void *arg) { (void)arg; nvn_vk_shutdown(); }
/* At exit (nvn_gl_report): the render thread destroys the Vulkan objects. It is given 3 s: a render thread that is held
 * in a job at that moment is left alone (the process is ending) and that is said. */
static void vk_shutdown_at_exit(void) {
    static Job j;
    if (GetCurrentThreadId() == g_render_tid) { nvn_vk_shutdown(); return; }
    j.fn = job_vk_shutdown; j.arg = NULL; j.next = NULL; j.done = CreateEventA(NULL, FALSE, FALSE, NULL);
    EnterCriticalSection(&g_job_lock);
    if (g_job_tail) g_job_tail->next = &j; else g_job_head = &j;
    g_job_tail = &j;
    LeaveCriticalSection(&g_job_lock);
    SetEvent(g_job_event);
    /* Session 13: the event is set when the job has FINISHED, not when it is taken, and the job writes the pipeline cache
     * (0.3-0.7 s read-out + 1-2 s file for 0.9 GB, more on a busy PC): 3 s was too short and the old line ("did not take
     * the job") was printed while the shutdown was running; the process then left with the cache half written. Now: 60 s. */
    if (WaitForSingleObject(j.done, 3000) != WAIT_OBJECT_0) {
        LONG64 t0 = GetTickCount64();
        printf("nvn_vk: shutdown still running after 3 s (the pipeline cache is being written); waiting for it, at most 60 s\n"); fflush(stdout);
        if (WaitForSingleObject(j.done, 60000) != WAIT_OBJECT_0) printf("nvn_vk: shutdown NOT finished after 63 s: the render thread is held or never took the job\n");
        else printf("nvn_vk: shutdown finished %.1f s after the first 3 s\n", (double)(GetTickCount64() - t0) / 1000.0);
        fflush(stdout);
    }
}

/* nvnBufferFlushMappedRange / nvnMemoryPoolFlushMappedRange (nvn_stub.c): the CPU wrote this memory. Textures stored there are read again. */
Tex *nvn_front_tex_first(void) { return g_all_tex; }
void nvn_gl_memory_written(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t end = tex_range_end(addr, size);
    AcquireSRWLockShared(&g_tex_range_lock);
    if (g_tex_range_fallback) {
        for (Tex *t = g_all_tex; t; t = t->all_next)
            if (!t->dead && t->mem && (uint64_t)(uintptr_t)t->mem < end && addr < tex_range_end((uint64_t)(uintptr_t)t->mem, t->storage)) t->dirty = 1;
    } else {
        size_t lo = 0, hi = g_tex_range_count;
        while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (g_tex_ranges[mid].prefix_end <= addr) lo = mid + 1; else hi = mid; }
        for (size_t i = lo; i < g_tex_range_count && g_tex_ranges[i].begin < end; i++)
            if (g_tex_ranges[i].end > addr) g_tex_ranges[i].tex->dirty = 1;
    }
    ReleaseSRWLockShared(&g_tex_range_lock);
}

static const struct { const char *name; ImportFn fn; } k_nvn[] = {
    {"nvnDeviceSetWindowOriginMode", dev_origin_mode}, {"nvnDeviceSetDepthMode", dev_depth_mode}, {"nvnDeviceGetTextureHandle", dev_texture_handle},
    {"nvnDeviceGetTexelFetchHandle", dev_texel_fetch_handle}, {"nvnDeviceGetImageHandle", dev_image_handle},
    {"nvnProgramInitialize", prog_initialize}, {"nvnProgramFinalize", prog_finalize}, {"nvnProgramSetShaders", prog_set_shaders}, {"nvnProgramSetShadersExt", prog_set_shaders},
    {"nvnSamplerBuilderSetDefaults", smpb_defaults}, {"nvnSamplerBuilderSetMinMagFilter", smpb_minmag}, {"nvnSamplerBuilderSetWrapMode", smpb_wrap},
    {"nvnSamplerBuilderSetLodClamp", smpb_lodclamp}, {"nvnSamplerBuilderSetLodBias", smpb_lodbias}, {"nvnSamplerBuilderSetCompare", smpb_compare},
    {"nvnSamplerBuilderSetMaxAnisotropy", smpb_aniso}, {"nvnSamplerBuilderSetReductionFilter", smpb_reduction},
    {"nvnSamplerInitialize", smp_initialize},     {"nvnSamplerPoolInitialize", spool_initialize}, {"nvnSamplerPoolRegisterSampler", spool_register}, {"nvnSamplerPoolRegisterSamplerBuilder", spool_register},
        {"nvnTextureBuilderSetDefaults", texb_defaults}, {"nvnTextureBuilderSetFlags", texb_flags}, {"nvnTextureBuilderSetTarget", texb_target},
    {"nvnTextureBuilderSetFormat", texb_format}, {"nvnTextureBuilderSetWidth", texb_width}, {"nvnTextureBuilderSetHeight", texb_height}, {"nvnTextureBuilderSetDepth", texb_depth},
    {"nvnTextureBuilderSetSize1D", texb_size1d}, {"nvnTextureBuilderSetSize2D", texb_size2d}, {"nvnTextureBuilderSetSize3D", texb_size3d}, {"nvnTextureBuilderSetLevels", texb_levels},
    {"nvnTextureBuilderSetSamples", texb_samples}, {"nvnTextureBuilderSetSwizzle", texb_swizzle}, {"nvnTextureBuilderSetDepthStencilMode", texb_dsmode},
    {"nvnTextureBuilderSetStorage", texb_storage}, {"nvnTextureBuilderSetPackagedTextureData", texb_packaged}, {"nvnTextureBuilderSetPackagedTextureLayout", texb_layout},
    {"nvnTextureBuilderSetStride", texb_stride}, {"nvnTextureBuilderGetStorageSize", texb_storage_size}, {"nvnTextureBuilderGetStorageAlignment", texb_storage_align},
    {"nvnTextureBuilderGetFlags", texb_get_flags}, {"nvnTextureBuilderGetTarget", texb_get_target}, {"nvnTextureBuilderGetFormat", texb_get_format},
    {"nvnTextureBuilderGetWidth", texb_get_width}, {"nvnTextureBuilderGetHeight", texb_get_height}, {"nvnTextureBuilderGetDepth", texb_get_depth},
    {"nvnTextureBuilderGetLevels", texb_get_levels}, {"nvnTextureBuilderGetSamples", texb_get_samples},
    {"nvnTextureInitialize", tex_initialize}, {"nvnTextureFinalize", tex_finalize}, {"nvnTextureGetFlags", tex_get_flags},
    {"nvnTextureGetTarget", tex_get_target}, {"nvnTextureGetFormat", tex_get_format}, {"nvnTextureGetWidth", tex_get_width}, {"nvnTextureGetHeight", tex_get_height},
    {"nvnTextureGetDepth", tex_get_depth}, {"nvnTextureGetLevels", tex_get_levels}, {"nvnTextureGetSamples", tex_get_samples}, {"nvnTextureGetStride", tex_get_stride},
    {"nvnTextureGetStorageSize", tex_get_storage_size}, {"nvnTextureGetMemoryOffset", tex_get_memory_offset}, {"nvnTextureGetTextureAddress", tex_get_address},
    {"nvnTextureBuilderGetStorageClass", texb_storage_class}, {"nvnTextureBuilderGetRawStorageClass", texb_raw_storage_class}, {"nvnTextureBuilderGetSparseTileLayout", texb_sparse_layout},
    {"nvnTextureGetStorageClass", tex_storage_class}, {"nvnTextureGetRawStorageClass", tex_raw_storage_class}, {"nvnTextureGetSparseTileLayout", tex_sparse_layout},
    {"nvnTextureGetDepthStencilMode", tex_get_dsmode},     {"nvnTextureWriteTexels", tex_write_texels}, {"nvnTextureWriteTexelsStrided", tex_write_texels}, {"nvnTextureFlushTexels", tex_flush_texels},
    {"nvnTextureViewSetDefaults", view_defaults}, {"nvnTextureViewSetLevels", view_levels}, {"nvnTextureViewSetLayers", view_layers}, {"nvnTextureViewSetFormat", view_format},
    {"nvnTextureViewSetSwizzle", view_swizzle}, {"nvnTextureViewSetDepthStencilMode", view_dsmode}, {"nvnTextureViewSetTarget", view_target},
    {"nvnTexturePoolInitialize", tpool_initialize}, {"nvnTexturePoolRegisterTexture", tpool_register},     {"nvnBlendStateSetBlendFunc", blend_func_set}, {"nvnBlendStateSetBlendEquation", blend_equation},
    {"nvnColorStateSetDefaults", color_defaults}, {"nvnColorStateSetBlendEnable", color_blend_enable}, {"nvnColorStateSetLogicOp", color_logic_op}, {"nvnColorStateSetAlphaTest", color_alpha_test},
    {"nvnChannelMaskStateSetDefaults", chanmask_defaults}, {"nvnChannelMaskStateSetChannelMask", chanmask_set},
                        {"nvnPolygonStateSetDefaults", poly_defaults}, {"nvnPolygonStateSetCullFace", poly_cull}, {"nvnPolygonStateSetFrontFace", poly_front}, {"nvnPolygonStateSetPolygonMode", poly_mode},
    {"nvnPolygonStateSetPolygonOffsetEnables", poly_offset}, {"nvnPolygonStateGetCullFace", poly_get_cull}, {"nvnPolygonStateGetFrontFace", poly_get_front},
    {"nvnPolygonStateGetPolygonMode", poly_get_mode}, {"nvnPolygonStateGetPolygonOffsetEnables", poly_get_offset},
    {"nvnDepthStencilStateSetDefaults", ds_defaults}, {"nvnDepthStencilStateSetDepthTestEnable", ds_depth_test}, {"nvnDepthStencilStateSetDepthWriteEnable", ds_depth_write},
    {"nvnDepthStencilStateSetDepthFunc", ds_depth_func}, {"nvnDepthStencilStateSetStencilTestEnable", ds_stencil_test}, {"nvnDepthStencilStateSetStencilFunc", ds_stencil_func},
    {"nvnDepthStencilStateSetStencilOp", ds_stencil_op}, {"nvnDepthStencilStateGetDepthTestEnable", ds_get_depth_test}, {"nvnDepthStencilStateGetDepthWriteEnable", ds_get_depth_write},
    {"nvnDepthStencilStateGetStencilTestEnable", ds_get_stencil_test}, {"nvnDepthStencilStateGetDepthFunc", ds_get_depth_func},
    {"nvnVertexAttribStateSetDefaults", vattr_defaults}, {"nvnVertexAttribStateSetFormat", vattr_format}, {"nvnVertexAttribStateSetStreamIndex", vattr_stream},
    {"nvnVertexStreamStateSetDefaults", vstream_defaults}, {"nvnVertexStreamStateSetStride", vstream_stride}, {"nvnVertexStreamStateSetDivisor", vstream_divisor},
    {"nvnCommandBufferInitialize", cmd_initialize}, {"nvnCommandBufferFinalize", cmd_finalize}, {"nvnCommandBufferAddCommandMemory", cmd_add_command_memory},
    {"nvnCommandBufferAddControlMemory", cmd_add_control_memory}, {"nvnCommandBufferGetCommandMemorySize", cmd_get_command_size}, {"nvnCommandBufferGetControlMemorySize", cmd_get_control_size},
        {"nvnCommandBufferIsRecording", cmd_is_recording},         {"nvnCommandBufferBeginRecording", cmd_begin}, {"nvnCommandBufferEndRecording", cmd_end}, {"nvnCommandBufferCallCommands", cmd_call}, {"nvnCommandBufferCopyCommands", cmd_call},
    {"nvnCommandBufferSetTexturePool", cmd_texpool}, {"nvnCommandBufferSetSamplerPool", cmd_smppool}, {"nvnCommandBufferBindProgram", cmd_program},
    {"nvnCommandBufferBindUniformBuffer", cmd_ubo}, {"nvnCommandBufferBindUniformBuffers", cmd_ubos}, {"nvnCommandBufferBindStorageBuffer", cmd_ssbo},
    {"nvnCommandBufferBindStorageBuffers", cmd_ssbos}, {"nvnCommandBufferBindTexture", cmd_texture}, {"nvnCommandBufferBindTextures", cmd_textures},
    {"nvnCommandBufferBindVertexBuffer", cmd_vbuf}, {"nvnCommandBufferBindVertexBuffers", cmd_vbufs}, {"nvnCommandBufferBindVertexAttribState", cmd_vattr},
    {"nvnCommandBufferBindVertexStreamState", cmd_vstream}, {"nvnCommandBufferBindBlendState", cmd_blend}, {"nvnCommandBufferBindColorState", cmd_color},
    {"nvnCommandBufferBindChannelMaskState", cmd_chanmask}, {"nvnCommandBufferBindMultisampleState", cmd_multisample}, {"nvnCommandBufferBindPolygonState", cmd_polygon},
    {"nvnCommandBufferBindDepthStencilState", cmd_depthstencil}, {"nvnCommandBufferSetViewport", cmd_viewport}, {"nvnCommandBufferSetViewports", cmd_viewports},
    {"nvnCommandBufferSetScissor", cmd_scissor}, {"nvnCommandBufferSetScissors", cmd_scissors}, {"nvnCommandBufferSetDepthRange", cmd_depth_range},
    {"nvnCommandBufferSetDepthRanges", cmd_depth_ranges}, {"nvnCommandBufferSetRenderTargets", cmd_targets}, {"nvnCommandBufferSetStencilRef", cmd_stencil_ref},
    {"nvnCommandBufferSetStencilValueMask", cmd_stencil_vmask}, {"nvnCommandBufferSetStencilMask", cmd_stencil_mask}, {"nvnCommandBufferSetBlendColor", cmd_blend_color},
    {"nvnCommandBufferSetAlphaRef", cmd_alpha_ref}, {"nvnCommandBufferSetPointSize", cmd_point_size}, {"nvnCommandBufferSetLineWidth", cmd_line_width},
    {"nvnCommandBufferSetPolygonOffsetClamp", cmd_poly_offset}, {"nvnCommandBufferSetRasterizerDiscard", cmd_rast_discard}, {"nvnCommandBufferSetDepthClamp", cmd_depth_clamp},
    {"nvnCommandBufferSetPrimitiveRestart", cmd_prim_restart}, {"nvnCommandBufferSetPatchSize", cmd_patch_size}, {"nvnCommandBufferClearColor", cmd_clear_color},
    {"nvnCommandBufferClearDepthStencil", cmd_clear_ds}, {"nvnCommandBufferDrawArrays", cmd_draw_arrays}, {"nvnCommandBufferDrawArraysInstanced", cmd_draw_arrays_inst},
    {"nvnCommandBufferDrawElements", cmd_draw_elements}, {"nvnCommandBufferDrawElementsBaseVertex", cmd_draw_elements_bv}, {"nvnCommandBufferDrawElementsInstanced", cmd_draw_elements_inst},
    {"nvnCommandBufferCopyBufferToTexture", cmd_copy_buf_tex}, {"nvnCommandBufferCopyTextureToTexture", cmd_copy_tex_tex}, {"nvnCommandBufferCopyTextureToBuffer", cmd_copy_tex_buf},
    {"nvnCommandBufferSetSampleMask", cmd_sample_mask}, {"nvnCommandBufferDownsample", cmd_downsample},
    {"nvnCommandBufferCopyBufferToBuffer", cmd_copy_buf_buf}, {"nvnCommandBufferUpdateUniformBuffer", cmd_update_ubo}, {"nvnCommandBufferClearBuffer", cmd_clear_buffer},
    {"nvnCommandBufferSetCopyRowStride", cmd_row_stride},
                {"nvnQueueSubmitCommands", queue_submit}, {"nvnQueuePresentTexture", queue_present},     {"nvnCommandBufferBindImages", cmd_images}, {"nvnCommandBufferBindImage", cmd_image}, {"nvnCommandBufferDispatchCompute", cmd_dispatch},
    {"nvnWindowBuilderSetTextures", wb_set_textures}, {"nvnWindowInitialize", win_initialize}, {"nvnWindowAcquireTexture", win_acquire}, {"nvnWindowSetCrop", win_set_crop},
    /* the rest of the API (see the block above the table) */
    {"nvnDeviceBuilderSetDefaults", devb_defaults}, {"nvnDeviceBuilderSetFlags", devb_set_flags}, {"nvnDeviceBuilderGetFlags", devb_get_flags}, {"nvnDeviceInitialize", dev_initialize},
    {"nvnDeviceFinalize", dev_finalize}, {"nvnDeviceGetWindowOriginMode", dev_get_origin_mode}, {"nvnDeviceGetDepthMode", dev_get_depth_mode},
    {"nvnDeviceGetCurrentTimestampInNanoseconds", dev_timestamp_now}, {"nvnDeviceGetTimestampInNanoseconds", dev_timestamp_of}, {"nvnDeviceGenerateDebugDomainId", dev_debug_domain},
    {"nvnDeviceGetSeparateTextureHandle", dev_separate_handle}, {"nvnDeviceGetSeparateSamplerHandle", dev_separate_sampler_handle}, {"nvnDeviceIsExternalDebuggerAttached", dev_debugger_attached},
    {"nvnDeviceInstallDebugCallback", n_nop}, {"nvnDeviceApplyDeferredFinalizes", dev_apply_deferred}, {"nvnDeviceSetDebugLabel", any_set_label},
    {"nvnDeviceFinalizeCommandHandle", dev_finalize_command_handle}, {"nvnQueueBuilderSetDevice", any_set_device}, {"nvnQueueBuilderGetDevice", any_get_device},
    {"nvnWindowBuilderSetDevice", any_set_device}, {"nvnWindowBuilderGetDevice", any_get_device}, {"nvnTextureBuilderSetDevice", any_set_device},
    {"nvnTextureBuilderGetDevice", any_get_device}, {"nvnSamplerBuilderSetDevice", any_set_device}, {"nvnSamplerBuilderGetDevice", any_get_device}, {"nvnSyncInitialize", sync_initialize},
    {"nvnSyncFinalize", sync_finalize}, {"nvnSyncSetDebugLabel", any_set_label}, {"nvnSyncWait", sync_wait}, {"nvnQueueFenceSync", queue_fence_sync}, {"nvnQueueWaitSync", queue_wait_sync},
    {"nvnSyncCreateGLSync", sync_no_gl_interop}, {"nvnSyncInitializeFromFencedGLSync", sync_no_gl_interop}, {"nvnEventBuilderSetDefaults", evb_defaults},
    {"nvnEventBuilderSetStorage", evb_storage}, {"nvnEventBuilderGetStorage", evb_get_storage}, {"nvnEventBuilderGetMemoryPool", ev_get_pool},
    {"nvnEventBuilderGetMemoryOffset", ev_get_offset}, {"nvnEventInitialize", ev_initialize}, {"nvnEventFinalize", ev_finalize}, {"nvnEventGetValue", ev_get_value},
    {"nvnEventSignal", ev_signal}, {"nvnEventGetMemoryPool", ev_get_pool}, {"nvnEventGetMemoryOffset", ev_get_offset}, {"nvnQueueBuilderSetDefaults", qb_defaults},
    {"nvnQueueBuilderSetFlags", qb_set_flags}, {"nvnQueueBuilderSetCommandMemorySize", qb_set_cmd}, {"nvnQueueBuilderSetComputeMemorySize", qb_set_compute},
    {"nvnQueueBuilderSetControlMemorySize", qb_set_control}, {"nvnQueueBuilderSetCommandFlushThreshold", qb_set_threshold}, {"nvnQueueBuilderSetQueuePriority", qb_set_priority},
    {"nvnQueueBuilderSetQueueMemory", qb_set_memory}, {"nvnQueueBuilderGetFlags", qb_get_flags}, {"nvnQueueBuilderGetCommandMemorySize", qb_get_cmd},
    {"nvnQueueBuilderGetComputeMemorySize", qb_get_compute}, {"nvnQueueBuilderGetControlMemorySize", qb_get_control}, {"nvnQueueBuilderGetCommandFlushThreshold", qb_get_threshold},
    {"nvnQueueBuilderGetQueuePriority", qb_get_priority}, {"nvnQueueBuilderGetMemory", qb_get_memory}, {"nvnQueueBuilderGetMemorySize", qb_get_memory_size},
    {"nvnQueueBuilderGetQueueMemorySize", qb_get_queue_memory_size}, {"nvnQueueInitialize", queue_initialize}, {"nvnQueueFinalize", queue_finalize}, {"nvnQueueSetDebugLabel", any_set_label},
    {"nvnQueueGetTotalCommandMemoryUsed", queue_total_cmd}, {"nvnQueueGetTotalControlMemoryUsed", queue_total_control}, {"nvnQueueGetTotalComputeMemoryUsed", queue_total_compute},
    {"nvnQueueResetMemoryUsageCounts", queue_reset_usage}, {"nvnQueueFlush", queue_flush}, {"nvnQueueFinish", queue_finish}, {"nvnQueueGetError", queue_get_error},
    {"nvnQueueAcquireTexture", queue_acquire}, {"nvnMultisampleStateSetDefaults", ms_defaults}, {"nvnMultisampleStateSetMultisampleEnable", ms_enable},
    {"nvnMultisampleStateSetAlphaToCoverageEnable", ms_a2c}, {"nvnMultisampleStateSetAlphaToCoverageDither", ms_a2c_dither}, {"nvnMultisampleStateSetSamples", ms_samples},
    {"nvnMultisampleStateSetRasterSamples", ms_raster_samples}, {"nvnMultisampleStateSetCoverageToColorEnable", ms_c2c}, {"nvnMultisampleStateSetCoverageToColorOutput", ms_c2c_output},
    {"nvnMultisampleStateSetCoverageModulationMode", ms_covmod}, {"nvnMultisampleStateSetSampleLocationsEnable", ms_locations_on},
    {"nvnMultisampleStateSetSampleLocationsGridEnable", ms_grid_on}, {"nvnMultisampleStateSetSampleLocations", ms_set_locations}, {"nvnMultisampleStateGetMultisampleEnable", ms_get_enable},
    {"nvnMultisampleStateGetAlphaToCoverageEnable", ms_get_a2c}, {"nvnMultisampleStateGetAlphaToCoverageDither", ms_get_a2c_dither}, {"nvnMultisampleStateGetSamples", ms_get_samples},
    {"nvnMultisampleStateGetRasterSamples", ms_get_raster_samples}, {"nvnMultisampleStateGetCoverageToColorEnable", ms_get_c2c},
    {"nvnMultisampleStateGetCoverageToColorOutput", ms_get_c2c_output}, {"nvnMultisampleStateGetCoverageModulationMode", ms_get_covmod},
    {"nvnMultisampleStateGetSampleLocationsEnable", ms_get_locations_on}, {"nvnMultisampleStateGetSampleLocationsGridEnable", ms_get_grid_on},
    {"nvnMultisampleStateGetSampleLocationsGrid", ms_get_grid}, {"nvnBlendStateSetDefaults", blend_defaults}, {"nvnBlendStateSetBlendTarget", blend_target},
    {"nvnBlendStateSetAdvancedMode", blend_adv_mode}, {"nvnBlendStateSetAdvancedOverlap", blend_adv_overlap}, {"nvnBlendStateSetAdvancedPremultipliedSrc", blend_adv_premult},
    {"nvnBlendStateSetAdvancedNormalizedDst", blend_adv_normdst}, {"nvnBlendStateGetBlendTarget", blend_get_target}, {"nvnBlendStateGetBlendFunc", blend_get_func},
    {"nvnBlendStateGetBlendEquation", blend_get_equation}, {"nvnBlendStateGetAdvancedMode", blend_get_adv_mode}, {"nvnBlendStateGetAdvancedOverlap", blend_get_adv_overlap},
    {"nvnBlendStateGetAdvancedPremultipliedSrc", blend_get_adv_premult}, {"nvnBlendStateGetAdvancedNormalizedDst", blend_get_adv_normdst},
    {"nvnColorStateGetBlendEnable", color_get_blend_enable}, {"nvnColorStateGetLogicOp", color_get_logic_op}, {"nvnColorStateGetAlphaTest", color_get_alpha_test},
    {"nvnChannelMaskStateGetChannelMask", chanmask_get}, {"nvnDepthStencilStateGetStencilFunc", ds_get_stencil_func}, {"nvnDepthStencilStateGetStencilOp", ds_get_stencil_op},
    {"nvnVertexAttribStateGetFormat", vattr_get_format}, {"nvnVertexAttribStateGetStreamIndex", vattr_get_stream}, {"nvnVertexStreamStateGetStride", vstream_get_stride},
    {"nvnVertexStreamStateGetDivisor", vstream_get_divisor}, {"nvnSamplerBuilderSetBorderColor", smpb_border_f}, {"nvnSamplerBuilderSetBorderColori", smpb_border_i},
    {"nvnSamplerBuilderSetBorderColorui", smpb_border_i}, {"nvnSamplerBuilderSetLodSnap", smpb_lod_snap}, {"nvnSamplerBuilderGetMinMagFilter", smp_get_minmag},
    {"nvnSamplerBuilderGetWrapMode", smp_get_wrap}, {"nvnSamplerBuilderGetLodClamp", smp_get_lodclamp}, {"nvnSamplerBuilderGetLodBias", smp_get_lodbias},
    {"nvnSamplerBuilderGetCompare", smp_get_compare}, {"nvnSamplerBuilderGetBorderColor", smp_get_border}, {"nvnSamplerBuilderGetBorderColori", smp_get_border},
    {"nvnSamplerBuilderGetBorderColorui", smp_get_border}, {"nvnSamplerBuilderGetMaxAnisotropy", smp_get_aniso}, {"nvnSamplerBuilderGetReductionFilter", smp_get_reduction},
    {"nvnSamplerBuilderGetLodSnap", smp_get_lod_snap}, {"nvnSamplerGetMinMagFilter", smp_get_minmag}, {"nvnSamplerGetWrapMode", smp_get_wrap}, {"nvnSamplerGetLodClamp", smp_get_lodclamp},
    {"nvnSamplerGetLodBias", smp_get_lodbias}, {"nvnSamplerGetCompare", smp_get_compare}, {"nvnSamplerGetBorderColor", smp_get_border}, {"nvnSamplerGetBorderColori", smp_get_border},
    {"nvnSamplerGetBorderColorui", smp_get_border}, {"nvnSamplerGetMaxAnisotropy", smp_get_aniso}, {"nvnSamplerGetReductionFilter", smp_get_reduction}, {"nvnSamplerCompare", smp_compare},
    {"nvnSamplerGetDebugID", any_debug_id}, {"nvnSamplerFinalize", smp_finalize}, {"nvnSamplerSetDebugLabel", any_set_label}, {"nvnSamplerPoolFinalize", spool_finalize},
    {"nvnSamplerPoolGetMemoryPool", spool_get_pool}, {"nvnSamplerPoolGetMemoryOffset", spool_get_offset}, {"nvnSamplerPoolGetSize", spool_get_size},
    {"nvnSamplerPoolSetDebugLabel", any_set_label}, {"nvnTextureBuilderGetSwizzle", texb_get_swizzle}, {"nvnTextureBuilderGetDepthStencilMode", texb_get_dsmode},
    {"nvnTextureBuilderGetMemoryPool", texb_get_pool}, {"nvnTextureBuilderGetMemoryOffset", texb_get_offset}, {"nvnTextureBuilderGetPackagedTextureData", texb_get_packaged},
    {"nvnTextureBuilderGetPackagedTextureLayout", texb_get_layout}, {"nvnTextureBuilderGetStride", texb_get_stride}, {"nvnTextureBuilderSetGLTextureName", texb_set_glname},
    {"nvnTextureBuilderGetGLTextureName", texb_get_glname}, {"nvnTextureGetSwizzle", tex_get_swizzle}, {"nvnTextureGetMemoryPool", tex_get_pool}, {"nvnTextureCompare", tex_compare},
    {"nvnTextureGetDebugID", any_debug_id}, {"nvnTextureGetViewOffset", tex_view_offset}, {"nvnTextureReadTexels", tex_read_texels}, {"nvnTextureReadTexelsStrided", tex_read_texels},
    {"nvnTextureInvalidateTexels", tex_invalidate_texels}, {"nvnTextureSetDebugLabel", any_set_label}, {"nvnTextureViewGetLevels", view_get_levels},
    {"nvnTextureViewGetLayers", view_get_layers}, {"nvnTextureViewGetFormat", view_get_format}, {"nvnTextureViewGetSwizzle", view_get_swizzle},
    {"nvnTextureViewGetDepthStencilMode", view_get_dsmode}, {"nvnTextureViewGetTarget", view_get_target}, {"nvnTextureViewCompare", view_compare}, {"nvnTexturePoolFinalize", tpool_finalize},
    {"nvnTexturePoolGetMemoryPool", tpool_get_pool}, {"nvnTexturePoolGetMemoryOffset", tpool_get_offset}, {"nvnTexturePoolGetSize", tpool_get_size},
    {"nvnTexturePoolRegisterImage", tpool_register}, {"nvnTexturePoolSetDebugLabel", any_set_label}, {"nvnProgramSetDebugLabel", any_set_label},
    {"nvnProgramSetSampleShading", prog_sample_shading}, {"nvnWindowBuilderSetDefaults", wb_defaults}, {"nvnWindowBuilderSetNativeWindow", wb_native},
    {"nvnWindowBuilderSetPresentInterval", wb_interval}, {"nvnWindowBuilderSetNumActiveTextures", wb_num_active}, {"nvnWindowBuilderGetNativeWindow", wb_get_native},
    {"nvnWindowBuilderGetPresentInterval", wb_get_interval}, {"nvnWindowBuilderGetNumTextures", wb_get_num_textures}, {"nvnWindowBuilderGetNumActiveTextures", wb_get_num_active},
    {"nvnWindowBuilderGetTexture", wb_get_texture}, {"nvnWindowFinalize", win_finalize}, {"nvnWindowSetDebugLabel", any_set_label}, {"nvnWindowSetPresentInterval", win_set_interval},
    {"nvnWindowGetPresentInterval", win_get_interval}, {"nvnWindowGetNativeWindow", win_get_native}, {"nvnWindowGetNumTextures", win_get_num_textures},
    {"nvnWindowGetNumActiveTextures", win_get_num_active}, {"nvnWindowSetNumActiveTextures", win_set_num_active}, {"nvnWindowGetCrop", win_get_crop},
    {"nvnCommandBufferSetMemoryCallback", cmd_set_callback}, {"nvnCommandBufferSetMemoryCallbackData", cmd_set_callback_data}, {"nvnCommandBufferGetMemoryCallback", cmd_get_callback},
    {"nvnCommandBufferGetMemoryCallbackData", cmd_get_callback_data}, {"nvnCommandBufferSetCommandMemoryCallbackEnabled", cmd_callback_enabled},
    {"nvnCommandBufferGetCommandMemoryUsed", cmd_get_command_used}, {"nvnCommandBufferGetControlMemoryUsed", cmd_get_control_used},
    {"nvnCommandBufferGetCommandMemoryFree", cmd_get_command_free}, {"nvnCommandBufferGetControlMemoryFree", cmd_get_control_free}, {"nvnCommandBufferGetCopyRowStride", cmd_get_row_stride},
    {"nvnCommandBufferGetCopyImageStride", cmd_get_image_stride}, {"nvnCommandBufferSetCopyImageStride", cmd_image_stride}, {"nvnCommandBufferSetDepthBounds", cmd_depth_bounds},
    {"nvnCommandBufferSetConservativeRasterEnable", cmd_cons_raster}, {"nvnCommandBufferSetConservativeRasterDilate", cmd_cons_dilate},
    {"nvnCommandBufferSetSubpixelPrecisionBias", cmd_subpixel_bias}, {"nvnCommandBufferSetViewportSwizzles", cmd_viewport_swizzles},
    {"nvnCommandBufferSetInnerTessellationLevels", cmd_tess_levels}, {"nvnCommandBufferSetOuterTessellationLevels", cmd_tess_levels}, {"nvnCommandBufferBarrier", cmd_barrier},
    {"nvnCommandBufferDiscardColor", cmd_discard_color}, {"nvnCommandBufferDiscardDepthStencil", cmd_discard_ds}, {"nvnCommandBufferClearColori", cmd_clear_color_typed},
    {"nvnCommandBufferClearColorui", cmd_clear_color_typed}, {"nvnCommandBufferClearTexture", cmd_clear_texture}, {"nvnCommandBufferClearTexturei", cmd_clear_texture},
    {"nvnCommandBufferClearTextureui", cmd_clear_texture}, {"nvnCommandBufferDownsampleTextureView", cmd_downsample_view}, {"nvnCommandBufferTiledDownsample", cmd_downsample},
    {"nvnCommandBufferTiledDownsampleTextureView", cmd_downsample_view}, {"nvnCommandBufferDrawArraysIndirect", cmd_draw_arrays_indirect},
    {"nvnCommandBufferDrawElementsIndirect", cmd_draw_elements_indirect}, {"nvnCommandBufferMultiDrawArraysIndirectCount", cmd_multi_draw_arrays_indirect},
    {"nvnCommandBufferMultiDrawElementsIndirectCount", cmd_multi_draw_elements_indirect}, {"nvnCommandBufferDispatchComputeIndirect", cmd_dispatch_indirect},
    {"nvnCommandBufferSetRenderEnable", cmd_render_enable}, {"nvnCommandBufferSetRenderEnableConditional", cmd_render_enable_cond}, {"nvnCommandBufferResetCounter", cmd_reset_counter},
    {"nvnCommandBufferReportCounter", cmd_report_counter}, {"nvnCommandBufferReportValue", cmd_report_value}, {"nvnCommandBufferFenceSync", cmd_fence_sync},
    {"nvnCommandBufferWaitSync", cmd_wait_sync}, {"nvnCommandBufferSignalEvent", cmd_signal_event}, {"nvnCommandBufferWaitEvent", cmd_wait_event},
    {"nvnCommandBufferDrawTexture", cmd_draw_texture},
    {"nvnCommandBufferBindCoverageModulationTable", cmd_covmod_table},{"nvnCommandBufferPushDebugGroup", cmd_push_group}, {"nvnCommandBufferPushDebugGroupStatic", cmd_push_group},
    {"nvnCommandBufferPushDebugGroupDynamic", cmd_push_group}, {"nvnCommandBufferPopDebugGroup", cmd_pop_group}, {"nvnCommandBufferPopDebugGroupId", cmd_pop_group},
    {"nvnCommandBufferInsertDebugMarker", cmd_marker}, {"nvnCommandBufferInsertDebugMarkerStatic", cmd_marker_id}, {"nvnCommandBufferInsertDebugMarkerDynamic", cmd_marker_id},
    {"nvnCommandBufferSetDebugLabel", cmd_set_label},
    /* no effect on this platform by the meaning of the call itself: each one is explained in nvn_audit_notes.tsv */
    {"nvnCommandBufferSetShaderScratchMemory", n_nop}, {"nvnSetExternalDebuggerEnabledNVX", n_nop}, {"nvnDeviceSetIntermediateShaderCache", n_nop}, {"nvnDeviceWalkDebugDatabase", n_nop},
    {"nvnCommandBufferSetTiledCacheAction", n_nop}, {"nvnCommandBufferSetTiledCacheTileSize", n_nop}, {"nvnCommandBufferSaveZCullData", n_nop},
    {"nvnCommandBufferRestoreZCullData", n_nop}, {"nvnCommandBufferSetStencilCullCriteria", n_nop}, {"nvnCommandBufferResolveDepthBuffer", n_nop},
    {"nvnCommandBufferSetColorReductionEnable", n_nop}, {"nvnCommandBufferSetColorReductionThresholds", n_nop}, {"nvnBufferInvalidateMappedRange", n_nop},
    {"nvnMemoryPoolInvalidateMappedRange", n_nop}, {"nvnDeviceRegisterFastClearColor", n_true}, {"nvnDeviceRegisterFastClearColori", n_true},
    {"nvnDeviceRegisterFastClearColorui", n_true}, {"nvnDeviceRegisterFastClearDepth", n_true}, {"nvnTextureBuilderGetZCullStorageSize", n_zero},
    {"nvnTextureGetZCullStorageSize", n_zero},
};

/* The host function for an NVN entry point, or NULL when this backend does not implement it. */
ImportFn nvn_gl_pick(const char *name) {
    for (size_t i = 0; i < sizeof k_nvn / sizeof k_nvn[0]; i++) if (strcmp(name, k_nvn[i].name) == 0) return k_nvn[i].fn;
    return NULL;
}
