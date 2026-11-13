/* NVN on Vulkan, the drawing part (session 2; design: plan\phases\V1_vulkan.md). Included at the end of nvn_vk.c: it uses
 * that file's device (V), loader functions and error helpers. Render thread only.
 *
 * What is here: device features, a block allocator, the stream ring (guest memory -> one mapped buffer, at most one copy
 * of a range per batch), images and views made at first use from the front end's texture source, samplers, translated
 * stages -> SPIR-V (ssbu_glslang.dll, cached in ..\cache\vk\spv) -> pipelines made synchronously, state tracking with
 * dynamic state, dynamic rendering, clears, draws, buffer->texture and texture->texture copies, present by blit, the
 * screenshot of the window texture, fences. Anything else stops with its name (nvn_vk_missing).
 *
 * Debug: SSBU_VK_DEBUG=1 turns on the checks that stand in for the validation layer that is not installed (image used
 * after destroy, barrier inside a pass, a view of an image in an unexpected layout) and prints every new pipeline. */
#include <stdarg.h>
#include <math.h>
#include "tex_decode.h"
static int have_targets_live(void);

#define VKD(X) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetDepthBias) \
    X(vkCmdSetBlendConstants) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) X(vkCmdSetLineWidth) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) \
    X(vkCmdSetPrimitiveTopology) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdSetRasterizerDiscardEnable) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdClearAttachments) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdBlitImage) X(vkCmdResolveImage) X(vkCmdClearDepthStencilImage) X(vkGetFenceStatus) \
    X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) X(vkGetQueryPoolResults) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache) X(vkCmdCopyBuffer)
#define X(n) static PFN_##n n;
VKD(X)
#undef X
static PFN_vkCmdSetVertexInputEXT vkCmdSetVertexInputEXT;
static PFN_vkCmdBindVertexBuffers2 vkCmdBindVertexBuffers2;  /* MAC2 / V_novids: the strides of the bindings without dynamic vertex input */
static PFN_vkCmdPushDescriptorSetKHR vkCmdPushDescriptorSetKHR;
static PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
static PFN_vkGetPhysicalDeviceFormatProperties vkGetPhysicalDeviceFormatProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
static PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) vk_fatal(#call, r_); } while (0)
static void ckpt_op(const char *what, uint64_t obj, int a, int b, int c);      /* PR #32, device loss report: a marker before a command that is not a draw */
#define CKPT_OP(what, obj, a, b, c) do { if (D.has_ckpt) ckpt_op(what, (uint64_t)(uintptr_t)(obj), (int)(a), (int)(b), (int)(c)); } while (0)

static struct {
    VkPhysicalDeviceFeatures2 f2; VkPhysicalDeviceVulkan11Features f11; VkPhysicalDeviceVulkan12Features f12; VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vids; VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc; VkPhysicalDeviceProvokingVertexFeaturesEXT pv;
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT iu8; VkPhysicalDevicePrimitiveTopologyListRestartFeaturesEXT plr; VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT vad;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3;
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl;
    VkPhysicalDeviceFaultFeaturesEXT fault;
#ifdef VK_KHR_portability_subset
    VkPhysicalDevicePortabilitySubsetFeaturesKHR port; int has_port;
#endif
    const char *exts[16]; uint32_t next;
    int has_vids, has_cbc, has_pv, has_iu8, has_plr, has_vad, has_push, has_eds3, debug;
    int has_gpl, ext_gpl, ext_so, has_fault, has_ckpt;
    VkPhysicalDeviceMemoryProperties mp;
} D;
/* V_novids (2026-10-21): a GPU without VK_EXT_vertex_input_dynamic_state keeps its own pipeline list: its keys name a
 * vertex layout (nvk_vl_*), those of the other GPUs do not (the layout of pipes.bin and its keys are unchanged there).
 * macOS keeps the layout inside the key (MAC2) and its pipes.bin. */
#ifdef __APPLE__
#define VK_PIPES_NOVI "pipes.bin"
#define VK_VL_FILE "the keys themselves"
#else
#define VK_PIPES_NOVI "pipes_novi.bin"
#define VK_VL_FILE "vi_layouts.bin"
#endif
#define VK_PIPES_BIN (D.has_vids ? "pipes.bin" : VK_PIPES_NOVI)
static PFN_vkGetDeviceFaultInfoEXT vkGetDeviceFaultInfoEXT;
static PFN_vkCmdSetCheckpointNV vkCmdSetCheckpointNV;
static PFN_vkGetQueueCheckpointDataNV vkGetQueueCheckpointDataNV;
static void vkd_fail(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); printf("nvn_vk: FATAL: "); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); ExitProcess(6);
}
#define DBG_ASSERT(c, ...) do { if (D.debug && !(c)) vkd_fail(__VA_ARGS__); } while (0)
/* Session 11: the pipeline-library mode. SSBU_VK_GPL wins (0 = whole pipelines, the path before session 9; 1 = a library per
 * stage pair, a measurement mode; 2 = a library per stage + preparation + barrier); without it VK_GPL_DEFAULT. */
#ifndef VK_GPL_DEFAULT
#define VK_GPL_DEFAULT 2                           /* session 11: on by default where the driver has the extension (validation run s11v3 and the first-run table of V1_vulkan.md "Session 11"); -DVK_GPL_DEFAULT=0 or SSBU_VK_GPL=0 for the old path */
#endif
static int vk_gpl_mode(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("SSBU_VK_GPL");
        m = e && e[0] ? atoi(e) : VK_GPL_DEFAULT;
        if (!(e && e[0]) && GetFileAttributesA("../cache/vk/no_stage_libraries.txt") != INVALID_FILE_ATTRIBUTES) {     /* nvn_vk_m4.c stagelib_ensure: the driver refused one */
            m = 0;
            printf("nvn_vk: ../cache/vk/no_stage_libraries.txt: this driver refused a stage library in an earlier run: whole pipelines\n");
        }
    }
    return m;
}

/* ---------------------------------------------------------------- device features */
static void vkd_device_info(VkDeviceCreateInfo *di, const VkExtensionProperties *dext, uint32_t ndext) {
    void **tail;
    vkGetPhysicalDeviceFeatures2 = vk_sym("vkGetPhysicalDeviceFeatures2");
    vkGetPhysicalDeviceFormatProperties = vk_sym("vkGetPhysicalDeviceFormatProperties");
    vkGetPhysicalDeviceMemoryProperties = vk_sym("vkGetPhysicalDeviceMemoryProperties");
    vkGetDeviceProcAddr = vk_sym("vkGetDeviceProcAddr");
    D.debug = getenv("SSBU_VK_DEBUG") && atoi(getenv("SSBU_VK_DEBUG"));
    D.f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2; D.f11.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    D.f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES; D.f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    D.f2.pNext = &D.f11; D.f11.pNext = &D.f12; D.f12.pNext = &D.f13; tail = &D.f13.pNext;
    D.exts[D.next++] = "VK_KHR_swapchain";
#define EXT(flag, name, st, stype) if (has_name(dext, ndext, name)) { D.flag = 1; D.exts[D.next++] = name; D.st.sType = stype; *tail = &D.st; tail = &D.st.pNext; }
    /* V_novids, TEST ONLY: SSBU_VK_TEST bit 512 = the device is taken as one without VK_EXT_vertex_input_dynamic_state
     * (an RX 550 / Polaris case on a GPU that has it): the extension is not enabled, the vertex layout goes into the key */
    if (getenv("SSBU_VK_TEST") && (atoi(getenv("SSBU_VK_TEST")) & 512)) printf("nvn_vk: TEST (SSBU_VK_TEST bit 512): VK_EXT_vertex_input_dynamic_state taken as missing\n");
    else { EXT(has_vids, "VK_EXT_vertex_input_dynamic_state", vids, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT) }
    EXT(has_cbc, "VK_EXT_custom_border_color", cbc, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT)
    EXT(has_pv, "VK_EXT_provoking_vertex", pv, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT)
    EXT(has_iu8, "VK_EXT_index_type_uint8", iu8, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT)
    EXT(has_plr, "VK_EXT_primitive_topology_list_restart", plr, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIMITIVE_TOPOLOGY_LIST_RESTART_FEATURES_EXT)
    EXT(has_vad, "VK_EXT_vertex_attribute_divisor", vad, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT)
    if (!(getenv("SSBU_VK_EDS3") && !atoi(getenv("SSBU_VK_EDS3")))) { EXT(has_eds3, "VK_EXT_extended_dynamic_state3", eds3, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) }
    /* session 9: graphics pipeline libraries (nvn_vk_m4.c "graphics pipeline libraries"): the stage pair is compiled at loading
     * without the pass formats, a draw only LINKS. SSBU_VK_GPL=1 turns it on (needs extended dynamic state 3 as well). */
    D.ext_gpl = has_name(dext, ndext, "VK_EXT_graphics_pipeline_library") && has_name(dext, ndext, "VK_KHR_pipeline_library"); D.ext_so = has_name(dext, ndext, "VK_EXT_shader_object");
    if (D.ext_gpl && vk_gpl_mode()) { D.exts[D.next++] = "VK_KHR_pipeline_library"; EXT(has_gpl, "VK_EXT_graphics_pipeline_library", gpl, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT) }
    /* PR #32: a lost device says what it knows (vkd_lost_report): the fault (costs nothing until a loss) and, with
     * SSBU_VK_CHECKPOINTS=1, the last commands the GPU started and finished (a marker before most commands) */
    EXT(has_fault, "VK_EXT_device_fault", fault, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT)
#ifdef VK_KHR_portability_subset
    /* macOS (MoltenVK): a device that lists VK_KHR_portability_subset must have it enabled; its features (all the
     * supported ones enabled, as for the others) say what Metal cannot do. MAC4: the drawing code respects them. */
    EXT(has_port, "VK_KHR_portability_subset", port, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR)
#endif
#undef EXT
    if (getenv("SSBU_VK_CHECKPOINTS") && atoi(getenv("SSBU_VK_CHECKPOINTS")) && has_name(dext, ndext, "VK_NV_device_diagnostic_checkpoints")) { D.has_ckpt = 1; D.exts[D.next++] = "VK_NV_device_diagnostic_checkpoints"; }
    if (has_name(dext, ndext, "VK_KHR_push_descriptor") && !(getenv("SSBU_VK_PUSH") && !atoi(getenv("SSBU_VK_PUSH")))) { D.has_push = 1; D.exts[D.next++] = "VK_KHR_push_descriptor"; }
    vkGetPhysicalDeviceFeatures2(V.gpu, &D.f2);                    /* every supported feature of these structures is enabled */
    D.has_fault = D.has_fault && D.fault.deviceFault; D.fault.deviceFaultVendorBinary = VK_FALSE;
    D.has_vids = D.has_vids && D.vids.vertexInputDynamicState; D.has_cbc = D.has_cbc && D.cbc.customBorderColors && D.cbc.customBorderColorWithoutFormat;
    D.has_pv = D.has_pv && D.pv.provokingVertexLast; D.has_iu8 = D.has_iu8 && D.iu8.indexTypeUint8; D.has_plr = D.has_plr && D.plr.primitiveTopologyListRestart;
    /* session 4: blend / mask / polygon / multisample state out of the pipeline key, only when the GPU has ALL of these (else all stay in the key) */
    D.has_eds3 = D.has_eds3 && D.eds3.extendedDynamicState3ColorBlendEnable && D.eds3.extendedDynamicState3ColorBlendEquation && D.eds3.extendedDynamicState3ColorWriteMask && D.eds3.extendedDynamicState3PolygonMode &&
                 D.eds3.extendedDynamicState3AlphaToCoverageEnable && D.eds3.extendedDynamicState3SampleMask && (D.eds3.extendedDynamicState3DepthClampEnable || !D.f2.features.depthClamp);
    printf("nvn_vk: extended dynamic state 3 (blend enable / equation / write mask, polygon mode, alpha to coverage, sample mask, depth clamp dynamic = not in the pipeline key): %s\n", D.has_eds3 ? "yes" : "no");
    D.has_gpl = D.has_gpl && D.gpl.graphicsPipelineLibrary && D.has_eds3 && D.has_vids;
    printf("nvn_vk: driver support for pipelines without a hold: VK_EXT_graphics_pipeline_library %s, VK_EXT_shader_object %s, extended dynamic state 3 (all seven states) %s, vertex input dynamic state %s | pipeline libraries in use: %s\n",
           D.ext_gpl ? "yes" : "NO", D.ext_so ? "yes" : "NO", D.has_eds3 ? "yes" : "NO", D.has_vids ? "yes" : "NO", D.has_gpl ? "YES" : "no");
    /* session 11: ONE line that says which path draws and why */
    {   const char *e = getenv("SSBU_VK_GPL");
        int m = vk_gpl_mode();
        printf("nvn_vk_path: %s: %s (SSBU_VK_GPL %s; default %d; SSBU_VK_GPL=0 is the switch back to whole pipelines)\n",
               D.has_gpl && m >= 2 ? "STAGE LIBRARIES + preparation + loading barrier (a draw links, nothing is compiled in a fight)" : D.has_gpl ? "PAIR LIBRARIES (SSBU_VK_GPL=1, a measurement mode)" : "WHOLE PIPELINES (made by the workers from the list of earlier runs; a first use can hold a frame)",
               !m ? (e ? "turned off by SSBU_VK_GPL=0" : "off by default in this build") : D.has_gpl ? (e ? "asked for by SSBU_VK_GPL" : "the default: the driver has VK_EXT_graphics_pipeline_library, extended dynamic state 3 and dynamic vertex input") :
               !D.ext_gpl ? "the driver has NO VK_EXT_graphics_pipeline_library: fallen back" : "the driver lacks extended dynamic state 3 or dynamic vertex input: fallen back", e ? e : "not set", VK_GPL_DEFAULT);
    }
#ifdef VK_KHR_portability_subset
    if (D.has_port)
        printf("nvn_vk: portability subset (MoltenVK): triangle fans %d, image view swizzle %d, format reinterpretation %d, events %d, point polygons %d, separate stencil ref %d, "
               "constant alpha blend %d, sampler LOD bias %d, mutable comparison samplers %d, 2D views of 3D images %d, vertex access beyond stride %d\n",
               (int)D.port.triangleFans, (int)D.port.imageViewFormatSwizzle, (int)D.port.imageViewFormatReinterpretation, (int)D.port.events, (int)D.port.pointPolygons,
               (int)D.port.separateStencilMaskRef, (int)D.port.constantAlphaColorBlendFactors, (int)D.port.samplerMipLodBias, (int)D.port.mutableComparisonSamplers,
               (int)D.port.imageView2DOn3DImage, (int)D.port.vertexAttributeAccessBeyondStride);
#endif
    if (!D.f13.dynamicRendering || !D.f13.synchronization2) vkd_fail("the device lacks dynamicRendering / synchronization2");
    /* MAC2 / V_novids (2026-10-21): every platform now draws without the extension (an RX 550 / Polaris on AMD's Windows
     * driver lacks it): the vertex layout is part of the pipeline (nvk_vi_pipe), the strides stay dynamic */
    if (!D.has_vids) printf("nvn_vk: no VK_EXT_vertex_input_dynamic_state: the vertex layout is part of the pipeline key, strides dynamic; instance divisors above 1: %s; "
                            "the pipeline list of this GPU is ../cache/vk/" VK_PIPES_NOVI " (layouts in " VK_VL_FILE ")\n",
                            D.has_vad && D.vad.vertexAttributeInstanceRateDivisor ? "yes (VK_EXT_vertex_attribute_divisor)" : "NO (a draw with one stops the game)");
    if (!D.f2.features.textureCompressionBC) nvn_vk_missing("a GPU without BC texture compression");
    di->enabledExtensionCount = D.next; di->ppEnabledExtensionNames = D.exts; di->pNext = &D.f2;
    printf("nvn_vk: features: vertex input dynamic %d, custom border colour %d, provoking vertex last %d, 8-bit indices %d, list restart %d, ASTC %d, depth clamp %d, wide lines %d, robust buffers %d\n",
           D.has_vids, D.has_cbc, D.has_pv, D.has_iu8, D.has_plr, (int)D.f2.features.textureCompressionASTC_LDR, (int)D.f2.features.depthClamp, (int)D.f2.features.wideLines, (int)D.f2.features.robustBufferAccess);
}

/* ---------------------------------------------------------------- memory: 64 MB blocks per memory type and class, first fit */
typedef struct { int block; uint64_t off, size; void *map; VkDeviceMemory mem; } VMem;
typedef struct { VkDeviceMemory mem; uint64_t size; void *map; uint32_t type; int cls, nfree, used; struct { uint64_t off, size; } fr[512]; } VBlock;
static VBlock *g_blk[2048];
static int g_nblk;
static LONG64 g_mem_total, g_mem_leak;
static VMem vmem_alloc(VkMemoryRequirements rq, VkFlags want, int cls) {
    VMem m = {-1, 0, 0, NULL, NULL};
    uint32_t type = 32;
    for (uint32_t i = 0; i < D.mp.memoryTypeCount && type == 32; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) type = i;
    if (type == 32) vkd_fail("no memory type for flags 0x%x (bits 0x%x)", (unsigned)want, (unsigned)rq.memoryTypeBits);
    uint64_t al = rq.alignment < 256 ? 256 : rq.alignment;
    for (int pass = 0; pass < 2; pass++) {
        for (int b = 0; b < g_nblk; b++) {
            VBlock *k = g_blk[b];
            if (!k || k->type != type || k->cls != cls) continue;
            for (int i = 0; i < k->nfree; i++) {
                uint64_t start = (k->fr[i].off + al - 1) / al * al, end = k->fr[i].off + k->fr[i].size;
                if (start + rq.size > end) continue;
                uint64_t head = start - k->fr[i].off, tail = end - (start + rq.size);
                if (head && tail) { if (k->nfree == 512) continue; memmove(&k->fr[i + 2], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree++; k->fr[i].size = head; k->fr[i + 1].off = start + rq.size; k->fr[i + 1].size = tail; }
                else if (head) k->fr[i].size = head;
                else if (tail) { k->fr[i].off = start + rq.size; k->fr[i].size = tail; }
                else { memmove(&k->fr[i], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree--; }
                m.block = b; m.off = start; m.size = rq.size; m.mem = k->mem; m.map = k->map ? (uint8_t *)k->map + start : NULL; k->used++;
                return m;
            }
        }
        if (pass) break;
        int b = 0;
        while (b < g_nblk && g_blk[b]) b++;
        if (b == 2048) vkd_fail("out of memory blocks");
        VBlock *k = calloc(1, sizeof *k);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        k->size = rq.size > (16u << 20) ? rq.size : (64u << 20); k->type = type; k->cls = rq.size > (16u << 20) ? 100 + b : cls;      /* large: a block of its own */
        ai.allocationSize = k->size; ai.memoryTypeIndex = type;
        VkResult r = vkAllocateMemory(V.device, &ai, NULL, &k->mem);
        if (r != VK_SUCCESS) vkd_fail("vkAllocateMemory of %llu MB (type %u): %s; %lld MB allocated so far", (unsigned long long)(k->size >> 20), type, vk_result_name(r), (long long)(g_mem_total >> 20));
        if (D.mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) VKC(vkMapMemory(V.device, k->mem, 0, VK_WHOLE_SIZE, 0, &k->map));
        k->nfree = 1; k->fr[0].off = 0; k->fr[0].size = k->size;
        g_blk[b] = k; if (b == g_nblk) g_nblk++;
        g_mem_total += (LONG64)k->size;
        cls = k->cls;
    }
    vkd_fail("allocation of %llu bytes failed in a fresh block", (unsigned long long)rq.size);
    return m;
}
static void vmem_free(VMem m) {
    if (m.block < 0) return;
    VBlock *k = g_blk[m.block];
    if (--k->used == 0 && k->cls >= 100) { vkFreeMemory(V.device, k->mem, NULL); g_mem_total -= (LONG64)k->size; free(k); g_blk[m.block] = NULL; return; }
    int i = 0;
    while (i < k->nfree && k->fr[i].off < m.off) i++;
    if (i > 0 && k->fr[i - 1].off + k->fr[i - 1].size == m.off) { k->fr[i - 1].size += m.size; if (i < k->nfree && k->fr[i].off == m.off + m.size) { k->fr[i - 1].size += k->fr[i].size; memmove(&k->fr[i], &k->fr[i + 1], sizeof k->fr[0] * (size_t)(k->nfree - i - 1)); k->nfree--; } return; }
    if (i < k->nfree && k->fr[i].off == m.off + m.size) { k->fr[i].off = m.off; k->fr[i].size += m.size; return; }
    if (k->nfree == 512) { g_mem_leak += (LONG64)m.size; return; }
    memmove(&k->fr[i + 1], &k->fr[i], sizeof k->fr[0] * (size_t)(k->nfree - i)); k->nfree++;
    k->fr[i].off = m.off; k->fr[i].size = m.size;
}
static VkBuffer vbuf_new(uint64_t size, VkFlags usage, VMem *mem) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    VkBuffer b;
    VkMemoryRequirements rq;
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateBuffer(V.device, &bi, NULL, &b));
    vkGetBufferMemoryRequirements(V.device, b, &rq);
    VkFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    {   /* Round "four players" (P3): SSBU_VK_BUFMEM (default 1, 0 = off) puts the buffers the GPU reads vertices
         * from (the stream ring) into a memory type that is DEVICE_LOCAL as well as host visible, when the device has one
         * whose heap is at least 2 GB (the whole video memory mapped; a 256 MB window is not enough for the ring). Without
         * it the first host-visible type is taken, which on a discrete GPU is system memory the GPU reads over the bus at
         * every draw. Nothing but 4-byte words per compute dispatch is read back from the ring by the CPU. */
        static int mode = -1;
        if (mode < 0) { const char *e = getenv("SSBU_VK_BUFMEM"); mode = e ? atoi(e) : 1; }      /* default 1 since run p15t_ica_bufmem / ica_final (state hash identical, 0 flashes, GPU 12.5 -> 9.7 ms); =0: as before */
        if (mode == 1 && (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            VkFlags dl = want | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++)
                if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & dl) == dl && D.mp.memoryHeaps[D.mp.memoryTypes[i].heapIndex].size >= (2ull << 30)) {
                    want = dl;
                    printf("nvn_vk: SSBU_VK_BUFMEM=1: a %llu MB buffer (usage 0x%x) in memory type %u (device local + host visible, heap %llu MB)\n", (unsigned long long)(size >> 20), (unsigned)usage, i,
                           (unsigned long long)(D.mp.memoryHeaps[D.mp.memoryTypes[i].heapIndex].size >> 20));
                    break;
                }
            if (want != dl) printf("nvn_vk: SSBU_VK_BUFMEM=1: no device-local host-visible memory type with a heap of 2 GB: a %llu MB buffer stays in system memory\n", (unsigned long long)(size >> 20));
        }
    }
    *mem = vmem_alloc(rq, want, 1);
    VKC(vkBindBufferMemory(V.device, b, mem->mem, mem->off));
    return b;
}

/* ---------------------------------------------------------------- command buffers in flight */
#define NSLOT 8
typedef struct { VkCommandBuffer cmd; VkFence fence; VkDescriptorPool pool[16]; int npool, ipool, pending; uint64_t serial; LONG64 ts_frame; } Slot;
typedef struct VTex VTex;
static struct {
    Slot slot[NSLOT]; int cur, recording; uint64_t serial, done;      /* serial: number of the last submission; done: every submission up to here has finished */
    VkCommandBuffer cmd;
    /* stream ring */
    VkBuffer ring; VMem ring_mem; uint64_t ring_pos, ring_size; uint32_t gen;
    struct { uint64_t addr, size, off; uint32_t gen; } acache[8192];
    VkBuffer zero; VMem zero_mem;
    /* pass */
    int in_pass, pass_w, pass_h;
    int pass_rs; float pass_sx, pass_sy;           /* track O: the pass draws into scaled images (pass_w, pass_h are the images'; sx, sy = image / game) */
    /* NVN state as left by the commands executed so far */
    TexPool *tpool; SamplerPool *spool;
    Prog *prog[6];
    Range ubo[6][16], ssbo[6][32], vbuf[32];
    uint64_t nvn_tex[6][32], c2[6][112];
    uint32_t tex_int[6], tex_sint[6];
    uint32_t vattr[32]; int nattr;
    struct { uint32_t stride, divisor; } vstream[32];
    uint64_t ds; uint32_t chanmask, sample_mask;
    int sref[2], svmask[2], swmask[2];
    float alpha_ref; int alpha_func;
    uint8_t blend[8][8], blend_on, logic, poly[4], ms[24];
    float viewport[4], drange[2], blendc[4], point_size, line_width, polyoff[3];
    int scissor[4], have_viewport, have_scissor, rast_discard, depth_clamp, restart, render_off;
    int ncol; TargetRef col[8], depth; int have_targets;
    int64_t row_stride, image_stride;
    int dyn_dirty, vi_dirty, dyn3_dirty, dyn3_samples;      /* dyn3: the extended-dynamic-state-3 group (blend, masks, polygon mode, multisample) */
    void *pipe_bound;
    LONG64 frame_draws, frame_skipped, frame_clears, pipelines, modules, uploads, upload_bytes, images, images_live, views, ring_wraps, feedback, flushes, sets;
    LONG64 sets_reused, tset_collisions, dummy_kind;
    LONG64 dispatches, cb_bytes, cb_syncs, cb_draws, cb_made, cb_waits, cb_wait_ticks, cb_guest_words, cb_conflict_words, readbacks;
} R;

static void pass_end(void) { if (R.in_pass) { vkCmdEndRendering(R.cmd); R.in_pass = 0; CKPT_OP("pass end (w, h)", 0, R.pass_w, R.pass_h, 0); } }
static LONG64 g_tm[12];                          /* ticks: 0 wait for a submission, 1 copy-back, 2 dispatch, 3 batch replay, 4 present, 5 pipelines, 6 modules, 7 copy-back wait at batch start, 8 draw preparation */
static LONG64 tk(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
/* Session 5, SSBU_VK_GPUTIME=1: two timestamp queries per command buffer (written at its first and after its last command),
 * read when the buffer has finished; "nvn_vk_gpu:" line every 300 frames. The span of a buffer includes the time the GPU
 * gave to other processes while it ran. */
static int g_ts_on = -1;
static VkQueryPool g_ts_pool;
static double g_ts_ns;
static LONG64 g_ts_n;
/* Session 9: the same per FRAME (the frame a buffer was recorded for), with the time between two presents and whether the frame
 * was a battle frame (compute dispatches and more than 120 draws): median / p99 of the battle frames at exit ("nvn_vk_gpu_fight:"). */
#define FRSTAT_N 40000
static float g_fr_gpu[FRSTAT_N], g_fr_wall[FRSTAT_N];
static uint8_t g_fr_fight[FRSTAT_N];
static LONG64 g_cur_frame, g_battle_frames;
/* Round 16 (P3 section 15), all test / switch code:
 * SSBU_VK_PASSTIME=1 (with SSBU_VK_GPUTIME=1): a timestamp at the start of every pass (and of every run of compute
 *   dispatches, and of the present blit); the time to the next mark of the same command buffer is that segment's GPU time.
 *   Summed per kind of pass over the frames with fighters: "nvn_vk_pass:" lines at exit.
 * SSBU_VK_SPLIT=<n>: the command buffer is given to the GPU at the first pass change after n draws instead of once per
 *   batch (the whole frame), so the GPU works on the frame's first passes while the render thread still records the rest.
 *   0 = one submission per batch as before.
 * "nvn_vk_timeline:" line (every 300 frames, frames with fighters): when, after the start of the batch replay, the first
 *   and the last submission of the frame were made, when the present job began, and the game's waits for a fence. */
#define PT_MAX 96
#define PT_KINDS 64
typedef struct { int w, h, ncol, cf, depth, win, what; } PtKind;      /* what: 0 pass, 1 compute dispatches, 2 present blit */
static int g_pt_on = -1, g_pt_nkind, g_split = -1;
static VkQueryPool g_pt_pool;
static PtKind g_pt_kind[PT_KINDS];
static double g_pt_ns[PT_KINDS];
static LONG64 g_pt_cnt[PT_KINDS], g_pt_draws[PT_KINDS], g_pt_frames, g_cmd_draws, g_splits;
static struct { int n, fight, last_what; uint8_t kind[PT_MAX]; LONG64 d0; } g_pt_slot[NSLOT];
/* Round 16, the owner's "something in our shaders": every frame with fighters in which a pipeline (a program pair in a
 * pass signature) is drawn for the FIRST time is counted with what that cost; SSBU_VK_FIRSTLOG=1 prints a line per such
 * frame ("nvn_vk_first:"); the sums are printed at exit in any case. linked = made at this draw by a link of ready stage
 * libraries; made = compiled at the draw or waited for; newsig = the pass signature (render-target formats) was new. */
static LONG64 g_fu_fr_n, g_fu_fr_linked, g_fu_fr_made, g_fu_fr_tk, g_fu_fr_newsig;
static LONG64 g_fu_frames, g_fu_total, g_fu_linked, g_fu_made, g_fu_newsig, g_fu_tk, g_fu_slow, g_fu_fight_frames, g_fu_fight_slow, g_fu_slow_tex;
#define VK_SPLIT_DEFAULT 0
static LONG64 g_tl_t0, g_tl_first, g_tl_last, g_tl_sum[8], g_tl_n;      /* sums: 0 first submission, 1 last submission, 2 present job, 3 fence waits (count), 4 fence wait ticks, 5 end of a fence wait after the batch start */
static void slot_wait(Slot *s) {
    if (!s->pending) return;
    LONG64 t0 = tk();
    VkResult r = vkWaitForFences(V.device, 1, &s->fence, VK_TRUE, 20000000000ull);
    g_tm[0] += tk() - t0;
    if (r != VK_SUCCESS) vkd_fail("a submitted command buffer did not finish within 20 s (%s): the GPU is stuck or the device is lost", vk_result_name(r));
    VKC(vkResetFences(V.device, 1, &s->fence));
    if (g_ts_on > 0) {                             /* SSBU_VK_GPUTIME=1: the GPU's own clock at the first and the last command of this buffer */
        uint64_t ts[2] = {0, 0};
        if (vkGetQueryPoolResults(V.device, g_ts_pool, (uint32_t)(s - R.slot) * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && ts[1] >= ts[0]) { double ns = (double)(ts[1] - ts[0]) * (double)V.props.limits.timestampPeriod; g_ts_ns += ns; g_ts_n++; if (s->ts_frame >= 0 && s->ts_frame < FRSTAT_N) g_fr_gpu[s->ts_frame] += (float)(ns / 1e6); }
        int si = (int)(s - R.slot);
        if (g_pt_on > 0 && g_pt_slot[si].n) {      /* SSBU_VK_PASSTIME: the segments of this command buffer */
            static uint64_t pt[PT_MAX];
            int n = g_pt_slot[si].n;
            if (ts[1] && vkGetQueryPoolResults(V.device, g_pt_pool, (uint32_t)si * PT_MAX, (uint32_t)n, sizeof pt[0] * (size_t)n, pt, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && g_pt_slot[si].fight)
                for (int i = 0; i < n; i++) {
                    uint64_t end = i + 1 < n ? pt[i + 1] : ts[1];
                    if (end >= pt[i]) { g_pt_ns[g_pt_slot[si].kind[i]] += (double)(end - pt[i]) * (double)V.props.limits.timestampPeriod; g_pt_cnt[g_pt_slot[si].kind[i]]++; }
                }
            g_pt_slot[si].n = 0;
        }
    }
    s->pending = 0;
    if (s->serial > R.done) R.done = s->serial;
}
static void garbage_collect(void);
static uint32_t g_tset_gen = 1;                   /* texture descriptor sets live as long as the command buffer whose pools they come from */
static int g_tset_used, g_tset_list_used;
static void cmd_begin(void) {
    if (R.recording) return;
    g_tset_gen++; g_tset_used = 0; g_tset_list_used = 0;
    R.cur = (R.cur + 1) % NSLOT;
    Slot *s = &R.slot[R.cur];
    slot_wait(s);
    for (int i = 0; i < s->npool; i++) VKC(vkResetDescriptorPool(V.device, s->pool[i], 0));
    s->ipool = 0;
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandBuffer(s->cmd, 0));
    VKC(vkBeginCommandBuffer(s->cmd, &bi));
    R.cmd = s->cmd; R.recording = 1; R.in_pass = 0; R.pipe_bound = NULL; R.dyn_dirty = R.vi_dirty = 1;
    if (g_ts_on < 0) {
        g_ts_on = getenv("SSBU_VK_GPUTIME") && atoi(getenv("SSBU_VK_GPUTIME")) && V.props.limits.timestampComputeAndGraphics;
        if (g_ts_on) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NSLOT * 2; VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_ts_pool)); }
    }
    s->ts_frame = g_cur_frame;
    if (g_ts_on > 0) { vkCmdResetQueryPool(s->cmd, g_ts_pool, (uint32_t)R.cur * 2, 2); vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_ts_pool, (uint32_t)R.cur * 2); }
    if (g_pt_on < 0) {
        g_pt_on = g_ts_on > 0 && getenv("SSBU_VK_PASSTIME") && atoi(getenv("SSBU_VK_PASSTIME"));
        if (g_pt_on) { VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; qi.queryType = VK_QUERY_TYPE_TIMESTAMP; qi.queryCount = NSLOT * PT_MAX; VKC(vkCreateQueryPool(V.device, &qi, NULL, &g_pt_pool)); }
    }
    if (g_pt_on > 0) { vkCmdResetQueryPool(s->cmd, g_pt_pool, (uint32_t)R.cur * PT_MAX, PT_MAX); g_pt_slot[R.cur].n = 0; g_pt_slot[R.cur].last_what = -1; }
    g_cmd_draws = 0;
    garbage_collect();
}
static volatile LONG g_in_fight;
/* SSBU_VK_PASSTIME: a mark in the command buffer being recorded (see the comment at PT_MAX). */
static void pt_mark(int w, int h, int ncol, int cf, int depth, int win, int what) {
    if (g_pt_on <= 0 || !R.recording) return;
    int si = R.cur, k;
    if (g_pt_slot[si].n) g_pt_draws[g_pt_slot[si].kind[g_pt_slot[si].n - 1]] += g_cmd_draws - g_pt_slot[si].d0;
    g_pt_slot[si].d0 = g_cmd_draws;
    if (g_pt_slot[si].n == PT_MAX) return;
    for (k = 0; k < g_pt_nkind; k++) { PtKind *q = &g_pt_kind[k]; if (q->w == w && q->h == h && q->ncol == ncol && q->cf == cf && q->depth == depth && q->win == win && q->what == what) break; }
    if (k == g_pt_nkind) { if (k == PT_KINDS) k = PT_KINDS - 1; else { PtKind q = {w, h, ncol, cf, depth, win, what}; g_pt_kind[k] = q; g_pt_nkind++; } }
    vkCmdWriteTimestamp(R.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pt_pool, (uint32_t)(si * PT_MAX + g_pt_slot[si].n));
    g_pt_slot[si].kind[g_pt_slot[si].n++] = (uint8_t)k; g_pt_slot[si].fight = g_in_fight; g_pt_slot[si].last_what = what;
}
/* Ends the command buffer being recorded and gives it to the queue. Returns its number (the last one when nothing was recorded). */
static uint64_t cmd_submit(VkSemaphore wait, VkSemaphore signal) {
    if (!R.recording) { if (!wait && !signal) return R.serial; cmd_begin(); }
    pass_end();
    Slot *s = &R.slot[R.cur];
    if (g_ts_on > 0) vkCmdWriteTimestamp(s->cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_ts_pool, (uint32_t)R.cur * 2 + 1);
    if (g_pt_on > 0 && g_pt_slot[R.cur].n) g_pt_draws[g_pt_slot[R.cur].kind[g_pt_slot[R.cur].n - 1]] += g_cmd_draws - g_pt_slot[R.cur].d0;
    if (g_tl_t0) { LONG64 now = tk(); if (!g_tl_first) g_tl_first = now; g_tl_last = now; }
    CKPT_OP("end of the command buffer (submission)", 0, (int)(R.serial + 1), 0, 0);
    VKC(vkEndCommandBuffer(s->cmd));
    VkFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo sb = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (wait) { sb.waitSemaphoreCount = 1; sb.pWaitSemaphores = &wait; sb.pWaitDstStageMask = &stage; }
    if (signal) { sb.signalSemaphoreCount = 1; sb.pSignalSemaphores = &signal; }
    sb.commandBufferCount = 1; sb.pCommandBuffers = &s->cmd;
    VKC(vkQueueSubmit(V.queue, 1, &sb, s->fence));
    s->pending = 1; s->serial = ++R.serial;
    R.recording = 0; R.cmd = NULL; R.flushes++;
    return R.serial;
}
static void cmd_wait_all(void) { cmd_submit(NULL, NULL); for (int i = 0; i < NSLOT; i++) slot_wait(&R.slot[i]); R.done = R.serial; }

/* ---------------------------------------------------------------- stream ring */
#define RING_ALIGN 256
static void cb_ring_wrap(void);
static uint64_t ring_raw(const void *p, uint64_t n) {
    R.ring_pos = (R.ring_pos + RING_ALIGN - 1) & ~(uint64_t)(RING_ALIGN - 1);
    if (n > R.ring_size / 4) vkd_fail("a range of %llu bytes does not fit the stream ring (%llu MB)", (unsigned long long)n, (unsigned long long)(R.ring_size >> 20));
    if (R.ring_pos + n > R.ring_size) { cmd_wait_all(); cb_ring_wrap(); R.ring_pos = 0; R.gen++; R.ring_wraps++; }     /* everything that reads the ring has finished */
    uint64_t off = R.ring_pos;
    if (p) memcpy((uint8_t *)R.ring_mem.map + off, p, (size_t)n);
    R.ring_pos += n;
    return off;
}
static uint64_t ring_put(uint64_t addr, uint64_t size) {
    unsigned h = (unsigned)(((addr >> 4) ^ (size * 0x9E3779B1u)) * 2654435761u) >> 19;
    for (int probe = 0; probe < 8; probe++) {
        unsigned i = (h + (unsigned)probe) & 8191;
        if (R.acache[i].gen == R.gen && R.acache[i].addr == addr && R.acache[i].size == size) return R.acache[i].off;
        if (R.acache[i].gen != R.gen) { h = i; break; }
    }
    uint64_t off = ring_raw(P(addr), size);
    h &= 8191;
    R.acache[h].addr = addr; R.acache[h].size = size; R.acache[h].off = off; R.acache[h].gen = R.gen;
    return off;
}

/* ---------------------------------------------------------------- formats */
static VkFormat comp_fmt(int comps, int i) {
    static const int b8[4] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}, b16[4] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
                     b32[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    static const struct { uint8_t w, o; } m[12] = {{8, 0}, {8, 1}, {8, 4}, {8, 5}, {16, 6}, {16, 0}, {16, 1}, {16, 4}, {16, 5}, {32, 2}, {32, 0}, {32, 1}};      /* offsets: UNORM 0 SNORM 1 USCALED 2 SSCALED 3 UINT 4 SINT 5 SFLOAT 6; 32-bit: UINT 0 SINT 1 SFLOAT 2 */
    return (VkFormat)((m[i].w == 8 ? b8 : m[i].w == 16 ? b16 : b32)[comps - 1] + m[i].o);
}
static int fmt_supported(VkFormat f, VkFlags need) { VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(V.gpu, f, &p); return (p.optimalTilingFeatures & need) == need; }
/* NVN texture format -> Vulkan. *expand = 1 + component kind when a 3-component format is stored with 4; *decode = 1 when the texels are decoded to RGBA8 on the CPU. */
static VkFormat tex_vkfmt(int nf, int *expand, int *decode) {
    static int d24 = -1, d24s8, astc;
    if (d24 < 0) { d24 = fmt_supported(VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT); d24s8 = fmt_supported(VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT); astc = D.f2.features.textureCompressionASTC_LDR != 0;
        if (getenv("SSBU_VK_TEST") && (atoi(getenv("SSBU_VK_TEST")) & 8)) { d24 = d24s8 = 0; printf("nvn_vk: TEST (SSBU_VK_TEST bit 8): 24-bit depth formats not used: D32_SFLOAT / D32_SFLOAT_S8 as on a GPU without them\n"); } }      /* session 14: the missing stage pieces on NVIDIA only */
    *expand = 0; *decode = 0;
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) {
        int comps = nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, i = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        if (comps == 3) { *expand = 1 + i; comps = 4; }
        return comp_fmt(comps, i);
    }
    if (nf >= 0x79 && nf <= 0x94) {
        if (astc) return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * ((nf - 0x79) % 14) + (nf >= 0x87));
        *decode = 1;
        return nf >= 0x87 ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    }
    switch (nf) {
    case 0x31: return VK_FORMAT_S8_UINT; case 0x32: return VK_FORMAT_D16_UNORM; case 0x33: return d24 ? VK_FORMAT_X8_D24_UNORM_PACK32 : VK_FORMAT_D32_SFLOAT; case 0x34: return VK_FORMAT_D32_SFLOAT;
    case 0x35: return d24s8 ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT; case 0x36: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 0x37: case 0x38: return VK_FORMAT_R8G8B8A8_SRGB; case 0x39: return VK_FORMAT_R4G4B4A4_UNORM_PACK16; case 0x3A: case 0x3B: return VK_FORMAT_R5G5B5A1_UNORM_PACK16;
    case 0x3C: return VK_FORMAT_R5G6B5_UNORM_PACK16; case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32; case 0x40: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 0x41: return VK_FORMAT_BC1_RGB_UNORM_BLOCK; case 0x42: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 0x43: return VK_FORMAT_BC2_UNORM_BLOCK; case 0x44: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 0x45: return VK_FORMAT_BC1_RGB_SRGB_BLOCK; case 0x46: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK; case 0x47: return VK_FORMAT_BC2_SRGB_BLOCK; case 0x48: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 0x49: return VK_FORMAT_BC4_UNORM_BLOCK; case 0x4A: return VK_FORMAT_BC4_SNORM_BLOCK; case 0x4B: return VK_FORMAT_BC5_UNORM_BLOCK; case 0x4C: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 0x4D: return VK_FORMAT_BC7_UNORM_BLOCK; case 0x4E: return VK_FORMAT_BC7_SRGB_BLOCK; case 0x4F: return VK_FORMAT_BC6H_SFLOAT_BLOCK; case 0x50: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case 0x95: return VK_FORMAT_B5G6R5_UNORM_PACK16; case 0x99: case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM; case 0x9B: case 0x9C: return VK_FORMAT_B8G8R8A8_SRGB;
    }
    return VK_FORMAT_UNDEFINED;
}
static VkFlags fmt_aspect(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
/* 2026-10-20 (render bugs V): NVN vertex format 0x69 (A2B10G10R10 SNORM: the stages' normals / tangents) on a GPU without it as
 * a vertex format, or on AMD (signed 2_10_10_10 fetches: alpha read as unsigned on older AMD hardware / drivers, broken
 * fetches in some Windows drivers; a suspect, NOT confirmed, of the player report of 2026-10-20: a stage drawn wrong on an
 * AMD GPU while the fighters were right): the attribute is fetched as R32_UINT and the vertex code decodes it
 * (mx_translate: bit 16 + n of the stage key's int_attrs). SSBU_VK_TEST bit 512 (test only): the same on any GPU. */
static int vfmt_snorm_emu(void) {
    static int on = -1;
    if (on < 0) {
        VkFormatProperties fp; memset(&fp, 0, sizeof fp);
        vkGetPhysicalDeviceFormatProperties(V.gpu, VK_FORMAT_A2B10G10R10_SNORM_PACK32, &fp);
        int test = getenv("SSBU_VK_TEST") && (atoi(getenv("SSBU_VK_TEST")) & 512);
        on = test || !(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) || V.props.vendorID == 0x1002;      /* AMD: 2_10_10_10 signed fetches have a history of driver faults (alpha read unsigned, broken fetches): decoded by us there */
        printf("nvn_vk: vertex format A2B10G10R10_SNORM (NVN 0x69): %s%s\n", (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "a vertex format on this GPU" : "NOT a vertex format on this GPU",
               on ? (V.props.vendorID == 0x1002 ? ": AMD: fetched as R32_UINT and decoded by the vertex code" : ": fetched as R32_UINT and decoded by the vertex code") : "");
    }
    return on;
}
static VkFormat vertex_vkfmt(int nf) {
    if (nf == 0x69 && vfmt_snorm_emu()) return VK_FORMAT_R32_UINT;
    if ((nf >= 0x01 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) return comp_fmt(nf >= 0x6D ? 4 : (nf - 1) / 12 + 1, nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12);
    if (nf >= 0x51 && nf <= 0x68) {
        int comps = (nf - 0x51) / 6 + 1, t = (nf - 0x51) % 6;
        if (t >= 4) return VK_FORMAT_UNDEFINED;                       /* 32-bit scaled: Vulkan has none */
        return (VkFormat)(comp_fmt(comps, t < 2 ? 0 : 5) + 2 + (t & 1));
    }
    switch (nf) {
    case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32; case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32; case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}

/* ---------------------------------------------------------------- images */
typedef struct VView { EView e; int vtype, attach; VkImageView view; struct VView *next; } VView;
struct VTex {
    uint32_t magic; Tex *t; VkImage img; VMem mem; VkFormat fmt; VkImageLayout layout; VkFlags aspect;
    int w, h, d, levels, layers, samples, expand, decode, is3d, feedback, bpb, bw, bh;
    int rs; float sx, sy;                          /* track O: rs = 1 when the image is scaled from the game's texture, smaller or larger (w, h are the image's; sx, sy = image / game; PR #8) */
    VView *views; VTex *next, *prev;
    VTex *shadow; int shadow_ok;                   /* 2026-10-21 (AMD render bugs): a copy of this depth target for the shaders that sample it while it is bound (depth_copy_on) */
};
/* ---- Track O: internal resolution scale (graphics.ini resolution_scale; SSBU_VK_SCALE wins; plan\phases\O1_options.md, session 7).
 * A texture of the screen class (2D, one layer, attachable, the game's size exactly 1920x1080 >> k, k = 0..6) gets an image
 * of round(size * scale); everything the game says in ITS pixels about such an image is converted where it is used:
 * viewport, scissor, clear rectangles, copy / blit / resolve / downsample regions, uploads and read-backs (through a
 * temporary image of the game's size), the crop of the present. The size table of the shaders (c2) and the regions the game
 * reads keep the game's sizes. Fragment stages are translated with MxIn.screen_scale (another hash: ssrc_hb). At 1.0 none
 * of this runs: every branch is behind RS_ON or VTex.rs / R.pass_rs, which are then 0. */
static float g_rs = -1.0f;
static LONG64 g_rs_images, g_rs_stages, g_rs_fcs, g_rs_tf, g_rs_up, g_rs_down, g_rs_blits;
static LONG64 g_rs_bytes;                          /* PR #8 review: device memory taken by the scaled images made (reported at exit: what a scale above 1.0 costs) */
#define RS_MAX 2.0f                                /* PR #8 (stephenjayakar): the upper limit of resolution_scale / SSBU_VK_SCALE (was 1.0; the PR's 4.0 capped at 2.0) */
static float rs_scale(void) {
    if (g_rs < 0) {
        const char *e = getenv("SSBU_VK_SCALE");
        float v = e && atof(e) > 0 ? (float)atof(e) : gfx_options()->scale;
        if (v < 0.25f) v = 0.25f;
        /* PR #8 (stephenjayakar): above 1.0 = supersampling (the present reduces the picture in 2:1 steps, rs_reduce). The PR
         * allowed up to 4.0; capped at 2.0 here (review 2026-10-16): 2.0 is the 4K case that was measured, the screen-class
         * targets' memory grows with the square of the scale and a failed vkAllocateMemory is fatal (vmem_alloc -> vkd_fail) */
        if (v > RS_MAX) v = RS_MAX;
        g_rs = v;
    }
    return g_rs;
}
#define RS_ON (rs_scale() != 1.0f)
static int rs_screen_class(int w, int h) { for (int k = 0; k < 7; k++) if (w == 1920 >> k && h == 1080 >> k) return 1; return 0; }
/* A coordinate of the game's grid on a scaled image (nearest image texel edge, inside 0..max). */
static int rs_edge(int v, float s, int max) { int r = (int)floorf((float)v * s + 0.5f); return r < 0 ? 0 : r > max ? max : r; }
/* Session 9: the textures of the game's window (nvn_gl.c win_initialize keeps the list; also noted here at every present).
 * SSBU_VK_UI_NATIVE=1 (resolution scale, experiment): a window texture is NOT scaled, so what the game draws straight into it
 * (its last full-screen pass and the 2D interface after it) is drawn at the game's 1920x1080. */
int nvn_front_tex_is_window(const Tex *t);
static Tex *g_wintex[8];
static LONG64 g_rs_mixed;
static int wintex_is(const Tex *t) { if (nvn_front_tex_is_window(t)) return 1; for (int i = 0; i < 8; i++) if (g_wintex[i] == t) return 1; return 0; }
static void wintex_note(Tex *t) { if (!t || wintex_is(t)) return; for (int i = 0; i < 8; i++) if (!g_wintex[i]) { g_wintex[i] = t; return; } }
static int rs_ui_native(void) { static int on = -1; if (on < 0) on = getenv("SSBU_VK_UI_NATIVE") && atoi(getenv("SSBU_VK_UI_NATIVE")); return on; }
static VTex *g_vtex_all;
static struct { VTex *vt; uint64_t serial; } *g_garbage;
static int g_ngarbage, g_garbage_cap;
static VkFlags layout_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT; case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT; case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case VK_IMAGE_LAYOUT_GENERAL: return VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    default: return 0;
    }
}
static void image_barrier(VkImage img, VkFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    DBG_ASSERT(!R.in_pass, "debug: an image layout change inside a render pass");
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange.aspectMask = aspect; b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS; b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    b.srcAccessMask = layout_access(from); b.dstAccessMask = layout_access(to); b.oldLayout = from; b.newLayout = to;
    CKPT_OP("layout change (image, from, to)", img, from, to, aspect);
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void vt_layout(VTex *vt, VkImageLayout to) {
    if (vt->layout == to) return;
    pass_end();
    cmd_begin();
    image_barrier(vt->img, vt->aspect, vt->layout, to);
    vt->layout = to;
}
static LONG64 g_depth_copies;                         /* depth_shadow: copies made */
static void vt_check(VTex *vt) { if (vt->magic != 0x56544558) vkd_fail("an image is used after it was destroyed (texture record %p)", (void *)vt); }
static void vt_destroy(VTex *vt) {
    if (vt->shadow) {
        VTex *sh = vt->shadow;
        for (VView *v = sh->views, *n; v; v = n) { n = v->next; vkDestroyImageView(V.device, v->view, NULL); free(v); }
        vkDestroyImage(V.device, sh->img, NULL); vmem_free(sh->mem); free(sh); vt->shadow = NULL; R.images_live--;
    }
    for (VView *v = vt->views, *n; v; v = n) { n = v->next; vkDestroyImageView(V.device, v->view, NULL); free(v); }
    vkDestroyImage(V.device, vt->img, NULL);
    vmem_free(vt->mem);
    if (vt->prev) vt->prev->next = vt->next; else g_vtex_all = vt->next;
    if (vt->next) vt->next->prev = vt->prev;
    vt->magic = 0xDEADDEAD; R.images_live--;
    free(vt);
}
static void garbage_collect(void) {
    int n = 0;
    LONG64 t0 = g_ngarbage ? tk() : 0;
    for (int i = 0; i < g_ngarbage; i++) if (g_garbage[i].serial <= R.done) vt_destroy(g_garbage[i].vt); else g_garbage[n++] = g_garbage[i];
    if (t0 && g_ngarbage - n >= 50) {              /* session 8: what destroying many images at once costs (the end of a screen) */
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double ms = (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart;
        static int nl;
        if (ms > 10.0 && nl++ < 40) printf("nvn_vk_tex: frame %lld: %d images destroyed in %.1f ms\n", (long long)nvn_front_frame_next(), g_ngarbage - n, ms);
    }
    g_ngarbage = n;
}
static void vk_drop(int kind, void *host) {
    if (kind != K_TEX) return;
    Tex *t = host;
    VTex *vt = t->vk;
    if (!vt) return;
    t->vk = NULL;
    for (int i = 0; i < 8; i++) if (R.col[i].tex == t) { pass_end(); R.col[i].tex = NULL; }
    if (R.depth.tex == t) { pass_end(); R.depth.tex = NULL; }
    if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
    g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;       /* the command buffer being recorded may still name it */
}
static int lvl_(int v, int l) { v >>= l; return v > 0 ? v : 1; }
/* Track O. A temporary image of w x h (one level, one layer, the format of `like`) for an upload into, or a read-back from,
 * an image that is smaller than the game's texture. The command buffer is being recorded; the image goes when it has finished. */
static VTex *rs_temp(VTex *like, int w, int h) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    vt->magic = 0x56544558; vt->fmt = like->fmt; vt->aspect = like->aspect; vt->w = w; vt->h = h; vt->d = 1; vt->levels = 1; vt->layers = 1; vt->samples = 1;
    vt->bpb = like->bpb; vt->bw = like->bw; vt->bh = like->bh;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = vt->fmt; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    R.images_live++;
    if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
    g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;
    return vt;
}
/* PR #8 (stephenjayakar): track O, scale above 1 (supersampling): the present's reduction chain. One linear blit samples 2x2
 * texels per window pixel only when the picture is at most twice the window's size: a larger picture (2.0 in a window smaller
 * than the screen) would lose the samples between and alias. So the picture is first halved, each step a linear blit of
 * exactly 2:1 (the mean of 2x2 texels), until it is at most twice the window; then the usual blit. The images stay from frame
 * to frame; one of another size goes to the garbage list (the frames in flight may still read it). */
static VTex *g_rs_chain[6];
static VTex *rs_chain_image(int i, VTex *like, int w, int h) {
    VTex *vt = g_rs_chain[i];
    if (vt && vt->w == w && vt->h == h && vt->fmt == like->fmt) return vt;
    if (vt) {
        if (g_ngarbage == g_garbage_cap) { g_garbage_cap = g_garbage_cap ? g_garbage_cap * 2 : 256; g_garbage = realloc(g_garbage, sizeof *g_garbage * (size_t)g_garbage_cap); }
        g_garbage[g_ngarbage].vt = vt; g_garbage[g_ngarbage++].serial = R.serial + 1;
    }
    vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    vt->magic = 0x56544558; vt->fmt = like->fmt; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT; vt->w = w; vt->h = h; vt->d = 1; vt->levels = 1; vt->layers = 1; vt->samples = 1;
    vt->bpb = like->bpb; vt->bw = like->bw; vt->bh = like->bh;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = vt->fmt; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    R.images_live++;
    return g_rs_chain[i] = vt;
}
/* PR #8 (stephenjayakar): halves the rectangle s4 (x, y, w, h) of *src while it is more than twice dw x dh; *src and s4 then
 * name the last step. The command buffer is being recorded, no pass is open, *src is in TRANSFER_SRC_OPTIMAL. */
static LONG64 g_rs_reduce_steps, g_rs_reduce_skipped;
static void rs_reduce(VTex **src, int s4[4], int dw, int dh) {
    if (s4[2] <= 2 * dw && s4[3] <= 2 * dh) return;
    /* PR #8 review: the steps are linear blits between images of the window texture's format: without blit and linear-filter
     * support for that format the picture goes to the window with the single present blit, as before (counted at exit) */
    if (!fmt_supported((*src)->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) { g_rs_reduce_skipped++; return; }
    for (int i = 0; i < 6 && (s4[2] > 2 * dw || s4[3] > 2 * dh); i++) {
        int nw = s4[2] > 2 * dw ? (s4[2] + 1) / 2 : s4[2], nh = s4[3] > 2 * dh ? (s4[3] + 1) / 2 : s4[3];
        VTex *d = rs_chain_image(i, *src, nw, nh);
        VkImageBlit b;
        image_barrier(d->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
        b.srcOffsets[0].x = s4[0]; b.srcOffsets[0].y = s4[1]; b.srcOffsets[1].x = s4[0] + s4[2]; b.srcOffsets[1].y = s4[1] + s4[3]; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = nw; b.dstOffsets[1].y = nh; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, (*src)->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        image_barrier(d->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        d->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        *src = d; s4[0] = s4[1] = 0; s4[2] = nw; s4[3] = nh;
        g_rs_reduce_steps++;
    }
}
static VkFilter rs_filter(VTex *vt) { return vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
/* Texels of the game's grid (w x h at `off` of the stream ring, for the rectangle x, y of one level) into a scaled image:
 * through a temporary image of the game's size and a filtered blit. The command buffer is being recorded, no pass is open. */
static void rs_upload(VTex *vt, int level, int layer, uint64_t off, uint32_t rowlen, uint32_t rows, int x, int y, int w, int h) {
    int lw = lvl_(vt->w, level), lh = lvl_(vt->h, level);
    VkBufferImageCopy c;
    VkImageBlit b;
    if (w <= 0 || h <= 0) return;
    if (!fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) { nvn_vk_missing("resolution scale: an upload into a scaled texture whose format cannot be blitted"); return; }
    VTex *tmp = rs_temp(vt, w, h);
    memset(&c, 0, sizeof c); memset(&b, 0, sizeof b);
    c.bufferOffset = off; c.bufferRowLength = rowlen; c.bufferImageHeight = rows;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
    vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(R.cmd, R.ring, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1;
    b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.dstSubresource.mipLevel = (uint32_t)level; b.dstSubresource.baseArrayLayer = (uint32_t)layer; b.dstSubresource.layerCount = 1;
    b.srcOffsets[1].x = w; b.srcOffsets[1].y = h; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = rs_edge(x, vt->sx, lw); b.dstOffsets[0].y = rs_edge(y, vt->sy, lh); b.dstOffsets[1].x = rs_edge(x + w, vt->sx, lw); b.dstOffsets[1].y = rs_edge(y + h, vt->sy, lh); b.dstOffsets[1].z = 1;
    if (b.dstOffsets[1].x <= b.dstOffsets[0].x || b.dstOffsets[1].y <= b.dstOffsets[0].y) return;
    vkCmdBlitImage(R.cmd, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, rs_filter(vt));
    g_rs_up++;
}
static VTex *g_put_vt;
static uint32_t g_fmt_hist[256];                   /* images made per NVN format (printed with the totals) */
static void vt_put(Tex *t, int level, int layer, int w, int h, int d, const void *data, size_t size, int rgba8, int64_t row_stride) {
    VTex *vt = g_put_vt;
    uint8_t *tmp = NULL;
    (void)t;
    if (level >= vt->levels) return;
    if (vt->expand && !rgba8) {                    /* 3 components -> 4, the fourth is one */
        static const uint8_t csize[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4};
        static const uint32_t one[12] = {255, 127, 1, 1, 0x3C00, 65535, 32767, 1, 1, 0x3F800000, 1, 1};
        int cs = csize[vt->expand - 1];
        size_t n = (size_t)w * h * d, stride = row_stride ? (size_t)row_stride : (size_t)w * 3 * cs;
        tmp = malloc(n * 4 * cs);
        for (size_t y = 0; y < (size_t)h * d; y++)
            for (size_t x = 0; x < (size_t)w; x++) { uint8_t *o = tmp + (y * w + x) * 4 * cs; memcpy(o, (const uint8_t *)data + y * stride + x * 3 * cs, 3 * (size_t)cs); memcpy(o + 3 * cs, &one[vt->expand - 1], (size_t)cs); }
        data = tmp; size = n * 4 * cs; row_stride = 0;
    }
    uint64_t off = ring_raw(data, size);           /* may submit what was recorded so far */
    free(tmp);
    pass_end(); cmd_begin();
    if (vt->rs) {                                  /* track O: the texture's own memory holds the game's size */
        if (!vt->is3d && (uint32_t)layer < (uint32_t)vt->layers) rs_upload(vt, level, layer, off, row_stride && vt->bpb ? (uint32_t)(row_stride / vt->bpb) : 0, 0, 0, 0, w, h);
        R.upload_bytes += (LONG64)size;
        return;
    }
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    if (row_stride && vt->bpb) c.bufferRowLength = (uint32_t)(row_stride / vt->bpb);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = (uint32_t)(vt->is3d ? d : 1);
    if (!vt->is3d && (uint32_t)layer >= (uint32_t)vt->layers) return;
    CKPT_OP("upload (image, level, layer, w)", vt->img, level, layer, w);
    vkCmdCopyBufferToImage(R.cmd, R.ring, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    R.upload_bytes += (LONG64)size;
}
/* The image of a texture: made at first use, and holding what the guest memory holds (unless the GPU wrote it). */
/* Session 8: textures made and read AHEAD of their first draw (tex_ahead, below), and what is still made at a draw. */
static LONG64 g_ta_n, g_ta_bytes, g_ta_tk, g_ta_max_tk, g_late_n, g_late_tk, g_late_fight_n, g_late_fight_tk, g_up_fr_tk, g_up_fr_bytes;
static int g_up_fr_n, g_ta_inside;
static volatile LONG g_in_fight;                   /* nvn_vk_m4.c (included further down) */
static VTex *vt_get2(Tex *t, int sync) {
    VTex *vt = t->vk;
    LONG64 tu0 = 0, ub0 = R.upload_bytes;
    if (!vt) {
        tu0 = tk();
        TexBuilder *b = &t->b;
        Fmt f;
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        vt = calloc(1, sizeof *vt);
        vt->magic = 0x56544558; vt->t = t;
        vt->fmt = tex_vkfmt(b->format, &vt->expand, &vt->decode);
        if (!vt->fmt || !nvn_front_fmt(b->format, &f)) { static char msg[80]; snprintf(msg, sizeof msg, "texture format 0x%x", b->format); nvn_vk_missing(msg); vt->fmt = VK_FORMAT_R8G8B8A8_UNORM; nvn_front_fmt(0x25, &f); }
        vt->bpb = vt->decode ? 4 : vt->expand ? f.bpb / 3 * 4 : f.bpb; vt->bw = vt->decode ? 1 : f.bw; vt->bh = vt->decode ? 1 : f.bh;
        vt->aspect = fmt_aspect(vt->fmt);
        vt->w = b->width > 0 ? b->width : 1; vt->h = b->height > 0 ? b->height : 1; vt->d = b->depth > 0 ? b->depth : 1;
        vt->levels = nvn_front_tex_levels(t); vt->layers = nvn_front_tex_layers(t); vt->samples = b->samples > 1 ? b->samples : 1; vt->is3d = b->target == 2;
        if (b->target == 0 || b->target == 3) { ci.imageType = VK_IMAGE_TYPE_1D; vt->h = 1; if (b->target == 3) vt->layers = b->height > 0 && b->depth <= 1 ? b->height : vt->layers; }
        else ci.imageType = vt->is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
        if (RS_ON && (b->target == 1 || b->target == 5) && vt->layers == 1 && !vt->decode && rs_screen_class(vt->w, vt->h) && !(rs_ui_native() && wintex_is(t)) &&
            (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || (f.kind != 4 && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)))) {      /* track O: a screen-class target */
            int gw = vt->w, gh = vt->h, lv = 1;
            vt->w = (int)((float)gw * rs_scale() + 0.5f); vt->h = (int)((float)gh * rs_scale() + 0.5f);
            if (vt->w < 1) vt->w = 1; if (vt->h < 1) vt->h = 1;
            vt->rs = 1; vt->sx = (float)vt->w / (float)gw; vt->sy = (float)vt->h / (float)gh;
            while ((vt->w >> lv) || (vt->h >> lv)) lv++;
            if (vt->levels > lv) vt->levels = lv;
            g_rs_images++;
        }
        ci.format = vt->fmt; ci.extent.width = (uint32_t)vt->w; ci.extent.height = (uint32_t)vt->h; ci.extent.depth = (uint32_t)(vt->is3d ? vt->d : 1);
        ci.mipLevels = (uint32_t)vt->levels; ci.arrayLayers = (uint32_t)(vt->is3d ? 1 : vt->layers); ci.samples = (VkSampleCountFlagBits)vt->samples; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        int can_color = vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && f.kind != 4 && !vt->decode && fmt_supported(vt->fmt, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
        if (can_color) ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (ci.imageType == VK_IMAGE_TYPE_2D && vt->w == vt->h && vt->layers >= 6 && vt->samples == 1) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(V.device, &ci, NULL, &vt->img);
        if (r != VK_SUCCESS) vkd_fail("vkCreateImage (NVN format 0x%x -> %d, target %d, %dx%dx%d, %d levels, %d layers, %d samples): %s", b->format, (int)vt->fmt, b->target, vt->w, vt->h, vt->d, vt->levels, vt->layers, vt->samples, vk_result_name(r));
        VkMemoryRequirements rq;
        vkGetImageMemoryRequirements(V.device, vt->img, &rq);
        if (vt->rs) g_rs_bytes += (LONG64)rq.size;      /* PR #8 review: what the scaled images take (0 at scale 1.0: vt->rs is never set) */
        vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
        vt->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
        t->vk = vt;
        R.images++; R.images_live++; nvn_front_count(5, 1);
        g_fmt_hist[b->format & 255]++;
        if (can_color || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {      /* an image nothing was written to reads as zero (the OpenGL driver's behaviour, which the comparison saw) */
            VkImageSubresourceRange sr = {vt->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            if (vt->samples == 1) {
                CKPT_OP("clear of a new image (image, w, h, levels)", vt->img, vt->w, vt->h, vt->levels);
                if (can_color) { VkClearColorValue z; memset(&z, 0, sizeof z); vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
                else { VkClearDepthStencilValue z = {0.0f, 0}; vkCmdClearDepthStencilImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
            }
        }
    }
    vt_check(vt);
    if (sync && nvn_front_tex_changed(t)) {
        if (!tu0) tu0 = tk();
        g_put_vt = vt;
        cmd_begin();
        if (!nvn_front_tex_read(t, vt->decode, vt_put)) { static int n; if (n++ < 20) printf("nvn_vk: texture %dx%d format 0x%x: its memory is not readable; not uploaded\n", t->b.width, t->b.height, t->b.format); }
        R.uploads++;
    }
    if (tu0 && !g_ta_inside) {                     /* made or read again at its use (a draw, a copy, a target): the frame pays */
        LONG64 d = tk() - tu0;
        g_up_fr_tk += d; g_up_fr_n++; g_up_fr_bytes += R.upload_bytes - ub0;
        g_late_n++; g_late_tk += d;
        if (g_in_fight) { g_late_fight_n++; g_late_fight_tk += d; }
    }
    return vt;
}
static VTex *vt_get(Tex *t) { return vt_get2(t, 1); }
/* Session 8, render thread, after every present. A texture used to get its image, and its texels read out of the game's
 * memory (hash, de-swizzle, copy into the stream ring), at the first draw that sampled it: the first frame of a fight paid
 * 37-50 ms for the stage's and the fighters' textures (runs s7fn2 / s7gn2, frames 1804-1823). Here every block-compressed
 * texture the game has made (nvnTextureInitialize) and not yet drawn with is given its image two frames after the render
 * thread first saw it, under a time budget per frame (4 ms outside a fight, 1 ms in one; one texture is always taken, so a
 * very large one can exceed it): the loading screen absorbs the work. Only compressed formats: they cannot be render
 * targets, so nothing the GPU is about to write is touched; if the game fills the memory later, the draw sees the change
 * (nvn_front_tex_changed) and reads it again, as before. SSBU_VK_TEXAHEAD=0 = as before (everything at the first draw). */
static int fight_recent(void);                     /* nvn_vk_m4.c */
static void present_progress(float frac);          /* session 11: the held loading screen presented again with the progress bar (defined above vk_present) */
static LONG64 g_ovl_presents, g_ovl_frames;        /* extra presents of a held loading screen; normal frames drawn with the thin bar */
static int holdlog_on(void);
static void tex_ahead(LONG64 frame) {
    static int on = -1;
    static Tex *done_head;
    static LONG64 fq;
    if (on < 0) { const char *e = getenv("SSBU_VK_TEXAHEAD"); LARGE_INTEGER f; QueryPerformanceFrequency(&f); fq = f.QuadPart; on = !(e && !atoi(e)); }
    if (!on) return;
    Tex *head = nvn_front_tex_first();
    if (head == done_head) return;
    int fr = fight_recent();
    LONG64 t0 = tk(), budget = fq * (fr ? 1 : 4) / 1000;
    int pending = 0, taken = 0;
    g_ta_inside = 1;
    for (Tex *t = head; t && t != done_head; t = t->all_next) {
        Fmt f;
        if (t->ahead_seen < 0) continue;
        if (t->vk || t->dead) { t->ahead_seen = -1; continue; }
        if (!t->ahead_seen) { t->ahead_seen = frame + 1; pending++; continue; }
        if (frame + 1 - t->ahead_seen < 2) { pending++; continue; }
        if (t->gpu_written || (!t->mem && !t->b.packaged) || t->b.samples > 1 || t->b.width <= 0 || t->b.height <= 0 || !nvn_front_fmt(t->b.format, &f) || f.kind != 4) { t->ahead_seen = -1; continue; }
        if (taken && tk() - t0 > budget) { pending++; continue; }
        if (fr) {
            /* in and around a fight (a frame of the last 600 had fighters) only what is certainly cheap: at most 512 KB, no
             * decoding on the CPU, no wrap of the stream ring (a wrap waits for the GPU). Measured without this rule (runs s8b
             * frame 3549, s8d 4297, s8f 3840: the result screen's textures arriving while the fight ends, in frames without
             * a dispatch): 15-22 ms after one present and a frame of 34-35 ms. The others wait for a frame far from a fight,
             * or are made at their use as before. */
            VTex probe;
            if (t->data_size > (512u << 10) || R.ring_pos + 2 * (uint64_t)t->data_size + (1u << 20) > R.ring_size) { pending++; continue; }
            tex_vkfmt(t->b.format, &probe.expand, &probe.decode);
            if (probe.decode) { pending++; continue; }
        }
        LONG64 b0 = R.upload_bytes;
        vt_get(t);
        t->ahead_seen = -1; taken++;
        g_ta_n++; g_ta_bytes += R.upload_bytes - b0;
    }
    g_ta_inside = 0;
    LONG64 d = tk() - t0;
    g_ta_tk += d; if (d > g_ta_max_tk) g_ta_max_tk = d;
    if (d * 1000 > 6 * fq && holdlog_on()) { static int nl; if (nl++ < 100) printf("nvn_vk_tex: frame %lld (%s): %d textures made ahead after the present in %.1f ms\n", (long long)frame, fr ? "in or near a fight" : "far from a fight", taken, (double)d * 1000.0 / (double)fq); }
    if (!pending) done_head = head;
}
/* tex_view_obj's rules: the effective view of `t` through `v`. */
static void eview_of(Tex *t, VTex *vt, const View *v, EView *e) {
    EView base;
    memset(&base, 0, sizeof base);
    base.target = t->b.target; base.format = t->b.format; base.nlvl = vt->levels; base.nlay = nvn_front_tex_layers(t); base.dsmode = t->b.dsmode;
    memcpy(base.swz, t->b.swz, 4);
    *e = base;
    if (!v || !(v->set & 0x80)) return;
    if (v->set & 1) { e->lvl0 = v->lvl0; e->nlvl = v->nlvl; }
    if (v->set & 2) { e->lay0 = v->lay0; e->nlay = v->nlay; }
    if ((v->set & 4) && v->format) e->format = v->format;
    if (v->set & 8) memcpy(e->swz, v->swz, 4);
    if (v->set & 16) e->dsmode = v->dsmode;
    if (v->set & 32) e->target = v->target;
    if (e->lvl0 < 0 || e->lvl0 >= base.nlvl) e->lvl0 = 0;
    if (e->nlvl <= 0 || e->lvl0 + e->nlvl > base.nlvl) e->nlvl = base.nlvl - e->lvl0;
    if (e->lay0 < 0 || e->lay0 >= base.nlay) e->lay0 = 0;
    if (e->nlay <= 0 || e->lay0 + e->nlay > base.nlay) e->nlay = base.nlay - e->lay0;
    if (t->b.target == 2 || t->b.samples > 1) { e->target = base.target; e->lay0 = 0; e->nlay = base.nlay; }
    if ((e->target == 1 || e->target == 7 || e->target == 0) && e->nlay > 1) e->nlay = 1;
    if (e->target == 8 && e->nlay != 6) e->target = e->nlay == 1 ? 1 : 4;
    if (e->target == 9 && e->nlay % 6) e->target = 4;
}
enum { DIM_1D, DIM_2D, DIM_3D, DIM_CUBE, DIM_1DA, DIM_2DA, DIM_CUBEA };
static const VkImageViewType k_vtype[7] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
static int target_dim(int target) { static const int m[10] = {DIM_1D, DIM_2D, DIM_3D, DIM_1DA, DIM_2DA, DIM_2D, DIM_2DA, DIM_2D, DIM_CUBE, DIM_CUBEA}; return target >= 0 && target < 10 ? m[target] : DIM_2D; }
/* dim < 0: an attachment view (one level, one layer, both aspects, no swizzle). */
static VkImageView vt_view(VTex *vt, const EView *e, int dim, int attach) {
    for (VView *n = vt->views; n; n = n->next) if (n->vtype == dim && n->attach == attach && !memcmp(&n->e, e, sizeof *e)) return n->view;
    VView *n = calloc(1, sizeof *n);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    static const VkComponentSwizzle sw[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    int ex, dec;
    n->e = *e; n->vtype = dim; n->attach = attach;
    ci.image = vt->img; ci.format = vt->fmt;
    if (e->format != vt->t->b.format && vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !vt->decode && !vt->expand) { VkFormat f = tex_vkfmt(e->format, &ex, &dec); if (f && !ex && !dec && fmt_aspect(f) == VK_IMAGE_ASPECT_COLOR_BIT) ci.format = f; }
    if (attach) {
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.subresourceRange.aspectMask = vt->aspect;
        ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = 1; ci.subresourceRange.baseArrayLayer = (uint32_t)(vt->is3d ? 0 : e->lay0); ci.subresourceRange.layerCount = 1;
        if (vt->is3d) nvn_vk_missing("a 3D texture as render target");
    } else {
        int lay = e->nlay;
        if (dim == DIM_1D || dim == DIM_2D || dim == DIM_3D) lay = 1; else if (dim == DIM_CUBE) lay = 6; else if (dim == DIM_CUBEA) lay -= lay % 6;
        ci.viewType = k_vtype[dim];
        ci.subresourceRange.aspectMask = vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (e->dsmode == 1 || vt->aspect == VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        ci.subresourceRange.baseMipLevel = (uint32_t)e->lvl0; ci.subresourceRange.levelCount = (uint32_t)e->nlvl; ci.subresourceRange.baseArrayLayer = (uint32_t)(vt->is3d ? 0 : e->lay0); ci.subresourceRange.layerCount = (uint32_t)(vt->is3d ? 1 : lay);
        ci.components.r = sw[e->swz[0] < 6 ? e->swz[0] : 0]; ci.components.g = sw[e->swz[1] < 6 ? e->swz[1] : 0]; ci.components.b = sw[e->swz[2] < 6 ? e->swz[2] : 0]; ci.components.a = sw[e->swz[3] < 6 ? e->swz[3] : 0];
    }
    VkResult r = vkCreateImageView(V.device, &ci, NULL, &n->view);
    if (r != VK_SUCCESS) vkd_fail("vkCreateImageView (type %d, format %d of image format %d, levels %d+%d, layers %d+%d of %d, attach %d): %s", (int)ci.viewType, (int)ci.format, (int)vt->fmt, e->lvl0, e->nlvl, e->lay0, e->nlay, vt->layers, attach, vk_result_name(r));
    n->next = vt->views; vt->views = n; R.views++;
    return n->view;
}

/* ---------------------------------------------------------------- samplers (shared by value, never deleted before shutdown) */
static struct { SamplerParams p; VkSampler s; } g_smp[2048];
static int g_nsmp;
static VkSampler g_smp_default;
static VkSampler sampler_get(const SamplerParams *p) {
    static const VkSamplerAddressMode wrap[8] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
                                                 VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    for (int i = 0; i < g_nsmp; i++) if (!memcmp(&g_smp[i].p, p, sizeof *p)) return g_smp[i].s;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo red = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO};
    int minf = p->min < 6 ? p->min : 1;          /* nearest, linear, nearest-mip-nearest, linear-mip-nearest, nearest-mip-linear, linear-mip-linear */
    ci.minFilter = (minf & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; ci.magFilter = p->mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = minf >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap[p->ws & 7]; ci.addressModeV = wrap[p->wt & 7]; ci.addressModeW = wrap[p->wr & 7];
    ci.mipLodBias = p->bias; ci.minLod = p->lodmin < 0 ? 0 : p->lodmin; ci.maxLod = p->lodmax;
    if (minf < 2) { ci.minLod = 0; ci.maxLod = 0.25f; }      /* no mip maps: level 0 only (the Vulkan specification's own mapping of GL_NEAREST / GL_LINEAR) */
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    float aniso = p->aniso;
    {   /* track O: anisotropic_filtering in graphics.ini = at least that on samplers that filter linearly between mip levels (not shadow-compare samplers) */
        static LONG64 raised;
        float want = (float)gfx_options()->aniso;
        if (want > aniso && minf >= 4 && p->cmode != 1) { aniso = want; raised++; if (raised == 1 || raised % 100 == 0) printf("nvn_vk: anisotropic_filtering=%d: %lld samplers raised so far (this one from %.0f)\n", gfx_options()->aniso, (long long)raised, p->aniso); }
    }
    if (aniso > 1.0f && D.f2.features.samplerAnisotropy) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = aniso > V.props.limits.maxSamplerAnisotropy ? V.props.limits.maxSamplerAnisotropy : aniso; }
    if (p->cmode == 1) { ci.compareEnable = VK_TRUE; ci.compareOp = (VkCompareOp)(p->cfunc ? (p->cfunc - 1) & 7 : 7); }
    {   /* the OpenGL path can only use four border colours (bindless handles: sampler_gl); the same rounding here */
        float b[4] = {p->border[0], p->border[1], p->border[2], p->border[3]}, mx = b[0] > b[1] ? b[0] : b[1];
        int rgb, al;
        if (b[2] > mx) mx = b[2];
        rgb = mx >= 0.5f; al = b[3] >= 0.5f;
        if (p->border_kind) { int32_t bi[4]; memcpy(bi, p->border, 16); rgb = bi[0] || bi[1] || bi[2]; al = bi[3] != 0; }
        if (!rgb) ci.borderColor = p->border_kind ? (al ? VK_BORDER_COLOR_INT_OPAQUE_BLACK : VK_BORDER_COLOR_INT_TRANSPARENT_BLACK) : (al ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
        else if (al) ci.borderColor = p->border_kind ? VK_BORDER_COLOR_INT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        else {
            int uses = (p->ws & 7) == 5 || (p->wt & 7) == 5 || (p->wr & 7) == 5;
            if (uses && !D.has_cbc) nvn_vk_missing("a sampler border colour (1, 1, 1, 0) on a GPU without VK_EXT_custom_border_color");
            if (D.has_cbc) {
                ci.borderColor = p->border_kind ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
                if (p->border_kind) { cb.customBorderColor.int32[0] = cb.customBorderColor.int32[1] = cb.customBorderColor.int32[2] = 1; }
                else { cb.customBorderColor.float32[0] = cb.customBorderColor.float32[1] = cb.customBorderColor.float32[2] = 1.0f; }
                cb.pNext = ci.pNext; ci.pNext = &cb;
            }
        }
    }
    if (p->reduction == 1 || p->reduction == 2) {
        if (!D.f12.samplerFilterMinmax) nvn_vk_missing("a min / max reduction sampler on a GPU without samplerFilterMinmax");
        else { red.reductionMode = p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX; red.pNext = ci.pNext; ci.pNext = &red; }
    }
    if (p->lod_snap != 0.0f) nvn_vk_missing("a sampler with LOD snap");
    VkSampler s;
    VKC(vkCreateSampler(V.device, &ci, NULL, &s));
    if (g_nsmp == 2048) vkd_fail("more than 2048 distinct samplers");
    g_smp[g_nsmp].p = *p; g_smp[g_nsmp++].s = s;
    return s;
}

/* MAC2 / V_novids (2026-10-21). A GPU without VK_EXT_vertex_input_dynamic_state (MoltenVK; an RX 550 / Polaris on AMD's
 * Windows driver): the vertex layout of a draw (per location: format, binding, offset; per binding: instance rate,
 * divisor) is part of the pipeline, the strides stay dynamic (VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE, Vulkan 1.3).
 * Where the layout lives in the key:
 *   - macOS: inside it (PKEY_VI = NvkVl vi: the MAC2 fields under one name, the same bytes), in pipes.bin as before;
 *   - elsewhere: the three pad bytes of the key hold the NUMBER of the layout (nvk_vl_id: 24 bits of a hash of the
 *     layout, the next free number on a collision), the layouts are in ../cache/vk/vi_layouts.bin, the keys in
 *     pipes_novi.bin. On a GPU that has the extension the pad bytes stay zero: PKey, pipes.bin and the prepared markers
 *     are those of before, to the byte. nvn_vk_m4.c only names PKEY_VI / NVK_VI_PIPE / VK_PIPES_BIN / nvk_vl_rec_ok on
 *     lines it already had (its WD_SLEEP uses __LINE__). */
typedef struct { uint32_t na, nb, rate; uint32_t loc[16], fmt[16], bind[16], off[16], div[17]; } NvkVl;
#ifdef __APPLE__
#define PKEY_VI NvkVl vi;
#else
#define PKEY_VI
#endif
typedef struct {
    VkVertexInputBindingDescription b[17];
    VkVertexInputAttributeDescription a[16];
    VkVertexInputBindingDivisorDescriptionEXT dd[17];
    VkPipelineVertexInputDivisorStateCreateInfoEXT di;
    VkPipelineVertexInputStateCreateInfo vi;
} NvkVi;
static void nvk_vi_pipe(const void *key, VkDynamicState *dyn, VkGraphicsPipelineCreateInfo *gi, NvkVi *s);
static int nvk_vl_rec_ok(const void *key);         /* a key of the pipeline list whose layout is known (else it is not prewarmed) */
#define NVK_VI_PIPE(k, dyn, gi) NvkVi nvk_vi_; nvk_vi_pipe(k, dyn, gi, &nvk_vi_);
#include "nvn_vk_m4.c"                             /* session 4: stages, pipelines, workers (M4) */
#ifndef __APPLE__
/* The layout table: 8192 slots, open addressing by number. Filled only by the render thread (vi_key) and at start
 * (nvk_vl_load); the workers read it (nvk_vi_pipe): a layout is written before its number, and a number reaches a worker
 * only inside a key queued under g_sh_lock. */
#define NVK_VL_SLOTS 8192
#define NVK_VL_MAGIC 0x31594C56u                   /* "VLY1" */
typedef struct { volatile LONG id; NvkVl l; } NvkVlEnt;
typedef struct { uint32_t magic, id; NvkVl l; } NvkVlRec;
static NvkVlEnt *g_vlt;
static int g_nvl, g_vl_coll;
static uint32_t nvk_vl_get(const void *key) { const PKey *k = key; return (uint32_t)k->pad[0] | (uint32_t)k->pad[1] << 8 | (uint32_t)k->pad[2] << 16; }
static void nvk_vl_set(PKey *k, uint32_t id) { k->pad[0] = (uint8_t)id; k->pad[1] = (uint8_t)(id >> 8); k->pad[2] = (uint8_t)(id >> 16); }
static NvkVlEnt *nvk_vl_slot(uint32_t id) {        /* the slot of number id, or the empty slot where it would go */
    for (uint32_t i = id & (NVK_VL_SLOTS - 1), n = 0; n < NVK_VL_SLOTS; n++, i = (i + 1) & (NVK_VL_SLOTS - 1))
        if ((uint32_t)g_vlt[i].id == id || !g_vlt[i].id) return &g_vlt[i];
    return NULL;
}
static const NvkVl *nvk_vl_find(uint32_t id) { NvkVlEnt *e = id && g_vlt ? nvk_vl_slot(id) : NULL; return e && (uint32_t)e->id == id ? &e->l : NULL; }
static void nvk_vl_put(NvkVlEnt *e, uint32_t id, const NvkVl *l) { e->l = *l; InterlockedExchange(&e->id, (LONG)id); g_nvl++; }      /* a full barrier: the layout is visible before its number */
static void nvk_vl_load(void) {                    /* start, before the prewarm reads pipes_novi.bin */
    g_vlt = calloc(NVK_VL_SLOTS, sizeof *g_vlt);
    if (!g_vlt) vkd_fail("no memory for the vertex layout table");
    FILE *fp = fopen(vkc_file(VK_VL_FILE), "rb");
    NvkVlRec r;
    int bad = 0;
    if (fp) {
        while (fread(&r, sizeof r, 1, fp) == 1) {
            NvkVlEnt *e;
            if (r.magic != NVK_VL_MAGIC || !r.id || r.id > 0xffffffu || g_nvl > NVK_VL_SLOTS * 3 / 4 || !(e = nvk_vl_slot(r.id))) { bad = 1; break; }
            if (!e->id) nvk_vl_put(e, r.id, &r.l);
        }
        fclose(fp);
    }
    printf("nvn_vk: vertex layouts of earlier runs (../cache/vk/" VK_VL_FILE "): %d%s\n", g_nvl, bad ? " (the file ends in a bad record: read up to it)" : "");
}
static uint32_t nvk_vl_id(const NvkVl *l) {        /* render thread: the number of this layout; a new one is written to the file */
    uint32_t id = (uint32_t)fnv(0xcbf29ce484222325ULL, l, sizeof *l) & 0xffffffu;
    if (!id) id = 1;
    for (int n = 0; n < NVK_VL_SLOTS; n++, id = id >= 0xffffffu ? 1 : id + 1) {
        NvkVlEnt *e = nvk_vl_slot(id);
        if (!e) break;
        if (e->id) { if (!memcmp(&e->l, l, sizeof *l)) return id; g_vl_coll++; continue; }      /* the number of another layout: the next one */
        if (g_nvl > NVK_VL_SLOTS * 3 / 4) break;
        nvk_vl_put(e, id, l);
        NvkVlRec r; memset(&r, 0, sizeof r); r.magic = NVK_VL_MAGIC; r.id = id; r.l = *l;
        FILE *fp = fopen(vkc_file(VK_VL_FILE), "ab");
        if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
        return id;
    }
    nvn_vk_missing("more than 6144 distinct vertex layouts without VK_EXT_vertex_input_dynamic_state");
    return 0;
}
static int nvk_vl_rec_ok(const void *key) { uint32_t id = nvk_vl_get(key); return D.has_vids || !id || nvk_vl_find(id) != NULL; }
#else
static int nvk_vl_rec_ok(const void *key) { (void)key; return 1; }
#endif
/* pipe_make without dynamic vertex input: the key's layout becomes the pipeline's; the strides come with the buffers */
static void nvk_vi_pipe(const void *key, VkDynamicState *dyn, VkGraphicsPipelineCreateInfo *gi, NvkVi *s) {
    static const NvkVl empty;
    uint32_t nvdd = 0;
    if (D.has_vids) return;
#ifdef __APPLE__
    const NvkVl *l = &((const PKey *)key)->vi;
#else
    const NvkVl *l = nvk_vl_find(nvk_vl_get(key));
    if (!l) l = &empty;                            /* number 0: a pipeline without vertex input (the stencil clear) */
#endif
    memset(s, 0, sizeof *s);
    s->di.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT;
    s->vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    dyn[19] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;         /* in place of VK_DYNAMIC_STATE_VERTEX_INPUT_EXT */
    for (uint32_t b = 0; b < l->nb && b < 17; b++) {
        int inst = l->rate >> b & 1;
        s->b[b].binding = b; s->b[b].stride = 0; s->b[b].inputRate = inst ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
        if (inst && l->div[b] > 1) {
            if (D.has_vad && D.vad.vertexAttributeInstanceRateDivisor) { s->dd[nvdd].binding = b; s->dd[nvdd].divisor = l->div[b]; nvdd++; }
            else nvn_vk_missing("an instance divisor above 1 without VK_EXT_vertex_attribute_divisor");
        }
    }
    for (uint32_t a = 0; a < l->na && a < 16; a++) { s->a[a].location = l->loc[a]; s->a[a].binding = l->bind[a]; s->a[a].format = (VkFormat)l->fmt[a]; s->a[a].offset = l->off[a]; }
    s->vi.vertexBindingDescriptionCount = l->nb < 17 ? l->nb : 17; s->vi.pVertexBindingDescriptions = s->b;
    s->vi.vertexAttributeDescriptionCount = l->na < 16 ? l->na : 16; s->vi.pVertexAttributeDescriptions = s->a;
    if (nvdd) { s->di.vertexBindingDivisorCount = nvdd; s->di.pVertexBindingDivisors = s->dd; s->vi.pNext = &s->di; }
    gi->pVertexInputState = &s->vi;
}
#if 0   /* SUPERSEDED by nvn_vk_m4.c (session 3's stages and pipelines made at the draw); kept only until someone deletes the block */
/* ---------------------------------------------------------------- stages: machine code -> Vulkan GLSL -> SPIR-V -> module */
typedef struct { uint8_t unit, dim, shadow; uint16_t binding; } VSmp;
typedef struct VStage {
    uint64_t h; int stage, code_len, ok; uint32_t key[4], fo, in_attr, out_attr, id, cbufs, ssbos, ssbo_w, images, dep, tex_units; int uses_alpha, nsmp;
    VkPipeline cp; VkPipelineLayout cl; VkDescriptorSetLayout cdl[2];      /* compute stage: its pipeline */
    uint32_t local[3];
    VSmp smp[64];
    VkShaderModule mod; struct VStage *next;
} VStage;
/* M4 (design section 5): what a stage needs to become a module again without the translator and glslang, in
 * ..\cache\vk\stage\<stage>_<hash of machine code, constants, variant, translator version>.st */
#define MX_VK_VERSION 3u                           /* part of the stage hash: change it when mx_translate's Vulkan text changes */
typedef struct { uint32_t magic; int32_t stage, code_len; uint32_t key[4], fo; uint64_t spv; uint32_t cbufs, ssbos, ssbo_w, images, dep, tex_units, in_attr, out_attr; int32_t uses_alpha, nsmp; VSmp smp[64]; } StageRec;
static CRITICAL_SECTION g_sh_lock;                 /* the stage and pipeline tables while the start-up workers run */
static VkPipelineCache g_pcache;
static LONG64 g_pw_stages, g_pw_pipes, g_pw_failed, g_pipe_miss;
static VStage *g_vstage[4096];
static uint32_t g_vstage_ids;
static int (*g_glsl_to_spirv)(int, const char *, uint32_t **, size_t *, char *, size_t);
static void (*g_spirv_free)(uint32_t *);
static uint64_t fnv(uint64_t h, const void *p, size_t n) { const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ULL; return h; }
static uint64_t g_spv_unused;
static VkShaderModule module_from_spv(uint64_t h) {              /* any thread; NULL when the file is not there */
    char path[128];
    VkShaderModule mod = NULL;
    snprintf(path, sizeof path, "../cache/vk/spv/%016llx.spv", (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    uint32_t *words = n > 20 && n % 4 == 0 ? malloc((size_t)n) : NULL;
    if (words && fread(words, 1, (size_t)n, fp) == (size_t)n && words[0] == 0x07230203) {
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = (size_t)n; ci.pCode = words;
        if (vkCreateShaderModule(V.device, &ci, NULL, &mod) != VK_SUCCESS) mod = NULL;
    }
    free(words); fclose(fp);
    return mod;
}
static void stage_rec_path(char *path, size_t n, int s, uint64_t h) { snprintf(path, n, "../cache/vk/stage/%d_%016llx.st", s, (unsigned long long)h); }
/* A stage from its record on disk (the caller holds g_sh_lock when workers run). NULL when there is none. */
static VStage *vstage_from_rec(int s, uint64_t h) {
    char path[128];
    StageRec r;
    stage_rec_path(path, sizeof path, s, h);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t got = fread(&r, 1, sizeof r, fp);
    fclose(fp);
    if (got != sizeof r || r.magic != 0x52545356 + MX_VK_VERSION || r.stage != s || r.nsmp < 0 || r.nsmp > 64) return NULL;
    VkShaderModule mod = module_from_spv(r.spv);
    if (!mod) return NULL;
    VStage *v = calloc(1, sizeof *v);
    v->h = h; v->stage = s; v->code_len = r.code_len; memcpy(v->key, r.key, 16); v->fo = r.fo; v->id = ++g_vstage_ids;
    v->cbufs = r.cbufs; v->ssbos = r.ssbos; v->ssbo_w = r.ssbo_w; v->images = r.images; v->dep = r.dep; v->tex_units = r.tex_units; v->in_attr = r.in_attr; v->out_attr = r.out_attr;
    v->uses_alpha = r.uses_alpha; v->nsmp = r.nsmp; memcpy(v->smp, r.smp, sizeof v->smp);
    v->mod = mod; v->ok = 1;
    v->next = g_vstage[h & 4095]; g_vstage[h & 4095] = v;
    InterlockedIncrement64(&R.modules);
    return v;
}
static VkShaderModule module_from_glsl(int stage, const char *glsl, const char *what, uint64_t *spv_hash) {
    char path[128], log[2000];
    uint32_t *words = NULL;
    size_t count = 0;
    int from_dll = 0;
    VkShaderModule mod = NULL;
    LONG64 t_mod = tk();
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)stage ^ 0x5601, glsl, strlen(glsl));
    *spv_hash = h;
    snprintf(path, sizeof path, "../cache/vk/spv/%016llx.spv", (unsigned long long)h);
    FILE *fp = fopen(path, "rb");
    if (fp) {
        fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (n > 20 && n % 4 == 0) { words = malloc((size_t)n); count = (size_t)n / 4; if (fread(words, 1, (size_t)n, fp) != (size_t)n || words[0] != 0x07230203) { free(words); words = NULL; } }
        fclose(fp);
    }
    if (!words) {
        if (!g_glsl_to_spirv) {
            HMODULE m = LoadLibraryA("ssbu_glslang.dll");
            g_glsl_to_spirv = m ? (void *)GetProcAddress(m, "ssbu_glsl_to_spirv") : NULL; g_spirv_free = m ? (void *)GetProcAddress(m, "ssbu_spirv_free") : NULL;
            if (!g_glsl_to_spirv || !g_spirv_free) vkd_fail("ssbu_glslang.dll cannot be loaded (build_glslang.bat makes it)");
        }
        if (!g_glsl_to_spirv(stage, glsl, &words, &count, log, sizeof log)) { printf("nvn_vk: %s: glslang rejected the translated text: %s\n", what, log); fflush(stdout); return NULL; }
        from_dll = 1;
        CreateDirectoryA("../cache/vk", NULL); CreateDirectoryA("../cache/vk/spv", NULL);
        fp = fopen(path, "wb");
        if (fp) { fwrite(words, 4, count, fp); fclose(fp); }
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = count * 4; ci.pCode = words;
    VKC(vkCreateShaderModule(V.device, &ci, NULL, &mod));
    if (from_dll) g_spirv_free(words); else free(words);
    InterlockedIncrement64(&R.modules);
    g_tm[6] += tk() - t_mod;
    return mod;
}
static VStage *vstage_get(Prog *p, int s, const uint32_t *key, uint32_t fo) {
    Stage *st = &p->st[s];
    VStage *v = st->vk;
    if (!st->code) return NULL;
    uint32_t dep_a = v && s == 0 ? v->dep | v->dep << 16 : 0;      /* 2026-10-20: bit 16 + n of key[0]: attribute n decoded from A2B10G10R10 SNORM (key_fits of nvn_vk_m4.c) */
    if (v && ((key[0] ^ v->key[0]) & dep_a) == 0 && ((key[1] ^ v->key[1]) & dep_a) == 0 && ((key[2] ^ v->key[2]) & v->tex_units) == 0 && ((key[3] ^ v->key[3]) & v->tex_units) == 0 && v->fo == fo) return v->ok ? v : NULL;
    const uint8_t *cdata = st->ctl[3] && st->ctl[4] ? st->code - (s == 5 ? 0 : 0x30) + st->ctl[4] : NULL;       /* a compute stage has no program header */
    uint64_t h = fnv(0xcbf29ce484222325ULL ^ (uint64_t)s, st->code, (size_t)st->code_len);
    if (s == 5) { h = fnv(h, st->local, sizeof st->local); h = fnv(h, &st->smem_bytes, 4); h = fnv(h, &st->lmem_bytes, 4); }
    if (cdata) h = fnv(h ^ st->ctl[3], cdata, st->ctl[3]);
    h = fnv(h, key, 16); h = fnv(h, &fo, 4);
    { uint32_t ver = MX_VK_VERSION; h = fnv(h, &ver, 4); }
    for (v = g_vstage[h & 4095]; v; v = v->next) if (v->h == h && v->stage == s && v->code_len == st->code_len && v->fo == fo && !memcmp(v->key, key, 16)) { st->vk = v; memcpy(v->local, st->local, sizeof v->local); return v->ok ? v : NULL; }
    {   LONG64 t_mod = tk();
        v = vstage_from_rec(s, h);                 /* met in an earlier run: no translator, no glslang */
        g_tm[6] += tk() - t_mod;
        if (v) { st->vk = v; memcpy(v->local, st->local, sizeof v->local); return v; }
    }
    MxIn in;
    MxOut out;
    char what[64];
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.stage = s; in.code = st->code; in.code_len = st->code_len; in.cdata = cdata; in.cdata_size = cdata ? st->ctl[3] : 0;
    in.int_attrs = key[0]; in.sint_attrs = key[1]; in.int_tex = key[2]; in.sint_tex = key[3]; in.force_out = fo;
    in.target = 1; in.binding_base = 40u * (unsigned)s; in.ssbo_base = 40u * (unsigned)s + 20; in.tex_base = 40u * (unsigned)s; in.image_base = 40u * (unsigned)s + 32; in.wmask_binding = 40u * (unsigned)s + 38;
    if (s == 5) {                                  /* compute has a layout of its own (design, section 4): the bindings of stage 0 */
        in.binding_base = 0; in.ssbo_base = 20; in.tex_base = 0; in.image_base = 32; in.wmask_binding = 38; in.track_writes = 1;
        memcpy(in.local_size, st->local, sizeof in.local_size); in.smem_bytes = st->smem_bytes; in.lmem_bytes = st->lmem_bytes;
    }
    v = calloc(1, sizeof *v);
    v->h = h; v->stage = s; v->code_len = st->code_len; memcpy(v->key, key, 16); v->fo = fo; v->id = ++g_vstage_ids;
    v->next = g_vstage[h & 4095]; g_vstage[h & 4095] = v;
    st->vk = v;
    snprintf(what, sizeof what, "program #%d %s stage", p->serial, s == 5 ? "compute" : s ? "fragment" : "vertex");
    if (!mx_translate(&in, &out)) { printf("nvn_vk: %s: machine code not translated: %s\n", what, out.why); fflush(stdout); return NULL; }
    uint64_t spv_hash = 0;
    v->mod = module_from_glsl(s, out.glsl, what, &spv_hash);
    memcpy(v->local, st->local, sizeof v->local);
    v->cbufs = out.cbufs; v->ssbos = out.ssbos; v->ssbo_w = out.ssbo_written; v->images = out.images; v->in_attr = out.in_attr; v->out_attr = out.out_attr; v->dep = s == 0 ? out.int_dep : 0; v->tex_units = out.tex_units; v->uses_alpha = out.uses_alpha; v->nsmp = out.nsmp;
    if (s == 0) v->dep = out.int_dep; else v->tex_units = out.tex_units | out.int_dep;
    v->tex_units = out.tex_units;
    for (int i = 0; i < out.nsmp && i < 64; i++) {
        const char *ty = out.smp[i].type;
        int arr = strstr(ty, "Array") != NULL, dim = strstr(ty, "Cube") ? (arr ? DIM_CUBEA : DIM_CUBE) : strstr(ty, "3D") ? DIM_3D : strstr(ty, "1D") ? (arr ? DIM_1DA : DIM_1D) : (arr ? DIM_2DA : DIM_2D);
        if (strstr(ty, "MS") || strstr(ty, "Rect") || strstr(ty, "Buffer")) { static char msg[96]; snprintf(msg, sizeof msg, "a shader sampler of type %s", ty); nvn_vk_missing(msg); }
        v->smp[i].unit = out.smp[i].unit; v->smp[i].dim = (uint8_t)dim; v->smp[i].shadow = out.smp[i].shadow; v->smp[i].binding = out.smp[i].binding;
    }
    free(out.glsl);
    v->ok = v->mod != NULL;
    if (v->ok) {
        char path[128];
        StageRec r;
        memset(&r, 0, sizeof r);
        r.magic = 0x52545356 + MX_VK_VERSION; r.stage = s; r.code_len = v->code_len; memcpy(r.key, v->key, 16); r.fo = v->fo; r.spv = spv_hash;
        r.cbufs = v->cbufs; r.ssbos = v->ssbos; r.ssbo_w = v->ssbo_w; r.images = v->images; r.dep = v->dep; r.tex_units = v->tex_units; r.in_attr = v->in_attr; r.out_attr = v->out_attr;
        r.uses_alpha = v->uses_alpha; r.nsmp = v->nsmp; memcpy(r.smp, v->smp, sizeof r.smp);
        CreateDirectoryA("../cache/vk", NULL); CreateDirectoryA("../cache/vk/stage", NULL);
        stage_rec_path(path, sizeof path, s, v->h);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
    }
    return v->ok ? v : NULL;
}

/* ---------------------------------------------------------------- pipelines (made at the draw that first needs them: session 2; workers are M4) */
typedef struct { uint32_t vs, fs, chanmask, sample_mask; int32_t cfmt[8], dfmt; uint8_t topo_class, ncolor, samples, poly, a2c, dclamp, logic, sshade, blend_on, pad[3]; uint8_t blend[8][8]; } PKey;
typedef struct Pipe { PKey k; VkPipeline p; VkPipelineLayout layout; VkDescriptorSetLayout dl[2]; int nb[2], push0; VStage *vs, *fs; struct Pipe *next; } Pipe;
static Pipe *g_pipes[1024];
static void pkey_targets(PKey *k);
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
/* Makes the pipeline of a key (any thread: the device calls are thread-safe, the pipeline cache is synchronised by the driver). */
static Pipe *pipe_make(const PKey *k, VStage *vs, VStage *fs, double *took) {
    Pipe *p = calloc(1, sizeof *p);
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    p->k = *k; p->vs = vs; p->fs = fs;
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
    rs.polygonMode = k->poly == 0 ? VK_POLYGON_MODE_POINT : k->poly == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL; rs.depthClampEnable = k->dclamp; rs.lineWidth = 1.0f;
    if (D.has_pv) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkSampleMask smask = k->sample_mask;
    ms.rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1); ms.alphaToCoverageEnable = k->a2c; ms.pSampleMask = &smask;
    if (k->sshade == 1 && D.f2.features.sampleRateShading) { ms.sampleShadingEnable = VK_TRUE; ms.minSampleShading = 1.0f; }
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba[8];
    memset(ba, 0, sizeof ba);
    for (int i = 0; i < k->ncolor; i++) {
        const uint8_t *b = k->blend[i];
        ba[i].blendEnable = k->blend_on >> i & 1; ba[i].colorWriteMask = (k->chanmask >> (i * 4)) & 15;
        ba[i].srcColorBlendFactor = blend_factor(b[0]); ba[i].dstColorBlendFactor = blend_factor(b[1]); ba[i].srcAlphaBlendFactor = blend_factor(b[2]); ba[i].dstAlphaBlendFactor = blend_factor(b[3]);
        ba[i].colorBlendOp = blend_op(b[4]); ba[i].alphaBlendOp = blend_op(b[5]);
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k->ncolor; cb.pAttachments = ba;
    if (k->logic != 3 && D.f2.features.logicOp) { cb.logicOpEnable = VK_TRUE; cb.logicOp = (VkLogicOp)(k->logic & 15); }
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof dyn / sizeof *dyn; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkFormat cf[8];
    for (int i = 0; i < 8; i++) cf[i] = (VkFormat)k->cfmt[i];
    ri.colorAttachmentCount = k->ncolor; ri.pColorAttachmentFormats = cf;
    if (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_DEPTH_BIT) ri.depthAttachmentFormat = (VkFormat)k->dfmt;
    if (k->dfmt && (fmt_aspect((VkFormat)k->dfmt) & VK_IMAGE_ASPECT_STENCIL_BIT)) ri.stencilAttachmentFormat = (VkFormat)k->dfmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ri; gi.stageCount = fs ? 2 : 1; gi.pStages = st; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp; gi.pRasterizationState = &rs; gi.pMultisampleState = &ms;
    gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = p->layout;
    VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &p->p);
    if (r != VK_SUCCESS) vkd_fail("vkCreateGraphicsPipelines (vertex stage %u, fragment stage %u, %d colour targets, depth format %d): %s", k->vs, k->fs, k->ncolor, k->dfmt, vk_result_name(r));
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&fq);
    *took = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)fq.QuadPart;
    InterlockedIncrement64(&R.pipelines);
    return p;
}
/* ..\cache\vk\pipes.bin: every pipeline key this PC ever needed, with its stages named by their hashes (design section 5:
 * "pipes.txt"). Read at start by the workers; a key missing at a draw is made there and appended (once per key per PC). */
typedef struct { uint32_t magic, compute; uint64_t vs_h, fs_h; PKey k; } PipeRec;
#define PIPEREC_MAGIC (0x31455049u + MX_VK_VERSION)
static void piperec_append(const PKey *k, VStage *vs, VStage *fs, int compute) {
    PipeRec r;
    memset(&r, 0, sizeof r);
    r.magic = PIPEREC_MAGIC; r.compute = (uint32_t)compute; r.vs_h = vs->h; r.fs_h = fs ? fs->h : 0;
    if (k) { r.k = *k; r.k.vs = r.k.fs = 0; }
    CreateDirectoryA("../cache/vk", NULL);
    FILE *fp = fopen("../cache/vk/pipes.bin", "ab");
    if (fp) { fwrite(&r, 1, sizeof r, fp); fclose(fp); }
}
static Pipe *pipe_get(const PKey *k, VStage *vs, VStage *fs) {
    uint64_t h = fnv(0xcbf29ce484222325ULL, k, sizeof *k);
    for (Pipe *p = g_pipes[h & 1023]; p; p = p->next) if (!memcmp(&p->k, k, sizeof *k)) return p;
    double ms = 0;
    LONG64 t0 = tk();
    Pipe *p = pipe_make(k, vs, fs, &ms);           /* never met on this PC: made here, the frame is late once (counted) */
    g_tm[5] += tk() - t0;
    g_pipe_miss++;
    if (D.debug) printf("nvn_vk: pipeline miss %lld: stages %u/%u, class %d, %d colour (format %d), depth %d, samples %d: %.1f ms\n", (long long)g_pipe_miss, k->vs, k->fs, k->topo_class, k->ncolor, k->cfmt[0], k->dfmt, k->samples, ms);
    p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
    piperec_append(k, vs, fs, 0);
    return p;
}

#endif
/* ---------------------------------------------------------------- passes */
static int target_level(const TargetRef *r) { return (r->view.set & 0x81) == 0x81 ? r->view.lvl0 : 0; }
static VkImageView target_view(const TargetRef *r, VTex *vt) {
    EView e;
    eview_of(r->tex, vt, &r->view, &e);
    memset(e.swz, 0, 4); e.nlvl = 1; e.nlay = 1; e.format = r->tex->b.format; e.target = 1; e.dsmode = 0;
    if ((r->view.set & 0x84) == 0x84 && r->view.format) e.format = r->view.format;
    return vt_view(vt, &e, -1, 1);
}
static void fl_pass(int w, int h, int fmt, int win, int ncol, int depth);      /* nvn_vk_flash.c (session 13) */
static int g_vk_test;                              /* session 13, TEST ONLY: SSBU_VK_TEST (see the dynamic state) */
static int pass_begin(void) {
    VkRenderingAttachmentInfo ca[8], da;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    int w = 0, h = 0;
    if (R.in_pass) return 1;
    if (g_split < 0) { const char *e = getenv("SSBU_VK_SPLIT"); g_split = e ? atoi(e) : VK_SPLIT_DEFAULT; }
    if (g_split > 0 && R.recording && g_cmd_draws >= g_split) { cmd_submit(NULL, NULL); g_splits++; }      /* round 16: the GPU starts on what is recorded so far (a pass has just ended: nothing is cut in two) */
    cmd_begin();
    R.pass_rs = 0;
    memset(ca, 0, sizeof ca); memset(&da, 0, sizeof da);
    for (int i = 0; i < R.ncol; i++) {
        Tex *t = R.col[i].tex;
        ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        if (!t || t->dead) continue;
        VTex *vt = vt_get(t);
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { nvn_vk_missing("a depth format bound as colour target"); continue; }
        t->gpu_written = 1;
        vt_layout(vt, vt->feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ca[i].imageView = target_view(&R.col[i], vt); ca[i].imageLayout = vt->layout;
        if (!w) { w = lvl_(vt->w, target_level(&R.col[i])); h = lvl_(vt->h, target_level(&R.col[i])); if (vt->rs) { R.pass_rs = 1; R.pass_sx = vt->sx; R.pass_sy = vt->sy; } }
    }
    if (R.depth.tex && !R.depth.tex->dead) {
        VTex *vt = vt_get(R.depth.tex);
        R.depth.tex->gpu_written = 1;
        if (vt->aspect == VK_IMAGE_ASPECT_COLOR_BIT) nvn_vk_missing("a colour format bound as depth target");
        else if (RS_ON && rs_ui_native() && vt->rs && !R.pass_rs && w && (lvl_(vt->w, target_level(&R.depth)) != w || lvl_(vt->h, target_level(&R.depth)) != h)) g_rs_mixed++;      /* session 9 (SSBU_VK_UI_NATIVE): an unscaled colour target with a scaled depth target: the pass runs WITHOUT depth (counted, reported at exit); PR #8 review: != (not <), above 1.0 the scaled depth is LARGER than the colour */
        else {
            vt_layout(vt, vt->feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
            da.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            da.imageView = target_view(&R.depth, vt); da.imageLayout = vt->layout;
            if (vt->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ri.pDepthAttachment = &da;
            if (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ri.pStencilAttachment = &da;
            if (!w) { w = lvl_(vt->w, target_level(&R.depth)); h = lvl_(vt->h, target_level(&R.depth)); if (vt->rs) { R.pass_rs = 1; R.pass_sx = vt->sx; R.pass_sy = vt->sy; } }
        }
    }
    if (!w) return 0;
    { Tex *c0 = R.ncol ? R.col[0].tex : NULL; fl_pass(w, h, c0 ? (int)c0->b.format : 0, c0 && wintex_is(c0), R.ncol, ri.pDepthAttachment != NULL); }      /* session 13: the flash detector's pass list */
    {   /* session 9, SSBU_VK_UILOG=<n>: every pass begun in the n-th battle frame ("nvn_vk_ui:" lines): its targets, whether the
         * colour target is a texture of the window, and the draws of the frame so far (the next line's number minus this one = its draws) */
        static int uil = -2, np;
        if (uil == -2) uil = getenv("SSBU_VK_UILOG") ? atoi(getenv("SSBU_VK_UILOG")) : -1;
        if (uil > 0 && g_battle_frames == uil) {
            Tex *c0 = R.ncol ? R.col[0].tex : NULL, *dt = R.depth.tex && !R.depth.tex->dead ? R.depth.tex : NULL;
            printf("nvn_vk_ui: pass %d: %d colour, first %dx%d format 0x%x level %d%s%s | depth %dx%d format 0x%x%s | image size of the pass %dx%d | %lld draws so far, %lld dispatches so far\n", np++, R.ncol,
                   c0 ? c0->b.width : 0, c0 ? c0->b.height : 0, c0 ? (int)c0->b.format : 0, c0 ? target_level(&R.col[0]) : 0, c0 && wintex_is(c0) ? " WINDOW TEXTURE" : "", c0 && c0->vk && ((VTex *)c0->vk)->rs ? " (scaled)" : "",
                   dt ? dt->b.width : 0, dt ? dt->b.height : 0, dt ? (int)dt->b.format : 0, dt && dt->vk && ((VTex *)dt->vk)->rs ? " (scaled)" : "", w, h, (long long)R.frame_draws, (long long)R.dispatches);
        }
    }
    {   /* SSBU_VK_RTLOG=1 (track O, resolution scale survey): every distinct render-target size a pass is begun with, with
         * the colour / depth formats and how many passes used it (printed at exit: "nvn_vk_rt:" lines) */
        static int on = -1;
        if (on < 0) on = getenv("SSBU_VK_RTLOG") && atoi(getenv("SSBU_VK_RTLOG"));
        if (on) {
            int cf = R.ncol && R.col[0].tex ? (int)R.col[0].tex->b.format : 0, df = R.depth.tex ? (int)R.depth.tex->b.format : 0, lv = R.ncol && R.col[0].tex ? target_level(&R.col[0]) : R.depth.tex ? target_level(&R.depth) : 0, i;
            for (i = 0; i < g_rtlog_n; i++) if (g_rtlog[i].w == w && g_rtlog[i].h == h && g_rtlog[i].ncol == R.ncol && g_rtlog[i].cf == cf && g_rtlog[i].df == df && g_rtlog[i].lv == lv) break;
            if (i == g_rtlog_n && g_rtlog_n < 256) { g_rtlog[i].w = w; g_rtlog[i].h = h; g_rtlog[i].ncol = R.ncol; g_rtlog[i].cf = cf; g_rtlog[i].df = df; g_rtlog[i].lv = lv; g_rtlog[i].first = nvn_front_frame_next(); g_rtlog_n++; }
            if (i < 256) { g_rtlog[i].n++; if (g_in_fight) g_rtlog[i].fight++; }
        }
    }
    cmd_begin();                                   /* vt_get may have submitted (ring wrap) */
    ri.renderArea.extent.width = (uint32_t)w; ri.renderArea.extent.height = (uint32_t)h; ri.layerCount = 1; ri.colorAttachmentCount = (uint32_t)R.ncol; ri.pColorAttachments = ca;
    if (g_pt_on > 0) { Tex *c0 = R.ncol ? R.col[0].tex : NULL; pt_mark(w, h, R.ncol, c0 ? (int)c0->b.format : 0, ri.pDepthAttachment != NULL, c0 && wintex_is(c0), 0); }
    CKPT_OP("pass begin (first colour view, w, h, colour count)", ca[0].imageView, w, h, R.ncol);
    vkCmdBeginRendering(R.cmd, &ri);
    R.in_pass = 1; R.pass_w = w; R.pass_h = h;
    return 1;
}

/* ---------------------------------------------------------------- draws */
static VkDescriptorSet set_alloc(VkDescriptorSetLayout layout) {
    Slot *s = &R.slot[R.cur];
    VkDescriptorSet set;
    for (;;) {
        if (s->ipool < s->npool) {
            VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = s->pool[s->ipool]; ai.descriptorSetCount = 1; ai.pSetLayouts = &layout;
            VkResult r = vkAllocateDescriptorSets(V.device, &ai, &set);
            if (r == VK_SUCCESS) { R.sets++; return set; }
            if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) vk_fatal("vkAllocateDescriptorSets", r);
            s->ipool++;
            continue;
        }
        if (s->npool == 16) vkd_fail("more than 16 descriptor pools in one command buffer");
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768}};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 8192; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        VKC(vkCreateDescriptorPool(V.device, &pi, NULL, &s->pool[s->npool++]));
    }
}
static struct { uint64_t key; VkDescriptorSet set; uint32_t gen; VkDescriptorSetLayout dl; int n, off; } g_tset[4096];      /* texture sets of the command buffer being recorded */
static struct { VTex *d2, *d1, *d3; VkImageView v[7]; } g_dummy;
static struct { VkImageView view; VkSampler smp; VkImageLayout layout; } g_dtex[140], g_tset_list[32768];      /* g_tset_list: the lists of the cached sets, for the full comparison */
static int g_ndtex;
/* ---- PR #32: the device loss report. vk_fatal (nvn_vk.c) calls vkd_lost_report once on VK_ERROR_DEVICE_LOST, before the
 * process ends with code 5. What it can say:
 *   - VK_EXT_device_fault (enabled wherever the driver lists it; nothing is called until a loss): the driver's description of
 *     the fault, its address records (read / write / execution of an invalid address) and its vendor records;
 *   - SSBU_VK_CHECKPOINTS=1 (off by default; VK_NV_device_diagnostic_checkpoints, NVIDIA only): a marker before most commands
 *     (every draw and dispatch, layout changes, uploads, the clear of a new image, pass begin / end, copies into and read-backs
 *     of textures, resolves, blits, attachment and texture clears, the progress-bar and present blits, the end of every command
 *     buffer; NOT the track-O upload / read-back, the reduction blits, the overlay clears, vkCmdCopyBuffer or the masked stencil
 *     clear, which their neighbours bracket), each with a record of the command; after a loss the queue says which markers
 *     reached the top and the bottom of the pipe, which names what the GPU was executing.
 * With the markers off the draw path does nothing more than before (every addition here is behind D.has_ckpt). */
typedef struct { uint16_t fmt, w, h, d; uint8_t lv, target, dim, unit, stage, dummy; } DTexInfo;      /* what a draw sampled: one entry per entry of g_dtex */
static DTexInfo g_dtex_info[140];
typedef struct { LONG64 frame; uint32_t seq; uint8_t kind, prim, itype, ntex; int prog; uint32_t vs, fs; uint64_t vs_hb, fs_hb; uint32_t count, inst, first; int32_t base; uint64_t serial; const char *what; uint64_t obj; DTexInfo tex[12]; } DrawRec;
#define DREC_N 16384
static DrawRec g_drec[DREC_N];                     /* 256 bytes a record, a ring (zero-filled: only the pages written take memory) */
static uint32_t g_drec_seq;
static VStage *g_dvs, *g_dfs;                      /* the stages of the draw draw_prepare prepared last */
static void ckpt_mark(int kind, unsigned prim, uint32_t count, uint32_t inst, uint32_t first, int32_t base, unsigned itype) {      /* D.has_ckpt; kind: 0 draw, 1 indexed draw, 2 dispatch */
    uint32_t seq = ++g_drec_seq;
    DrawRec *d = &g_drec[seq % DREC_N];
    memset(d, 0, sizeof *d);
    d->seq = seq; d->frame = nvn_front_frame_next(); d->kind = (uint8_t)kind; d->prim = (uint8_t)prim; d->itype = (uint8_t)itype; d->count = count; d->inst = inst; d->first = first; d->base = base;
    d->serial = R.serial + 1;
    if (kind != 2) {
        d->prog = R.prog[1] ? R.prog[1]->serial : R.prog[0] ? R.prog[0]->serial : -1;
        if (g_dvs) { d->vs = g_dvs->id; d->vs_hb = g_dvs->hb; }
        if (g_dfs) { d->fs = g_dfs->id; d->fs_hb = g_dfs->hb; }
        d->ntex = (uint8_t)(g_ndtex < 12 ? g_ndtex : 12);
        memcpy(d->tex, g_dtex_info, sizeof d->tex[0] * d->ntex);
    } else d->prog = R.prog[5] ? R.prog[5]->serial : -1;
    vkCmdSetCheckpointNV(R.cmd, (void *)(uintptr_t)seq);
}
static void ckpt_op(const char *what, uint64_t obj, int a, int b, int c) {      /* through CKPT_OP (D.has_ckpt) */
    if (!R.cmd) return;
    uint32_t seq = ++g_drec_seq;
    DrawRec *d = &g_drec[seq % DREC_N];
    memset(d, 0, sizeof *d);
    d->seq = seq; d->frame = nvn_front_frame_next(); d->kind = 3; d->what = what; d->obj = obj; d->count = (uint32_t)a; d->inst = (uint32_t)b; d->first = (uint32_t)c; d->serial = R.serial + 1;
    vkCmdSetCheckpointNV(R.cmd, (void *)(uintptr_t)seq);
}
static void drec_print(const char *tag, uint32_t seq) {
    DrawRec *d = &g_drec[seq % DREC_N];
    if (d->seq != seq) { printf("nvn_vk_lost: %s: marker %u (no record)\n", tag, seq); return; }
    if (d->kind == 3) { printf("nvn_vk_lost: %s: marker %u, frame %lld, submission %llu, %s: object %llx, %d %d %d\n", tag, seq, (long long)d->frame, (unsigned long long)d->serial, d->what, (unsigned long long)d->obj, (int)d->count, (int)d->inst, (int)d->first); return; }
    printf("nvn_vk_lost: %s: marker %u, frame %lld, submission %llu, %s prim %u count %u instances %u first %u base %d, program #%d, stages %u (%016llx) / %u (%016llx), textures:",
           tag, seq, (long long)d->frame, (unsigned long long)d->serial, d->kind == 2 ? "DISPATCH" : d->kind ? "indexed draw" : "draw", d->prim, d->count, d->inst, d->first, d->base, d->prog,
           d->vs, (unsigned long long)d->vs_hb, d->fs, (unsigned long long)d->fs_hb);
    for (int i = 0; i < d->ntex; i++) {
        DTexInfo *t = &d->tex[i];
        printf(" [%s%u kind %u: fmt 0x%x %ux%ux%u lv %u target %u%s]", t->stage ? "f" : "v", t->unit, t->dim, t->fmt, t->w, t->h, t->d, t->lv, t->target, t->dummy ? " DUMMY" : "");
    }
    printf("\n");
}
static void vkd_lost_report(void) {                /* nvn_vk.c vk_fatal: once, whichever thread lost the device first */
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    printf("nvn_vk_lost: the device is lost; submissions made %llu, seen finished %llu\n", (unsigned long long)R.serial, (unsigned long long)R.done);
    if (D.has_fault && vkGetDeviceFaultInfoEXT) {  /* the entry points are loaded in vkd_init: a loss before it reports nothing */
        VkDeviceFaultCountsEXT c = {VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        VkResult r = vkGetDeviceFaultInfoEXT(V.device, &c, NULL);
        printf("nvn_vk_lost: vkGetDeviceFaultInfoEXT: %s, %u address records, %u vendor records\n", vk_result_name(r), c.addressInfoCount, c.vendorInfoCount);
        if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
            VkDeviceFaultInfoEXT fi = {VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
            fi.pAddressInfos = calloc(c.addressInfoCount + 1, sizeof *fi.pAddressInfos); fi.pVendorInfos = calloc(c.vendorInfoCount + 1, sizeof *fi.pVendorInfos);
            c.vendorBinarySize = 0;
            if (fi.pAddressInfos && fi.pVendorInfos) {
                r = vkGetDeviceFaultInfoEXT(V.device, &c, &fi);
                static const char *const k_at[7] = {"none", "READ OF AN INVALID ADDRESS", "WRITE TO AN INVALID ADDRESS", "EXECUTION OF AN INVALID ADDRESS", "instruction pointer unknown", "instruction pointer invalid", "instruction pointer of a fault"};
                printf("nvn_vk_lost: fault: \"%s\" (%s)\n", fi.description, vk_result_name(r));
                for (uint32_t i = 0; i < c.addressInfoCount; i++)
                    printf("nvn_vk_lost:   address record %u: %s, address 0x%llx (precision 0x%llx)\n", i, fi.pAddressInfos[i].addressType < 7 ? k_at[fi.pAddressInfos[i].addressType] : "?",
                           (unsigned long long)fi.pAddressInfos[i].reportedAddress, (unsigned long long)fi.pAddressInfos[i].addressPrecision);
                for (uint32_t i = 0; i < c.vendorInfoCount; i++)
                    printf("nvn_vk_lost:   vendor record %u: %s, code 0x%llx, data 0x%llx\n", i, fi.pVendorInfos[i].description, (unsigned long long)fi.pVendorInfos[i].vendorFaultCode, (unsigned long long)fi.pVendorInfos[i].vendorFaultData);
            }
        }
    }
    if (D.has_ckpt && vkGetQueueCheckpointDataNV) {
        uint32_t n = 0, top = 0, bottom = 0, mn = ~0u, mx = 0;
        VkCheckpointDataNV cd[32];
        vkGetQueueCheckpointDataNV(V.queue, &n, NULL);
        if (n > 32) n = 32;
        for (uint32_t i = 0; i < n; i++) { memset(&cd[i], 0, sizeof cd[i]); cd[i].sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV; }
        vkGetQueueCheckpointDataNV(V.queue, &n, cd);
        printf("nvn_vk_lost: %u checkpoints reported; the last marker recorded is %u\n", n, g_drec_seq);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t m = (uint32_t)(uintptr_t)cd[i].pCheckpointMarker;
            printf("nvn_vk_lost: checkpoint at pipeline stage 0x%x: marker %u\n", (unsigned)cd[i].stage, m);
            if (cd[i].stage == VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT) top = m;
            if (cd[i].stage == VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT) bottom = m;
            if (m < mn) mn = m;
            if (m > mx) mx = m;
        }
        if (n && !top) top = mx;
        if (n && !bottom) bottom = mn;
        /* a marker that reached a stage = everything recorded BEFORE it passed that stage: the commands between the last
         * marker that reached the bottom of the pipe and the last one that reached the top are the ones that may not have finished */
        if (bottom) for (uint32_t m = bottom > 12 ? bottom - 12 : 1; m < bottom; m++) drec_print("finished", m);
        for (uint32_t m = bottom ? bottom : 1; top && m <= top + 6 && m <= bottom + 60; m++) drec_print(m <= top ? "STARTED, MAY NOT HAVE FINISHED" : "after the last started", m);
        if (top > bottom + 60) drec_print("the last marker that reached the top of the pipe", top);      /* beyond the range printed above */
    }
    fflush(stdout);
}
/* 2026-10-21, AMD render bugs (four players on AMD RX 6700 XT / 6750 XT / 9070 XT and an integrated Radeon: stages dark,
 * posterized or in a white haze; NVIDIA and Intel fine): every frame some stage effects (fog, soft particles: nvn_vk_fb lines)
 * sample the depth target that is bound to the pass. That was done by keeping the depth image in GENERAL layout, attached
 * and sampled at once (a feedback loop). Legal for reads, but AMD's depth compression is the suspect. With depth_copy_on()
 * (AMD by default; SSBU_VK_DEPTHCOPY=1 / 0 forces it) the depth is instead copied once per target binding into a shadow
 * image of the same format, which the shader samples; the depth target itself stays in its attachment layout. */
static int depth_copy_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("SSBU_VK_DEPTHCOPY");
        on = e && e[0] ? atoi(e) != 0 : V.props.vendorID == 0x1002;
        printf("nvn_vk: depth targets sampled while bound: %s (%s)\n", on ? "a copy is sampled" : "sampled in place (GENERAL layout)", e && e[0] ? "SSBU_VK_DEPTHCOPY" : on ? "AMD: the default there" : "the default");
    }
    return on;
}
static VTex *depth_shadow(VTex *vt) {                  /* the copy of depth target vt, up to date for this binding; the pass is ended */
    VTex *sh = vt->shadow;
    if (!sh) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        sh = calloc(1, sizeof *sh);
        *sh = *vt; sh->views = NULL; sh->next = sh->prev = NULL; sh->shadow = NULL; sh->feedback = 0;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = vt->fmt; ci.extent.width = (uint32_t)vt->w; ci.extent.height = (uint32_t)vt->h; ci.extent.depth = 1;
        ci.mipLevels = (uint32_t)vt->levels; ci.arrayLayers = (uint32_t)vt->layers; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkResult r = vkCreateImage(V.device, &ci, NULL, &sh->img);
        if (r != VK_SUCCESS) vkd_fail("vkCreateImage (depth copy %dx%d format %d): %s", vt->w, vt->h, (int)vt->fmt, vk_result_name(r));
        vkGetImageMemoryRequirements(V.device, sh->img, &rq);
        sh->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, sh->img, sh->mem.mem, sh->mem.off));
        sh->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        vt->shadow = sh; R.images++; R.images_live++;
    }
    if (!vt->shadow_ok) {
        VkImageCopy rg[32];
        int n = 0;
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vt_layout(sh, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        for (int l = 0; l < vt->levels && n < 30; l++)
            for (int a = 0; a < 2; a++) {
                VkFlags bit = a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
                if (!(vt->aspect & bit)) continue;
                memset(&rg[n], 0, sizeof rg[n]);
                rg[n].srcSubresource.aspectMask = rg[n].dstSubresource.aspectMask = bit;
                rg[n].srcSubresource.mipLevel = rg[n].dstSubresource.mipLevel = (uint32_t)l;
                rg[n].srcSubresource.layerCount = rg[n].dstSubresource.layerCount = (uint32_t)vt->layers;
                rg[n].extent.width = (uint32_t)lvl_(vt->w, l); rg[n].extent.height = (uint32_t)lvl_(vt->h, l); rg[n].extent.depth = 1;
                n++;
            }
        vkCmdCopyImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sh->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)n, rg);
        vt->shadow_ok = 1;
        g_depth_copies++;
    }
    vt_layout(sh, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return sh;
}
/* The image view and sampler of one declared sampler of stage s. May end the pass (upload, layout change). */
static void resolve_sampler(int s, VStage *v, int i) {
    unsigned u = v->smp[i].unit;
    Tex *t = NULL;
    View view;
    SamplerParams sp;
    int r = u < 32 ? nvn_front_handle(R.tpool, R.spool, R.nvn_tex[s][u], &t, &view, &sp) : 0;
    DTexInfo *di = D.has_ckpt ? &g_dtex_info[g_ndtex] : NULL;      /* PR #32: for the device loss report, only with the markers on */
    if (di) { memset(di, 0, sizeof *di); di->dim = v->smp[i].dim; di->unit = (uint8_t)u; di->stage = (uint8_t)s; di->dummy = 1; }
    g_dtex[g_ndtex].view = g_dummy.v[v->smp[i].dim]; g_dtex[g_ndtex].smp = g_smp_default; g_dtex[g_ndtex].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (r & 1) {
        VTex *vt = vt_get(t);
        EView e;
        int attached = R.depth.tex == t, dim = v->smp[i].dim;
        for (int k = 0; k < R.ncol; k++) attached |= R.col[k].tex == t;
        eview_of(t, vt, &view, &e);
        int have = target_dim(e.target);
        if (di) { di->fmt = (uint16_t)t->b.format; di->w = (uint16_t)t->b.width; di->h = (uint16_t)t->b.height; di->d = (uint16_t)t->b.depth; di->lv = (uint8_t)nvn_front_tex_levels(t); di->target = (uint8_t)e.target; }
        int ok = have == dim || (dim == DIM_2D && (have == DIM_2DA || have == DIM_CUBE || have == DIM_CUBEA)) || (dim == DIM_2DA && (have == DIM_2D || have == DIM_CUBE || have == DIM_CUBEA)) ||
                 (dim == DIM_CUBE && have == DIM_CUBEA) || (dim == DIM_CUBEA && have == DIM_CUBE) || (dim == DIM_1D && have == DIM_1DA) || (dim == DIM_1DA && have == DIM_1D) ||
                 ((dim == DIM_CUBE || dim == DIM_CUBEA) && (have == DIM_2DA) && e.nlay >= 6 && vt->w == vt->h);
        if (ok && vt->samples == 1 && attached && R.depth.tex == t && vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT && !vt->is3d && have_targets_live() && depth_copy_on()) {
            VTex *sh = depth_shadow(vt);              /* (AMD render bugs) the copy, not the bound image */
            g_dtex[g_ndtex].view = vt_view(sh, &e, dim, 0); g_dtex[g_ndtex].layout = sh->layout; if (di) di->dummy = 0;
        } else if (ok && vt->samples == 1) {
            if (attached && have_targets_live()) { if (!vt->feedback) { vt->feedback = 1; R.feedback++; pass_end();
                    {   /* 2026-10-20 (render bugs V): the first feedback passes in the log: which shader samples which bound target */
                        static int nfb;
                        if (nfb++ < 24) printf("nvn_vk_fb: frame %lld, program #%d, %s unit %u samples a bound %s target (format 0x%x, %d x %d, view kind %d), depth test %d write %d, colour mask %x, targets %d\n",
                                               (long long)nvn_front_frame_next(), R.prog[s] ? R.prog[s]->serial : -1, s ? "fragment" : "vertex", u, R.depth.tex == t ? "DEPTH" : "COLOUR",
                                               t->b.format, t->b.width, t->b.height, dim, (int)(R.ds & 1), (int)((R.ds >> 1) & 1), (unsigned)R.chanmask, R.ncol);
                    } }
                vt_layout(vt, VK_IMAGE_LAYOUT_GENERAL); }
            else vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_dtex[g_ndtex].view = vt_view(vt, &e, dim, 0); g_dtex[g_ndtex].layout = vt->layout; if (di) di->dummy = 0;
        } else { static int n; R.dummy_kind++;
            if (n < 3) printf("nvn_vk: (detail of the next line) frame %lld, program #%d, texture format 0x%x, %d x %d x %d, %d levels, draws so far with a dummy for a view-kind mismatch: %lld\n", (long long)nvn_front_frame_next(), R.prog[s] ? R.prog[s]->serial : -1, t->b.format, t->b.width, t->b.height, t->b.depth, nvn_front_tex_levels(t), (long long)R.dummy_kind);
            if (n++ < 20) printf("nvn_vk: texture unit %u of the %s stage: the shader declares view kind %d, the texture is kind %d (target %d, %d samples): a dummy is bound\n", u, s ? "fragment" : "vertex", dim, have, e.target, vt->samples); }
        if (r & 2) g_dtex[g_ndtex].smp = sampler_get(&sp);
    }
    g_ndtex++;
}
static int have_targets_live(void);
/* Track O: a rectangle of the game's pixels (scissor, clear) on the scaled images of the pass: every image pixel the
 * rectangle touches (floor of the first edge, ceiling of the last). */
static void rs_rect(int *x, int *y, int *w, int *h) {
    int x0 = (int)floorf((float)*x * R.pass_sx), y0 = (int)floorf((float)*y * R.pass_sy);
    int x1 = (int)ceilf((float)(*x + *w) * R.pass_sx - 0.001f), y1 = (int)ceilf((float)(*y + *h) * R.pass_sy - 0.001f);
    *x = x0; *y = y0; *w = x1 > x0 ? x1 - x0 : 0; *h = y1 > y0 ? y1 - y0 : 0;
}
static void set_dynamic_state(void) {
    VkViewport vp;
    VkRect2D sc;
    uint64_t d = R.ds;
    float x = R.have_viewport ? R.viewport[0] : 0, y = R.have_viewport ? R.viewport[1] : 0, w = R.have_viewport ? R.viewport[2] : (float)R.pass_w, h = R.have_viewport ? R.viewport[3] : (float)R.pass_h;
    if (R.pass_rs && R.have_viewport) { x *= R.pass_sx; w *= R.pass_sx; y *= R.pass_sy; h *= R.pass_sy; }      /* track O: the game's rectangle on the scaled image */
    vp.x = x; vp.y = y + h; vp.width = w; vp.height = -h; vp.minDepth = R.drange[0]; vp.maxDepth = R.drange[1];      /* NVN upper-left origin: clip y = +1 is row y (design, section 3) */
    if (vp.minDepth < 0) vp.minDepth = 0; if (vp.maxDepth > 1) vp.maxDepth = 1;
    vkCmdSetViewport(R.cmd, 0, 1, &vp);
    int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
    if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
    if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
    if (sx > R.pass_w) sx = R.pass_w; if (sy > R.pass_h) sy = R.pass_h;
    if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
    sc.offset.x = sx; sc.offset.y = sy; sc.extent.width = (uint32_t)(sw > 0 ? sw : 0); sc.extent.height = (uint32_t)(sh > 0 ? sh : 0);
    vkCmdSetScissor(R.cmd, 0, 1, &sc);
    if (R.pass_rs && D.f2.features.wideLines) { float lw = R.line_width * R.pass_sx; vkCmdSetLineWidth(R.cmd, lw < V.props.limits.lineWidthRange[0] ? V.props.limits.lineWidthRange[0] : lw); } else
    vkCmdSetLineWidth(R.cmd, D.f2.features.wideLines ? R.line_width : 1.0f);
    vkCmdSetDepthBias(R.cmd, R.polyoff[1], D.f2.features.depthBiasClamp ? R.polyoff[2] : 0.0f, R.polyoff[0]);
    vkCmdSetBlendConstants(R.cmd, R.blendc);
    vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.svmask[0]); vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.svmask[1]);
    vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.swmask[0]); vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.swmask[1]);
    vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)R.sref[0]); vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_BACK_BIT, (uint32_t)R.sref[1]);
    vkCmdSetCullMode(R.cmd, R.poly[0] & 3);
    vkCmdSetFrontFace(R.cmd, R.poly[1] ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    {   /* session 13, TEST ONLY (the missing stage pieces on zelda_temple): SSBU_VK_TEST bits: 1 no culling, 2 depth clamp always on, 4 no depth bias */
        static int tst = -1;
        if (tst < 0) tst = getenv("SSBU_VK_TEST") ? atoi(getenv("SSBU_VK_TEST")) : 0;
        if (tst & 1) vkCmdSetCullMode(R.cmd, VK_CULL_MODE_NONE);
        if (tst & 4) vkCmdSetDepthBias(R.cmd, 0, 0, 0);
        g_vk_test = tst;
    }
    vkCmdSetDepthTestEnable(R.cmd, d & 1); vkCmdSetDepthWriteEnable(R.cmd, (d >> 1) & 1);
    { unsigned f = (unsigned)(d >> 4) & 15; VkCompareOp op = (VkCompareOp)(f >= 1 && f <= 8 ? f - 1 : 7);
      if ((g_vk_test & 32) && op == VK_COMPARE_OP_EQUAL) op = VK_COMPARE_OP_ALWAYS;      /* session 14, TEST: SSBU_VK_TEST bit 32: a depth test "equal" passes always (is the missing wall an equal-depth pass after a depth pre-pass?) */
      if ((g_vk_test & 128) && op == VK_COMPARE_OP_EQUAL) op = VK_COMPARE_OP_LESS_OR_EQUAL;      /* round 17, TEST: bit 128 / 256: "equal" becomes less-or-equal / greater-or-equal */
      if ((g_vk_test & 256) && op == VK_COMPARE_OP_EQUAL) op = VK_COMPARE_OP_GREATER_OR_EQUAL;
      if ((g_vk_test & 64) && (d & 1)) { static LONG64 n[8]; if (n[op & 7]++ == 0) printf("nvn_vk: TEST: depth compare op %d in use (0 never, 1 less, 2 equal, 3 less-or-equal, 4 greater, 5 not equal, 6 greater-or-equal, 7 always), depth write %d\n", (int)op, (int)((d >> 1) & 1)); }
      vkCmdSetDepthCompareOp(R.cmd, op); }
    vkCmdSetStencilTestEnable(R.cmd, (d >> 2) & 1);
    {   static const uint8_t op[9] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
                                      VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
#define SOP(sh) ((VkStencilOp)op[((d >> (sh)) & 15) < 9 ? ((d >> (sh)) & 15) : 1])
#define SFN(sh) ((VkCompareOp)((((d >> (sh)) & 15) >= 1 && ((d >> (sh)) & 15) <= 8) ? ((d >> (sh)) & 15) - 1 : 7))
        vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_FRONT_BIT, SOP(16), SOP(24), SOP(20), SFN(8));       /* (fail, pass, depth fail, compare); NVN order: stencil fail, depth fail, depth pass */
        vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_BACK_BIT, SOP(28), SOP(36), SOP(32), SFN(12));
#undef SOP
#undef SFN
    }
    vkCmdSetDepthBiasEnable(R.cmd, (R.poly[3] >> (R.poly[2] < 3 ? R.poly[2] : 2)) & 1);          /* offset enable bits: point, line, fill; the polygon mode picks one */
    vkCmdSetRasterizerDiscardEnable(R.cmd, R.rast_discard);
}
static int have_targets_live(void) { return R.in_pass || R.have_targets; }
/* Session 4: the state that left the pipeline key (VK_EXT_extended_dynamic_state3). Set after a pipeline is bound, whenever
 * one of the values changed, the pass changed, or a pipeline of our own (static state) was bound in between. */
static void set_dynamic_state3(int samples) {
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
    uint32_t n = V.props.limits.maxColorAttachments < 8 ? V.props.limits.maxColorAttachments : 8;
    for (int i = 0; i < 8; i++) {
        const uint8_t *b = R.blend[i];
        en[i] = R.blend_on >> i & 1; wm[i] = (R.chanmask >> (i * 4)) & 15;
        eq[i].srcColorBlendFactor = blend_factor(b[0]); eq[i].dstColorBlendFactor = blend_factor(b[1]); eq[i].colorBlendOp = blend_op(b[4]);
        eq[i].srcAlphaBlendFactor = blend_factor(b[2]); eq[i].dstAlphaBlendFactor = blend_factor(b[3]); eq[i].alphaBlendOp = blend_op(b[5]);
    }
    vkCmdSetColorBlendEnableEXT(R.cmd, 0, n, en);
    vkCmdSetColorBlendEquationEXT(R.cmd, 0, n, eq);
    vkCmdSetColorWriteMaskEXT(R.cmd, 0, n, wm);
    vkCmdSetPolygonModeEXT(R.cmd, R.poly[2] == 0 ? VK_POLYGON_MODE_POINT : R.poly[2] == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    vkCmdSetAlphaToCoverageEnableEXT(R.cmd, R.ms[1] != 0);
    { VkSampleMask m = R.sample_mask; vkCmdSetSampleMaskEXT(R.cmd, (VkSampleCountFlagBits)(samples ? samples : 1), &m); }
    if (D.f2.features.depthClamp) vkCmdSetDepthClampEnableEXT(R.cmd, R.depth_clamp != 0 || (g_vk_test & 2));
    R.dyn3_samples = samples;
}

/* ---------------------------------------------------------------- compute (M3; design section 3, "DispatchCompute" and "Storage buffers written by compute")
 * A storage buffer a compute stage may write gets a Vulkan buffer of its own (CB: host-visible memory, filled from guest
 * memory at its first dispatch of the batch). Later commands of the SAME batch whose range lies inside it bind that buffer
 * (the fighter's vertex buffers: no round trip). At the end of the batch the command buffer is submitted and the CBs become
 * "pending"; when that submission has finished the ranges a dispatch REALLY wrote (mx_wm, one word per dispatch in the
 * stream ring) are copied back to guest memory (cb_land): at the latest before the next batch reads guest memory, earlier
 * when the render thread is idle or a sync is waited for. The OpenGL path copies back before the next draw; what guest
 * memory holds once the batch is done is the same. Anything that reads guest memory through a range that only partly lies
 * in a CB forces the copy-back at once (cb_sync: submit, wait; counted). */
typedef struct { uint64_t addr, size, cap, busy; VkBuffer buf; VMem mem; int state, dirty; uint8_t *snap; uint64_t snap_cap; int stale, age; uint64_t kserial, ww; VkBuffer stg; VMem stg_mem; uint64_t stg_cap, lserial; uint32_t wchk; uint8_t wrote, touch, mid_stg; } CB;  /* state: 0 free, 1 live in this batch, 2 pending (batch submitted), 3 kept (SSBU_VK_CBKEEP); snap: guest memory as it was when the CB was filled; stale: the buffer holds what compute wrote and guest memory does not; kserial: the submission that ended the last batch that used it */
static int g_cb_merge = -1;                        /* SSBU_VK_CBMERGE=0: the whole range is copied back as before session 5 */
/* Round 16 (P3 section 15), SSBU_VK_CBKEEP=1 (0 = before: every buffer is filled from guest memory at its first dispatch of
 * a batch, twice, and merged back after the batch): a compute-written buffer STAYS on the GPU side between batches.
 * When the batch that used it has finished it becomes "kept": nothing is copied back. The next batch's dispatch into
 * the SAME range (address and size) takes the kept buffer as it is: no copy in either. Its bytes go back to guest memory
 * ("landing") only when something needs them there: a read of an overlapping range by a draw or a dispatch that is not
 * served by the buffer itself, a new compute buffer over a different extent, a command that reads or writes guest memory
 * (buffer copies, texel copies), or when two batches have passed without a dispatch into it. What a draw reads is the
 * same bytes as before as long as the GAME does not write the range itself between two dispatches (it would be
 * overwritten by the old copy-back as well, one batch later) and does not read compute's output from guest memory.
 * The game writing the range is not left to luck: the game's heap is write-watched (nvn_gl.c, ww_newest); before a
 * kept buffer is taken again the watch is asked, and when a page of the range was written by the CPU since the buffer
 * was filled, the buffer lands with the word-by-word merge of session 5 (a word the game changed stays the game's) and is
 * filled from guest memory again: what the old path did every batch, now only when there was a write (counted). Memory
 * that is not write-watched: no such check (counted as "unwatched").
 * SSBU_VK_CBKEEP=2 (test): the same, and the whole pages inside every kept range are made inaccessible; any access by
 * any thread is counted and logged ("nvn_vk_cbguard:": address, read or write, thread, host code address), the pages
 * are opened again and the access goes on. Counts in the "nvn_vk_cbkeep:" line every 600 frames.
 * SSBU_VK_CBMEM=1: the buffers are made in device-local host-visible memory (as SSBU_VK_BUFMEM does for the ring). */
#define VK_CBKEEP_DEFAULT 1                        /* since runs p16_s_* / p16_g_* / p16t_*: state hash identical on four stages, same-frame pictures identical on two, 0 trapped accesses */
static int g_cb_keep = -1, g_cb_nkept, g_cb_dl = -1;
static uint64_t g_kept_lo = ~0ull, g_kept_hi, g_live_lo = ~0ull, g_live_hi;      /* g_live_*: bounds of the buffers live in this batch (cb_inside / cb_overlap are not asked for a range outside them) */
static LONG64 g_ck_hits, g_ck_fills, g_ck_fill_bytes, g_ck_land[5], g_ck_land_bytes, g_ck_guard_fail, g_ck_unwatched, g_ck_gwords, g_ck_conflicts, g_ck_land_tk, g_ck_land_max;     /* landings by cause: 0 not dispatched into for two batches, 1 a read of an overlapping range, 2 a new compute buffer over another extent, 3 a guest-memory command, 4 the game wrote a page of the range */
uint64_t nvn_front_mem_newest(const void *p, size_t n);      /* nvn_gl.c */
static SRWLOCK g_cg_lock = SRWLOCK_INIT;
static struct { uintptr_t lo, hi; DWORD old; } g_cg[256];
static volatile LONG64 g_cg_reads, g_cg_writes, g_cg_render;
static uintptr_t g_cg_span_lo = ~(uintptr_t)0, g_cg_span_hi;
static DWORD g_cg_render_tid;
#ifdef _WIN32
static LONG CALLBACK cg_veh(EXCEPTION_POINTERS *x) {
    if (x->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || x->ExceptionRecord->NumberParameters < 2 || x->ExceptionRecord->ExceptionInformation[0] > 1) return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t a = (uintptr_t)x->ExceptionRecord->ExceptionInformation[1];
    int wr = x->ExceptionRecord->ExceptionInformation[0] == 1;
    LONG res = EXCEPTION_CONTINUE_SEARCH;
    if (a < g_cg_span_lo || a >= g_cg_span_hi) return res;
    AcquireSRWLockExclusive(&g_cg_lock);
    for (int i = 0; i < 256; i++) {
        if (!g_cg[i].hi || a < g_cg[i].lo || a >= g_cg[i].hi) continue;
        DWORD old;
        VirtualProtect((void *)g_cg[i].lo, g_cg[i].hi - g_cg[i].lo, g_cg[i].old, &old);
        if (wr) InterlockedIncrement64(&g_cg_writes); else InterlockedIncrement64(&g_cg_reads);
        if (GetCurrentThreadId() == g_cg_render_tid) InterlockedIncrement64(&g_cg_render);
        { static volatile LONG n; if (InterlockedIncrement(&n) <= 60) printf("nvn_vk_cbguard: %s of %llx (kept compute range pages %llx..%llx) by thread %lu%s, host code %p\n", wr ? "WRITE" : "READ", (unsigned long long)a, (unsigned long long)g_cg[i].lo, (unsigned long long)g_cg[i].hi,
                                                                 (unsigned long)GetCurrentThreadId(), GetCurrentThreadId() == g_cg_render_tid ? " (the render thread)" : "", (void *)x->ExceptionRecord->ExceptionAddress); }
        g_cg[i].lo = g_cg[i].hi = 0;
        res = EXCEPTION_CONTINUE_EXECUTION;
        break;
    }
    if (res == EXCEPTION_CONTINUE_SEARCH) {       /* opened by the render thread between the fault and here */
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery((void *)a, &mi, sizeof mi) && mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE))) res = EXCEPTION_CONTINUE_EXECUTION;
    }
    ReleaseSRWLockExclusive(&g_cg_lock);
    return res;
}
#endif
static int cb_keep_mode(void) {
    if (g_cb_keep < 0) {
        const char *e = getenv("SSBU_VK_CBKEEP");
        g_cb_keep = e ? atoi(e) : VK_CBKEEP_DEFAULT;
        if (getenv("SSBU_COMPUTE_CMP") && atoi(getenv("SSBU_COMPUTE_CMP"))) g_cb_keep = 0;      /* that comparison reads compute's output from guest memory after every batch */
#ifdef _WIN32
        if (g_cb_keep == 2) { AddVectoredExceptionHandler(1, cg_veh); printf("nvn_vk: SSBU_VK_CBKEEP=2: accesses to kept compute ranges are trapped and logged (test)\n"); }
#else
        if (g_cb_keep == 2) { g_cb_keep = 1; printf("nvn_vk: SSBU_VK_CBKEEP=2 (trapping accesses, a test) is not available in the Linux build: 1 is used\n"); }
#endif
        g_cg_render_tid = GetCurrentThreadId();
    }
    return g_cb_keep;
}
static void cg_guard(int i, uint64_t addr, uint64_t size) {
    uintptr_t lo = ((uintptr_t)addr + 4095) & ~(uintptr_t)4095, hi = (uintptr_t)(addr + size) & ~(uintptr_t)4095;
    if (hi <= lo) return;
    AcquireSRWLockExclusive(&g_cg_lock);
    if (!g_cg[i].hi) {
        DWORD old = 0;
        if (lo < g_cg_span_lo) g_cg_span_lo = lo;
        if (hi > g_cg_span_hi) g_cg_span_hi = hi;
        if (VirtualProtect((void *)lo, hi - lo, PAGE_NOACCESS, &old)) { g_cg[i].lo = lo; g_cg[i].hi = hi; g_cg[i].old = old; } else g_ck_guard_fail++;
    }
    ReleaseSRWLockExclusive(&g_cg_lock);
}
static void cg_unguard(int i) {
    if (g_cb_keep != 2) return;
    AcquireSRWLockExclusive(&g_cg_lock);
    if (g_cg[i].hi) { DWORD old; VirtualProtect((void *)g_cg[i].lo, g_cg[i].hi - g_cg[i].lo, g_cg[i].old, &old); g_cg[i].lo = g_cg[i].hi = 0; }
    ReleaseSRWLockExclusive(&g_cg_lock);
}
typedef struct { uint64_t wm_off; uint32_t written; int16_t cb[16]; } CDisp;
static CB g_cb[256];
static CDisp g_cdisp[2048];
static int g_ncdisp, g_cb_live, g_cb_pending, g_cb_cached = -1;
static uint64_t g_cb_serial;                       /* the submission the pending CBs wait for */
/* SSBU_VK_CBKEEP: the buffer's bytes go to guest memory. With the snapshot of the fill (SSBU_VK_CBMERGE, the default) only
 * the words compute changed since the fill are written, and a word the game changed since then stays the game's. */
static void cb_keep_back(CB *c, const uint8_t *src) {      /* src: the bytes in ordinary memory already (the copy of an asynchronous landing), or NULL = the buffer itself */
    if (!c->dirty && !c->stale) return;
#ifdef R1_MARK
    { void r1_wrote(uint64_t, uint64_t); r1_wrote(c->addr, c->size); }      /* R1 (r1_state.c): compute results written back into guest memory are marked */
#endif
    if (!c->snap) memcpy(P(c->addr), src ? src : (const uint8_t *)c->mem.map, (size_t)c->size);
    else if (src) {
        uint64_t *g = P(c->addr);
        const uint64_t *s = (const uint64_t *)c->snap, *m = (const uint64_t *)src;
        size_t nw = (size_t)(c->size / 8), tail = (size_t)(c->size & 7);
        for (size_t k = 0; k < nw; k++) {
            uint64_t sv = s[k], mv = m[k];
            if (mv == sv) { if (g[k] != sv) g_ck_gwords++; continue; }
            if (g[k] == sv) g[k] = mv; else { g_ck_gwords++; g_ck_conflicts++; }
        }
        if (tail) memcpy((uint8_t *)g + nw * 8, (const uint8_t *)m + nw * 8, tail);
    } else {
        /* the buffer may be write-combined device memory (SSBU_VK_CBMEM): read word by word it costs about 1 us per word
         * (run p16t_bf_after: 12.8 ms of the render thread for one landing); it is read ONCE, in one block, into ordinary
         * memory, and compared there */
        static uint8_t *tmp; static uint64_t tmp_cap;
        if (tmp_cap < c->size) { free(tmp); tmp_cap = (c->size + 65535) & ~(uint64_t)65535; tmp = malloc((size_t)tmp_cap); }
        memcpy(tmp, c->mem.map, (size_t)c->size);
        uint64_t *g = P(c->addr);
        const uint64_t *s = (const uint64_t *)c->snap, *m = (const uint64_t *)tmp;
        size_t nw = (size_t)(c->size / 8), tail = (size_t)(c->size & 7);
        for (size_t k = 0; k < nw; k++) {
            uint64_t sv = s[k], mv = m[k];
            if (mv == sv) { if (g[k] != sv) g_ck_gwords++; continue; }
            if (g[k] == sv) g[k] = mv; else { g_ck_gwords++; g_ck_conflicts++; }
        }
        if (tail) memcpy((uint8_t *)g + nw * 8, (const uint8_t *)m + nw * 8, tail);
    }
    R.cb_bytes += (LONG64)c->size; g_ck_land_bytes += (LONG64)c->size;
}
static void cb_masks(void) {                       /* every recorded dispatch has run: which buffers did each one write? */
    for (int i = 0; i < g_ncdisp; i++) {
        uint32_t wm;
        memcpy(&wm, (uint8_t *)R.ring_mem.map + g_cdisp[i].wm_off, 4);
        wm &= g_cdisp[i].written;
        for (int k = 0; k < 16; k++) if ((wm >> k & 1) && g_cdisp[i].cb[k] >= 0) g_cb[g_cdisp[i].cb[k]].dirty = 1;
    }
    g_ncdisp = 0;
}
static uint32_t g_cb_epoch;
/* midbatch: the command being prepared may still name a freed CB's buffer (it is recorded into the next submission), so
 * that buffer is not given out again before that submission has finished. */
static void cb_land(int midbatch) {                /* the GPU has finished everything that wrote a CB */
    LONG64 t0 = tk();
    cb_masks();
    for (int i = 0; i < 256; i++) {
        CB *c = &g_cb[i];
        if (!c->state || c->state >= 3) continue;
        if (g_cb_keep > 0 && c->state == 2) {      /* SSBU_VK_CBKEEP: the batch is over: the buffer stays, nothing is copied */
            c->stale |= c->dirty; c->dirty = 0; c->state = 3; c->age = 0; g_cb_nkept++;
            if (c->addr < g_kept_lo) g_kept_lo = c->addr;
            if (c->addr + c->size > g_kept_hi) g_kept_hi = c->addr + c->size;
            if (g_cb_keep == 2) cg_guard(i, c->addr, c->size);
            continue;
        }
#ifdef R1_MARK
        if (g_cb_keep <= 0 && c->dirty) { void r1_wrote(uint64_t, uint64_t); r1_wrote(c->addr, c->size); }      /* R1 (r1_state.c): the copy-back below is marked */
#endif
        if (g_cb_keep > 0) { cg_unguard(i); cb_keep_back(c, c->mid_stg ? (const uint8_t *)c->stg_mem.map : NULL); c->stale = 0; }      /* mid_stg: cb_sync had the GPU copy it into ordinary memory (V 2026-10-21) */
        else if (c->dirty && !c->snap) { memcpy(P(c->addr), c->mem.map, (size_t)c->size); R.cb_bytes += (LONG64)c->size; }
        else if (c->dirty) {
            /* Session 5: the copy-back is DEFERRED (up to the next batch), so the guest may have written into the range since
             * the CB was filled. Only what compute changed goes back: a word the guest changed meanwhile (guest != snapshot)
             * is kept and counted; when compute changed that word too it is a real conflict (counted apart; the guest's later
             * write stays, as on the OpenGL path where the copy-back comes before it). */
            uint64_t *g = P(c->addr);
            const uint64_t *s = (const uint64_t *)c->snap, *m = (const uint64_t *)c->mem.map;
            size_t nw = (size_t)(c->size / 8), tail = (size_t)(c->size & 7);
            LONG64 gw = 0, cf = 0;
            for (size_t k = 0; k < nw; k++) {
                uint64_t sv = s[k], mv = m[k];
                if (mv == sv) { if (g[k] != sv) gw++; continue; }
                if (g[k] == sv) g[k] = mv; else { gw++; cf++; }
            }
            if (tail) memcpy((uint8_t *)g + nw * 8, (const uint8_t *)m + nw * 8, tail);
            R.cb_bytes += (LONG64)c->size; R.cb_guest_words += gw; R.cb_conflict_words += cf;
            if (gw) { static int n; if (n++ < 10) printf("nvn_vk: copy-back of %llx+%llu: the guest wrote %lld words of the range while it was pending (%lld of them also written by compute); merged\n", (unsigned long long)c->addr, (unsigned long long)c->size, (long long)gw, (long long)cf); }
        }
        c->state = c->dirty = 0; c->mid_stg = 0;
        c->busy = midbatch ? R.serial + 1 : 0;
    }
    g_cb_live = g_cb_pending = 0; g_live_lo = ~0ull; g_live_hi = 0;
    nvn_front_ccmp_landed();                       /* SSBU_COMPUTE_CMP: what the dispatches wrote is in guest memory now */
    g_cb_epoch++;
    g_tm[1] += tk() - t0;
    R.gen++;                                       /* guest memory changed: ranges are streamed again */
}
static void cb_ring_wrap(void) { if (g_cb_pending) cb_land(0); else cb_masks(); }      /* after cmd_wait_all: the mask words in the ring are about to be reused */
/* SSBU_VK_CBKEEP: a kept buffer's bytes go to guest memory and the entry is free (see the comment at g_cb_keep). The batch
 * that used it last has finished (it became "kept" only then); its Vulkan buffer is not given out again before the
 * submission that ended that batch has finished (busy). */
static void cb_kept_land(CB *c, int cause) {
    LONG64 t0 = tk();
    cg_unguard((int)(c - g_cb));
    cb_keep_back(c, NULL);
    c->state = 0; c->stale = c->dirty = 0; c->busy = c->touch ? R.serial + 2 : c->kserial; c->touch = 0;      /* touch: a draw of this batch names the buffer (buf_ref): it may be in the command buffer after the one being recorded */
    g_ck_land[cause]++;
    if (--g_cb_nkept == 0) { g_kept_lo = ~0ull; g_kept_hi = 0; }
    R.gen++;                                       /* guest memory changed */
    { LONG64 dt = tk() - t0; g_tm[1] += dt; g_ck_land_tk += dt; if (dt > g_ck_land_max) g_ck_land_max = dt; }
}
/* Round 16, SSBU_VK_CBASYNC (default 1; 0 = every landing reads the buffer itself): a kept buffer that was not dispatched
 * into for two batches lands WITHOUT the CPU reading device memory (write-combined: 19-20 ms for the largest buffer,
 * runs p16t_bf_d / ica_d): the GPU copies it into a buffer in ordinary host memory (state 4, "landing"), and when that
 * submission has finished the bytes are merged into guest memory from there. Until then the entry counts as kept: a
 * read of an overlapping range, a new compute buffer over it or a guest-memory command first makes the copy finish
 * (submit if it is still being recorded, wait) and lands it: what guest memory holds when anything looks is the same. */
static int g_cb_async = -1;
static LONG64 g_ck_async, g_ck_async_forced, g_ck_dropped, g_ck_drawn;
static uint32_t g_cb_batch = 1;                    /* counts batches (vk_submit) */
static void cb_stg_ensure(CB *c) {               /* the buffer in ordinary (cached) memory a GPU copy of the CB lands in */
    if (c->stg_cap < c->size) {
        if (c->stg) { vkDestroyBuffer(V.device, c->stg, NULL); vmem_free(c->stg_mem); c->stg = NULL; }      /* only ever named by a landing that has finished */
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        VkMemoryRequirements rq;
        c->stg_cap = (c->size + 4095) & ~(uint64_t)4095;
        bi.size = c->stg_cap; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKC(vkCreateBuffer(V.device, &bi, NULL, &c->stg));
        vkGetBufferMemoryRequirements(V.device, c->stg, &rq);
        VkFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        int found = 0;
        for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) found = 1;
        if (!found) want &= ~(VkFlags)VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        c->stg_mem = vmem_alloc(rq, want, 2);
        VKC(vkBindBufferMemory(V.device, c->stg, c->stg_mem.mem, c->stg_mem.off));
    }
}
static void cb_land_start(CB *c) {
    cb_stg_ensure(c);
    pass_end(); cmd_begin();
    VkBufferCopy r = {0, 0, c->size};
    vkCmdCopyBuffer(R.cmd, c->buf, c->stg, 1, &r);
    c->state = 4; c->lserial = R.serial + 1;       /* the number the command buffer being recorded gets at its submission */
    g_ck_async++;
}
static void cb_land_finish(CB *c) {                /* state 4 and its copy has finished */
    LONG64 t0 = tk();
    cg_unguard((int)(c - g_cb));
    cb_keep_back(c, (const uint8_t *)c->stg_mem.map);
    c->state = 0; c->stale = c->dirty = 0; c->busy = c->lserial > c->kserial ? c->lserial : c->kserial;
    if (--g_cb_nkept == 0) { g_kept_lo = ~0ull; g_kept_hi = 0; }
    R.gen++;
    { LONG64 dt = tk() - t0; g_tm[1] += dt; g_ck_land_tk += dt; if (dt > g_ck_land_max) g_ck_land_max = dt; }
}
static void cb_land_force(CB *c, int cause) {      /* the bytes are needed in guest memory now */
    if (R.recording && c->lserial > R.serial) cmd_submit(NULL, NULL);
    if (R.done < c->lserial) {
        for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && R.slot[i].serial <= c->lserial) slot_wait(&R.slot[i]);
        if (R.done < c->lserial) R.done = c->lserial;
    }
    g_ck_async_forced++; g_ck_land[cause]++;
    cb_land_finish(c);
}
static void cb_landing_poll(void) {                /* at the start of a batch: landings whose copy has finished; never waits */
    int open = 0;
    for (int i = 0; i < 256; i++) if (g_cb[i].state == 4 && R.done < g_cb[i].lserial) open = 1;
    if (open) for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && vkGetFenceStatus(V.device, R.slot[i].fence) == VK_SUCCESS) slot_wait(&R.slot[i]);
    for (int i = 0; i < 256; i++) if (g_cb[i].state == 4 && R.done >= g_cb[i].lserial) cb_land_finish(&g_cb[i]);
}
/* The kept and landing buffers as they are at the START of the batch (cb_kept_index, from vk_submit): during a batch the
 * set only shrinks (a buffer becomes kept only when a batch has finished), so a walk over this list sees every one. With
 * a landing in flight the count stays above zero for the whole batch, and every range a draw names was compared with
 * all 256 entries (runs p16_e_bfe / p16t_bf_g: draw preparation 4.1 -> 5.5-6.5 ms); now with the few of the list, inside
 * bounds that are exact at the start of the batch. */
static int16_t g_kept_idx[256];
static int g_kept_n;
static void cb_kept_index(void) {
    g_kept_n = 0; g_kept_lo = ~0ull; g_kept_hi = 0;
    if (!g_cb_nkept) return;
    for (int i = 0; i < 256; i++) {
        if (g_cb[i].state < 3) continue;
        g_kept_idx[g_kept_n++] = (int16_t)i;
        if (g_cb[i].addr < g_kept_lo) g_kept_lo = g_cb[i].addr;
        if (g_cb[i].addr + g_cb[i].size > g_kept_hi) g_kept_hi = g_cb[i].addr + g_cb[i].size;
    }
}
static void cb_kept_range(uint64_t addr, uint64_t size, int cause) {
    if (!g_cb_nkept || addr >= g_kept_hi || addr + size <= g_kept_lo) return;
    for (int n = 0; n < g_kept_n && g_cb_nkept; n++) {
        int i = g_kept_idx[n];
        if (g_cb[i].state < 3 || !(addr < g_cb[i].addr + g_cb[i].size && g_cb[i].addr < addr + size)) continue;
        if (g_cb[i].state == 3) cb_kept_land(&g_cb[i], cause); else cb_land_force(&g_cb[i], cause);
    }
}
/* SSBU_VK_CBKEEP, the write check (SSBU_VK_CBWW=0: off). The whole pages inside the range are asked of the write watch
 * (a stamp newer than the one of the fill = the CPU wrote one of them); the bytes of the range on its first and last,
 * shared, page are compared with the snapshot of the fill (guest memory there is untouched by us since the fill). */
static int g_cb_ww = -1;
static uint64_t cb_ww_query(CB *k) {
    uintptr_t a = (uintptr_t)k->addr, e = a + (uintptr_t)k->size, lo = (a + 4095) & ~(uintptr_t)4095, hi = e & ~(uintptr_t)4095;
    if (g_cb_merge <= 0) return nvn_front_mem_newest((void *)a, (size_t)k->size);
    return hi > lo ? nvn_front_mem_newest((void *)lo, hi - lo) : 1;
}
static int cb_guest_wrote(CB *k) {
    uintptr_t a = (uintptr_t)k->addr, e = a + (uintptr_t)k->size, lo = (a + 4095) & ~(uintptr_t)4095, hi = e & ~(uintptr_t)4095;
    if (k->snap) {
        if (hi <= lo) return memcmp((void *)a, k->snap, (size_t)k->size) != 0;
        if (lo > a && memcmp((void *)a, k->snap, lo - a)) return 1;
        if (e > hi && memcmp((void *)hi, k->snap + (hi - a), e - hi)) return 1;
    }
    uint64_t nw = cb_ww_query(k);
    if (!nw) { g_ck_unwatched++; return 0; }
    return nw > k->ww;
}
static void cb_kept_all(int cause) { for (int i = 0; i < 256 && g_cb_nkept; i++) { if (g_cb[i].state == 3) cb_kept_land(&g_cb[i], cause); else if (g_cb[i].state == 4) cb_land_force(&g_cb[i], cause); } }
static void cb_kept_age(void) {                    /* at the start of a batch */
    static int lim = -1;
    if (lim < 0) lim = getenv("SSBU_VK_CBAGE") && atoi(getenv("SSBU_VK_CBAGE")) > 0 ? atoi(getenv("SSBU_VK_CBAGE")) : 2;      /* batches a kept buffer may stay without a dispatch into it */
    if (g_cb_async < 0) g_cb_async = !(getenv("SSBU_VK_CBASYNC") && !atoi(getenv("SSBU_VK_CBASYNC")));
    cb_landing_poll();
    for (int i = 0; i < 256 && g_cb_nkept; i++) {
        CB *c = &g_cb[i];
        if (c->state == 3 && c->touch) { c->touch = 0; c->age = 0; c->kserial = R.serial + 1; continue; }      /* drawn from in the last batch: every command of that batch is in a submission up to this number */
        if (c->state != 3 || ++c->age <= lim) continue;
        if (!c->stale) {                           /* compute never wrote it: guest memory holds the same bytes: nothing to copy */
            cg_unguard(i);
            c->state = 0; c->dirty = 0; c->busy = c->kserial; g_ck_dropped++;
            if (--g_cb_nkept == 0) { g_kept_lo = ~0ull; g_kept_hi = 0; }
        } else if (g_cb_async) { g_ck_land[0]++; cb_land_start(c); }
        else cb_kept_land(c, 0);
    }
}
static void cb_poll(void) {                        /* pending CBs whose submission has finished are copied back; never waits */
    if (!g_cb_pending) return;
    if (R.done < g_cb_serial) for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && vkGetFenceStatus(V.device, R.slot[i].fence) == VK_SUCCESS) slot_wait(&R.slot[i]);
    if (R.done >= g_cb_serial) cb_land(0);
}
/* V 2026-10-21 (plan/phases/V_fps_collapse_2026-10-20.md): the owner's online match went to 28 fps, then 2, as soon as
 * Pikachu was in it: no dispatch target was ever kept any more (0.0 taken, 4.5 filled from guest memory per frame, 2.9 MB)
 * and copy-back cost 21 ms per frame inside the draw preparation. That is cb_sync called while buffers of the batch are
 * live: every one lands at once (submit, wait, and with SSBU_VK_CBMEM the CPU reads write-combined device memory, about
 * 20 ms per MB) and is freed, so nothing is kept for the next batch, and the next batch does it again: a slow path that
 * holds as long as the content that causes it is on screen. Every such landing is now counted by its cause and the first
 * ones are logged with the range and the buffer; 30 frames in a row with one print "SLOW PATH" with the cause. */
static int g_cb_hi;                                /* defined with the searches below */
enum { CBW_READ, CBW_ALIGN, CBW_WRITE, CBW_GCMD, CBW_TEX, CBW_FULL, CBW_RETIRE, CBW_OTHER, CBW_N };
static const char *const g_cbw_name[CBW_N] = {
    "a draw or dispatch reads a range that overlaps a compute buffer of this batch without lying inside it",
    "a draw or dispatch reads a range inside a compute buffer of this batch at an offset its descriptor cannot take",
    "a dispatch writes a range that overlaps another compute buffer of this batch",
    "a guest-memory command (buffer copy / uniform update / buffer clear)",
    "a texture copy or texel read-back",
    "2048 dispatches in one batch",
    "memory the game gave back",
    "other"};
static int g_cbw = CBW_OTHER;
static uint64_t g_cbw_addr, g_cbw_size;
static LONG64 g_ck_mid[CBW_N], g_ck_mid_bytes, g_ck_mid_gpu, g_ck_asm, g_ck_asm_bytes;
static LONG64 g_mid_last_frame = -10, g_mid_run_start, g_mid_run_n;
static int g_mid_reported, g_mid_last_why;
static char g_mid_last_desc[256];
#define CB_WHY(w, a, n) (g_cbw = (w), g_cbw_addr = (uint64_t)(a), g_cbw_size = (uint64_t)(n))
static void cb_mid_note(int why) {                 /* cb_sync is about to land the buffers live in this batch */
    uint64_t bytes = 0; const CB *hit = NULL;
    for (int i = 0; i < g_cb_hi; i++) {
        const CB *c = &g_cb[i];
        if (c->state != 1) continue;
        bytes += c->size;
        if (!hit && g_cbw_size && g_cbw_addr < c->addr + c->size && c->addr < g_cbw_addr + g_cbw_size) hit = c;
    }
    g_ck_mid[why]++; g_ck_mid_bytes += (LONG64)bytes;
    if (hit) snprintf(g_mid_last_desc, sizeof g_mid_last_desc, "range %llx+%llu, compute buffer %llx+%llu", (unsigned long long)g_cbw_addr, (unsigned long long)g_cbw_size, (unsigned long long)hit->addr, (unsigned long long)hit->size);
    else if (g_cbw_size) snprintf(g_mid_last_desc, sizeof g_mid_last_desc, "range %llx+%llu", (unsigned long long)g_cbw_addr, (unsigned long long)g_cbw_size);
    else g_mid_last_desc[0] = 0;
    g_mid_last_why = why; g_cbw_size = 0;
    { static int n; if (n++ < 16) printf("nvn_vk_cbkeep: frame %lld: compute buffers landed INSIDE a batch (%d live, %llu KB): %s%s%s\n", (long long)g_cur_frame, g_cb_live, (unsigned long long)(bytes >> 10), g_cbw_name[why], g_mid_last_desc[0] ? ": " : "", g_mid_last_desc); }
    if (g_cur_frame != g_mid_last_frame) {
        if (g_cur_frame - g_mid_last_frame > 2) { g_mid_run_start = g_cur_frame; g_mid_run_n = 0; }
        g_mid_run_n++; g_mid_last_frame = g_cur_frame;
        if (g_mid_run_n == 30 && !g_mid_reported) {
            g_mid_reported = 1;
            printf("nvn_vk_cbkeep: SLOW PATH since frame %lld: compute buffers are landed inside the batch in 30 frames in a row, so none is kept between batches (each one filled from guest memory and copied back every frame); cause: %s%s%s\n",
                   (long long)g_mid_run_start, g_cbw_name[why], g_mid_last_desc[0] ? ": " : "", g_mid_last_desc);
        }
    }
}
static int g_cb_midcopy = -1;                      /* SSBU_VK_CBMIDCOPY=0: a landing inside a batch reads the buffer with the CPU as before */
static void cb_sync(void) {                        /* guest memory must hold what compute wrote, now */
    int mid = g_cb_live != 0, why = g_cbw;
    g_cbw = CBW_OTHER;
    if (!g_cb_live && !g_cb_pending) { g_cbw_size = 0; return; }
    if (g_cb_live) {
        cb_mid_note(why);
        if (g_cb_midcopy < 0) g_cb_midcopy = !(getenv("SSBU_VK_CBMIDCOPY") && !atoi(getenv("SSBU_VK_CBMIDCOPY")));
        if (g_cb_keep > 0 && g_cb_dl == 1 && g_cb_midcopy) {
            /* the buffers are in write-combined device memory: the GPU copies them into ordinary memory in the submission
             * waited for below, and cb_land merges from there (the CPU reading the buffer itself: ~20 ms per MB) */
            pass_end(); cmd_begin();
            VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            for (int i = 0; i < g_cb_hi; i++) {
                CB *c = &g_cb[i];
                if (c->state != 1) continue;
                cb_stg_ensure(c);
                VkBufferCopy r = {0, 0, c->size};
                vkCmdCopyBuffer(R.cmd, c->buf, c->stg, 1, &r);
                c->mid_stg = 1; g_ck_mid_gpu++;
            }
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        }
        g_cb_serial = cmd_submit(NULL, NULL); R.cb_syncs++;
    }
    if (R.done < g_cb_serial) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < NSLOT; i++) if (R.slot[i].pending && R.slot[i].serial <= g_cb_serial) slot_wait(&R.slot[i]);
        if (R.done < g_cb_serial) R.done = g_cb_serial;
        QueryPerformanceCounter(&t1);
        R.cb_waits++; R.cb_wait_ticks += t1.QuadPart - t0.QuadPart;
    }
    cb_land(mid);
}
/* PR #41 (v1Ta44): finalization runs in queue order, while the guest storage still exists. Drain every overlapping CB,
 * including state 4 (an asynchronous landing). Never use the per-batch kept index here: cb_sync can add entries to it
 * after that index was built. Unrelated kept buffers retain their GPU-side contents and their reuse opportunity. */
static int cb_in_range(const CB *c, const Range *r) {
    if (!c->size || !r->size) return 0;
    return c->addr >= r->addr ? c->addr - r->addr < r->size : r->addr - c->addr < c->size;
}
void nvn_vk_memory_retire(void *arg) {
    const Range *range = arg;
    for (int i = 0; i < 256; i++) {
        if (g_cb[i].state && g_cb[i].state <= 2 && cb_in_range(&g_cb[i], range)) { CB_WHY(CBW_RETIRE, range->addr, range->size); cb_sync(); break; }
    }
    for (int i = 0; i < 256; i++) {
        CB *c = &g_cb[i];
        if (c->state < 3 || !cb_in_range(c, range)) continue;
        if (c->state == 3) cb_kept_land(c, 3); else cb_land_force(c, 3);
    }
}
static void cb_batch_end(void) {
    if (!g_cb_live) return;
    g_cb_serial = cmd_submit(NULL, NULL);
    for (int i = 0; i < 256; i++) if (g_cb[i].state == 1) { g_cb[i].state = 2; g_cb[i].kserial = g_cb_serial; g_cb[i].touch = 0; }
    g_cb_pending = g_cb_live; g_cb_live = 0; g_live_lo = ~0ull; g_live_hi = 0;
}
/* Round "four players" (P3): both searches are made for every buffer a draw names (about 2.5 million entries looked at per
 * frame with four fighters: 1.9 ms of the render thread). g_cb_hi = one past the highest entry cb_new has ever handed out
 * (it never goes down): entries above it have state 0, so stopping there finds exactly what the walk over all 256 found,
 * in the same order. SSBU_VK_CBSCAN=0 = the walk over all 256 as before. */
static int g_cb_hi, g_cb_scan = -1, g_cb_bound = -1;
static CB *cb_inside(uint64_t addr, uint64_t size) {
    int n = g_cb_scan ? g_cb_hi : 256;
    for (int i = 0; i < n; i++) if (g_cb[i].state == 1 && addr >= g_cb[i].addr && addr + size <= g_cb[i].addr + g_cb[i].size) return &g_cb[i];
    return NULL;
}
static int cb_overlap(uint64_t addr, uint64_t size) {
    int n = g_cb_scan ? g_cb_hi : 256;
    for (int i = 0; i < n; i++) if (g_cb[i].state == 1 && addr < g_cb[i].addr + g_cb[i].size && g_cb[i].addr < addr + size) return 1;
    return 0;
}
static CB *cb_new(uint64_t addr, uint64_t size) {
    CB *c = NULL, *empty = NULL, *victim = NULL;
    if (cb_keep_mode() && g_cb_nkept) {            /* SSBU_VK_CBKEEP: the buffer of the last dispatch into this very range, as the GPU left it */
        for (int i = 0; i < g_cb_hi; i++) {
            CB *k = &g_cb[i];
            if (k->state != 3 || k->addr != addr || k->size != size) continue;
            if (g_cb_ww && cb_guest_wrote(k)) { cb_kept_land(k, 4); break; }      /* the game wrote into the range: merged back, then filled again below */
            k->state = 1; k->dirty = 0; g_cb_live++; g_ck_hits++;
            if (--g_cb_nkept == 0) { g_kept_lo = ~0ull; g_kept_hi = 0; }
            if (addr < g_live_lo) g_live_lo = addr;
            if (addr + size > g_live_hi) g_live_hi = addr + size;
            return k;
        }
        cb_kept_range(addr, size, 2);
    }
    for (int i = 0; i < 256 && !c; i++) {
        CB *k = &g_cb[i];
        if (k->state || k->busy > R.done) continue;
        if (!k->buf) { if (!empty) empty = k; }
        else if (k->cap >= size && k->cap <= size * 2 + 4096) c = k;
        else if (!victim) victim = k;
    }
    if (!c) {
        c = empty ? empty : victim;
        if (!c) vkd_fail("more than 256 storage buffers written by compute in one batch");
        if (c->buf) { vkDestroyBuffer(V.device, c->buf, NULL); vmem_free(c->mem); c->buf = NULL; }      /* free and not busy: nothing in flight names it */
        if (g_cb_cached < 0) {                     /* read back by the CPU: cached host memory when the device has it */
            g_cb_cached = 0;
            for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((D.mp.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) g_cb_cached = 1;
        }
        VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        VkMemoryRequirements rq;
        c->cap = (size + 4095) & ~(uint64_t)4095;
        bi.size = c->cap; bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VKC(vkCreateBuffer(V.device, &bi, NULL, &c->buf));
        vkGetBufferMemoryRequirements(V.device, c->buf, &rq);
        VkFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | (g_cb_cached ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0);
        int found = 0;
        for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++) if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & want) == want) found = 1;
        if (!found) want &= ~(VkFlags)VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        if (g_cb_dl < 0) g_cb_dl = getenv("SSBU_VK_CBMEM") ? atoi(getenv("SSBU_VK_CBMEM")) : cb_keep_mode() ? 1 : 0;      /* default 1 with SSBU_VK_CBKEEP (the CPU then reads them only at a landing), 0 without */
        if (g_cb_dl == 1) {                        /* round 16: where the GPU writes and reads them without the bus (the CPU reads them only at a landing) */
            VkFlags dl = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            for (uint32_t i = 0; i < D.mp.memoryTypeCount; i++)
                if ((rq.memoryTypeBits >> i & 1) && (D.mp.memoryTypes[i].propertyFlags & dl) == dl && D.mp.memoryHeaps[D.mp.memoryTypes[i].heapIndex].size >= (2ull << 30)) { want = dl; break; }
        }
        c->mem = vmem_alloc(rq, want, want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? 3 : 2);
        VKC(vkBindBufferMemory(V.device, c->buf, c->mem.mem, c->mem.off));
        R.cb_made++;
    }
    c->addr = addr; c->size = size; c->state = 1; c->dirty = 0;
    if (addr < g_live_lo) g_live_lo = addr;
    if (addr + size > g_live_hi) g_live_hi = addr + size;
    if (g_cb_scan < 0) g_cb_scan = !(getenv("SSBU_VK_CBSCAN") && !atoi(getenv("SSBU_VK_CBSCAN")));
    if ((int)(c - g_cb) + 1 > g_cb_hi) g_cb_hi = (int)(c - g_cb) + 1;
    if (g_cb_merge < 0) g_cb_merge = !(getenv("SSBU_VK_CBMERGE") && !atoi(getenv("SSBU_VK_CBMERGE")));
    if (g_cb_ww < 0) g_cb_ww = !(getenv("SSBU_VK_CBWW") && !atoi(getenv("SSBU_VK_CBWW")));
    if (g_cb_keep > 0 && g_cb_ww) c->ww = cb_ww_query(c);      /* BEFORE the copy: a write between this question and the copy is in the copy and is also seen at the next question (the buffer is then filled once more) */
    if (g_cb_merge) {                              /* the snapshot the copy-back is compared with (cb_land) */
        if (c->snap_cap < size) { free(c->snap); c->snap_cap = (size + 4095) & ~(uint64_t)4095; c->snap = malloc((size_t)c->snap_cap); }
        memcpy(c->snap, P(addr), (size_t)size);
        memcpy(c->mem.map, c->snap, (size_t)size);
    } else
    memcpy(c->mem.map, P(addr), (size_t)size);
    c->stale = 0; g_ck_fills++; g_ck_fill_bytes += (LONG64)size;
    g_cb_live++;
    return c;
}
typedef struct { uint64_t off, size; int zero; VkBuffer buf; } BufRef;
/* V 2026-10-21, SSBU_VK_CBASM (default 1; 0 = as before: the buffers land first): a range a draw or dispatch reads that
 * overlaps compute buffers (live in this batch, or kept) without lying inside one, or lies inside one at an offset its
 * descriptor cannot take, is put together ON THE GPU: guest memory is streamed into a fresh ring slot as for any range,
 * then the overlapping parts are copied over it from the buffers, in command order. What the command reads is what the
 * old path gave it after landing the buffers (compute's bytes inside them, guest memory outside), without the wait and
 * without the CPU reading device memory, and the buffers stay kept. Not when one of them is pending, landing, or kept
 * with a write of the game into its range: the old path then. */
static int g_cb_asm = -1;
static int cb_assemble(uint64_t addr, uint64_t size, BufRef *b) {
    if (g_cb_asm < 0) g_cb_asm = !(getenv("SSBU_VK_CBASM") && !atoi(getenv("SSBU_VK_CBASM")));
    if (!g_cb_asm || g_cb_keep <= 0) return 0;
    for (int i = 0; i < g_cb_hi; i++) {                /* first, without side effects: can it be done? */
        CB *c = &g_cb[i];
        if (!c->state || !(addr < c->addr + c->size && c->addr < addr + size)) continue;
        if (c->state == 2 || c->state == 4) return 0;
        if (c->state == 3) {
            if (c->wchk != g_cb_batch) { c->wchk = g_cb_batch; c->wrote = (uint8_t)(g_cb_ww > 0 && cb_guest_wrote(c)); }
            if (c->wrote) return 0;
        }
    }
    uint64_t off = ring_raw(P(addr), size);        /* may wrap the ring (wait for everything; pending buffers become kept): asked again below */
    static int16_t idx[256];
    int n = 0;
    for (int i = 0; i < g_cb_hi; i++) {
        CB *c = &g_cb[i];
        if (!c->state || !(addr < c->addr + c->size && c->addr < addr + size)) continue;
        if (c->state == 2 || c->state == 4) return 0;
        if (c->state == 3) {
            if (c->wchk != g_cb_batch) { c->wchk = g_cb_batch; c->wrote = (uint8_t)(g_cb_ww > 0 && cb_guest_wrote(c)); }
            if (c->wrote) return 0;
        }
        idx[n++] = (int16_t)i;
    }
    b->buf = R.ring; b->off = off; b->size = size;
    if (!n) return 0;                              /* landed by a ring wrap meanwhile: the slot holds it all */
    pass_end(); cmd_begin();
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    for (int k = 0; k < n; k++) {
        CB *c = &g_cb[idx[k]];
        uint64_t lo = addr > c->addr ? addr : c->addr, hi = addr + size < c->addr + c->size ? addr + size : c->addr + c->size;
        VkBufferCopy r = {lo - c->addr, off + (lo - addr), hi - lo};
        vkCmdCopyBuffer(R.cmd, c->buf, R.ring, 1, &r);
        if (c->state == 3) c->touch = 1;           /* named by this batch: not given out again before it has finished (cb_kept_age, cb_kept_land) */
        g_ck_asm_bytes += (LONG64)(hi - lo);
    }
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    g_ck_asm++;
    { static int lg; if (lg++ < 8) printf("nvn_vk_cbkeep: frame %lld: range %llx+%llu put together on the GPU from %d compute buffer(s) (first %llx+%llu) and guest memory, no landing\n", (long long)g_cur_frame, (unsigned long long)addr, (unsigned long long)size, n, (unsigned long long)g_cb[idx[0]].addr, (unsigned long long)g_cb[idx[0]].size); }
    return 1;
}
/* Where a command reads the guest range from: a CB of this batch that contains it, else the stream ring. align: the offset
 * alignment the use needs (storage descriptors), 1 = none. Returns 1 when a CB is bound. */
static int buf_ref(uint64_t addr, uint64_t size, uint64_t align, BufRef *b) {
    b->zero = 0; b->size = size;
    if (g_cb_bound < 0) g_cb_bound = !(getenv("SSBU_VK_CBBOUND") && !atoi(getenv("SSBU_VK_CBBOUND")));      /* round 16, default 1 (=0: cb_inside / cb_overlap are asked for every range as before): a range outside the bounds of the live buffers cannot be inside or overlap one. Proof of the picture: run p16_h_bf5 (old path + this) = p16_d_bfd (this off), all pixels, three frames. NOTE for picture tests on battle_field: the picture has two forms that alternate with the PRESENTED frame number (17-19 % of the pixels, small differences): compare shots whose "saved at presented frame" numbers have the same parity. */
    if (g_cb_live && (!g_cb_bound || (addr < g_live_hi && addr + size > g_live_lo))) {
        CB *c = cb_inside(addr, size);
        if (c && (addr - c->addr) % align == 0) { b->buf = c->buf; b->off = addr - c->addr; return 1; }
        if (cb_overlap(addr, size)) {
            if (cb_assemble(addr, size, b)) return 1;
            CB_WHY(c ? CBW_ALIGN : CBW_READ, addr, size); cb_sync();
        }
    }
    if (g_cb_nkept && addr < g_kept_hi && addr + size > g_kept_lo) {
        /* SSBU_VK_CBKEEP: guest memory would be read. A kept buffer that CONTAINS the range holds exactly the bytes the old
         * path would have copied back and streamed (unless the game wrote into the range since: asked once per batch):
         * it is bound as it is (SSBU_VK_CBDRAW=0: it lands first and the range is streamed; landing reads device
         * memory, up to 19 ms: runs p16_e_bfe / p16t_bf_d). Any other overlap: the buffer lands first. */
        static int on = -1;
        if (on < 0) on = !(getenv("SSBU_VK_CBDRAW") && !atoi(getenv("SSBU_VK_CBDRAW")));
        if (on) for (int n = 0; n < g_kept_n; n++) {
            CB *k = &g_cb[g_kept_idx[n]];
            if (k->state != 3 || addr < k->addr || addr + size > k->addr + k->size || (addr - k->addr) % align) continue;
            if (k->wchk != g_cb_batch) { k->wchk = g_cb_batch; k->wrote = (uint8_t)(g_cb_ww > 0 && cb_guest_wrote(k)); }
            if (k->wrote) break;
            b->buf = k->buf; b->off = addr - k->addr; k->touch = 1; g_ck_drawn++;
            return 1;
        }
        if (on && cb_assemble(addr, size, b)) return 1;
        cb_kept_range(addr, size, 1);
    }
    b->off = ring_put(addr, size); b->buf = R.ring;      /* in this order: a ring wrap submits */
    return 0;
}
static void compute_pipe_make(VStage *v);
static void compute_dispatch_run(Prog *p, Stage *st, VStage *v, uint32_t gx, uint32_t gy, uint32_t gz);
static void compute_dispatch(uint32_t gx, uint32_t gy, uint32_t gz) {
    Prog *p = R.prog[5];
    static int nlog;
    R.dispatches++;
    if (!p) { if (nlog++ < 10) printf("nvn_vk: DispatchCompute with no compute program bound\n"); return; }
    Stage *st = &p->st[5];
    if (!st->code) { nvn_vk_missing("nvnCommandBufferDispatchCompute of a program that has no machine code (assembly text only)"); return; }
    uint32_t key[4] = {0, 0, R.tex_int[5], R.tex_sint[5]};
    VStage *v = vstage_get(p, 5, key, 0);
    if (!v) { nvn_vk_missing("nvnCommandBufferDispatchCompute: the compute stage could not be translated or compiled (the log names the program)"); return; }
    if (v->images) { nvn_vk_missing("a compute program that uses images (nvnCommandBufferBindImage)"); return; }
    if (v->nsmp > 32) { nvn_vk_missing("a compute program with more than 32 samplers"); return; }
    if (!gx || !gy || !gz) return;
    int cp_how = 0;
    if (v->cp_state != 2) { LONG64 t0 = tk(); cp_how = compute_pipe_ensure(v); if (cp_how) { hold_note(cp_how == 1 ? H_PIPE_DRAW : H_PIPE_WAIT, tk() - t0); g_tm[5] += tk() - t0; } }
    if (!v->cp_recorded) { v->cp_recorded = 1; piperec_append(NULL, v, NULL, 1); }
    if (cp_how == 1) {
        g_pipe_miss++;
        printf("nvn_vk: compute program #%d: pipeline made at its first dispatch (%d bytes of machine code, work group %u x %u x %u, constant buffers 0x%x, storage 0x%x written 0x%x, %d samplers)\n", p->serial, st->code_len,
               st->local[0], st->local[1], st->local[2], v->cbufs, v->ssbos, v->ssbo_w, v->nsmp);
    }
    compute_dispatch_run(p, st, v, gx, gy, gz);
}
static void compute_pipe_make(VStage *v) {         /* any thread */
    {
        VkDescriptorSetLayoutBinding b0[40], b1[64];
        int n0 = 0, n1 = 0;
        for (unsigned n = 0; n < 18; n++) if (v->cbufs >> n & 1) { VkDescriptorSetLayoutBinding b = {n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        for (unsigned k = 0; k < 16; k++) if (v->ssbos >> k & 1) { VkDescriptorSetLayoutBinding b = {20 + k, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        if (v->ssbo_w) { VkDescriptorSetLayoutBinding b = {38, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b0[n0++] = b; }
        for (int i = 0; i < v->nsmp; i++) { VkDescriptorSetLayoutBinding b = {v->smp[i].binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; b1[n1++] = b; }
        for (int i = 0; i < 2; i++) {
            VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            li.bindingCount = (uint32_t)(i ? n1 : n0); li.pBindings = i ? b1 : b0;
            VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &v->cdl[i]));
        }
        VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 2; pl.pSetLayouts = v->cdl;
        VKC(vkCreatePipelineLayout(V.device, &pl, NULL, &v->cl));
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = v->mod; ci.stage.pName = "main"; ci.layout = v->cl;
        VkPipeline cp = NULL;
        VkResult r = pw_compute(&ci, &cp);         /* PR #33: vkCreateComputePipelines (g_pcache), measured with SSBU_VK_PREWARM_PROFILE=1 */
        if (r != VK_SUCCESS) vkd_fail("vkCreateComputePipelines (compute stage %u): %s", v->id, vk_result_name(r));
        v->cp = cp;
        InterlockedIncrement64(&R.pipelines);
    }
}
static void compute_dispatch_run(Prog *p, Stage *st, VStage *v, uint32_t gx, uint32_t gy, uint32_t gz) {
    static BufRef bufs[40];
    int nbuf = 0;
    int16_t wcb[16];
    uint64_t al = V.props.limits.minStorageBufferOffsetAlignment ? V.props.limits.minStorageBufferOffsetAlignment : 1;
    pass_end();
    /* textures, then buffers (both may submit: upload, ring wrap, copy-back), then the mask word, then the commands */
    g_ndtex = 0;
    for (int i = 0; i < v->nsmp; i++) resolve_sampler(5, v, i);
    if (g_ncdisp == 2048 || g_cb_pending) { CB_WHY(g_ncdisp == 2048 ? CBW_FULL : CBW_OTHER, 0, 0); cb_sync(); }
    uint64_t wm_off = 0;
    for (int attempt = 0; ; attempt++) {           /* a copy-back in here (cb_sync) frees the CBs chosen before it: everything is then chosen again */
        uint32_t epoch = g_cb_epoch;
        nbuf = 0;
        memset(wcb, 0xff, sizeof wcb);
        for (unsigned n = 0; n < 18; n++) {
            if (!(v->cbufs >> n & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = 0; b->buf = R.ring;
            if (n == 2) { b->off = ring_raw(R.c2[5], sizeof R.c2[5]); b->size = sizeof R.c2[5]; }
            else if (n == 1 && st->ctl[3] && st->ctl[4]) { b->size = st->ctl[3]; b->off = ring_put((uint64_t)(uintptr_t)(st->code + st->ctl[4]), b->size); }
            else if (n >= 3 && R.ubo[5][n - 3].addr && R.ubo[5][n - 3].size) buf_ref(R.ubo[5][n - 3].addr, R.ubo[5][n - 3].size, al, b);
            else b->zero = 1;                      /* c0 outside the storage descriptors, or nothing bound: zeros (OpenGL: nothing bound) */
        }
        for (unsigned k = 0; k < 16; k++) {
            if (!(v->ssbos >> k & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            Range r = R.ssbo[5][k];
            b->zero = !(r.addr && r.size); b->buf = R.ring;
            if (b->zero) continue;
            if (!(v->ssbo_w >> k & 1)) { buf_ref(r.addr, r.size, al, b); continue; }
            CB *c = g_cb_live ? cb_inside(r.addr, r.size) : NULL;
            if (c && (r.addr - c->addr) % al) c = NULL;
            if (!c) { if (g_cb_live && cb_overlap(r.addr, r.size)) { CB_WHY(CBW_WRITE, r.addr, r.size); cb_sync(); } c = cb_new(r.addr, r.size); }
            b->buf = c->buf; b->off = r.addr - c->addr; b->size = r.size; wcb[k] = (int16_t)(c - g_cb);
        }
        if (v->ssbo_w) { static const uint32_t zero[4]; wm_off = ring_raw(zero, 16); }
        if (epoch == g_cb_epoch) break;
        if (attempt == 3) vkd_fail("compute program #%d: its buffers cannot be bound without a copy-back in between", p->serial);
    }
    nvn_front_ccmp_dispatch(R.ubo[5], R.ssbo[5], v->ssbo_w, st->code, st->code_len, gx, gy, gz);      /* SSBU_COMPUTE_CMP */
    cmd_begin();
    if (g_pt_on > 0 && g_pt_slot[R.cur].last_what != 1) pt_mark(0, 0, 0, 0, 0, 0, 1);
    static VkWriteDescriptorSet w[80];
    static VkDescriptorBufferInfo bi[40];
    static VkDescriptorImageInfo ii[64];
    VkDescriptorSet sets[2];
    int nw = 0, ib = 0;
    sets[0] = set_alloc(v->cdl[0]); sets[1] = set_alloc(v->cdl[1]);
    memset(w, 0, sizeof w);
    for (unsigned n = 0; n < 34; n++) {
        unsigned bit = n < 18 ? v->cbufs >> n & 1 : v->ssbos >> (n - 18) & 1;
        if (!bit) continue;
        BufRef *b = &bufs[ib];
        bi[ib].buffer = b->zero ? R.zero : b->buf; bi[ib].offset = b->zero ? 0 : b->off; bi[ib].range = b->zero ? 65536 : b->size;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = n < 18 ? n : 20 + n - 18; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
    }
    if (v->ssbo_w) {
        bi[ib].buffer = R.ring; bi[ib].offset = wm_off; bi[ib].range = 16;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = 38; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
    }
    for (int i = 0; i < v->nsmp; i++) {
        ii[i].sampler = g_dtex[i].smp; ii[i].imageView = g_dtex[i].view; ii[i].imageLayout = g_dtex[i].layout;
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[1]; w[nw].dstBinding = v->smp[i].binding; w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[nw].pImageInfo = &ii[i]; nw++;
    }
    if (nw) vkUpdateDescriptorSets(V.device, (uint32_t)nw, w, 0, NULL);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->cp);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, v->cl, 0, 2, sets, 0, NULL);
    if (D.has_ckpt) ckpt_mark(2, 0, gx, gy, gz, 0, 0);
    vkCmdDispatch(R.cmd, gx, gy, gz);
    if (v->ssbo_w) {                               /* what the dispatch wrote is read by later dispatches and draws of this command buffer */
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT; mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        CDisp *d = &g_cdisp[g_ncdisp++];
        d->wm_off = wm_off; d->written = v->ssbo_w; memcpy(d->cb, wcb, sizeof wcb);
    }
}
/* Everything of a draw up to the draw call. topo: NVN primitive. Returns 0 when the draw cannot be done (counted as skipped). */
static uint64_t g_dt_voff[16]; static VkBuffer g_dt_vb[16]; int nvn_front_draw_tag(void);      /* round 17, test (SSBU_DRAWTAG): see dt_vk */
/* MAC2 / V_novids: the vertex layout of the draw into the pipeline key, without VK_EXT_vertex_input_dynamic_state. The
 * same bindings, in the same order, with the same formats, as step 4 of draw_prepare gives vkCmdSetVertexInputEXT on the
 * other devices (2026-10-21: the constant binding's integer format also for ints bit 16 + i, as step 4 does). */
static void vi_key(PKey *k, const uint64_t *voff, uint32_t ints, uint32_t sints) {
    NvkVl l;
    uint32_t bmap[32], nb = 0, na = 0, need_const = 0;
    memset(&l, 0, sizeof l);
    memset(bmap, 0xff, sizeof bmap);
    for (int i = 0; i < 16; i++) {
        uint32_t a = i < R.nattr ? R.vattr[i] : 0;
        int nf = (int)(a & 0xff), stream = (int)((a >> 8) & 31);
        VkFormat f = nf ? vertex_vkfmt(nf) : VK_FORMAT_UNDEFINED;
        l.loc[na] = (uint32_t)i;
        if (f && stream < 16 && voff[stream] != ~0ull) {
            if (bmap[stream] == 0xffffffffu) {
                bmap[stream] = nb;
                if (R.vstream[stream].divisor) { l.rate |= 1u << nb; l.div[nb] = R.vstream[stream].divisor; }
                nb++;
            }
            l.bind[na] = bmap[stream]; l.fmt[na] = (uint32_t)f; l.off[na] = a >> 14; na++;
        } else {                                   /* not supplied: the constant binding, as in step 4 */
            int in = (ints >> i & 1) || (ints >> (16 + i) & 1);
            l.bind[na] = 16; l.fmt[na] = in ? ((sints >> i & 1) ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R32G32B32A32_SFLOAT;
            l.off[na] = in ? 16 : 0;
            need_const = 1; na++;
        }
    }
    if (need_const) {
        for (uint32_t i = 0; i < na; i++) if (l.bind[i] == 16) l.bind[i] = nb;
        l.rate |= 1u << nb; l.div[nb] = 1; nb++;
    }
    l.na = na; l.nb = nb;
#ifdef __APPLE__
    k->vi = l;
#else
    nvk_vl_set(k, nvk_vl_id(&l));
#endif
}
static int draw_prepare(unsigned prim) {
    static const int8_t topo[7] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST, -1, VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN};
    static const uint8_t tclass[7] = {0, 1, 1, 1, 2, 2, 2};
    if (!R.have_targets || R.render_off) return 0;
    if (prim > 6 || topo[prim] < 0) { static char msg[64]; snprintf(msg, sizeof msg, "a draw with NVN primitive type %u", prim); nvn_vk_missing(msg); return 0; }
    if (!R.prog[0]) return 0;
    if (R.prog[2] || R.prog[3] || R.prog[4]) { nvn_vk_missing("a draw with a geometry or tessellation stage"); return 0; }
    uint32_t ints = 0, sints = 0;
    for (int i = 0; i < 16 && i < R.nattr; i++) {
        int nf = (int)(R.vattr[i] & 0xff), k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
        if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) { ints |= 1u << i; if (k == 3 || k == 8 || k == 11) sints |= 1u << i; }
        if (nf == 0x3E) ints |= 1u << i;
        if (nf == 0x6A) { ints |= 1u << i; sints |= 1u << i; }
        if (nf == 0x69 && vfmt_snorm_emu()) ints |= 1u << (16 + i);      /* fetched as R32_UINT, decoded by the vertex code */
    }
    uint32_t key0[4] = {ints, sints, R.tex_int[0], R.tex_sint[0]}, key1[4] = {0, 0, R.tex_int[1], R.tex_sint[1]};
    int depth_only = !R.prog[1];
    /* fragment inputs the vertex code never writes are declared by a vertex variant of their own, left at (0, 0, 0, 1). Session 4:
     * the variant the program used last is asked for first (session 3 asked for the plain one at every draw of such a pair,
     * which hashed the machine code each time). */
    VStage *cur = R.prog[0]->st[0].vk;
    VStage *fs = depth_only ? NULL : vstage_get(R.prog[1], 1, key1, 0), *vs = vstage_get(R.prog[0], 0, key0, cur ? cur->fo : 0);
    if (vs) { uint32_t need = fs ? fs->in_attr & ~(vs->out_attr & ~vs->fo) : 0; if (need != vs->fo) vs = vstage_get(R.prog[0], 0, key0, need); }
    if (g_skip_flag) { g_skip_flag = 0; if (!vs || (!depth_only && !fs)) return 0; }      /* shader_wait = skip: the draw is left out (counted in vstage_get) */
    if (!vs || (!depth_only && !fs)) { nvn_vk_missing("a draw whose vertex or fragment stage could not be translated or compiled (the log names the stage)"); return 0; }
    if (D.has_ckpt) { g_dvs = vs; g_dfs = fs; }    /* PR #32: for the device loss report's draw markers */

    /* 1. textures (may upload, change layouts, end the pass) */
    g_ndtex = 0;
    for (int i = 0; i < vs->nsmp; i++) resolve_sampler(0, vs, i);
    if (fs) for (int i = 0; i < fs->nsmp; i++) resolve_sampler(1, fs, i);
    /* 2. buffers: every range the draw reads, into the ring (may wrap the ring = submit) */
    static BufRef bufs[80];
    int nbuf = 0;
    uint64_t sal = V.props.limits.minStorageBufferOffsetAlignment ? V.props.limits.minStorageBufferOffsetAlignment : 1;
    for (int s = 0; s < (fs ? 2 : 1); s++) {
        VStage *v = s ? fs : vs;
        Stage *st = &R.prog[s]->st[s];
        for (unsigned n = 1; n < 18; n++) {
            if (!(v->cbufs >> n & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = 0; b->buf = R.ring;
            if (n == 2) { b->off = ring_raw(R.c2[s], sizeof R.c2[s]); b->size = sizeof R.c2[s]; }
            else if (n == 1 && st->ctl[3] && st->ctl[4]) { b->size = st->ctl[3]; b->off = ring_put((uint64_t)(uintptr_t)(st->code - 0x30 + st->ctl[4]), b->size); }
            else if (n >= 3 && R.ubo[s][n - 3].addr && R.ubo[s][n - 3].size) buf_ref(R.ubo[s][n - 3].addr, R.ubo[s][n - 3].size, sal, b);
            else b->zero = 1;
        }
        for (unsigned k = 0; k < 16; k++) {
            if (!(v->ssbos >> k & 1)) continue;
            BufRef *b = &bufs[nbuf++];
            b->zero = !(R.ssbo[s][k].addr && R.ssbo[s][k].size); b->buf = R.ring;
            if (!b->zero) buf_ref(R.ssbo[s][k].addr, R.ssbo[s][k].size, sal, b);
        }
    }
    /* vertex streams: only the ones an attribute names (a stale binding of another mesh is not read, and is not copied) */
    uint64_t voff[16];
    VkBuffer vbufs[16];
    uint32_t streams = 0;
    int from_cb = 0;
    for (int i = 0; i < 16 && i < R.nattr; i++) if (R.vattr[i] & 0xff) streams |= 1u << ((R.vattr[i] >> 8) & 31);
    for (int i = 0; i < 16; i++) {
        BufRef b;
        voff[i] = ~0ull; vbufs[i] = R.ring;
        if (!(streams >> i & 1) || !R.vbuf[i].addr || !R.vbuf[i].size) continue;
        from_cb |= buf_ref(R.vbuf[i].addr, R.vbuf[i].size, 1, &b);
        voff[i] = b.off; vbufs[i] = b.buf;
    }
    if (nvn_front_draw_tag()) { memcpy(g_dt_voff, voff, sizeof g_dt_voff); memcpy(g_dt_vb, vbufs, sizeof g_dt_vb); }      /* round 17, test (SSBU_DRAWTAG) */
    R.cb_draws += from_cb;
    /* 3. pass and pipeline */
    if (!pass_begin()) return 0;
    PKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.fs = fs ? fs->id : 0; k.topo_class = tclass[prim]; k.logic = R.logic; k.sshade = fs ? (uint8_t)R.prog[1]->sample_shading : 0;
    if (!D.has_eds3) {                             /* without extended dynamic state 3 this state is part of the key */
        k.chanmask = R.chanmask; k.sample_mask = R.sample_mask; k.poly = R.poly[2] < 3 ? R.poly[2] : 2;
        k.a2c = R.ms[1] != 0; k.dclamp = R.depth_clamp && D.f2.features.depthClamp; k.blend_on = R.blend_on;
        memcpy(k.blend, R.blend, sizeof k.blend);
    }
    if (!D.has_vids) vi_key(&k, voff, ints, sints);      /* MAC2 / V_novids */
    pkey_targets(&k);
    Pipe *p = pipe_get(&k, vs, fs);
    if (!p) return 0;                              /* shader_wait = skip: the pipeline is being made by a worker, the draw is left out (counted in pipe_get) */
    if (!R.pipe_bound) R.dyn3_dirty = 1;          /* a new command buffer, or a pipeline of our own with static state was bound */
    if (R.pipe_bound != p) { vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p); R.pipe_bound = p; }
    if (R.dyn_dirty) { set_dynamic_state(); R.dyn_dirty = 0; }
    if (D.has_eds3 && (R.dyn3_dirty || R.dyn3_samples != k.samples)) { set_dynamic_state3(k.samples); R.dyn3_dirty = 0; }
    vkCmdSetPrimitiveTopology(R.cmd, (VkPrimitiveTopology)topo[prim]);
    vkCmdSetPrimitiveRestartEnable(R.cmd, R.restart && ((D.has_plr && !(g_vk_test & 16)) || prim == 3 || prim == 5 || prim == 6));      /* SSBU_VK_TEST bit 16 (session 14, test): no restart in list topologies */
    /* 4. vertex input */
    {
        VkVertexInputBindingDescription2EXT bd[17];
        VkVertexInputAttributeDescription2EXT ad[16];
        VkBuffer vb[17];
        VkDeviceSize vo[17];
        uint32_t nb = 0, na = 0, bmap[32], need_const = 0;
        memset(bmap, 0xff, sizeof bmap);
        for (int i = 0; i < 16; i++) {
            uint32_t a = i < R.nattr ? R.vattr[i] : 0;
            int nf = (int)(a & 0xff), stream = (int)((a >> 8) & 31);
            VkFormat f = nf ? vertex_vkfmt(nf) : VK_FORMAT_UNDEFINED;
            if (nf && !f) { static char msg[64]; snprintf(msg, sizeof msg, "vertex attribute format 0x%x", nf); nvn_vk_missing(msg); }
            if (nf && f) {   /* 2026-10-20 (render bugs V): each vertex format once in the log, with this GPU's support for it */
                static uint8_t seen[256];
                if (!seen[nf & 255]) { VkFormatProperties fp; memset(&fp, 0, sizeof fp); vkGetPhysicalDeviceFormatProperties(V.gpu, f, &fp); seen[nf & 255] = 1;
                    printf("nvn_vk_vfmt: vertex attribute format 0x%x -> VkFormat %d: %s\n", nf, (int)f, (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "a vertex format on this GPU" : "NOT a vertex format on this GPU"); }
            }
            memset(&ad[na], 0, sizeof ad[na]);
            ad[na].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT; ad[na].location = (uint32_t)i;
            if (f && stream < 16 && voff[stream] != ~0ull) {
                if (bmap[stream] == 0xffffffffu) {
                    bmap[stream] = nb;
                    memset(&bd[nb], 0, sizeof bd[nb]);
                    bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = R.vstream[stream].stride;
                    bd[nb].inputRate = R.vstream[stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX; bd[nb].divisor = R.vstream[stream].divisor ? R.vstream[stream].divisor : 1;
                    vb[nb] = vbufs[stream]; vo[nb] = voff[stream]; nb++;
                }
                ad[na].binding = bmap[stream]; ad[na].format = f; ad[na].offset = a >> 14; na++;
            } else if (vs->dep >> i & 1 || 1) {       /* not supplied: (0, 0, 0, 1), as OpenGL's current attribute value; integer inputs take the integer copy */
                ad[na].binding = 16; ad[na].format = (ints >> i & 1) || (ints >> (16 + i) & 1) ? ((sints >> i & 1) ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT) : VK_FORMAT_R32G32B32A32_SFLOAT; ad[na].offset = (ints >> i & 1) || (ints >> (16 + i) & 1) ? 16 : 0;
                need_const = 1; na++;
            }
        }
        if (need_const) {
            memset(&bd[nb], 0, sizeof bd[nb]);
            bd[nb].sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT; bd[nb].binding = nb; bd[nb].stride = 0; bd[nb].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE; bd[nb].divisor = 1;
            for (uint32_t i = 0; i < na; i++) if (ad[i].binding == 16) ad[i].binding = nb;
            vb[nb] = R.zero; vo[nb] = 65536; nb++;
        }
        if (!D.has_vids) {                         /* MAC2 / V_novids: the layout is the pipeline's (vi_key); only the strides are given here */
            VkDeviceSize vst[17];
            for (uint32_t i = 0; i < nb; i++) vst[i] = bd[i].stride;
            if (nb) vkCmdBindVertexBuffers2(R.cmd, 0, nb, vb, vo, NULL, vst);
        } else {
        vkCmdSetVertexInputEXT(R.cmd, nb, bd, na, ad);
        if (nb) vkCmdBindVertexBuffers(R.cmd, 0, nb, vb, vo);
        }
    }
    /* 5. descriptors */
    {
        static VkWriteDescriptorSet w[220];
        static VkDescriptorBufferInfo bi[80];
        static VkDescriptorImageInfo ii[140];
        /* M4: buffers change with every draw: pushed into the command buffer (no set, no pool) when the device can; the
         * textures of a draw repeat: one set per distinct list (layout, views, samplers, image layouts) per command buffer */
        VkDescriptorSet sets[2] = {NULL, NULL};
        int nw = 0, ib = 0, it = 0;
        if (!p->push0 && p->nb[0]) sets[0] = set_alloc(p->dl[0]);
        memset(w, 0, sizeof w[0] * (size_t)(p->nb[0] + p->nb[1]));
        for (int s = 0; s < (fs ? 2 : 1); s++) {
            VStage *v = s ? fs : vs;
            for (unsigned n = 1; n < 34; n++) {
                unsigned bit = n < 18 ? v->cbufs >> n & 1 : v->ssbos >> (n - 18) & 1;
                if (!bit) continue;
                BufRef *b = &bufs[ib];
                bi[ib].buffer = b->zero ? R.zero : b->buf; bi[ib].offset = b->zero ? 0 : b->off; bi[ib].range = b->zero ? 65536 : b->size;
                w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[nw].dstSet = sets[0]; w[nw].dstBinding = 40u * (unsigned)s + (n < 18 ? n : 20 + n - 18); w[nw].descriptorCount = 1;
                w[nw].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[nw].pBufferInfo = &bi[ib]; nw++; ib++;
            }
        }
        if (p->push0) { if (nw) vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, (uint32_t)nw, w); }
        else if (sets[0]) { vkUpdateDescriptorSets(V.device, (uint32_t)nw, w, 0, NULL); vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &sets[0], 0, NULL); }
        if (p->nb[1]) {
            uint64_t key = fnv(0xcbf29ce484222325ULL, &p->dl[1], sizeof p->dl[1]);
            unsigned slot;
            if (p->dl[1] == g_udl[1]) for (int s = 0; s < (fs ? 2 : 1); s++) { VStage *v = s ? fs : vs; for (int i = 0; i < v->nsmp; i++) key = fnv(key, &v->smp[i].binding, sizeof v->smp[i].binding); }      /* session 9: one layout for every pipeline: the binding numbers tell the lists apart */
            for (int i = 0; i < g_ndtex; i++) { key = fnv(key, &g_dtex[i].view, sizeof g_dtex[i].view); key = fnv(key, &g_dtex[i].smp, sizeof g_dtex[i].smp); key = fnv(key, &g_dtex[i].layout, sizeof g_dtex[i].layout); }
            if (!key) key = 1;
            for (slot = (unsigned)key & 4095; g_tset[slot].gen == g_tset_gen && g_tset[slot].key != key; slot = (slot + 1) & 4095) {}
            /* session 4: a hash hit is confirmed by comparing the whole list (layout, views, samplers, image layouts); a
             * different list with the same hash gets a set of its own and is counted */
            int hit = g_tset[slot].gen == g_tset_gen && g_tset[slot].key == key;
            if (hit && (g_tset[slot].dl != p->dl[1] || g_tset[slot].n != g_ndtex || memcmp(&g_tset_list[g_tset[slot].off], g_dtex, sizeof g_dtex[0] * (size_t)g_ndtex))) { hit = 0; R.tset_collisions++; slot = 4096; }
            if (hit) { sets[1] = g_tset[slot].set; R.sets_reused++; }
            else {
                int n1 = 0;
                sets[1] = set_alloc(p->dl[1]);
                for (int s = 0; s < (fs ? 2 : 1); s++) {
                    VStage *v = s ? fs : vs;
                    for (int i = 0; i < v->nsmp; i++) {
                        ii[it].sampler = g_dtex[it].smp; ii[it].imageView = g_dtex[it].view; ii[it].imageLayout = g_dtex[it].layout;
                        memset(&w[n1], 0, sizeof w[n1]);
                        w[n1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n1].dstSet = sets[1]; w[n1].dstBinding = v->smp[i].binding; w[n1].descriptorCount = 1;
                        w[n1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w[n1].pImageInfo = &ii[it]; n1++; it++;
                    }
                }
                vkUpdateDescriptorSets(V.device, (uint32_t)n1, w, 0, NULL);
                if (slot < 4096 && g_tset_used < 3000 && g_tset_list_used + g_ndtex <= (int)(sizeof g_tset_list / sizeof g_tset_list[0])) {
                    g_tset[slot].key = key; g_tset[slot].set = sets[1]; g_tset[slot].gen = g_tset_gen; g_tset[slot].dl = p->dl[1]; g_tset[slot].n = g_ndtex; g_tset[slot].off = g_tset_list_used;
                    memcpy(&g_tset_list[g_tset_list_used], g_dtex, sizeof g_dtex[0] * (size_t)g_ndtex);
                    g_tset_list_used += g_ndtex; g_tset_used++;
                }
            }
            vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 1, 1, &sets[1], 0, NULL);
        }
    }
    {   uint32_t pc[4] = {(uint32_t)R.alpha_func, 0, 0, 0};
        memcpy(&pc[1], &R.alpha_ref, 4); memcpy(&pc[2], &R.point_size, 4);
        if (RS_ON) {                               /* track O: mx_fcs = game pixels per image pixel of this pass (fragment stages translated with screen_scale); points shrink with the image */
            float fcs = R.pass_rs ? 1.0f / R.pass_sx : 1.0f, ps = R.pass_rs ? R.point_size * R.pass_sx : R.point_size;
            memcpy(&pc[3], &fcs, 4); memcpy(&pc[2], &ps, 4);
        }
        vkCmdPushConstants(R.cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    }
    return 1;
}

/* The pass formats of a target set as the pipeline key holds them (also used by the look-ahead of nvn_vk_m4.c on targets
 * that are not bound yet). Returns 0 when one of the images does not exist yet. */
static int pkey_targets_of(PKey *k, int ncol, const TargetRef *col, const TargetRef *depth) {
    int all = 1;
    k->ncolor = (uint8_t)ncol; k->samples = 1;
    for (int i = 0; i < ncol; i++) {
        if (!col[i].tex) continue;
        VTex *vt = col[i].tex->vk;
        int ex, dec;
        if (!vt) { all = 0; continue; }
        VkFormat f = vt->fmt;
        if ((col[i].view.set & 0x84) == 0x84 && col[i].view.format) { VkFormat f2 = tex_vkfmt(col[i].view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; }
        k->cfmt[i] = (int32_t)f; k->samples = (uint8_t)vt->samples;
    }
    if (depth->tex) { VTex *vt = depth->tex->vk; if (!vt) all = 0; else { k->dfmt = (int32_t)vt->fmt; k->samples = (uint8_t)vt->samples; } }
    return all;
}
static void pkey_targets(PKey *k) { pkey_targets_of(k, R.ncol, R.col, &R.depth); }
#if 0   /* session 3's form */
static void pkey_targets(PKey *k) {
    k->ncolor = (uint8_t)R.ncol; k->samples = 1;
    for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) { VTex *vt = R.col[i].tex->vk; int ex, dec; VkFormat f = vt->fmt; if ((R.col[i].view.set & 0x84) == 0x84 && R.col[i].view.format) { VkFormat f2 = tex_vkfmt(R.col[i].view.format, &ex, &dec); if (f2 && !ex && !dec) f = f2; } k->cfmt[i] = (int32_t)f; k->samples = (uint8_t)vt->samples; }
    if (R.depth.tex && R.depth.tex->vk) { VTex *vt = R.depth.tex->vk; k->dfmt = (int32_t)vt->fmt; k->samples = (uint8_t)vt->samples; }
}
#endif
/* ClearDepthStencil with a stencil mask that is not all bits: Vulkan's clears have no mask, so one triangle over the
 * rectangle replaces the masked bits (no colour, no depth). The pass is active. */
static void stencil_clear_masked(uint32_t mask, uint32_t value, int sx, int sy, int sw, int sh) {
    static VStage *vs;
    PKey k;
    if (!vs) {
        vs = calloc(1, sizeof *vs);
        vs->id = ++g_vstage_ids; vs->ok = 1; vs->stage = -1;
        vs->mod = module_from_glsl(0, "#version 460\nvoid main() { gl_Position = vec4(vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2) * 2.0 - 1.0, 0.0, 1.0); }\n", "internal stencil clear", &g_spv_unused);
        if (!vs->mod) vkd_fail("the internal stencil-clear vertex shader does not compile");
        vs->next = g_vstage[0]; g_vstage[0] = vs;
    }
    memset(&k, 0, sizeof k);
    k.vs = vs->id; k.sample_mask = 0xffffffffu; k.topo_class = 2; k.poly = 2; k.logic = 3;
    pkey_targets(&k);
    Pipe *p = pipe_get(&k, vs, NULL);
    VkViewport vp = {0, 0, (float)R.pass_w, (float)R.pass_h, 0, 1};
    VkRect2D sc = {{sx, sy}, {(uint32_t)sw, (uint32_t)sh}};
    float zero4[4] = {0, 0, 0, 0};
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->p);
    if (D.has_eds3) {                              /* session 4: what the key held (no colour written, no blending, fill, every sample) is dynamic state now */
        VkBool32 en[8] = {0};
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8] = {0};
        VkSampleMask all = 0xffffffffu;
        uint32_t n = V.props.limits.maxColorAttachments < 8 ? V.props.limits.maxColorAttachments : 8;
        for (int i = 0; i < 8; i++) { eq[i].srcColorBlendFactor = eq[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; eq[i].dstColorBlendFactor = eq[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; eq[i].colorBlendOp = eq[i].alphaBlendOp = VK_BLEND_OP_ADD; }
        vkCmdSetColorBlendEnableEXT(R.cmd, 0, n, en); vkCmdSetColorBlendEquationEXT(R.cmd, 0, n, eq); vkCmdSetColorWriteMaskEXT(R.cmd, 0, n, wm);
        vkCmdSetPolygonModeEXT(R.cmd, VK_POLYGON_MODE_FILL); vkCmdSetAlphaToCoverageEnableEXT(R.cmd, VK_FALSE); vkCmdSetSampleMaskEXT(R.cmd, (VkSampleCountFlagBits)(k.samples ? k.samples : 1), &all);
        if (D.f2.features.depthClamp) vkCmdSetDepthClampEnableEXT(R.cmd, VK_FALSE);
        R.dyn3_dirty = 1;
    }
    vkCmdSetViewport(R.cmd, 0, 1, &vp); vkCmdSetScissor(R.cmd, 0, 1, &sc); vkCmdSetLineWidth(R.cmd, 1.0f); vkCmdSetDepthBias(R.cmd, 0, 0, 0); vkCmdSetBlendConstants(R.cmd, zero4);
    vkCmdSetStencilCompareMask(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xff); vkCmdSetStencilWriteMask(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, mask); vkCmdSetStencilReference(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, value);
    vkCmdSetCullMode(R.cmd, VK_CULL_MODE_NONE); vkCmdSetFrontFace(R.cmd, VK_FRONT_FACE_COUNTER_CLOCKWISE); vkCmdSetPrimitiveTopology(R.cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    vkCmdSetDepthTestEnable(R.cmd, VK_FALSE); vkCmdSetDepthWriteEnable(R.cmd, VK_FALSE); vkCmdSetDepthCompareOp(R.cmd, VK_COMPARE_OP_ALWAYS); vkCmdSetStencilTestEnable(R.cmd, VK_TRUE);
    vkCmdSetStencilOp(R.cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS);
    vkCmdSetDepthBiasEnable(R.cmd, VK_FALSE); vkCmdSetPrimitiveRestartEnable(R.cmd, VK_FALSE); vkCmdSetRasterizerDiscardEnable(R.cmd, VK_FALSE);
    if (D.has_vids)                                /* without it the pipeline has no vertex input (layout number 0 / an all-zero layout in the key) */
    vkCmdSetVertexInputEXT(R.cmd, 0, NULL, 0, NULL);
    vkCmdDraw(R.cmd, 3, 1, 0, 0);
    R.pipe_bound = NULL; R.dyn_dirty = 1;
}

/* ---------------------------------------------------------------- copies */
/* PR #31. The layout of the texels of a copy between a texture and the game's memory (region r: x, y, z, w, h, d). In the
 * game's memory a row of blocks starts every `pitch` bytes (the copy row stride, uncompressed formats; else rows are tight)
 * and a layer / slice every `ipitch` bytes (the copy image stride, only with a row stride; else tight). Vulkan's copy is
 * always given the texels PACKED TIGHT (bufferRowLength = bufferImageHeight = 0, always valid): when the game's strides are
 * not tight the rows are moved between that and the game's memory on the CPU, one row at a time, in order (a row stride
 * under the row's size makes the rows overlap, as the console's copy engine would presumably write them: not verified).
 * Before, the strides went into the copy (bufferRowLength = row stride / bytes per texel, bufferImageHeight = image stride /
 * row stride) and the buffer was sized from them. A 1024 x 1024 RGBA8 read-back with the strides of an earlier 320 x 360
 * copy still set (row stride 1280, image stride 460800) gave bufferRowLength 320, under the width (invalid in Vulkan), and
 * a buffer of 460800 bytes for a copy of about 4 MB: the read-back wrote past its buffer, and NVIDIA lost the device at the
 * next submission ("VK_ERROR_DEVICE_LOST", 2026-10-15, RTX 4060 Laptop). PR #14's per-command-buffer strides removed that
 * trigger; this removes the overrun for any stride. It also stops a padded read-back from writing the buffer's bytes into
 * the game's memory between the rows and after the last one, and a padded upload from reading whole padded images. */
typedef struct { uint64_t rowb, nrows, pitch, ipitch, size, span; int nz, direct; } CopyLayout;
static void copy_layout(const VTex *vt, const int *r, int64_t row_stride, int64_t image_stride, int nz, CopyLayout *L) {
    L->nz = nz > 0 ? nz : 1;
    L->rowb = ((uint64_t)(r[3] > 0 ? r[3] : 0) + vt->bw - 1) / vt->bw * vt->bpb; L->nrows = ((uint64_t)(r[4] > 0 ? r[4] : 0) + vt->bh - 1) / vt->bh;
    L->pitch = row_stride > 0 && vt->bw == 1 && vt->bpb ? (uint64_t)row_stride / vt->bpb * vt->bpb : L->rowb;      /* as GL_PACK_ROW_LENGTH = stride / bytes per texel */
    L->ipitch = image_stride > 0 && row_stride > 0 && vt->bw == 1 ? (uint64_t)image_stride / (uint64_t)row_stride * L->pitch : L->pitch * L->nrows;
    L->size = L->rowb * L->nrows * (uint64_t)L->nz;      /* the packed texels: what the Vulkan copy reads or writes */
    L->span = L->nrows ? (uint64_t)(L->nz - 1) * L->ipitch + (L->nrows - 1) * L->pitch + L->rowb : 0;      /* the bytes of the game's memory the region covers */
    L->direct = L->pitch == L->rowb && (L->nz == 1 || L->ipitch == L->rowb * L->nrows);
    if (vt->bw == 1 && row_stride > 0 && (L->pitch < L->rowb || (L->nz > 1 && image_stride > 0 && L->ipitch < L->pitch * L->nrows))) {      /* the image stride counts only with more than one slice */
        static int n;
        if (n++ < 10) printf("nvn_vk: a copy of a %d x %d x %d region (block size %d bytes) with row stride %lld and image stride %lld: smaller than the region; packed copy, laid out at those strides (before: an invalid copy that went past its buffer)\n",
                             r[3], r[4], L->nz, vt->bpb, (long long)row_stride, (long long)image_stride);
    } else if (!L->direct && L->size) {
        static int n;
        if (n++ < 3) printf("nvn_vk: a copy of a %d x %d x %d region (block size %d bytes) with padded strides (row %lld, image %lld): the rows are moved on the CPU, the Vulkan copy is packed\n",
                            r[3], r[4], L->nz, vt->bpb, (long long)row_stride, (long long)image_stride);
    }
}
/* CopyBufferToTexture and WriteTexels: texels from memory into a region of one level. The image is not synchronised
 * with the texture's own memory first (the OpenGL path does not either). */
static void write_region(Tex *t, const View *vp, const int *r, const void *data, int64_t row_stride, int64_t image_stride) {
    View v;
    memset(&v, 0, sizeof v);
    if (vp) v = *vp;
    VTex *vt = vt_get2(t, 0);
    if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || vt->decode || vt->expand) { nvn_vk_missing("CopyBufferToTexture / WriteTexels into a depth, ASTC-decoded or 3-component texture"); return; }
    int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0, depth = r[5] > 0 ? r[5] : 1;
    if (t->b.target != 2 && t->b.target != 4 && t->b.target != 9 && t->b.target != 8) layer = 0; else if (t->b.target != 2) layer += r[2];
    /* the texels go into the stream ring packed tight (see copy_layout); only the slices the copy takes (one layer) */
    CopyLayout L;
    copy_layout(vt, r, row_stride, image_stride, vt->is3d ? depth : 1, &L);
    if (!L.size) { t->gpu_written = 1; t->uploaded = 1; return; }      /* an empty region: no zero-extent copy is recorded (invalid in Vulkan); the texture's flags as before */
    uint64_t off = ring_raw(L.direct ? data : NULL, L.size);      /* may submit what was recorded so far */
    if (!L.direct) {                               /* strides not tight: the rows are packed straight into the ring (no temporary) */
        uint8_t *dst = (uint8_t *)R.ring_mem.map + off;
        for (int z = 0; z < L.nz; z++) for (uint64_t y = 0; y < L.nrows; y++) memcpy(dst + (z * L.nrows + y) * L.rowb, (const uint8_t *)data + z * L.ipitch + y * L.pitch, (size_t)L.rowb);
    }
    pass_end(); cmd_begin();
    if (vt->rs) { rs_upload(vt, level, layer, off, 0, 0, r[0], r[1], r[3], r[4]); t->gpu_written = 1; t->uploaded = 1; return; }      /* track O (packed data: tight) */
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)layer; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageOffset.z = vt->is3d ? r[2] : 0;
    c.imageExtent.width = (uint32_t)r[3]; c.imageExtent.height = (uint32_t)r[4]; c.imageExtent.depth = (uint32_t)(vt->is3d ? depth : 1);
    CKPT_OP("copy into a texture (image, x, y, w)", vt->img, c.imageOffset.x, c.imageOffset.y, c.imageExtent.width);
    vkCmdCopyBufferToImage(R.cmd, R.ring, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    t->gpu_written = 1; t->uploaded = 1;
}
static void copy_buf_tex(const Cmd *m) {
    Tex *t = P(m->a[1]);
    View v;
    int r[6];
    if (!t || t->dead || !m->a[0]) return;
    memcpy(&v, BLOB(m), sizeof v); memcpy(r, BLOB(m) + 24, 24);
    write_region(t, &v, r, P(m->a[0]), (int64_t)m->a[5], (int64_t)m->a[6]);     /* PR #14 (xtlg511): the strides of its own command buffer (nvn_gl.c cmd_copy_buf_tex) */
}
static void vk_write_texels(void *arg) {
    WriteJob *j = arg;
    if (V.alive && j->tex && !j->tex->dead) write_region(j->tex, j->has_view ? &j->view : NULL, j->region, j->data, j->row_stride, 0);
}
/* Session 5. CopyTextureToBuffer, ReadTexels, the movie maker's present: a region of one level to host memory, as the
 * OpenGL path's glGetTextureSubImage (tight rows, or the given row / image strides). Synchronous: the command buffer is
 * submitted and waited for (these calls are rare: album, replays, screenshots). Returns the bytes written, 0 when it stopped. */
static size_t read_region(Tex *t, const View *vp, const int *r, void *out, int64_t row_stride, int64_t image_stride) {
    VTex *vt = vt_get(t);
    if (vt->decode || vt->expand || vt->samples > 1) { nvn_vk_missing("ReadTexels / CopyTextureToBuffer of an ASTC-decoded, 3-component or multisample texture"); return 0; }
    if (vt->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) { nvn_vk_missing("ReadTexels / CopyTextureToBuffer of a combined depth-stencil texture"); return 0; }
    int level = vp && (vp->set & 0x81) == 0x81 ? vp->lvl0 : 0, layer = vp && (vp->set & 0x82) == 0x82 ? vp->lay0 : 0, depth = r[5] > 0 ? r[5] : 1;
    CopyLayout L;                                  /* PR #31: the copy lands packed; its rows are laid out at the game's strides below */
    copy_layout(vt, r, row_stride, image_stride, depth, &L);
    uint64_t size = L.size;
    if (!size) return 0;
    VMem mem;
    VkBuffer buf = vbuf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &mem);
    VkBufferImageCopy c;
    pass_end(); cmd_begin();
    VkImageLayout was = vt->layout;
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = vt->aspect; c.imageSubresource.mipLevel = (uint32_t)level; c.imageSubresource.baseArrayLayer = vt->is3d ? 0 : (uint32_t)(layer + r[2]); c.imageSubresource.layerCount = vt->is3d ? 1 : (uint32_t)depth;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageOffset.z = vt->is3d ? r[2] : 0;
    c.imageExtent.width = (uint32_t)r[3]; c.imageExtent.height = (uint32_t)r[4]; c.imageExtent.depth = (uint32_t)(vt->is3d ? depth : 1);
    if (vt->rs) {                                  /* track O: the game reads ITS rectangle: the image's part of it is blitted up to a temporary image of that size */
        int lw = lvl_(vt->w, level), lh = lvl_(vt->h, level);
        VkImageBlit b;
        if (r[3] <= 0 || r[4] <= 0 || !fmt_supported(vt->fmt, VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
            nvn_vk_missing("resolution scale: ReadTexels / CopyTextureToBuffer of a scaled texture whose format cannot be blitted");
            if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
            vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
            return 0;
        }
        VTex *tmp = rs_temp(vt, r[3], r[4]);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = vt->aspect; b.srcSubresource.mipLevel = (uint32_t)level; b.srcSubresource.baseArrayLayer = (uint32_t)(layer + r[2]); b.srcSubresource.layerCount = 1;
        b.dstSubresource.aspectMask = vt->aspect; b.dstSubresource.layerCount = 1;
        b.srcOffsets[0].x = rs_edge(r[0], vt->sx, lw); b.srcOffsets[0].y = rs_edge(r[1], vt->sy, lh); b.srcOffsets[1].x = rs_edge(r[0] + r[3], vt->sx, lw); b.srcOffsets[1].y = rs_edge(r[1] + r[4], vt->sy, lh); b.srcOffsets[1].z = 1;
        if (b.srcOffsets[1].x <= b.srcOffsets[0].x) b.srcOffsets[1].x = b.srcOffsets[0].x + 1; if (b.srcOffsets[1].y <= b.srcOffsets[0].y) b.srcOffsets[1].y = b.srcOffsets[0].y + 1;
        if (b.srcOffsets[1].x > lw) { b.srcOffsets[0].x = lw - 1; b.srcOffsets[1].x = lw; } if (b.srcOffsets[1].y > lh) { b.srcOffsets[0].y = lh - 1; b.srcOffsets[1].y = lh; }
        b.dstOffsets[1].x = r[3]; b.dstOffsets[1].y = r[4]; b.dstOffsets[1].z = 1;
        vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, rs_filter(vt));
        vt_layout(tmp, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        c.imageSubresource.mipLevel = 0; c.imageSubresource.baseArrayLayer = 0; c.imageSubresource.layerCount = 1; c.imageOffset.x = c.imageOffset.y = c.imageOffset.z = 0;
        vkCmdCopyImageToBuffer(R.cmd, tmp->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        g_rs_down++;
    } else {
        CKPT_OP("read back of a texture (image, w, h, layers)", vt->img, c.imageExtent.width, c.imageExtent.height, depth);
        vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    }
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
    cmd_wait_all();
    if (L.direct) memcpy(out, mem.map, (size_t)size);
    else for (int z = 0; z < L.nz; z++) for (uint64_t y = 0; y < L.nrows; y++) memcpy((uint8_t *)out + z * L.ipitch + y * L.pitch, (const uint8_t *)mem.map + (z * L.nrows + y) * L.rowb, (size_t)L.rowb);      /* only the texel rows: the gaps between them stay the game's */
    vkDestroyBuffer(V.device, buf, NULL); vmem_free(mem);
    R.readbacks++;
    return (size_t)L.span;                         /* the bytes of the game's memory written (callers test it for 0 only) */
}
static void vk_read_texels(void *arg) {
    ReadJob *j = arg;
    if (!V.alive || !j->tex || j->tex->dead) return;
    if (j->to_pool) { nvn_vk_missing("nvnTextureInvalidateTexels (rendered texels written back into the texture's pool memory)"); return; }
    CB_WHY(CBW_TEX, 0, 0); cb_sync();
    read_region(j->tex, j->has_view ? &j->view : NULL, j->region, j->out, j->row_stride, j->image_stride);
}
int album_movie_push_frame(const uint8_t *rgba, int w, int h, int stride, int bottom_up);      /* nnalbum.c, as nvn_gl.c job_present_movie */
static void vk_present_movie(void *arg) {
    PresentJob *j = arg;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    if (!V.alive || !t || t->dead || t->b.width <= 0 || t->b.height <= 0) return;
    VTex *vt = vt_get(t);
    int w = vt->rs ? t->b.width : vt->w, h = vt->rs ? t->b.height : vt->h, r[6] = {0, 0, 0, w, h, 1}, bgr = vt->fmt == VK_FORMAT_B8G8R8A8_UNORM || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB;      /* track O: read_region takes the game's rectangle */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { nvn_vk_missing("nvnQueuePresentTexture to the movie maker's window with a window texture that is not 8-bit RGBA"); return; }
    uint8_t *px = malloc((size_t)w * h * 4);
    if (!px) return;
    if (read_region(t, NULL, r, px, 0, 0)) {
        if (bgr) for (size_t i = 0; i < (size_t)w * h; i++) { uint8_t b = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = b; }
        album_movie_push_frame(px, w, h, w * 4, 0);
    }
    free(px);
}
static void blit_tex(Tex *s, const View *sv, const int *sr, Tex *d, const View *dv, const int *dr, int linear) {
    VTex *vs = vt_get(s), *vd = vt_get(d);
    VkImageBlit b;
    d->gpu_written = 1;
    if (vs == vd) { nvn_vk_missing("CopyTextureToTexture within one texture"); return; }
    pass_end(); cmd_begin();
    vt_layout(vs, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(vd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = vs->aspect; b.srcSubresource.mipLevel = sv && (sv->set & 0x81) == 0x81 ? (uint32_t)sv->lvl0 : 0; b.srcSubresource.baseArrayLayer = sv && (sv->set & 0x82) == 0x82 && !vs->is3d ? (uint32_t)sv->lay0 : 0; b.srcSubresource.layerCount = 1;
    b.dstSubresource.aspectMask = vd->aspect; b.dstSubresource.mipLevel = dv && (dv->set & 0x81) == 0x81 ? (uint32_t)dv->lvl0 : 0; b.dstSubresource.baseArrayLayer = dv && (dv->set & 0x82) == 0x82 && !vd->is3d ? (uint32_t)dv->lay0 : 0; b.dstSubresource.layerCount = 1;
    b.srcOffsets[0].x = sr[0]; b.srcOffsets[0].y = sr[1]; b.srcOffsets[1].x = sr[0] + sr[3]; b.srcOffsets[1].y = sr[1] + sr[4]; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = dr[0]; b.dstOffsets[0].y = dr[1]; b.dstOffsets[1].x = dr[0] + dr[3]; b.dstOffsets[1].y = dr[1] + dr[4]; b.dstOffsets[1].z = 1;
    int rs_sw = sr[3], rs_sh = sr[4], rs_dw = dr[3], rs_dh = dr[4];
    if (vs->rs || vd->rs) {                        /* track O: the game's rectangles on the scaled images */
        if (vs->rs) {
            int lw = lvl_(vs->w, (int)b.srcSubresource.mipLevel), lh = lvl_(vs->h, (int)b.srcSubresource.mipLevel);
            b.srcOffsets[0].x = rs_edge(sr[0], vs->sx, lw); b.srcOffsets[0].y = rs_edge(sr[1], vs->sy, lh); b.srcOffsets[1].x = rs_edge(sr[0] + sr[3], vs->sx, lw); b.srcOffsets[1].y = rs_edge(sr[1] + sr[4], vs->sy, lh);
            rs_sw = b.srcOffsets[1].x - b.srcOffsets[0].x; rs_sh = b.srcOffsets[1].y - b.srcOffsets[0].y;
        }
        if (vd->rs) {
            int lw = lvl_(vd->w, (int)b.dstSubresource.mipLevel), lh = lvl_(vd->h, (int)b.dstSubresource.mipLevel);
            b.dstOffsets[0].x = rs_edge(dr[0], vd->sx, lw); b.dstOffsets[0].y = rs_edge(dr[1], vd->sy, lh); b.dstOffsets[1].x = rs_edge(dr[0] + dr[3], vd->sx, lw); b.dstOffsets[1].y = rs_edge(dr[1] + dr[4], vd->sy, lh);
            rs_dw = b.dstOffsets[1].x - b.dstOffsets[0].x; rs_dh = b.dstOffsets[1].y - b.dstOffsets[0].y;
        }
        if (!rs_sw || !rs_sh || !rs_dw || !rs_dh) return;
        g_rs_blits++;
    }
    if (vs->aspect != vd->aspect) { nvn_vk_missing("CopyTextureToTexture between a colour and a depth format"); return; }
    if (vs->samples > 1 && vd->samples == 1 && vs->aspect == VK_IMAGE_ASPECT_COLOR_BIT && rs_sw == rs_dw && rs_sh == rs_dh) {
        VkImageResolve rv;
        memset(&rv, 0, sizeof rv);
        rv.srcSubresource = b.srcSubresource; rv.dstSubresource = b.dstSubresource; rv.srcOffset = b.srcOffsets[0]; rv.dstOffset = b.dstOffsets[0];
        rv.extent.width = (uint32_t)rs_sw; rv.extent.height = (uint32_t)rs_sh; rv.extent.depth = 1;
        CKPT_OP("resolve (destination image, w, h)", vd->img, rv.extent.width, rv.extent.height, 0);
        vkCmdResolveImage(R.cmd, vs->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vd->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rv);
        return;
    }
    if (vs->samples > 1 || vd->samples > 1) { nvn_vk_missing("a scaled or depth copy of a multisample texture"); return; }
    /* glBlitFramebuffer with FRAMEBUFFER_SRGB off copies the stored values; vkCmdBlitImage converts between an sRGB and a
     * linear format. Same class on both sides = the same values. */
    CKPT_OP("blit (destination image, x1, y1)", vd->img, b.dstOffsets[1].x, b.dstOffsets[1].y, 0);
    vkCmdBlitImage(R.cmd, vs->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vd->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, linear && vs->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

/* ---------------------------------------------------------------- replay */
/* Round 17, TEST ONLY (nvn_dt.h, SSBU_DRAWTAG=1 with SSBU_SHOT_FIGHT): the draws of the tagged game frame. "DT vk" = the
 * same line the OpenGL back end prints (command data, hashes of guest memory); "DTV" = what THIS back end bound: the hash
 * of the bytes in the Vulkan buffer at the bound offset when the draw was recorded (ring = the stream ring's copy; cb = a
 * compute buffer, whose content at that moment is not what the GPU will read), and the state values set for the draw. */
#include "nvn_dt.h"
int nvn_front_draw_tag(void); int nvn_front_draw_tag_next(void); void nvn_front_draw_tag_clear(void);      /* nvn_gl.c */
static uint64_t g_dt_voff[16];
static VkBuffer g_dt_vb[16];
static void *dt_host(uint64_t a) { return P(a); }
static const uint8_t *dt_map(VkBuffer b) { if (b == R.ring) return R.ring_mem.map; for (int i = 0; i < 256; i++) if (g_cb[i].state && g_cb[i].buf == b) return g_cb[i].mem.map; return NULL; }
static void dt_vk(int fight, int idx, int ok, const Cmd *m, int indexed, const BufRef *ib) {
    const uint64_t *a = m->a;
    unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1;
    if (indexed) dt_line("vk", fight, idx, ok, (unsigned)a[0], (int)t, (int)a[2], a[3], (long long)(int32_t)a[4], (unsigned)a[5], (unsigned)a[6], R.prog, R.nattr, R.vattr, R.vbuf, (const uint32_t *)R.vstream, R.ubo, R.ssbo, dt_host);
    else dt_line("vk", fight, idx, ok, (unsigned)a[0], -1, (int)a[2], 0, (long long)a[1], (unsigned)a[3], (unsigned)a[4], R.prog, R.nattr, R.vattr, R.vbuf, (const uint32_t *)R.vstream, R.ubo, R.ssbo, dt_host);
    printf("DTV %d %d", fight, idx);
    if (indexed && ib) {
        const uint8_t *map = dt_map(ib->buf);
        printf(" ib_bound=%s@%llu:%016llx", ib->buf == R.ring ? "ring" : "cb", (unsigned long long)ib->off, (unsigned long long)(map ? dt_fnv(map + ib->off, (size_t)ib->size) : 0));
    }
    if (ok) for (int i = 0; i < 16; i++) {
        if (g_dt_voff[i] == ~0ull) continue;
        const uint8_t *map = dt_map(g_dt_vb[i]);
        printf(" s%d_bound=%s@%llu:%016llx", i, g_dt_vb[i] == R.ring ? "ring" : "cb", (unsigned long long)g_dt_voff[i], (unsigned long long)(map ? dt_fnv(map + g_dt_voff[i], (size_t)R.vbuf[i].size) : 0));
    }
    printf(" | vkfmt");
    for (int i = 0; i < 16 && i < R.nattr; i++) if (R.vattr[i] & 0xff) {
        VkFormat f = vertex_vkfmt((int)(R.vattr[i] & 0xff));
        VkFormatProperties fp; memset(&fp, 0, sizeof fp);
        if (f) vkGetPhysicalDeviceFormatProperties(V.gpu, f, &fp);
        printf(" %d:%d%s", i, (int)f, (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? "" : "(NOT a vertex format on this GPU)");
    }
    printf(" | ds=%llx poly=%u,%u,%u,%u polyoff=%g,%g,%g viewport=%g,%g,%g,%g(%d) scissor=%d,%d,%d,%d(%d) drange=%g,%g dclamp=%d restart=%d discard=%d pass=%dx%d ncol=%d depth=%d chanmask=%x blend_on=%x ring_gen=%u\n",
           (unsigned long long)R.ds, R.poly[0], R.poly[1], R.poly[2], R.poly[3], R.polyoff[0], R.polyoff[1], R.polyoff[2], R.viewport[0], R.viewport[1], R.viewport[2], R.viewport[3], R.have_viewport,
           R.scissor[0], R.scissor[1], R.scissor[2], R.scissor[3], R.have_scissor, R.drange[0], R.drange[1], R.depth_clamp, R.restart, R.rast_discard, R.pass_w, R.pass_h, R.ncol, R.depth.tex != NULL, R.chanmask, R.blend_on, R.gen);
}
static void replay(uint64_t handle, int depth);
static void exec(const Cmd *m, int depth) {
    const uint8_t *blob = BLOB(m);
    const uint64_t *a = m->a;
    switch (m->op) {
    case OP_NOP: case OP_DEBUG: case OP_DISCARD: case OP_CONSDILATE: case OP_SUBPIXEL: break;
    case OP_TEXPOOL: R.tpool = P(a[0]); break;
    case OP_SMPPOOL: R.spool = P(a[0]); break;
    case OP_PROGRAM:
        for (int s = 0; s < 6; s++) if (a[1] & (1u << s)) { Prog *p = P(a[0]); R.prog[s] = p && !p->dead && p->st[s].state ? p : NULL; }
        break;
    case OP_UBO: case OP_SSBO: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2], max = m->op == OP_UBO ? 16 : 32;
        Range *dst = m->op == OP_UBO ? R.ubo[stage] : R.ssbo[stage];
        for (int i = 0; i < count && first + i < max; i++) memcpy(&dst[first + i], blob + i * 16, 16);
        break;
    }
    case OP_TEXTURES: {
        int stage = (int)a[0] % 6, first = (int)a[1], count = (int)a[2];
        for (int i = 0; i < count && first + i < 32; i++) {
            uint64_t h;
            unsigned u = (unsigned)(first + i);
            uint32_t dims[4] = {0, 0, 0, 0};
            int isint = 0;
            Tex *t;
            View v;
            SamplerParams sp;
            memcpy(&h, blob + i * 8, 8);
            R.nvn_tex[stage][u] = h;
            if (nvn_front_handle(R.tpool, R.spool, h, &t, &v, &sp) & 1) {      /* what TXQ reports, and whether the shader must use an integer sampler (resolve_handle's rules) */
                int glevels = nvn_front_tex_levels(t), l0 = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, nl = (v.set & 0x81) == 0x81 && v.nlvl > 0 ? v.nlvl : glevels - l0, is3d = t->b.target == 2;
                int layers = (v.set & 0x82) == 0x82 && v.nlay > 0 ? v.nlay : nvn_front_tex_layers(t), nf = (v.set & 0x84) == 0x84 && v.format ? v.format : t->b.format, k = nf >= 0x6D ? nf - 0x6D : (nf - 1) % 12;
                dims[0] = (uint32_t)lvl_(t->b.width, l0); dims[1] = (uint32_t)lvl_(t->b.height, l0);
                dims[2] = is3d ? (uint32_t)lvl_(t->b.depth, l0) : (uint32_t)(t->b.target == 9 ? layers / 6 : layers);
                dims[3] = (uint32_t)(nl > 0 ? nl : 1) | ((uint32_t)is3d << 16);
                if (((nf >= 1 && nf <= 0x30) || (nf >= 0x6D && nf <= 0x78)) && (k == 2 || k == 3 || k == 7 || k == 8 || k == 10 || k == 11)) isint = k == 3 || k == 8 || k == 11 ? 2 : 1;
                if (nf == 0x3E) isint = 1;
            }
            memcpy(&R.c2[stage][48 + 2 * u], dims, 16);
            R.tex_int[stage] = (R.tex_int[stage] & ~(1u << u)) | ((uint32_t)(isint != 0) << u);
            R.tex_sint[stage] = (R.tex_sint[stage] & ~(1u << u)) | ((uint32_t)(isint == 2) << u);
        }
        break;
    }
    case OP_VBUF: for (int i = 0; i < (int)a[1] && (int)a[0] + i < 32; i++) memcpy(&R.vbuf[a[0] + i], blob + i * 16, 16); break;
    case OP_VATTR: R.nattr = (int)a[0] < 32 ? (int)a[0] : 32; memcpy(R.vattr, blob, (size_t)R.nattr * 4); break;
    case OP_VSTREAM: for (int i = 0; i < (int)a[0] && i < 32; i++) memcpy(&R.vstream[i], blob + i * 8, 8); break;
    case OP_BLEND: { const uint8_t *b = (const uint8_t *)&a[0]; if (b[7]) nvn_vk_missing("an advanced blend equation (nvnBlendStateSetAdvancedMode)"); memcpy(R.blend[b[0] & 7], b + 1, 6); R.dyn3_dirty = 1; break; }
    case OP_COLOR: { const uint8_t *b = (const uint8_t *)&a[0]; R.blend_on = b[0]; R.logic = b[1]; R.alpha_func = b[2]; R.dyn3_dirty = 1; if (b[1] != 3 && !D.f2.features.logicOp) nvn_vk_missing("a logic op on a GPU without logicOp"); break; }
    case OP_CHANMASK: R.chanmask = (uint32_t)a[0]; R.dyn3_dirty = 1; break;
    case OP_MULTISAMPLE: { const uint8_t *b = (const uint8_t *)&a[0]; memcpy(R.ms, b, 24); R.dyn3_dirty = 1; if (b[3] || b[5]) nvn_vk_missing("a multisample state with coverage-to-colour, coverage modulation, sample locations or raster samples"); break; }
    case OP_POLYGON: memcpy(R.poly, &a[0], 4); R.dyn_dirty = 1; R.dyn3_dirty = 1; break;
    case OP_DEPTHSTENCIL: R.ds = a[0]; R.dyn_dirty = 1; break;
    case OP_VIEWPORTS: if (a[0] == 0 && a[1] > 0) { memcpy(R.viewport, blob, 16); R.have_viewport = 1; R.dyn_dirty = 1; } break;
    case OP_SCISSORS: if (a[0] == 0 && a[1] > 0) { memcpy(R.scissor, blob, 16); if (R.scissor[2] < 0) R.scissor[2] = 0; if (R.scissor[3] < 0) R.scissor[3] = 0; R.have_scissor = 1; R.dyn_dirty = 1; } break;
    case OP_DEPTHRANGES: if (a[0] == 0 && a[1] > 0) { memcpy(R.drange, blob, 8); R.dyn_dirty = 1; } break;
    case OP_TARGETS: {
        const TargetRef *col = (const TargetRef *)blob, *dep = col + 8;
        int n = (int)a[0] < 8 ? (int)a[0] : 8;
        pass_end();
        for (int i = 0; i < R.ncol; i++) if (R.col[i].tex && R.col[i].tex->vk) ((VTex *)R.col[i].tex->vk)->feedback = 0;
        if (R.depth.tex && R.depth.tex->vk) { ((VTex *)R.depth.tex->vk)->feedback = 0; ((VTex *)R.depth.tex->vk)->shadow_ok = 0; }
        memset(R.col, 0, sizeof R.col); memset(&R.depth, 0, sizeof R.depth);
        R.ncol = 0; R.have_targets = 0;
        for (int i = 0; i < n; i++) if (col[i].tex && !col[i].tex->dead) { R.col[i] = col[i]; R.ncol = i + 1; R.have_targets = 1; }
        if (dep->tex && !dep->tex->dead) { R.depth = *dep; R.have_targets = 1; }
        R.dyn_dirty = 1;                           /* default viewport / scissor follow the target size */
        break;
    }
    case OP_STENCIL_REF: case OP_STENCIL_VMASK: case OP_STENCIL_MASK: {
        int *dst = m->op == OP_STENCIL_REF ? R.sref : m->op == OP_STENCIL_VMASK ? R.svmask : R.swmask;
        if (a[0] & 1) dst[0] = (int)a[1];
        if (a[0] & 2) dst[1] = (int)a[1];
        R.dyn_dirty = 1;
        break;
    }
    case OP_BLENDCOLOR: memcpy(R.blendc, blob, 16); R.dyn_dirty = 1; break;
    case OP_ALPHAREF: memcpy(&R.alpha_ref, &a[0], 4); break;
    case OP_POINTSIZE: memcpy(&R.point_size, &a[0], 4); break;
    case OP_LINEWIDTH: memcpy(&R.line_width, &a[0], 4); R.dyn_dirty = 1; break;
    case OP_POLYOFFSET: memcpy(R.polyoff, &a[0], 12); R.dyn_dirty = 1; break;
    case OP_SAMPLEMASK: R.sample_mask = (uint32_t)a[0]; R.dyn3_dirty = 1; break;
    case OP_RASTDISCARD: R.rast_discard = a[0] & 1; R.dyn_dirty = 1; break;
    case OP_DEPTHCLAMP: R.depth_clamp = a[0] & 1; R.dyn3_dirty = 1; if ((a[0] & 1) && !D.f2.features.depthClamp) nvn_vk_missing("depth clamp on a GPU without depthClamp"); break;
    case OP_PRIMRESTART: R.restart = a[0] & 1; break;
    case OP_PATCHSIZE: break;                      /* only read by tessellation, which stops at the draw */
    case OP_IMAGES: break;                         /* only read by compute programs: DispatchCompute stops with its name until M3 */
    case OP_FENCE: if (a[0]) { SyncObj *o = P(a[0]); if (!o->dead) o->s = (void *)(uintptr_t)(cmd_submit(NULL, NULL) + 1); } break;
    case OP_WAITSYNC: break;                       /* one queue: what was submitted before has run before */
    case OP_CONSRASTER: if (a[0] & 1) nvn_vk_missing("conservative rasterisation (nvnCommandBufferSetConservativeRasterEnable)"); break;
    case OP_DEPTHBOUNDS: if (a[0] & 1) nvn_vk_missing("the depth bounds test (nvnCommandBufferSetDepthBounds)"); break;
    case OP_RENDER_ENABLE: if (!a[1]) R.render_off = !(a[0] & 1); else nvn_vk_missing("conditional rendering (nvnCommandBufferSetRenderEnableConditional)"); break;
    case OP_ROWSTRIDE: R.row_stride = (int64_t)a[0]; break;      /* PR #14 (xtlg511): no longer read by the copies (they carry their own strides); kept as state */
    case OP_IMAGESTRIDE: R.image_stride = (int64_t)a[0]; break;
    case OP_BARRIER: if (a[0] & 0x10) nvn_front_barrier_texmem(); break;      /* ordering bits: every draw is recorded in order; feedback is handled at the draw */
    case OP_DISPATCH: { LONG64 t0 = tk(); compute_dispatch((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]); g_tm[2] += tk() - t0; break; }
    case OP_COPY_BUF_BUF: CB_WHY(CBW_GCMD, a[0], a[2]); cb_sync(); if (g_cb_nkept) { cb_kept_range(a[0], a[2], 3); cb_kept_range(a[1], a[2], 3); } if (a[0] && a[1]) memmove(P(a[1]), P(a[0]), (size_t)a[2]); R.gen++; break;      /* guest memory is read / written: what compute wrote must be there (as OpenGL's compute_flush) */
    case OP_UPDATE_UBO: CB_WHY(CBW_GCMD, a[0] + a[2], a[3]); cb_sync(); if (g_cb_nkept) cb_kept_range(a[0] + a[2], a[3], 3); if (a[0]) memcpy((uint8_t *)P(a[0]) + a[2], blob, (size_t)a[3]); R.gen++; break;
    case OP_CLEAR_BUF: CB_WHY(CBW_GCMD, a[0], a[1]); cb_sync(); if (g_cb_nkept) cb_kept_range(a[0], a[1], 3); if (a[0]) { uint32_t v = (uint32_t)a[2]; for (uint64_t i = 0; i + 4 <= a[1]; i += 4) memcpy((uint8_t *)P(a[0]) + i, &v, 4); } R.gen++; break;
    case OP_CLEARCOLOR: {
        unsigned i = (unsigned)a[0] & 7, mask = (unsigned)a[2] & 15;
        if (!R.have_targets || R.render_off || (int)i >= R.ncol || !R.col[i].tex) break;
        if (!pass_begin()) break;
        if (mask != 15) {                          /* a partial channel mask only matters for channels the format has */
            Fmt f; int comps = 4, nf = R.col[i].tex->b.format;
            if (nf >= 1 && nf <= 0x30) comps = (nf - 1) / 12 + 1;
            if (!nvn_front_fmt(nf, &f) || (mask & ((1u << comps) - 1)) != (1u << comps) - 1) { if (mask) nvn_vk_missing("ClearColor with a partial channel mask"); break; }
        }
        VkClearAttachment ca;
        VkClearRect cr;
        int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
        if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
        if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
        if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
        if (sw <= 0 || sh <= 0) break;
        memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ca.colorAttachment = i; memcpy(&ca.clearValue.color, blob, 16);
        cr.rect.offset.x = sx; cr.rect.offset.y = sy; cr.rect.extent.width = (uint32_t)sw; cr.rect.extent.height = (uint32_t)sh; cr.layerCount = 1;
        CKPT_OP("clear of an attachment (aspect, w, h, colour attachment)", ca.aspectMask, cr.rect.extent.width, cr.rect.extent.height, ca.colorAttachment);
        vkCmdClearAttachments(R.cmd, 1, &ca, 1, &cr);
        R.frame_clears++; nvn_front_count(2, 1);
        break;
    }
    case OP_CLEARDS: {
        if (!R.have_targets || R.render_off || !R.depth.tex || !((a[0] & 1) || (uint32_t)a[2])) break;
        if (!pass_begin()) break;
        VTex *vt = R.depth.tex->vk;
        VkClearAttachment ca;
        VkClearRect cr;
        int sx = R.have_scissor ? R.scissor[0] : 0, sy = R.have_scissor ? R.scissor[1] : 0, sw = R.have_scissor ? R.scissor[2] : R.pass_w, sh = R.have_scissor ? R.scissor[3] : R.pass_h;
        if (R.pass_rs && R.have_scissor) rs_rect(&sx, &sy, &sw, &sh);
        if (sx < 0) { sw += sx; sx = 0; } if (sy < 0) { sh += sy; sy = 0; }
        if (sw > R.pass_w - sx) sw = R.pass_w - sx; if (sh > R.pass_h - sy) sh = R.pass_h - sy;
        if (sw <= 0 || sh <= 0 || !vt) break;
        memset(&ca, 0, sizeof ca); memset(&cr, 0, sizeof cr);
        ca.aspectMask = ((a[0] & 1) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((uint32_t)a[2] ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if ((ca.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) && (vt->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && ((uint32_t)a[2] & 0xff) != 0xff) {
            stencil_clear_masked((uint32_t)a[2] & 0xff, (uint32_t)a[1], sx, sy, sw, sh);
            ca.aspectMask &= ~(VkFlags)VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        ca.aspectMask &= vt->aspect;
        if (!ca.aspectMask) break;
        memcpy(&ca.clearValue.depthStencil.depth, &a[3], 4); ca.clearValue.depthStencil.stencil = (uint32_t)a[1];
        cr.rect.offset.x = sx; cr.rect.offset.y = sy; cr.rect.extent.width = (uint32_t)sw; cr.rect.extent.height = (uint32_t)sh; cr.layerCount = 1;
        CKPT_OP("clear of an attachment (aspect, w, h, colour attachment)", ca.aspectMask, cr.rect.extent.width, cr.rect.extent.height, ca.colorAttachment);
        vkCmdClearAttachments(R.cmd, 1, &ca, 1, &cr);
        R.frame_clears++; nvn_front_count(2, 1);
        break;
    }
    case OP_DRAW_ARRAYS:
        { LONG64 t0 = tk(); int ok = draw_prepare((unsigned)a[0]); g_tm[8] += tk() - t0; { int dtag = nvn_front_draw_tag(); if (dtag) dt_vk(dtag, nvn_front_draw_tag_next(), ok, m, 0, NULL); } if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); break; } }
        if (D.has_ckpt) ckpt_mark(0, (unsigned)a[0], (uint32_t)a[2], (uint32_t)a[4], (uint32_t)a[1], 0, 0);
        vkCmdDraw(R.cmd, (uint32_t)a[2], (uint32_t)a[4], (uint32_t)a[1], (uint32_t)a[3]);
        R.frame_draws++; g_cmd_draws++; nvn_front_count(0, 1);
        break;
    case OP_DRAW_ELEMENTS: {
        unsigned t = (unsigned)a[1] < 3 ? (unsigned)a[1] : 1;
        int count = (int)a[2];
        if (count <= 0 || !a[3]) { int dtag = nvn_front_draw_tag(); if (dtag) dt_vk(dtag, nvn_front_draw_tag_next(), 0, m, 0, NULL); R.frame_skipped++; nvn_front_count(1, 1); break; }
        if (t == 0 && !D.has_iu8) { nvn_vk_missing("8-bit indices on a GPU without VK_EXT_index_type_uint8"); break; }
        BufRef ib;
        buf_ref(a[3], (uint64_t)count << t, (uint64_t)1 << t, &ib);    /* before the draw is prepared: a ring wrap submits */
        { LONG64 t0 = tk(); int ok = draw_prepare((unsigned)a[0]); g_tm[8] += tk() - t0; { int dtag = nvn_front_draw_tag(); if (dtag) dt_vk(dtag, nvn_front_draw_tag_next(), ok, m, 1, &ib); } if (!ok) { R.frame_skipped++; nvn_front_count(1, 1); break; } }
        vkCmdBindIndexBuffer(R.cmd, ib.buf, ib.off, t == 0 ? VK_INDEX_TYPE_UINT8_EXT : t == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        if (D.has_ckpt) ckpt_mark(1, (unsigned)a[0], (uint32_t)count, (uint32_t)a[6], 0, (int32_t)a[4], t);
        vkCmdDrawIndexed(R.cmd, (uint32_t)count, (uint32_t)a[6], 0, (int32_t)a[4], (uint32_t)a[5]);
        R.frame_draws++; g_cmd_draws++; nvn_front_count(0, 1);
        break;
    }
    case OP_COPY_BUF_TEX: CB_WHY(CBW_TEX, 0, 0); cb_sync(); if (g_cb_nkept) cb_kept_all(3); copy_buf_tex(m); break;
    case OP_COPY_TEX_BUF: {                        /* a0 texture, a3 buffer address, a5 / a6 the strides of its command buffer; blob: view, region (as nvn_gl.c) */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        if (!t || t->dead || !a[3]) break;
        memcpy(&v, blob, sizeof v); memcpy(r, blob + 24, 24);
        CB_WHY(CBW_TEX, a[3], 0); cb_sync();
        if (g_cb_nkept) cb_kept_all(3);
        read_region(t, &v, r, P(a[3]), (int64_t)a[5], (int64_t)a[6]);     /* PR #14 (xtlg511): the strides of its own command buffer */
        R.gen++;                                  /* guest memory changed: ranges are streamed again */
        break;
    }
    case OP_COPY_TEX_TEX: {
        Tex *s = P(a[0]), *d = P(a[3]);
        View sv, dv;
        int sr[6], dr[6];
        if (!s || !d || s->dead || d->dead) break;
        memcpy(&sv, blob, sizeof sv); memcpy(sr, blob + 24, 24); memcpy(&dv, blob + 48, sizeof dv); memcpy(dr, blob + 72, 24);
        if (a[6] & 4) { dr[0] += dr[3]; dr[3] = -dr[3]; }
        if (a[6] & 8) { dr[1] += dr[4]; dr[4] = -dr[4]; }
        blit_tex(s, &sv, sr, d, &dv, dr, (int)(a[6] & 1));
        break;
    }
    case OP_DOWNSAMPLE: {
        Tex *s = P(a[0]), *d = P(a[1]);
        if (!s || !d || s->dead || d->dead) break;
        int sr[6] = {0, 0, 0, s->b.width, s->b.height, 1}, dr[6] = {0, 0, 0, d->b.width, d->b.height, 1};
        blit_tex(s, NULL, sr, d, NULL, dr, 0);
        break;
    }
    case OP_CLEARTEX: {                            /* a0 texture, a4 channel mask, a5 0 float / 1 int / 2 uint, a6 whole level; blob: view, region, colour (as nvn_gl.c) */
        Tex *t = P(a[0]);
        View v;
        int r[6];
        Fmt f;
        if (!t || t->dead) break;
        memcpy(&v, blob, sizeof v); memcpy(r, blob + 24, 24);
        int level = (v.set & 0x81) == 0x81 ? v.lvl0 : 0, layer = (v.set & 0x82) == 0x82 ? v.lay0 : 0;
        int whole = a[6] || (r[0] == 0 && r[1] == 0 && r[2] == 0 && r[3] >= lvl_(t->b.width, level) && r[4] >= lvl_(t->b.height, level));
        pass_end();
        VTex *vt = vt_get(t);
        int comps = t->b.format >= 1 && t->b.format <= 0x30 ? (t->b.format - 1) / 12 + 1 : 4;
        if (vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !nvn_front_fmt(t->b.format, &f) || f.kind == 4 || vt->decode || vt->expand) { nvn_vk_missing("nvnCommandBufferClearTexture of a depth, compressed or 3-component texture"); break; }
        if (!whole || ((unsigned)a[4] & ((1u << comps) - 1)) != (1u << comps) - 1) { nvn_vk_missing("nvnCommandBufferClearTexture of a part of a level or with a partial channel mask"); break; }
        cmd_begin();
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 1, (uint32_t)(vt->is3d ? 0 : layer), 1};
        VkClearColorValue cv;
        memcpy(&cv, blob + 48, 16);
        CKPT_OP("clear of a texture (image, level, layer)", vt->img, sr.baseMipLevel, sr.baseArrayLayer, 0);
        vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &sr);
        t->gpu_written = 1; t->uploaded = 1;
        break;
    }
    case OP_CALL: for (int i = 0; i < (int)a[0]; i++) { uint64_t h; memcpy(&h, blob + i * 8, 8); if (depth < 8) replay(h, depth + 1); } break;
    default: { static char msg[OP_COUNT][64]; snprintf(msg[m->op % OP_COUNT], 64, "nvnCommandBuffer%s", nvn_front_op_name(m->op)); nvn_vk_missing(msg[m->op % OP_COUNT]); break; }
    }
}
static void replay(uint64_t handle, int depth) {
    size_t len = 0;
    uint8_t *data = nvn_front_list(handle, &len);
    if (!data) { static int n; n++; if (n <= 10 || !(n & (n - 1))) { int r1_ra_phase(void); printf("nvn_vk: command handle %llx is unknown or expired (%d so far; depth %d, run-ahead phase at the replay %d)\n", (unsigned long long)handle, n, depth, r1_ra_phase()); } return; }
    for (size_t off = 0; off + sizeof(Cmd) <= len;) {
        const Cmd *m = (const Cmd *)(data + off);
        if (m->size < sizeof(Cmd)) break;
        exec(m, depth);
        off += m->size;
    }
}

/* ---------------------------------------------------------------- the RBackend members */
static void prewarm_wait(void);
static void vk_submit(void *arg) {
    SubmitJob *j = arg;
    if (!V.alive) return;                          /* after the shutdown at exit: the device is gone */
    prewarm_wait();
    LONG64 t0 = tk();
    cb_sync();                                     /* what the last batch's dispatches wrote is in guest memory before this batch reads it */
    g_cb_batch++;
    if (g_cb_nkept) cb_kept_age();
    cb_kept_index();
    LONG64 t1 = tk();
    if (g_in_fight) { g_tl_t0 = t0; g_tl_first = g_tl_last = 0; } else g_tl_t0 = 0;
    nvn_front_batch_begin();
    R.gen++;                                       /* guest memory may have changed since the last batch */
    if (g_scan_on) { LONG64 ts = tk(); Scan s; scan_begin(&s); for (int i = 0; i < j->count; i++) scan_list(j->handles[i], 0, &s); g_tm[9] += tk() - ts; }      /* session 4: what this batch will need goes to the workers before it is replayed */
    { void nvn_front_racechk(const uint64_t *handles, int count, int phase); nvn_front_racechk(j->handles, j->count, 1);      /* round 18, TEST (SSBU_RACECHK) */
      for (int i = 0; i < j->count; i++) replay(j->handles[i], 0);
      nvn_front_racechk(j->handles, j->count, 2); }
    cb_batch_end();
    g_tm[7] += t1 - t0; g_tm[3] += tk() - t1;
    nvn_front_count(3, 1);
}
static void vk_submit_async(void *arg) { SubmitJob *j = arg; j->handles = (const uint64_t *)(j + 1); vk_submit(j); }
static void vk_idle(void) { if (V.alive) { cb_poll(); nvn_front_take_dead(vk_drop); } }
static void vk_housekeeping(void *arg) { (void)arg; if (V.alive) nvn_front_take_dead(vk_drop); }
static void vk_flush(void *arg) { (void)arg; if (V.alive) cmd_submit(NULL, NULL); }
static void vk_finish(void *arg) { (void)arg; if (V.alive) cmd_wait_all(); }
static void vk_fence(void *arg) { SyncJob *j = arg; if (j->o->dead || !V.alive) return; j->o->s = (void *)(uintptr_t)(cmd_submit(NULL, NULL) + 1); }
static void vk_sync_wait(void *arg) {
    SyncJob *j = arg;
    uint64_t want = (uint64_t)(uintptr_t)j->o->s;
    if (!want || !V.alive) { j->result = 3; return; }       /* never fenced */
    want--;
    if (want <= R.done) { j->result = 0; return; }
    for (int i = 0; i < NSLOT; i++) {
        Slot *s = &R.slot[i];
        if (!s->pending || s->serial != want) continue;
        LONG64 tw0 = tk();
        VkResult r = vkWaitForFences(V.device, 1, &s->fence, VK_TRUE, j->timeout);
        if (g_tl_t0 && j->timeout) { LONG64 now = tk(); g_tl_sum[3]++; g_tl_sum[4] += now - tw0; g_tl_sum[5] += now - g_tl_t0; }
        if (r == VK_SUCCESS) { if (s->serial > R.done) R.done = s->serial; j->result = 1; cb_poll(); } else j->result = r == VK_TIMEOUT ? 2 : 3;
        return;
    }
    j->result = 0;                                  /* its command buffer was reused: finished long ago */
}
static void vk_server_wait(void *arg) { SyncJob *j = arg; j->timeout = ~0ull; if (j->o->s) vk_sync_wait(j); j->result = j->o->s != NULL; }
static void vk_sync_drop(void *arg) { SyncObj *o = arg; o->s = NULL; }
static void vk_reset_status(void *arg) { *(int *)arg = 0; }       /* NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR: a lost device ends the process with its own message */
static void vk_no_grid(void *arg) { nvn_vk_missing("nvnMultisampleStateGetSampleLocationsGrid"); ((int *)arg)[0] = ((int *)arg)[1] = 1; }

/* The window texture as a 24-bit BMP, the file save_bmp of the OpenGL path writes (row 0 of the texture is the top row). */
static void save_bmp_vk(const char *path, VTex *vt) {
    VMem mem;
    int w = vt->w, h = vt->h, bgr = vt->fmt == VK_FORMAT_B8G8R8A8_UNORM || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB;
    if (vt->rs && getenv("SSBU_VK_SHOT_NATIVE") && atoi(getenv("SSBU_VK_SHOT_NATIVE"))) {      /* track O: the scaled image itself, texel for texel (the default below is the game's size, filtered up as the window shows it) */
        VMem m2;
        uint64_t size = (uint64_t)w * h * 4;
        VkBuffer buf = vbuf_new(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m2);
        VkBufferImageCopy c;
        if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { vkDestroyBuffer(V.device, buf, NULL); vmem_free(m2); return; }
        pass_end(); cmd_begin();
        VkImageLayout was = vt->layout;
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        if (was != VK_IMAGE_LAYOUT_UNDEFINED) vt_layout(vt, was);
        cmd_wait_all();
        FILE *f = fopen(path, "wb");
        if (f) {
            uint8_t head[54] = {'B', 'M'};
            uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, fsize = 54 + row * (uint32_t)h, v;
            uint8_t *rowb = calloc(1, row);
            memcpy(head + 2, &fsize, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
            fwrite(head, 1, 54, f);
            for (int y = h - 1; y >= 0; y--) {
                const uint8_t *s = (const uint8_t *)m2.map + (size_t)y * w * 4;
                for (int x = 0; x < w; x++) { rowb[x * 3] = s[x * 4 + (bgr ? 0 : 2)]; rowb[x * 3 + 1] = s[x * 4 + 1]; rowb[x * 3 + 2] = s[x * 4 + (bgr ? 2 : 0)]; }
                fwrite(rowb, 1, row, f);
            }
            free(rowb); fclose(f);
            printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);
        }
        vkDestroyBuffer(V.device, buf, NULL); vmem_free(m2);
        return;
    }
    if (vt->rs) { w = vt->t->b.width; h = vt->t->b.height; }      /* track O: read_region takes the game's rectangle */
    if (vt->bpb != 4 || vt->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { printf("nvn_vk: screenshot: the window texture's format %d is not an 8-bit RGBA one; not saved\n", (int)vt->fmt); return; }
    /* session 5: through read_region, the path of ReadTexels / CopyTextureToBuffer (so every screenshot exercises it) */
    int rr[6] = {0, 0, 0, w, h, 1};
    uint8_t *px = malloc((size_t)w * h * 4);
    (void)mem;
    if (!px || !read_region(vt->t, NULL, rr, px, 0, 0)) { free(px); return; }
    uint8_t head[54] = {'B', 'M'}, *rowb;
    uint32_t row = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + row * (uint32_t)h, v;
    FILE *f = fopen(path, "wb");
    if (f) {
        memcpy(head + 2, &size, 4); v = 54; memcpy(head + 10, &v, 4); v = 40; memcpy(head + 14, &v, 4); memcpy(head + 18, &w, 4); memcpy(head + 22, &h, 4); head[26] = 1; head[28] = 24;
        fwrite(head, 1, 54, f);
        rowb = calloc(1, row);
        for (int y = h - 1; y >= 0; y--) {
            const uint8_t *s = px + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) { rowb[x * 3] = s[x * 4 + (bgr ? 0 : 2)]; rowb[x * 3 + 1] = s[x * 4 + 1]; rowb[x * 3 + 2] = s[x * 4 + (bgr ? 2 : 0)]; }
            fwrite(rowb, 1, row, f);
        }
        free(rowb); fclose(f);
        printf("nvn_gl: saved %s (%dx%d)\n", path, w, h);      /* the front end's line, word for word: tests/ref/port_capture.py and the shot tools look for it */
    }
    free(px);
}
static VkSemaphore g_sem[8][2];
static VkSemaphore g_sem_img[16];                  /* session 11: the "rendering finished" semaphore, one per swapchain image */
static int g_isem;
/* Session 11, the PROGRESS BAR: our own overlay, drawn over the swapchain image after the blit of the game's picture (never
 * into the game's textures, so pictures saved by SSBU_SHOT* do not have it). A bar at the bottom of the window:
 *   - while the loading barrier holds a loading screen: the same picture is presented again about ten times a second with
 *     the bar growing (jobs done of jobs queued when the hold began), so the screen is not a still picture;
 *   - while the container store is read from data.arc (first start) and while the preparation runs: a thinner bar on every
 *     presented frame, NOT near a fight.
 * SSBU_VK_PROGRESS=0 turns it off. The image: 8x8, two layers (bar colour, track colour), cleared once; drawn by two blits. */
static struct { VkImage img; VMem mem; int made, on; float frac; int thick; void *bar_arg; int screen; char text[4][48]; } g_ovl = {.on = -1};
/* Session 14: text of the first-run preparation screen. A built-in 5x7 dot font (digits, capitals, % / : . -), each lit dot
 * one blit of the overlay's light layer; no game asset. */
static void ovl_text(uint32_t index, int x, int y, int dot, const char *s) {
    static const char k_ch[] = "0123456789%/:.-ABCDEFGHIKLMNOPRSTUVWXY";
    static const uint8_t k_gl[][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},
        {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
        {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
        {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}};
    int W = (int)V.extent.width, H = (int)V.extent.height;
    for (; *s; s++, x += 6 * dot) {
        const char *q = strchr(k_ch, *s);
        if (!q || *s == ' ') continue;
        const uint8_t *g = k_gl[q - k_ch];
        for (int r = 0; r < 7; r++) for (int c = 0; c < 5; c++) {
            if (!(g[r] >> (4 - c) & 1)) continue;
            int run = 1;
            while (c + run < 5 && (g[r] >> (4 - c - run) & 1)) run++;      /* one blit per run of lit dots */
            VkImageBlit b;
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
            b.srcOffsets[1].x = 8; b.srcOffsets[1].y = 8; b.srcOffsets[1].z = 1;
            b.dstOffsets[0].x = x + c * dot; b.dstOffsets[0].y = y + r * dot; b.dstOffsets[1].x = x + (c + run) * dot; b.dstOffsets[1].y = y + (r + 1) * dot; b.dstOffsets[1].z = 1;
            if (b.dstOffsets[0].x >= 0 && b.dstOffsets[0].y >= 0 && b.dstOffsets[1].x <= W && b.dstOffsets[1].y <= H)
                vkCmdBlitImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
            c += run - 1;
        }
    }
}
/* The on-screen keyboard (osk.c): its picture, drawn there with GDI, copied over the presented frame (inside the
 * picture's 16:9 rectangle). Three staging buffers in turn, so that a new picture is never written into one a frame
 * still in flight reads. The image takes the swapchain's format (UNORM or SRGB) so the blit keeps the values. */
int osk_active(void); int osk_take(uint32_t *dst, LONG *seen); void osk_size(int *w, int *h);      /* osk.c */
static struct { VkImage img; VMem mem; VkFormat fmt; int made, w, h, cur; LONG seen; VkBuffer buf[3]; VMem bmem[3]; } g_osk;
static PresentJob g_osk_job;                       /* the last real present: shown again under the keyboard */
static int g_osk_job_ok;
static LONG64 hfr_frames_count(void);
long long nvn_vk_presents(void) { return (long long)(g_vk_frames + hfr_frames_count()); }
static void vk_present2(void *arg, int progress);
void nvn_vk_osk_redraw_job(void *arg) { (void)arg; if (V.alive && g_osk_job_ok) vk_present2(&g_osk_job, 1); }
static void osk_draw(uint32_t index) {
    if (!osk_active()) return;
    VkFormat fmt = V.format.format;
    if (!g_osk.buf[0]) {
        osk_size(&g_osk.w, &g_osk.h);
        for (int i = 0; i < 3; i++) g_osk.buf[i] = vbuf_new((uint64_t)g_osk.w * g_osk.h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_osk.bmem[i]);
    }
    if (g_osk.made && g_osk.fmt != fmt) { cmd_wait_all(); vkDestroyImage(V.device, g_osk.img, NULL); vmem_free(g_osk.mem); g_osk.made = 0; }
    int fresh = 0;
    if (!g_osk.made) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt; ci.extent.width = (uint32_t)g_osk.w; ci.extent.height = (uint32_t)g_osk.h; ci.extent.depth = 1;
        ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_osk.img));
        vkGetImageMemoryRequirements(V.device, g_osk.img, &rq);
        g_osk.mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_osk.img, g_osk.mem.mem, g_osk.mem.off));
        g_osk.made = 1; g_osk.fmt = fmt; g_osk.seen = -1; fresh = 1;
    }
    int next = (g_osk.cur + 1) % 3;
    if (osk_take((uint32_t *)g_osk.bmem[next].map, &g_osk.seen)) {
        g_osk.cur = next;
        image_barrier(g_osk.img, VK_IMAGE_ASPECT_COLOR_BIT, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy cp;
        memset(&cp, 0, sizeof cp);
        cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; cp.imageSubresource.layerCount = 1;
        cp.imageExtent.width = (uint32_t)g_osk.w; cp.imageExtent.height = (uint32_t)g_osk.h; cp.imageExtent.depth = 1;
        vkCmdCopyBufferToImage(R.cmd, g_osk.buf[next], g_osk.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
        image_barrier(g_osk.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    } else if (fresh) return;                      /* nothing to show yet */
    void display_fit(int sw, int sh, int dw, int dh, int out[4]);
    int d4[4];
    display_fit(16, 9, (int)V.extent.width, (int)V.extent.height, d4);
    VkImageBlit b;
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
    b.srcOffsets[1].x = g_osk.w; b.srcOffsets[1].y = g_osk.h; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = d4[0]; b.dstOffsets[0].y = d4[1]; b.dstOffsets[1].x = d4[0] + d4[2]; b.dstOffsets[1].y = d4[1] + d4[3]; b.dstOffsets[1].z = 1;
    vkCmdBlitImage(R.cmd, g_osk.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
}
static void menu_vk_draw(uint32_t index);          /* U1: the settings menu (nvn_vk_menu.c, included below) */
static void overlay_draw(uint32_t index) {
    osk_draw(index);
    menu_vk_draw(index);                           /* U1: over the keyboard; returns at once unless SSBU_MENU / graphics.ini menu = on */
    if (g_ovl.on < 0) g_ovl.on = !(getenv("SSBU_VK_PROGRESS") && !atoi(getenv("SSBU_VK_PROGRESS")));
    if ((!g_ovl.on && !g_ovl.screen) || g_ovl.thick <= 0) return;
    if (g_ovl.thick < 8) g_ovl_frames++;
    VkImageSubresourceRange sr2 = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!g_ovl.made) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        VkClearColorValue c0 = {{0.92f, 0.92f, 0.92f, 1.0f}}, c1 = {{0.06f, 0.06f, 0.06f, 1.0f}};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_B8G8R8A8_UNORM; ci.extent.width = 8; ci.extent.height = 8; ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = 2;
        ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_ovl.img));
        vkGetImageMemoryRequirements(V.device, g_ovl.img, &rq);
        g_ovl.mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_ovl.img, g_ovl.mem.mem, g_ovl.mem.off));
        image_barrier(g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c0, 1, &sr2);
        sr2.baseArrayLayer = 1;
        vkCmdClearColorImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c1, 1, &sr2);
        image_barrier(g_ovl.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        g_ovl.made = 1;
    }
    int W = (int)V.extent.width, H = (int)V.extent.height, th = g_ovl.thick * H / 720, x0 = W / 10, x1 = W - W / 10, y1 = H - H / 40, y0;
    float f = g_ovl.frac < 0 ? 0 : g_ovl.frac > 1 ? 1 : g_ovl.frac;
    if (th < 2) th = 2;
    y0 = y1 - th;
    if (x1 - x0 < 8 || y0 < 0) return;
    for (int pass = 0; pass < 2; pass++) {          /* the track, then the bar over it */
        VkImageBlit b;
        int xe = pass ? x0 + (int)((float)(x1 - x0) * f) : x1;
        if (xe <= x0) continue;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.srcSubresource.baseArrayLayer = pass ? 0 : 1;
        b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = 8; b.srcOffsets[1].y = 8; b.srcOffsets[1].z = 1;
        b.dstOffsets[0].x = pass ? x0 : x0 - 2; b.dstOffsets[0].y = pass ? y0 : y0 - 2; b.dstOffsets[1].x = pass ? xe : x1 + 2; b.dstOffsets[1].y = pass ? y1 : y1 + 2; b.dstOffsets[1].z = 1;
        CKPT_OP("progress bar blit (swapchain image, index)", V.images[index], index, 0, 0);
        vkCmdBlitImage(R.cmd, g_ovl.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    }
    if (g_ovl.screen) {                             /* session 14: the first-run preparation screen: four lines of text over the bar */
        int dot = H / 180 > 1 ? H / 180 : 2;
        for (int i = 0; i < 4; i++) if (g_ovl.text[i][0]) { int d = i ? dot : dot * 2; ovl_text(index, (W - (int)strlen(g_ovl.text[i]) * 6 * d) / 2, H / 4 + i * dot * 22, d, g_ovl.text[i]); }
    }
}
static void vk_present2(void *arg, int progress);
/* Session 14: THE KEPT PRESENT. vk_present2 has blitted and submitted the frame; when display.c says the frame is to be shown
 * at the next tick, vkQueuePresentKHR is not called there: what it needs is kept here and the vsync thread posts
 * nvn_vk_present_flush_job at the tick (render thread, between two other jobs). An out-of-date swapchain found here is
 * made again at the next present (not in the middle of a frame's commands). */
int display_present_defer(void); int display_present_due(void); void display_present_flushed(void); void display_present_done(int fight);      /* display.c */
static struct { int on, suboptimal, fight, recreate; VkSwapchainKHR sc; uint32_t index; VkSemaphore rend; } g_pend;
static volatile LONG64 g_gap_prev, g_gap_max, g_wd_beat;
static void present_flush(void) {
    if (!g_pend.on) return;
    g_pend.on = 0;
    display_present_flushed();
    if (g_pend.sc != V.swapchain || !V.swapchain) return;      /* the swapchain was made again meanwhile: the image went with it */
    VkPresentInfoKHR pr = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &g_pend.rend; pr.swapchainCount = 1; pr.pSwapchains = &g_pend.sc; pr.pImageIndices = &g_pend.index;
    VkResult r = vkQueuePresentKHR(V.queue, &pr);
    display_present_done(g_pend.fight);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || g_pend.suboptimal) g_pend.recreate = 1;
    else vk_ok("vkQueuePresentKHR", r);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { LONG64 nowt = tk(); if (g_gap_prev && nowt - g_gap_prev > g_gap_max) g_gap_max = nowt - g_gap_prev; g_gap_prev = nowt; g_wd_beat = nowt; }
}
void nvn_vk_present_flush_job(void *arg) { (void)arg; if (V.alive) present_flush(); }
#include "nvn_vk_menu.c"                          /* U1: the settings menu over the picture (menu.cpp builds it) */
static LONG64 hfr_frames_count(void) { return g_hfr.frames; }
int display_hfr_mode(void);
void display_hfr_job_begin(void);
void display_hfr_present_done(void);
double display_vblank_period_qpc(void);
double display_hfr_alpha(LONG64 t_prev, LONG64 t_cur);
void nvn_vk_hfr_present_job(void *arg) {
    (void)arg;
    display_hfr_job_begin();
    if (!V.alive || g_pend.on || !g_hfr.ready || g_ovl.screen || display_hfr_mode() <= 0) return;
    if (g_pend.recreate || !V.swapchain) {
        g_pend.recreate = 0;
        cmd_wait_all();
        if (V.swapchain) swapchain_destroy();
        InterlockedIncrement64(&g_vk_recreated);
        if (!swapchain_create()) return;
    }
    double per_qpc = display_vblank_period_qpc();
    if (tk() - g_hfr.t_last_present < (LONG64)(per_qpc * 0.25)) return;
    int next_sem = (g_isem + 1) % 8;
    if (!g_sem[next_sem][0]) {
        VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[next_sem][0]));
        VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[next_sem][1]));
    }
    VkSemaphore acq = g_sem[next_sem][0], rend = g_sem[next_sem][1];
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { g_pend.recreate = 1; return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    g_isem = next_sem;
    int suboptimal = r == VK_SUBOPTIMAL_KHR;
    if (index < 16) {
        if (!g_sem_img[index]) { VkSemaphoreCreateInfo smi2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi2, NULL, &g_sem_img[index])); }
        rend = g_sem_img[index];
    }
    int sl = g_hfr.slot = (g_hfr.slot + 1) & 3;
    if (!g_hfr.cmd[sl]) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = V.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(V.device, &ai, &g_hfr.cmd[sl]));
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VKC(vkCreateFence(V.device, &fi, NULL, &g_hfr.fence[sl]));
    }
    if (g_hfr.pending[sl]) {
        vkWaitForFences(V.device, 1, &g_hfr.fence[sl], VK_TRUE, 5000000000ull);
        vkResetFences(V.device, 1, &g_hfr.fence[sl]);
        g_hfr.pending[sl] = 0;
    }
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkResetCommandBuffer(g_hfr.cmd[sl], 0));
    VKC(vkBeginCommandBuffer(g_hfr.cmd[sl], &bi));
    VkCommandBuffer prev_cmd = R.cmd;
    R.cmd = g_hfr.cmd[sl];
    VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    void display_fit(int sw, int sh, int dw, int dh, int out[4]);
    int d4[4];
    display_fit(16, 9, (int)V.extent.width, (int)V.extent.height, d4);
    if (d4[2] != (int)V.extent.width || d4[3] != (int)V.extent.height) {
        VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f;
        vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
        VkImageMemoryBarrier ib; memset(&ib, 0, sizeof ib);
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; ib.srcAccessMask = ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = V.images[index]; ib.subresourceRange = sr;
        vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    }
    float alpha = (float)display_hfr_alpha(g_hfr.t_prev, g_hfr.t_cur);
    if (alpha < g_hfr.last_alpha) alpha = g_hfr.last_alpha;
    g_hfr.last_alpha = alpha;
    hfr_blit_or_blend(index, d4, alpha);
    overlay_draw(index);
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VKC(vkEndCommandBuffer(g_hfr.cmd[sl]));
    R.cmd = prev_cmd;
    VkFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo sb = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    sb.waitSemaphoreCount = 1; sb.pWaitSemaphores = &acq; sb.pWaitDstStageMask = &stage;
    sb.signalSemaphoreCount = 1; sb.pSignalSemaphores = &rend;
    sb.commandBufferCount = 1; sb.pCommandBuffers = &g_hfr.cmd[sl];
    VKC(vkQueueSubmit(V.queue, 1, &sb, g_hfr.fence[sl]));
    g_hfr.pending[sl] = 1;
    VkPresentInfoKHR pr = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &rend; pr.swapchainCount = 1; pr.pSwapchains = &V.swapchain; pr.pImageIndices = &index;
    r = vkQueuePresentKHR(V.queue, &pr);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || suboptimal) g_pend.recreate = 1;
    else vk_ok("vkQueuePresentKHR", r);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        LONG64 nowt = tk();
        g_hfr.t_last_present = nowt;
        g_wd_beat = nowt;
        InterlockedIncrement64(&g_hfr.frames);
        display_hfr_present_done();
    }
}
#include "nvn_vk_flash.c"                         /* session 13: the white-flash detector (fl_pass, fl_capture, fl_shutdown) */
/* nvn_vk_m4.c m4_barrier, render thread, g_sh_lock NOT held: the held loading screen once more, with the bar at `frac` */
static void present_progress(float frac) { if (!g_ovl.bar_arg) return; g_ovl.frac = frac; g_ovl.thick = 8; vk_present2(g_ovl.bar_arg, 1); g_ovl.thick = 0; }
/* Session 14, THE FIRST-RUN PREPARATION (the owner's order: "all at the first launch, then never again, with a percentage").
 * graphics.ini shader_prepare = first_run (default) | background | off; SSBU_VK_PREPARE=<the same words> wins.
 * At the game's FIRST present of a start without the preparation's marker (first start on this GPU + driver + version of
 * ours, or new game data: prep_main removed the marker), the render thread keeps that present and shows a screen of its
 * own ten times a second (dark picture, "PREPARING SHADERS NN %", stages done / known so far, time left, the bar) while
 * the preparation runs with EVERY worker at normal priority (g_prep_urgent): the container store, every stage of every
 * container, every stage library into the driver's pipeline cache. The game's main thread stands in its present. At the
 * end the pipeline cache and the marker are written here (a thread of its own writes the file), then the game goes on.
 * The window closed meanwhile: the hold ends at once, the exit path saves the pipeline cache (what was added), no marker:
 * the next start shows the screen again and finds the stage records and the cache of the part already done.
 * NOT covered (said in the log line at the end): pipelines of pairs never drawn on this PC are still linked at their first
 * loading screen or draw (a link is 0.011 ms on NVIDIA, up to 3.9 ms on Intel). */
extern volatile LONG g_display_close;              /* display.c */
static void prep_first_hold(LONG64 frame) {
    static int done, mode = -1;
    if (done) return;
    if (mode < 0) { const char *e = getenv("SSBU_VK_PREPARE"); mode = e ? (!_stricmp(e, "background") ? 1 : !_stricmp(e, "off") ? 2 : 0) : gfx_options()->shader_prepare; }
    /* 2026-10-16: on a PREPARED start with shader_load = start (g_prep_loadall, nvn_vk_m4.c prep_start) the same screen
     * says "LOADING SHADERS": the game's first present waits until every container is in memory (the work began at the
     * device's creation, during the white screen of the game's own boot; this shows what is left) */
    int loadall = g_prep_marked && g_prep_loadall;
    if ((mode && !loadall) || !g_prep_thread || (g_prep_marked && !loadall) || g_prep_limited || !g_ovl.bar_arg) { done = 1; if (frame < 3) printf("nvn_vk_prep: first-run screen not shown: %s\n", mode == 1 ? "shader_prepare = background" : mode == 2 ? "shader_prepare = off" : !g_prep_thread ? "no preparation on this device (no stage libraries)" : g_prep_marked ? "the marker of an earlier run is there: everything was prepared for this GPU, driver and version; straight to the game (shader_load = loading)" : "test limit"); return; }
    if (g_prep_state == 2 || g_prep_state == 3) { done = 1; if (loadall) printf("nvn_vk_prep: SHADERS LOADED AT START: everything was in memory before the game's first frame (%lld): no wait\n", (long long)frame); return; }
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONG64 t0 = tk(), tlog = t0, tprog = t0, dn_last = -1;
    int stopped = 0, stuck = 0;
    done = 1;
    if (loadall) printf("nvn_vk_prep: LOADING SHADERS: the game is held at its first frame (%lld) until every container of the game's data is in memory (%lld of %lld stage jobs done, %d of %d files read so far); %d workers at normal priority\n", (long long)frame, (long long)g_pq_done, (long long)g_pq_queued, g_prep_read, g_prep_nfiles, g_pw_nthread);
    else printf("nvn_vk_prep: FIRST-RUN PREPARATION: no marker %s for this GPU, driver and version (first start here, a new driver, or new game data): the game is held at its first frame (%lld) until every shader is prepared; %d workers at normal priority\n", g_prep_marker, (long long)frame, g_pw_nthread);
    fflush(stdout);
    g_ovl.screen = 1;
    for (;;) {
        int st = g_prep_state;
        if (st == 2 || st == 3) break;
        if (g_display_close || g_m4_quit) { stopped = 1; break; }
        g_prep_urgent = 1;
        WakeAllConditionVariable(&g_jq_cv);
        LONG64 dn = g_pq_done, qd = g_pq_queued, left_files = g_prep_nfiles - g_prep_read;
        double el = (double)(tk() - t0) / (double)fq.QuadPart, f;
        if (loadall) {   /* a safety net, never seen: no job done and no file read for 30 s = let the game go on (the loading screens make the rest) */
            if (dn + g_prep_read != dn_last) { dn_last = dn + g_prep_read; tprog = tk(); }
            else if (tk() - tprog > 30 * fq.QuadPart) { stuck = 1; break; }
        }
        /* the store (reading data.arc) is the first tenth; then jobs done of jobs known + about 90 a file not read yet */
        if (st == 5 || !g_prep_nfiles) f = 0.10 * (double)g_store_pct / 100.0;
        else f = 0.10 + 0.90 * (qd > 0 ? (double)dn / (double)(qd + left_files * 90) : 0.0);
        if (f > 0.999) f = 0.999;
        int pct = (int)(f * 100.0), eta = f > 0.03 && el > 3 ? (int)(el * (1.0 - f) / f) : -1;
        if (loadall) f = qd > 0 ? (double)dn / (double)(qd + left_files * 90) : 0.0;
        if (f > 0.999) f = 0.999;
        pct = (int)(f * 100.0); eta = f > 0.03 && el > 3 ? (int)(el * (1.0 - f) / f) : -1;
        snprintf(g_ovl.text[0], sizeof g_ovl.text[0], loadall ? "LOADING SHADERS %d %%" : "PREPARING SHADERS %d %%", pct);
        if (st == 5 || !g_prep_nfiles) snprintf(g_ovl.text[1], sizeof g_ovl.text[1], "READING THE GAME DATA");
        else snprintf(g_ovl.text[1], sizeof g_ovl.text[1], "%lld / %lld", (long long)dn, (long long)(qd + left_files * 90));
        if (eta >= 0) snprintf(g_ovl.text[2], sizeof g_ovl.text[2], "ABOUT %d:%02d LEFT", eta / 60, eta % 60); else snprintf(g_ovl.text[2], sizeof g_ovl.text[2], "TIME LEFT: ...");
        snprintf(g_ovl.text[3], sizeof g_ovl.text[3], loadall ? "NO SHADER LOADING BEFORE A FIGHT" : "FIRST START ONLY");
        present_progress((float)f);
        if (tk() - tlog > 10 * fq.QuadPart) { tlog = tk(); printf("nvn_vk_prep: %s screen: %d %%, %lld of about %lld stages, %.0f s so far, about %d s left\n", loadall ? "loading" : "first-run", pct, (long long)dn, (long long)(qd + left_files * 90), el, eta); fflush(stdout); }
        Sleep(100);
    }
    g_prep_urgent = 0;
    g_ovl.screen = 0;
    double el = (double)(tk() - t0) / (double)fq.QuadPart;
    if (loadall) {
        if (stopped) printf("nvn_vk_prep: LOADING SHADERS stopped after %.1f s (the window was closed)\n", el);
        else if (stuck) printf("nvn_vk_prep: LOADING SHADERS: no progress for 30 s (%lld of %lld jobs, %d of %d files): the game goes on, the rest is made at the loading screens\n", (long long)g_pq_done, (long long)g_pq_queued, g_prep_read, g_prep_nfiles);
        else printf("nvn_vk_prep: SHADERS LOADED AT START: the game's first frame waited %.1f s; %lld stage jobs, every container of the game's data in memory (memory: the FINISHED line above)\n", el, (long long)g_pq_done);
        fflush(stdout);
        return;
    }
    if (stopped) printf("nvn_vk_prep: FIRST-RUN PREPARATION STOPPED after %.1f s (the window was closed): the pipeline cache is saved on the exit path, no marker: the next start goes on from the stage records and the cache\n", el);
    else {
        printf("nvn_vk_prep: FIRST-RUN PREPARATION FINISHED in %.1f s on %d workers (state %d): %lld stage jobs; the pipeline cache and the marker are written now; pipelines of pairs never drawn on this PC are NOT made here (linked at their first loading screen)\n", el, g_pw_nthread, g_prep_state, (long long)g_pq_done);
        if (InterlockedExchange(&g_pcsave_want, 0)) pcache_save_at(1, 1);
    }
    fflush(stdout);
}
static void vk_present(void *arg) { vk_present2(arg, 0); }
static void vk_present2(void *arg, int progress) {
    PresentJob *j = arg;
    static int shot = -2, every, shot_from;
    if (!V.alive) return;
    LONG64 t_present = tk();
    LONG64 tp[8] = {0, 0, 0, 0, 0, 0, 0, 0};      /* round 17: the present job timed in parts ("nvn_vk_presentlong:"): after the barrier, before / after the kept present of the last frame, after the acquire, after the blit's submission, after the present call, after the front end's frame end */
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    VTex *vt = t && !t->dead ? t->vk : NULL;
    LONG64 frame = nvn_front_frame_next();
    {   /* session 12, TEST ONLY: SSBU_VK_WD_TEST=<frame>[,<seconds>] holds the render thread at that frame (default 12 s), to see the watchdog's report */
        static int wt = -2, ws = 12;
        if (wt == -2) { const char *e = getenv("SSBU_VK_WD_TEST"); wt = e ? atoi(e) : -1; if (e && strchr(e, ',')) ws = atoi(strchr(e, ',') + 1); }
        if (wt > 0 && frame == wt && !progress) { printf("nvn_vk_watchdog: TEST: the render thread is held %d s at frame %lld (SSBU_VK_WD_TEST)\n", ws, (long long)frame); fflush(stdout); wt = -1; Sleep((DWORD)ws * 1000); }
    }
    if (progress) goto swap;
    if (j != &g_osk_job) { g_osk_job = *j; g_osk_job_ok = 1; }      /* osk.c: what to show again under the keyboard */
    if (g_tl_t0 && g_tl_first) { LONG64 now = tk(); g_tl_sum[0] += g_tl_first - g_tl_t0; g_tl_sum[1] += g_tl_last - g_tl_t0; g_tl_sum[2] += now - g_tl_t0; g_tl_n++; g_tl_first = 0; }
    if (g_pt_on > 0 && g_in_fight) g_pt_frames++;
    if (j->win) for (int i = 0; i < j->win->count && i < 8; i++) wintex_note(j->win->tex[i]);
    g_ovl.bar_arg = arg;
    g_wd_where = 1;
    m4_barrier(frame, R.frame_draws);
    prep_first_hold(frame);                        /* session 14: the first-run preparation screen (returns at once on every later present) */
    g_wd_where = 0;
    g_ovl.bar_arg = NULL;
    tp[0] = tk();
    /* the thin bar of a normal frame: the store being read, or the preparation running, and no fight near */
    g_ovl.thick = 0;
    if ((g_prep_state == 5 || g_prep_state == 1) && !fight_recent()) {
        g_ovl.thick = 4;
        g_ovl.frac = g_prep_state == 5 ? (float)g_store_pct / 100.0f : g_prep_nfiles ? (float)g_pq_done / (float)(g_pq_queued > 0 ? g_pq_queued + (LONG64)(g_prep_nfiles - g_prep_read) * 90 : 1) : 0;      /* about 90 variations a file */
    }
    g_render_tid = GetCurrentThreadId();
    if (g_skip_fr_n) { g_skip_frames++; printf("nvn_vk_skipf: frame %lld (%s): %lld draws left out (shader_wait = skip)\n", (long long)frame, g_in_fight ? "fight" : "no fight", (long long)g_skip_fr_n); g_skip_fr_n = 0; }
    if (shot == -2) { shot = getenv("SSBU_SHOT") ? atoi(getenv("SSBU_SHOT")) : -1; every = getenv("SSBU_SHOT_EVERY") ? atoi(getenv("SSBU_SHOT_EVERY")) : 0; shot_from = getenv("SSBU_SHOT_FROM") ? atoi(getenv("SSBU_SHOT_FROM")) : 0; }
    pass_end();
    { int step = nvn_front_take_step_shot(); if (step && vt) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sstep_%03d.bmp", dir ? dir : "", dir ? "/" : "", step - 1); save_bmp_vk(path, vt); } }
    nvn_front_draw_tag_clear();
    if (j->shot_fight && vt) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sfight_%05d.bmp", dir ? dir : "", dir ? "/" : "", j->shot_fight); save_bmp_vk(path, vt); printf("shot: fight frame %d saved at presented frame %lld (Vulkan)\n", j->shot_fight, (long long)frame); }      /* session 13, SSBU_SHOT_FIGHT */
    if (vt && (frame == shot || (every > 0 && frame >= shot_from && frame % every == 0))) {
        char path[300];
        const char *dir = getenv("SSBU_SHOT_DIR");
        snprintf(path, sizeof path, "%s%sframe_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)frame);
        save_bmp_vk(path, vt);
    }
    {   /* round 18, TEST: SSBU_SHOT_BURST=<period>,<length>[,<from>]: of the presented frames with a fight in them (counted
         * here), <length> consecutive ones every <period> are saved as burst_<fight present>_<presented frame>.bmp */
        static int bper = -2, blen, bfrom;
        static LONG64 ffn;
        if (bper == -2) { const char *e = getenv("SSBU_SHOT_BURST"); bper = -1; if (e) sscanf(e, "%d,%d,%d", &bper, &blen, &bfrom); }
        if (bper > 0 && g_in_fight) {
            ffn++;
            if (vt && ffn >= bfrom && ffn % bper < blen) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sburst_%06lld_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)ffn, (long long)frame); save_bmp_vk(path, vt); }
        }
    }
    {   /* round 18: SSBU_VK_CAPKEY=1 (F9) or =<virtual key code>: when the key goes down the next 12 presented frames are
         * saved as cap_<press>_<presented frame>.bmp in SSBU_SHOT_DIR, and with SSBU_DRAWTAG=1 the draws of those frames are
         * printed (DT / DTV lines, tag = the presented frame modulo 100000 + 100000). For the player who SEES a picture fault
         * that scripted runs do not show: press while it is on screen. One GetAsyncKeyState per frame, only with the variable. */
        static int ck = -2, left, was_down;
        static LONG64 press;
        if (ck == -2) { const char *e = getenv("SSBU_VK_CAPKEY"); ck = e ? (atoi(e) > 1 ? atoi(e) : 0x78) : 0; if (ck) printf("nvn_vk_capture: key 0x%x saves 12 consecutive frames (SSBU_VK_CAPKEY)\n", ck); }
        if (ck) {
            int down = (GetAsyncKeyState(ck) & 0x8000) != 0;
            if (down && !was_down && !left) { left = 12; press++; printf("nvn_vk_capture: press %lld at presented frame %lld (%s): 12 frames follow\n", (long long)press, (long long)frame, g_in_fight ? "fight" : "no fight"); }
            was_down = down;
            if (left && vt) {
                char path[300]; const char *dir = getenv("SSBU_SHOT_DIR");
                snprintf(path, sizeof path, "%s%scap_%02lld_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)press, (long long)frame);
                save_bmp_vk(path, vt);
                if (--left) { extern volatile LONG g_drawtag_next; InterlockedExchange(&g_drawtag_next, (LONG)((frame + 1) % 100000) + 100000); }
            }
        }
    }
    {   /* session 9, SSBU_SHOT_BATTLE=<n>: one picture at the n-th battle frame (dispatches and more than 120 draws), i.e. at the
         * same moment of the fight whatever the loading took: battle_<n>.bmp in SSBU_SHOT_DIR */
        static int bshot = -2;
        if (bshot == -2) bshot = getenv("SSBU_SHOT_BATTLE") ? atoi(getenv("SSBU_SHOT_BATTLE")) : -1;
        if (vt && bshot > 0 && g_battle_frames == bshot) {
            char path[300];
            const char *dir = getenv("SSBU_SHOT_DIR");
            snprintf(path, sizeof path, "%s%sbattle_%d.bmp", dir ? dir : "", dir ? "/" : "", bshot);
            save_bmp_vk(path, vt);
            printf("nvn_vk: picture of battle frame %d saved at frame %lld: %s\n", bshot, (long long)frame, path);
            bshot = -1;
        }
    }
swap: ;
    pass_end();
    tp[1] = tk();
    present_flush();
    tp[2] = tk();                               /* session 14: a present still kept for its tick goes out before the next one is made */
    if (g_pend.recreate) { g_pend.recreate = 0; cmd_wait_all(); swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); }
    if (!V.swapchain) swapchain_create();
    if (!progress && display_hfr_mode() > 0 && !g_ovl.screen && V.swapchain) {
        if (vt && vt->samples == 1) {
            int s4[4] = {0, 0, vt->w, vt->h};
            if (j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(s4, j->win->crop, sizeof s4);
            if (vt->rs && j->win->crop[2] > 0 && j->win->crop[3] > 0) {
                int x1 = rs_edge(s4[0] + s4[2], vt->sx, vt->w), y1 = rs_edge(s4[1] + s4[3], vt->sy, vt->h);
                s4[0] = rs_edge(s4[0], vt->sx, vt->w); s4[1] = rs_edge(s4[1], vt->sy, vt->h); s4[2] = x1 - s4[0]; s4[3] = y1 - s4[1];
            }
            void display_fit(int sw, int sh, int dw, int dh, int out[4]);
            int d4[4];
            display_fit(16, 9, (int)V.extent.width, (int)V.extent.height, d4);
            VTex *src = vt;
            if (vt->rs && vt->sx > 1.0f) rs_reduce(&src, s4, d4[2], d4[3]);
            cmd_begin();
            vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            cmd_begin();
            hfr_capture(src, s4, d4);
            cmd_submit(NULL, NULL);
        } else {
            cmd_submit(NULL, NULL);
        }
        g_wd_where = 0;
        tp[5] = tk();
        g_tm[4] += tp[5] - t_present;
        nvn_front_frame_end(R.frame_draws, R.frame_skipped, R.frame_clears);
        tp[6] = tk();
        { static LONG64 disp_prev; g_in_fight = R.dispatches != disp_prev; disp_prev = R.dispatches; if (g_in_fight) { g_fight_last = frame; if (g_prep_state == 1) g_prep_fight_frames++; } }
        {   static LONG64 t_prev;
            LONG64 t_now = tk();
            int battle = g_in_fight && R.frame_draws > 120;
            if (battle) g_battle_frames++;
            if (frame >= 0 && frame < FRSTAT_N) {
                LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
                g_fr_fight[frame] = (uint8_t)battle;
                g_fr_wall[frame] = t_prev ? (float)((double)(t_now - t_prev) * 1000.0 / (double)fq.QuadPart) : 0.0f;
            }
            t_prev = t_now;
        }
        return;
    }
    /* an extra present of a held loading screen: everything submitted so far is waited for first, so that the semaphore pair
     * taken below has no pending operation (validation run s11v2: VUID-vkAcquireNextImageKHR-semaphore-01779, three times,
     * all at such presents: with one submission a present the pair came round before its command buffer had been waited for) */
    if (progress) cmd_wait_all();
    /* The swapchain has the window texture's colour class: a blit between them then keeps the stored values (the OpenGL
     * path blits with FRAMEBUFFER_SRGB off). */
    VkFormat want = vt && (vt->fmt == VK_FORMAT_R8G8B8A8_SRGB || vt->fmt == VK_FORMAT_B8G8R8A8_SRGB) ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    if (want != V.format.format) { cmd_wait_all(); swapchain_destroy(); V.format.format = want; printf("nvn_vk: swapchain format %d (the window texture's colour class)\n", (int)want); }
    uint32_t index = 0;
    VkResult r = VK_SUCCESS;
    /* one pair of semaphores per present, 8 in turn: a semaphore handed to vkAcquireNextImageKHR must not still be waited
     * for by an earlier submission (validation layer: VUID-vkAcquireNextImageKHR-semaphore-01779) */
    g_isem = (g_isem + 1) % 8;
    if (!g_sem[g_isem][0]) { VkSemaphoreCreateInfo smi = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[g_isem][0])); VKC(vkCreateSemaphore(V.device, &smi, NULL, &g_sem[g_isem][1])); }
    VkSemaphore acq = g_sem[g_isem][0], rend = g_sem[g_isem][1];
    int ok = V.swapchain || swapchain_create();
    InterlockedIncrement64(&g_wd_try);
    if (ok) {
        g_wd_where = 2;
        r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { cmd_wait_all(); swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); ok = swapchain_create(); if (ok) r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, acq, NULL, &index); }
        if (ok && r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_ok("vkAcquireNextImageKHR", r); ok = 0; }
    }
    tp[7] = tk();
    if (!ok) { InterlockedIncrement64(&g_vk_skipped); cmd_submit(NULL, NULL); }
    else {
        int suboptimal = r == VK_SUBOPTIMAL_KHR;
        VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        {   /* session 11: the semaphore a present waits for is one PER SWAPCHAIN IMAGE (validation run s11v3, twice in 9,783
             * frames: VUID-vkQueueSubmit-pSignalSemaphores-00067, "may still be in use by the swapchain", when the driver
             * hands out the same image many times in a row and the ring of 8 comes round); the acquire semaphore stays in the ring */
            if (index < 16) {
                if (!g_sem_img[index]) { VkSemaphoreCreateInfo smi2 = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(V.device, &smi2, NULL, &g_sem_img[index])); }
                rend = g_sem_img[index];
            }
        }
        cmd_begin();
        int fl_layout = vt ? (int)vt->layout : -1, fl_s4[4] = {0, 0, vt ? vt->w : 0, vt ? vt->h : 0};
        if (vt) vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        cmd_begin();
        image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        if (vt && vt->samples == 1 && !(progress && g_ovl.screen)) {      /* the first-run screen is ours: a dark picture, not the game's frame */
            VkImageBlit b;
            int s4[4] = {0, 0, vt->w, vt->h};
            if (j->win->crop[2] > 0 && j->win->crop[3] > 0) memcpy(s4, j->win->crop, sizeof s4);
            if (vt->rs && j->win->crop[2] > 0 && j->win->crop[3] > 0) {      /* track O: the crop is in the game's pixels */
                int x1 = rs_edge(s4[0] + s4[2], vt->sx, vt->w), y1 = rs_edge(s4[1] + s4[3], vt->sy, vt->h);
                s4[0] = rs_edge(s4[0], vt->sx, vt->w); s4[1] = rs_edge(s4[1], vt->sy, vt->h); s4[2] = x1 - s4[0]; s4[3] = y1 - s4[1];
            }
            memcpy(fl_s4, s4, sizeof fl_s4);           /* PR #8: taken BEFORE rs_reduce: fl_capture reads this rectangle of vt itself, not of a reduction step */
            VTex *src = vt;
            /* PR #24 (v1Ta44): the picture keeps its shape (display.c display_fit): black bars when the window is not 16:9 */
            void display_fit(int sw, int sh, int dw, int dh, int out[4]);
            int d4[4];
            display_fit(16, 9, (int)V.extent.width, (int)V.extent.height, d4);
            if (vt->rs && vt->sx > 1.0f) rs_reduce(&src, s4, d4[2], d4[3]);      /* PR #8 (stephenjayakar): track O, supersampling: halved in 2:1 steps to at most twice the picture */
            memset(&b, 0, sizeof b);
            b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
            b.srcOffsets[0].x = s4[0]; b.srcOffsets[0].y = s4[1]; b.srcOffsets[1].x = s4[0] + s4[2]; b.srcOffsets[1].y = s4[1] + s4[3]; b.srcOffsets[1].z = 1;
            if (d4[2] != (int)V.extent.width || d4[3] != (int)V.extent.height) {
                VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f;
                vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
                VkImageMemoryBarrier ib; memset(&ib, 0, sizeof ib);      /* the clear is written before the blit writes over it */
                ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; ib.srcAccessMask = ib.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                ib.image = V.images[index]; ib.subresourceRange = sr;
                vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
            }
            b.dstOffsets[0].x = d4[0]; b.dstOffsets[0].y = d4[1]; b.dstOffsets[1].x = d4[0] + d4[2]; b.dstOffsets[1].y = d4[1] + d4[3]; b.dstOffsets[1].z = 1;
            if (g_pt_on > 0) pt_mark((int)V.extent.width, (int)V.extent.height, 0, 0, 0, 1, 2);
            CKPT_OP("present blit (swapchain image, index, w)", V.images[index], index, vt->w, 0);
            vkCmdBlitImage(R.cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);      /* PR #8: src = vt, or the last reduction step */
        } else { VkClearColorValue z; memset(&z, 0, sizeof z); z.float32[3] = 1.0f; vkCmdClearColorImage(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr); }
        fl_capture(vt, fl_s4, frame, fl_layout);      /* session 13: 16x9 copy of what is presented, for the flash detector */
        overlay_draw(index);
        image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        tp[3] = tk();
        cmd_submit(acq, rend);
        tp[4] = tk();
        VkPresentInfoKHR pr = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &rend; pr.swapchainCount = 1; pr.pSwapchains = &V.swapchain; pr.pImageIndices = &index;
        g_wd_where = 4;
        if (!progress && display_present_defer()) {      /* session 14: the finished frame goes to the window AT THE TICK (display.c): kept here, presented by present_flush */
            g_pend.sc = V.swapchain; g_pend.index = index; g_pend.rend = rend; g_pend.suboptimal = suboptimal; g_pend.fight = g_in_fight; g_pend.on = 1;
            if (display_present_due()) present_flush();
            r = VK_SUCCESS;
        } else {
        r = vkQueuePresentKHR(V.queue, &pr);
        if (!progress) display_present_done(g_in_fight);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || suboptimal) { cmd_wait_all(); swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); }
        else vk_ok("vkQueuePresentKHR", r);
        }
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) { LONG64 nowt = tk(); if (g_gap_prev && nowt - g_gap_prev > g_gap_max) g_gap_max = nowt - g_gap_prev; g_gap_prev = nowt; g_wd_beat = nowt; }      /* session 12: the watchdog's heartbeat = a frame reached the window */
        if (!progress) InterlockedIncrement64(&g_vk_frames);
    }
    g_wd_where = 0;
    if (progress) { g_ovl_presents++; return; }
    tp[5] = tk();
    g_tm[4] += tp[5] - t_present;
    nvn_front_frame_end(R.frame_draws, R.frame_skipped, R.frame_clears);
    tp[6] = tk();
    { static LONG64 disp_prev; g_in_fight = R.dispatches != disp_prev; disp_prev = R.dispatches; if (g_in_fight) { g_fight_last = frame; if (g_prep_state == 1) g_prep_fight_frames++; } }      /* a frame with compute dispatches (skinning): fighters are on screen */
    {   /* session 9: per-frame record for the "nvn_vk_gpu_fight:" line */
        static LONG64 t_prev;
        LONG64 t_now = tk();
        int battle = g_in_fight && R.frame_draws > 120;      /* the fight draws 126..450 a frame (entrance over 205), loading screens 38..45 */
        if (battle) g_battle_frames++;
        {   /* how long the way to the fight took (menus are scripted by polls, so two runs differ by their loading) */
            static LONG64 t_first, t_load; static int said;
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            if (!t_first) t_first = t_now;
            if (!battle && R.frame_draws <= 60 && frame > 600) { if (!t_load) t_load = t_now; } else if (!battle && R.frame_draws > 60) t_load = 0;      /* t_load: the start of the run of loading-screen frames before the battle */
            if (battle && !said) { said = 1; printf("nvn_vk: first battle frame is frame %lld, %.1f s after the first present; the loading screen before it lasted %.1f s\n", (long long)frame, (double)(t_now - t_first) / (double)fq.QuadPart, t_load ? (double)(t_now - t_load) / (double)fq.QuadPart : 0.0); }
        }
        if (frame >= 0 && frame < FRSTAT_N) {
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            g_fr_fight[frame] = (uint8_t)battle;
            g_fr_wall[frame] = t_prev ? (float)((double)(t_now - t_prev) * 1000.0 / (double)fq.QuadPart) : 0.0f;
            if (battle && g_battle_frames > 30) {  /* round 16: first draws of a pipeline in this frame against the frame's time */
                static int fl = -1;
                double wall = (double)g_fr_wall[frame], k = 1000.0 / (double)fq.QuadPart;
                if (fl < 0) fl = getenv("SSBU_VK_FIRSTLOG") && atoi(getenv("SSBU_VK_FIRSTLOG"));
                g_fu_fight_frames++; if (wall > 20.0) g_fu_fight_slow++;
                if (g_fu_fr_n) {
                    g_fu_frames++; g_fu_total += g_fu_fr_n; g_fu_linked += g_fu_fr_linked; g_fu_made += g_fu_fr_made; g_fu_newsig += g_fu_fr_newsig; g_fu_tk += g_fu_fr_tk;
                    if (wall > 20.0) { g_fu_slow++; if (g_up_fr_n) g_fu_slow_tex++; }
                    if (fl) printf("nvn_vk_first: frame %lld: %lld pipelines drawn for the first time (%lld linked at the draw from ready stage libraries, %lld compiled or waited for, %lld in a pass signature never seen), %.3f ms making them | textures made or read at their use in this frame: %d (%.2f ms, %.0f KB) | %.1f ms since the previous present%s\n",
                                   (long long)frame, (long long)g_fu_fr_n, (long long)g_fu_fr_linked, (long long)g_fu_fr_made, (long long)g_fu_fr_newsig, (double)g_fu_fr_tk * k, g_up_fr_n, (double)g_up_fr_tk * k, (double)g_up_fr_bytes / 1024.0, wall, wall > 20.0 ? "  OVER 20 ms" : "");
                }
            }
            g_fu_fr_n = g_fu_fr_linked = g_fu_fr_made = g_fu_fr_tk = g_fu_fr_newsig = 0;
        }
        t_prev = t_now;
        g_cur_frame = frame + 1;
    }
    {   /* session 8: what this frame paid for textures made or read at their use (hold log), then the textures made ahead */
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        if (holdlog_on() && g_up_fr_tk * 1000 > 3 * fq.QuadPart) { static int nl; if (nl++ < 300) printf("nvn_vk_upl: frame %lld (%s): %d textures made or read at their use, %.1f MB, %.1f ms\n", (long long)frame, g_in_fight ? "fight" : "no fight", g_up_fr_n, (double)g_up_fr_bytes / 1048576.0, (double)g_up_fr_tk * 1000.0 / (double)fq.QuadPart); }
        g_up_fr_tk = g_up_fr_bytes = 0; g_up_fr_n = 0;
        tex_ahead(frame);
    }
    if (frame % 300 == 0) {                      /* where the render thread's time went, per frame, in the last 300 frames */
        static LONG64 last[12], last_sets, last_flush, last_reused, last_disp;
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double k = 1000.0 / (double)fq.QuadPart / 300.0;
        if (g_ts_on > 0) { static double ts_last; static LONG64 n_last; printf("nvn_vk_gpu: frames %lld..%lld: GPU time %.2f ms per frame (first to last command of %lld command buffers finished in the window)\n", (long long)frame - 300, (long long)frame, (g_ts_ns - ts_last) / 1e6 / 300.0, (long long)(g_ts_n - n_last)); ts_last = g_ts_ns; n_last = g_ts_n; }
        printf("nvn_vk_perf: frames %lld..%lld ms/frame: batch replay %.2f (of it: dispatch %.2f, draw preparation %.2f, pipelines %.2f, shader modules %.2f), wait for the GPU %.2f, copy-back %.2f (waiting at batch start %.2f), present %.2f | per frame: %.1f dispatches, %.0f descriptor sets allocated, %.0f reused, %.1f submissions | pipelines not made ahead so far: %lld | holds so far: %lld, %.1f ms, longest %.1f ms\n",
               (long long)frame - 300, (long long)frame, (double)(g_tm[3] - last[3]) * k, (double)(g_tm[2] - last[2]) * k, (double)(g_tm[8] - last[8]) * k, (double)(g_tm[5] - last[5]) * k, (double)(g_tm[6] - last[6]) * k,
               (double)(g_tm[0] - last[0]) * k, (double)(g_tm[1] - last[1]) * k, (double)(g_tm[7] - last[7]) * k, (double)(g_tm[4] - last[4]) * k, (double)(R.dispatches - last_disp) / 300.0, (double)(R.sets - last_sets) / 300.0, (double)(R.sets_reused - last_reused) / 300.0, (double)(R.flushes - last_flush) / 300.0, (long long)g_pipe_miss,
               (long long)(g_hold_n[0] + g_hold_n[1] + g_hold_n[2] + g_hold_n[3]), (double)(g_hold_tk[0] + g_hold_tk[1] + g_hold_tk[2] + g_hold_tk[3]) * k * 300.0, (double)g_hold_max * k * 300.0);
        if (g_tl_n) {
            double q = 1000.0 / (double)fq.QuadPart / (double)g_tl_n;
            printf("nvn_vk_timeline: frames %lld..%lld, %lld frames with fighters, ms after the start of the batch replay: first submission to the GPU %.2f, last (the frame is complete on the GPU's queue) %.2f, present job begins %.2f | per frame %.2f submissions between passes (SSBU_VK_SPLIT=%d) | the game's fence waits that had to wait: %.2f per frame, %.2f ms each, ending %.2f ms after the batch start\n",
                   (long long)frame - 300, (long long)frame, (long long)g_tl_n, (double)g_tl_sum[0] * q, (double)g_tl_sum[1] * q, (double)g_tl_sum[2] * q, (double)g_splits / (double)g_tl_n, g_split,
                   (double)g_tl_sum[3] / (double)g_tl_n, g_tl_sum[3] ? (double)g_tl_sum[4] * 1000.0 / (double)fq.QuadPart / (double)g_tl_sum[3] : 0.0, g_tl_sum[3] ? (double)g_tl_sum[5] * 1000.0 / (double)fq.QuadPart / (double)g_tl_sum[3] : 0.0);
            memset(g_tl_sum, 0, sizeof g_tl_sum); g_tl_n = 0; g_splits = 0;
        }
        if (g_cb_keep > 0) {
            static LONG64 lh, lf, lfb, ll[5], llb;
            printf("nvn_vk_cbkeep: frames %lld..%lld per frame: %.1f dispatch targets taken from a kept buffer (no copy), %.1f filled from guest memory (%.0f KB), landings: %.2f unused for two batches, %.2f read of an overlapping range, %.2f other extent, %.2f guest-memory command, %.2f the game wrote into the range (%.0f KB copied back; all landings so far %.1f ms of the render thread, the longest %.2f ms; so far %lld through a GPU copy, %lld of them made to finish early, %lld dropped unwritten; %lld ranges read by a draw or dispatch straight from a kept buffer) | kept now %d | write check %s: so far %lld takes of a range that is not write-watched, %lld words the game had changed at a landing (%lld also changed by compute) | SSBU_VK_CBKEEP=%d: trapped accesses so far: %lld reads, %lld writes (%lld of them by the render thread), %lld ranges that could not be protected\n",
                   (long long)frame - 300, (long long)frame, (double)(g_ck_hits - lh) / 300.0, (double)(g_ck_fills - lf) / 300.0, (double)(g_ck_fill_bytes - lfb) / 300.0 / 1024.0, (double)(g_ck_land[0] - ll[0]) / 300.0, (double)(g_ck_land[1] - ll[1]) / 300.0,
                   (double)(g_ck_land[2] - ll[2]) / 300.0, (double)(g_ck_land[3] - ll[3]) / 300.0, (double)(g_ck_land[4] - ll[4]) / 300.0, (double)(g_ck_land_bytes - llb) / 300.0 / 1024.0, (double)g_ck_land_tk * 1000.0 / (double)fq.QuadPart, (double)g_ck_land_max * 1000.0 / (double)fq.QuadPart, (long long)g_ck_async, (long long)g_ck_async_forced, (long long)g_ck_dropped, (long long)g_ck_drawn, g_cb_nkept, g_cb_ww > 0 ? "on" : "off", (long long)g_ck_unwatched, (long long)g_ck_gwords, (long long)g_ck_conflicts,
                   g_cb_keep, (long long)g_cg_reads, (long long)g_cg_writes, (long long)g_cg_render, (long long)g_ck_guard_fail);
            lh = g_ck_hits; lf = g_ck_fills; lfb = g_ck_fill_bytes; memcpy(ll, g_ck_land, sizeof ll); llb = g_ck_land_bytes;
            {   /* V 2026-10-21: landings INSIDE a batch (cb_sync with live buffers) by cause, and ranges put together on the GPU */
                static LONG64 lm[CBW_N], lmb, la, lab;
                LONG64 tot = 0, d[CBW_N];
                for (int w = 0; w < CBW_N; w++) { d[w] = g_ck_mid[w] - lm[w]; tot += d[w]; }
                if (tot || g_ck_asm != la)
                    printf("nvn_vk_cbmid: frames %lld..%lld per frame: %.2f landings of the batch's compute buffers INSIDE the batch (%.0f KB; read of an overlapping range %.2f, offset a descriptor cannot take %.2f, dispatch write over another buffer %.2f, guest-memory command %.2f, texture copy or read-back %.2f, 2048 dispatches %.2f, memory given back %.2f, other %.2f; so far %lld buffers copied by the GPU for them) | %.2f ranges put together on the GPU (%.0f KB) instead (SSBU_VK_CBASM=%d)\n",
                           (long long)frame - 300, (long long)frame, (double)tot / 300.0, (double)(g_ck_mid_bytes - lmb) / 300.0 / 1024.0, (double)d[CBW_READ] / 300.0, (double)d[CBW_ALIGN] / 300.0, (double)d[CBW_WRITE] / 300.0, (double)d[CBW_GCMD] / 300.0,
                           (double)d[CBW_TEX] / 300.0, (double)d[CBW_FULL] / 300.0, (double)d[CBW_RETIRE] / 300.0, (double)d[CBW_OTHER] / 300.0, (long long)g_ck_mid_gpu, (double)(g_ck_asm - la) / 300.0, (double)(g_ck_asm_bytes - lab) / 300.0 / 1024.0, g_cb_asm);
                memcpy(lm, g_ck_mid, sizeof lm); lmb = g_ck_mid_bytes; la = g_ck_asm; lab = g_ck_asm_bytes;
                if (g_mid_reported && g_cur_frame - g_mid_last_frame > 60) {
                    printf("nvn_vk_cbkeep: SLOW PATH ended: the last landing inside a batch was at frame %lld (%lld frames with one since frame %lld)\n", (long long)g_mid_last_frame, (long long)g_mid_run_n, (long long)g_mid_run_start);
                    g_mid_reported = 0;
                }
            }
        }
        {   /* Round 16: the long report every 3000 frames cost the present job 17.5-18.4 ms (runs p16t_bf_g, p16t_zt_g,
             * and the owner's logged sessions: frame 3001 late each time): it now waits for a frame without fighters.
             * SSBU_VK_REPORT_FIGHT=1: at the frame, as before. */
            static int due, infight = -1;
            if (infight < 0) infight = getenv("SSBU_VK_REPORT_FIGHT") && atoi(getenv("SSBU_VK_REPORT_FIGHT"));
            if (frame % 3000 == 0) due = 1;
            if (due && (infight || !g_in_fight)) { m4_report(); due = 0; }
        }
        memcpy(last, g_tm, sizeof last); last_sets = R.sets; last_flush = R.flushes; last_reused = R.sets_reused; last_disp = R.dispatches;
    }
    if (frame % 600 == 0)
        printf("nvn_vk: totals at frame %lld: %lld images made (%lld live, %lld views), %lld uploads (%lld MB), %lld shader modules, %lld pipelines, %lld descriptor sets, %lld command buffers, %lld ring wraps, %lld feedback passes, %lld MB device memory\n",
               (long long)frame, (long long)R.images, (long long)R.images_live, (long long)R.views, (long long)R.uploads, (long long)(R.upload_bytes >> 20), (long long)R.modules, (long long)R.pipelines, (long long)R.sets,
               (long long)R.flushes, (long long)R.ring_wraps, (long long)R.feedback, (long long)(g_mem_total >> 20));
    if (frame % 600 == 0 && R.dispatches) {
        LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        printf("nvn_vk: compute at frame %lld: %lld dispatches, %lld KB copied back to guest memory, draws using a vertex buffer written by compute in the same batch: %lld, copy-backs forced inside a batch: %lld, waits for the GPU before a copy-back: %lld (%.1f ms in all), %lld buffers made; words the guest wrote into a range pending copy-back: %lld (also written by compute: %lld)\n",
               (long long)frame, (long long)R.dispatches, (long long)(R.cb_bytes >> 10), (long long)R.cb_draws, (long long)R.cb_syncs, (long long)R.cb_waits, (double)R.cb_wait_ticks * 1000.0 / (double)fq.QuadPart, (long long)R.cb_made,
               (long long)R.cb_guest_words, (long long)R.cb_conflict_words);
    }
    if (frame % 600 == 0) { printf("nvn_vk: images made per NVN texture format (format:count):"); for (int i = 0; i < 256; i++) if (g_fmt_hist[i]) printf(" 0x%x:%u", i, g_fmt_hist[i]); printf("\n"); }
    R.frame_draws = R.frame_skipped = R.frame_clears = 0;
    {   /* Round 17: a present job that took long, in parts (default: 4 ms and more, at most 400 lines a run;
         * SSBU_VK_PRESENTLOG=<ms>, 0 = off). tp[3] is 0 when no swapchain image could be acquired. */
        static int lim = -1, nl;
        LONG64 t_end = tk();
        if (lim < 0) { const char *e = getenv("SSBU_VK_PRESENTLOG"); lim = e ? atoi(e) : 4; }
        if (lim > 0 && tp[3] && nl < 400) {
            LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
            double k = 1000.0 / (double)fq.QuadPart;
            if ((double)(t_end - t_present) * k >= (double)lim) {
                nl++;
                printf("nvn_vk_presentlong: frame %lld (%s): present job %.2f ms = barrier and first-run hold %.2f + pictures %.2f + the kept present of the frame before (queue-present) %.2f + acquire %.2f + blit recorded %.2f + its submission %.2f + defer or queue-present %.2f + front end frame end (frame marks, hitch table, log flush, list collect) %.2f + our reports and look-ahead %.2f\n",
                       (long long)frame, g_in_fight ? "fight" : "no fight", (double)(t_end - t_present) * k, (double)(tp[0] - t_present) * k, (double)(tp[1] - tp[0]) * k, (double)(tp[2] - tp[1]) * k,
                       (double)(tp[7] - tp[2]) * k, (double)(tp[3] - tp[7]) * k, (double)(tp[4] - tp[3]) * k, (double)(tp[5] - tp[4]) * k, (double)(tp[6] - tp[5]) * k, (double)(t_end - tp[6]) * k);
            }
        }
    }
}
/* L1 round 2 (pipe, test): save the picture of a window texture without presenting it (SSBU_SHOT_FIGHT with SSBU_L1_PIPE) */
void nvn_vk_shot_job(void *arg) {
    PresentJob *j = arg;
    Tex *t = j->win && j->index >= 0 && j->index < j->win->count ? j->win->tex[j->index] : NULL;
    VTex *vt = t && !t->dead ? t->vk : NULL;
    if (j->shot_fight && vt) { char path[300]; const char *dir = getenv("SSBU_SHOT_DIR"); snprintf(path, sizeof path, "%s%sfight_%05d.bmp", dir ? dir : "", dir ? "/" : "", j->shot_fight); save_bmp_vk(path, vt); printf("shot: fight frame %d saved from window texture %d (pipe, not presented again)%c", j->shot_fight, j->index, 10); }
}
const RBackend rb_vk = {"vk", vk_submit, vk_submit_async, vk_present, vk_present_movie, vk_fence, vk_sync_wait, vk_server_wait, vk_sync_drop, vk_flush, vk_finish, vk_write_texels, vk_read_texels, vk_housekeeping,
                        vk_reset_status, vk_no_grid, vk_idle};

/* ---------------------------------------------------------------- start and end */
static VTex *dummy_image(VkImageType type, uint32_t layers, VkFlags flags) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    VkClearColorValue z;
    VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vt->magic = 0x56544558; vt->fmt = VK_FORMAT_R8G8B8A8_UNORM; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT; vt->w = vt->h = vt->d = 1; vt->levels = 1; vt->layers = (int)layers; vt->samples = 1;
    ci.imageType = type; ci.format = vt->fmt; ci.extent.width = ci.extent.height = ci.extent.depth = 1; ci.mipLevels = 1; ci.arrayLayers = layers; ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.flags = flags;
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&z, 0, sizeof z);
    vkCmdClearColorImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
    vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    return vt;
}
static VkImageView dummy_view(VTex *vt, VkImageViewType type, uint32_t layers) {
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VView *n = calloc(1, sizeof *n);
    ci.image = vt->img; ci.viewType = type; ci.format = vt->fmt; ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ci.subresourceRange.levelCount = 1; ci.subresourceRange.layerCount = layers;
    VKC(vkCreateImageView(V.device, &ci, NULL, &n->view));
    n->vtype = 100 + (int)type; n->next = vt->views; vt->views = n;
    return n->view;
}
/* ---------------------------------------------------------------- M4: pipelines made before they are needed (design section 5, step 1)
 * At start the worker threads read ..\cache\vk\pipes.bin and make every pipeline listed there (stages from their records and
 * SPIR-V files, pipelines through the driver's pipeline cache kept in ..\cache\vk\pipelines_<gpu>.bin). The render thread
 * waits for them before it replays its first batch, so the tables are never shared with a running worker. */
#if 0   /* SUPERSEDED by nvn_vk_m4.c (session 3's start-up workers) */
static HANDLE g_pw_thread[16];
static int g_pw_nthread, g_pw_n, g_pw_waited;
static PipeRec *g_pw_rec;
static volatile LONG g_pw_next;
static LONG64 g_pw_t0, g_pw_t1;
static char g_pcache_path[160];
static VStage *pw_stage(int s, uint64_t h) {      /* g_sh_lock held */
    for (VStage *v = g_vstage[h & 4095]; v; v = v->next) if (v->h == h && v->stage == s) return v;
    VStage *v = vstage_from_rec(s, h);
    if (v) InterlockedIncrement64(&g_pw_stages);
    return v;
}
static DWORD WINAPI pw_main(void *arg) {
    (void)arg;
    for (;;) {
        LONG i = InterlockedIncrement(&g_pw_next) - 1;
        if (i >= g_pw_n) break;
        PipeRec *r = &g_pw_rec[i];
        EnterCriticalSection(&g_sh_lock);
        VStage *vs = pw_stage(r->compute ? 5 : 0, r->vs_h), *fs = r->fs_h ? pw_stage(1, r->fs_h) : NULL;
        LeaveCriticalSection(&g_sh_lock);
        if (!vs || (r->fs_h && !fs)) { InterlockedIncrement64(&g_pw_failed); continue; }      /* a stage record or SPIR-V file is gone: made at the draw as on a first run */
        if (r->compute) { compute_pipe_make(vs); InterlockedIncrement64(&g_pw_pipes); continue; }
        double ms;
        r->k.vs = vs->id; r->k.fs = fs ? fs->id : 0;
        Pipe *p = pipe_make(&r->k, vs, fs, &ms);
        uint64_t h = fnv(0xcbf29ce484222325ULL, &r->k, sizeof r->k);
        EnterCriticalSection(&g_sh_lock);
        p->next = g_pipes[h & 1023]; g_pipes[h & 1023] = p;
        LeaveCriticalSection(&g_sh_lock);
        InterlockedIncrement64(&g_pw_pipes);
    }
    InterlockedExchange64(&g_pw_t1, tk());
    return 0;
}
static int piperec_cmp(const void *a, const void *b) { return memcmp(a, b, sizeof(PipeRec)); }
static void prewarm_start(void) {
    InitializeCriticalSection(&g_sh_lock);
    {   /* the driver's pipeline cache, ours on disk: keyed by GPU and driver version (the driver checks its own header too) */
        VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        void *data = NULL;
        snprintf(g_pcache_path, sizeof g_pcache_path, "../cache/vk/pipelines_%04x_%04x_%08x.bin", (unsigned)V.props.vendorID, (unsigned)V.props.deviceID, (unsigned)V.props.driverVersion);
        FILE *fp = fopen(g_pcache_path, "rb");
        if (fp) { fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET); if (n > 0 && (data = malloc((size_t)n)) && fread(data, 1, (size_t)n, fp) == (size_t)n) { ci.initialDataSize = (size_t)n; ci.pInitialData = data; } fclose(fp); }
        if (vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; VKC(vkCreatePipelineCache(V.device, &ci, NULL, &g_pcache)); }
        printf("nvn_vk: pipeline cache %s: %llu KB read\n", g_pcache_path, (unsigned long long)(ci.initialDataSize >> 10));
        free(data);
    }
    if (getenv("SSBU_VK_PREWARM") && !atoi(getenv("SSBU_VK_PREWARM"))) { printf("nvn_vk: SSBU_VK_PREWARM=0: no pipeline is made ahead\n"); return; }
    FILE *fp = fopen("../cache/vk/pipes.bin", "rb");
    if (!fp) { printf("nvn_vk: no ../cache/vk/pipes.bin: a first run on this PC, every pipeline is made at the draw that first needs it\n"); return; }
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    g_pw_rec = malloc((size_t)n + 1);
    g_pw_n = (int)(fread(g_pw_rec, sizeof(PipeRec), (size_t)n / sizeof(PipeRec), fp));
    fclose(fp);
    int kept = 0;
    qsort(g_pw_rec, (size_t)g_pw_n, sizeof(PipeRec), piperec_cmp);
    for (int i = 0; i < g_pw_n; i++) if (g_pw_rec[i].magic == PIPEREC_MAGIC && (!kept || memcmp(&g_pw_rec[kept - 1], &g_pw_rec[i], sizeof(PipeRec)))) g_pw_rec[kept++] = g_pw_rec[i];
    g_pw_n = kept;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_pw_nthread = (int)si.dwNumberOfProcessors - 2;
    if (g_pw_nthread < 1) g_pw_nthread = 1;
    if (g_pw_nthread > 16) g_pw_nthread = 16;
    if (getenv("SSBU_VK_WORKERS") && atoi(getenv("SSBU_VK_WORKERS")) > 0 && atoi(getenv("SSBU_VK_WORKERS")) <= 16) g_pw_nthread = atoi(getenv("SSBU_VK_WORKERS"));
    g_pw_t0 = tk();
    for (int i = 0; i < g_pw_nthread; i++) g_pw_thread[i] = CreateThread(NULL, 0, pw_main, NULL, 0, NULL);
    printf("nvn_vk: %d pipelines listed in ../cache/vk/pipes.bin: %d worker threads make them now\n", g_pw_n, g_pw_nthread);
}
static void prewarm_wait(void) {                   /* render thread, before its first batch */
    if (g_pw_waited) return;
    g_pw_waited = 1;
    if (!g_pw_nthread) return;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    LONG64 t0 = tk();
    WaitForMultipleObjects((DWORD)g_pw_nthread, g_pw_thread, TRUE, INFINITE);
    for (int i = 0; i < g_pw_nthread; i++) CloseHandle(g_pw_thread[i]);
    printf("nvn_vk: pipelines made ahead: %lld of %d listed (%lld stages from their records, %lld entries without their stage files) in %.0f ms on %d threads; the first batch waited %.0f ms for them\n",
           (long long)g_pw_pipes, g_pw_n, (long long)g_pw_stages, (long long)g_pw_failed, (double)(g_pw_t1 - g_pw_t0) * 1000.0 / (double)fq.QuadPart, g_pw_nthread, (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart);
    fflush(stdout);
    free(g_pw_rec); g_pw_rec = NULL;
}
static void pcache_save(void) {
    size_t n = 0;
    if (!g_pcache || vkGetPipelineCacheData(V.device, g_pcache, &n, NULL) != VK_SUCCESS || !n) return;
    void *data = malloc(n);
    if (data && vkGetPipelineCacheData(V.device, g_pcache, &n, data) == VK_SUCCESS) {
        char tmp[176];
        snprintf(tmp, sizeof tmp, "%s.tmp", g_pcache_path);
        FILE *fp = fopen(tmp, "wb");
        if (fp) { fwrite(data, 1, n, fp); fclose(fp); MoveFileExA(tmp, g_pcache_path, MOVEFILE_REPLACE_EXISTING); printf("nvn_vk: pipeline cache saved: %llu KB\n", (unsigned long long)(n >> 10)); }
    }
    free(data);
}
#endif
#include "nvn_vk_wd.c"                             /* session 12: the freeze watchdog (wd_start / wd_stop) */
static void vkd_init(void) {
#define X(n) n = (PFN_##n)vkGetDeviceProcAddr(V.device, #n); if (!n) vkd_fail("the device has no " #n);
    VKD(X)
#undef X
    vkCmdSetVertexInputEXT = (PFN_vkCmdSetVertexInputEXT)vkGetDeviceProcAddr(V.device, "vkCmdSetVertexInputEXT");
    vkCmdBindVertexBuffers2 = (PFN_vkCmdBindVertexBuffers2)vkGetDeviceProcAddr(V.device, "vkCmdBindVertexBuffers2");
    if (!D.has_vids && !vkCmdBindVertexBuffers2) vkd_fail("the device has neither VK_EXT_vertex_input_dynamic_state nor vkCmdBindVertexBuffers2");
    if (D.has_push) { vkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(V.device, "vkCmdPushDescriptorSetKHR"); if (!vkCmdPushDescriptorSetKHR) D.has_push = 0; }
    if (D.has_fault) { vkGetDeviceFaultInfoEXT = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(V.device, "vkGetDeviceFaultInfoEXT"); D.has_fault = vkGetDeviceFaultInfoEXT != NULL; }
    if (D.has_ckpt) {
        vkCmdSetCheckpointNV = (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(V.device, "vkCmdSetCheckpointNV"); vkGetQueueCheckpointDataNV = (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(V.device, "vkGetQueueCheckpointDataNV");
        D.has_ckpt = vkCmdSetCheckpointNV && vkGetQueueCheckpointDataNV;
    }
    printf("nvn_vk: device loss report: fault information %s, a marker before most commands %s\n", D.has_fault ? "yes (VK_EXT_device_fault)" : "no", D.has_ckpt ? "YES (SSBU_VK_CHECKPOINTS=1)" : "no (SSBU_VK_CHECKPOINTS=1)");
    if (D.has_eds3) {
#define X3(n) n = (PFN_##n)vkGetDeviceProcAddr(V.device, #n); if (!n) vkd_fail("the device has VK_EXT_extended_dynamic_state3 but no " #n);
        X3(vkCmdSetColorBlendEnableEXT) X3(vkCmdSetColorBlendEquationEXT) X3(vkCmdSetColorWriteMaskEXT) X3(vkCmdSetPolygonModeEXT) X3(vkCmdSetAlphaToCoverageEnableEXT) X3(vkCmdSetSampleMaskEXT)
        if (D.f2.features.depthClamp) { X3(vkCmdSetDepthClampEnableEXT) }
#undef X3
    }
    printf("nvn_vk: descriptors: buffers (set 0) %s, textures (set 1) one set per distinct binding list per command buffer\n", D.has_push ? "pushed (VK_KHR_push_descriptor)" : "one allocated set per draw");
    vkGetPhysicalDeviceMemoryProperties(V.gpu, &D.mp);
    for (int i = 0; i < NSLOT; i++) {
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        ai.commandPool = V.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(V.device, &ai, &R.slot[i].cmd));
        VKC(vkCreateFence(V.device, &fi, NULL, &R.slot[i].fence));
    }
    R.ring_size = 256ull << 20;
    R.ring = vbuf_new(R.ring_size, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, &R.ring_mem);      /* TRANSFER_DST: cb_assemble */
    R.zero = vbuf_new(65536 + 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &R.zero_mem);
    memset(R.zero_mem.map, 0, 65536 + 64);
    { float f1 = 1.0f; uint32_t i1 = 1; memcpy((uint8_t *)R.zero_mem.map + 65536 + 12, &f1, 4); memcpy((uint8_t *)R.zero_mem.map + 65536 + 28, &i1, 4); }
    R.gen = 1; R.point_size = 1.0f; R.line_width = 1.0f; R.drange[1] = 1.0f; R.sample_mask = 0xffffffffu; R.chanmask = 0xffffffffu; R.logic = 3; R.poly[2] = 2; R.poly[1] = 1;
    R.svmask[0] = R.svmask[1] = R.swmask[0] = R.swmask[1] = 0xff; R.ds = 0x10 * 2;
    for (int i = 0; i < 8; i++) { R.blend[i][0] = 2; R.blend[i][1] = 1; R.blend[i][2] = 2; R.blend[i][3] = 1; R.blend[i][4] = 1; R.blend[i][5] = 1; }
    if (nvn_front_clip_modes() != (1 | 1 << 8)) nvn_vk_missing("a window origin other than upper left or a depth range other than 0..1 (nvnDeviceSetWindowOriginMode / SetDepthMode)");
    cmd_begin();
    g_dummy.d2 = dummy_image(VK_IMAGE_TYPE_2D, 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT); g_dummy.d1 = dummy_image(VK_IMAGE_TYPE_1D, 1, 0); g_dummy.d3 = dummy_image(VK_IMAGE_TYPE_3D, 1, 0);
    g_dummy.v[DIM_1D] = dummy_view(g_dummy.d1, VK_IMAGE_VIEW_TYPE_1D, 1); g_dummy.v[DIM_1DA] = dummy_view(g_dummy.d1, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1);
    g_dummy.v[DIM_2D] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_2D, 1); g_dummy.v[DIM_2DA] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1);
    g_dummy.v[DIM_CUBE] = dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_CUBE, 6); g_dummy.v[DIM_CUBEA] = D.f2.features.imageCubeArray ? dummy_view(g_dummy.d2, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6) : g_dummy.v[DIM_CUBE];
    g_dummy.v[DIM_3D] = dummy_view(g_dummy.d3, VK_IMAGE_VIEW_TYPE_3D, 1);
    {   /* a texture bound without a sampler: the texture's own OpenGL parameters (nearest, nearest mip, repeat) */
        VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        ci.maxLod = 1000.0f; ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        VKC(vkCreateSampler(V.device, &ci, NULL, &g_smp_default));
    }
    cmd_wait_all();
    CreateDirectoryA("../cache/vk", NULL);
#ifndef __APPLE__
    if (!D.has_vids) nvk_vl_load();                /* V_novids: the layout numbers of pipes_novi.bin, before the prewarm reads it */
#endif
    prewarm_start();
    /* PR #33, SSBU_VK_PREWARM_BENCH=1 (a measurement, tests\prewarm_bench.py): the prewarm of pipes.bin alone, read-only; the
     * process ends here, before the watchdog, the preparation and the game: no cache is written */
    if (getenv("SSBU_VK_PREWARM_BENCH") && atoi(getenv("SSBU_VK_PREWARM_BENCH"))) {
        prewarm_wait();
        printf("nvn_vk_prewarm_bench: complete; no game started, no cache saved\n");
        fflush(stdout); ExitProcess(0);
    }
    wd_start();
    printf("nvn_vk: drawing part ready: %d command buffers in flight, stream ring %llu MB, %u memory types; debug checks %s\n", NSLOT, (unsigned long long)(R.ring_size >> 20), (unsigned)D.mp.memoryTypeCount, D.debug ? "on" : "off (SSBU_VK_DEBUG=1)");
    if (RS_ON) printf("nvn_vk_scale: internal resolution scale %.3f (%s): screen-class targets are made at %d x %d instead of 1920 x 1080; fragment stages are another translation\n",
                      (double)rs_scale(), getenv("SSBU_VK_SCALE") ? "SSBU_VK_SCALE" : "graphics.ini", (int)(1920.0f * rs_scale() + 0.5f), (int)(1080.0f * rs_scale() + 0.5f));
    if (rs_scale() > 1.0f) {                       /* PR #8 review: above 1.0 the scaled targets take scale^2 times their memory; a failed allocation is fatal */
        VkDeviceSize heap = 0;
        for (uint32_t i = 0; i < D.mp.memoryHeapCount; i++) if ((D.mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && D.mp.memoryHeaps[i].size > heap) heap = D.mp.memoryHeaps[i].size;
        printf("nvn_vk_scale: supersampling (scale above 1.0, at most %.1f): the screen-class targets take %.1f times their memory at 1.0 (the device's largest local heap: %llu MB; what they took is printed at exit); a GPU with little memory may stop at an allocation\n",
               (double)RS_MAX, (double)(rs_scale() * rs_scale()), (unsigned long long)(heap >> 20));
    }
    fflush(stdout);
}
static int frstat_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
/* Session 9: battle frames only (dispatches and more than 120 draws: the fight and the result screen's fighters); the first 30 are left out (their buffers were recorded
 * while the stage was still being set up). GPU ms = sum of the spans of the frame's command buffers (SSBU_VK_GPUTIME=1). */
static void frstat_report(void) {
    static float g[FRSTAT_N], w[FRSTAT_N];
    int n = 0, seen = 0, slow = 0;
    double wsum = 0, gsum = 0;
    for (int f = 0; f < FRSTAT_N; f++) {
        if (!g_fr_fight[f] || seen++ < 30) continue;
        g[n] = g_fr_gpu[f]; w[n] = g_fr_wall[f]; wsum += w[n]; gsum += g[n]; if (w[n] > 33.4f) slow++; n++;
    }
    if (n < 100) return;
    qsort(g, (size_t)n, sizeof g[0], frstat_cmp); qsort(w, (size_t)n, sizeof w[0], frstat_cmp);
    printf("nvn_vk_gpu_fight: %d battle frames: GPU ms per frame median %.2f, p99 %.2f, max %.2f, mean %.2f%s | time between presents: median %.2f ms, p99 %.2f, max %.2f, %d frames over 33.4 ms | %.1f frames per second over the battle\n",
           n, (double)g[n / 2], (double)g[n * 99 / 100], (double)g[n - 1], gsum / n, g_ts_on > 0 ? "" : " (SSBU_VK_GPUTIME not set: no GPU times)",
           (double)w[n / 2], (double)w[n * 99 / 100], (double)w[n - 1], slow, wsum > 0 ? 1000.0 * n / wsum : 0.0);
    {   LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        printf("nvn_vk_first: %lld battle frames, %lld of them over 20 ms between presents | frames in which a pipeline was drawn for the first time: %lld (%lld pipelines: %lld linked at the draw from ready stage libraries, %lld compiled or waited for, %lld in a pass signature never seen; %.2f ms in all making them) | of those frames over 20 ms: %lld (%lld of them also made or read a texture at its use)\n",
               (long long)g_fu_fight_frames, (long long)g_fu_fight_slow, (long long)g_fu_frames, (long long)g_fu_total, (long long)g_fu_linked, (long long)g_fu_made, (long long)g_fu_newsig, (double)g_fu_tk * 1000.0 / (double)fq.QuadPart, (long long)g_fu_slow, (long long)g_fu_slow_tex);
    }
    if (g_pt_on > 0 && g_pt_frames) {             /* SSBU_VK_PASSTIME */
        double tot = 0;
        for (int k = 0; k < g_pt_nkind; k++) tot += g_pt_ns[k];
        printf("nvn_vk_pass: %lld frames with fighters, %.2f ms of GPU time per frame in marked segments; per kind (in the order of first use):\n", (long long)g_pt_frames, tot / 1e6 / (double)g_pt_frames);
        for (int k = 0; k < g_pt_nkind; k++) {
            PtKind *q = &g_pt_kind[k];
            char name[160];
            if (q->what == 1) snprintf(name, sizeof name, "compute dispatches");
            else if (q->what == 2) snprintf(name, sizeof name, "present blit to the window %dx%d (and what follows it)", q->w, q->h);
            else snprintf(name, sizeof name, "pass %dx%d, %d colour (format 0x%x)%s%s", q->w, q->h, q->ncol, q->cf, q->depth ? " + depth" : "", q->win ? ", WINDOW TEXTURE" : "");
            printf("nvn_vk_pass:   %6.3f ms per frame (%4.1f %%), %5.1f segments and %6.1f draws per frame: %s\n", g_pt_ns[k] / 1e6 / (double)g_pt_frames, tot > 0 ? 100.0 * g_pt_ns[k] / tot : 0.0, (double)g_pt_cnt[k] / (double)g_pt_frames, (double)g_pt_draws[k] / (double)g_pt_frames, name);
        }
    }
}
static void vkd_shutdown(void) {
    wd_stop();
    if (!R.ring) return;
    {   LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
        double k = 1000.0 / (double)fq.QuadPart;
        printf("nvn_vk_tex: textures made ahead of their first use: %lld (%.0f MB, %.0f ms in all, at most %.1f ms after one present); made or read at their use: %lld times, %.0f ms (of them in fight frames: %lld, %.0f ms)\n",
               (long long)g_ta_n, (double)g_ta_bytes / 1048576.0, (double)g_ta_tk * k, (double)g_ta_max_tk * k, (long long)g_late_n, (double)g_late_tk * k, (long long)g_late_fight_n, (double)g_late_fight_tk * k);
    }
    frstat_report();
    if (RS_ON && rs_ui_native()) printf("nvn_vk_scale: SSBU_VK_UI_NATIVE=1: the window's textures are kept at the game's size; passes with such a colour target and a SCALED depth target (drawn without depth): %lld\n", (long long)g_rs_mixed);
    if (RS_ON) printf("nvn_vk_scale: resolution scale %.3f: %lld images made scaled, %lld fragment stages translated in this run for it (%lld of them read the pixel position, %lld fetch texels by integer position), %lld uploads into and %lld read-backs from a scaled image, %lld copies / blits touching one\n",
                      (double)rs_scale(), (long long)g_rs_images, (long long)g_rs_stages, (long long)g_rs_fcs, (long long)g_rs_tf, (long long)g_rs_up, (long long)g_rs_down, (long long)g_rs_blits);
    if (RS_ON) printf("nvn_vk_scale: the scaled images made took %.0f MB of device memory in all\n", (double)g_rs_bytes / 1048576.0);      /* PR #8 review */
    if (RS_ON && rs_scale() > 1.0f) printf("nvn_vk_scale: supersampling: %lld 2:1 reduction steps at the present (rs_reduce, PR #8), %lld presents without them (format cannot be blitted linearly)\n", (long long)g_rs_reduce_steps, (long long)g_rs_reduce_skipped);
    m4_stop();
    pcache_save();
    if (g_pcache) { vkDestroyPipelineCache(V.device, g_pcache, NULL); g_pcache = NULL; }
    for (int i = 0; i < NSLOT; i++) slot_wait(&R.slot[i]);
    R.done = ~0ull; garbage_collect();
    while (g_vtex_all) { VTex *vt = g_vtex_all; if (vt->t) vt->t->vk = NULL; vt_destroy(vt); }
    for (int i = 0; i < 1024; i++) for (Pipe *p = g_pipes[i], *n; p; p = n) { n = p->next; vkDestroyPipeline(V.device, p->p, NULL); if (p->p_fast) vkDestroyPipeline(V.device, p->p_fast, NULL); if (!p->libbed) { vkDestroyPipelineLayout(V.device, p->layout, NULL); vkDestroyDescriptorSetLayout(V.device, p->dl[0], NULL); vkDestroyDescriptorSetLayout(V.device, p->dl[1], NULL); } free(p); }
    gpl_destroy();
    for (int i = 0; i < 8; i++) for (int k = 0; k < 2; k++) if (g_sem[i][k]) { vkDestroySemaphore(V.device, g_sem[i][k], NULL); g_sem[i][k] = NULL; }
    for (int i = 0; i < 16; i++) if (g_sem_img[i]) { vkDestroySemaphore(V.device, g_sem_img[i], NULL); g_sem_img[i] = NULL; }
    fl_shutdown();
    if (g_ovl.made) { vkDestroyImage(V.device, g_ovl.img, NULL); g_ovl.made = 0; }      /* the progress bar's image; its memory goes with the blocks */
    for (int i = 0; i < 256; i++) if (g_cb[i].buf) { vkDestroyBuffer(V.device, g_cb[i].buf, NULL); g_cb[i].buf = NULL; g_cb[i].state = 0; }
    g_cb_live = g_cb_pending = g_ncdisp = 0;
    for (int i = 0; i < 4096; i++) for (VStage *v = g_vstage[i], *n; v; v = n) {
        n = v->next;
        if (v->cp) { vkDestroyPipeline(V.device, v->cp, NULL); vkDestroyPipelineLayout(V.device, v->cl, NULL); vkDestroyDescriptorSetLayout(V.device, v->cdl[0], NULL); vkDestroyDescriptorSetLayout(V.device, v->cdl[1], NULL); }
        if (v->mod) vkDestroyShaderModule(V.device, v->mod, NULL);
        free(v);
    }
    for (int i = 0; i < g_nsmp; i++) vkDestroySampler(V.device, g_smp[i].s, NULL);
    vkDestroySampler(V.device, g_smp_default, NULL);
    if (g_ts_pool) { vkDestroyQueryPool(V.device, g_ts_pool, NULL); g_ts_pool = NULL; g_ts_on = 0; }
    for (int i = 0; i < NSLOT; i++) { for (int k = 0; k < R.slot[i].npool; k++) vkDestroyDescriptorPool(V.device, R.slot[i].pool[k], NULL); vkDestroyFence(V.device, R.slot[i].fence, NULL); }
    vkDestroyBuffer(V.device, R.ring, NULL); vkDestroyBuffer(V.device, R.zero, NULL);
    for (int b = 0; b < g_nblk; b++) if (g_blk[b]) { vkFreeMemory(V.device, g_blk[b]->mem, NULL); free(g_blk[b]); g_blk[b] = NULL; }
    R.ring = NULL;
    printf("nvn_vk: drawing part shut down: %lld images, %lld pipelines, %lld shader modules, %d samplers destroyed; %lld bytes of block space lost to fragmentation records\n", (long long)R.images, (long long)R.pipelines, (long long)R.modules, g_nsmp, (long long)g_mem_leak);
}
