/* nnpad_gca.c: the GameCube controller adapter (Nintendo WUP-028, and clones such as the Mayflash 4-port adapter in its
 * "Switch" mode: USB 057e:0337) read straight over WinUSB, the way Dolphin / Slippi read it.
 *
 * Why not SDL alone: the SDL3 for Windows that this project links has no libusb, and the adapter's driver here is WinUSB
 * (the one Slippi's installer / Zadig put on it), which SDL's HIDAPI cannot open. WinUSB is part of Windows, so nothing
 * new is installed: winusb.dll and setupapi are loaded here, and when the adapter has no WinUSB driver nothing happens.
 *
 * What is measured, on a Mayflash in Switch mode with this driver (tests\ and plan notes):
 *   - the adapter answers with a 37-byte report on endpoint 0x81, every 8 ms (125 Hz) with 2 or more reads pending and
 *     WinUSB's RAW_IO set; with one read at a time, or without RAW_IO, only every 16 ms (62.5 Hz). So each adapter has
 *     a thread that keeps DEPTH reads queued and applies every report the moment its read completes: nothing is queued
 *     or smoothed on this side, the newest report is always the state.
 *   - report: 0x21, then 4 ports of 9 bytes: status, buttons 1, buttons 2, stick x, stick y, c-stick x, c-stick y,
 *     trigger L, trigger R. status bits 0x30 = a controller is there (0x10 normal, 0x20 WaveBird), 0x04 = rumble power.
 *     buttons 1: A 01, B 02, X 04, Y 08, left 10, right 20, down 40, up 80; buttons 2: start 01, Z 02, R 04, L 08.
 *   - 0x13 on endpoint 0x02 starts the reports; 0x11 + 4 bytes (one per port, 1 = motor on) sets the rumble.
 *
 * Each connected port becomes one SDL virtual gamepad (vendor 057e, product 0337, name "GameCube Controller (port N)"):
 * nnpad_sdl.c opens it like any pad, and every profile, the settings menu, --pad-test and the player slots work on it.
 * nnpad_sdl.c recognises these devices (gca flag), reports them to the game as GameCube controllers (nnhid.c) and
 * sends their rumble back here through the virtual gamepad's Rumble callback.
 * Sticks and triggers are scaled with fixed constants below (NOT VERIFIED against a console's calibration): the stick
 * origin is the mean of the first reports after the controller appears.
 *
 * SSBU_GCA=0 turns the whole thing off. SSBU_GCA_STATS=1 logs every 2 s the report rate, the delay from a finished read to
 * the SDL state (apply) and the age of the state at the moment the game samples it. */
#ifndef _WIN32
/* Linux build (plat/): no WinUSB here; SDL3 on Linux reads the adapter itself when it has libusb. Nothing to do. */
#include <SDL3/SDL.h>
#include "nnpad.h"
int gca_start(void) { return 0; }
int gca_port_of(unsigned joy_id) { (void)joy_id; return -1; }
void gca_sample(unsigned joy_id) { (void)joy_id; }
unsigned gca_seq(void) { return 0; }
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <setupapi.h>
#include <SDL3/SDL.h>
#include "nnpad.h"

#define GCA_VID 0x057E
#define GCA_PID 0x0337
#define MAX_ADAPTERS 2
#define DEPTH 4
#define REPORT_LEN 37
#define DEFAULT_EP_IN 0x81
#define DEFAULT_EP_OUT 0x02
#define STICK_RANGE 88.0f                     /* counts from the origin to full deflection (SDL's own base range for this adapter; this pad's gate measured ~94) */
#define TRIG_FULL 255                         /* raw trigger value taken as fully pressed */
#define CALIB_REPORTS 5

typedef PVOID WUSB;
#pragma pack(push, 1)
typedef struct _USB_INTERFACE_DESCRIPTOR {
    UCHAR bLength;
    UCHAR bDescriptorType;
    UCHAR bInterfaceNumber;
    UCHAR bAlternateSetting;
    UCHAR bNumEndpoints;
    UCHAR bInterfaceClass;
    UCHAR bInterfaceSubClass;
    UCHAR bInterfaceProtocol;
    UCHAR iInterface;
} USB_INTERFACE_DESCRIPTOR;
typedef struct _WINUSB_PIPE_INFORMATION {
    ULONG PipeType;
    UCHAR PipeId;
    USHORT MaximumPacketSize;
    UCHAR Interval;
} WINUSB_PIPE_INFORMATION;
#pragma pack(pop)

static BOOL (WINAPI *pInit)(HANDLE, WUSB *);
static BOOL (WINAPI *pFree)(WUSB);
static BOOL (WINAPI *pQueryInterfaceSettings)(WUSB, UCHAR, USB_INTERFACE_DESCRIPTOR *);
static BOOL (WINAPI *pQueryPipeInformation)(WUSB, UCHAR, UCHAR, WINUSB_PIPE_INFORMATION *);
static BOOL (WINAPI *pSetPipePolicy)(WUSB, UCHAR, ULONG, ULONG, PVOID);
static BOOL (WINAPI *pRead)(WUSB, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
static BOOL (WINAPI *pWrite)(WUSB, UCHAR, PUCHAR, ULONG, PULONG, LPOVERLAPPED);
static BOOL (WINAPI *pOverlapped)(WUSB, LPOVERLAPPED, PULONG, BOOL);
static BOOL (WINAPI *pAbort)(WUSB, UCHAR);
#define RAW_IO 0x07

typedef struct {
    SDL_JoystickID id;
    SDL_Joystick *js;
    int seen, acc[6];                         /* consecutive reports with a controller, sum of the first CALIB_REPORTS */
    int origin[4], rest[2];
    int calibrated;
    Uint32 btn;
    Sint16 ax[6];
    volatile LONG64 t_apply;                  /* QueryPerformanceCounter of the last report applied to SDL */
} Port;
typedef struct {
    volatile LONG running;
    int index;
    WCHAR path[MAX_PATH];
    UCHAR ep_in;
    UCHAR ep_out;
    Port port[4];
    volatile LONG rumble[4];
} Adapter;

static Adapter g_ad[MAX_ADAPTERS];
static char g_names[MAX_ADAPTERS][4][48];
static LARGE_INTEGER g_qpf;
static int g_stats = -1;
static volatile LONG64 g_age_sum, g_age_max, g_age_n;      /* game-side samples: age of the state in microseconds */
static int g_started;
static volatile LONG g_seq;                                /* reports applied so far: nnpad_poll refreshes SDL at once when this moved */
unsigned gca_seq(void) { return (unsigned)g_seq; }

static int stats_on(void) { if (g_stats < 0) { const char *e = getenv("SSBU_GCA_STATS"); g_stats = e && e[0] && e[0] != '0'; } return g_stats; }
static LONG64 now_ticks(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static double ticks_us(LONG64 t) { return (double)t * 1e6 / (double)g_qpf.QuadPart; }

/* ================================================================ SDL side */
static bool SDLCALL gca_rumble_cb(void *u, Uint16 lo, Uint16 hi) {
    int i = (int)(intptr_t)u, a = i >> 2, p = i & 3;
    if (a < MAX_ADAPTERS) InterlockedExchange(&g_ad[a].rumble[p], (lo | hi) != 0);      /* the adapter's thread sends it: never call out of here (SDL holds its joystick lock) */
    return true;
}
static void port_attach(Adapter *a, int p) {
    SDL_VirtualJoystickDesc d;
    Port *pt = &a->port[p];
    SDL_INIT_INTERFACE(&d);
    if (a->index == 0) snprintf(g_names[a->index][p], sizeof g_names[0][0], "GameCube Controller (port %d)", p + 1);
    else snprintf(g_names[a->index][p], sizeof g_names[0][0], "GameCube Controller (adapter %d, port %d)", a->index + 1, p + 1);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = GCA_VID; d.product_id = GCA_PID;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT; d.nhats = 0;
    d.name = g_names[a->index][p];
    d.userdata = (void *)(intptr_t)(a->index * 4 + p);
    d.Rumble = gca_rumble_cb;
    pt->id = SDL_AttachVirtualJoystick(&d);
    if (!pt->id) { printf("nn::gca: cannot make a virtual gamepad for port %d: %s\n", p + 1, SDL_GetError()); return; }
    pt->js = SDL_OpenJoystick(pt->id);
    if (pt->js) {
        for (int i = 0; i < 4; i++) SDL_SetJoystickVirtualAxis(pt->js, i, 0);
        SDL_SetJoystickVirtualAxis(pt->js, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);     /* a virtual trigger rests at the bottom of its raw range */
        SDL_SetJoystickVirtualAxis(pt->js, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    }
    for (int i = 0; i < 4; i++) pt->ax[i] = 0;
    pt->ax[4] = pt->ax[5] = -32768;
    pt->btn = 0;
    printf("nn::gca: adapter %d port %d: controller connected (%s)\n", a->index + 1, p + 1, g_names[a->index][p]);
    fflush(stdout);
}
static void port_detach(Adapter *a, int p) {
    Port *pt = &a->port[p];
    if (pt->id) {
        if (pt->js) SDL_CloseJoystick(pt->js);
        SDL_DetachVirtualJoystick(pt->id);
        printf("nn::gca: adapter %d port %d: controller gone\n", a->index + 1, p + 1);
        fflush(stdout);
    }
    memset(pt, 0, sizeof *pt);
    InterlockedExchange(&a->rumble[p], 0);
}
static Sint16 stick16(int v, int origin, int invert) {
    float f = ((float)(v - origin)) / STICK_RANGE;
    if (f > 1) f = 1; if (f < -1) f = -1;
    if (invert) f = -f;
    return (Sint16)(f * 32767.0f);
}
static Sint16 trig16(int v, int rest) {
    float f = (float)(v - rest) / (float)(TRIG_FULL - rest);
    if (f < 0) f = 0; if (f > 1) f = 1;
    return (Sint16)(-32768.0f + f * 65535.0f);
}
/* one port's 9 bytes: status, b1, b2, stick x / y, c-stick x / y, trigger L / R */
static void port_apply(Adapter *a, int p, const unsigned char *s, LONG64 t_done) {
    Port *pt = &a->port[p];
    int present = (s[0] & 0x30) != 0;
    if (!present) { if (pt->id) port_detach(a, p); pt->seen = 0; pt->calibrated = 0; memset(pt->acc, 0, sizeof pt->acc); return; }
    if (!pt->calibrated) {
        int v[6] = {s[3], s[4], s[5], s[6], s[7], s[8]};
        for (int i = 0; i < 6; i++) pt->acc[i] += v[i];
        if (++pt->seen < CALIB_REPORTS) return;
        for (int i = 0; i < 4; i++) { int o = pt->acc[i] / CALIB_REPORTS; pt->origin[i] = o > 128 - 50 && o < 128 + 50 ? o : 128; }
        for (int i = 0; i < 2; i++) { int r = pt->acc[4 + i] / CALIB_REPORTS; pt->rest[i] = r < TRIG_FULL - 40 ? r : 30; }
        pt->calibrated = 1;
        port_attach(a, p);
        if (!pt->js) return;
    }
    if (!pt->js) return;
    unsigned b1 = s[1], b2 = s[2];
    Uint32 btn = 0;
    static const struct { int sdl; unsigned mask; int second; } bm[] = {
        {SDL_GAMEPAD_BUTTON_SOUTH, 0x01, 0}, {SDL_GAMEPAD_BUTTON_EAST, 0x02, 0}, {SDL_GAMEPAD_BUTTON_WEST, 0x04, 0}, {SDL_GAMEPAD_BUTTON_NORTH, 0x08, 0},
        {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0x10, 0}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x20, 0}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0x40, 0}, {SDL_GAMEPAD_BUTTON_DPAD_UP, 0x80, 0},
        {SDL_GAMEPAD_BUTTON_START, 0x01, 1}, {SDL_GAMEPAD_BUTTON_MISC2, 0x02, 1} /* Z */, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x04, 1}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0x08, 1},
    };
    for (size_t i = 0; i < sizeof bm / sizeof bm[0]; i++) if ((bm[i].second ? b2 : b1) & bm[i].mask) btn |= 1u << bm[i].sdl;
    Uint32 diff = btn ^ pt->btn;
    for (int i = 0; diff; i++, diff >>= 1) if (diff & 1) SDL_SetJoystickVirtualButton(pt->js, i, (btn >> i & 1) != 0);
    pt->btn = btn;
    Sint16 ax[6] = {stick16(s[3], pt->origin[0], 0), stick16(s[4], pt->origin[1], 1), stick16(s[5], pt->origin[2], 0), stick16(s[6], pt->origin[3], 1),
                    trig16(s[7], pt->rest[0]), trig16(s[8], pt->rest[1])};
    for (int i = 0; i < 6; i++) if (ax[i] != pt->ax[i]) { pt->ax[i] = ax[i]; SDL_SetJoystickVirtualAxis(pt->js, i, ax[i]); }
    pt->t_apply = t_done;
    InterlockedIncrement(&g_seq);
}

/* ================================================================ one adapter's thread */
static void send_rumble(Adapter *a, WUSB w, unsigned char *last, int *last_valid, const unsigned char *rep) {
    unsigned char cmd[5] = {0x11, 0, 0, 0, 0};
    int changed = !*last_valid;
    for (int p = 0; p < 4; p++) {
        int power = rep && (rep[1 + 9 * p] & 0x04);
        cmd[1 + p] = (unsigned char)(power && a->rumble[p] ? 1 : 0);
        if (cmd[1 + p] != last[p]) changed = 1;
    }
    if (!changed) return;
    ULONG n = 0;
    if (pWrite(w, a->ep_out, cmd, sizeof cmd, &n, NULL)) {
        memcpy(last, cmd + 1, 4); *last_valid = 1;
    } else {
        static volatile LONG werr;
        if (!InterlockedExchange(&werr, 1)) printf("nn::gca: adapter %d: writing the rumble command failed (error %lu)\n", a->index + 1, GetLastError());
    }
}
static void log_stats(Adapter *a, int reports, double imin, double isum, double imax, double ap_sum, double ap_max) {
    LONG64 n = InterlockedExchange64(&g_age_n, 0), sum = InterlockedExchange64(&g_age_sum, 0), mx = InterlockedExchange64(&g_age_max, 0);
    printf("nn::gca: adapter %d: %.1f reports/s, interval %.2f / %.2f / %.2f ms (min / avg / max), apply %.0f us avg %.0f us max", a->index + 1,
           reports / 2.0, imin, reports > 1 ? isum / (reports - 1) : 0.0, imax, reports ? ap_sum / reports : 0.0, ap_max);
    if (n) printf(", state age at the game's samples %.2f ms avg %.2f ms max (%lld samples)", (double)sum / (double)n / 1000.0, (double)mx / 1000.0, (long long)n);
    printf("\n");
    fflush(stdout);
}
static DWORD WINAPI adapter_thread(LPVOID arg) {
    Adapter *a = (Adapter *)arg;
    HANDLE h = CreateFileW(a->path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    WUSB w = NULL;
    static volatile LONG warned;
    if (h == INVALID_HANDLE_VALUE) { if (!InterlockedExchange(&warned, 1)) printf("nn::gca: cannot open the adapter (error %lu)\n", GetLastError()); goto done_nohandle; }
    if (!pInit(h, &w)) {
        if (!InterlockedExchange(&warned, 1)) printf("nn::gca: the adapter is found but WinUSB cannot claim it (error %lu): is another program (Slippi, Dolphin, a driver tool) using it?\n", GetLastError());
        goto done_handle;
    }
    InterlockedExchange(&warned, 0);
    a->ep_in = DEFAULT_EP_IN;
    a->ep_out = DEFAULT_EP_OUT;
    if (pQueryInterfaceSettings && pQueryPipeInformation) {
        USB_INTERFACE_DESCRIPTOR iface;
        memset(&iface, 0, sizeof iface);
        if (pQueryInterfaceSettings(w, 0, &iface)) {
            for (UCHAR p = 0; p < iface.bNumEndpoints; p++) {
                WINUSB_PIPE_INFORMATION pipe;
                memset(&pipe, 0, sizeof pipe);
                if (pQueryPipeInformation(w, 0, p, &pipe)) {
                    if ((pipe.PipeId & 0x80) && a->ep_in == DEFAULT_EP_IN) a->ep_in = pipe.PipeId;
                    else if (!(pipe.PipeId & 0x80) && a->ep_out == DEFAULT_EP_OUT) a->ep_out = pipe.PipeId;
                }
            }
        }
    }
    {
        unsigned char raw = 1, start = 0x13, last[4] = {0}, rep_last[REPORT_LEN] = {0};
        int last_valid = 0, have_rep = 0, reports = 0, i, next = 0;
        ULONG n = 0;
        OVERLAPPED ov[DEPTH];
        unsigned char buf[DEPTH][REPORT_LEN];
        LONG64 t_prev = 0, t_win = now_ticks();
        double imin = 1e9, isum = 0, imax = 0, ap_sum = 0, ap_max = 0;
        int dead = 0;
        pSetPipePolicy(w, a->ep_in, RAW_IO, 1, &raw);
        if (!pWrite(w, a->ep_out, &start, 1, &n, NULL)) { printf("nn::gca: the adapter does not accept the start command (error %lu)\n", GetLastError()); goto done_wusb; }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        memset(ov, 0, sizeof ov);
        for (i = 0; i < DEPTH; i++) ov[i].hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        for (i = 0; i < DEPTH; i++) {
            if (!pRead(w, a->ep_in, buf[i], REPORT_LEN, NULL, &ov[i]) && GetLastError() != ERROR_IO_PENDING) dead = 1;
        }
        printf("nn::gca: adapter %d opened (%d reads queued, RAW_IO, in 0x%02x, out 0x%02x)\n", a->index + 1, DEPTH, a->ep_in, a->ep_out);
        fflush(stdout);
        while (!dead) {
            DWORD r = WaitForSingleObject(ov[next].hEvent, 1500);
            if (r != WAIT_OBJECT_0) { printf("nn::gca: adapter %d: no report for 1.5 s, closing it\n", a->index + 1); break; }
            ULONG got = 0;
            LONG64 t_done = now_ticks();
            if (!pOverlapped(w, &ov[next], &got, FALSE)) { dead = 1; break; }
            if (got == REPORT_LEN && buf[next][0] == 0x21) {
                for (int p = 0; p < 4; p++) port_apply(a, p, &buf[next][1 + 9 * p], t_done);
                memcpy(rep_last, buf[next], REPORT_LEN); have_rep = 1;
                if (stats_on()) {
                    LONG64 t_ap = now_ticks();
                    double ap = ticks_us(t_ap - t_done);
                    reports++; ap_sum += ap; if (ap > ap_max) ap_max = ap;
                    if (t_prev) { double iv = ticks_us(t_done - t_prev) / 1000.0; isum += iv; if (iv < imin) imin = iv; if (iv > imax) imax = iv; }
                    t_prev = t_done;
                    if (ticks_us(t_ap - t_win) >= 2e6) { log_stats(a, reports, imin, isum, imax, ap_sum, ap_max); reports = 0; imin = 1e9; isum = imax = ap_sum = ap_max = 0; t_win = t_ap; t_prev = 0; }
                }
            }
            ResetEvent(ov[next].hEvent);
            if (!pRead(w, a->ep_in, buf[next], REPORT_LEN, NULL, &ov[next]) && GetLastError() != ERROR_IO_PENDING) { dead = 1; break; }
            next = (next + 1) % DEPTH;
            send_rumble(a, w, last, &last_valid, have_rep ? rep_last : NULL);
        }
        pAbort(w, a->ep_in);
        for (i = 0; i < DEPTH; i++) { ULONG g; pOverlapped(w, &ov[i], &g, TRUE); CloseHandle(ov[i].hEvent); }
    }
done_wusb:
    for (int p = 0; p < 4; p++) port_detach(a, p);
    pFree(w);
done_handle:
    CloseHandle(h);
done_nohandle:
    InterlockedExchange(&a->running, 0);
    return 0;
}

/* ================================================================ finding the adapter */
static int path_known(const WCHAR *path) {
    for (int i = 0; i < MAX_ADAPTERS; i++) if (g_ad[i].running && !_wcsicmp(g_ad[i].path, path)) return 1;
    return 0;
}
static void start_adapter(const WCHAR *path) {
    for (int i = 0; i < MAX_ADAPTERS; i++) {
        if (g_ad[i].running) continue;
        memset(&g_ad[i], 0, sizeof g_ad[i]);
        g_ad[i].index = i;
        wcsncpy(g_ad[i].path, path, MAX_PATH - 1);
        g_ad[i].running = 1;
        HANDLE t = CreateThread(NULL, 0, adapter_thread, &g_ad[i], 0, NULL);
        if (t) CloseHandle(t); else g_ad[i].running = 0;
        return;
    }
}
/* every present USB device 057e:0337 whose driver registered a device interface (WinUSB does): its interface path */
static void scan(void) {
    static int hinted;
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"USB", NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA dd;
    int found = 0, usable = 0;
    if (set == INVALID_HANDLE_VALUE) return;
    dd.cbSize = sizeof dd;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dd); i++) {
        WCHAR id[200];
        if (!SetupDiGetDeviceInstanceIdW(set, &dd, id, 200, NULL)) continue;
        if (!wcsstr(id, L"VID_057E&PID_0337") && !wcsstr(id, L"vid_057e&pid_0337")) continue;
        found++;
        HKEY k = SetupDiOpenDevRegKey(set, &dd, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        WCHAR guids[320];
        DWORD type = 0, size = sizeof guids - 4;
        memset(guids, 0, sizeof guids);
        LONG rc = RegQueryValueExW(k, L"DeviceInterfaceGUIDs", NULL, &type, (LPBYTE)guids, &size);
        RegCloseKey(k);
        GUID g;
        if (rc != ERROR_SUCCESS || (type != REG_MULTI_SZ && type != REG_SZ) || CLSIDFromString(guids, &g) != S_OK) continue;
        HDEVINFO is = SetupDiGetClassDevsW(&g, NULL, NULL, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        if (is == INVALID_HANDLE_VALUE) continue;
        SP_DEVICE_INTERFACE_DATA ifd;
        ifd.cbSize = sizeof ifd;
        for (DWORD j = 0; SetupDiEnumDeviceInterfaces(is, NULL, &g, j, &ifd); j++) {
            DWORD need = 0;
            SetupDiGetDeviceInterfaceDetailW(is, &ifd, NULL, 0, &need, NULL);
            SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)malloc(need);
            if (!det) continue;
            det->cbSize = sizeof *det;
            if (SetupDiGetDeviceInterfaceDetailW(is, &ifd, det, need, NULL, NULL)) {
                usable++;
                if (!path_known(det->DevicePath)) start_adapter(det->DevicePath);
            }
            free(det);
        }
        SetupDiDestroyDeviceInfoList(is);
    }
    SetupDiDestroyDeviceInfoList(set);
    if (found && !usable && !hinted) {
        hinted = 1;
        printf("nn::gca: a GameCube adapter (USB 057e:0337) is plugged in but has no WinUSB driver, so it cannot be read. Slippi's installer (\"install GameCube adapter drivers\") or Zadig (WinUSB on \"WUP-028\") installs it.\n");
        fflush(stdout);
    }
}
static DWORD WINAPI manager_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        scan();
        Sleep(2000);
    }
    return 0;
}

int gca_start(void) {
    const char *e = getenv("SSBU_GCA");
    HMODULE wu;
    if (g_started) return 1;
    if (e && e[0] == '0') return 0;
    wu = LoadLibraryA("winusb.dll");
    if (!wu) return 0;
    pInit = (void *)GetProcAddress(wu, "WinUsb_Initialize");
    pFree = (void *)GetProcAddress(wu, "WinUsb_Free");
    pQueryInterfaceSettings = (void *)GetProcAddress(wu, "WinUsb_QueryInterfaceSettings");
    pQueryPipeInformation = (void *)GetProcAddress(wu, "WinUsb_QueryPipeInformation");
    pSetPipePolicy = (void *)GetProcAddress(wu, "WinUsb_SetPipePolicy");
    pRead = (void *)GetProcAddress(wu, "WinUsb_ReadPipe");
    pWrite = (void *)GetProcAddress(wu, "WinUsb_WritePipe");
    pOverlapped = (void *)GetProcAddress(wu, "WinUsb_GetOverlappedResult");
    pAbort = (void *)GetProcAddress(wu, "WinUsb_AbortPipe");
    if (!pInit || !pFree || !pSetPipePolicy || !pRead || !pWrite || !pOverlapped || !pAbort) return 0;
    QueryPerformanceFrequency(&g_qpf);
    g_started = 1;
    scan();                                                  /* the first scan runs here, so an adapter that is already in is opening before the first poll */
    { HANDLE t = CreateThread(NULL, 0, manager_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
    return 1;
}
int gca_port_of(unsigned joy_id) {
    for (int a = 0; a < MAX_ADAPTERS; a++) for (int p = 0; p < 4; p++) if (g_ad[a].port[p].id && g_ad[a].port[p].id == (SDL_JoystickID)joy_id) return a * 4 + p;
    return -1;
}
void gca_sample(unsigned joy_id) {
    if (!g_started || !stats_on()) return;
    int i = gca_port_of(joy_id);
    if (i < 0) return;
    LONG64 t = g_ad[i >> 2].port[i & 3].t_apply;
    if (!t) return;
    LONG64 age = (LONG64)ticks_us(now_ticks() - t), cur;
    InterlockedExchangeAdd64(&g_age_sum, age);
    InterlockedIncrement64(&g_age_n);
    while (age > (cur = g_age_max)) if (InterlockedCompareExchange64(&g_age_max, age, cur) == cur) break;
}
#endif
