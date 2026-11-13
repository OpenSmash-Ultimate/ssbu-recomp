/* U1: the settings menu's drawing (menu.cpp builds the Dear ImGui frame; plan\phases\U1_settings_menu.md). Included by
 * nvn_vk_draw.c: render thread only, uses that file's device (V), loader functions, memory and command helpers.
 *
 * overlay_draw calls menu_vk_draw with the swapchain image in TRANSFER_DST layout, inside the present's command buffer,
 * after the game's picture was blitted into it: TRANSFER_DST -> COLOR_ATTACHMENT, a dynamic-rendering pass that LOADS the
 * picture and draws the menu's triangles over it (alpha blending), COLOR_ATTACHMENT -> TRANSFER_DST again (the present
 * path then moves it to PRESENT_SRC as before). Nothing runs unless the menu is on (menu_enabled) and has something to draw.
 * One pipeline (made again when the swapchain's format changes: UNORM or SRGB; with SRGB the vertex colours are made linear
 * first), one font texture, one view per swapchain image (dropped by nvn_vk.c swapchain_destroy), vertex and index buffers
 * in a ring of NSLOT + 1 (a buffer is written again only after its command buffer has certainly finished). */
#include "menu.h"
#include "menu_spv.h"

#define MU_RING 8
#define vkQueueWaitIdle_() vkDeviceWaitIdle(V.device)      /* rare: a buffer grows, the format changes, the test picture is read */
static struct {
    int failed;
    VkFormat fmt;
    VkPipeline pipe; VkPipelineLayout layout; VkDescriptorSetLayout dsl; VkDescriptorPool dpool; VkDescriptorSet set;
    VkImage font; VMem font_mem; VkImageView font_view; VkSampler sampler; VkBuffer font_stage; VMem font_stage_mem; int font_ok;
    VkSwapchainKHR views_sc; VkImageView views[16];
    VkBuffer vb[MU_RING], ib[MU_RING]; VMem vbm[MU_RING], ibm[MU_RING]; uint64_t vcap[MU_RING], icap[MU_RING]; int ring;
    VkBuffer shot; VMem shot_mem; int shot_pending, shot_w, shot_h; char shot_path[300];
    LONG64 frames;
} g_mu;

static struct {
    VkImage img[2];
    VMem mem[2];
    VkImageView view[2];
    VkDescriptorSet set[2];
    VkSampler sampler;
    int init[2];
    int w, h, cur, ready;
    VkFormat fmt;
    int d4[4];
    LONG64 t_prev, t_cur, t_last_present;
    volatile LONG64 frames;
    float last_alpha;
    VkBuffer vb[MU_RING], ib;
    VMem vbm[MU_RING], ibm;
    int ring;
    VkCommandBuffer cmd[4];
    VkFence fence[4];
    int pending[4], slot;
} g_hfr;

static void hfr_wait_all(void) {
    for (int i = 0; i < 4; i++) if (g_hfr.pending[i] && g_hfr.fence[i]) {
        vkWaitForFences(V.device, 1, &g_hfr.fence[i], VK_TRUE, 5000000000ull);
        vkResetFences(V.device, 1, &g_hfr.fence[i]);
        g_hfr.pending[i] = 0;
    }
}

const char *nvn_vk_device_name(void) { return V.props.deviceName; }
static void menu_vk_views_drop(void) {             /* nvn_vk.c swapchain_destroy: the device is idle, the swapchain is about to go */
    hfr_wait_all();
    for (int i = 0; i < 16; i++) if (g_mu.views[i]) { vkDestroyImageView(V.device, g_mu.views[i], NULL); g_mu.views[i] = NULL; }
    g_mu.views_sc = NULL;
}
static VkShaderModule mu_module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    VkShaderModule m;
    ci.codeSize = bytes; ci.pCode = code;
    VKC(vkCreateShaderModule(V.device, &ci, NULL, &m));
    return m;
}
static void mu_pipeline(VkFormat fmt) {
    if (g_mu.pipe && g_mu.fmt == fmt) return;
    if (g_mu.pipe) { vkQueueWaitIdle_(); vkDestroyPipeline(V.device, g_mu.pipe, NULL); g_mu.pipe = NULL; }
    if (!g_mu.layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dl.bindingCount = 1; dl.pBindings = &b;
        VKC(vkCreateDescriptorSetLayout(V.device, &dl, NULL, &g_mu.dsl));
        VkPushConstantRange pc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 20};
        VkPipelineLayoutCreateInfo li = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &g_mu.dsl; li.pushConstantRangeCount = 1; li.pPushConstantRanges = &pc;
        VKC(vkCreatePipelineLayout(V.device, &li, NULL, &g_mu.layout));
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
        VkDescriptorPoolCreateInfo pi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 4; pi.poolSizeCount = 1; pi.pPoolSizes = &ps;
        VKC(vkCreateDescriptorPool(V.device, &pi, NULL, &g_mu.dpool));
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_mu.dpool; ai.descriptorSetCount = 1; ai.pSetLayouts = &g_mu.dsl;
        VKC(vkAllocateDescriptorSets(V.device, &ai, &g_mu.set));
    }
    VkShaderModule vs = mu_module(k_menu_vert_spv, sizeof k_menu_vert_spv), fs = mu_module(k_menu_frag_spv, sizeof k_menu_frag_spv);
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = fs; st[1].pName = "main";
    VkVertexInputBindingDescription vb = {0, 20, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[3] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 8}, {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &vb; vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = va;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba;
    memset(&ba, 0, sizeof ba);
    ba.blendEnable = VK_TRUE; ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rinfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rinfo.colorAttachmentCount = 1; rinfo.pColorAttachmentFormats = &fmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &rinfo; gi.stageCount = 2; gi.pStages = st; gi.pVertexInputState = &vi; gi.pInputAssemblyState = &ia; gi.pViewportState = &vp;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &ds; gi.pColorBlendState = &cb; gi.pDynamicState = &dy; gi.layout = g_mu.layout;
    VKC(vkCreateGraphicsPipelines(V.device, NULL, 1, &gi, NULL, &g_mu.pipe));
    vkDestroyShaderModule(V.device, vs, NULL);
    vkDestroyShaderModule(V.device, fs, NULL);
    g_mu.fmt = fmt;
    printf("menu: Vulkan pipeline made for swapchain format %d\n", (int)fmt);
}
static void mu_font(void) {                        /* the atlas, uploaded once inside the present's command buffer */
    if (g_mu.font_ok) return;
    int w = 0, h = 0;
    const unsigned char *px = menu_font_rgba(&w, &h);
    if (!px || w <= 0 || h <= 0) return;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = VK_FORMAT_R8G8B8A8_UNORM; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1;
    ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(V.device, &ci, NULL, &g_mu.font));
    vkGetImageMemoryRequirements(V.device, g_mu.font, &rq);
    g_mu.font_mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, g_mu.font, g_mu.font_mem.mem, g_mu.font_mem.off));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g_mu.font; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = ci.format; vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(V.device, &vi, NULL, &g_mu.font_view));
    VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR; si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod = 0.25f;
    VKC(vkCreateSampler(V.device, &si, NULL, &g_mu.sampler));
    g_mu.font_stage = vbuf_new((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_mu.font_stage_mem);
    memcpy(g_mu.font_stage_mem.map, px, (size_t)w * h * 4);
    image_barrier(g_mu.font, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy cp;
    memset(&cp, 0, sizeof cp);
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = (uint32_t)w; cp.imageExtent.height = (uint32_t)h; cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(R.cmd, g_mu.font_stage, g_mu.font, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    image_barrier(g_mu.font, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii = {g_mu.sampler, g_mu.font_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = g_mu.set; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
    vkUpdateDescriptorSets(V.device, 1, &wr, 0, NULL);
    g_mu.font_ok = 1;
    printf("menu: font atlas %dx%d uploaded\n", w, h);
}
static VkImageView mu_view(uint32_t index) {
    if (g_mu.views_sc != V.swapchain) { for (int i = 0; i < 16; i++) if (g_mu.views[i]) { vkDestroyImageView(V.device, g_mu.views[i], NULL); g_mu.views[i] = NULL; } g_mu.views_sc = V.swapchain; }
    if (index >= 16) return NULL;
    if (!g_mu.views[index]) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = V.images[index]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = V.format.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        VKC(vkCreateImageView(V.device, &vi, NULL, &g_mu.views[index]));
    }
    return g_mu.views[index];
}
static void mu_buf(VkBuffer *b, VMem *m, uint64_t *cap, uint64_t need, VkFlags usage) {
    if (*b && *cap >= need) return;
    if (*b) { vkQueueWaitIdle_(); vkDestroyBuffer(V.device, *b, NULL); vmem_free(*m); }
    *cap = need < (1u << 20) ? (1u << 20) : need * 2;      /* 1 MB: about 50,000 vertices, more than any page draws: no growth (and no device wait) in a fight */
    *b = vbuf_new(*cap, usage, m);
}
/* SSBU_MENU_TEST: the swapchain image of the previous frame, read back, as a BMP (32 bits, top-down) */
static void mu_shot_write(void) {
    if (g_mu.shot_pending != 1) return;
    g_mu.shot_pending = 0;
    vkQueueWaitIdle_();
    int w = g_mu.shot_w, h = g_mu.shot_h, ok = 0;
    FILE *f = fopen(g_mu.shot_path, "wb");
    if (f) {
        unsigned char hd[54];
        uint32_t img = (uint32_t)w * (uint32_t)h * 4, file = 54 + img, off = 54, hs = 40, wd = (uint32_t)w, ht = (uint32_t)-h, planes_bpp = 1 | 32 << 16, z = 0;
        memset(hd, 0, sizeof hd);
        hd[0] = 'B'; hd[1] = 'M'; memcpy(hd + 2, &file, 4); memcpy(hd + 10, &off, 4); memcpy(hd + 14, &hs, 4); memcpy(hd + 18, &wd, 4); memcpy(hd + 22, &ht, 4);
        memcpy(hd + 26, &planes_bpp, 4); memcpy(hd + 30, &z, 4); memcpy(hd + 34, &img, 4);
        ok = fwrite(hd, 1, 54, f) == 54 && fwrite(g_mu.shot_mem.map, 1, img, f) == img;
        ok = !fclose(f) && ok;
    }
    vkDestroyBuffer(V.device, g_mu.shot, NULL); vmem_free(g_mu.shot_mem); g_mu.shot = NULL;
    menu_shot_done(g_mu.shot_path, w, h, ok);
}
static void menu_vk_draw(uint32_t index) {
    if (!menu_enabled() || g_mu.failed) return;
    mu_shot_write();
    MenuDraw d;
    if (!menu_frame((int)V.extent.width, (int)V.extent.height, &d) || !d.nidx || !d.nvtx) return;
    VkFormat fmt = V.format.format;
    mu_pipeline(fmt);
    mu_font();
    VkImageView view = mu_view(index);
    if (!g_mu.font_ok || !view) return;
    int r = g_mu.ring = (g_mu.ring + 1) % MU_RING;
    mu_buf(&g_mu.vb[r], &g_mu.vbm[r], &g_mu.vcap[r], (uint64_t)d.nvtx * 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    mu_buf(&g_mu.ib[r], &g_mu.ibm[r], &g_mu.icap[r], (uint64_t)d.nidx * 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    memcpy(g_mu.vbm[r].map, d.vtx, (size_t)d.nvtx * 20);
    memcpy(g_mu.ibm[r].map, d.idx, (size_t)d.nidx * 2);
    int W = (int)V.extent.width, H = (int)V.extent.height;
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = V.extent; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    CKPT_OP("settings menu pass (swapchain image, index)", V.images[index], index, W, H);
    vkCmdBeginRendering(R.cmd, &ri);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mu.pipe);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mu.layout, 0, 1, &g_mu.set, 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(R.cmd, 0, 1, &g_mu.vb[r], &zero);
    vkCmdBindIndexBuffer(R.cmd, g_mu.ib[r], 0, VK_INDEX_TYPE_UINT16);
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    vkCmdSetViewport(R.cmd, 0, 1, &vp);
    float pc[5] = {2.0f / d.disp_w, 2.0f / d.disp_h, -1.0f, -1.0f, fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_R8G8B8A8_SRGB ? 1.0f : 0.0f};
    vkCmdPushConstants(R.cmd, g_mu.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    for (int i = 0; i < d.ncmd; i++) {
        const MenuCmd *c = &d.cmd[i];
        float x0 = c->clip[0] < 0 ? 0 : c->clip[0], y0 = c->clip[1] < 0 ? 0 : c->clip[1];
        float x1 = c->clip[2] > (float)W ? (float)W : c->clip[2], y1 = c->clip[3] > (float)H ? (float)H : c->clip[3];
        if (x1 <= x0 || y1 <= y0) continue;
        VkRect2D sc = {{(int32_t)x0, (int32_t)y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
        vkCmdSetScissor(R.cmd, 0, 1, &sc);
        vkCmdDrawIndexed(R.cmd, c->count, 1, c->idx_off, (int32_t)c->vtx_off, 0);
    }
    vkCmdEndRendering(R.cmd);
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    R.pipe_bound = NULL; R.dyn_dirty = R.vi_dirty = 1; R.dyn3_dirty = 1;      /* the game's state tracking: nothing of ours stays bound for it */
    if (!g_mu.shot_pending && menu_shot_wanted(g_mu.shot_path, sizeof g_mu.shot_path)) {
        if (!g_mu_shot_src) { printf("menu: test: the swapchain cannot be read back (no TRANSFER_SRC usage)\n"); menu_shot_done(g_mu.shot_path, W, H, 0); }
        else {
            g_mu.shot = vbuf_new((uint64_t)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_mu.shot_mem);
            image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy cp;
            memset(&cp, 0, sizeof cp);
            cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; cp.imageSubresource.layerCount = 1;
            cp.imageExtent.width = (uint32_t)W; cp.imageExtent.height = (uint32_t)H; cp.imageExtent.depth = 1;
            vkCmdCopyImageToBuffer(R.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_mu.shot, 1, &cp);
            image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            g_mu.shot_w = W; g_mu.shot_h = H; g_mu.shot_pending = 1;
        }
    }
    g_mu.frames++;
}

static void hfr_ensure(int w, int h, VkFormat fmt) {
    if (w <= 0 || h <= 0) return;
    mu_pipeline(fmt);
    if (!g_hfr.sampler) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.25f;
        VKC(vkCreateSampler(V.device, &si, NULL, &g_hfr.sampler));
    }
    if (!g_hfr.ib) {
        static const uint16_t k_idx[6] = {0, 1, 2, 0, 2, 3};
        g_hfr.ib = vbuf_new(64, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &g_hfr.ibm);
        memcpy(g_hfr.ibm.map, k_idx, sizeof k_idx);
        for (int i = 0; i < MU_RING; i++) g_hfr.vb[i] = vbuf_new(256, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &g_hfr.vbm[i]);
    }
    if (!g_hfr.set[0]) {
        VkDescriptorSetLayout dsls[2] = {g_mu.dsl, g_mu.dsl};
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = g_mu.dpool; ai.descriptorSetCount = 2; ai.pSetLayouts = dsls;
        VKC(vkAllocateDescriptorSets(V.device, &ai, g_hfr.set));
    }
    if (g_hfr.w == w && g_hfr.h == h && g_hfr.fmt == fmt && g_hfr.img[0] && g_hfr.img[1]) return;
    vkQueueWaitIdle_();
    for (int i = 0; i < 2; i++) {
        if (g_hfr.view[i]) { vkDestroyImageView(V.device, g_hfr.view[i], NULL); g_hfr.view[i] = NULL; }
        if (g_hfr.img[i]) { vkDestroyImage(V.device, g_hfr.img[i], NULL); vmem_free(g_hfr.mem[i]); g_hfr.img[i] = NULL; }
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        VkMemoryRequirements rq;
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = fmt;
        ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1;
        ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VKC(vkCreateImage(V.device, &ci, NULL, &g_hfr.img[i]));
        vkGetImageMemoryRequirements(V.device, g_hfr.img[i], &rq);
        g_hfr.mem[i] = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        VKC(vkBindImageMemory(V.device, g_hfr.img[i], g_hfr.mem[i].mem, g_hfr.mem[i].off));
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = g_hfr.img[i]; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; vi.subresourceRange.levelCount = 1; vi.subresourceRange.layerCount = 1;
        VKC(vkCreateImageView(V.device, &vi, NULL, &g_hfr.view[i]));
        VkDescriptorImageInfo ii = {g_hfr.sampler, g_hfr.view[i], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet wr = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr.dstSet = g_hfr.set[i]; wr.descriptorCount = 1; wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &ii;
        vkUpdateDescriptorSets(V.device, 1, &wr, 0, NULL);
        g_hfr.init[i] = 0;
    }
    g_hfr.w = w; g_hfr.h = h; g_hfr.fmt = fmt; g_hfr.ready = 0;
    printf("nvn_vk_hfr: 144 Hz+ frame history textures %dx%d (format %d) allocated\n", w, h, (int)fmt);
    fflush(stdout);
}

static void hfr_capture(VTex *src, const int s4[4], const int d4[4]) {
    if (!src || d4[2] <= 0 || d4[3] <= 0) return;
    hfr_ensure(d4[2], d4[3], V.format.format);
    int c = g_hfr.cur ^= 1;
    for (int pass = 0; pass < 2; pass++) {
        int dst = pass == 0 ? c : (c ^ 1);
        if (pass == 1 && g_hfr.init[dst]) break;
        image_barrier(g_hfr.img[dst], VK_IMAGE_ASPECT_COLOR_BIT, g_hfr.init[dst] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit b;
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
        b.srcOffsets[0].x = s4[0]; b.srcOffsets[0].y = s4[1]; b.srcOffsets[1].x = s4[0] + s4[2]; b.srcOffsets[1].y = s4[1] + s4[3]; b.srcOffsets[1].z = 1;
        b.dstOffsets[1].x = g_hfr.w; b.dstOffsets[1].y = g_hfr.h; b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_hfr.img[dst], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        image_barrier(g_hfr.img[dst], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        g_hfr.init[dst] = 1;
    }
    g_hfr.t_prev = g_hfr.t_cur;
    g_hfr.t_cur = tk();
    g_hfr.last_alpha = 0.0f;
    memcpy(g_hfr.d4, d4, sizeof g_hfr.d4);
    g_hfr.ready = 1;
}

static void hfr_blit_or_blend(uint32_t index, const int d4[4], float alpha) {
    int cur = g_hfr.cur, prev = cur ^ 1;
    int base = alpha >= 0.99f ? cur : prev;
    VkImageBlit b;
    memset(&b, 0, sizeof b);
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = 1; b.dstSubresource = b.srcSubresource;
    b.srcOffsets[1].x = g_hfr.w; b.srcOffsets[1].y = g_hfr.h; b.srcOffsets[1].z = 1;
    b.dstOffsets[0].x = d4[0]; b.dstOffsets[0].y = d4[1]; b.dstOffsets[1].x = d4[0] + d4[2]; b.dstOffsets[1].y = d4[1] + d4[3]; b.dstOffsets[1].z = 1;
    vkCmdBlitImage(R.cmd, g_hfr.img[base], VK_IMAGE_LAYOUT_GENERAL, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    if (alpha <= 0.01f || alpha >= 0.99f) return;
    VkImageView view = mu_view(index);
    if (!view || !g_mu.pipe) return;
    int W = (int)V.extent.width, H = (int)V.extent.height;
    int r = g_hfr.ring = (g_hfr.ring + 1) % MU_RING;
    uint32_t a8 = (uint32_t)(alpha * 255.0f + 0.5f);
    if (a8 > 255) a8 = 255;
    uint32_t col = 0x00FFFFFFu | (a8 << 24);
    float x0 = (float)d4[0], y0 = (float)d4[1], x1 = (float)(d4[0] + d4[2]), y1 = (float)(d4[1] + d4[3]);
    struct { float x, y, u, v; uint32_t c; } vtx[4] = {
        {x0, y0, 0.0f, 0.0f, col},
        {x1, y0, 1.0f, 0.0f, col},
        {x1, y1, 1.0f, 1.0f, col},
        {x0, y1, 0.0f, 1.0f, col}
    };
    memcpy(g_hfr.vbm[r].map, vtx, sizeof vtx);
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent = V.extent; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca;
    vkCmdBeginRendering(R.cmd, &ri);
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mu.pipe);
    vkCmdBindDescriptorSets(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mu.layout, 0, 1, &g_hfr.set[cur], 0, NULL);
    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(R.cmd, 0, 1, &g_hfr.vb[r], &zero);
    vkCmdBindIndexBuffer(R.cmd, g_hfr.ib, 0, VK_INDEX_TYPE_UINT16);
    VkViewport vp = {0, 0, (float)W, (float)H, 0, 1};
    vkCmdSetViewport(R.cmd, 0, 1, &vp);
    VkRect2D sc = {{(int32_t)d4[0], (int32_t)d4[1]}, {(uint32_t)d4[2], (uint32_t)d4[3]}};
    vkCmdSetScissor(R.cmd, 0, 1, &sc);
    float pc[5] = {2.0f / (float)W, 2.0f / (float)H, -1.0f, -1.0f, 0.0f};
    vkCmdPushConstants(R.cmd, g_mu.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof pc, pc);
    vkCmdDrawIndexed(R.cmd, 6, 1, 0, 0, 0);
    vkCmdEndRendering(R.cmd);
    image_barrier(V.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    R.pipe_bound = NULL; R.dyn_dirty = R.vi_dirty = 1; R.dyn3_dirty = 1;
}
