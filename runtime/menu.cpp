/* U1: the in-game settings menu, in the style of Ship of Harkinian (the Ocarina of Time PC port): Dear ImGui (MIT,
 * reference\upstream\imgui-1.91.9b) drawn over the game's picture. Design, keys, proofs: plan\phases\U1_settings_menu.md.
 *
 * Open / close: F1 on the keyboard; on a controller the Home / Guide button (like the Switch's HOME menu), or the
 * Capture / Share button; + or Esc also close it. Those two buttons never reach the game while the menu exists (nnpad_sdl.c).
 * While it is open the game gets nothing from the real pads and keyboard
 * (nnhid.c asks menu_input_blocked), and after it closes, nothing until every pad button is released.
 * The game is not paused: it keeps running under the menu (a fight goes on; the players' inputs are held).
 *
 * Threads: the window thread (display.c wndproc) queues mouse and key messages here; the render thread (nvn_vk_menu.c,
 * included by nvn_vk_draw.c) calls menu_frame once per presented frame: the ImGui frame is built and drawn there.
 * Settings are written to config\graphics.ini (gfx_ini_set), the keyboard bindings to controllers.ini (nnpad_sdl.c),
 * the add-on content list to config\dlc.txt. Each control says whether it acts at once [live] or at the next start [restart].
 *
 * Log page: the runtime's own output, read back from the file stdout is redirected to (gioca.bat, the test runners):
 * the last LOG_MAX lines in memory, a filter, auto-scroll, "Export log" (logs\ssbu_log_<time>.txt beside the exe, then the
 * folder is opened in Explorer: from that button only). Stats overlay (F2 or the menu; graphics.ini stats_overlay):
 * frames per second, frame time, the online state of net_stats.h, run-ahead.
 *
 * Off unless SSBU_MENU=1 or graphics.ini menu = on (SSBU_MENU=0 wins). SSBU_MENU_TEST=<presented frame>: the test hook
 * (opens the menu, changes frame_time_overlay through the same code as the control, pictures of the Graphics and Log
 * pages and of the stats overlay with the menu closed, read back from the swapchain image into BMPs; "menu: test:" lines). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <shellapi.h>
#include <psapi.h>
#else
/* U2 (plan\phases\U2_menu_linux.md): Linux and macOS. The window is SDL's (display.c): its events reach the menu through
 * an SDL event watch (menu_sdl_attach) instead of window messages, and the few calls SDL wants on the window's thread
 * (text input on / off, the clipboard) are made by that thread (menu_sdl_service). The drawing is the same Vulkan pass
 * (nvn_vk_menu.c). Text boxes for a controller player: the desktop's own prompt (kdialog / zenity; osascript on a Mac). */
#include <SDL3/SDL.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <spawn.h>
#ifdef __APPLE__
#include <sys/param.h>
#include <mach-o/dyld.h>
#else
#include <link.h>
#endif
extern char **environ;
#endif
#include "imgui.h"
#include "net_stats.h"
extern "C" uint32_t net_match_host(void);      /* net_relay.c / net_off.c: the matched host (the F1 table marks its room) */
extern "C" {
#include "gfx_options.h"
#include "menu.h"
#include "nnpad.h"
uint64_t nnhid_preview(int id, int st[4]); void nnhid_kb_player_refresh(void); int nnhid_kb_player(void);      /* nnhid.c */
void nnhid_test_key(int vk); long nnhid_test_result(uint64_t *buttons); int nnpad_kb_default(int idx);
uint64_t nnhid_menu_buttons(void);                                   /* nnhid.c: every pad, Npad bit order, no effect on the game */
void display_set_window_mode(int mode); void display_set_client_size(int w, int h); int display_window_mode(void);      /* display.c */
int nnaudio_volume(void); void nnaudio_set_volume(int percent);     /* nnaudio.c */
int nnpad_kb_vk(int idx); void nnpad_kb_bind(int idx, int vk); void nnpad_kb_player_set(int p); int nnpad_kb_player_cfg(void);      /* nnpad_sdl.c */
const char *nnpad_kb_label(int idx); void nnpad_vk_name(int vk, char *o, int n); int nnpad_describe(char *out, int n); const char *nnpad_config_path(void);
int nnaoc_owned(const int **list);                                   /* nnmisc.c */
long long nvn_front_presents(void); void nvn_front_osk_redraw(void); /* nvn_gl.c */
const char *nvn_vk_device_name(void);                                /* nvn_vk_menu.c */
int osk_active(void);                                                /* osk.c */
int osk_run(const wchar_t *header, const wchar_t *guide, const wchar_t *initial, int max_len, int min_len, int password, wchar_t *out);      /* osk.c */
extern "C" void display_refresh_info(double *hz, int *lock_k);      /* display.c */
extern "C" int display_hfr_mode(void);
extern "C" int display_hfr_hz(void);
extern "C" void display_set_hfr_mode(int mode);
extern "C" int display_hfr_toggle(void);
int net_online(void); int net_mode(void); const char *net_room_code(void); const char *net_lan_code(void); const char *net_upnp_result(void);      /* net_relay.c */
int r1_ra_phase(void);                                               /* r1_state.c (read only): 0 = run-ahead not running now */
}

enum { B_A = 0, B_B, B_X, B_Y, B_LS, B_RS, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LEFT, B_UP, B_RIGHT, B_DOWN, B_HOME, B_CAPTURE };
#define BIT(b) (1ull << (b))
enum { NET_MODE_LOBBY, NET_MODE_LAN, NET_MODE_HOST, NET_MODE_JOIN };      /* net_relay.h */

/* ------------------------------------------------------------------ state shared between threads */
static volatile LONG g_open, g_guard, g_toggle_req;
static int g_enabled = -1;
extern "C" int menu_enabled(void) {
    if (g_enabled < 0) {
        const char *e = getenv("SSBU_MENU"), *t = getenv("SSBU_MENU_TEST");
        int on = e && e[0] ? atoi(e) != 0 : gfx_menu();
        if (t && t[0]) on = 1;
        g_enabled = on;
        if (on) { printf("menu: settings menu on (F1, or the Home / Guide or Capture / Share button of a controller, or hold - and + together for 1 s; %s)\n", e && e[0] ? "SSBU_MENU" : t && t[0] ? "SSBU_MENU_TEST" : "graphics.ini menu = on"); fflush(stdout); }
    }
    return g_enabled;
}
extern "C" int menu_is_open(void) { return g_enabled > 0 && g_open; }
extern "C" int menu_input_blocked(void) { return g_enabled > 0 && (g_open || g_guard); }
extern "C" void menu_request_toggle(void) { InterlockedIncrement(&g_toggle_req); }
/* R3: the online match's message (any thread; copied under a lock, read by the render thread) */
static SRWLOCK g_notice_lock = SRWLOCK_INIT;
static char g_notice[512];
static volatile LONG g_notice_on, g_notice_drawn;
extern "C" void menu_notice(const char *text) {
    AcquireSRWLockExclusive(&g_notice_lock);
    snprintf(g_notice, sizeof g_notice, "%s", text ? text : "");
    InterlockedExchange(&g_notice_on, g_notice[0] != 0);
    if (!g_notice[0]) InterlockedExchange(&g_notice_drawn, 0);
    ReleaseSRWLockExclusive(&g_notice_lock);
}
/* O1: a short message at the top of the picture ("Cannot join this room" + the cause), any thread, for 7 s */
static SRWLOCK g_popup_lock = SRWLOCK_INIT;
static char g_popup_title[96], g_popup_text[512];
static volatile LONG64 g_popup_until;
static volatile LONG g_popup_drawn;
extern "C" void menu_popup(const char *title, const char *text) {
    AcquireSRWLockExclusive(&g_popup_lock);
    snprintf(g_popup_title, sizeof g_popup_title, "%s", title ? title : "");
    snprintf(g_popup_text, sizeof g_popup_text, "%s", text ? text : "");
    InterlockedExchange(&g_popup_drawn, 0);
    InterlockedExchange64(&g_popup_until, (LONG64)GetTickCount64() + 7000);
    ReleaseSRWLockExclusive(&g_popup_lock);
}
static int popup_on(void) { return (ULONGLONG)g_popup_until > GetTickCount64(); }
/* net_stats.h: the default answer until the online code defines net_stats_get itself */
extern "C" void net_stats_get_default(NetStats *o) { memset(o, 0, sizeof *o); o->state = NET_OFFLINE; o->ping_ms = -1; }
#ifdef _WIN32
#pragma comment(linker, "/alternatename:net_stats_get=net_stats_get_default")
#endif                                               /* (Linux / macOS: net_off.c or net_relay.c defines net_stats_get) */
static volatile LONG g_stats_req;                    /* F2 */
static int g_asserts;
extern "C" void menu_assert_fail(const char *what, const char *file, int line) {
    if (g_asserts++ < 10) { const char *f = strrchr(file, '\\'); printf("menu: ImGui check failed: %s (%s:%d)\n", what, f ? f + 1 : file, line); fflush(stdout); }
}

/* the window's messages, queued by the window thread, read by the render thread */
enum { EV_MOVE = 1, EV_BTN, EV_WHEEL, EV_KEY, EV_CHAR };
struct Ev { int type, a, b; };
static CRITICAL_SECTION g_qcs;
static INIT_ONCE g_qonce = INIT_ONCE_STATIC_INIT;
static Ev g_q[256];
static int g_qn;
static BOOL CALLBACK q_init(PINIT_ONCE, PVOID, PVOID *) { InitializeCriticalSection(&g_qcs); return TRUE; }
static void q_push(int type, int a, int b) {
    InitOnceExecuteOnce(&g_qonce, q_init, NULL, NULL);
    EnterCriticalSection(&g_qcs);
    if (g_qn < 256) { g_q[g_qn].type = type; g_q[g_qn].a = a; g_q[g_qn].b = b; g_qn++; }
    LeaveCriticalSection(&g_qcs);
}
static int q_take(Ev *out, int max) {
    InitOnceExecuteOnce(&g_qonce, q_init, NULL, NULL);
    EnterCriticalSection(&g_qcs);
    int n = g_qn < max ? g_qn : max;
    memcpy(out, g_q, sizeof(Ev) * (size_t)n);
    g_qn = 0;
    LeaveCriticalSection(&g_qcs);
    return n;
}

#ifdef _WIN32
extern "C" int menu_wndproc(void *hwnd, unsigned m, uintptr_t w, intptr_t l) {
    (void)hwnd;
    if (!menu_enabled()) return 0;
    if (m == WM_KEYDOWN && w == VK_F1) { if (!(l & (1 << 30))) menu_request_toggle(); return 1; }
    if (m == WM_KEYDOWN && w == VK_F2) { if (!(l & (1 << 30))) InterlockedIncrement(&g_stats_req); return 1; }      /* the stats overlay */
    if (!g_open || osk_active()) return 0;
    switch (m) {
    case WM_MOUSEMOVE: q_push(EV_MOVE, (short)LOWORD(l), (short)HIWORD(l)); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: q_push(EV_BTN, 0, m == WM_LBUTTONDOWN); return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: q_push(EV_BTN, 1, m == WM_RBUTTONDOWN); return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: q_push(EV_BTN, 2, m == WM_MBUTTONDOWN); return 1;
    case WM_MOUSEWHEEL: q_push(EV_WHEEL, GET_WHEEL_DELTA_WPARAM(w), 0); return 1;
    case WM_KEYDOWN: case WM_KEYUP: q_push(EV_KEY, (int)w, m == WM_KEYDOWN); return 1;
    case WM_CHAR: {                                   /* the window is an ANSI one: a character of the system code page */
        char a = (char)w; wchar_t u = 0;
        if (w < 0x80 || MultiByteToWideChar(CP_ACP, 0, &a, 1, &u, 1) != 1) u = (wchar_t)w;
        q_push(EV_CHAR, (int)u, 0); return 1; }
    }
    return 0;
}
#else
extern "C" int menu_wndproc(void *hwnd, unsigned m, uintptr_t w, intptr_t l) { (void)hwnd; (void)m; (void)w; (void)l; return 0; }      /* (no window messages here) */
/* the SDL window's events, as the window procedure above takes the Windows ones. An event watch sees every event on the
 * thread that pumps it (display.c's window thread, or nnpad_sdl.c's), whoever empties the queue afterwards. */
static SDL_Window *g_sdl_win;
static float g_sdl_density = 1.0f;                   /* window pixels per SDL window coordinate (2 on a Retina screen) */
static volatile LONG g_text_want, g_text_on;         /* render thread: an ImGui text box is active; window thread: SDL_StartTextInput done */
static SRWLOCK g_clip_lock = SRWLOCK_INIT;
static char *g_clip_set, *g_clip_got;                /* text to put on the clipboard / what the window thread read from it */
static volatile LONG g_clip_get_req, g_clip_get_done;
static int sdl_vk(SDL_Scancode sc) {                 /* the Windows key code of a key, as plat_posix.c's vk_scancode maps them back */
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return '1' + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return VK_F1 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return VK_NUMPAD0 + 1 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_KP_0: return VK_NUMPAD0;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB: return VK_TAB;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
    case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
    case SDL_SCANCODE_SPACE: return VK_SPACE;
    case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
    case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
    case SDL_SCANCODE_END: return VK_END;
    case SDL_SCANCODE_HOME: return VK_HOME;
    case SDL_SCANCODE_LEFT: return VK_LEFT;
    case SDL_SCANCODE_UP: return VK_UP;
    case SDL_SCANCODE_RIGHT: return VK_RIGHT;
    case SDL_SCANCODE_DOWN: return VK_DOWN;
    case SDL_SCANCODE_INSERT: return VK_INSERT;
    case SDL_SCANCODE_DELETE: return VK_DELETE;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return VK_SHIFT;      /* as Windows sends them (WM_KEYDOWN VK_SHIFT) */
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return VK_CONTROL;
    case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return VK_MENU;
    case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
    case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_COMMA: return 0xBC;
    case SDL_SCANCODE_PERIOD: return 0xBE;
    case SDL_SCANCODE_SLASH: return 0xBF;
    case SDL_SCANCODE_GRAVE: return 0xC0;
    case SDL_SCANCODE_LEFTBRACKET: return 0xDB;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    default: return 0;
    }
}
static bool SDLCALL menu_sdl_watch(void *user, SDL_Event *e) {
    (void)user;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        int down = e->type == SDL_EVENT_KEY_DOWN;
        if (down && e->key.scancode == SDL_SCANCODE_F1) { if (!e->key.repeat) menu_request_toggle(); return true; }
        if (down && e->key.scancode == SDL_SCANCODE_F2) { if (!e->key.repeat) InterlockedIncrement(&g_stats_req); return true; }      /* the stats overlay */
        if (!g_open || osk_active()) return true;
        int vk = sdl_vk(e->key.scancode);
        if (vk) q_push(EV_KEY, vk, down);
        return true; }
    case SDL_EVENT_TEXT_INPUT: {                     /* UTF-8: one EV_CHAR per character (a code point, not UTF-16 as on Windows) */
        if (!g_open || osk_active() || !e->text.text) return true;
        const unsigned char *s = (const unsigned char *)e->text.text;
        while (*s) {
            unsigned c = *s++, more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
            if (more) c &= 0x3f >> more;
            for (; more && (*s & 0xc0) == 0x80; more--) c = c << 6 | (*s++ & 0x3f);
            if (!more) q_push(EV_CHAR, (int)c, 0);
        }
        return true; }
    case SDL_EVENT_MOUSE_MOTION:
        if (g_open && !osk_active()) q_push(EV_MOVE, (int)(e->motion.x * g_sdl_density), (int)(e->motion.y * g_sdl_density));
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (g_open && !osk_active() && b >= 0) q_push(EV_BTN, b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return true; }
    case SDL_EVENT_MOUSE_WHEEL: {
        float y = e->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e->wheel.y : e->wheel.y;
        if (g_open && !osk_active() && y != 0.0f) q_push(EV_WHEEL, (int)(y * WHEEL_DELTA), 0);
        return true; }
    }
    return true;
}
/* display.c (Linux / macOS), window thread: the window is made / once per turn of its event loop (at least every 8 ms) */
extern "C" void menu_sdl_attach(void *window) {
    if (!menu_enabled() || !window) return;
    g_sdl_win = (SDL_Window *)window;
    float d = SDL_GetWindowPixelDensity(g_sdl_win);
    if (d > 0.0f) g_sdl_density = d;
    SDL_AddEventWatch(menu_sdl_watch, NULL);
}
extern "C" void menu_sdl_service(void) {
    if (!g_sdl_win) return;
    float d = SDL_GetWindowPixelDensity(g_sdl_win);
    if (d > 0.0f) g_sdl_density = d;
    LONG want = g_text_want;
    if (want != g_text_on) {                         /* text input only while a text box is active (on a Steam Deck SDL may show Steam's keyboard) */
        if (want) SDL_StartTextInput(g_sdl_win); else SDL_StopTextInput(g_sdl_win);
        g_text_on = want;
    }
    char *set = NULL;
    AcquireSRWLockExclusive(&g_clip_lock); set = g_clip_set; g_clip_set = NULL; ReleaseSRWLockExclusive(&g_clip_lock);
    if (set) { SDL_SetClipboardText(set); free(set); }
    if (InterlockedExchange(&g_clip_get_req, 0)) {
        char *t = SDL_GetClipboardText();
        AcquireSRWLockExclusive(&g_clip_lock); free(g_clip_got); g_clip_got = strdup(t ? t : ""); ReleaseSRWLockExclusive(&g_clip_lock);
        SDL_free(t);
        InterlockedExchange(&g_clip_get_done, 1);
    }
}
/* ImGui's clipboard (render thread): handed to the window thread; a paste waits for it at most 100 ms */
static void clip_set(ImGuiContext *, const char *text) {
    AcquireSRWLockExclusive(&g_clip_lock); free(g_clip_set); g_clip_set = strdup(text ? text : ""); ReleaseSRWLockExclusive(&g_clip_lock);
}
static const char *clip_get(ImGuiContext *) {
    static char *ret;
    if (!g_sdl_win) return "";
    InterlockedExchange(&g_clip_get_done, 0);
    InterlockedExchange(&g_clip_get_req, 1);
    for (int i = 0; i < 25 && !g_clip_get_done; i++) Sleep(4);
    AcquireSRWLockExclusive(&g_clip_lock); free(ret); ret = strdup(g_clip_got ? g_clip_got : ""); ReleaseSRWLockExclusive(&g_clip_lock);
    return ret;
}
/* a line of text from the desktop's own prompt (a controller player cannot type in the menu's boxes): kdialog, else zenity
 * (as nnapplet.c's software keyboard); osascript on a Mac. 1 = OK (out), 0 = cancelled, -1 = no prompt program. Blocks. */
static void shq(const char *in, char *out, size_t n) {      /* '...' for the shell, a ' inside becomes '\'' */
    size_t k = 0;
    if (n < 3) { if (n) out[0] = 0; return; }
    out[k++] = '\'';
    for (; *in && k + 6 < n; in++) { if (*in == '\'') { memcpy(out + k, "'\\''", 4); k += 4; } else out[k++] = *in; }
    out[k++] = '\''; out[k] = 0;
}
static int text_prompt(const char *title, const char *text, const char *initial, char *out, int n) {
    char qt[256], qx[512], qi[256], cmd[1400];
#ifdef __APPLE__
    char as[900], a1[300], a2[300];             /* AppleScript strings: \ and " escaped, then the whole script quoted for the shell */
    size_t k = 0;
    for (const char *p = text; *p && k + 3 < sizeof a1; p++) { if (*p == '"' || *p == '\\') a1[k++] = '\\'; a1[k++] = *p; } a1[k] = 0; k = 0;
    for (const char *p = initial; *p && k + 3 < sizeof a2; p++) { if (*p == '"' || *p == '\\') a2[k++] = '\\'; a2[k++] = *p; } a2[k] = 0;
    snprintf(as, sizeof as, "text returned of (display dialog \"%s\" default answer \"%s\" with title \"%s\")", a1, a2, title);
    shq(as, qx, sizeof qx);
    snprintf(cmd, sizeof cmd, "osascript -e %s 2>/dev/null", qx);
    (void)qt; (void)qi;
#else
    shq(title, qt, sizeof qt); shq(text, qx, sizeof qx); shq(initial, qi, sizeof qi);
    int kd = system("command -v kdialog >/dev/null 2>&1") == 0, zn = !kd && system("command -v zenity >/dev/null 2>&1") == 0;
    if (kd) snprintf(cmd, sizeof cmd, "kdialog --title %s --inputbox %s %s", qt, qx, qi);
    else if (zn) snprintf(cmd, sizeof cmd, "zenity --entry --title=%s --text=%s --entry-text=%s", qt, qx, qi);
    else { printf("menu: no text prompt program (kdialog or zenity): type in the box with a keyboard\n"); fflush(stdout); return -1; }
#endif
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    size_t got = fread(out, 1, (size_t)n - 1, f);
    int st = pclose(f);
    out[got] = 0;
    out[strcspn(out, "\r\n")] = 0;
    if (st == -1 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) return 0;      /* cancelled or closed */
    return 1;
}
static void utf8_cut(char *s, int max_bytes) {        /* at most max_bytes bytes, not in the middle of a character */
    int n = (int)strlen(s);
    if (n <= max_bytes) return;
    n = max_bytes;
    while (n > 0 && ((unsigned char)s[n] & 0xc0) == 0x80) n--;
    s[n] = 0;
}
#endif

/* ------------------------------------------------------------------ the settings */
static const char *const k_window[] = {"windowed", "borderless", "fullscreen"};
static const char *const k_vsync[] = {"clock", "on", "off", "mailbox", "display", "auto"};      /* GfxOptions.present */
static const char *const k_latency[] = {"normal", "low", "lowest"};
static const char *const k_renderer[] = {"vulkan", "opengl"};
static const int k_aniso_v[] = {0, 2, 4, 8, 16};
static const char *const k_aniso[] = {"game", "2", "4", "8", "16"};
static const char *const k_profile[] = {"custom", "low"};
static const char *const k_wait[] = {"hold", "skip"};
static const char *const k_prepare[] = {"first_run", "background", "off"};
static const char *const k_load[] = {"start", "loading"};
static const char *const k_console[] = {"docked", "handheld"};
static const char *const k_onoff[] = {"off", "on"};
static const char *const k_lang[] = {"auto", "ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419", "zh-Hans", "zh-Hant", "ko", "nl", "ru"};
static const char *const k_lang_name[] = {"auto (Windows display language)", "Japanese", "English (US)", "English (UK)", "French", "French (Canada)", "German", "Italian",
                                          "Spanish (Spain)", "Spanish (Latin America)", "Chinese (simplified)", "Chinese (traditional)", "Korean", "Dutch", "Russian"};
static const int k_sizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}};
static const char *const k_size_name[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

struct Settings {
    int window, size, vsync, latency, renderer, aniso, profile, wait, prepare, load, console, overlay, volume, runahead, lang, kb_player, stats;
    float scale;
    char gpu[64];
};
static Settings S, S0;               /* S0 = what this run started with (restart marks) */
static char g_lang_start[16];
static int g_sections_inited;

static int idx_of(const char *const *names, int n, const char *v) { for (int i = 0; i < n; i++) if (!_stricmp(names[i], v)) return i; return 0; }
static void settings_init(int cw, int ch) {
    const GfxOptions *o = gfx_options();
    memset(&S, 0, sizeof S);
    S.window = o->window_mode; S.vsync = o->present >= 0 && o->present < N(k_vsync) ? o->present : 5; S.latency = gfx_latency(); S.renderer = o->renderer;
    S.aniso = 0; for (int i = 0; i < N(k_aniso_v); i++) if (k_aniso_v[i] == o->aniso) S.aniso = i;
    S.profile = o->profile; S.wait = o->shader_skip; S.prepare = o->shader_prepare; S.load = o->shader_load; S.console = o->handheld; S.overlay = o->overlay;
    S.volume = gfx_volume(); S.runahead = gfx_runahead(); S.scale = o->scale;
    snprintf(S.gpu, sizeof S.gpu, "%s", o->gpu[0] ? o->gpu : "auto");
    S.lang = idx_of(k_lang, N(k_lang), gfx_language()[0] ? gfx_language() : "auto");
    snprintf(g_lang_start, sizeof g_lang_start, "%s", k_lang[S.lang]);
    S.size = -1; for (int i = 0; i < N(k_sizes); i++) if (k_sizes[i][0] == o->width && k_sizes[i][1] == o->height) S.size = i;
    S.kb_player = nnpad_kb_player_cfg();
    S.stats = gfx_stats_overlay();
    S0 = S;
    (void)cw; (void)ch;
}
static void ini(const char *key, const char *value) { gfx_ini_set(key, value); }
static int restart_pending(void) {
    return S.vsync != S0.vsync || S.latency != S0.latency || S.renderer != S0.renderer || S.aniso != S0.aniso || S.profile != S0.profile || S.wait != S0.wait ||
           S.prepare != S0.prepare || S.load != S0.load || S.console != S0.console || S.runahead != S0.runahead || S.lang != S0.lang ||
           fabsf(S.scale - S0.scale) > 0.001f || strcmp(S.gpu, S0.gpu);
}
static void apply_stats(int on) {            /* live: the stats box (F2) */
    S.stats = on;
    ini("stats_overlay", k_onoff[on]);
}
static void apply_overlay(int on) {          /* live: title bar + log line (display.c reads it every second) + the picture (below) */
    S.overlay = on;
    gfx_set_overlay(on);
    ini("frame_time_overlay", k_onoff[on]);
}

/* ------------------------------------------------------------------ add-on content list (config\dlc.txt) */
struct DlcLine { char text[160]; int index, entry, on; };
static DlcLine *g_dlc;
static int g_dlc_n, g_dlc_loaded, g_dlc_dirty;
static char g_dlc_path[MAX_PATH];
static int dlc_entry(const char *p) {        /* the index of an entry line (nnmisc.c aoc_parse, simplified: 0x.., decimal or the 16-digit id), 0 = none */
    while (*p == ' ' || *p == '\t') p++;
    char *e = NULL;
    unsigned long long v;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) v = strtoull(p + 2, &e, 16);
    else { size_t nd = strspn(p, "0123456789abcdefABCDEF"); v = nd == 16 ? strtoull(p, &e, 16) : strtoull(p, &e, 10); }
    if (!e || e == p) return 0;
    if (v > 0xfff) v = v > 0x01006A800016E000ULL + 0x1000 && v < 0x01006A800016E000ULL + 0x2000 ? v - 0x01006A800016E000ULL - 0x1000 : 0;
    return v > 0 && v < 0x1000 ? (int)v : 0;
}
static void dlc_load(void) {
    const char *e = getenv("SSBU_DLC_LIST");
    if (e && e[0]) snprintf(g_dlc_path, sizeof g_dlc_path, "%s", e);
    else { char exe[MAX_PATH], tmp[MAX_PATH + 32]; GetModuleFileNameA(NULL, exe, MAX_PATH); char *s = strrchr(exe, '\\'); if (s) *s = 0; snprintf(tmp, sizeof tmp, "%s\\..\\config\\dlc.txt", exe); if (!_fullpath(g_dlc_path, tmp, MAX_PATH)) snprintf(g_dlc_path, sizeof g_dlc_path, "%s", tmp); }
    free(g_dlc); g_dlc = NULL; g_dlc_n = 0;
    FILE *f = fopen(g_dlc_path, "r");
    char ln[256];
    int cap = 0;
    while (f && fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        if (g_dlc_n == cap) { cap = cap ? cap * 2 : 64; g_dlc = (DlcLine *)realloc(g_dlc, sizeof(DlcLine) * (size_t)cap); }
        DlcLine *d = &g_dlc[g_dlc_n++];
        memset(d, 0, sizeof *d);
        snprintf(d->text, sizeof d->text, "%s", ln);
        const char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncmp(p, "#off ", 5) && dlc_entry(p + 5)) { d->entry = 1; d->on = 0; d->index = dlc_entry(p + 5); }
        else if (*p != '#' && dlc_entry(p)) { d->entry = 1; d->on = 1; d->index = dlc_entry(p); }
    }
    if (f) fclose(f);
    g_dlc_loaded = 1; g_dlc_dirty = 0;
}
static void dlc_save(void) {
    char bak[MAX_PATH + 8];
    snprintf(bak, sizeof bak, "%s.bak", g_dlc_path);
    CopyFileA(g_dlc_path, bak, FALSE);
    FILE *f = fopen(g_dlc_path, "w");
    if (!f) { printf("menu: %s could NOT be written\n", g_dlc_path); return; }
    for (int i = 0; i < g_dlc_n; i++) {
        DlcLine *d = &g_dlc[i];
        const char *p = d->text;
        while (*p == ' ' || *p == '\t') p++;
        if (!d->entry) fprintf(f, "%s\n", d->text);
        else if (!strncmp(p, "#off ", 5)) fprintf(f, d->on ? "%s\n" : "#off %s\n", p + 5);
        else fprintf(f, d->on ? "%s\n" : "#off %s\n", p);
    }
    fclose(f);
    printf("menu: %s written (old copy: %s); the game reads it at the next start\n", g_dlc_path, bak);
    fflush(stdout);
    dlc_load();
}

/* ------------------------------------------------------------------ the ImGui side */
static int g_im_ready, g_section, g_section_req = -1, g_capture = -1;
static float g_base_h;
static unsigned char *g_font_px;
static int g_font_w, g_font_h;
static ImVector<ImDrawVert> g_vtx;
static ImVector<ImDrawIdx> g_idx;
static ImVector<MenuCmd> g_cmd;
static LARGE_INTEGER g_qpf, g_last_t;
static float g_ft[120];
static int g_ft_pos;
static uint64_t g_pad_prev;
static long long g_closed_at, g_frame;
static char g_pads[1024];
static ULONGLONG g_pads_at;
enum { SEC_DISPLAY, SEC_GRAPHICS, SEC_AUDIO, SEC_CONTROLS, SEC_EXPERIMENTAL, SEC_ROOMS, SEC_DLC, SEC_LANG, SEC_LOG, SEC_ABOUT, SEC_N };
static const char *const k_sections[SEC_N] = {"Display", "Graphics", "Audio", "Controls", "Experimental", "Online", "Add-on content", "Language", "Log", "About"};

static ImGuiKey vk_key(int vk) {
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab; case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow; case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow; case VK_PRIOR: return ImGuiKey_PageUp; case VK_NEXT: return ImGuiKey_PageDown; case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End; case VK_INSERT: return ImGuiKey_Insert; case VK_DELETE: return ImGuiKey_Delete; case VK_BACK: return ImGuiKey_Backspace;
    case VK_SPACE: return ImGuiKey_Space; case VK_RETURN: return ImGuiKey_Enter; case VK_ESCAPE: return ImGuiKey_Escape;
    case VK_SHIFT: return ImGuiKey_LeftShift; case VK_CONTROL: return ImGuiKey_LeftCtrl; case VK_MENU: return ImGuiKey_LeftAlt;
    }
    return ImGuiKey_None;
}

static void im_init(int h) {
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL; io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "ssbu_menu (display.c window messages)";
    io.BackendRendererName = "nvn_vk_menu (Vulkan, dynamic rendering)";
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 6.0f; st.FrameRounding = 4.0f; st.GrabRounding = 4.0f; st.WindowBorderSize = 1.0f;
    st.Colors[ImGuiCol_WindowBg].w = 0.92f;
    float px = (float)h / 40.0f;
    if (px < 15.0f) px = 15.0f;
    if (px > 40.0f) px = 40.0f;
    g_base_h = (float)h;
#ifdef _WIN32
    char font[MAX_PATH];
    UINT n = GetWindowsDirectoryA(font, MAX_PATH);
    snprintf(font + n, sizeof font - n, "\\Fonts\\segoeui.ttf");
    ImFont *f = GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES ? io.Fonts->AddFontFromFileTTF(font, px) : NULL;
#else
    /* a sans-serif font of the system (Debian / Ubuntu, Fedora, Arch / SteamOS, macOS), else ImGui's own */
    static const char *const fonts[] = {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf"};
    char font[MAX_PATH] = "";
    ImFont *f = NULL;
    for (size_t i = 0; i < sizeof fonts / sizeof fonts[0] && !f; i++) {
        struct stat st;
        if (stat(fonts[i], &st) == 0 && S_ISREG(st.st_mode)) { snprintf(font, sizeof font, "%s", fonts[i]); f = io.Fonts->AddFontFromFileTTF(font, px); }
    }
    ImGui::GetPlatformIO().Platform_SetClipboardTextFn = clip_set;      /* the system clipboard, through the window thread */
    ImGui::GetPlatformIO().Platform_GetClipboardTextFn = clip_get;
#endif
    if (!f) { ImFontConfig c; c.SizePixels = px; io.Fonts->AddFontDefault(&c); }
    st.ScaleAllSizes(px / 15.0f);
    int bpp = 0;
    io.Fonts->GetTexDataAsRGBA32(&g_font_px, &g_font_w, &g_font_h, &bpp);
    io.Fonts->SetTexID((ImTextureID)1);
    QueryPerformanceFrequency(&g_qpf);
    g_im_ready = 1;
    printf("menu: Dear ImGui %s ready: font %s %.0f px, atlas %dx%d\n", ImGui::GetVersion(), f ? font : "(built-in)", px, g_font_w, g_font_h);
    fflush(stdout);
}
extern "C" const unsigned char *menu_font_rgba(int *w, int *h) { *w = g_font_w; *h = g_font_h; return g_font_px; }

/* the pad, read every frame (also while the menu is closed: the combination that opens it) */
static void pad_feed(ImGuiIO &io, uint64_t b) {
    struct { int bit; ImGuiKey k; } m[] = {
        {B_A, ImGuiKey_GamepadFaceDown}, {B_B, ImGuiKey_GamepadFaceRight}, {B_Y, ImGuiKey_GamepadFaceLeft}, {B_X, ImGuiKey_GamepadFaceUp},
        {B_ZL, ImGuiKey_GamepadL2}, {B_ZR, ImGuiKey_GamepadR2}, {B_LEFT, ImGuiKey_GamepadDpadLeft}, {B_RIGHT, ImGuiKey_GamepadDpadRight},
        {B_UP, ImGuiKey_GamepadDpadUp}, {B_DOWN, ImGuiKey_GamepadDpadDown}};
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) io.AddKeyEvent(m[i].k, (b & BIT(m[i].bit)) != 0);
}

static void tag(int restart, int changed) {
    ImGui::SameLine();
    if (!restart) ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.45f, 1.0f), "[live]");
    else ImGui::TextColored(changed ? ImVec4(1.0f, 0.75f, 0.2f, 1.0f) : ImVec4(0.6f, 0.6f, 0.6f, 1.0f), changed ? "[restart: changed]" : "[restart]");
}
static bool combo(const char *label, int *v, const char *const *names, int n) { return ImGui::Combo(label, v, names, n); }
static void help(const char *s) { ImGui::PushTextWrapPos(0.0f); ImGui::TextDisabled("%s", s); ImGui::PopTextWrapPos(); }

static void sec_display(int cw, int ch) {
    int wm = display_window_mode();
    S.window = wm;
    if (combo("Window", &S.window, k_window, N(k_window))) { display_set_window_mode(S.window); ini("window", k_window[S.window]); }
    tag(0, 0);
    {   /* a screen that is not k x 60 Hz (75, 144, 165 Hz ...): the game is 60 frames per second, so frames cannot all stay
         * on screen equally long and motion stutters (testers, 2026-10-20: "frame pacing is terrible, 144 Hz screen") */
        double hz; int k; display_refresh_info(&hz, &k);
        int hfr = display_hfr_mode();
        int target_hz = display_hfr_hz();
        static const char *const k_hfr[] = {
            "Off (60 Hz standard)",
            "120 Hz Fixed (8.33 ms)",
            "144 Hz Fixed (6.94 ms)",
            "165 Hz Fixed (6.06 ms)",
            "240 Hz Fixed (4.17 ms)"
        };
        if (combo("Fixed Render Cadence", &hfr, k_hfr, N(k_hfr))) display_set_hfr_mode(hfr);
        tag(0, 0);
        ImGui::SameLine(); ImGui::TextDisabled("(F10 cycles in game)");
        if (hfr > 0) {
            ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "Fixed %d Hz cadence active (%.2f ms render period, exact 60.000 Hz game logic, VSync off).", target_hz, 1000.0 / (double)target_hz);
        } else if (hz > 30 && k == 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1), "Your screen runs at %.0f Hz: not a multiple of 60, so motion is uneven.", hz);
#ifdef _WIN32
            help("Turn on High refresh rate (F10) above, or set the screen to 120 Hz (or 60 Hz) in Windows Settings > Display > "
                 "Advanced display > Refresh rate. Or turn on G-Sync / FreeSync and play in fullscreen.");
#else
            help("Turn on High refresh rate (F10) above, or set the screen to 120 Hz (or 60 Hz) in the system's display settings. "
                 "Or turn on G-Sync / FreeSync (VRR) and play in fullscreen.");
#endif
        } else if (hz > 30) ImGui::TextDisabled("Screen %.0f Hz: locked, %d refresh%s per game frame.", hz, k, k > 1 ? "es" : "");
    }
#ifdef _WIN32
    help("F11 also switches between the window and borderless at any time. Fullscreen = borderless and always on top (no display mode change).");
#else
    help("Borderless = a window without a frame over the whole screen; fullscreen = the desktop's fullscreen (no display mode change).");
#endif
    int sz = S.size < 0 ? 0 : S.size;
    if (combo("Window size", &sz, k_size_name, N(k_size_name))) {
        S.size = sz;
        char v[16];
        display_set_client_size(k_sizes[sz][0], k_sizes[sz][1]);
        snprintf(v, sizeof v, "%d", k_sizes[sz][0]); ini("width", v);
        snprintf(v, sizeof v, "%d", k_sizes[sz][1]); ini("height", v);
    }
    tag(0, 0);
    ImGui::TextDisabled("now %d x %d (the windowed size is kept for later when borderless)", cw, ch);
    ImGui::Separator();
    if (combo("Vsync / pacing", &S.vsync, k_vsync, N(k_vsync))) ini("vsync", k_vsync[S.vsync]);
    tag(1, S.vsync != S0.vsync);
    help("auto: 'display' on a 60/120/240 Hz monitor, else 'clock'. display: the game's clock is the monitor's refresh. clock: our own 60 Hz clock. off: no wait, tearing. on: FIFO (two clocks). mailbox: as clock, mailbox only.");
    if (combo("Input latency", &S.latency, k_latency, N(k_latency))) ini("latency", k_latency[S.latency]);
    tag(1, S.latency != S0.latency);
    if (combo("Renderer", &S.renderer, k_renderer, N(k_renderer))) ini("renderer", k_renderer[S.renderer]);
    tag(1, S.renderer != S0.renderer);
    ImGui::InputText("GPU", S.gpu, sizeof S.gpu);
    if (ImGui::IsItemDeactivatedAfterEdit()) ini("gpu", S.gpu[0] ? S.gpu : "auto");
    tag(1, strcmp(S.gpu, S0.gpu) != 0);
    ImGui::TextDisabled("in use: %s   (auto, a number of the list in the log, or a part of the name)", nvn_vk_device_name());
}
static void sec_graphics(void) {
    if (ImGui::SliderFloat("Resolution scale", &S.scale, 0.25f, 2.0f, "%.2f")) S.scale = floorf(S.scale * 20.0f + 0.5f) / 20.0f;
    if (ImGui::IsItemDeactivatedAfterEdit()) { char v[16]; snprintf(v, sizeof v, "%.2f", S.scale); ini("resolution_scale", v); }
    tag(1, fabsf(S.scale - S0.scale) > 0.001f);
    ImGui::TextDisabled("1.00 = 1920 x 1080 inside; 0.50 = 960 x 540 (weak GPU); above 1 = supersampling (strong GPU)");
    if (combo("Anisotropic filtering", &S.aniso, k_aniso, N(k_aniso))) ini("anisotropic_filtering", k_aniso[S.aniso]);
    tag(1, S.aniso != S0.aniso);
    if (combo("Profile", &S.profile, k_profile, N(k_profile))) ini("profile", k_profile[S.profile]);
    tag(1, S.profile != S0.profile);
    ImGui::TextDisabled("low = scale 0.50, filtering game, vsync clock (those three keys are then not used)");
    if (combo("Shader wait", &S.wait, k_wait, N(k_wait))) ini("shader_wait", k_wait[S.wait]);
    tag(1, S.wait != S0.wait);
    if (combo("Shader preparation", &S.prepare, k_prepare, N(k_prepare))) ini("shader_prepare", k_prepare[S.prepare]);
    tag(1, S.prepare != S0.prepare);
    if (combo("Shader loading", &S.load, k_load, N(k_load))) ini("shader_load", k_load[S.load]);
    tag(1, S.load != S0.load);
    if (combo("Console mode", &S.console, k_console, N(k_console))) ini("console_mode", k_console[S.console]);
    tag(1, S.console != S0.console);
    bool ov = S.overlay != 0;
    if (ImGui::Checkbox("Frame time overlay", &ov)) apply_overlay(ov);
    tag(0, 0);
    ImGui::TextDisabled("in the picture (top left), in the window title and as a 'frame-time:' log line every second");
    bool so = S.stats != 0;
    if (ImGui::Checkbox("Stats overlay (F2)", &so)) apply_stats(so);
    tag(0, 0);
    ImGui::TextDisabled("top right: fps, frame time, online state, ping, input delay, rollback, run-ahead");
}
static void sec_audio(void) {
    int cur = nnaudio_volume();
    int v = S.volume < 0 ? (cur >= 0 ? cur : 100) : S.volume;
    const bool dis_1 = cur < 0;
    if (dis_1) ImGui::BeginDisabled();
    if (ImGui::SliderInt("Volume", &v, 0, 100, "%d %%")) { S.volume = v; nnaudio_set_volume(v); }
    if (ImGui::IsItemDeactivatedAfterEdit()) { char t[8]; snprintf(t, sizeof t, "%d", S.volume); ini("volume", t); }
    if (dis_1) ImGui::EndDisabled();
    tag(0, 0);
    if (cur < 0) help("No sound device in this copy (test copies, SSBU_AUDIO_MUTE): the volume is saved for the copies a person plays.");
    if (ImGui::Button("Volume: auto (100 %)")) { S.volume = -1; nnaudio_set_volume(100); ini("volume", "auto"); }
    ImGui::Separator();
    help("Music and sound effects apart: the game's own Options > Sound (it mixes them itself; the PC gets the finished mix, and this volume scales all of it).");
}
/* ------------------------------------------------------------------ the Controls page: remapping of the keyboard and of every
 * controller (nnpad_sdl.c writes controllers.ini; all of it is used at once). The game's own actions next to each control are
 * the game's DEFAULT controls (its Options > Controls can change them per player name). */
static const char *const k_target_label[24] = {"A", "B", "X", "Y", "L-stick click", "R-stick click", "L", "R", "ZL", "ZR", "+ (Plus)", "- (Minus)",
    "D-pad left", "D-pad up", "D-pad right", "D-pad down", "Left stick left", "Left stick up", "Left stick right", "Left stick down",
    "Right stick left", "Right stick up", "Right stick right", "Right stick down"};
static const char *const k_action[24] = {"attack", "special", "jump", "jump", "-", "-", "grab", "grab", "shield", "shield", "pause", "pause",
    "taunt", "taunt", "taunt", "taunt", "move", "move / jump", "move", "move / crouch", "smash attack", "smash attack", "smash attack", "smash attack"};
static const char *const k_display_order_note = "Order: the Switch Pro Controller's buttons, then the d-pad, then the two sticks.";
static int g_ctl_dev = -1;          /* -1 = the keyboard, else the index of a controller (nnpad_list) */
static int g_ctl_scope = 1;         /* 1 = every controller of this kind, 0 = this controller only */
static int g_pcap = -1;             /* a controller target waiting for "press the button / stick direction" */
static ULONGLONG g_cap_t0;          /* when the waiting (keyboard or controller) began */
#define CAPTURE_MS 6000
static NnPadDev g_devs[16];
static int g_ndev;
static ULONGLONG g_devs_at;
static void capture_cancel(const char *why) {
    if (g_capture >= 0 || g_pcap >= 0) { printf("menu: rebinding of %s cancelled (%s)\n", nnpad_target_name(g_capture >= 0 ? g_capture : g_pcap), why); fflush(stdout); }
    g_capture = -1;
    if (g_pcap >= 0) nnpad_capture_end();
    g_pcap = -1;
}
static int capturing(void) { return g_capture >= 0 || g_pcap >= 0; }
static const NnPadDev *ctl_dev(void) { for (int i = 0; i < g_ndev; i++) if (g_devs[i].index == g_ctl_dev) return &g_devs[i]; return NULL; }
/* once per frame (render thread): the controller capture and the time-outs */
static void capture_tick(uint64_t down) {
    if (!capturing()) return;
    ULONGLONG now = GetTickCount64();
    if (now - g_cap_t0 > CAPTURE_MS) { capture_cancel("time-out"); return; }
    if (g_capture >= 0 && (down & BIT(B_B))) { capture_cancel("B on a controller"); return; }      /* a keyboard key is wanted: B of a pad cancels */
    if (g_pcap >= 0) {
        char src[32];
        int r = nnpad_capture_poll(src, sizeof src);
        if (r == 1) {
            nnpad_target_set(g_ctl_dev, g_ctl_scope, g_pcap, src);
            printf("menu: %s of controller %d bound to %s (%s)\n", nnpad_target_name(g_pcap), g_ctl_dev, src, g_ctl_scope ? "its type profile" : "this controller only"); fflush(stdout);
            nnpad_capture_end(); g_pcap = -1;
        } else if (r == 2) capture_cancel("Home / Capture");
        else if (r < 0) capture_cancel("the controller is gone");
    }
}
static void test_area(int player) {
    int st[4];
    uint64_t b = player >= 0 ? nnhid_preview(player, st) : 0;
    if (player < 0) { ImGui::TextDisabled("Test: this device is not a player now."); return; }
    ImGui::Text("Test (player %d, what the game would get):", player + 1);
    static const char *const sh[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "+", "-", "<", "^", ">", "v"};
    for (int i = 0; i < 16; i++) {
        bool on = (b >> i & 1) != 0;
        if (i) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.95f, 0.55f, 0.15f, 1) : ImVec4(0.25f, 0.25f, 0.28f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.95f, 0.55f, 0.15f, 1) : ImVec4(0.25f, 0.25f, 0.28f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, on ? ImVec4(0.95f, 0.55f, 0.15f, 1) : ImVec4(0.25f, 0.25f, 0.28f, 1));
        ImGui::PushID(1000 + i);
        ImGui::SmallButton(sh[i]);
        ImGui::PopID();
        ImGui::PopStyleColor(3);
    }
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = ImGui::GetFontSize() * 1.6f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    for (int s = 0; s < 2; s++) {
        ImVec2 c(p.x + r + s * (r * 2.6f), p.y + r);
        dl->AddCircle(c, r, IM_COL32(160, 160, 170, 255), 32, 1.5f);
        dl->AddCircleFilled(ImVec2(c.x + r * st[s * 2] / 32767.0f, c.y - r * st[s * 2 + 1] / 32767.0f), r * 0.18f, IM_COL32(240, 140, 40, 255));
    }
    ImGui::Dummy(ImVec2(r * 4.6f, r * 2.0f));
    ImGui::SameLine();
    ImGui::Text("left (%6d, %6d)\nright (%6d, %6d)", st[0], st[1], st[2], st[3]);
}
static void sec_controls(void) {
    if (GetTickCount64() - g_devs_at > 500) { g_devs_at = GetTickCount64(); g_ndev = nnpad_list(g_devs, 16); }
    const NnPadDev *dv = ctl_dev();
    if (g_ctl_dev >= 0 && !dv) { capture_cancel("the controller is gone"); g_ctl_dev = -1; }
    char cur[160];
    if (dv) snprintf(cur, sizeof cur, "%s (%s)%s", dv->name, nnpad_type_label(dv->type), dv->player >= 0 ? "" : " - no player");
    else snprintf(cur, sizeof cur, "Keyboard");
    const bool dis_2 = capturing();
    if (dis_2) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("Device", cur)) {
        if (ImGui::Selectable("Keyboard", g_ctl_dev < 0)) g_ctl_dev = -1;
        for (int i = 0; i < g_ndev; i++) {
            char l[200];
            if (g_devs[i].player >= 0) snprintf(l, sizeof l, "Player %d: %s (%s)##%d", g_devs[i].player + 1, g_devs[i].name, nnpad_type_label(g_devs[i].type), g_devs[i].index);
            else snprintf(l, sizeof l, "No player: %s (%s)##%d", g_devs[i].name, nnpad_type_label(g_devs[i].type), g_devs[i].index);
            if (ImGui::Selectable(l, g_ctl_dev == g_devs[i].index)) g_ctl_dev = g_devs[i].index;
        }
        ImGui::EndCombo();
    }
    if (dis_2) ImGui::EndDisabled();
    tag(0, 0);
    if (!g_ndev) { ImGui::SameLine(); ImGui::TextDisabled("(no controller connected)"); }
    dv = ctl_dev();
    static const char *const kp[] = {"auto", "nobody", "1", "2", "3", "4", "5", "6", "7", "8"};
    static const char *const pp[] = {"auto", "1", "2", "3", "4", "5", "6", "7", "8"};
    if (!dv) {
        int p = S.kb_player + 2;
        if (combo("Keyboard plays as player", &p, kp, N(kp))) { S.kb_player = p - 2; S0.kb_player = S.kb_player; nnpad_kb_player_set(S.kb_player); nnhid_kb_player_refresh(); }
        tag(0, 0);
        ImGui::SameLine(); ImGui::TextDisabled("now: %s", nnhid_kb_player() >= 0 ? kp[nnhid_kb_player() + 2] : "nobody");
    } else {
        int p = nnpad_player_cfg(dv->index) + 1;
        if (combo("Player", &p, pp, N(pp))) nnpad_player_assign(dv->index, p - 1);
        tag(0, 0);
        ImGui::SameLine(); ImGui::TextDisabled("now: %s", dv->player >= 0 ? pp[dv->player + 1] : "none");
        char every[64];
        snprintf(every, sizeof every, "Every %s controller", nnpad_type_label(dv->type));
        if (ImGui::RadioButton(every, g_ctl_scope == 1)) g_ctl_scope = 1;
        ImGui::SameLine();
        if (ImGui::RadioButton("This controller only", g_ctl_scope == 0)) g_ctl_scope = 0;
        ImGui::SameLine();
        ImGui::TextDisabled("changes are saved for: %s", g_ctl_scope ? "its kind (a profile used whenever one is connected)" : "this controller (its GUID)");
    }
    ImGui::TextDisabled("%s   %s", nnpad_config_path(), k_display_order_note);
    if (g_capture >= 0) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Press the KEY for %s ... %d s (Esc or B on a controller = cancel)", k_target_label[g_capture], (int)((CAPTURE_MS - (GetTickCount64() - g_cap_t0)) / 1000) + 1);
    if (g_pcap >= 0) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "Press the BUTTON or push the STICK / TRIGGER for %s ... %d s (Esc or Home = cancel)", k_target_label[g_pcap], (int)((CAPTURE_MS - (GetTickCount64() - g_cap_t0)) / 1000) + 1);
    float tbl_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * (dv ? 7.4f : 4.6f);
    if (tbl_h < ImGui::GetFrameHeight() * 4) tbl_h = ImGui::GetFrameHeight() * 4;
    if (ImGui::BeginTable("map", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, tbl_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Switch control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("In the game (default)", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(dv ? "Bound to" : "Key", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < 24; i++) {
            char name[96];
            int from = 0;
            if (dv) from = nnpad_target_text(dv->index, i, name, sizeof name); else nnpad_vk_name(nnpad_kb_vk(i), name, sizeof name);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(k_target_label[i]);
            ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", k_action[i]);
            ImGui::TableSetColumnIndex(2);
            if ((!dv && g_capture == i) || (dv && g_pcap == i)) ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "... waiting");
            else if (dv) { ImGui::TextUnformatted(name); ImGui::SameLine(); ImGui::TextDisabled(from == 2 ? "(this controller)" : from == 1 ? "(profile)" : "(default)"); }
            else ImGui::TextUnformatted(name);
            ImGui::TableSetColumnIndex(3);
            const bool dis_3 = capturing();
            if (dis_3) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Change")) {
                g_cap_t0 = GetTickCount64();
                if (dv) { g_pcap = i; nnpad_capture_begin(dv->index); } else g_capture = i;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear")) { if (dv) nnpad_target_set(dv->index, g_ctl_scope, i, "none"); else nnpad_kb_bind(i, 0); }
            ImGui::SameLine();
            if (ImGui::SmallButton("Default")) { if (dv) nnpad_target_set(dv->index, g_ctl_scope, i, NULL); else nnpad_kb_bind(i, nnpad_kb_default(i)); }
            if (dis_3) ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    const bool dis_4 = capturing();
    if (dis_4) ImGui::BeginDisabled();
    if (dv) {
        char rl[96];
        snprintf(rl, sizeof rl, "Reset %s to the default", g_ctl_scope ? "the profile" : "this controller");
        if (ImGui::Button(rl)) nnpad_profile_reset(dv->index, g_ctl_scope);
        float tv[3];
        nnpad_tune_get(dv->index, tv);
        static const char *const tn[3] = {"Stick dead zone", "Stick outer zone (lower = more sensitive)", "Trigger threshold (ZL / ZR)"};
        static const float lo[3] = {0.0f, 0.5f, 0.05f}, hi[3] = {0.5f, 1.0f, 0.95f};
        static float edit[3];
        static int editing = -1;
        for (int k = 0; k < 3; k++) {
            float v = editing == k ? edit[k] : tv[k];
            if (ImGui::SliderFloat(tn[k], &v, lo[k], hi[k], "%.2f")) { edit[k] = v; editing = k; }
            if (ImGui::IsItemDeactivatedAfterEdit()) { nnpad_tune_set(dv->index, g_ctl_scope, k, v); editing = -1; }
        }
    } else {
        if (ImGui::Button("All keys back to the default")) for (int i = 0; i < 24; i++) nnpad_kb_bind(i, nnpad_kb_default(i));
    }
    if (dis_4) ImGui::EndDisabled();
    ImGui::Separator();
    test_area(dv ? dv->player : nnhid_kb_player());
}
/* ------------------------------------------------------------------ the Online page (net_relay.c / nnldn.c, docs/ONLINE.md)
 * The online code reads its graphics.ini keys once at the start (nnldn_register replaces the game's nn::ldn imports at boot
 * only when online play is on), so every control here is [restart]; "Restart the game now" starts this exe again with the
 * same command line and ends this copy (net_start of the new copy waits for the old one to be gone). */
#define ONLINE_DEFAULT_LOBBY "opensmash-lobby.duckdns.org:39500"      /* the project's public lobby server (host:port); a domain name can replace it */
static const char *const k_online_mode[] = {"off", "lan", "host", "join", "lobby"};
static const char *const k_online_name[] = {"Off", "Same network (LAN)", "Host a room (room code)", "Join with a code", "Lobby server (recommended)"};
static struct { int mode, last_mode; bool upnp, relay_only; char join[64], server[128]; } O, O0;
static void sec_online(void);
static int g_online_inited;
static volatile LONG g_osk_busy;
static char g_osk_text[64];
static volatile LONG g_osk_done;
static int ini_read(const char *key, char *out, int n) {       /* "key = value" of graphics.ini (the online code's own reading rule) */
    FILE *f = fopen(gfx_options()->path, "r");
    char ln[512];
    int found = 0;
    out[0] = 0;
    while (f && !found && fgets(ln, sizeof ln, f)) {
        char *s = ln;
        while (*s == ' ' || *s == '\t') s++;
        size_t kl = strlen(key);
        if (_strnicmp(s, key, kl) || (s[kl] != ' ' && s[kl] != '\t' && s[kl] != '=')) continue;
        s += kl;
        while (*s == ' ' || *s == '\t') s++;
        if (*s++ != '=') continue;
        while (*s == ' ' || *s == '\t') s++;
        char *e = s + strcspn(s, ";#\r\n");
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) e--;
        *e = 0;
        snprintf(out, (size_t)n, "%s", s);
        found = 1;
    }
    if (f) fclose(f);
    return found;
}
static int off_word(const char *v) { return !*v || !strcmp(v, "0") || !_stricmp(v, "off") || !_stricmp(v, "no") || !_stricmp(v, "false"); }
static void online_init(void) {
    char v[128];
    memset(&O, 0, sizeof O);
    O.upnp = 1;
    if (ini_read("online_mode", v, sizeof v)) {
        if (off_word(v)) O.mode = 0;
        else if (!_stricmp(v, "lan")) O.mode = 1;
        else if (!_strnicmp(v, "host", 4)) O.mode = 2;
        else if (!_strnicmp(v, "join", 4)) { O.mode = 3; if (v[4] == ':') snprintf(O.join, sizeof O.join, "%s", v + 5); }
        else if (!_stricmp(v, "1") || !_stricmp(v, "on") || !_stricmp(v, "auto")) O.mode = ini_read("online_server", v, sizeof v) && !off_word(v) ? 4 : 1;
        else O.mode = 4;
    } else if (!ini_read("online_server", v, sizeof v) || !off_word(v)) O.mode = 4;   /* nothing in the file: ON with the public lobby (2026-10-20) */
    if (!O.join[0]) ini_read("online_join", O.join, sizeof O.join);
    if (!ini_read("online_server", O.server, sizeof O.server) || off_word(O.server)) snprintf(O.server, sizeof O.server, "%s", ONLINE_DEFAULT_LOBBY);
    if (ini_read("online_upnp", v, sizeof v)) O.upnp = !off_word(v);
    if (ini_read("online_direct", v, sizeof v)) O.relay_only = off_word(v);
    /* what this run started with: the online code's own answer (SSBU_ONLINE wins over the file) */
    O.last_mode = O.mode;
    O0 = O;
    O0.mode = !net_online() ? 0 : net_mode() == NET_MODE_LAN ? 1 : net_mode() == NET_MODE_HOST ? 2 : net_mode() == NET_MODE_JOIN ? 3 : 4;
    g_online_inited = 1;
}
static void online_save(void) {
    ini("online_mode", k_online_mode[O.mode]);
    if (O.mode == 3) ini("online_join", O.join[0] ? O.join : "");
    if (O.mode == 4) ini("online_server", O.server[0] ? O.server : "off");
    ini("online_upnp", O.upnp ? "on" : "off");
    ini("online_direct", O.relay_only ? "off" : "on");
    printf("menu: online settings written: online_mode = %s%s%s (used at the next start)\n", k_online_mode[O.mode], O.mode == 3 ? ", online_join = " : O.mode == 4 ? ", online_server = " : "",
           O.mode == 3 ? O.join : O.mode == 4 ? O.server : "");
    fflush(stdout);
}
#ifdef _WIN32
static DWORD WINAPI osk_code_thread(LPVOID) {             /* the on-screen keyboard for the room code (it blocks: not on the render thread) */
    wchar_t init[64], out[64] = L"";
    MultiByteToWideChar(CP_UTF8, 0, O.join, -1, init, 64);
    int r = osk_run(L"Room code", L"The code the host's game shows (like 1A2B3-C4D5E)", init, 32, 0, 0, out);
    if (r == 1) WideCharToMultiByte(CP_UTF8, 0, out, -1, g_osk_text, sizeof g_osk_text, NULL, NULL);
    InterlockedExchange(&g_osk_done, r == 1 ? 1 : 2);
    InterlockedExchange(&g_osk_busy, 0);
    return 0;
}
static char g_osk_name[64];
static volatile LONG g_osk_name_done;
static DWORD WINAPI osk_name_thread(LPVOID arg) {          /* the on-screen keyboard for the player's name */
    wchar_t init[64], out[64] = L"";
    MultiByteToWideChar(CP_UTF8, 0, (const char *)arg, -1, init, 64);
    int r = osk_run(L"Your name", L"The name other players see", init, 32, 0, 0, out);
    if (r == 1) WideCharToMultiByte(CP_UTF8, 0, out, -1, g_osk_name, sizeof g_osk_name, NULL, NULL);
    InterlockedExchange(&g_osk_name_done, r == 1 ? 1 : 2);
    InterlockedExchange(&g_osk_busy, 0);
    return 0;
}
static DWORD WINAPI restart_thread(LPVOID) { Sleep(100); exit(0); return 0; }      /* the exit handlers (UPnP, lobby) run */
static void restart_game(void) {
    wchar_t dir[MAX_PATH], pid[16];
    GetCurrentDirectoryW(MAX_PATH, dir);
    swprintf(pid, 16, L"%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"SSBU_RESTART_WAIT_PID", pid);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    HANDLE h[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE), GetStdHandle(STD_ERROR_HANDLE)};
    for (int i = 0; i < 3; i++) if (h[i] && h[i] != INVALID_HANDLE_VALUE) SetHandleInformation(h[i], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = h[0]; si.hStdOutput = h[1]; si.hStdError = h[2];
    wchar_t *cmd = _wcsdup(GetCommandLineW());
    printf("menu: restarting the game: %ls\n", cmd);
    fflush(stdout);
    if (CreateProcessW(NULL, cmd, NULL, NULL, TRUE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        CreateThread(NULL, 0, restart_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, [](LPVOID) -> DWORD { Sleep(5000); ExitProcess(0); return 0; }, NULL, 0, NULL);      /* if an exit handler hangs */
    } else { printf("menu: the restart FAILED (error %lu): start the game again by hand\n", GetLastError()); fflush(stdout); }
    free(cmd);
}
static int g_ra_build = -1;               /* the run-ahead pieces are there: a loaded game DLL with the r1_m1 marks (r1_state.c mark_bind) */
static int ra_build(void) {
    if (g_ra_build < 0) {
        HMODULE mods[512];
        DWORD need = 0;
        g_ra_build = 0;
        if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need))
            for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 512; i++) if (mods[i] != GetModuleHandleA(NULL) && GetProcAddress(mods[i], "r1_m1")) g_ra_build = 1;      /* (the exe has its own r1_m1: not a sign) */
    }
    return g_ra_build;
}
#else
static DWORD WINAPI osk_code_thread(LPVOID) {             /* the room code from the desktop's text prompt (it blocks: not on the render thread) */
    char out[256] = "";
    int r = text_prompt("Room code (SSBU native)", "The code the host's game shows (like 1A2B3-C4D5E)", O.join, out, sizeof out);
    if (r == 1) { utf8_cut(out, 32); snprintf(g_osk_text, sizeof g_osk_text, "%s", out); }
    InterlockedExchange(&g_osk_done, r == 1 ? 1 : 2);
    InterlockedExchange(&g_osk_busy, 0);
    return 0;
}
static char g_osk_name[64];
static volatile LONG g_osk_name_done;
static DWORD WINAPI osk_name_thread(LPVOID arg) {          /* the player's name from the desktop's text prompt */
    char out[256] = "";
    int r = text_prompt("Your name (SSBU native)", "The name other players see", (const char *)arg, out, sizeof out);
    if (r == 1) { utf8_cut(out, 32); snprintf(g_osk_name, sizeof g_osk_name, "%s", out); }
    InterlockedExchange(&g_osk_name_done, r == 1 ? 1 : 2);
    InterlockedExchange(&g_osk_busy, 0);
    return 0;
}
/* (2026-10-21, a Linux tester: Exit game / Restart -> SIGSEGV, then the process stayed and grew until the OOM killer) exit()
 * ran the exit handlers while the game's threads still ran: a thread jumped into what they had freed. Here (Linux / macOS:
 * no UPnP or lobby handlers to run) the end is report_and_exit: the reports, the log flushed, then _exit, as the window's X does. */
extern "C" void report_and_exit(int code);                 /* hostrt.c */
static DWORD WINAPI restart_thread(LPVOID) { Sleep(100); report_and_exit(0); return 0; }
static void restart_game(void) {                           /* this exe again, same arguments, environment and standard handles */
    char pid[16], exe[MAX_PATH], path[MAX_PATH];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    setenv("SSBU_RESTART_WAIT_PID", pid, 1);
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    plat_path(exe, path, sizeof path);
    printf("menu: restarting the game: %s\n", path);
    fflush(stdout);
    pid_t child;
    int e = posix_spawn(&child, path, NULL, NULL, __argv, environ);
    if (!e) {
        CreateThread(NULL, 0, restart_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, [](LPVOID) -> DWORD { Sleep(5000); _exit(0); return 0; }, NULL, 0, NULL);      /* if an exit handler hangs */
    } else { printf("menu: the restart FAILED (error %d): start the game again by hand\n", e); fflush(stdout); }
}
static int g_ra_build = -1;               /* the run-ahead pieces are there: a loaded game library with the r1_m1 marks (r1_state.c mark_bind) */
static int ra_has_mark(const char *name) { /* (the exe has its own r1_m1: not a sign; it is never asked) */
    void *h = name && name[0] ? dlopen(name, RTLD_LAZY | RTLD_NOLOAD) : NULL;
    int r = h && dlsym(h, "r1_m1") != NULL;
    if (h) dlclose(h);
    return r;
}
#ifndef __APPLE__
static int ra_phdr(struct dl_phdr_info *i, size_t n, void *arg) { (void)n; (void)arg; return ra_has_mark(i->dlpi_name); }
#endif
static int ra_build(void) {
    if (g_ra_build < 0) {
#ifdef __APPLE__
        g_ra_build = 0;
        for (uint32_t i = 1; i < _dyld_image_count() && !g_ra_build; i++) g_ra_build = ra_has_mark(_dyld_get_image_name(i));      /* image 0: the exe */
#else
        g_ra_build = dl_iterate_phdr(ra_phdr, NULL) != 0;      /* the exe's own entry has no name */
#endif
    }
    return g_ra_build;
}
#endif
static bool onoff(const char *id, bool on, bool enabled) {     /* a big ON / OFF switch; true when clicked */
    char l[64];
    snprintf(l, sizeof l, "%s##%s", on ? "ON" : "OFF", id);
    const bool dis_5 = !enabled;
    if (dis_5) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.20f, 0.60f, 0.25f, 1) : ImVec4(0.35f, 0.35f, 0.38f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.25f, 0.70f, 0.30f, 1) : ImVec4(0.45f, 0.45f, 0.48f, 1));
    bool c = ImGui::Button(l, ImVec2(ImGui::GetFontSize() * 4.0f, 0));
    ImGui::PopStyleColor(2);
    if (dis_5) ImGui::EndDisabled();
    return c && enabled;
}
static int online_differs(void);
static void sec_experimental(void) {
    if (!g_online_inited) online_init();
    help("Features still being tested. Each is saved to config\\graphics.ini and used from the next start.");
    ImGui::Separator();
    /* (1) run-ahead (r1_state.c: graphics.ini runahead = on | off; SSBU_RUNAHEAD wins) */
    if (onoff("ra", S.runahead != 0, ra_build() || S.runahead)) { S.runahead = !S.runahead; ini("runahead", k_onoff[S.runahead]); }
    ImGui::SameLine(); ImGui::Text("Run-ahead");
    tag(1, S.runahead != S0.runahead);
    ImGui::SameLine();
    if (ra_build()) ImGui::TextDisabled("one frame less input delay in fights; costs a second pass of each frame on the CPU");
    else ImGui::TextDisabled("not ready yet: it is being moved onto the normal build (like rollback)");
    ImGui::Separator();
    /* (2) your name: the console user's nickname (nnfs.c) and the name the lobby shows (net_relay.c); graphics.ini player_name */
    static char pname[33], pname0[33];
    static int pname_read;
    if (!pname_read) { pname_read = 1; ini_read("player_name", pname, sizeof pname); memcpy(pname0, pname, sizeof pname); }
    if (InterlockedExchange(&g_osk_name_done, 0) == 1) { snprintf(pname, sizeof pname, "%s", g_osk_name); ini("player_name", pname); }
    ImGui::SetNextItemWidth(260);
    ImGui::InputTextWithHint("Your name", "the name other players see", pname, sizeof pname);
    if (ImGui::IsItemDeactivatedAfterEdit()) ini("player_name", pname);
    tag(1, strcmp(pname, pname0) != 0);
    ImGui::SameLine();
    const bool dis_6 = g_osk_busy;
    if (dis_6) ImGui::BeginDisabled();
#ifdef _WIN32
    if (ImGui::Button("Type with the controller##name")) { InterlockedExchange(&g_osk_busy, 1); CreateThread(NULL, 0, osk_name_thread, pname, 0, NULL); }
#else
    if (ImGui::Button("Type in a dialog##name")) { InterlockedExchange(&g_osk_busy, 1); CreateThread(NULL, 0, osk_name_thread, pname, 0, NULL); }
#endif
    if (dis_6) ImGui::EndDisabled();
#ifdef _WIN32
    if (!pname[0]) ImGui::TextDisabled("Empty: the Windows user name is used.");
#else
    if (!pname[0]) ImGui::TextDisabled("Empty: your login name is used.");
#endif
    ImGui::Separator();
    /* (3) online play (net_relay.c / nnldn.c) */
    bool on = O.mode != 0;
    if (onoff("net", on, true)) { O.mode = on ? 0 : (O.last_mode ? O.last_mode : 4); online_save(); }
    ImGui::SameLine(); ImGui::Text("Online play");
    tag(1, (O.mode != 0) != (O0.mode != 0));
    ImGui::SameLine(); ImGui::TextDisabled("play with others through the game's Local Wireless menu");
#ifndef _WIN32
    if (O.mode) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "Online play is not in the Linux / macOS build yet: these settings are kept for when it is.");
#endif
    if (O.mode) {
        /* (4) rollback netcode over the online connection: Play SSBU.bat (scripts/run.py) starts the normal exe with
         * SSBU_RB_NET=net when graphics.ini has rollback = on (R4: no second build any more, runtime/rb_learned.c) */
        static int rb_have = -1, rb_on = -1, rb_on0;
        if (rb_have < 0) {
            char v[16];
#ifdef _WIN32
            rb_have = 1;
#else
            rb_have = 0;
#endif
            rb_on = ini_read("rollback", v, sizeof v) && !off_word(v); rb_on0 = rb_on;
        }
        if (onoff("rb", rb_on != 0, rb_have || rb_on)) { rb_on = !rb_on; ini("rollback", rb_on ? "on" : "off"); }
        ImGui::SameLine(); ImGui::Text("Rollback netcode");
        tag(1, rb_on != rb_on0);
        ImGui::SameLine();
        if (rb_have) ImGui::TextDisabled("smooth online fights at high ping (experimental); both players need it on");
        else ImGui::TextDisabled("not in the Linux / macOS build yet");
        if (getenv("SSBU_RB_NET")) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "Rollback is active in this run (SSBU_RB_NET=%s).", getenv("SSBU_RB_NET"));
        sec_online();
    }
    if (online_differs() || S.runahead != S0.runahead || strcmp(pname, pname0)) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Saved. These changes are used when the game starts again.");
        if (ImGui::Button("Restart the game now")) restart_game();
        ImGui::SameLine(); ImGui::TextDisabled("(progress not saved by the game is lost)");
    }
}
static void sec_online(void) {
    if (InterlockedExchange(&g_osk_done, 0) == 1) { snprintf(O.join, sizeof O.join, "%s", g_osk_text); online_save(); }
    NetStats ns;
    net_stats_get(&ns);
    static const char *const st[] = {"Offline", "Connecting...", "Connected", "Disconnected"};
    ImVec4 col = ns.state == NET_CONNECTED ? ImVec4(0.45f, 0.9f, 0.45f, 1) : ns.state == NET_CONNECTING ? ImVec4(1, 0.8f, 0.2f, 1) : ns.state == NET_DISCONNECTED ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);
    ImGui::Text("Now:"); ImGui::SameLine();
    if (ns.state == NET_CONNECTED) ImGui::TextColored(col, "%s - %s, %d player%s%s", st[2], k_online_name[O0.mode], ns.players, ns.players == 1 ? "" : "s", "");
    else ImGui::TextColored(col, "%s%s%s", ns.state >= 0 && ns.state <= 3 ? st[ns.state] : "?", O0.mode ? " - " : "", O0.mode ? k_online_name[O0.mode] : "");
    if (ns.state == NET_CONNECTED && ns.ping_ms >= 0) { ImGui::SameLine(); ImGui::Text("  ping %d ms", ns.ping_ms); }
    ImGui::Indent();
    ImGui::SetWindowFontScale(1.2f);
    int changed = 0;
    static const int order[4] = {4, 1, 2, 3};      /* the lobby first: it needs nothing from the router */
    for (int k = 0; k < 4; k++) { int i = order[k]; if (ImGui::RadioButton(k_online_name[i], O.mode == i)) { O.mode = O.last_mode = i; changed = 1; } }
    ImGui::SetWindowFontScale(1.0f);
    tag(1, O.mode != O0.mode);
    if (O.mode == 1) help("Every player on the same home network chooses Same network. One game hosts the room list by itself; nothing to type.");
    if (O.mode == 2) {
        const char *code = O0.mode == 2 ? net_room_code() : "";
        if (code && code[0]) {
            ImGui::Text("ROOM CODE (give it to the other player):");
            ImGui::SetWindowFontScale(2.6f);
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.25f, 1), "%s", code);
            ImGui::SetWindowFontScale(1.0f);
            ImGui::SameLine();
            if (ImGui::Button("Copy")) { ImGui::SetClipboardText(code); printf("menu: room code copied\n"); fflush(stdout); }
            if (net_lan_code()[0] && strcmp(net_lan_code(), code)) ImGui::TextDisabled("On the same network: %s", net_lan_code());
            if (net_upnp_result()[0]) ImGui::TextDisabled("%s", net_upnp_result());
        } else help(O0.mode == 2 ? "The room code appears here in a few seconds (it needs the public address: STUN / UPnP)."
                                 : "The room code appears here after the restart.");
        if (ImGui::Checkbox("Open the router's port by itself (UPnP)", &O.upnp)) changed = 1;
        tag(1, O.upnp != O0.upnp);
    }
    if (O.mode == 3) {
        ImGui::InputTextWithHint("Room code", "1A2B3-C4D5E", O.join, sizeof O.join, ImGuiInputTextFlags_CharsUppercase);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = 1;
        tag(1, strcmp(O.join, O0.join) != 0);
        ImGui::SameLine();
        const bool dis_7 = g_osk_busy;
        if (dis_7) ImGui::BeginDisabled();
#ifdef _WIN32
        if (ImGui::Button("Type with the controller")) { InterlockedExchange(&g_osk_busy, 1); CreateThread(NULL, 0, osk_code_thread, NULL, 0, NULL); }
#else
        if (ImGui::Button("Type in a dialog")) { InterlockedExchange(&g_osk_busy, 1); CreateThread(NULL, 0, osk_code_thread, NULL, 0, NULL); }
#endif
        if (dis_7) ImGui::EndDisabled();
        if (O0.mode == 3 && net_room_code()[0]) ImGui::TextDisabled("Your own code (only if the host's router blocks you; the host sets it as online_peer): %s", net_room_code());
    }
    if (O.mode == 4) {
        ImGui::InputTextWithHint("Lobby server", "host:port", O.server, sizeof O.server);
        if (ImGui::IsItemDeactivatedAfterEdit()) changed = 1;
        tag(1, strcmp(O.server, O0.server) != 0);
        if (strcmp(O.server, ONLINE_DEFAULT_LOBBY) && ImGui::SmallButton("Use the public lobby")) { snprintf(O.server, sizeof O.server, "%s", ONLINE_DEFAULT_LOBBY); changed = 1; }
        help("Everyone using the same lobby sees each other's rooms. Your own lobby: tools/net/ssbu_lobby.py.");
        if (ImGui::Checkbox("Relay only (everything through the lobby, no direct connection)", &O.relay_only)) changed = 1;
        tag(1, O.relay_only != O0.relay_only);
    }
    if (changed) online_save();
    help("Then, in the game: Smash > Local Wireless (the game's own menu): one player Create Room, the others Rooms List.");
    if (getenv("SSBU_ONLINE")) ImGui::TextColored(ImVec4(1, 0.6f, 0.3f, 1), "SSBU_ONLINE=%s is set for this run: it wins over these settings.", getenv("SSBU_ONLINE"));
    ImGui::Unindent();
}
static int online_differs(void) {
    return O.mode != O0.mode || (O.mode == 3 && strcmp(O.join, O0.join)) || (O.mode == 4 && (strcmp(O.server, O0.server) || O.relay_only != O0.relay_only)) || (O.mode == 2 && O.upnp != O0.upnp);
}


static void sec_dlc(void) {
    const int *own = NULL;
    int n = nnaoc_owned(&own);
    if (!g_dlc_loaded) dlc_load();
    if (n >= 0) ImGui::Text("The game was told of %d owned add-on content(s).", n); else ImGui::Text("The game has not asked for its add-on contents yet.");
    ImGui::TextDisabled("%s  (tools_audit\\dlc_list.py writes it from your own DLC dumps)", g_dlc_path);
    int entries = 0;
    for (int i = 0; i < g_dlc_n; i++) entries += g_dlc[i].entry;
    if (!entries) help("No entry in the list: no add-on content is owned (DLC fighters, stages and costumes stay locked).");
    ImGui::BeginChild("dlc", ImVec2(0, ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.5f), ImGuiChildFlags_None);
    for (int i = 0; i < g_dlc_n; i++) {
        DlcLine *d = &g_dlc[i];
        if (!d->entry) continue;
        const char *c = strchr(d->text, '#');
        if (c == d->text) c = strchr(c + 1, '#');
        char label[200];
        snprintf(label, sizeof label, "0x%03x  %s##%d", d->index, c ? c + 1 : "", i);
        bool on = d->on != 0;
        if (ImGui::Checkbox(label, &on)) { d->on = on; g_dlc_dirty = 1; }
    }
    ImGui::EndChild();
    const bool dis_8 = !g_dlc_dirty;
    if (dis_8) ImGui::BeginDisabled();
    if (ImGui::Button("Save the list")) dlc_save();
    if (dis_8) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Read it again")) dlc_load();
    tag(1, g_dlc_dirty);
}
static void sec_lang(void) {
    if (combo("Language", &S.lang, k_lang_name, N(k_lang_name))) ini("language", k_lang[S.lang]);
    tag(1, S.lang != S0.lang);
    help("The language the game is told (nn::oe::GetDesiredLanguage). SSBU_LANGUAGE=<code> wins over this setting.");
}
/* ------------------------------------------------------------------ the Log page: the file stdout goes to, read back */
#define LOG_MAX 5000
static char *g_log[LOG_MAX];
static int g_log_head, g_log_n, g_log_state, g_log_auto = 1;      /* state: 0 not tried, 1 reading, -1 stdout is not a file */
static HANDLE g_log_h = INVALID_HANDLE_VALUE;
#ifndef _WIN32
static int g_log_fd = -1;
#endif
static char g_log_path[MAX_PATH + 8], g_log_part[1024], g_export_msg[MAX_PATH + 64];
static int g_log_partn;
static ImGuiTextFilter g_log_filter;
static void log_add(const char *s, int n) {
    char *l = (char *)malloc((size_t)n + 1);
    if (!l) return;
    memcpy(l, s, (size_t)n); l[n] = 0;
    int at = (g_log_head + g_log_n) % LOG_MAX;
    if (g_log_n == LOG_MAX) { free(g_log[g_log_head]); g_log_head = (g_log_head + 1) % LOG_MAX; at = (g_log_head + LOG_MAX - 1) % LOG_MAX; } else g_log_n++;
    g_log[at] = l;
}
static const char *log_line(int i) { return g_log[(g_log_head + i) % LOG_MAX]; }
static void log_poll(void) {
#ifdef _WIN32
    if (g_log_state == 0) {
        HANDLE so = GetStdHandle(STD_OUTPUT_HANDLE);
        g_log_state = -1;
        if (so && so != INVALID_HANDLE_VALUE && GetFileType(so) == FILE_TYPE_DISK && GetFinalPathNameByHandleA(so, g_log_path, MAX_PATH, FILE_NAME_NORMALIZED)) {
            g_log_h = CreateFileA(g_log_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
            if (g_log_h != INVALID_HANDLE_VALUE) {
                LARGE_INTEGER sz, at;
                GetFileSizeEx(g_log_h, &sz);
                at.QuadPart = sz.QuadPart > (2 << 20) ? sz.QuadPart - (2 << 20) : 0;      /* the last 2 MB at most */
                SetFilePointerEx(g_log_h, at, NULL, FILE_BEGIN);
                g_log_state = 1;
                if (at.QuadPart) g_log_partn = -1;                                      /* the first (cut) line is dropped */
            }
        }
    }
#else
    if (g_log_state == 0) {                          /* stdout is a regular file (play.sh, the test runners): its name, opened again to read */
        struct stat st;
        g_log_state = -1;
        int named = 0;
        if (fstat(1, &st) == 0 && S_ISREG(st.st_mode)) {
#ifdef __APPLE__
            char p[MAXPATHLEN];
            if (fcntl(1, F_GETPATH, p) != -1) { snprintf(g_log_path, sizeof g_log_path, "%s", p); named = 1; }
#else
            ssize_t k = readlink("/proc/self/fd/1", g_log_path, MAX_PATH - 1);
            if (k > 0) { g_log_path[k] = 0; named = 1; }
#endif
        }
        if (named && (g_log_fd = open(g_log_path, O_RDONLY | O_CLOEXEC)) >= 0) {
            off_t sz = lseek(g_log_fd, 0, SEEK_END), at = sz > (2 << 20) ? sz - (2 << 20) : 0;      /* the last 2 MB at most */
            lseek(g_log_fd, at, SEEK_SET);
            g_log_state = 1;
            if (at) g_log_partn = -1;                                                  /* the first (cut) line is dropped */
        }
    }
#endif
    if (g_log_state != 1) return;
    fflush(stdout);
    static char buf[256 * 1024];
#ifdef _WIN32
    DWORD got = 0;
    for (int k = 0; k < 4 && ReadFile(g_log_h, buf, sizeof buf, &got, NULL) && got; k++) {
#else
    ssize_t rd;
    DWORD got = 0;
    for (int k = 0; k < 4 && (rd = read(g_log_fd, buf, sizeof buf)) > 0 && (got = (DWORD)rd) != 0; k++) {
#endif
        for (DWORD i = 0; i < got; i++) {
            char ch = buf[i];
            if (ch == '\n') { if (g_log_partn >= 0) { while (g_log_partn && g_log_part[g_log_partn - 1] == '\r') g_log_partn--; log_add(g_log_part, g_log_partn); } g_log_partn = 0; }
            else if (g_log_partn >= 0 && g_log_partn < (int)sizeof g_log_part - 1) g_log_part[g_log_partn++] = ch;
        }
    }
}
static int log_export(char *out, int n) {             /* logs\ssbu_log_<date>_<time>.txt beside the exe; 1 = written */
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    strncat(dir, "\\logs", sizeof dir - strlen(dir) - 1);
    CreateDirectoryA(dir, NULL);
    SYSTEMTIME t;
    GetLocalTime(&t);
    snprintf(out, (size_t)n, "%s\\ssbu_log_%04d%02d%02d_%02d%02d%02d.txt", dir, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    FILE *f = fopen(out, "wb");
    if (!f) return 0;
    for (int i = 0; i < g_log_n; i++) { fputs(log_line(i), f); fputs("\r\n", f); }
    int ok = !fclose(f);
#ifndef _WIN32
    { char t[MAX_PATH + 64]; plat_path(out, t, sizeof t); snprintf(out, (size_t)n, "%s", t); }      /* shown with '/' */
#endif
    printf("menu: log exported: %s (%d lines)\n", out, g_log_n);
    fflush(stdout);
    return ok;
}
#ifndef _WIN32
static DWORD WINAPI reap_thread(LPVOID arg) { int st; waitpid((pid_t)(intptr_t)arg, &st, 0); return 0; }
static void open_folder(const char *dir) {            /* xdg-open (open on a Mac), not waited for */
#ifdef __APPLE__
    const char *prog = "open";
#else
    const char *prog = "xdg-open";
#endif
    char *argv[3] = {(char *)prog, (char *)dir, NULL};
    pid_t pid;
    if (posix_spawnp(&pid, prog, NULL, NULL, argv, environ) == 0) CreateThread(NULL, 0, reap_thread, (LPVOID)(intptr_t)pid, 0, NULL);
    else { printf("menu: %s could not be started: open %s by hand\n", prog, dir); fflush(stdout); }
}
#endif
static void sec_log(void) {
    log_poll();
    if (g_log_state != 1) { help("The output of this run does not go to a file (started from a console without '> file'): nothing to show. gioca.bat writes it to logs_gioco\\gioca.log."); return; }
    g_log_filter.Draw("Filter (a,b = either; -x = without)", ImGui::GetFontSize() * 16);
    ImGui::SameLine();
    bool au = g_log_auto != 0;
    if (ImGui::Checkbox("Auto-scroll", &au)) g_log_auto = au;
    ImGui::SameLine();
    if (ImGui::Button("Export log")) {
        char path[MAX_PATH + 64];
        if (log_export(path, sizeof path)) {
            snprintf(g_export_msg, sizeof g_export_msg, "written: %s", path);
#ifdef _WIN32
            char *s = strrchr(path, '\\');
            if (s) *s = 0;
            ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);      /* the folder, in Explorer: this button only */
#else
            char *s = strrchr(path, '/');
            if (s) *s = 0;
            open_folder(path);                                             /* the folder, in the file manager: this button only */
#endif
        } else snprintf(g_export_msg, sizeof g_export_msg, "could NOT be written: %s", path);
    }
    ImGui::TextDisabled("%s - %d lines kept (the last %d)%s%s", g_log_path + (strncmp(g_log_path, "\\\\?\\", 4) ? 0 : 4), g_log_n, LOG_MAX, g_export_msg[0] ? " - " : "", g_export_msg);
    ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    static ImVector<int> idx;
    idx.resize(0);
    for (int i = 0; i < g_log_n; i++) if (!g_log_filter.IsActive() || g_log_filter.PassFilter(log_line(i))) idx.push_back(i);
    ImGuiListClipper clip;
    clip.Begin(idx.Size);
    while (clip.Step()) for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) ImGui::TextUnformatted(log_line(idx[i]));
    clip.End();
    if (g_log_auto) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

/* ------------------------------------------------------------------ the stats overlay (F2), top right */
static void draw_stats(void) {
    float sum = 0;
    int n = 0;
    for (int i = 0; i < 120; i++) if (g_ft[i] > 0) { sum += g_ft[i]; n++; }
    float avg = n ? sum / (float)n : 0;
    NetStats ns;
    memset(&ns, 0, sizeof ns);
    net_stats_get(&ns);
    static int ra_cfg = -1;
    if (ra_cfg < 0) { const char *e = getenv("SSBU_RUNAHEAD"); ra_cfg = e ? (e[0] && e[0] != '0') : gfx_runahead(); }
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 8, g_open ? ImGui::GetFrameHeight() + 8 : 8), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##stats", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("%.0f fps  %.2f ms", avg > 0 ? 1000.0f / avg : 0.0f, avg);
    { int hfr = display_hfr_mode();
      if (hfr > 0) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "Fixed %d Hz (60Hz logic, F10)", display_hfr_hz());
      else ImGui::TextDisabled("Fixed 60 Hz (F10)"); }
    static const char *const st[] = {"Offline", "Connecting...", "Online - connected", "Disconnected"};
    ImVec4 col = ns.state == NET_CONNECTED ? ImVec4(0.45f, 0.9f, 0.45f, 1) : ns.state == NET_CONNECTING ? ImVec4(1, 0.8f, 0.2f, 1) : ns.state == NET_DISCONNECTED ? ImVec4(1, 0.4f, 0.4f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1);
    if (ns.state == NET_CONNECTED) ImGui::TextColored(col, "%s (%d players)", st[2], ns.players);
    else ImGui::TextColored(col, "%s", ns.state >= 0 && ns.state <= 3 ? st[ns.state] : "?");
    if (ns.state == NET_CONNECTED || ns.state == NET_CONNECTING) {
        if (ns.ping_ms >= 0) ImGui::Text("ping %d ms  delay %d f", ns.ping_ms, ns.delay); else ImGui::Text("ping -  delay %d f", ns.delay);
        ImGui::Text("rollback %d / %d f", ns.rollback_last, ns.rollback_max);
    }
    ImGui::Text("run-ahead %s", !ra_cfg ? "off" : r1_ra_phase() ? "on (running)" : "on");
    ImGui::End();
}

/* ------------------------------------------------------------------ O2: Find an opponent (net_relay.c net_match_*)
 * Returns the matched host's address while this game is matched to join (its row in the table below says so). */
static uint32_t sec_match(void) __attribute__((unused));
static uint32_t sec_match(void) {
    NetMatch m;
    net_match_get(&m);
    const ImVec4 green(0.45f, 0.9f, 0.45f, 1), amber(1, 0.8f, 0.2f, 1);
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "Find an opponent");
    if (m.state == MM_OFFLINE) { ImGui::TextDisabled("Not connected to the lobby yet."); return 0; }
    if (m.players >= 0) {
        ImGui::Text("Online now: %d player%s with your version and mods", m.players, m.players == 1 ? "" : "s");
        if (m.code_players >= 0 && m.code_players != m.players) { ImGui::SameLine(); ImGui::TextDisabled("(%d in this lobby, any version)", m.code_players); }
        if (m.searching >= 0) ImGui::Text("Searching for an opponent: %d", m.searching);
        else ImGui::TextDisabled("Searching for an opponent: not known (an older lobby server)");
    }
    float bw = ImGui::GetFontSize() * 11;
    if (m.kind == 2 && (m.state == MM_ASKING || m.state == MM_SEARCHING || m.state == MM_MATCHED || m.state == MM_TOGETHER)) {   /* (R3 net3) rb_net.c's search */
        if (m.state == MM_ASKING || m.state == MM_SEARCHING) ImGui::TextColored(amber, "Rollback match (Smash versus): finding an opponent... %d s", m.waited_s);
        else ImGui::TextColored(green, "Rollback match (Smash versus) with %s", m.peer_name);
        help("Started by a Smash (versus) match with rollback on and no room code; the box over the game shows each step (Esc or B held stops waiting).");
        return 0;
    }
    switch (m.state) {
    case MM_ASKING:
    case MM_SEARCHING:
        ImGui::TextColored(amber, "Searching%s... %d s", m.state == MM_ASKING ? " (asking the lobby)" : "", m.waited_s);
        help("You can keep playing: a box at the top of the picture says when an opponent is found.");
        if (ImGui::Button("Cancel##mm", ImVec2(bw, 0))) net_match_cancel();
        break;
    case MM_MATCHED:
        if (m.role == MM_ROLE_HOST) {
            ImGui::TextColored(green, "Matched with %s: you host.", m.peer_name);
            help("Create a room: Local Wireless > Create Room (an open room of yours counts). Accept their request when \"wants to join!\" appears.");
        } else {
            ImGui::TextColored(green, "Matched with %s: they host.", m.peer_name);
            if (m.room_open) help("Their room is open: Local Wireless > Rooms List, their room is first. Join it.");
            else help("Waiting for their room: open Local Wireless > Rooms List, their room will be first as soon as it exists.");
        }
        ImGui::TextDisabled("Matched %d s ago.", m.waited_s);
        if (ImGui::Button("Cancel the match##mm", ImVec2(bw, 0))) net_match_cancel();
        break;
    default:
        if (m.state == MM_TOGETHER) ImGui::TextColored(green, "In a room with %s.", m.peer_name);
        else if (m.state == MM_TIMEOUT) ImGui::TextColored(amber, "No opponent found in time: nobody with your version and mods was searching.");
        else if (m.state == MM_PEER_LEFT) ImGui::TextColored(amber, "%s cancelled the match or left.", m.peer_name);
        else if (m.state == MM_EXPIRED) ImGui::TextColored(amber, "The match with %s expired: you did not meet in a room in time.", m.peer_name);
        else if (m.state == MM_OLD_LOBBY) ImGui::TextColored(amber, "This lobby server is an older version: it cannot find opponents. Create a room or join one.");
        if (ImGui::Button(m.state == MM_IDLE ? "Find an opponent##mm" : "Search again##mm", ImVec2(bw, 0))) net_match_find();
        help("The lobby pairs you with another player of the same version and mods who is searching too, and says who hosts the room.");
        break;
    }
    return m.state == MM_MATCHED && m.role == MM_ROLE_JOIN ? m.peer : 0;
}

/* ------------------------------------------------------------------ O1: Online rooms (net_relay.c net_rooms_get) */
static void sec_rooms(void) {
    static NetRoom rooms[32];
    static int sel = -1;
    static char pop_title[96], pop_text[512];
    int state = 0, n = net_rooms_get(rooms, 32, &state);
    if (!state) {
        help("Online play is off: Experimental > Online play.");
        return;
    }
    {   const char *b = net_build_id(), *plus = strchr(b, '+'), *end = plus ? plus : b + strlen(b), *d = end;
        while (d > b && d[-1] != '-') d--;
        ImGui::Text("This game: version"); ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "%.*s", (int)(end - d), d); }
    ImGui::SameLine(); ImGui::Text("  mods:"); ImGui::SameLine();
    ImGui::PushTextWrapPos(0.0f); ImGui::TextUnformatted(net_mods_text()); ImGui::PopTextWrapPos();
    uint32_t match_host = net_match_host();      /* (owner 2026-10-22: no matchmaking from F1; the game's own Online menu will do it) */
    ImGui::Separator();
    help("Every room of your lobby, any version or mod set. Join a room from the game: Local Wireless > Rooms List. "
         "Rooms of another version or mod set are listed there too, but joining one is refused and says why. Select a row for details.");
    ImGui::Separator();
    if (state == 1 && !n) { ImGui::TextDisabled("Asking the lobby..."); return; }
    if (state == 2) { ImGui::TextColored(ImVec4(1, 0.8f, 0.2f, 1), "The lobby does not list rooms of other versions (an older lobby server): only the game's own list works."); return; }
    if (!n) { ImGui::TextDisabled("No room is open in this lobby right now. Create one: Local Wireless > Create Room."); return; }
    if (ImGui::BeginTable("rooms", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Host", ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 0.6f);
        ImGui::TableSetupColumn("Mods", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("Players", ImGuiTableColumnFlags_WidthStretch, 0.5f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch, 1.1f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; i++) {
            const NetRoom &r = rooms[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            char id[64];
            snprintf(id, sizeof id, "%s##room%u", r.name[0] ? r.name : "?", r.host);
            if (ImGui::Selectable(id, sel == i, ImGuiSelectableFlags_SpanAllColumns)) {
                sel = i;
                if (r.mine) { snprintf(pop_title, sizeof pop_title, "Your room"); snprintf(pop_text, sizeof pop_text, "This is the room you host."); }
                else if (r.joinable) { snprintf(pop_title, sizeof pop_title, "%s's room", r.name); snprintf(pop_text, sizeof pop_text, "Same version and mods as yours: join it from the game, Local Wireless > Rooms List."); }
                else { snprintf(pop_title, sizeof pop_title, "Cannot join %s's room", r.name); snprintf(pop_text, sizeof pop_text, "%s", r.why); }
                ImGui::OpenPopup("##roomwhy");
            }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(r.version[0] ? r.version : "?");
            ImGui::TableNextColumn(); ImGui::PushTextWrapPos(0.0f); ImGui::TextUnformatted(r.mods[0] ? r.mods : "none"); ImGui::PopTextWrapPos();
            ImGui::TableNextColumn(); ImGui::Text("%d / %d", r.players, r.most);
            ImGui::TableNextColumn();
            if (r.mine) ImGui::TextDisabled("yours");
            else if (match_host && r.host == match_host) ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1), "YOUR MATCH: join it");
            else if (r.joinable) ImGui::TextColored(r.open ? ImVec4(0.45f, 0.9f, 0.45f, 1) : ImVec4(0.7f, 0.7f, 0.7f, 1), "%s", r.open ? "can join" : "full / closed");
            else {
                const char *nl = strchr(r.why, '\n'), *colon = strchr(r.why, ':');
                int len = (int)((colon && (!nl || colon < nl)) ? colon - r.why : nl ? nl - r.why : (ptrdiff_t)strlen(r.why));
                ImGui::TextColored(ImVec4(1, 0.45f, 0.4f, 1), "%.*s", len, r.why);
            }
        }
        ImGui::EndTable();
    }
    if (ImGui::BeginPopupModal("##roomwhy", NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", pop_title);
        ImGui::Separator();
        ImGui::TextUnformatted(pop_text);
        if (ImGui::Button("OK", ImVec2(ImGui::GetFontSize() * 6, 0))) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
}

static void sec_about(void) {
#ifdef _WIN32
    ImGui::Text("SSBU native: Super Smash Bros. Ultimate, statically recompiled for Windows");
#elif defined(__APPLE__)
    ImGui::Text("SSBU native: Super Smash Bros. Ultimate, statically recompiled for macOS");
#else
    ImGui::Text("SSBU native: Super Smash Bros. Ultimate, statically recompiled for Linux");
#endif
    ImGui::Text("Runtime built %s %s (study tree)", __DATE__, __TIME__);
    ImGui::Text("Renderer: Vulkan on %s", nvn_vk_device_name());
    ImGui::Text("Settings menu: Dear ImGui %s (MIT licence, Omar Cornut and contributors)", ImGui::GetVersion());
    ImGui::Text("graphics.ini: %s", gfx_options()->path);
    ImGui::Text("controllers.ini: %s", nnpad_config_path());
    ImGui::Separator();
    help("No game content is part of this program: it runs the game files you dumped from your own console.");
}

static void draw_overlay(void) {
    float sum = 0;
    int n = 0;
    for (int i = 0; i < 120; i++) if (g_ft[i] > 0) { sum += g_ft[i]; n++; }
    float avg = n ? sum / (float)n : 0;
    ImGui::SetNextWindowPos(ImVec2(8, g_open ? ImGui::GetFrameHeight() + 8 : 8), ImGuiCond_Always);      /* below the menu bar while the menu is open */
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##frametime", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("%.2f ms  %.0f fps", avg, avg > 0 ? 1000.0f / avg : 0.0f);
    ImGui::PlotLines("##ft", g_ft, 120, g_ft_pos, NULL, 0.0f, 33.4f, ImVec2(ImGui::GetFontSize() * 9, ImGui::GetFontSize() * 2.5f));
    ImGui::End();
}

static void exit_game(void) {                              /* F1 -> Exit game: the exit handlers run, then a hard end if one hangs */
    static volatile LONG once;
    if (InterlockedExchange(&once, 1)) return;
    printf("menu: exit game\n");
    fflush(stdout);
    CreateThread(NULL, 0, restart_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, [](LPVOID) -> DWORD { Sleep(4000); fflush(stdout); _Exit(0); return 0; }, NULL, 0, NULL);
}
static void draw_menu(int cw, int ch) {
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "SSBU native");
        for (int i = 0; i < SEC_N; i++) if (ImGui::MenuItem(k_sections[i], NULL, g_section == i)) g_section = i;
        ImGui::TextDisabled("   F1 / Home: close   L / R: section");
        ImGui::EndMainMenuBar();
    }
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 size(io.DisplaySize.x * 0.62f, io.DisplaySize.y * 0.78f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.53f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    char title[64];
    snprintf(title, sizeof title, "%s###settings", k_sections[g_section]);
    if (g_section_req >= 0) ImGui::SetNextWindowFocus();
    ImGui::Begin(title, NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);
    g_section_req = -1;
    ImGui::BeginChild("body", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * (restart_pending() ? 2.2f : 1.2f)), ImGuiChildFlags_NavFlattened);
    ImGui::PushItemWidth(ImGui::GetFontSize() * 14);
    switch (g_section) {
    case SEC_DISPLAY: sec_display(cw, ch); break;
    case SEC_GRAPHICS: sec_graphics(); break;
    case SEC_AUDIO: sec_audio(); break;
    case SEC_CONTROLS: sec_controls(); break;
    case SEC_EXPERIMENTAL: sec_experimental(); break;
    case SEC_ROOMS: sec_rooms(); break;
    case SEC_DLC: sec_dlc(); break;
    case SEC_LANG: sec_lang(); break;
    case SEC_LOG: sec_log(); break;
    default: sec_about(); break;
    }
    ImGui::PopItemWidth();
    ImGui::EndChild();
    if (restart_pending()) ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Saved. The settings marked [restart: changed] take effect at the next start.");
    if (ImGui::Button("Close")) menu_request_toggle();
    ImGui::SameLine();
    if (ImGui::Button("Exit game")) ImGui::OpenPopup("Exit the game?");
    ImGui::SameLine();
    ImGui::TextDisabled("Settings are saved at once (config\\graphics.ini)");
    if (ImGui::BeginPopupModal("Exit the game?", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Are you sure? Progress not saved by the game is lost.");
        if (ImGui::Button("Yes, exit")) { exit_game(); ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("No") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

static void set_open(int on) {
    if (on == (g_open != 0)) return;
    if (on) {
        InterlockedExchange(&g_open, 1);
        InterlockedExchange(&g_guard, 1);
        g_section_req = g_section;
        capture_cancel("menu opened");
        g_dlc_loaded = 0;
        printf("menu: opened at presented frame %lld\n", g_frame);
    } else {
        InterlockedExchange(&g_open, 0);
        g_closed_at = g_frame;
        capture_cancel("menu closed");
        printf("menu: closed at presented frame %lld\n", g_frame);
    }
    fflush(stdout);
}

/* while the menu is open and the game shows nothing new (a loading wait), the last frame is shown again with the menu over it */
static DWORD WINAPI redraw_thread(LPVOID) {
    long long seen = -1;
    ULONGLONG at = 0;
    for (;;) {
        Sleep(50);
        if (!g_open || osk_active()) { seen = -1; continue; }
        long long p = nvn_front_presents();
        ULONGLONG now = GetTickCount64();
        if (p != seen) { seen = p; at = now; continue; }
        if (now - at > 100) nvn_front_osk_redraw();
    }
}

/* ------------------------------------------------------------------ SSBU_MENU_TEST */
static long long g_test_at = -2;
static int g_test_step, g_test_kb_old;
static char g_shot_path[MAX_PATH];
static int g_shot_req;
static void test_shot(const char *what) {
    const char *dir = getenv("SSBU_SHOT_DIR");
    snprintf(g_shot_path, sizeof g_shot_path, "%s%smenu_test_%s.bmp", dir ? dir : "", dir ? "/" : "", what);
    g_shot_req = 1;
}
static void test_tick(void) {
    if (g_test_at == -2) { const char *e = getenv("SSBU_MENU_TEST"); g_test_at = e && e[0] ? atoll(e) : -1; }
    if (g_test_at < 0 || g_frame < g_test_at) return;
    long long t = g_frame - g_test_at;
    static long long step_at;
    switch (g_test_step) {
    case 0: set_open(1); printf("menu: test: opened; input to the game blocked = %d\n", menu_input_blocked()); g_test_step = 1; step_at = t; break;
    case 1: if (t - step_at < 5) break;
        g_section = SEC_GRAPHICS; g_section_req = g_section; printf("menu: test: section Graphics\n");
        {   int was = S.overlay;
            apply_overlay(!was);
            FILE *f = fopen(gfx_options()->path, "rb");
            char ln[256], got[256] = "(not found)";
            while (f && fgets(ln, sizeof ln, f)) if (!strncmp(ln, "frame_time_overlay", 18)) { ln[strcspn(ln, "\r\n")] = 0; snprintf(got, sizeof got, "%s", ln); }
            if (f) fclose(f);
            printf("menu: test: frame_time_overlay changed to %s; the file now says \"%s\"; live value %d\n", k_onoff[S.overlay], got, gfx_options()->overlay); }
        g_test_step = 2; step_at = t; break;
    case 2: if (t - step_at >= 10) { test_shot("graphics"); g_test_step = 3; } break;
    case 4: g_section = SEC_LOG; g_section_req = g_section; printf("menu: test: section Log\n"); g_test_step = 5; step_at = t; break;
    case 5: if (t - step_at >= 10) {
            char path[MAX_PATH + 64];
            printf("menu: test: log page: %d lines read from %s; export %s\n", g_log_n, g_log_state == 1 ? g_log_path : "(stdout is not a file)", log_export(path, sizeof path) ? path : "FAILED");
            test_shot("log"); g_test_step = 6; } break;
    case 7: apply_stats(1); set_open(0); printf("menu: test: stats overlay on, menu closed\n"); g_test_step = 8; step_at = t; break;
    case 8: if (t - step_at >= 10) { test_shot("stats"); g_test_step = 9; } break;
    case 10: apply_stats(0); apply_overlay(!S.overlay); printf("menu: test: stats overlay and frame time overlay back as they were\n"); g_test_step = 11; break;
    case 11: if (!g_guard) { printf("menu: test: closed; input to the game blocked = %d (released) at frame %lld\n", menu_input_blocked(), g_frame); g_test_step = 12; } break;
    /* the Controls page: a key rebound through the same code as the page (capture + window key message), and the game's
     * own read of the keyboard player's pad before and after (nnhid_test_key holds the key P whatever the focus) */
    case 12: g_test_kb_old = nnpad_kb_vk(0); nnhid_test_key('P'); g_test_step = 13; step_at = t;
        printf("menu: test: controls: key P held for the keyboard's player %d (A is bound to key 0x%02x now)\n", nnhid_kb_player() + 1, g_test_kb_old); break;
    case 13: if (t - step_at >= 15) {
            uint64_t seen; long polls = nnhid_test_result(&seen); nnhid_test_key(0);
            printf("menu: test: controls: BEFORE: %ld polls of player %d with key P held: the game read buttons 0x%llx (A %s)\n", polls, nnhid_kb_player() + 1, (unsigned long long)seen, seen & 1 ? "PRESSED" : "not pressed");
            set_open(1); g_section = SEC_CONTROLS; g_section_req = g_section; g_ctl_dev = -1; g_test_step = 14; step_at = t; } break;
    case 14: if (t - step_at >= 10) { test_shot("controls"); g_test_step = 15; } break;
    case 16: g_cap_t0 = GetTickCount64(); g_capture = 0;                          /* the Change button of row A */
        q_push(EV_KEY, 'P', 1); q_push(EV_KEY, 'P', 0);                          /* what the window thread queues for a press of P */
        printf("menu: test: controls: Change on A, then the key P (window messages)\n"); g_test_step = 17; step_at = t; break;
    case 17: if (t - step_at >= 3) {
            char nm[16], got[128] = "(not found)", ln[256];
            int in_kb = 0;
            nnpad_vk_name(nnpad_kb_vk(0), nm, sizeof nm);
            FILE *f = fopen(nnpad_config_path(), "rb");
            while (f && fgets(ln, sizeof ln, f)) { if (ln[0] == '[') in_kb = !_strnicmp(ln, "[keyboard]", 10); else if (in_kb && !strncmp(ln, "A = ", 4)) { ln[strcspn(ln, "\r\n")] = 0; snprintf(got, sizeof got, "%s", ln); } }
            if (f) fclose(f);
            printf("menu: test: controls: A is now bound to %s; %s [keyboard] says \"%s\"\n", nm, nnpad_config_path(), got);
            set_open(0); g_test_step = 18; } break;
    case 18: if (!g_guard) { nnhid_test_key('P'); g_test_step = 19; step_at = t; } break;
    case 19: if (t - step_at >= 15) {
            uint64_t seen; long polls = nnhid_test_result(&seen); nnhid_test_key(0);
            printf("menu: test: controls: AFTER: %ld polls of player %d with key P held: the game read buttons 0x%llx (A %s)\n", polls, nnhid_kb_player() + 1, (unsigned long long)seen, seen & 1 ? "PRESSED" : "not pressed");
            nnpad_kb_bind(0, g_test_kb_old);
            printf("menu: test: controls: A bound back to its old key 0x%02x\n", g_test_kb_old);
            set_open(1); g_section = SEC_EXPERIMENTAL; g_section_req = g_section; g_test_step = 20; step_at = t; } break;
    case 20: if (t - step_at >= 10) {
            NetStats ns; net_stats_get(&ns);
            printf("menu: test: online page: saved mode %s, this run %s, state %d\n", k_online_mode[O.mode], k_online_mode[O0.mode], ns.state);
            test_shot("online"); g_test_step = 21; } break;
    case 22: set_open(0); printf("menu: test: over\n"); g_test_step = 23; break;
    }
    fflush(stdout);
}
/* TEST SWITCH SSBU_MENU_ROOMS_TEST=popup (O1, default off): 1 s after the first online popup is drawn, a picture of it
 * (SSBU_SHOT_DIR/menu_test_popup.bmp); then F1 opens on Online rooms, 3 s later its rooms go to the log ("menu: rooms
 * test:") with a picture (menu_test_rooms.bmp), and the menu closes. */
static void rooms_test_tick(void) {
    static int on = -1, step;
    static ULONGLONG t0;
    if (on < 0) { const char *e = getenv("SSBU_MENU_ROOMS_TEST"); on = e && !_stricmp(e, "popup"); }
    if (!on || step >= 5) return;
    ULONGLONG t = GetTickCount64();
    if (step == 0 && g_popup_drawn) { t0 = t; step = 1; }
    else if (step == 1 && t - t0 >= 1000) { test_shot("popup"); printf("menu: rooms test: picture of the popup\n"); step = 2; t0 = t; }
    else if (step == 2 && t - t0 >= 1500) { set_open(1); g_section = SEC_ROOMS; g_section_req = g_section; printf("menu: rooms test: Online rooms opened\n"); step = 3; t0 = t; }
    else if (step == 3 && t - t0 >= 3000) {
        NetRoom r[32];
        int st = 0, n = net_rooms_get(r, 32, &st);
        printf("menu: rooms test: state %d, this game %s, mods %s, %d room(s)\n", st, net_build_id(), net_mods_text(), n);
        for (int i = 0; i < n; i++)
            printf("menu: rooms test: room %d: host %s, version %s, mods [%s], players %d/%d, %s%s%s\n", i, r[i].name, r[i].version, r[i].mods,
                   r[i].players, r[i].most, r[i].mine ? "yours" : r[i].joinable ? "can join" : "CANNOT JOIN: ", r[i].mine || r[i].joinable ? "" : r[i].why, "");
        test_shot("rooms"); step = 4; t0 = t;
    } else if (step == 4 && t - t0 >= 1500) { set_open(0); printf("menu: rooms test: over\n"); step = 5; }
    fflush(stdout);
}
extern "C" int menu_shot_wanted(char *path, int n) {
    if (!g_shot_req) return 0;
    g_shot_req = 0;
    snprintf(path, (size_t)n, "%s", g_shot_path);
    return 1;
}
extern "C" void menu_shot_done(const char *path, int w, int h, int ok) {
    printf("menu: test: picture %s %dx%d %s; draw data of that frame: %d vertices, %d indices, %d commands\n", path, w, h, ok ? "written" : "NOT written", g_vtx.Size, g_idx.Size, g_cmd.Size);
    fflush(stdout);
    if (g_test_step == 3 || g_test_step == 6 || g_test_step == 9 || g_test_step == 15 || g_test_step == 21) g_test_step++;
}

/* ------------------------------------------------------------------ once per presented frame (render thread) */
extern "C" int menu_frame(int width, int height, MenuDraw *out) {
    if (!menu_enabled() || width <= 0 || height <= 0) return 0;
    long long fr = nvn_front_presents();
    int fresh = fr != g_frame;
    g_frame = fr;
    {   LARGE_INTEGER now; QueryPerformanceCounter(&now);
        if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
        if (fresh && g_last_t.QuadPart) { g_ft[g_ft_pos] = (float)((double)(now.QuadPart - g_last_t.QuadPart) * 1000.0 / (double)g_qpf.QuadPart); g_ft_pos = (g_ft_pos + 1) % 120; }
        if (fresh) g_last_t = now; }
    if (!g_sections_inited) { settings_init(width, height); g_sections_inited = 1; CreateThread(NULL, 0, redraw_thread, NULL, 0, NULL); }
    test_tick();
    rooms_test_tick();
    uint64_t b = nnhid_menu_buttons(), down = b & ~g_pad_prev;
    g_pad_prev = b;
    {   /* U2: - and + held together for 1 s open the menu too: a controller whose Home / Guide button is taken (the Steam
         * button of a Steam Deck) or that has none. The game sees the two buttons until the menu opens (+ may pause a match). */
        static ULONGLONG combo_t0;
        static int combo_done;
        const uint64_t combo = BIT(B_MINUS) | BIT(B_PLUS);
        if ((b & combo) == combo && !g_open && !combo_done) {
            ULONGLONG now = GetTickCount64();
            if (!combo_t0) combo_t0 = now;
            else if (now - combo_t0 >= 1000) { combo_done = 1; menu_request_toggle(); printf("menu: - and + held for 1 s\n"); fflush(stdout); }
        } else if ((b & combo) != combo) { combo_t0 = 0; combo_done = 0; }
    }
    if (g_open && capturing()) { if (g_pcap < 0 && (down & (BIT(B_HOME) | BIT(B_CAPTURE)))) capture_cancel("Home / Capture"); }      /* Home cancels a rebinding instead of closing */
    else {
        if (down & (BIT(B_HOME) | BIT(B_CAPTURE))) menu_request_toggle();      /* nnhid_menu_buttons bits 16 / 17: never seen by the game */
        if (g_open && (down & BIT(B_PLUS))) menu_request_toggle();
    }
    if (g_open) capture_tick(down);
    for (LONG t = InterlockedExchange(&g_toggle_req, 0); t > 0; t--) set_open(!g_open);
    if (!g_open && g_guard && b == 0 && g_frame - g_closed_at >= 6) InterlockedExchange(&g_guard, 0);      /* closed and every pad button released */
    for (LONG t = InterlockedExchange(&g_stats_req, 0); t > 0; t--) apply_stats(!S.stats);
#ifndef _WIN32
    if (!g_open) InterlockedExchange(&g_text_want, 0);      /* the window thread turns SDL's text input off */
#endif
    if (!g_open && !S.overlay && !S.stats && !g_notice_on && !popup_on()) { Ev drop[8]; while (q_take(drop, 8) == 8) continue; return 0; }
    if (!g_im_ready) im_init(height);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)width, (float)height);
    io.FontGlobalScale = g_base_h > 0 ? (float)height / g_base_h : 1.0f;
    {   static LARGE_INTEGER prev; LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float dt = prev.QuadPart ? (float)((double)(now.QuadPart - prev.QuadPart) / (double)g_qpf.QuadPart) : 1.0f / 60.0f;
        io.DeltaTime = dt > 0.0f && dt < 1.0f ? dt : 1.0f / 60.0f; prev = now; }
    Ev ev[256];
    int n = q_take(ev, 256);
    for (int i = 0; i < n && g_open; i++) {
        Ev &e = ev[i];
        if (e.type == EV_KEY && g_capture >= 0 && e.b) {                      /* a new keyboard binding */
            if (e.a != VK_ESCAPE) { nnpad_kb_bind(g_capture, e.a); char nm[16]; nnpad_vk_name(e.a, nm, sizeof nm); printf("menu: keyboard %s bound to %s\n", nnpad_target_name(g_capture), nm); fflush(stdout); g_capture = -1; }
            else capture_cancel("Esc");
            continue;
        }
        if (e.type == EV_KEY && g_pcap >= 0) { if (e.b && e.a == VK_ESCAPE) capture_cancel("Esc"); continue; }      /* the keyboard does not drive the menu meanwhile */
        switch (e.type) {
        case EV_MOVE: io.AddMousePosEvent((float)e.a, (float)e.b); break;
        case EV_BTN: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_WHEEL: io.AddMouseWheelEvent(0.0f, (float)e.a / (float)WHEEL_DELTA); break;
#ifdef _WIN32
        case EV_CHAR: io.AddInputCharacterUTF16((ImWchar16)e.a); break;
#else
        case EV_CHAR: io.AddInputCharacter((unsigned int)e.a); break;      /* a code point (SDL's text is UTF-8) */
#endif
        case EV_KEY:
            if (e.a == VK_ESCAPE && e.b && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId)) { menu_request_toggle(); break; }
            if (e.a == VK_CONTROL) io.AddKeyEvent(ImGuiMod_Ctrl, e.b != 0);
            if (e.a == VK_SHIFT) io.AddKeyEvent(ImGuiMod_Shift, e.b != 0);
            if (e.a == VK_MENU) io.AddKeyEvent(ImGuiMod_Alt, e.b != 0);
            if (vk_key(e.a) != ImGuiKey_None) io.AddKeyEvent(vk_key(e.a), e.b != 0);
            break;
        }
    }
    pad_feed(io, g_open && !capturing() && !osk_active() ? b : 0);
    if (g_open && !capturing() && (down & BIT(B_L))) { g_section = (g_section + SEC_N - 1) % SEC_N; g_section_req = g_section; }
    if (g_open && !capturing() && (down & BIT(B_R))) { g_section = (g_section + 1) % SEC_N; g_section_req = g_section; }
    io.MouseDrawCursor = false;
    ImGui::NewFrame();
    if (S.overlay) draw_overlay();
    if (S.stats) draw_stats();
    if (g_notice_on) {                                   /* R3: the online match's message, in the middle */
        char text[512];
        AcquireSRWLockShared(&g_notice_lock); snprintf(text, sizeof text, "%s", g_notice); ReleaseSRWLockShared(&g_notice_lock);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowBgAlpha(0.8f);
        ImGui::Begin("##notice", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.2f, 1.0f), "Online match");
        ImGui::Separator();
        ImGui::TextUnformatted(text);
        ImGui::End();
        if (!InterlockedExchange(&g_notice_drawn, 1)) { printf("menu: online notice drawn (presented frame %lld): %s\n", g_frame, text); fflush(stdout); }
    }
    if (popup_on()) {                                    /* O1: "Cannot join this room" and why, at the top, taking no input */
        char title[96], text[512];
        AcquireSRWLockShared(&g_popup_lock); snprintf(title, sizeof title, "%s", g_popup_title); snprintf(text, sizeof text, "%s", g_popup_text); ReleaseSRWLockShared(&g_popup_lock);
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.06f + (g_open ? ImGui::GetFrameHeight() : 0.0f)), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.88f);
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 0.45f, 0.3f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetFontSize() * 1.2f, ImGui::GetFontSize() * 0.8f));
        ImGui::Begin("##popup", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f), "%s", title);
        ImGui::Separator();
        ImGui::TextUnformatted(text);
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        if (!InterlockedExchange(&g_popup_drawn, 1)) { printf("menu: online popup drawn (presented frame %lld): %s: %s\n", g_frame, title, text); fflush(stdout); }
    }
    if (g_open && !osk_active()) draw_menu(width, height);
    ImGui::Render();
#ifndef _WIN32
    InterlockedExchange(&g_text_want, g_open && io.WantTextInput ? 1 : 0);      /* a text box is active: SDL's text input on (window thread) */
#endif
    ImDrawData *dd = ImGui::GetDrawData();
    g_vtx.resize(0); g_idx.resize(0); g_cmd.resize(0);
    for (int i = 0; i < dd->CmdListsCount; i++) {
        const ImDrawList *dl = dd->CmdLists[i];
        uint32_t vbase = (uint32_t)g_vtx.Size, ibase = (uint32_t)g_idx.Size;
        for (int k = 0; k < dl->VtxBuffer.Size; k++) g_vtx.push_back(dl->VtxBuffer[k]);
        for (int k = 0; k < dl->IdxBuffer.Size; k++) g_idx.push_back(dl->IdxBuffer[k]);
        for (int k = 0; k < dl->CmdBuffer.Size; k++) {
            const ImDrawCmd &c = dl->CmdBuffer[k];
            if (c.UserCallback || !c.ElemCount) continue;
            MenuCmd m;
            m.clip[0] = c.ClipRect.x - dd->DisplayPos.x; m.clip[1] = c.ClipRect.y - dd->DisplayPos.y;
            m.clip[2] = c.ClipRect.z - dd->DisplayPos.x; m.clip[3] = c.ClipRect.w - dd->DisplayPos.y;
            m.idx_off = ibase + c.IdxOffset; m.vtx_off = vbase + c.VtxOffset; m.count = c.ElemCount;
            g_cmd.push_back(m);
        }
    }
    if (!g_cmd.Size) return 0;
    out->vtx = g_vtx.Data; out->idx = g_idx.Data; out->cmd = g_cmd.Data;
    out->nvtx = g_vtx.Size; out->nidx = g_idx.Size; out->ncmd = g_cmd.Size;
    out->disp_w = dd->DisplaySize.x; out->disp_h = dd->DisplaySize.y;
    return 1;
}
