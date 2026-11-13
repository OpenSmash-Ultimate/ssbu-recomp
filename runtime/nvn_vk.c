/* NVN on Vulkan: the second back end of the graphics layer (design, milestones, state: plan\phases\V1_vulkan.md).
 *
 * Selected with SSBU_RENDERER=vk (default: gl). The front end (NVN entry points, command lists, the job queue and its
 * render thread) is in nvn_gl.c; in Vulkan mode that render thread calls nvn_vk_init() instead of creating an OpenGL
 * context and gives its jobs to the functions below (nvn_gl.c, vk_route: seam step S0).
 *
 * What exists (milestone M1): instance, device selection (discrete GPU preferred), a swapchain on the game window,
 * clear + present, paced by the one 60 Hz clock of display.c, a clean shutdown. Everything else stops the game with
 * the name of the operation (nvn_vk_missing), as nvn_stub.c does for a missing NVN function.
 *
 * Environment:
 *   SSBU_RENDERER=vk          use this back end
 *   SSBU_VK_SELFTEST=<n>      present n cleared frames on the display clock, print the timing, shut down, exit 0
 *                             (exit 1 when a Vulkan call failed)
 *   SSBU_VK_VALIDATE=1        enable VK_LAYER_KHRONOS_validation and print its messages (says so if not installed)
 *   SSBU_VK_DEVICE=<i>        take physical device i of the printed list instead of the preferred one
 *   SSBU_VK_PRESENT=mailbox|immediate|fifo   present mode (default: mailbox, else immediate, else fifo: the pacing is ours)
 *   SSBU_VK_MISSING=warn      development only: an operation that is not implemented is reported once, counted and
 *                             skipped instead of stopping the game (the picture is then NOT the game's)
 *
 * Vulkan declarations: vk_min.h (see there why it is not the Khronos header). The loader is vulkan-1.dll. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
/* Session 2: the Khronos headers (reference\upstream\Vulkan-Headers\include, -I in build.py) replace our own vk_min.h subset. */
#ifdef _WIN32
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#define VK_NO_PROTOTYPES
#ifdef __APPLE__
#define VK_ENABLE_BETA_EXTENSIONS                       /* VK_KHR_portability_subset (MoltenVK) is declared in vulkan_beta.h */
#endif
#include <vulkan/vulkan.h>
#ifndef _WIN32
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>                         /* Linux build: the window is an SDL window; SDL makes the surface */
#endif
#include "nvn_front.h"
#include "gfx_options.h"
#define P(a) ((void *)(uintptr_t)(a))                     /* guest addresses are host addresses (as in nvn_gl.c) */
static void vkd_device_info(VkDeviceCreateInfo *di, const VkExtensionProperties *dext, uint32_t ndext);     /* nvn_vk_draw.c (included at the end) */
static void vkd_init(void);
static void vkd_shutdown(void);

typedef char vk_min_check_properties[sizeof(VkPhysicalDeviceProperties) == 824 ? 1 : -1];
typedef char vk_min_check_caps[sizeof(VkSurfaceCapabilitiesKHR) == 52 ? 1 : -1];
typedef char vk_min_check_swapchain[sizeof(VkSwapchainCreateInfoKHR) == 104 ? 1 : -1];
typedef char vk_min_check_barrier[sizeof(VkImageMemoryBarrier) == 72 ? 1 : -1];
typedef char vk_min_check_submit[sizeof(VkSubmitInfo) == 72 ? 1 : -1];

void nvn_render_run(void (*fn)(void *), void *arg);       /* nvn_gl.c: run on the render thread and wait */
void display_pace(void);                                  /* display.c */
int display_wait_refresh(void);
void display_before_present(void);
int display_hfr_mode(void);

/* ---------------------------------------------------------------- the loader's functions */
#ifdef _WIN32
#define VKF_SURFACE(X) X(VkResult, vkCreateWin32SurfaceKHR, (VkInstance, const VkWin32SurfaceCreateInfoKHR *, const VkAllocationCallbacks *, VkSurfaceKHR *))
#else
#define VKF_SURFACE(X)
#endif
#define VKF(X) \
    X(VkResult, vkEnumerateInstanceLayerProperties, (uint32_t *, VkLayerProperties *)) \
    X(VkResult, vkEnumerateInstanceExtensionProperties, (const char *, uint32_t *, VkExtensionProperties *)) \
    X(VkResult, vkCreateInstance, (const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *)) \
    X(void, vkDestroyInstance, (VkInstance, const VkAllocationCallbacks *)) \
    X(PFN_vkVoidFunction, vkGetInstanceProcAddr, (VkInstance, const char *)) \
    X(VkResult, vkEnumeratePhysicalDevices, (VkInstance, uint32_t *, VkPhysicalDevice *)) \
    X(void, vkGetPhysicalDeviceProperties, (VkPhysicalDevice, VkPhysicalDeviceProperties *)) \
    X(void, vkGetPhysicalDeviceQueueFamilyProperties, (VkPhysicalDevice, uint32_t *, VkQueueFamilyProperties *)) \
    X(VkResult, vkEnumerateDeviceExtensionProperties, (VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *)) \
    VKF_SURFACE(X) \
    X(void, vkDestroySurfaceKHR, (VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceSupportKHR, (VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceCapabilitiesKHR, (VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *)) \
    X(VkResult, vkGetPhysicalDeviceSurfaceFormatsKHR, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *)) \
    X(VkResult, vkGetPhysicalDeviceSurfacePresentModesKHR, (VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkPresentModeKHR *)) \
    X(VkResult, vkCreateDevice, (VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *)) \
    X(void, vkDestroyDevice, (VkDevice, const VkAllocationCallbacks *)) \
    X(void, vkGetDeviceQueue, (VkDevice, uint32_t, uint32_t, VkQueue *)) \
    X(VkResult, vkDeviceWaitIdle, (VkDevice)) \
    X(VkResult, vkCreateSwapchainKHR, (VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *)) \
    X(void, vkDestroySwapchainKHR, (VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *)) \
    X(VkResult, vkGetSwapchainImagesKHR, (VkDevice, VkSwapchainKHR, uint32_t *, VkImage *)) \
    X(VkResult, vkAcquireNextImageKHR, (VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *)) \
    X(VkResult, vkQueuePresentKHR, (VkQueue, const VkPresentInfoKHR *)) \
    X(VkResult, vkQueueSubmit, (VkQueue, uint32_t, const VkSubmitInfo *, VkFence)) \
    X(VkResult, vkCreateCommandPool, (VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *)) \
    X(void, vkDestroyCommandPool, (VkDevice, VkCommandPool, const VkAllocationCallbacks *)) \
    X(VkResult, vkAllocateCommandBuffers, (VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *)) \
    X(VkResult, vkBeginCommandBuffer, (VkCommandBuffer, const VkCommandBufferBeginInfo *)) \
    X(VkResult, vkEndCommandBuffer, (VkCommandBuffer)) \
    X(VkResult, vkResetCommandBuffer, (VkCommandBuffer, VkFlags)) \
    X(void, vkCmdPipelineBarrier, (VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t, const void *, uint32_t, const void *, uint32_t, const VkImageMemoryBarrier *)) \
    X(void, vkCmdClearColorImage, (VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *)) \
    X(void, vkCmdCopyImage, (VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t, const VkImageCopy *)) \
    X(VkResult, vkCreateSemaphore, (VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *, VkSemaphore *)) \
    X(void, vkDestroySemaphore, (VkDevice, VkSemaphore, const VkAllocationCallbacks *)) \
    X(VkResult, vkCreateFence, (VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *)) \
    X(void, vkDestroyFence, (VkDevice, VkFence, const VkAllocationCallbacks *)) \
    X(VkResult, vkWaitForFences, (VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t)) \
    X(VkResult, vkResetFences, (VkDevice, uint32_t, const VkFence *))
#define X(ret, name, args) static ret (VKAPI_PTR *name) args;
VKF(X)
#undef X
static VkResult (VKAPI_PTR *vkEnumerateInstanceVersion)(uint32_t *);          /* absent from a Vulkan 1.0 loader */
static VkResult (VKAPI_PTR *vkCreateDebugUtilsMessengerEXT)(VkInstance, const VkDebugUtilsMessengerCreateInfoEXT *, const VkAllocationCallbacks *, VkDebugUtilsMessengerEXT *);
static void (VKAPI_PTR *vkDestroyDebugUtilsMessengerEXT)(VkInstance, VkDebugUtilsMessengerEXT, const VkAllocationCallbacks *);

/* ---------------------------------------------------------------- state (render thread only, except the counters) */
static struct {
    HWND hwnd;
    HMODULE loader;
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice gpu;
    VkPhysicalDeviceProperties props;
    uint32_t family;
    VkDevice device;
    VkQueue queue;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkSurfaceFormatKHR format;
    VkPresentModeKHR mode;
    VkExtent2D extent;
    VkImage images[16];
    uint32_t image_count;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkSemaphore acquired, rendered;
    VkFence fence;
    int fence_pending, alive, validate;
} V;
static volatile LONG64 g_vk_frames, g_vk_recreated, g_vk_skipped, g_vk_errors, g_vk_messages, g_vk_missing_calls;
static int g_vk_missing_warn = -1;

static const char *vk_result_name(VkResult r) {
    static char buf[32];
    switch (r) {
    case VK_SUCCESS: return "VK_SUCCESS"; case VK_NOT_READY: return "VK_NOT_READY"; case VK_TIMEOUT: return "VK_TIMEOUT"; case VK_INCOMPLETE: return "VK_INCOMPLETE";
    case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR"; case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR"; case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
    case -1: return "VK_ERROR_OUT_OF_HOST_MEMORY"; case -2: return "VK_ERROR_OUT_OF_DEVICE_MEMORY"; case -3: return "VK_ERROR_INITIALIZATION_FAILED"; case -4: return "VK_ERROR_DEVICE_LOST";
    case -6: return "VK_ERROR_LAYER_NOT_PRESENT"; case -7: return "VK_ERROR_EXTENSION_NOT_PRESENT"; case -8: return "VK_ERROR_FEATURE_NOT_PRESENT"; case -9: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    }
    snprintf(buf, sizeof buf, "VkResult %d", (int)r);
    return buf;
}
static void vkd_lost_report(void);                                    /* nvn_vk_draw.c (PR #32): what the driver and the markers say about the loss */
static void vk_unavailable(void);
static void vk_fatal(const char *what, VkResult r) {
    printf("nvn_vk: %s failed: %s\n", what, vk_result_name(r));
    fflush(stdout);
    if (r == VK_ERROR_DEVICE_LOST && V.device) vkd_lost_report();
    /* (2026-10-21, tester: Quadro M1200, the device lost at the 4th frame, every start) a device lost in the first 20 s: this
     * driver cannot run the game's Vulkan; graphics.ini gets renderer = opengl (F1 > Graphics switches back) and the game
     * starts again with OpenGL (vk_unavailable) */
    if (r == VK_ERROR_DEVICE_LOST && nvn_front_frame_next() < 1200 && !getenv("SSBU_RENDERER")) {
        printf("nvn_vk: the device was lost at frame %lld, at the start: graphics.ini now says renderer = opengl\n", nvn_front_frame_next());
        fflush(stdout);
        gfx_ini_set("renderer", "opengl");
        vk_unavailable();
    }
    ExitProcess(5);
}
/* A call that may fail while running (present path): counted, reported a few times, the frame goes on. */
static int vk_ok(const char *what, VkResult r) {
    if (r == VK_SUCCESS) return 1;
    if (InterlockedIncrement64(&g_vk_errors) <= 20) { printf("nvn_vk: %s: %s\n", what, vk_result_name(r)); fflush(stdout); }
    return 0;
}

/* An NVN operation this back end does not implement yet: named, and the game stops (exit code 7). */
void nvn_vk_missing(const char *what) {
    static const char *seen[64];
    static int nseen;
    if (g_vk_missing_warn < 0) { const char *e = getenv("SSBU_VK_MISSING"); g_vk_missing_warn = e && !strcmp(e, "warn"); }
    InterlockedIncrement64(&g_vk_missing_calls);
    if (!g_vk_missing_warn) {
        printf("nvn_vk: NOT IMPLEMENTED in the Vulkan back end: %s. The game stops here (plan\\phases\\V1_vulkan.md; SSBU_RENDERER=gl is the working renderer).\n", what);
        fflush(stdout);
        ExitProcess(7);
    }
    for (int i = 0; i < nseen; i++) if (seen[i] == what) return;
    if (nseen < 64) seen[nseen++] = what;
    printf("nvn_vk: NOT IMPLEMENTED (SSBU_VK_MISSING=warn: skipped, the picture is not the game's): %s\n", what);
    fflush(stdout);
}

static VkBool32 VKAPI_PTR vk_debug_cb(VkFlags severity, VkFlags types, const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types; (void)user;
    if (InterlockedIncrement64(&g_vk_messages) <= 200)
        printf("vk-validation: %s: %s: %s\n", severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", data && data->pMessageIdName ? data->pMessageIdName : "?",
               data && data->pMessage ? data->pMessage : "");
    fflush(stdout);
    return VK_FALSE;
}

static int has_name(const VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

/* ---------------------------------------------------------------- swapchain */
static void wait_last_submit(void);
static void menu_vk_views_drop(void);                  /* U1 (nvn_vk_menu.c): its views of the swapchain images */
static int g_mu_shot_src;                               /* U1: the swapchain images can be read back (SSBU_MENU_TEST only) */
static void swapchain_destroy(void) {
    if (!V.swapchain) return;
    wait_last_submit();                                  /* also resets the fence for the next submit */
    vkDeviceWaitIdle(V.device);
    menu_vk_views_drop();
    vkDestroySwapchainKHR(V.device, V.swapchain, NULL);
    V.swapchain = NULL; V.image_count = 0;
}
/* Returns 0 when there is nothing to present to (a minimised window has a zero-sized surface). */
static int swapchain_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.gpu, V.surface, &caps);
    if (r != VK_SUCCESS) vk_fatal("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
    VkExtent2D ext = caps.currentExtent;
#ifdef _WIN32
    if (ext.width == 0xffffffffu) { RECT rc; GetClientRect(V.hwnd, &rc); ext.width = (uint32_t)rc.right; ext.height = (uint32_t)rc.bottom; }
#else
    if (ext.width == 0xffffffffu) { int w = 0, h = 0; SDL_GetWindowSizeInPixels((SDL_Window *)V.hwnd, &w, &h); ext.width = (uint32_t)w; ext.height = (uint32_t)h; }   /* Wayland leaves the size to us */
#endif
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (!ext.width || !ext.height) return 0;
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) nvn_vk_missing("a surface whose images cannot be cleared by a transfer (no TRANSFER_DST usage)");
    VkSwapchainCreateInfoKHR ci;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = V.surface;
    ci.minImageCount = caps.minImageCount + 1 < 3 ? 3 : caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = V.format.format; ci.imageColorSpace = V.format.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    {   const char *mt = getenv("SSBU_MENU_TEST");             /* U1, test only: the settings menu's picture is read back from the swapchain image */
        g_mu_shot_src = mt && mt[0] && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (g_mu_shot_src) ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT; }
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = V.mode;
    ci.clipped = VK_TRUE;
    r = vkCreateSwapchainKHR(V.device, &ci, NULL, &V.swapchain);
    if (r != VK_SUCCESS) vk_fatal("vkCreateSwapchainKHR", r);
    V.image_count = 16;
    r = vkGetSwapchainImagesKHR(V.device, V.swapchain, &V.image_count, V.images);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) vk_fatal("vkGetSwapchainImagesKHR", r);
    V.extent = ext;
    return 1;
}

/* ---------------------------------------------------------------- start */
static void *vk_sym(const char *name) {
    void *p = (void *)GetProcAddress(V.loader, name);
    if (!p) { printf("nvn_vk: vulkan-1.dll does not export %s\n", name); fflush(stdout); ExitProcess(5); }
    return p;
}

/* (2026-10-21, tester: "game not booting error 5": the Vulkan loader had no window surface) Vulkan cannot be used on this PC
 * (no driver, no surface, no device): unless a renderer was asked for (SSBU_RENDERER), this exe starts again with the
 * OpenGL renderer, same arguments and standard handles, and this process ends. Otherwise the exit code 5 as before. */
static void vk_unavailable(void) {
#ifdef _WIN32
    const char *e = getenv("SSBU_RENDERER");
    if (!(e && e[0])) {
        wchar_t dir[MAX_PATH], pid[16];
        STARTUPINFOW si; PROCESS_INFORMATION pi;
        GetCurrentDirectoryW(MAX_PATH, dir);
        swprintf(pid, 16, L"%lu", GetCurrentProcessId());
        SetEnvironmentVariableW(L"SSBU_RESTART_WAIT_PID", pid);
        SetEnvironmentVariableW(L"SSBU_RENDERER", L"gl");
        memset(&si, 0, sizeof si); si.cb = sizeof si;
        HANDLE h[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
        for (int i = 0; i < 3; i++) if (h[i] && h[i] != INVALID_HANDLE_VALUE) SetHandleInformation(h[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = h[0]; si.hStdOutput = h[1]; si.hStdError = h[2];
        wchar_t *cmd = _wcsdup(GetCommandLineW());
        printf("nvn_vk: Vulkan cannot be used on this PC: the game starts again with the OpenGL renderer (F1 > Graphics > renderer)\n");
        fflush(stdout);
        if (cmd && CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, dir, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); ExitProcess(0); }
        printf("nvn_vk: starting again with OpenGL FAILED (error %lu)\n", GetLastError());
        fflush(stdout);
    }
#endif
    ExitProcess(5);
}
/* Render thread, once. Returns 1; any failure prints its cause and ends the process with code 5 (as the OpenGL path does). */
int nvn_vk_init(void *hwnd) {
    static const char *const k_wanted[] = {"VK_KHR_swapchain", "VK_EXT_extended_dynamic_state3", "VK_EXT_vertex_input_dynamic_state", "VK_KHR_push_descriptor", "VK_EXT_custom_border_color",
                                           "VK_EXT_conservative_rasterization", "VK_EXT_external_memory_host", "VK_EXT_depth_clip_control", "VK_EXT_provoking_vertex", "VK_EXT_4444_formats",
                                           "VK_EXT_index_type_uint8", "VK_EXT_full_screen_exclusive", "VK_EXT_blend_operation_advanced", "VK_KHR_pipeline_library"};
    VkResult r;
    V.hwnd = hwnd;
    V.loader = LoadLibraryA("vulkan-1.dll");
    if (!V.loader) { printf("nvn_vk: vulkan-1.dll cannot be loaded (error %lu): no Vulkan driver on this PC\n", GetLastError()); fflush(stdout); vk_unavailable(); }
#define X(ret, name, args) name = vk_sym(#name);
    VKF(X)
#undef X
    vkEnumerateInstanceVersion = (void *)GetProcAddress(V.loader, "vkEnumerateInstanceVersion");
    uint32_t loader_version = VK_MAKE_API_VERSION(0, 1, 0, 0);
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&loader_version);

    /* instance: surface extensions; validation + debug messages on request */
    VkExtensionProperties iext[128];
    uint32_t niext = 128;
    r = vkEnumerateInstanceExtensionProperties(NULL, &niext, iext);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) vk_fatal("vkEnumerateInstanceExtensionProperties", r);
#ifdef _WIN32
    const char *exts[4], *layers[1];
#else
    const char *exts[14], *layers[1];                 /* Linux build: SDL may name several surface extensions */
#endif
    uint32_t next = 0, nlayers = 0;
#ifdef _WIN32
    exts[next++] = "VK_KHR_surface"; exts[next++] = "VK_KHR_win32_surface";
    if (!has_name(iext, niext, "VK_KHR_surface") || !has_name(iext, niext, "VK_KHR_win32_surface")) { printf("nvn_vk: the Vulkan loader has no VK_KHR_surface / VK_KHR_win32_surface\n"); fflush(stdout); vk_unavailable(); }
#else
    {   /* the surface extensions of SDL's video driver (X11 or Wayland) */
        Uint32 n = 0;
        const char *const *se = SDL_Vulkan_GetInstanceExtensions(&n);
        if (!se || !n) { printf("nvn_vk: SDL names no Vulkan surface extensions (%s)\n", SDL_GetError()); fflush(stdout); ExitProcess(5); }
        for (Uint32 i = 0; i < n && next < 10; i++) {
            if (!has_name(iext, niext, se[i])) { printf("nvn_vk: the Vulkan loader has no %s\n", se[i]); fflush(stdout); ExitProcess(5); }
            exts[next++] = se[i];
        }
    }
    /* macOS: MoltenVK is a portability driver; the loader lists it only for an instance that asks for portability
     * devices (VK_KHR_portability_enumeration + the enumerate-portability flag). Harmless where the loader lacks it. */
    VkInstanceCreateFlags iflags = 0;
    if (has_name(iext, niext, "VK_KHR_portability_enumeration")) { exts[next++] = "VK_KHR_portability_enumeration"; iflags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; }
#endif
    { const char *e = getenv("SSBU_VK_VALIDATE"); V.validate = e && atoi(e) != 0; }
    if (V.validate) {
        static VkLayerProperties lp[64];
        uint32_t nlp = 64;
        int found = 0;
        r = vkEnumerateInstanceLayerProperties(&nlp, lp);
        for (uint32_t i = 0; (r == VK_SUCCESS || r == VK_INCOMPLETE) && i < nlp; i++) found |= !strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation");
        if (found && has_name(iext, niext, "VK_EXT_debug_utils")) { layers[nlayers++] = "VK_LAYER_KHRONOS_validation"; exts[next++] = "VK_EXT_debug_utils"; }
        else { V.validate = 0; printf("nvn_vk: SSBU_VK_VALIDATE=1 but VK_LAYER_KHRONOS_validation is not installed on this PC (%u layers known to the loader): running WITHOUT validation\n", (unsigned)nlp); }
    }
    VkApplicationInfo app;
    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO; app.pApplicationName = "ssbu-native"; app.pEngineName = "nvn_vk"; app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof ici);
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO; ici.pApplicationInfo = &app;
    ici.enabledLayerCount = nlayers; ici.ppEnabledLayerNames = layers; ici.enabledExtensionCount = next; ici.ppEnabledExtensionNames = exts;
#ifndef _WIN32
    ici.flags = iflags;
#endif
    r = vkCreateInstance(&ici, NULL, &V.instance);
    if (r != VK_SUCCESS) { printf("nvn_vk: vkCreateInstance failed: %s\n", vk_result_name(r)); fflush(stdout); vk_unavailable(); }
    if (V.validate) {
        vkCreateDebugUtilsMessengerEXT = (void *)vkGetInstanceProcAddr(V.instance, "vkCreateDebugUtilsMessengerEXT");
        vkDestroyDebugUtilsMessengerEXT = (void *)vkGetInstanceProcAddr(V.instance, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi;
        memset(&mi, 0, sizeof mi);
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mi.pfnUserCallback = vk_debug_cb;
        if (vkCreateDebugUtilsMessengerEXT) { r = vkCreateDebugUtilsMessengerEXT(V.instance, &mi, NULL, &V.messenger); if (r != VK_SUCCESS) vk_fatal("vkCreateDebugUtilsMessengerEXT", r); }
        printf("nvn_vk: validation layer on (VK_LAYER_KHRONOS_validation); messages are printed as vk-validation:\n");
    }

#ifdef _WIN32
    VkWin32SurfaceCreateInfoKHR si;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR; si.hinstance = GetModuleHandleA(NULL); si.hwnd = hwnd;
    r = vkCreateWin32SurfaceKHR(V.instance, &si, NULL, &V.surface);
    if (r != VK_SUCCESS) vk_fatal("vkCreateWin32SurfaceKHR", r);
#else
    if (!SDL_Vulkan_CreateSurface((SDL_Window *)hwnd, V.instance, NULL, &V.surface)) { printf("nvn_vk: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError()); fflush(stdout); ExitProcess(5); }
#endif

    /* device: discrete > integrated > virtual > other; needs one queue family with graphics + compute that can present to the window */
    VkPhysicalDevice gpus[16];
    uint32_t ngpu = 16;
    r = vkEnumeratePhysicalDevices(V.instance, &ngpu, gpus);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !ngpu) { printf("nvn_vk: no Vulkan device (%s, %u devices)\n", vk_result_name(r), (unsigned)ngpu); fflush(stdout); vk_unavailable(); }
    int forced = getenv("SSBU_VK_DEVICE") ? atoi(getenv("SSBU_VK_DEVICE")) : -1, best = -1, best_score = -1;
    uint32_t best_family = 0;
    if (forced < 0 && gfx_options()->gpu[0]) {    /* track O: gpu = <number> | <part of the name> in graphics.ini (the environment switch wins) */
        const char *want = gfx_options()->gpu;
        if (want[strspn(want, "0123456789")] == 0) forced = atoi(want);
        else for (uint32_t i = 0; i < ngpu && forced < 0; i++) {
            VkPhysicalDeviceProperties p;
            char name[256], w[64];
            vkGetPhysicalDeviceProperties(gpus[i], &p);
            snprintf(name, sizeof name, "%s", p.deviceName); snprintf(w, sizeof w, "%s", want);
            _strlwr(name); _strlwr(w);
            if (strstr(name, w)) forced = (int)i;
        }
        printf("nvn_vk: graphics.ini gpu = %s: %s\n", want, forced >= 0 ? "taken as the device with that number / the first device with that name" : "no device has that name, the preferred one is used");
    }
    for (uint32_t i = 0; i < ngpu; i++) {
        VkPhysicalDeviceProperties p;
        VkQueueFamilyProperties q[32];
        uint32_t nq = 32;
        int family = -1;
        vkGetPhysicalDeviceProperties(gpus[i], &p);
        vkGetPhysicalDeviceQueueFamilyProperties(gpus[i], &nq, q);
        for (uint32_t k = 0; k < nq && family < 0; k++) {
            VkBool32 can = VK_FALSE;
            if ((q[k].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) continue;
            if (vkGetPhysicalDeviceSurfaceSupportKHR(gpus[i], k, V.surface, &can) == VK_SUCCESS && can) family = (int)k;
        }
        static const char *const k_type[5] = {"other", "integrated GPU", "discrete GPU", "virtual GPU", "CPU"};
        int type = p.deviceType >= 0 && p.deviceType <= 4 ? p.deviceType : 0;
        int usable = family >= 0 && p.apiVersion >= VK_API_VERSION_1_3;
        int score = !usable ? -1 : type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : type == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 1 : 0;
        printf("nvn_vk: device %u: %s (%s, vendor 0x%04x device 0x%04x), Vulkan %u.%u.%u, driver 0x%08x%s\n", (unsigned)i, p.deviceName, k_type[type], p.vendorID, p.deviceID,
               VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion), p.driverVersion,
               family < 0 ? " - cannot present to the window with a graphics + compute queue" : p.apiVersion < VK_API_VERSION_1_3 ? " - below Vulkan 1.3" : "");
        if (forced >= 0 ? (int)i == forced && usable : score > best_score) { best = (int)i; best_score = score; best_family = (uint32_t)family; }
    }
    if (best < 0) { printf("nvn_vk: no usable Vulkan 1.3 device%s\n", forced >= 0 ? " (SSBU_VK_DEVICE names one that is not usable)" : ""); fflush(stdout); if (forced >= 0) ExitProcess(5); vk_unavailable(); }
    V.gpu = gpus[best]; V.family = best_family;
    vkGetPhysicalDeviceProperties(V.gpu, &V.props);

    static VkExtensionProperties dext[512];
    uint32_t ndext = 512;
    r = vkEnumerateDeviceExtensionProperties(V.gpu, NULL, &ndext, dext);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) vk_fatal("vkEnumerateDeviceExtensionProperties", r);
    printf("nvn_vk: using device %d: %s (loader Vulkan %u.%u.%u); extensions of the design:", best, V.props.deviceName, VK_API_VERSION_MAJOR(loader_version), VK_API_VERSION_MINOR(loader_version),
           VK_API_VERSION_PATCH(loader_version));
    for (unsigned i = 1; i < sizeof k_wanted / sizeof *k_wanted; i++) printf(" %s=%s", k_wanted[i] + 3, has_name(dext, ndext, k_wanted[i]) ? "yes" : "NO");
    printf("%s\n", r == VK_INCOMPLETE ? " (list cut at 512: NO may be wrong)" : "");
    if (!has_name(dext, ndext, "VK_KHR_swapchain")) { printf("nvn_vk: the device has no VK_KHR_swapchain\n"); fflush(stdout); ExitProcess(5); }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi;
    memset(&qi, 0, sizeof qi);
    qi.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO; qi.queueFamilyIndex = V.family; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    const char *dev_exts[1] = {"VK_KHR_swapchain"};
    VkDeviceCreateInfo di;
    memset(&di, 0, sizeof di);
    di.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi; di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = dev_exts;
    vkd_device_info(&di, dext, ndext);                    /* the extensions and features the drawing part uses */
    r = vkCreateDevice(V.gpu, &di, NULL, &V.device);
    if (r != VK_SUCCESS) vk_fatal("vkCreateDevice", r);
    vkGetDeviceQueue(V.device, V.family, 0, &V.queue);

    /* surface format: 8-bit BGRA / RGBA UNORM, sRGB colour space (the window texture's values are shown as they are, as the OpenGL blit does) */
    VkSurfaceFormatKHR fmts[64];
    uint32_t nfmt = 64;
    r = vkGetPhysicalDeviceSurfaceFormatsKHR(V.gpu, V.surface, &nfmt, fmts);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nfmt) vk_fatal("vkGetPhysicalDeviceSurfaceFormatsKHR", r);
    V.format.format = 0;
    for (uint32_t i = 0; i < nfmt && !V.format.format; i++) if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) V.format = fmts[i];
    for (uint32_t i = 0; i < nfmt && !V.format.format; i++) if (fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) V.format = fmts[i];
    if (!V.format.format) { printf("nvn_vk: the surface offers no 8-bit UNORM format in the sRGB colour space (%u formats, first: %d)\n", (unsigned)nfmt, (int)fmts[0].format); fflush(stdout); ExitProcess(5); }

    /* present mode: the 60 Hz pacing is display.c's, so the swapchain must not wait for the display */
    VkPresentModeKHR modes[16];
    uint32_t nmode = 16;
    int have[4] = {0, 0, 0, 0};
    r = vkGetPhysicalDeviceSurfacePresentModesKHR(V.gpu, V.surface, &nmode, modes);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE) vk_fatal("vkGetPhysicalDeviceSurfacePresentModesKHR", r);
    for (uint32_t i = 0; i < nmode; i++) if (modes[i] >= 0 && modes[i] < 4) have[modes[i]] = 1;
    static const char *const k_mode[4] = {"immediate", "mailbox", "fifo", "fifo_relaxed"};
    const char *want = getenv("SSBU_VK_PRESENT");
    V.mode = have[VK_PRESENT_MODE_MAILBOX_KHR] ? VK_PRESENT_MODE_MAILBOX_KHR : have[VK_PRESENT_MODE_IMMEDIATE_KHR] ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    if (!want && gfx_options()->present && gfx_options()->present < 4 && !(gfx_options()->present == 1 && display_hfr_mode() > 0)) {      /* 4 display, 5 auto: mailbox, the tick comes from display.c (session 14) */       /* track O: vsync = on | off | mailbox in graphics.ini (the environment switch wins); when HFR fixed cadence is active, bypass FIFO VSync so display.c paces at 144 Hz without double-waiting */
        static const char *const k_opt[4] = {NULL, "fifo", "immediate", "mailbox"};
        want = k_opt[gfx_options()->present & 3];
        printf("nvn_vk: graphics.ini vsync = %s: present mode %s asked for\n", gfx_options()->present == 1 ? "on" : gfx_options()->present == 2 ? "off" : "mailbox", want);
    }
    if (want) {
        int m = -1;
        for (int i = 0; i < 4; i++) if (!strcmp(want, k_mode[i])) m = i;
        if (m >= 0 && have[m]) V.mode = m;
        else printf("nvn_vk: SSBU_VK_PRESENT=%s is %s: using %s\n", want, m < 0 ? "not a mode (immediate, mailbox, fifo, fifo_relaxed)" : "not offered by the surface", k_mode[V.mode]);
    }
    if (V.mode == VK_PRESENT_MODE_FIFO_KHR || V.mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR)
        printf("nvn_vk: present mode %s: the display's own refresh now also holds the presents (a second clock next to display.c's)\n", k_mode[V.mode]);

    VkCommandPoolCreateInfo pi;
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = V.family;
    r = vkCreateCommandPool(V.device, &pi, NULL, &V.pool);
    if (r != VK_SUCCESS) vk_fatal("vkCreateCommandPool", r);
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO; ai.commandPool = V.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    r = vkAllocateCommandBuffers(V.device, &ai, &V.cmd);
    if (r != VK_SUCCESS) vk_fatal("vkAllocateCommandBuffers", r);
    VkSemaphoreCreateInfo smi;
    memset(&smi, 0, sizeof smi);
    smi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if ((r = vkCreateSemaphore(V.device, &smi, NULL, &V.acquired)) != VK_SUCCESS || (r = vkCreateSemaphore(V.device, &smi, NULL, &V.rendered)) != VK_SUCCESS) vk_fatal("vkCreateSemaphore", r);
    VkFenceCreateInfo fi;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    r = vkCreateFence(V.device, &fi, NULL, &V.fence);
    if (r != VK_SUCCESS) vk_fatal("vkCreateFence", r);

    int ok = swapchain_create();
    printf("nvn_vk: swapchain %ux%u, %u images, format %d, present mode %s%s\n", (unsigned)V.extent.width, (unsigned)V.extent.height, (unsigned)V.image_count, (int)V.format.format, k_mode[V.mode],
           ok ? "" : " (the window has no area yet: created at the first present)");
    fflush(stdout);
    V.alive = 1;
    vkd_init();
    return 1;
}

/* ---------------------------------------------------------------- frames */
static void wait_last_submit(void) {
    if (!V.fence_pending) return;
    vk_ok("vkWaitForFences", vkWaitForFences(V.device, 1, &V.fence, VK_TRUE, 5000000000ull));
    vk_ok("vkResetFences", vkResetFences(V.device, 1, &V.fence));
    V.fence_pending = 0;
}

/* Clears the next swapchain image to one colour and presents it. Never waits for the display (see the present mode). */
static void present_colour(const float rgba[4]) {
    if (!V.alive) return;
    wait_last_submit();
    if (!V.swapchain && !swapchain_create()) { InterlockedIncrement64(&g_vk_skipped); return; }
    uint32_t index = 0;
    VkResult r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, V.acquired, NULL, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {                        /* the window changed size: a new swapchain, this frame is shown by it */
        swapchain_destroy();
        InterlockedIncrement64(&g_vk_recreated);
        if (!swapchain_create()) { InterlockedIncrement64(&g_vk_skipped); return; }
        r = vkAcquireNextImageKHR(V.device, V.swapchain, 1000000000ull, V.acquired, NULL, &index);
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vk_ok("vkAcquireNextImageKHR", r); InterlockedIncrement64(&g_vk_skipped); return; }
    int suboptimal = r == VK_SUBOPTIMAL_KHR;

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_ok("vkResetCommandBuffer", vkResetCommandBuffer(V.cmd, 0));
    vk_ok("vkBeginCommandBuffer", vkBeginCommandBuffer(V.cmd, &bi));
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = V.images[index];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue cv;
    memcpy(cv.float32, rgba, sizeof cv.float32);
    vkCmdClearColorImage(V.cmd, V.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &b.subresourceRange);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(V.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vk_ok("vkEndCommandBuffer", vkEndCommandBuffer(V.cmd));

    VkFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;       /* the first barrier waits for the acquired image in this stage */
    VkSubmitInfo sb;
    memset(&sb, 0, sizeof sb);
    sb.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    sb.waitSemaphoreCount = 1; sb.pWaitSemaphores = &V.acquired; sb.pWaitDstStageMask = &wait_stage;
    sb.commandBufferCount = 1; sb.pCommandBuffers = &V.cmd;
    sb.signalSemaphoreCount = 1; sb.pSignalSemaphores = &V.rendered;
    r = vkQueueSubmit(V.queue, 1, &sb, V.fence);
    if (!vk_ok("vkQueueSubmit", r)) { InterlockedIncrement64(&g_vk_skipped); return; }
    V.fence_pending = 1;

    VkPresentInfoKHR pr;
    memset(&pr, 0, sizeof pr);
    pr.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pr.waitSemaphoreCount = 1; pr.pWaitSemaphores = &V.rendered; pr.swapchainCount = 1; pr.pSwapchains = &V.swapchain; pr.pImageIndices = &index;
    r = vkQueuePresentKHR(V.queue, &pr);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR || suboptimal) { swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); }      /* made again at the next present */
    else vk_ok("vkQueuePresentKHR", r);
    InterlockedIncrement64(&g_vk_frames);
}

/* nvnQueuePresentTexture while this back end holds no texture of the game (M1): the window is cleared to black, as the
 * OpenGL path does for a window texture nothing was drawn into. */
void nvn_vk_present_black(void) {
    static const float black[4] = {0, 0, 0, 1};
    present_colour(black);
}

/* nvnQueueFlush / nvnQueueFinish: nothing is recorded between presents yet; Finish waits for the last submit. */
void nvn_vk_flush(int finish) { if (V.alive && finish) wait_last_submit(); }

void nvn_vk_shutdown(void) {
    if (!V.alive) return;
    V.alive = 0;
    vk_ok("vkDeviceWaitIdle", vkDeviceWaitIdle(V.device));
    vkd_shutdown();
    if (V.swapchain) vkDestroySwapchainKHR(V.device, V.swapchain, NULL);
    vkDestroyFence(V.device, V.fence, NULL);
    vkDestroySemaphore(V.device, V.acquired, NULL);
    vkDestroySemaphore(V.device, V.rendered, NULL);
    vkDestroyCommandPool(V.device, V.pool, NULL);
    vkDestroyDevice(V.device, NULL);
    vkDestroySurfaceKHR(V.instance, V.surface, NULL);
    if (V.messenger && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(V.instance, V.messenger, NULL);
    vkDestroyInstance(V.instance, NULL);
    printf("nvn_vk: shutdown clean: swapchain, sync objects, command pool, device, surface and instance destroyed (%lld frames presented, %lld Vulkan call failures, %lld validation messages%s)\n",
           (long long)g_vk_frames, (long long)g_vk_errors, (long long)g_vk_messages, V.validate ? "" : ": layer not active");
    fflush(stdout);
}

void nvn_vk_report(FILE *out) {
    fprintf(out, "nvn_vk: %lld frames presented, %lld not presented (no surface area or a failed call), swapchain made again %lld times, %lld Vulkan call failures, %lld validation messages (%s), %lld calls of operations that are not implemented\n",
            (long long)g_vk_frames, (long long)g_vk_skipped, (long long)g_vk_recreated, (long long)g_vk_errors, (long long)g_vk_messages, V.validate ? "layer on" : "layer off", (long long)g_vk_missing_calls);
}

/* ---------------------------------------------------------------- M1 self-test (SSBU_VK_SELFTEST=<frames>)
 * Presents <frames> cleared frames exactly as the game presents: display_before_present, the present job on the render
 * thread, display_pace, then the wait the game does on the window texture's sync (display_wait_refresh). Prints the
 * number a 60 Hz clock must give: frames / seconds, and the frame intervals. */
static void job_selftest_frame(void *arg) {
    int i = *(int *)arg;
    float c[4] = {(float)(i % 120) / 119.0f, (float)((i / 2) % 90) / 89.0f, 0.25f, 1.0f};
    present_colour(c);
}
static void job_shutdown(void *arg) { (void)arg; nvn_vk_shutdown(); }
static int cmp_double(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
void nvn_vk_selftest_hook(void) {
    const char *e = getenv("SSBU_VK_SELFTEST");
    int n = e ? atoi(e) : 0;
    if (!V.alive || n <= 0) return;
    LARGE_INTEGER f, t0, prev, now;
    double *dt = malloc(sizeof *dt * (size_t)n), sum = 0;
    QueryPerformanceFrequency(&f);
    printf("nvn_vk: self-test: %d frames on the display clock\n", n);
    fflush(stdout);
    for (int i = -30; i < n; i++) {                          /* 30 frames to settle (first swapchain images, clock start), not measured */
        if (i == 0) { QueryPerformanceCounter(&t0); prev = t0; }
        display_before_present();
        nvn_render_run(job_selftest_frame, &i);
        display_pace();
        display_wait_refresh();
        if (i >= 0) { QueryPerformanceCounter(&now); dt[i] = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / (double)f.QuadPart; sum += dt[i]; prev = now; }
    }
    qsort(dt, (size_t)n, sizeof *dt, cmp_double);
    int late = 0;
    for (int i = 0; i < n; i++) late += dt[i] > 25.0;
    printf("nvn_vk: self-test: %d frames in %.3f s = %.2f fps; frame interval ms: min %.2f median %.2f p99 %.2f max %.2f; %d over 25 ms; %lld not presented, %lld Vulkan call failures, swapchain made again %lld times\n",
           n, sum / 1000.0, (double)n * 1000.0 / sum, dt[0], dt[n / 2], dt[(int)((double)(n - 1) * 0.99)], dt[n - 1], late, (long long)g_vk_skipped, (long long)g_vk_errors, (long long)g_vk_recreated);
    nvn_render_run(job_shutdown, NULL);
    fflush(stdout);
    ExitProcess(g_vk_errors || g_vk_skipped ? 1 : 0);
}

#include "nvn_vk_draw.c"                                   /* everything that draws (session 2): not a build unit of its own */
