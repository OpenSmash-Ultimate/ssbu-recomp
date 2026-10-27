/* nnpad_sdl.c: every pad the game can see, through SDL3 (public release 3.4.18, reference\upstream).
 * SDL is started with the gamepad / joystick / events subsystems only: no video, no window, nothing that touches this
 * process's window messages. Devices are enumerated with SDL_GetJoysticks on each poll (hot-plug by diffing the list);
 * SDL's own event queue is flushed unread. A device SDL has a mapping for is opened as a GAMEPAD (Xbox, PlayStation,
 * Switch Pro / Joy-Con, GameCube adapter, 8BitDo, generic mapped HID / DirectInput pads); one without a mapping is
 * opened as a plain JOYSTICK and gets a guessed default (buttons in order, hat 0 = d-pad, axes 0..3 = sticks) that
 * controllers.ini or `--pad-map` corrects.
 * What the file does and how it is written: see the comment block written to controllers.ini (default_ini below)
 * and plan\phases\C1_controllers.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <windows.h>
#include <SDL3/SDL.h>
#include "nnpad.h"

#define MAXDEV 16
#define MAXCFG 64
#define NTYPE 6
static const char *const g_type_key[NTYPE] = {"xbox", "playstation", "switch", "gamecube", "generic", "joystick"};
enum { SK_NONE = 0, SK_BTN, SK_AXP, SK_AXN, SK_HAT, SK_AXF };
typedef struct { unsigned char kind, raw; short idx; } Src;     /* raw: joystick numbering (button3, axis2, hat0up); else SDL gamepad names */
typedef struct {
    char guid[40], name[80];
    int layout;                 /* -1 unset, 0 positional, 1 labels, 2 auto */
    float dz, outer, trig, rstr;/* < 0 = unset */
    int rumble;                 /* -1 unset */
    int inv[4];
    Src axis[4]; char has_axis[4];
    Src map[24][4]; int nmap[24];       /* -1 = unset: the default for this device. 0..15 Pro buttons, 16..23 stick directions (g_kbn order) */
} DevCfg;
typedef struct {
    char path[MAX_PATH]; ULONGLONG mtime;
    DevCfg def; DevCfg dev[MAXCFG]; int ndev;
    DevCfg type[NTYPE];                 /* U1: [type xbox|playstation|switch|gamecube|generic|joystick]: a profile for every device of that kind */
    char pguid[8][40]; int pnone[8];    /* [players]: a GUID reserves the slot for that device, none = nobody */
    char ignore[16][40]; int nignore;   /* [general] ignore = <GUID>: devices that are never used */
    int kb_player;                      /* -2 auto, -1 none, 0..7 */
    int kb_vk[24];
    int kb_move;                        /* [keyboard] movement = ssf2: 1 (nnhid.c kb_ssf2); default full: 0 */
    int gc_pro;                         /* [general] gamecube_style = pro: GameCube controllers reach the game as Pro Controllers (default: native) */
} Cfg;
typedef struct {
    int used, ignored;
    SDL_JoystickID id; SDL_Gamepad *gp; SDL_Joystick *js;
    int slot, wireless, dropped, nbtn, naxes, nhats, gtype, has_accel, has_gyro, virt, gca;      /* gca: a port of the GameCube adapter (nnpad_gca.c) */
    char guid[40], name[80];
    Uint16 rl, rh; ULONGLONG rsent;
} Dev;

static const char *const g_pro[16] = {"A", "B", "X", "Y", "STICK_L", "STICK_R", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
static const char *const g_gpbtn[] = {"south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder", "rightshoulder",
    "dpup", "dpdown", "dpleft", "dpright", "misc1", "rightpaddle1", "leftpaddle1", "rightpaddle2", "leftpaddle2", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"};
static const char *const g_gpax[6] = {"leftx", "lefty", "rightx", "righty", "lefttrigger", "righttrigger"};
static const char *const g_stk[4] = {"LX", "LY", "RX", "RY"};
static const char *const g_kbn[24] = {"A", "B", "X", "Y", "STICK_L", "STICK_R", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN",
    "LSTICK_LEFT", "LSTICK_UP", "LSTICK_RIGHT", "LSTICK_DOWN", "RSTICK_LEFT", "RSTICK_UP", "RSTICK_RIGHT", "RSTICK_DOWN"};
static const int g_kb_default[24] = {'Z', 'X', 'C', 'V', 0, 0, 'Q', 'E', 'A', 'S', VK_RETURN, VK_BACK, 'J', 'I', 'L', 'K',
    VK_LEFT, VK_UP, VK_RIGHT, VK_DOWN, 0, 0, 0, 0};

static CRITICAL_SECTION g_cs;
static volatile LONG g_cs_ready;
static int g_sdl_ok, g_inited, g_only_virtual, g_quiet;
static Cfg g_cfg;
static Dev g_dev[MAXDEV];
static struct { char guid[40]; int slot; } g_mem[32];
static int g_mem_n;
static int g_kb_slot = -3;                  /* resolved lazily */
static ULONGLONG g_init_ms;
static LARGE_INTEGER g_qpf, g_last_poll;
static ULONGLONG g_last_cfg_check;

static void lock(void) { if (!g_cs_ready) { if (InterlockedCompareExchange(&g_cs_ready, 2, 0) == 0) { InitializeCriticalSection(&g_cs); g_cs_ready = 1; } while (g_cs_ready != 1) Sleep(0); } EnterCriticalSection(&g_cs); }
static void unlock(void) { LeaveCriticalSection(&g_cs); }
#define say(...) do { if (!g_quiet) printf(__VA_ARGS__); } while (0)

/* ================================================================ sources
 * gamepad devices: south east west north back guide start leftstick rightstick leftshoulder rightshoulder dpup dpdown dpleft dpright
 *                  misc1 rightpaddle1 leftpaddle1 rightpaddle2 leftpaddle2 touchpad misc2..misc6, and the axes leftx lefty rightx righty
 *                  lefttrigger righttrigger with an optional + or - (half of the axis).
 * any device:      button<N>, axis<N>[+|-], hat<N>up|right|down|left (the device's own numbering, 0-based) */
static void src_name(Src s, char *o, size_t n) {
    static const char *const dir[4] = {"up", "right", "down", "left"};
    o[0] = 0;
    if (s.kind == SK_NONE) snprintf(o, n, "none");
    else if (!s.raw && s.kind == SK_BTN) snprintf(o, n, "%s", s.idx >= 0 && s.idx < (int)(sizeof g_gpbtn / sizeof g_gpbtn[0]) ? g_gpbtn[s.idx] : "?");
    else if (!s.raw) snprintf(o, n, "%s%s", s.idx >= 0 && s.idx < 6 ? g_gpax[s.idx] : "?", s.kind == SK_AXP ? (s.idx >= 4 ? "" : "+") : s.kind == SK_AXN ? "-" : "");
    else if (s.kind == SK_BTN) snprintf(o, n, "button%d", s.idx);
    else if (s.kind == SK_HAT) { int d = 0; while (d < 3 && !((s.idx & 15) >> d & 1)) d++; snprintf(o, n, "hat%d%s", s.idx >> 4, dir[d]); }
    else snprintf(o, n, "axis%d%s", s.idx, s.kind == SK_AXP ? "+" : s.kind == SK_AXN ? "-" : "");
}
static int src_parse(const char *t, Src *s) {
    char b[48];
    size_t n;
    memset(s, 0, sizeof *s);
    while (*t == ' ' || *t == '\t') t++;
    snprintf(b, sizeof b, "%s", t);
    n = strlen(b);
    while (n && (b[n - 1] == ' ' || b[n - 1] == '\t' || b[n - 1] == '\r' || b[n - 1] == '\n')) b[--n] = 0;
    if (!_stricmp(b, "none") || !b[0]) return 1;
    for (int i = 0; i < (int)(sizeof g_gpbtn / sizeof g_gpbtn[0]); i++) if (!_stricmp(b, g_gpbtn[i])) { s->kind = SK_BTN; s->idx = (short)i; return 1; }
    for (int i = 0; i < 6; i++) {
        size_t l = strlen(g_gpax[i]);
        if (_strnicmp(b, g_gpax[i], l)) continue;
        if (!b[l]) { s->kind = i >= 4 ? SK_AXP : SK_AXF; s->idx = (short)i; return 1; }
        if (!b[l + 1] && (b[l] == '+' || b[l] == '-')) { s->kind = b[l] == '+' ? SK_AXP : SK_AXN; s->idx = (short)i; return 1; }
    }
    s->raw = 1;
    if (!_strnicmp(b, "button", 6) && b[6] >= '0' && b[6] <= '9') { s->kind = SK_BTN; s->idx = (short)atoi(b + 6); return 1; }
    if (!_strnicmp(b, "axis", 4) && b[4] >= '0' && b[4] <= '9') {
        char *e; int i = (int)strtol(b + 4, &e, 10);
        s->idx = (short)i; s->kind = *e == '+' ? SK_AXP : *e == '-' ? SK_AXN : SK_AXF;
        return !*e || ((*e == '+' || *e == '-') && !e[1]);
    }
    if (!_strnicmp(b, "hat", 3) && b[3] >= '0' && b[3] <= '9') {
        char *e; int h = (int)strtol(b + 3, &e, 10);
        static const char *const dir[4] = {"up", "right", "down", "left"};
        for (int d = 0; d < 4; d++) if (!_stricmp(e, dir[d])) { s->kind = SK_HAT; s->idx = (short)(h << 4 | 1 << d); return 1; }
    }
    memset(s, 0, sizeof *s);
    return 0;
}

/* ================================================================ configuration */
static const char *const g_default_ini =
"; controllers.ini: pads and keyboard for the native build (read by nnpad_sdl.c; reloaded within a second of being saved).\r\n"
"; Lines starting with ; or # are comments. Names are not case sensitive. Delete this file to get it written again.\r\n"
";\r\n"
"; Find out what SDL sees:       ssbu_native.exe --pad-test       (devices, GUIDs, live button names)\r\n"
"; Map a pad by pressing keys:   ssbu_native.exe --pad-map 1      (player 1; writes a [device <GUID>] section below)\r\n"
";\r\n"
"; Pro Controller buttons: A B X Y STICK_L STICK_R L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN\r\n"
"; Sources on a pad SDL has a mapping for: south east west north back guide start leftstick rightstick leftshoulder\r\n"
";   rightshoulder dpup dpdown dpleft dpright misc1 rightpaddle1 leftpaddle1 rightpaddle2 leftpaddle2 touchpad misc2..misc6\r\n"
";   and the axes leftx lefty rightx righty lefttrigger righttrigger (leftx+ / leftx- = the positive / negative half).\r\n"
"; Sources on any device (the device's own numbers, from 0): button<N>  axis<N>[+|-]  hat<N>up|right|down|left\r\n"
"\r\n"
"[general]\r\n"
"; Face buttons. positional = the button in Nintendo's A position (right) acts as A: right = A, bottom = B, top = X, left = Y.\r\n"
";   labels = the printed letter decides (an Xbox pad's A is Pro A). auto = labels for GameCube controllers, positional otherwise.\r\n"
"layout = auto\r\n"
"; Radial dead zone and outer zone of the sticks (fractions of full travel): inside the dead zone nothing, beyond the outer\r\n"
"; zone full deflection; the stick is rescaled in between and its length is limited to 1 (circular gate).\r\n"
"deadzone = 0.10\r\n"
"outer_zone = 0.98\r\n"
"; Trigger travel (0..1) from which ZL / ZR (or any axis mapped to a button) counts as pressed.\r\n"
"trigger_threshold = 0.33\r\n"
"; Vibration: on / off and a strength factor (0..2)\r\n"
"rumble = on\r\n"
"rumble_strength = 1.0\r\n"
"; GameCube controllers on the GameCube adapter (Nintendo WUP-028, Mayflash in Switch mode; needs the adapter's WinUSB driver,\r\n"
"; see nnpad_gca.c): native = the game sees real GameCube controllers (as on the console); pro = the game sees Pro Controllers.\r\n"
"gamecube_style = native\r\n"
"; Devices that must never be used (GUIDs from --pad-test; e.g. a keyboard receiver that presents itself as a joystick). One line per device:\r\n"
"; ignore = 03000000efbe00006d04000000000000\r\n"
"\r\n"
"[players]\r\n"
"; Which device is player 1..8: auto (first free slot when it connects; a pad that comes back gets its old slot if free),\r\n"
"; a device GUID (from --pad-test; that slot is kept for that device) or none (nobody uses the slot).\r\n"
"player1 = auto\r\nplayer2 = auto\r\nplayer3 = auto\r\nplayer4 = auto\r\nplayer5 = auto\r\nplayer6 = auto\r\nplayer7 = auto\r\nplayer8 = auto\r\n"
"\r\n"
"[keyboard]\r\n"
"; auto = the first free player slot after the pads found at start (so a machine with one pad has pad = player 1, keyboard = player 2,\r\n"
"; and without any pad the keyboard is player 1); 1..8 = that player (merged with the pad of that player); none = no keyboard.\r\n"
"; The environment variable SSBU_KEYBOARD_PLAYER (0 = none, 1..8) overrides this line. The keyboard counts only while the game window has the focus.\r\n"
"player = auto\r\n"
"; Keys: a letter or digit, LEFT UP RIGHT DOWN ENTER BACKSPACE SPACE TAB ESC SHIFT CTRL ALT F1..F12, NUM0..NUM9, or 0x<virtual key code>; none = unbound.\r\n"
"A = Z\r\nB = X\r\nX = C\r\nY = V\r\nL = Q\r\nR = E\r\nZL = A\r\nZR = S\r\nPLUS = ENTER\r\nMINUS = BACKSPACE\r\n"
"STICK_L = none\r\nSTICK_R = none\r\nLEFT = J\r\nUP = I\r\nRIGHT = L\r\nDOWN = K\r\n"
"; movement = full: a stick key pushes the stick fully, as a button does. movement = ssf2: the left stick keys as in Super\r\n"
";   Smash Flash 2: a key walks, the same key again within 200 ms runs, down crouches, down twice drops through a platform,\r\n"
";   a key and A within about 130 ms is a smash attack, up and A or B within 4 frames is an up smash / up special (BUILDING.md).\r\n"
"; The environment variable SSBU_KEYBOARD_MOVEMENT (full | ssf2) overrides this line.\r\n"
"movement = full\r\n"
"LSTICK_LEFT = LEFT\r\nLSTICK_UP = UP\r\nLSTICK_RIGHT = RIGHT\r\nLSTICK_DOWN = DOWN\r\n"
"RSTICK_LEFT = none\r\nRSTICK_UP = none\r\nRSTICK_RIGHT = none\r\nRSTICK_DOWN = none\r\n"
"\r\n"
"; Per device (GUID from --pad-test). Every key is optional; what is missing falls back to [general] and the built-in default:\r\n"
"; [device 03000000de280000ff11000000007801]\r\n"
"; name = Example pad\r\n"
"; layout = labels            ; positional | labels | auto\r\n"
"; deadzone = 0.15\r\n"
"; outer_zone = 0.95\r\n"
"; trigger_threshold = 0.5\r\n"
"; rumble = off\r\n"
"; rumble_strength = 0.6\r\n"
"; map.A = south, button7      ; any source(s) for a Pro button; the button is pressed when any of them is\r\n"
"; map.ZL = leftshoulder\r\n"
"; axis.LX = leftx             ; which axis feeds a Pro stick axis (LX LY RX RY)\r\n"
"; invert.LY = on              ; flip it (up is already handled: SDL's y axis is down-positive)\r\n"
"; map.LSTICK_LEFT = dpleft    ; a stick direction from its own source(s) (LSTICK_ / RSTICK_ LEFT UP RIGHT DOWN): a button\r\n"
";                             ; pushes the stick fully, an axis half as far as it is moved; the other direction keeps the axis\r\n"
";\r\n"
"; Per kind of controller, the same keys: [type xbox] [type playstation] [type switch] [type gamecube] [type generic]\r\n"
"; [type joystick] (a pad without an SDL mapping). Used for every connected controller of that kind; a [device] section wins.\r\n"
"; The settings menu (F1 or Home, page Controls) writes these sections.\r\n";

static void cfg_init_dev(DevCfg *d) {
    memset(d, 0, sizeof *d);
    d->layout = -1; d->dz = d->outer = d->trig = d->rstr = -1.0f; d->rumble = -1;
    for (int i = 0; i < 24; i++) d->nmap[i] = -1;
}
static void cfg_defaults(Cfg *c) {
    memset(c, 0, sizeof *c);
    cfg_init_dev(&c->def);
    for (int t = 0; t < NTYPE; t++) cfg_init_dev(&c->type[t]);
    c->def.layout = 2; c->def.dz = 0.10f; c->def.outer = 0.98f; c->def.trig = 0.33f; c->def.rstr = 1.0f; c->def.rumble = 1;
    c->kb_player = -2;
    memcpy(c->kb_vk, g_kb_default, sizeof c->kb_vk);
}
static int parse_vk(const char *t) {
    char b[24];
    snprintf(b, sizeof b, "%s", t);
    for (char *p = b + strlen(b); p > b && (p[-1] == ' ' || p[-1] == '\t' || p[-1] == '\r' || p[-1] == '\n'); ) *--p = 0;
    if (!b[0] || !_stricmp(b, "none")) return 0;
    if (b[1] == 0) { char ch = b[0]; if (ch >= 'a' && ch <= 'z') ch -= 32; return ch; }
    if (!_stricmp(b, "LEFT")) return VK_LEFT; if (!_stricmp(b, "UP")) return VK_UP;
    if (!_stricmp(b, "RIGHT")) return VK_RIGHT; if (!_stricmp(b, "DOWN")) return VK_DOWN;
    if (!_stricmp(b, "ENTER")) return VK_RETURN; if (!_stricmp(b, "BACKSPACE")) return VK_BACK;
    if (!_stricmp(b, "SPACE")) return VK_SPACE; if (!_stricmp(b, "TAB")) return VK_TAB; if (!_stricmp(b, "ESC")) return VK_ESCAPE;
    if (!_stricmp(b, "SHIFT")) return VK_SHIFT; if (!_stricmp(b, "CTRL")) return VK_CONTROL; if (!_stricmp(b, "ALT")) return VK_MENU;
    if ((b[0] == 'F' || b[0] == 'f') && atoi(b + 1) >= 1 && atoi(b + 1) <= 12) return VK_F1 + atoi(b + 1) - 1;
    if (!_strnicmp(b, "NUM", 3) && b[3] >= '0' && b[3] <= '9') return VK_NUMPAD0 + b[3] - '0';
    if (b[0] == '0' && (b[1] == 'x' || b[1] == 'X')) return (int)strtol(b, NULL, 16);
    return 0;
}
static int parse_bool(const char *v) { return !(_stricmp(v, "off") && _stricmp(v, "0") && _stricmp(v, "no") && _stricmp(v, "false")) ? 0 : 1; }

static DevCfg *cfg_find(Cfg *c, const char *guid, int create) {
    for (int i = 0; i < c->ndev; i++) if (!_stricmp(c->dev[i].guid, guid)) return &c->dev[i];
    if (!create || c->ndev >= MAXCFG) return NULL;
    cfg_init_dev(&c->dev[c->ndev]);
    snprintf(c->dev[c->ndev].guid, sizeof c->dev[0].guid, "%s", guid);
    return &c->dev[c->ndev++];
}
static void cfg_set(Cfg *c, DevCfg *d, int in_dev, const char *key, char *val) {
    if (!_stricmp(key, "layout")) d->layout = !_stricmp(val, "labels") || !_stricmp(val, "label") ? 1 : !_stricmp(val, "auto") ? 2 : 0;
    else if (!_stricmp(key, "deadzone")) d->dz = (float)atof(val);
    else if (!_stricmp(key, "outer_zone")) d->outer = (float)atof(val);
    else if (!_stricmp(key, "trigger_threshold")) d->trig = (float)atof(val);
    else if (!_stricmp(key, "rumble")) d->rumble = parse_bool(val);
    else if (!_stricmp(key, "rumble_strength")) d->rstr = (float)atof(val);
    else if (in_dev && !_stricmp(key, "name")) snprintf(d->name, sizeof d->name, "%s", val);
    else if (in_dev && !_strnicmp(key, "invert.", 7)) { for (int i = 0; i < 4; i++) if (!_stricmp(key + 7, g_stk[i])) d->inv[i] = parse_bool(val); }
    else if (in_dev && !_strnicmp(key, "axis.", 5)) { for (int i = 0; i < 4; i++) if (!_stricmp(key + 5, g_stk[i]) && src_parse(val, &d->axis[i])) d->has_axis[i] = d->axis[i].kind != SK_NONE; }
    else if (in_dev && !_strnicmp(key, "map.", 4)) {
        for (int b = 0; b < 24; b++) {
            if (_stricmp(key + 4, g_kbn[b])) continue;
            d->nmap[b] = 0;
            for (char *tok = val; tok && d->nmap[b] < 4; ) {            /* not strtok: cfg_parse is using it */
                char *comma = strchr(tok, ',');
                Src s;
                if (comma) *comma = 0;
                if (src_parse(tok, &s) && s.kind) d->map[b][d->nmap[b]++] = s;
                tok = comma ? comma + 1 : NULL;
            }
        }
    }
    (void)c;
}
static void cfg_parse(Cfg *c, char *text) {
    DevCfg *cur = NULL;
    int sect = 0;                                     /* 1 general, 2 players, 3 keyboard, 4 device */
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        while (*line == ' ' || *line == '\t') line++;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
        if (!n || *line == ';' || *line == '#') continue;
        if (*line == '[') {
            char *e = strchr(line, ']');
            if (e) *e = 0;
            if (!_stricmp(line + 1, "general")) sect = 1; else if (!_stricmp(line + 1, "players")) sect = 2;
            else if (!_stricmp(line + 1, "keyboard")) sect = 3;
            else if (!_strnicmp(line + 1, "device ", 7)) { char *g = line + 8; while (*g == ' ') g++; cur = cfg_find(c, g, 1); sect = cur ? 4 : 0; }
            else if (!_strnicmp(line + 1, "type ", 5)) {      /* U1: a controller-type profile */
                char *g = line + 6; while (*g == ' ') g++;
                cur = NULL; for (int t = 0; t < NTYPE; t++) if (!_stricmp(g, g_type_key[t])) cur = &c->type[t];
                sect = cur ? 4 : 0;
            }
            else sect = 0;
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        for (char *p = key + strlen(key); p > key && (p[-1] == ' ' || p[-1] == '\t'); ) *--p = 0;
        while (*val == ' ' || *val == '\t') val++;
        if (_stricmp(key, "name")) for (char *p = val; *p; p++) if ((*p == ';' || *p == '#') && p > val && (p[-1] == ' ' || p[-1] == '\t')) { *p = 0; break; }
        for (char *p = val + strlen(val); p > val && (p[-1] == ' ' || p[-1] == '\t'); ) *--p = 0;
        if (sect == 1 && !_stricmp(key, "ignore")) { if (c->nignore < 16) snprintf(c->ignore[c->nignore++], sizeof c->ignore[0], "%s", val); }
        else if (sect == 1 && !_stricmp(key, "gamecube_style")) c->gc_pro = !_stricmp(val, "pro");
        else if (sect == 1) cfg_set(c, &c->def, 0, key, val);
        else if (sect == 4 && cur) cfg_set(c, cur, 1, key, val);
        else if (sect == 2 && !_strnicmp(key, "player", 6) && key[6] >= '1' && key[6] <= '8' && !key[7]) {
            int p = key[6] - '1';
            c->pguid[p][0] = 0; c->pnone[p] = 0;
            if (!_stricmp(val, "none")) c->pnone[p] = 1; else if (_stricmp(val, "auto")) snprintf(c->pguid[p], sizeof c->pguid[0], "%s", val);
        } else if (sect == 3) {
            if (!_stricmp(key, "player")) c->kb_player = !_stricmp(val, "auto") ? -2 : !_stricmp(val, "none") ? -1 : atoi(val) >= 1 && atoi(val) <= 8 ? atoi(val) - 1 : -2;
            else if (!_stricmp(key, "movement")) c->kb_move = !_stricmp(val, "ssf2");
            else for (int i = 0; i < 24; i++) if (!_stricmp(key, g_kbn[i])) c->kb_vk[i] = parse_vk(val);
        }
    }
}
static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 2);
    size_t got = b ? fread(b, 1, (size_t)n, f) : 0;
    fclose(f);
    if (!b) return NULL;
    b[got] = 0; if (len) *len = got;
    return b;
}
static ULONGLONG file_mtime(const char *path) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &a)) return 0;
    return ((ULONGLONG)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
}
static void cfg_path(char *out) {
    const char *e = getenv("SSBU_CONFIG");
    if (e && e[0]) { snprintf(out, MAX_PATH, "%s", e); return; }
    char exe[MAX_PATH], tmp[MAX_PATH];
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(tmp, sizeof tmp, "%s\\..\\config\\controllers.ini", exe);
    if (!_fullpath(out, tmp, MAX_PATH)) snprintf(out, MAX_PATH, "%s", tmp);
}
static void make_dirs(const char *file) {
    char b[MAX_PATH];
    snprintf(b, sizeof b, "%s", file);
    for (char *p = b + 3; *p; p++) if (*p == '\\' || *p == '/') { *p = 0; CreateDirectoryA(b, NULL); *p = '\\'; }
}
static void cfg_load(int first) {                 /* lock held */
    static Cfg n;
    char *text;
    if (first) cfg_path(g_cfg.path);
    if (!g_cfg.path[0]) cfg_path(g_cfg.path);
    if (first && !file_mtime(g_cfg.path)) {
        make_dirs(g_cfg.path);
        FILE *f = fopen(g_cfg.path, "wb");
        if (f) { fwrite(g_default_ini, 1, strlen(g_default_ini), f); fclose(f); say("nn::pad: wrote the default %s\n", g_cfg.path); }
    }
    cfg_defaults(&n);
    text = read_file(g_cfg.path, NULL);
    ULONGLONG mt = file_mtime(g_cfg.path);
    if (text) { cfg_parse(&n, text); free(text); }
    strcpy(n.path, g_cfg.path);
    n.mtime = mt;
    {   const char *l = getenv("SSBU_PAD_LAYOUT");
        if (l && l[0]) n.def.layout = !_stricmp(l, "label") || !_stricmp(l, "labels") ? 1 : 0; }
    g_cfg = n;
}

/* ================================================================ devices */
static const DevCfg *dev_cfg(const Dev *d) { for (int i = 0; i < g_cfg.ndev; i++) if (!_stricmp(g_cfg.dev[i].guid, d->guid)) return &g_cfg.dev[i]; return NULL; }
/* U1: what kind of controller a device is, for the [type ...] profiles: Xbox, PlayStation, Switch (Pro, Joy-Con), GameCube,
 * any other pad SDL has a mapping for, a joystick without a mapping (its sources are its own numbers) */
static int dev_type(const Dev *d) {
    if (!d->gp) return 5;
    switch (d->gtype) {
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return 0;
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return 1;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return 2;
    case SDL_GAMEPAD_TYPE_GAMECUBE: return 3;
    }
    return 4;
}
static const DevCfg *type_cfg(const Dev *d) { return &g_cfg.type[dev_type(d)]; }
/* every setting: the device's own [device GUID] section, else its [type ...] profile, else [general] / the built-in default */
static float eff_f(const Dev *d, size_t off, float def) {
    const DevCfg *c = dev_cfg(d), *t = type_cfg(d);
    float v = c ? *(const float *)((const char *)c + off) : -1.0f;
    (void)def;
    if (v < 0) v = *(const float *)((const char *)t + off);
    return v >= 0 ? v : *(const float *)((const char *)&g_cfg.def + off);
}
#define EFF(d, field) eff_f(d, offsetof(DevCfg, field), 0)
static int eff_layout(const Dev *d) {
    const DevCfg *c = dev_cfg(d), *t = type_cfg(d);
    int l = c && c->layout >= 0 ? c->layout : t->layout >= 0 ? t->layout : g_cfg.def.layout;
    if (l == 2) l = d->gtype == SDL_GAMEPAD_TYPE_GAMECUBE;
    return l;
}
static int eff_rumble(const Dev *d) { const DevCfg *c = dev_cfg(d), *t = type_cfg(d); return c && c->rumble >= 0 ? c->rumble : t->rumble >= 0 ? t->rumble : g_cfg.def.rumble; }

static int slot_usable(int p, const char *guid) {            /* configured reservations */
    if (g_cfg.pnone[p]) return 0;
    if (g_cfg.pguid[p][0] && _stricmp(g_cfg.pguid[p], guid)) return 0;
    return 1;
}
static int slot_taken(int p) { for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used && !g_dev[i].ignored && g_dev[i].slot == p) return 1; return 0; }
static int gca_port_from_name(const char *name) {      /* "GameCube Controller (port 2)" -> 1; "... (adapter 2, port 3)" -> 6; else -1 (nnpad_gca.c names them) */
    const char *p = strstr(name, "port "), *a = strstr(name, "adapter ");
    if (!p || strncmp(name, "GameCube Controller (", 21)) return -1;
    int port = atoi(p + 5) - 1, ad = a ? atoi(a + 8) - 1 : 0;
    return port >= 0 && port < 4 && ad >= 0 && ad < 2 ? ad * 4 + port : -1;
}
static int assign_slot(const Dev *d) {
    for (int p = 0; p < 8; p++) if (g_cfg.pguid[p][0] && !_stricmp(g_cfg.pguid[p], d->guid) && !slot_taken(p)) return p;
    for (int i = 0; i < g_mem_n; i++) {
        int p = g_mem[i].slot;
        if (!_stricmp(g_mem[i].guid, d->guid) && !slot_taken(p) && slot_usable(p, d->guid) && p != g_kb_slot) return p;
    }
    if (d->gca) {      /* adapter port N is player N when that slot is free */
        int p = gca_port_from_name(d->name);
        if (p >= 0 && !slot_taken(p) && slot_usable(p, d->guid) && !g_cfg.pguid[p][0] && p != g_kb_slot) return p;
    }
    for (int p = 0; p < 8; p++) if (!slot_taken(p) && slot_usable(p, d->guid) && !g_cfg.pguid[p][0] && p != g_kb_slot) return p;
    return -1;
}
static void remember(const char *guid, int slot) {
    int i;
    for (i = 0; i < g_mem_n; i++) if (!_stricmp(g_mem[i].guid, guid)) break;
    if (i == g_mem_n) { if (g_mem_n == 32) return; g_mem_n++; snprintf(g_mem[i].guid, sizeof g_mem[0].guid, "%s", guid); }
    g_mem[i].slot = slot;
}
static void apply_player_index(Dev *d) {
    static const Uint8 col[8][3] = {{0, 80, 255}, {255, 30, 30}, {30, 200, 30}, {255, 200, 0}, {200, 0, 255}, {0, 220, 220}, {255, 120, 0}, {255, 255, 255}};
    if (!d->gp || d->slot < 0) return;
    SDL_SetGamepadPlayerIndex(d->gp, d->slot);
    SDL_SetGamepadLED(d->gp, col[d->slot][0], col[d->slot][1], col[d->slot][2]);
}
static const char *type_name(int t) {
    static const char *const n[] = {"unknown", "standard", "Xbox 360", "Xbox One", "PS3", "PS4", "PS5", "Switch Pro", "Joy-Con left", "Joy-Con right", "Joy-Con pair", "GameCube"};
    return t >= 0 && t < (int)(sizeof n / sizeof n[0]) ? n[t] : "other";
}
static void dev_add(SDL_JoystickID id) {
    Dev *d = NULL;
    for (int i = 0; i < MAXDEV && !d; i++) if (!g_dev[i].used) d = &g_dev[i];
    if (!d) return;
    memset(d, 0, sizeof *d);
    d->used = 1; d->id = id; d->slot = -1;
    d->virt = SDL_IsJoystickVirtual(id);
    d->gca = d->virt && SDL_GetJoystickVendorForID(id) == 0x057E && SDL_GetJoystickProductForID(id) == 0x0337;
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id), d->guid, sizeof d->guid);
    snprintf(d->name, sizeof d->name, "%s", SDL_GetJoystickNameForID(id) ? SDL_GetJoystickNameForID(id) : "?");
    if (g_only_virtual && !d->virt) { d->ignored = 1; return; }
    if (SDL_IsGamepad(id)) { d->gp = SDL_OpenGamepad(id); if (d->gp) d->js = SDL_GetGamepadJoystick(d->gp); }
    else d->js = SDL_OpenJoystick(id);
    if (!d->js) { d->ignored = 1; return; }
    d->nbtn = SDL_GetNumJoystickButtons(d->js); d->naxes = SDL_GetNumJoystickAxes(d->js); d->nhats = SDL_GetNumJoystickHats(d->js);
    for (int i = 0; i < g_cfg.nignore; i++) if (!_stricmp(g_cfg.ignore[i], d->guid)) { d->ignored = 1; say("nn::pad: ignored \"%s\" %s (ignore = in controllers.ini)\n", d->name, d->guid); SDL_CloseJoystick(d->js); if (d->gp) SDL_CloseGamepad(d->gp); d->js = NULL; d->gp = NULL; return; }
    if (!d->gp && (!strncmp(d->name, "0x", 2) || d->naxes < 2 || d->nbtn < 4)) {       /* "0xVID/0xPID" is SDL's name for a HID device without a product string: not a pad */ d->ignored = 1; say("nn::pad: ignored \"%s\" %s: no mapping and only %d axes / %d buttons\n", d->name, d->guid, d->naxes, d->nbtn); SDL_CloseJoystick(d->js); d->js = NULL; return; }
    if (d->gp) {
        d->gtype = SDL_GetGamepadType(d->gp);
        if (d->gca) d->gtype = SDL_GAMEPAD_TYPE_GAMECUBE;
        d->has_accel = SDL_GamepadHasSensor(d->gp, SDL_SENSOR_ACCEL); d->has_gyro = SDL_GamepadHasSensor(d->gp, SDL_SENSOR_GYRO);
        if (d->has_accel) SDL_SetGamepadSensorEnabled(d->gp, SDL_SENSOR_ACCEL, true);
        if (d->has_gyro) SDL_SetGamepadSensorEnabled(d->gp, SDL_SENSOR_GYRO, true);
    }
    d->wireless = SDL_GetJoystickConnectionState(d->js) == SDL_JOYSTICK_CONNECTION_WIRELESS;
    d->slot = assign_slot(d);
    if (d->slot >= 0) { remember(d->guid, d->slot); apply_player_index(d); }
    say("nn::pad: connected \"%s\" (%s, %s%s) GUID %s -> %s", d->name, d->gp ? "gamepad" : "joystick WITHOUT mapping (guessed layout)", d->gp ? type_name(d->gtype) : "raw",
        d->has_gyro ? ", gyro" : "", d->guid, d->slot >= 0 ? "player " : "no free slot\n");
    if (d->slot >= 0) say("%d\n", d->slot + 1);
}
static void dev_remove(Dev *d) {
    if (d->used && !d->ignored) {
        if (d->gp) { SDL_RumbleGamepad(d->gp, 0, 0, 0); SDL_CloseGamepad(d->gp); } else if (d->js) SDL_CloseJoystick(d->js);
        say("nn::pad: disconnected \"%s\" (player %d)\n", d->name, d->slot + 1);
    }
    memset(d, 0, sizeof *d);
}
static void reassign_all(void) {                  /* [players] changed in the file */
    for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used && !g_dev[i].ignored) g_dev[i].slot = -1;
    for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used && !g_dev[i].ignored) {
        g_dev[i].slot = assign_slot(&g_dev[i]);
        if (g_dev[i].slot >= 0) { remember(g_dev[i].guid, g_dev[i].slot); apply_player_index(&g_dev[i]); }
        say("nn::pad: \"%s\" -> player %d\n", g_dev[i].name, g_dev[i].slot + 1);
    }
}
static void poll_devices(void) {                  /* lock held */
    int n = 0;
    SDL_UpdateJoysticks();
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
    SDL_JoystickID *ids = SDL_GetJoysticks(&n);
    for (int i = 0; i < MAXDEV; i++) {
        int alive = 0;
        if (!g_dev[i].used) continue;
        for (int k = 0; k < n && ids; k++) if (ids[k] == g_dev[i].id) alive = 1;
        if (!alive) dev_remove(&g_dev[i]);
    }
    for (int k = 0; k < n && ids; k++) {
        int have = 0;
        for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used && g_dev[i].id == ids[k]) have = 1;
        if (!have) dev_add(ids[k]);
    }
    SDL_free(ids);
    for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used && !g_dev[i].ignored && g_dev[i].slot < 0) {      /* a slot may have been freed */
        g_dev[i].slot = assign_slot(&g_dev[i]);
        if (g_dev[i].slot >= 0) { remember(g_dev[i].guid, g_dev[i].slot); apply_player_index(&g_dev[i]); say("nn::pad: \"%s\" -> player %d\n", g_dev[i].name, g_dev[i].slot + 1); }
    }
}
static void check_config(int force) {             /* lock held */
    ULONGLONG now = GetTickCount64();
    if (!force && now - g_last_cfg_check < 1000) return;
    g_last_cfg_check = now;
    ULONGLONG mt = file_mtime(g_cfg.path);
    if (!mt || mt == g_cfg.mtime) return;
    char before[8][40]; int bn[8];
    memcpy(before, g_cfg.pguid, sizeof before); memcpy(bn, g_cfg.pnone, sizeof bn);
    cfg_load(0);
    say("nn::pad: %s changed, reloaded\n", g_cfg.path);
    if (memcmp(before, g_cfg.pguid, sizeof before) || memcmp(bn, g_cfg.pnone, sizeof bn)) reassign_all();
}

int nnpad_init(void) {
    lock();
    if (g_inited) { unlock(); return g_sdl_ok; }
    g_inited = 1;
    g_init_ms = GetTickCount64();
    QueryPerformanceFrequency(&g_qpf);
    cfg_load(1);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");     /* the game's own focus rule decides (nnhid.c devices_live) */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_JOYSTICK | SDL_INIT_EVENTS)) { printf("nn::pad: SDL_Init failed: %s\n", SDL_GetError()); unlock(); return 0; }
    SDL_SetJoystickEventsEnabled(false); SDL_SetGamepadEventsEnabled(false);       /* state is read directly; events would only fill a queue */
    g_sdl_ok = 1;
    if (!g_only_virtual) gca_start();                    /* the GameCube adapter over WinUSB (nnpad_gca.c); its ports come in as virtual gamepads */
    poll_devices();
    say("nn::pad: SDL %d.%d.%d ready (no video); %s\n", SDL_GetVersion() / 1000000, SDL_GetVersion() / 1000 % 1000, SDL_GetVersion() % 1000, g_cfg.path);
    unlock();
    return 1;
}
void nnpad_poll(int force) {
    LARGE_INTEGER t;
    static unsigned s_gca_seen;
    if (!g_sdl_ok) return;
    QueryPerformanceCounter(&t);
    unsigned gs = gca_seq();                           /* an adapter report applied since the last refresh must not wait for the 1 ms gate */
    if (!force && (t.QuadPart - g_last_poll.QuadPart) * 1000 < g_qpf.QuadPart && gs == s_gca_seen) return;
    s_gca_seen = gs;
    lock();
    g_last_poll = t;
    poll_devices();
    check_config(force);
    unlock();
}

/* ================================================================ state */
static float src_val(const Dev *d, Src s) {
    int rawdev = s.raw || !d->gp;
    switch (s.kind) {
    case SK_BTN: return rawdev ? SDL_GetJoystickButton(d->js, s.idx) : SDL_GetGamepadButton(d->gp, (SDL_GamepadButton)s.idx);
    case SK_HAT: return (SDL_GetJoystickHat(d->js, s.idx >> 4) & (s.idx & 15)) != 0;
    case SK_AXP: case SK_AXN: case SK_AXF: {
        Sint16 r = rawdev ? SDL_GetJoystickAxis(d->js, s.idx) : SDL_GetGamepadAxis(d->gp, (SDL_GamepadAxis)s.idx);
        float v = r >= 0 ? r / 32767.0f : r / 32768.0f;
        return s.kind == SK_AXP ? (v > 0 ? v : 0) : s.kind == SK_AXN ? (v < 0 ? -v : 0) : v;
    }
    }
    return 0;
}
static Src mk(int kind, int idx, int raw) { Src s; s.kind = (unsigned char)kind; s.raw = (unsigned char)raw; s.idx = (short)idx; return s; }
static int def_map(const Dev *d, int labels, int b, Src *o) {          /* the built-in default sources of a Pro button */
    int n = 0;
    if (d->gp) {
        static const int face[4] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
        static const int pos[4] = {1, 0, 3, 2};                         /* south -> B, east -> A, west -> Y, north -> X */
        for (int i = 0; i < 4; i++) {
            int target = d->gca ? i : pos[i];                           /* the adapter's pads: south A, east B, west X, north Y (nnpad_gca.c), whatever SDL labels say */
            if (labels && !d->gca) {
                SDL_GamepadButtonLabel l = SDL_GetGamepadButtonLabel(d->gp, (SDL_GamepadButton)face[i]);
                target = l == SDL_GAMEPAD_BUTTON_LABEL_A ? 0 : l == SDL_GAMEPAD_BUTTON_LABEL_B ? 1 : l == SDL_GAMEPAD_BUTTON_LABEL_X ? 2 : l == SDL_GAMEPAD_BUTTON_LABEL_Y ? 3 : pos[i];
            }
            if (target == b) o[n++] = mk(SK_BTN, face[i], 0);
        }
        switch (b) {
        case 4: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_LEFT_STICK, 0); break;
        case 5: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_RIGHT_STICK, 0); break;
        case 6: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0); break;
        case 7: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0); break;
        case 8: if (!d->gca) o[n++] = mk(SK_AXP, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0); break;      /* a GameCube pad has no ZL: its analog triggers are reported as such */
        case 9: o[n++] = d->gca ? mk(SK_BTN, SDL_GAMEPAD_BUTTON_MISC2, 0) : mk(SK_AXP, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0); break;      /* GameCube Z acts as ZR, as on the console */
        case 10: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_START, 0); break;
        case 11: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_BACK, 0); break;
        case 12: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, 0); break;
        case 13: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_DPAD_UP, 0); break;
        case 14: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0); break;
        case 15: o[n++] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_DPAD_DOWN, 0); break;
        }
        return n;
    }
    {   /* an unmapped joystick: buttons in the usual DirectInput order, hat 0 = d-pad (a guess; --pad-map fixes it) */
        static const signed char btn[12] = {1, 0, 3, 2, 6, 7, 8, 9, 11, 10, 4, 5};      /* raw button k acts as Pro button btn[k]... inverted below */
        for (int k = 0; k < 12; k++) if (btn[k] == b) o[n++] = mk(SK_BTN, k, 1);
        if (b >= 12) o[n++] = mk(SK_HAT, b == 12 ? 8 : b == 13 ? 1 : b == 14 ? 2 : 4, 1);
        return n;
    }
}
static int stick_src(const Dev *d, int i, Src *s) {
    const DevCfg *c = dev_cfg(d), *t = type_cfg(d);
    if (c && c->has_axis[i]) { *s = c->axis[i]; return 1; }
    if (t->has_axis[i]) { *s = t->axis[i]; return 1; }
    *s = d->gp ? mk(SK_AXF, i, 0) : mk(SK_AXF, i, 1);
    if (!d->gp && i >= 2 && d->naxes >= 6) *s = mk(SK_AXF, i == 2 ? 2 : 3, 1);
    return d->gp || d->naxes > i;
}
static int stick_inv(const Dev *d, int i) { const DevCfg *c = dev_cfg(d), *t = type_cfg(d); return c && (c->has_axis[i] || c->inv[i]) ? c->inv[i] : t->inv[i]; }
/* U1: the sources of a target (0..15 Pro button, 16..23 stick direction): device section, type profile, built-in default.
 * *from = 2 device, 1 type profile, 0 built-in. A stick direction has no built-in source: the stick's axis drives it. */
static int prof_map(const Dev *d, int b, Src *s, int *from) {
    const DevCfg *c = dev_cfg(d), *t = type_cfg(d);
    if (c && c->nmap[b] >= 0) { memcpy(s, c->map[b], sizeof(Src) * 4); if (from) *from = 2; return c->nmap[b]; }
    if (t->nmap[b] >= 0) { memcpy(s, t->map[b], sizeof(Src) * 4); if (from) *from = 1; return t->nmap[b]; }
    if (from) *from = 0;
    return b < 16 ? def_map(d, eff_layout(d), b, s) : 0;
}
static int prof_set(const Dev *d, int b) { const DevCfg *c = dev_cfg(d), *t = type_cfg(d); return (c && c->nmap[b] >= 0) || t->nmap[b] >= 0; }
static void sensors(const Dev *d, NnPadState *o) {
    float a[3], g[3];
    o->accel[2] = -1.0f;
    if (!d->gp || !d->has_accel || !d->has_gyro) return;
    if (!SDL_GetGamepadSensorData(d->gp, SDL_SENSOR_ACCEL, a, 3) || !SDL_GetGamepadSensorData(d->gp, SDL_SENSOR_GYRO, g, 3)) return;
    /* SDL: x right, y up, z towards the viewer, gravity reads +z when the pad lies face up. The record at rest of nnhid.c is (0, 0, -1)
     * for a pad lying face up, so the Nintendo frame is taken as SDL's turned 180 degrees about y: (x, y, z) -> (-x, y, -z). */
    o->has_motion = 1;
    o->accel[0] = -a[0] / SDL_STANDARD_GRAVITY; o->accel[1] = a[1] / SDL_STANDARD_GRAVITY; o->accel[2] = -a[2] / SDL_STANDARD_GRAVITY;
    o->gyro[0] = -g[0] / 6.2831853f; o->gyro[1] = g[1] / 6.2831853f; o->gyro[2] = -g[2] / 6.2831853f;
}
/* U1, the settings menu (menu.cpp): the Home / Guide button and the Capture / Share button (SDL misc1) open it; while the
 * menu exists in this run they are taken out of every mapping, so the game never sees them (a controllers.ini that maps
 * them to a Pro button loses that button then). */
int menu_enabled(void);
static int menu_owns(const Dev *d, Src s) {
    static int on = -1;
    if (on < 0) on = menu_enabled();
    return on && d->gp && !s.raw && s.kind == SK_BTN && (s.idx == SDL_GAMEPAD_BUTTON_GUIDE || s.idx == SDL_GAMEPAD_BUTTON_MISC1);
}
unsigned nnpad_menu_keys(void) {                      /* bit 0: a Home / Guide button is down, bit 1: a Capture / Share button (any controller) */
    unsigned r = 0;
    if (!g_sdl_ok) return 0;
    nnpad_poll(0);
    lock();
    for (int i = 0; i < MAXDEV; i++) {
        const Dev *d = &g_dev[i];
        if (!d->used || d->ignored || !d->gp) continue;
        if (SDL_GetGamepadButton(d->gp, SDL_GAMEPAD_BUTTON_GUIDE)) r |= 1;
        if (SDL_GetGamepadButton(d->gp, SDL_GAMEPAD_BUTTON_MISC1)) r |= 2;
    }
    unlock();
    return r;
}
static void dev_state(const Dev *d, NnPadState *o) {      /* lock held */
    float dz = EFF(d, dz), outer = EFF(d, outer), trig = EFF(d, trig);
    float v[4];
    memset(o, 0, sizeof *o);
    o->wired = !d->wireless;
    for (int b = 0; b < 16; b++) {
        Src s[4]; int n = prof_map(d, b, s, NULL);
        for (int k = 0; k < n; k++) {
            if (menu_owns(d, s[k])) continue;                    /* U1: Home / Guide and Capture open the settings menu, never the game */
            float x = src_val(d, s[k]);
            int axis = s[k].kind == SK_AXP || s[k].kind == SK_AXN || s[k].kind == SK_AXF;
            float thr = axis && (b == 8 || b == 9 || (!s[k].raw && s[k].idx >= 4)) ? trig : 0.5f;
            if (x > 0 && x >= thr) o->buttons |= 1ull << b;
        }
    }
    for (int i = 0; i < 4; i++) {
        Src s;
        v[i] = stick_src(d, i, &s) ? src_val(d, s) : 0;
        if (i & 1) v[i] = -v[i];                             /* SDL: down is positive; the Npad: up is positive */
        if (stick_inv(d, i)) v[i] = -v[i];
        if (v[i] > 1) v[i] = 1; if (v[i] < -1) v[i] = -1;
        {   /* U1: a stick direction bound to its own sources (a button = full deflection, an axis half = that much); the
             * other direction of the same axis keeps the axis. Npad: x right positive, y up positive. */
            static const int dir_neg[4] = {16, 19, 20, 23}, dir_pos[4] = {18, 17, 22, 21};
            int bn = dir_neg[i], bp = dir_pos[i];
            if (prof_set(d, bn) || prof_set(d, bp)) {
                float pos = v[i] > 0 ? v[i] : 0, neg = v[i] < 0 ? -v[i] : 0;
                for (int side = 0; side < 2; side++) {
                    int tb = side ? bp : bn;
                    if (!prof_set(d, tb)) continue;
                    Src s[4]; int n = prof_map(d, tb, s, NULL);
                    float m = 0;
                    for (int k = 0; k < n; k++) { if (menu_owns(d, s[k])) continue; float x = src_val(d, s[k]); if (x < 0) x = -x; if (x > m) m = x; }
                    if (side) pos = m; else neg = m;
                }
                v[i] = pos - neg;
                if (v[i] > 1) v[i] = 1; if (v[i] < -1) v[i] = -1;
            }
        }
    }
    for (int st = 0; st < 2; st++) {
        float x = v[st * 2], y = v[st * 2 + 1], m = sqrtf(x * x + y * y), k = 0;
        int *px = st ? &o->rx : &o->lx, *py = st ? &o->ry : &o->ly;
        if (m > dz && m > 0) {
            float mm = (m - dz) / (outer > dz + 0.01f ? outer - dz : 0.01f);
            k = (mm > 1 ? 1 : mm) / m;
        }
        *px = (int)(x * k * 32767.0f + (x * k >= 0 ? 0.5f : -0.5f)); *py = (int)(y * k * 32767.0f + (y * k >= 0 ? 0.5f : -0.5f));
    }
    sensors(d, o);
    if (d->gca) {
        o->gc = 1;
        o->trig_l = (int)(src_val(d, mk(SK_AXP, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0)) * 32767.0f + 0.5f);
        o->trig_r = (int)(src_val(d, mk(SK_AXP, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0)) * 32767.0f + 0.5f);
    }
}
static Dev *dev_of(int player) { for (int i = 0; i < MAXDEV; i++) if (g_dev[i].used && !g_dev[i].ignored && g_dev[i].slot == player) return &g_dev[i]; return NULL; }
int nnpad_is_gc(int player) {
    int r;
    if (!g_sdl_ok || player < 0 || player > 7) return 0;
    lock();
    Dev *d = dev_of(player);
    r = d && d->gca;
    unlock();
    return r;
}
int nnpad_gc_native(void) { return !g_cfg.gc_pro; }

int nnpad_present(int player) {
    int r = 0;
    if (!g_sdl_ok || player < 0 || player > 7) return 0;
    nnpad_poll(0);
    lock();
    Dev *d = dev_of(player);
    if (d && d->dropped) { NnPadState s; dev_state(d, &s); if (s.buttons) d->dropped = 0; }
    r = d && !d->dropped;
    unlock();
    return r;
}
int nnpad_get(int player, NnPadState *out) {
    int r = 0;
    if (!g_sdl_ok || player < 0 || player > 7) return 0;
    nnpad_poll(0);
    lock();
    Dev *d = dev_of(player);
    if (d && !d->dropped) { dev_state(d, out); if (d->gca) gca_sample(d->id); r = 1; }
    unlock();
    return r;
}
void nnpad_disconnect(int player) {
    Dev *d;
    lock();
    if ((d = dev_of(player)) != NULL) { d->dropped = 1; if (d->gp) SDL_RumbleGamepad(d->gp, 0, 0, 0); d->rl = d->rh = 0; }
    unlock();
}
void nnpad_swap(int a, int b) {
    lock();
    Dev *x = dev_of(a), *y = dev_of(b);
    if (x) x->slot = b;
    if (y) y->slot = a;
    if (x) apply_player_index(x);
    if (y) apply_player_index(y);
    unlock();
}
void nnpad_rumble(int player, float low, float high) {
    Dev *d;
    if (!g_sdl_ok || player < 0 || player > 7) return;
    lock();
    d = dev_of(player);
    if (d && (d->gp || d->js)) {
        float s = eff_rumble(d) ? EFF(d, rstr) : 0.0f;
        float l = low * s, h = high * s;
        Uint16 rl = (Uint16)(l >= 1 ? 65535 : l <= 0 ? 0 : (int)(l * 65535.0f)), rh = (Uint16)(h >= 1 ? 65535 : h <= 0 ? 0 : (int)(h * 65535.0f));
        ULONGLONG now = GetTickCount64();
        if (rl != d->rl || rh != d->rh || ((rl || rh) && now - d->rsent > 20000)) {
            d->rl = rl; d->rh = rh; d->rsent = now;
            if (d->gp) SDL_RumbleGamepad(d->gp, rl, rh, rl || rh ? 30000 : 0); else SDL_RumbleJoystick(d->js, rl, rh, rl || rh ? 30000 : 0);
        }
    }
    unlock();
}

/* ================================================================ keyboard */
static int env_kb(void) { const char *k = getenv("SSBU_KEYBOARD_PLAYER"); return k && k[0] ? atoi(k) : -1; }   /* -1 unset; 0 none; 1..8 */
int nnpad_keyboard_player(void) {
    int r;
    if (g_kb_slot == -3 && g_sdl_ok && !getenv("SSBU_SEQ") && !getenv("SSBU_PLAY") && !getenv("SSBU_SCRIPT") && !getenv("SSBU_PRESS") && env_kb() < 0 && g_cfg.kb_player == -2) {
        /* pads on Windows can show up a moment after SDL starts: give them up to 1.5 s before the keyboard takes a slot */
        ULONGLONG el = GetTickCount64() - g_init_ms;
        if (el < 1500) Sleep((DWORD)(1500 - el));
        nnpad_poll(1);
    }
    lock();
    if (g_kb_slot == -3) {
        int e = env_kb();
        if (e >= 0) g_kb_slot = e == 0 ? -1 : e <= 8 ? e - 1 : -1;
        else if (g_cfg.kb_player == -1) g_kb_slot = -1;
        else if (g_cfg.kb_player >= 0) g_kb_slot = g_cfg.kb_player;
        else {
            g_kb_slot = -1;
            for (int p = 0; p < 8 && g_kb_slot < 0; p++) if (!slot_taken(p) && slot_usable(p, "") && !g_cfg.pguid[p][0]) g_kb_slot = p;
        }
        if (g_kb_slot >= 0) say("nn::pad: keyboard = player %d\n", g_kb_slot + 1); else say("nn::pad: keyboard = nobody\n");
    }
    r = g_kb_slot;
    unlock();
    return r;
}
/* [keyboard] movement: 1 = ssf2 (nnhid.c kb_ssf2), 0 = full; SSBU_KEYBOARD_MOVEMENT=ssf2|full overrides the file (tests) */
int nnpad_kb_movement(void) {          /* once per poll of the keyboard's player: the variable is read once */
    static int env = -2;
    if (env == -2) { const char *e = getenv("SSBU_KEYBOARD_MOVEMENT"); env = e && e[0] ? !_stricmp(e, "ssf2") : -1; }
    if (env >= 0) return env;
    nnpad_kb_vk(0);
    return g_cfg.kb_move;
}
int nnpad_kb_vk(int idx) {
    if (!g_inited) { lock(); if (!g_cfg.path[0]) { cfg_load(1); } unlock(); }
    return idx >= 0 && idx < 24 ? g_cfg.kb_vk[idx] : 0;
}

/* ================================================================ tools: --pad-test, --pad-map */
static int active_sources(const Dev *d, char out[][24], int max) {
    int n = 0;
    Src s;
    if (d->gp) {
        for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT && i < 26 && n < max; i++) if (SDL_GetGamepadButton(d->gp, (SDL_GamepadButton)i)) { s = mk(SK_BTN, i, 0); src_name(s, out[n++], 24); }
        for (int i = 0; i < 6 && n < max; i++) for (int sg = 0; sg < 2; sg++) {
            s = mk(i >= 4 ? SK_AXP : sg ? SK_AXN : SK_AXP, i, 0);
            if (i >= 4 && sg) continue;
            if (src_val(d, s) >= 0.5f && n < max) src_name(s, out[n++], 24);
        }
    } else {
        for (int i = 0; i < d->nbtn && n < max; i++) if (SDL_GetJoystickButton(d->js, i)) { s = mk(SK_BTN, i, 1); src_name(s, out[n++], 24); }
        for (int h = 0; h < d->nhats; h++) for (int k = 0; k < 4 && n < max; k++) { s = mk(SK_HAT, h << 4 | 1 << k, 1); if (src_val(d, s) > 0) src_name(s, out[n++], 24); }
        for (int i = 0; i < d->naxes; i++) for (int sg = 0; sg < 2 && n < max; sg++) { s = mk(sg ? SK_AXN : SK_AXP, i, 1); if (src_val(d, s) >= 0.6f) src_name(s, out[n++], 24); }
    }
    return n;
}
static void print_devices(void) {
    for (int i = 0; i < MAXDEV; i++) {
        const Dev *d = &g_dev[i];
        if (!d->used) continue;
        if (d->ignored) { printf("  (ignored) \"%s\" GUID %s\n", d->name, d->guid); continue; }
        printf("  player %d: \"%s\"\n    GUID %s   type %s   SDL mapping: %s   buttons %d axes %d hats %d   %s%s%s\n", d->slot + 1, d->name, d->guid,
               d->gp ? type_name(d->gtype) : "raw joystick", d->gp ? "yes" : "NO (guessed layout; use --pad-map)", d->nbtn, d->naxes, d->nhats,
               d->wireless ? "wireless" : "wired/unknown", d->has_gyro ? "   gyro+accel" : "", d->gp && SDL_GetBooleanProperty(SDL_GetGamepadProperties(d->gp), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false) ? "   rumble" : "");
    }
}
static int tool_init(void) {
    if (!nnpad_init()) return 0;
    Sleep(800); nnpad_poll(1);                             /* pads that appear a moment after SDL starts */
    return 1;
}
static int pad_test(int seconds) {
    char prev[MAXDEV][8][24] = {{{0}}};
    int pn[MAXDEV] = {0};
    int lastq[MAXDEV][4] = {{0}}, known = 0;
    if (!tool_init()) return 1;
    printf("pad-test: devices SDL sees (config %s):\n", g_cfg.path);
    print_devices();
    printf("pad-test: press buttons / move sticks; plug and unplug pads. %s\n", seconds ? "Stops by itself." : "Ctrl+C to stop.");
    ULONGLONG t0 = GetTickCount64();
    while (!seconds || GetTickCount64() - t0 < (ULONGLONG)seconds * 1000) {
        nnpad_poll(1);
        lock();
        int cnt = 0;
        for (int i = 0; i < MAXDEV; i++) cnt += g_dev[i].used;
        if (cnt != known) { known = cnt; printf("pad-test: %d device(s)\n", cnt); print_devices(); }
        for (int i = 0; i < MAXDEV; i++) {
            const Dev *d = &g_dev[i];
            char cur[8][24];
            int n, q[4];
            NnPadState s;
            if (!d->used || d->ignored) continue;
            n = active_sources(d, cur, 8);
            dev_state(d, &s);
            q[0] = s.lx / 4096; q[1] = s.ly / 4096; q[2] = s.rx / 4096; q[3] = s.ry / 4096;
            if (n != pn[i] || memcmp(cur, prev[i], (size_t)n * 24) || memcmp(q, lastq[i], sizeof q)) {
                printf("  player %d  sources:", d->slot + 1);
                for (int k = 0; k < n; k++) printf(" %s", cur[k]);
                printf("   ->  Pro buttons:");
                for (int b = 0; b < 16; b++) if (s.buttons >> b & 1) printf(" %s", g_pro[b]);
                printf("   L(%d,%d) R(%d,%d)\n", s.lx, s.ly, s.rx, s.ry);
                pn[i] = n; memcpy(prev[i], cur, sizeof cur); memcpy(lastq[i], q, sizeof q);
            }
        }
        unlock();
        Sleep(8);
    }
    return 0;
}

static void ini_replace_section(const char *path, const char *header, const char *body) {
    size_t len = 0;
    char *old = read_file(path, &len), *out = malloc(len + (body ? strlen(body) : 0) + strlen(header) + 64);
    size_t o = 0;
    int skipping = 0;
    if (!out) { free(old); return; }
    if (old) for (char *p = old; *p; ) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
        if (*p == '[') skipping = !_strnicmp(p, header, strlen(header));
        if (!skipping) { memcpy(out + o, p, l); o += l; }
        p += l;
    }
    if (o && out[o - 1] != '\n') { out[o++] = '\r'; out[o++] = '\n'; }
    if (body) o += (size_t)sprintf(out + o, "%s\r\n%s", header, body);      /* body NULL: the section is removed */
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(out, 1, o, f); fclose(f); }
    free(old); free(out);
}
static void write_device(const Dev *d, const DevCfg *c) {  /* [device GUID] with the mapping in c (map / axis / invert only; the rest is kept) */
    char head[80], body[4096], tmp[64];
    size_t o = 0;
    snprintf(head, sizeof head, "[device %s]", d->guid);
    o += (size_t)snprintf(body + o, sizeof body - o, "name = %s\r\n", d->name);
    for (int b = 0; b < 16; b++) if (c->nmap[b] >= 0) {
        o += (size_t)snprintf(body + o, sizeof body - o, "map.%s = ", g_pro[b]);
        if (!c->nmap[b]) o += (size_t)snprintf(body + o, sizeof body - o, "none");
        for (int k = 0; k < c->nmap[b]; k++) { src_name(c->map[b][k], tmp, sizeof tmp); o += (size_t)snprintf(body + o, sizeof body - o, "%s%s", k ? ", " : "", tmp); }
        o += (size_t)snprintf(body + o, sizeof body - o, "\r\n");
    }
    for (int i = 0; i < 4; i++) {
        if (c->has_axis[i]) { src_name(c->axis[i], tmp, sizeof tmp); o += (size_t)snprintf(body + o, sizeof body - o, "axis.%s = %s\r\n", g_stk[i], tmp); }
        if (c->inv[i]) o += (size_t)snprintf(body + o, sizeof body - o, "invert.%s = on\r\n", g_stk[i]);
    }
    ini_replace_section(g_cfg.path, head, body);
}
static int wait_press(Dev *d, int seconds, Src *out) {     /* waits for release of everything, then for a new source; 1 = got one */
    char cur[8][24];
    ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < (ULONGLONG)seconds * 1000) { nnpad_poll(1); lock(); int n = active_sources(d, cur, 8); unlock(); if (!n) break; Sleep(20); }
    while (GetTickCount64() - t0 < (ULONGLONG)seconds * 1000) {
        nnpad_poll(1);
        lock();
        int n = active_sources(d, cur, 8);
        unlock();
        if (n) { Src s; if (src_parse(cur[0], &s)) { *out = s; return 1; } }
        Sleep(10);
    }
    return 0;
}
static int pad_map(int player) {
    static const int order[16] = {0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 4, 5, 13, 15, 12, 14};
    static const char *const sname[4] = {"LEFT stick pushed RIGHT", "LEFT stick pushed DOWN", "RIGHT stick pushed RIGHT", "RIGHT stick pushed DOWN"};
    DevCfg c;
    Dev *d = NULL;
    cfg_init_dev(&c);
    if (!tool_init()) return 1;
    if (player < 1 || player > 8) { printf("pad-map: player must be 1..8\n"); return 2; }
    for (int w = 0; w < 100 && !d; w++) { nnpad_poll(1); lock(); d = dev_of(player - 1); unlock(); if (!d) { if (w == 0) printf("pad-map: waiting for a pad on player %d (10 s)...\n", player); Sleep(100); } }
    if (!d) { printf("pad-map: no device on player %d. Devices:\n", player); print_devices(); return 1; }
    printf("pad-map: \"%s\" GUID %s (%s). Press the button named; it is skipped (default kept) after 8 s.\n", d->name, d->guid, d->gp ? "SDL gamepad: names like south/east" : "raw joystick");
    for (int k = 0; k < 16; k++) {
        int b = order[k];
        Src s;
        printf("  press %-8s ... ", g_pro[b]); fflush(stdout);
        if (wait_press(d, 8, &s)) {
            char nm[32];
            src_name(s, nm, sizeof nm);
            /* a stick pushed far enough counts as an axis half; a trigger as the axis */
            c.map[b][0] = s; c.nmap[b] = 1;
            printf("%s\n", nm);
        } else printf("skipped\n");
    }
    for (int i = 0; i < 4; i++) {
        Src s;
        printf("  move the %s ... ", sname[i]); fflush(stdout);
        if (wait_press(d, 8, &s) && (s.kind == SK_AXP || s.kind == SK_AXN)) {
            char nm[32];
            c.axis[i] = mk(SK_AXF, s.idx, s.raw); c.has_axis[i] = 1;
            /* right-pushed is + for x; pushed down must come out negative on the Npad, the flip for y is automatic (SDL down = positive) */
            c.inv[i] = (i & 1) ? (s.kind == SK_AXN) : (s.kind == SK_AXN);
            src_name(c.axis[i], nm, sizeof nm);
            printf("%s%s\n", nm, c.inv[i] ? " (inverted)" : "");
        } else printf("skipped\n");
    }
    write_device(d, &c);
    printf("pad-map: written to %s ([device %s]); a running game picks it up within a second.\n", g_cfg.path, d->guid);
    return 0;
}
int nnpad_tool(int argc, char **argv) {
    int secs = 0;
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--seconds") && i + 1 < argc) secs = atoi(argv[i + 1]);
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--pad-map")) return pad_map(i + 1 < argc ? atoi(argv[i + 1]) : 1);
    if ((argc > 1 && !strcmp(argv[1], "--pad-test")) || getenv("SSBU_PAD_TEST")) return pad_test(secs);
    return -1;
}

/* ================================================================ self-test (SSBU_PAD_SELFTEST=1): SDL virtual joysticks, no physical pad */
static struct { Uint16 lo, hi; int calls, led_r, led_g, led_b, pidx; } g_vj[16];
static bool SDLCALL vj_rumble(void *u, Uint16 lo, Uint16 hi) { int i = (int)(intptr_t)u; g_vj[i].lo = lo; g_vj[i].hi = hi; g_vj[i].calls++; return true; }
static bool SDLCALL vj_led(void *u, Uint8 r, Uint8 g, Uint8 b) { int i = (int)(intptr_t)u; g_vj[i].led_r = r; g_vj[i].led_g = g; g_vj[i].led_b = b; return true; }
static void SDLCALL vj_pidx(void *u, int p) { g_vj[(int)(intptr_t)u].pidx = p; }
static int g_t_pass, g_t_fail;
static void check(int ok, const char *what) { printf("%s  %s\n", ok ? "PASS" : "FAIL", what); if (ok) g_t_pass++; else g_t_fail++; }
static SDL_JoystickID vj_make(int idx, int pid, int gamepad, int sensors_on) {
    static const SDL_VirtualJoystickSensorDesc sd[2] = {{SDL_SENSOR_ACCEL, 100.0f}, {SDL_SENSOR_GYRO, 100.0f}};
    SDL_VirtualJoystickDesc d;
    static char names[16][40];
    SDL_INIT_INTERFACE(&d);
    snprintf(names[idx], sizeof names[0], "Selftest %s %d", gamepad ? "pad" : "raw stick", idx);
    d.type = gamepad ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
    d.vendor_id = 0x1209; d.product_id = (Uint16)pid;
    d.naxes = gamepad ? SDL_GAMEPAD_AXIS_COUNT : 6; d.nbuttons = gamepad ? SDL_GAMEPAD_BUTTON_COUNT : 12; d.nhats = gamepad ? 0 : 1;
    d.name = names[idx]; d.userdata = (void *)(intptr_t)idx;
    d.Rumble = vj_rumble; d.SetLED = vj_led; d.SetPlayerIndex = vj_pidx;
    if (sensors_on) { d.nsensors = 2; d.sensors = sd; }
    return SDL_AttachVirtualJoystick(&d);
}
static void settle(void) { Sleep(3); nnpad_poll(1); }
static void vj_clear(SDL_Joystick *j, int n_btn, int n_ax) { for (int i = 0; i < n_btn; i++) SDL_SetJoystickVirtualButton(j, i, false); for (int i = 0; i < n_ax; i++) SDL_SetJoystickVirtualAxis(j, i, n_ax == 6 && (i == 4 || i == 5) ? -32768 : 0); }   /* a virtual trigger rests at the bottom of its raw range */
static void write_text(const char *path, const char *text) { FILE *f = fopen(path, "wb"); if (f) { fputs(text, f); fclose(f); } }
static int near_(int a, int b, int tol) { return abs(a - b) <= tol; }

int nnpad_selftest(const NnHidHooks *h) {
    char path[MAX_PATH], guid0[40];
    SDL_JoystickID id[10];
    SDL_Joystick *js[10];
    uint64_t btn; int32_t st[4]; uint32_t attr;
    GetTempPathA(MAX_PATH, path); strcat(path, "ssbu_pad_selftest\\controllers.ini");
    make_dirs(path);
    DeleteFileA(path);
    _putenv_s("SSBU_CONFIG", path);
    _putenv_s("SSBU_KEYBOARD_PLAYER", "0");
    _putenv_s("SSBU_PAD_LAYOUT", "");
    g_only_virtual = 1;
    h->test_mode(1);
    printf("pad self-test (SDL virtual joysticks, no physical pad):\n");
    check(nnpad_init() != 0, "SDL started with gamepad/joystick/events only (no video)");
    check(!SDL_WasInit(SDL_INIT_VIDEO), "SDL video subsystem NOT initialised: no window, no window messages");
    check(file_mtime(path) != 0, "controllers.ini written with defaults on first run");
    { size_t n = 0; char *t = read_file(path, &n); check(t && strstr(t, "[keyboard]") && strstr(t, "layout = auto") && strstr(t, "--pad-map"), "default file is documented (sections, layout, tool hints)"); free(t); }
    check(h->style(0) == 0, "no device: style set 0");

    /* ---- detection and slot of one pad */
    id[0] = vj_make(0, 0x100, 1, 1);
    check(id[0] != 0, "virtual gamepad attached");
    settle();
    js[0] = SDL_OpenJoystick(id[0]);
    check(SDL_IsGamepad(id[0]), "SDL has a mapping for the virtual gamepad (opened as GAMEPAD)");
    check(h->style(0) == 1 && nnpad_present(0), "pad appears as player 1 (style FullKey) after attach");
    h->state(0, &btn, st, &attr);
    check(attr == 3 && btn == 0 && st[0] == 0 && st[1] == 0, "connected state: attribute 3 (connected, wired), all zero");
    check(g_vj[0].pidx == 0 && g_vj[0].led_b == 255, "player index and LED colour set on the pad (player 1)");
    check(h->led(0) == 1, "player LED pattern for player 1 reported");
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(id[0]), guid0, sizeof guid0);

    /* ---- hot-plug out and in */
    SDL_CloseJoystick(js[0]);
    SDL_DetachVirtualJoystick(id[0]);
    settle();
    h->state(0, &btn, st, &attr);
    check(h->style(0) == 0 && attr == 0 && !nnpad_present(0), "unplug: style 0, state all zero, player 1 gone");
    id[0] = vj_make(0, 0x100, 1, 1);
    settle();
    js[0] = SDL_OpenJoystick(id[0]);
    check(h->style(0) == 1, "replug: back in its old slot (player 1)");

    /* ---- a port of the GameCube adapter (nnpad_gca.c makes these virtual gamepads): shown to the game as a GameCube controller */
    {
        SDL_VirtualJoystickDesc d;
        SDL_JoystickID gid;
        SDL_Joystick *gj;
        uint32_t trig[2], gattr;
        uint64_t gb;
        int p = -1;
        SDL_INIT_INTERFACE(&d);
        d.type = SDL_JOYSTICK_TYPE_GAMEPAD; d.vendor_id = 0x057E; d.product_id = 0x0337;
        d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        d.name = "GameCube Controller (port 3)"; d.userdata = (void *)(intptr_t)13; d.Rumble = vj_rumble;
        gid = SDL_AttachVirtualJoystick(&d);
        settle();
        gj = SDL_OpenJoystick(gid);
        for (int i = 0; i < 8; i++) if (nnpad_is_gc(i)) p = i;
        check(gid != 0 && p == 2, "GameCube adapter: port 3 takes player 3 and is known as a GameCube controller");
        if (p >= 0) {
            check(h->style((uint32_t)p) == 0x20 && nnpad_gc_native(), "GameCube adapter: the game gets style Gc (bit 5), the Pro Controller's style stays for the other pads");
            vj_clear(gj, SDL_GAMEPAD_BUTTON_COUNT, 6);
            SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_SOUTH, true); settle(); h->gc_state((uint32_t)p, &gb, trig, &gattr);
            int a = gb == 1ull << 0 && gattr == 3;
            SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_SOUTH, false); SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_EAST, true); settle(); h->gc_state((uint32_t)p, &gb, trig, &gattr);
            int b = gb == 1ull << 1;
            SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_EAST, false); SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_WEST, true); settle(); h->gc_state((uint32_t)p, &gb, trig, &gattr);
            int x = gb == 1ull << 2;
            SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_WEST, false); SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_NORTH, true); settle(); h->gc_state((uint32_t)p, &gb, trig, &gattr);
            int y = gb == 1ull << 3;
            SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_NORTH, false); SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_MISC2, true); settle(); h->gc_state((uint32_t)p, &gb, trig, &gattr);
            int z = gb == 1ull << 9;
            SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_MISC2, false); SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true); SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true); settle(); h->gc_state((uint32_t)p, &gb, trig, &gattr);
            int lr = gb == (1ull << 6 | 1ull << 7);
            if (!(a && b && x && y && z && lr)) printf("    a=%d b=%d x=%d y=%d z=%d lr=%d buttons=%llx attr=%u\n", a, b, x, y, z, lr, (unsigned long long)gb, gattr);
            check(a && b && x && y && z && lr, "GameCube adapter: A B X Y = Pro A B X Y, Z = ZR (as on the console), L / R clicks = L / R");
            SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, false); SDL_SetJoystickVirtualButton(gj, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
            settle(); h->gc_state((uint32_t)p, &gb, trig, &gattr);
            int rest = trig[0] == 0 && trig[1] == 0 && gb == 0;
            SDL_SetJoystickVirtualAxis(gj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767); SDL_SetJoystickVirtualAxis(gj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0); settle(); h->gc_state((uint32_t)p, &gb, trig, &gattr);
            int full = near_((int)trig[0], 32767, 2) && near_((int)trig[1], 16384, 2) && !(gb >> 8 & 3);
            if (!(rest && full)) printf("    rest=%d full=%d triggers %u %u buttons %llx\n", rest, full, trig[0], trig[1], (unsigned long long)gb);
            check(rest && full, "GameCube adapter: analog triggers 0..0x7fff in the state (full / half / rest), no ZL / ZR button from them");
            SDL_SetJoystickVirtualAxis(gj, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768); SDL_SetJoystickVirtualAxis(gj, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
            h->gc_erm((uint32_t)p, 1);
            check(g_vj[13].lo == 65535 && g_vj[13].hi == 65535, "GameCube adapter: rumble command Start reaches the controller's motor");
            h->gc_erm((uint32_t)p, 0);
            check(g_vj[13].lo == 0 && g_vj[13].hi == 0, "GameCube adapter: rumble command Stop stops it");
            Sleep(1100); write_text(path, "[general]\r\ngamecube_style = pro\r\n"); Sleep(1100); nnpad_poll(1);
            check(h->style((uint32_t)p) == 1 && !nnpad_gc_native(), "GameCube adapter: gamecube_style = pro shows it to the game as a Pro Controller");
            Sleep(1100); write_text(path, "[general]\r\n"); Sleep(1100); nnpad_poll(1);
            check(h->style((uint32_t)p) == 0x20 && nnpad_gc_native(), "GameCube adapter: gamecube_style back to native");
        }
        SDL_CloseJoystick(gj);
        SDL_DetachVirtualJoystick(gid);
        settle();
        check(!nnpad_is_gc(2) && h->style(2) == 0, "GameCube adapter: unplugged port: player 3 is gone");
    }

    /* ---- buttons, positional layout */
    {
        static const struct { int sdl, pro; const char *n; } m[] = {
            {SDL_GAMEPAD_BUTTON_EAST, 0, "east -> A"}, {SDL_GAMEPAD_BUTTON_SOUTH, 1, "south -> B"}, {SDL_GAMEPAD_BUTTON_NORTH, 2, "north -> X"}, {SDL_GAMEPAD_BUTTON_WEST, 3, "west -> Y"},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, 4, "left stick click"}, {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 5, "right stick click"},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 6, "L"}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 7, "R"}, {SDL_GAMEPAD_BUTTON_START, 10, "start -> Plus"},
            {SDL_GAMEPAD_BUTTON_BACK, 11, "back -> Minus"}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, 12, "d-pad left"}, {SDL_GAMEPAD_BUTTON_DPAD_UP, 13, "d-pad up"},
            {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 14, "d-pad right"}, {SDL_GAMEPAD_BUTTON_DPAD_DOWN, 15, "d-pad down"},
        };
        int ok = 1;
        for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
            vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
            SDL_SetJoystickVirtualButton(js[0], m[i].sdl, true);
            settle();
            h->state(0, &btn, st, &attr);
            if (btn != 1ull << m[i].pro) { ok = 0; printf("    %s: got %llx expected bit %d\n", m[i].n, (unsigned long long)btn, m[i].pro); }
        }
        check(ok, "14 buttons one by one, positional layout: each reaches exactly its Pro Controller bit");
        vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
        SDL_SetJoystickVirtualButton(js[0], SDL_GAMEPAD_BUTTON_GUIDE, true); settle(); h->state(0, &btn, st, &attr);
        check(btn == 0, "guide/home is not mapped (no Pro bit)");
        vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
    }

    /* ---- axes */
    {
        int ok;
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTX, 32767); settle(); h->state(0, &btn, st, &attr);
        check(st[0] == 32767 && st[1] == 0, "left stick full right: lx = +32767");
        check((btn >> 18 & 1) == 1 && (btn >> 16 & 1) == 0, "stick-as-button bit for left-stick right (bit 18) set");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTX, 0);
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTY, -32768); settle(); h->state(0, &btn, st, &attr);
        check(st[1] == 32767 && st[0] == 0, "left stick full UP (SDL y = -32768): ly = +32767 (up positive)");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTY, 32767); settle(); h->state(0, &btn, st, &attr);
        if (st[1] != -32767) printf("    ly=%d lx=%d\n", st[1], st[0]);
        check(st[1] == -32767, "left stick full down: ly = -32767");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTY, 0);
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_RIGHTX, -32768); SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_RIGHTY, -32768); settle(); h->state(0, &btn, st, &attr);
        ok = st[2] < 0 && st[3] > 0 && st[2] >= -32767 && st[3] <= 32767 && (int)sqrt((double)st[2] * st[2] + (double)st[3] * st[3]) <= 32768;
        check(ok, "right stick into the square corner: x<0, y>0 and the length is limited to the unit circle (circular gate)");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_RIGHTX, 0); SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_RIGHTY, 0);
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTX, 2500); settle(); h->state(0, &btn, st, &attr);     /* 0.076 < dead zone 0.10 */
        check(st[0] == 0, "stick inside the dead zone (0.076 of travel) reads 0");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTX, 16384); settle(); h->state(0, &btn, st, &attr);
        check(st[0] > 12000 && st[0] < 20000, "half deflection rescaled past the dead zone: 0.5 -> ~0.45..0.6 of range");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTX, 0);
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 10000 * 2 - 32768); settle(); h->state(0, &btn, st, &attr);
        check((btn >> 8 & 1) == 0, "left trigger at 0.305 (below the 0.33 threshold): no ZL");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 11000 * 2 - 32768); settle(); h->state(0, &btn, st, &attr);
        check((btn >> 8 & 1) == 1 && (btn >> 9 & 1) == 0, "left trigger at 0.336: ZL (analog trigger -> digital)");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767); settle(); h->state(0, &btn, st, &attr);
        check(btn == 1ull << 9, "right trigger full: exactly ZR");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768); settle();
        (void)ok;
    }

    /* ---- rumble, six-axis */
    {
        h->vibrate(0, 0.5f, 0.25f);
        check(g_vj[0].calls > 0 && near_(g_vj[0].lo, 32767, 2) && near_(g_vj[0].hi, 16383, 2), "rumble: the game's low band 0.5 / high band 0.25 reach the pad as 32767 / 16383");
        h->vibrate(0, 0, 0);
        check(g_vj[0].lo == 0 && g_vj[0].hi == 0, "rumble stop reaches the pad");
        float a[3] = {9, 9, 9}, g[3] = {9, 9, 9}; int rest = 0;
        float acc[3] = {0, 0, SDL_STANDARD_GRAVITY}, gy[3] = {0, 0, 0};
        SDL_SendJoystickVirtualSensorData(js[0], SDL_SENSOR_ACCEL, SDL_GetTicksNS(), acc, 3);
        SDL_SendJoystickVirtualSensorData(js[0], SDL_SENSOR_GYRO, SDL_GetTicksNS(), gy, 3);
        settle();
        h->six(0, a, g, &rest);
        check(fabsf(a[0]) < 0.01f && fabsf(a[1]) < 0.01f && fabsf(a[2] + 1.0f) < 0.01f && fabsf(g[0]) + fabsf(g[1]) + fabsf(g[2]) < 0.01f && rest, "six-axis from a pad with sensors, lying still: accel (0,0,-1) G, gyro 0, at rest");
        gy[2] = 6.2831853f;
        SDL_SendJoystickVirtualSensorData(js[0], SDL_SENSOR_GYRO, SDL_GetTicksNS(), gy, 3);
        settle();
        h->six(0, a, g, &rest);
        check(fabsf(fabsf(g[2]) - 1.0f) < 0.01f && !rest, "six-axis: a turn of 2*pi rad/s arrives as 1 revolution/s, not at rest");
    }
    h->vibrate(0, 0, 0);

    /* ---- 8 pads, slot assignment */
    {
        int ok = 1;
        for (int i = 1; i < 8; i++) { id[i] = vj_make(i, 0x100 + i, 1, 0); settle(); js[i] = SDL_OpenJoystick(id[i]); }
        for (int i = 0; i < 8; i++) if (h->style(i) != 1) ok = 0;
        check(ok, "8 pads attached one after another: players 1..8 all present");
        ok = 1;
        for (int i = 0; i < 8; i++) {
            vj_clear(js[i], SDL_GAMEPAD_BUTTON_COUNT, 6);
            SDL_SetJoystickVirtualButton(js[i], SDL_GAMEPAD_BUTTON_EAST, true);
            settle();
            for (int p = 0; p < 8; p++) { h->state(p, &btn, st, &attr); if ((btn & 1) != (p == i ? 1u : 0u)) ok = 0; }
            SDL_SetJoystickVirtualButton(js[i], SDL_GAMEPAD_BUTTON_EAST, false);
        }
        check(ok, "pad k (attach order) drives exactly player k, no crosstalk between the 8");
        ok = g_vj[7].pidx == 7;
        check(ok, "player index 7 set on the 8th pad (LED/lightbar slot)");
        SDL_CloseJoystick(js[2]); SDL_DetachVirtualJoystick(id[2]); js[2] = NULL; id[2] = 0; settle();
        check(h->style(2) == 0 && h->style(1) == 1 && h->style(3) == 1, "pad 3 unplugged: only player 3 disappears, the others keep their slots");
        SDL_CloseJoystick(js[5]); SDL_DetachVirtualJoystick(id[5]); settle();
        id[8] = vj_make(8, 0x200, 1, 0); settle(); js[8] = SDL_OpenJoystick(id[8]);
        check(h->style(2) == 1 && h->style(5) == 0, "a NEW pad takes the first free slot (player 3) while player 6 is empty");
        SDL_CloseJoystick(js[8]); SDL_DetachVirtualJoystick(id[8]); settle();
        id[5] = vj_make(5, 0x105, 1, 0); settle(); js[5] = SDL_OpenJoystick(id[5]);
        vj_clear(js[5], SDL_GAMEPAD_BUTTON_COUNT, 6); SDL_SetJoystickVirtualButton(js[5], SDL_GAMEPAD_BUTTON_EAST, true); settle();
        h->state(5, &btn, st, &attr); ok = btn & 1; h->state(2, &btn, st, &attr); ok = ok && !(btn & 1) && h->style(2) == 0;
        SDL_SetJoystickVirtualButton(js[5], SDL_GAMEPAD_BUTTON_EAST, false);
        check(ok, "pad 6 replugged while player 3 is also free: it returns to ITS slot (player 6), not the first free one");
    }

    /* ---- remap from the ini, layout labels, hot reload */
    {
        char text[4096], g5[40];
        SDL_GUIDToString(SDL_GetJoystickGUIDForID(id[0]), g5, sizeof g5);
        vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
        SDL_SetJoystickVirtualButton(js[0], SDL_GAMEPAD_BUTTON_SOUTH, true); settle(); h->state(0, &btn, st, &attr);
        int before = btn == 1ull << 1;
        snprintf(text, sizeof text,
                 "[general]\r\nlayout = positional\r\ndeadzone = 0.10\r\n[keyboard]\r\nplayer = none\r\n"
                 "[device %s]\r\nlayout = labels\r\nmap.PLUS = north, button14\r\nmap.MINUS = none\r\nmap.Y = none\r\naxis.RX = leftx\r\ninvert.LY = on\r\nrumble = off\r\n", g5);
        Sleep(1100);
        write_text(path, text);
        Sleep(1100); nnpad_poll(1);
        h->state(0, &btn, st, &attr);
        check(before && btn == 1ull << 0, "ini edited while running: reloaded within ~1 s; layout=labels turns south (printed A on this pad) into Pro A");
        vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
        SDL_SetJoystickVirtualButton(js[0], SDL_GAMEPAD_BUTTON_NORTH, true); settle(); h->state(0, &btn, st, &attr);
        check(btn == 1ull << 10, "remap: north -> Plus (map.PLUS); X no longer gets north");
        vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
        SDL_SetJoystickVirtualButton(js[0], SDL_GAMEPAD_BUTTON_BACK, true); settle(); h->state(0, &btn, st, &attr);
        check(btn == 0, "remap: map.MINUS = none removes the button");
        vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTX, 32767); settle(); h->state(0, &btn, st, &attr);
        check(st[2] == 32767, "remap: axis.RX = leftx (left stick x drives the Pro right-stick x)");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTX, 0);
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTY, -32768); settle(); h->state(0, &btn, st, &attr);
        check(st[1] == -32767, "remap: invert.LY = on flips the up/down axis");
        SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_LEFTY, 0);
        h->vibrate(0, 1.0f, 1.0f);
        check(g_vj[0].lo == 0 && g_vj[0].hi == 0, "rumble = off for this device: nothing reaches the pad");
        /* writer used by --pad-map: write a mapping, reload, effect */
        {
            Dev dd; DevCfg c;
            memset(&dd, 0, sizeof dd); snprintf(dd.guid, sizeof dd.guid, "%s", g5); snprintf(dd.name, sizeof dd.name, "Selftest pad 0");
            cfg_init_dev(&c);
            c.map[3][0] = mk(SK_BTN, SDL_GAMEPAD_BUTTON_EAST, 0); c.map[3][1] = mk(SK_AXP, 3, 1); c.nmap[3] = 2; c.nmap[0] = 0;      /* Y <- east, axis3+ */
            c.axis[0] = mk(SK_AXF, SDL_GAMEPAD_AXIS_RIGHTX, 0); c.has_axis[0] = 1; c.inv[0] = 1;
            Sleep(1100);
            write_device(&dd, &c);
            Sleep(1100); nnpad_poll(1);
            vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
            SDL_SetJoystickVirtualButton(js[0], SDL_GAMEPAD_BUTTON_EAST, true);
            SDL_SetJoystickVirtualAxis(js[0], SDL_GAMEPAD_AXIS_RIGHTX, 32767); settle(); h->state(0, &btn, st, &attr);
            check((btn & 0xffff) == (1ull << 3) && st[0] == -32767, "--pad-map writer round trip: [device] section written, reloaded; map.Y = east, axis.LX = rightx inverted");
        }
        /* source detection used by --pad-map */
        {
            char names[8][24]; int n;
            vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
            SDL_SetJoystickVirtualButton(js[0], SDL_GAMEPAD_BUTTON_WEST, true); settle();
            lock(); n = active_sources(dev_of(0), names, 8); unlock();
            check(n == 1 && !strcmp(names[0], "west"), "--pad-map / --pad-test source detection names the pressed button (west)");
            vj_clear(js[0], SDL_GAMEPAD_BUTTON_COUNT, 6);
        }
        /* [players]: a GUID pins the device to a slot, none removes a slot */
        {
            char g3[40];
            SDL_GUIDToString(SDL_GetJoystickGUIDForID(id[3]), g3, sizeof g3);
            snprintf(text, sizeof text, "[keyboard]\r\nplayer = none\r\n[players]\r\nplayer1 = %s\r\nplayer2 = none\r\n", g3);
            Sleep(1100); write_text(path, text); Sleep(1100); nnpad_poll(1);
            vj_clear(js[3], SDL_GAMEPAD_BUTTON_COUNT, 6);
            SDL_SetJoystickVirtualButton(js[3], SDL_GAMEPAD_BUTTON_EAST, true); settle(); h->state(0, &btn, st, &attr);
            check((btn & 1) == 1 && h->style(1) == 0, "[players]: player1 = <GUID of pad 4> puts that pad on player 1; player2 = none leaves slot 2 empty");
            SDL_SetJoystickVirtualButton(js[3], SDL_GAMEPAD_BUTTON_EAST, false);
        }
    }

    /* ---- U1: the Controls page API (type profile, device section over it, a stick direction from a button, capture, reset) */
    {
        NnPadDev l[16];
        int n = nnpad_list(l, 16), ix = -1, k = -1, from = -1, ok;
        char t[64];
        for (int i = 0; i < n; i++) if (l[i].player == 0) ix = l[i].index;
        lock(); Dev *d0 = dev_of(0); SDL_JoystickID want = d0 ? d0->id : 0; int ty = d0 ? dev_type(d0) : -1; unlock();
        for (int i = 0; i < 10; i++) if (id[i] && id[i] == want) k = i;
        check(ix >= 0 && k >= 0, "controls: the device list names player 1's pad");
        if (ix >= 0 && k >= 0) {
            SDL_Joystick *j = js[k];
            nnpad_target_set(ix, 1, 0, "north");                         /* A <- north for every pad of this kind */
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_NORTH, true); settle(); h->state(0, &btn, st, &attr);
            from = nnpad_target_text(ix, 0, t, sizeof t);
            { char *f = read_file(path, NULL); char hd[48]; snprintf(hd, sizeof hd, "[type %s]", g_type_key[ty]); ok = f && strstr(f, hd) && strstr(f, "map.A = north"); free(f); }
            check((btn & 1) && from == 1 && !strcmp(t, "north") && ok, "controls: a [type ...] profile binding (A = north) is used at once and written");
            nnpad_target_set(ix, 0, 0, "west");                          /* this controller only: A <- west wins over the profile */
            h->state(0, &btn, st, &attr); int north_a = (int)(btn & 1);
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_WEST, true); settle(); h->state(0, &btn, st, &attr);
            from = nnpad_target_text(ix, 0, t, sizeof t);
            check(!north_a && (btn & 1) && from == 2, "controls: the [device] binding wins over the type profile");
            nnpad_target_set(ix, 0, 16, "south");                        /* left stick LEFT <- south (a button = full deflection) */
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true); settle(); h->state(0, &btn, st, &attr);
            int left = st[0];
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, 32767); settle(); h->state(0, &btn, st, &attr);
            check(left == -32767 && st[0] == 32767, "controls: a stick direction bound to a button (left stick left = south); the other direction keeps the axis");
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); settle();
            nnpad_capture_begin(ix);
            int c0 = nnpad_capture_poll(t, sizeof t);
            SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, true); settle();
            int c1 = nnpad_capture_poll(t, sizeof t);
            ok = c0 == 0 && c1 == 1 && !strcmp(t, "east");
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); settle(); nnpad_capture_begin(ix); nnpad_capture_poll(t, sizeof t);
            SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTY, -32768); settle();
            int c2 = nnpad_capture_poll(t, sizeof t);
            ok = ok && c2 == 1 && !strcmp(t, "righty-");
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); settle(); nnpad_capture_begin(ix); nnpad_capture_poll(t, sizeof t);
            SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_GUIDE, true); settle();
            int c3 = nnpad_capture_poll(t, sizeof t);
            nnpad_capture_end();
            check(ok && c3 == 2, "controls: capture waits for release, then names the button (east) or stick direction (righty-); Guide cancels");
            nnpad_tune_set(ix, 1, 0, 0.5f);
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHTX, 13000); settle(); h->state(0, &btn, st, &attr);
            int rx_dz = st[2];
            check(rx_dz == 0, "controls: dead zone 0.5 in the profile: a stick at 0.4 gives nothing");
            nnpad_profile_reset(ix, 0); nnpad_profile_reset(ix, 1);
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, true); settle(); h->state(0, &btn, st, &attr);
            from = nnpad_target_text(ix, 0, t, sizeof t);
            { char *f = read_file(path, NULL); char hd[48]; snprintf(hd, sizeof hd, "[type %s]", g_type_key[ty]); ok = f && !strstr(f, hd); free(f); }
            check((btn & 1) && from == 0 && ok, "controls: reset of both levels: the built-in default again (east = A), the [type] section removed");
            vj_clear(j, SDL_GAMEPAD_BUTTON_COUNT, 6); settle();
        }
    }

    /* ---- a joystick SDL has no mapping for */
    {
        SDL_JoystickID fill = vj_make(12, 0x600, 1, 0), rid;
        SDL_Joystick *rj, *fj;
        int p = -1, raw_has = 0;
        Sleep(1100); write_text(path, "[keyboard]\r\nplayer = none\r\n"); Sleep(1100); nnpad_poll(1);       /* back to the default player assignment */
        settle();
        fj = SDL_OpenJoystick(fill);                         /* fills the last free slot: 8 pads */
        rid = vj_make(10, 0x400, 0, 0);
        settle();
        rj = SDL_OpenJoystick(rid);
        check(rid != 0 && !SDL_IsGamepad(rid), "a virtual joystick of unknown type has NO gamepad mapping");
        for (int i = 0; i < 8; i++) { lock(); Dev *d = dev_of(i); if (d && !d->gp) raw_has = 1; unlock(); }
        check(raw_has == 0, "8 slots full: a 9th device gets no slot and does not disturb the others");
        SDL_CloseJoystick(js[7]); SDL_DetachVirtualJoystick(id[7]); js[7] = NULL; id[7] = 0; settle();
        for (int i = 0; i < 8; i++) { lock(); Dev *d = dev_of(i); int raw = d && !d->gp; unlock(); if (raw) p = i; }
        check(p >= 0, "the unmapped joystick is opened as a plain joystick and gets the free slot");
        if (p >= 0) {
            SDL_SetJoystickVirtualButton(rj, 1, true); settle(); h->state((uint32_t)p, &btn, st, &attr);
            int a = btn == 1ull << 0;
            SDL_SetJoystickVirtualButton(rj, 1, false); SDL_SetJoystickVirtualButton(rj, 0, true); settle(); h->state((uint32_t)p, &btn, st, &attr);
            int b = btn == 1ull << 1;
            SDL_SetJoystickVirtualButton(rj, 0, false); SDL_SetJoystickVirtualHat(rj, 0, SDL_HAT_UP); settle(); h->state((uint32_t)p, &btn, st, &attr);
            int u = btn == 1ull << 13;
            SDL_SetJoystickVirtualHat(rj, 0, SDL_HAT_CENTERED); SDL_SetJoystickVirtualAxis(rj, 0, 32767); settle(); h->state((uint32_t)p, &btn, st, &attr); int sx = st[0]; SDL_SetJoystickVirtualAxis(rj, 0, 0); SDL_SetJoystickVirtualAxis(rj, 1, -32768); settle(); h->state((uint32_t)p, &btn, st, &attr);
            int s = sx == 32767 && st[0] == 0 && st[1] == 32767;
            if (!(a && b && u && s)) printf("    a=%d b=%d u=%d s=%d btn=%llx stick %d %d\n", a, b, u, s, (unsigned long long)btn, st[0], st[1]);
            check(a && b && u && s, "fallback default for an unmapped joystick: button1 = A, button0 = B, hat up = d-pad up, axes 0/1 = left stick (y inverted)");
            SDL_SetJoystickVirtualAxis(rj, 0, 0); SDL_SetJoystickVirtualAxis(rj, 1, 0);
        }
        SDL_CloseJoystick(rj); SDL_DetachVirtualJoystick(rid);
    }

    /* ---- [keyboard] movement = ssf2 (nnhid.c kb_ssf2): read from the file, and kept when the settings menu writes the section */
    {
        Sleep(1100); write_text(path, "[keyboard]\r\nplayer = none\r\nmovement = ssf2\r\n"); Sleep(1100); nnpad_poll(1);
        int on = nnpad_kb_movement();
        nnpad_kb_bind(0, nnpad_kb_vk(0));                    /* what the Controls page does: the whole [keyboard] section written again */
        size_t n = 0; char *t = read_file(path, &n);
        check(on == 1 && t && strstr(t, "movement = ssf2"), "[keyboard] movement = ssf2 is read, and kept when the settings menu writes the section");
        free(t);
        Sleep(1100); write_text(path, "[keyboard]\r\nplayer = none\r\n"); Sleep(1100); nnpad_poll(1);
        check(nnpad_kb_movement() == 0, "[keyboard] without a movement line: full (a stick key pushes fully)");
    }

    /* ---- clean up */
    for (int i = 0; i < 8; i++) { if (id[i] && js[i]) { SDL_CloseJoystick(js[i]); SDL_DetachVirtualJoystick(id[i]); } }
    settle();
    check(h->style(0) == 0 && h->style(7) == 0, "all pads detached: no player present");
    h->test_mode(0);
    printf("pad self-test: %d passed, %d failed\n", g_t_pass, g_t_fail);
    printf("PAD SELFTEST %s (%d/%d)\n", g_t_fail ? "FAILED" : "PASSED", g_t_pass, g_t_pass + g_t_fail);
    return g_t_fail;
}

/* ================================================================ U1, the settings menu (menu.cpp)
 * The keyboard bindings: shown, and changed by writing the [keyboard] section of controllers.ini (the file is read again
 * within a second, check_config: a new key works at once; `player` is fixed once resolved and needs a restart). */
const char *nnpad_kb_label(int idx) { return idx >= 0 && idx < 24 ? g_kbn[idx] : ""; }
void nnpad_vk_name(int vk, char *o, int n) {         /* the spelling parse_vk reads back */
    if (!vk) { snprintf(o, (size_t)n, "none"); return; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { snprintf(o, (size_t)n, "%c", vk); return; }
    switch (vk) {
    case VK_LEFT: snprintf(o, (size_t)n, "LEFT"); return; case VK_UP: snprintf(o, (size_t)n, "UP"); return;
    case VK_RIGHT: snprintf(o, (size_t)n, "RIGHT"); return; case VK_DOWN: snprintf(o, (size_t)n, "DOWN"); return;
    case VK_RETURN: snprintf(o, (size_t)n, "ENTER"); return; case VK_BACK: snprintf(o, (size_t)n, "BACKSPACE"); return;
    case VK_SPACE: snprintf(o, (size_t)n, "SPACE"); return; case VK_TAB: snprintf(o, (size_t)n, "TAB"); return;
    case VK_ESCAPE: snprintf(o, (size_t)n, "ESC"); return; case VK_SHIFT: snprintf(o, (size_t)n, "SHIFT"); return;
    case VK_CONTROL: snprintf(o, (size_t)n, "CTRL"); return; case VK_MENU: snprintf(o, (size_t)n, "ALT"); return;
    }
    if (vk >= VK_F1 && vk <= VK_F12) { snprintf(o, (size_t)n, "F%d", vk - VK_F1 + 1); return; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(o, (size_t)n, "NUM%d", vk - VK_NUMPAD0); return; }
    snprintf(o, (size_t)n, "0x%02X", vk);
}
int nnpad_kb_player_cfg(void) { nnpad_kb_vk(0); return g_cfg.kb_player; }      /* -2 auto, -1 none, 0..7 (the file's value) */
static void kb_write(void) {                          /* lock held */
    char body[2048], nm[16];
    int o = snprintf(body, sizeof body, "; written by the settings menu (F1); key names: a letter or digit, LEFT UP RIGHT DOWN ENTER BACKSPACE SPACE TAB ESC SHIFT CTRL ALT F1..F12 NUM0..NUM9, 0x.. or none\r\n");
    o += snprintf(body + o, sizeof body - (size_t)o, "player = %s\r\n", g_cfg.kb_player == -2 ? "auto" : g_cfg.kb_player == -1 ? "none" : (snprintf(nm, sizeof nm, "%d", g_cfg.kb_player + 1), nm));
    for (int i = 0; i < 24 && o < (int)sizeof body - 64; i++) { nnpad_vk_name(g_cfg.kb_vk[i], nm, sizeof nm); o += snprintf(body + o, sizeof body - (size_t)o, "%s = %s\r\n", g_kbn[i], nm); }
    o += snprintf(body + o, sizeof body - (size_t)o, "movement = %s\r\n", g_cfg.kb_move ? "ssf2" : "full");      /* kept: the menu does not show it */
    ini_replace_section(g_cfg.path, "[keyboard]", body);
    g_cfg.mtime = file_mtime(g_cfg.path);             /* what is in memory is what was written: no reload needed */
    say("nn::pad: [keyboard] of %s written (settings menu)\n", g_cfg.path);
}
void nnpad_kb_bind(int idx, int vk) {
    if (idx < 0 || idx >= 24) return;
    nnpad_kb_vk(0);
    lock();
    g_cfg.kb_vk[idx] = vk;
    kb_write();
    unlock();
}
void nnpad_kb_player_set(int p) {                     /* -2 auto, -1 none, 0..7: written, and used at once (nnhid_kb_player_refresh) */
    nnpad_kb_vk(0);
    lock();
    g_cfg.kb_player = p < -2 ? -2 : p > 7 ? 7 : p;
    kb_write();
    if (g_kb_slot != -3 && !getenv("SSBU_KEYBOARD_PLAYER")) {
        if (g_cfg.kb_player >= -1) g_kb_slot = g_cfg.kb_player;
        else { g_kb_slot = -1; for (int q = 0; q < 8 && g_kb_slot < 0; q++) if (!slot_taken(q) && slot_usable(q, "") && !g_cfg.pguid[q][0]) g_kb_slot = q; }
        say("nn::pad: keyboard = player %d (settings menu)\n", g_kb_slot + 1);
    }
    unlock();
}
void nnpad_kb_movement_set(int on) {                  /* the Controls page's "SSF2 style": written, and used at the next poll */
    nnpad_kb_vk(0);
    lock();
    g_cfg.kb_move = on != 0;
    kb_write();
    unlock();
}
int nnpad_describe(char *out, int n) {               /* one line per device: "player N: name (type)"; returns the number of devices */
    int k = 0, o = 0;
    out[0] = 0;
    if (!g_cs_ready) return 0;
    lock();
    for (int i = 0; i < MAXDEV && o < n - 1; i++) {
        const Dev *d = &g_dev[i];
        if (!d->used) continue;
        k++;
        if (d->ignored) o += snprintf(out + o, (size_t)(n - o), "(ignored) %s\n", d->name);
        else o += snprintf(out + o, (size_t)(n - o), "player %d: %s (%s%s)\n", d->slot + 1, d->name, d->gp ? type_name(d->gtype) : "raw joystick, guessed layout", d->wireless ? ", wireless" : "");
        if (o >= n) o = n - 1;
    }
    unlock();
    return k;
}
const char *nnpad_config_path(void) { nnpad_kb_vk(0); return g_cfg.path; }

/* ================================================================ U1, the Controls page of the settings menu (menu.cpp)
 * Every pad target (16 Pro buttons + 8 stick directions) can be bound for ONE controller ([device <GUID>]) or for every
 * controller of its kind ([type xbox|playstation|switch|gamecube|generic|joystick]); the device section wins over the type
 * profile, which wins over the built-in default. All of it is written to controllers.ini at once and used at once (the
 * memory copy is changed together with the file; the 1 s reload then finds nothing new). */
static const char *const g_type_label[NTYPE] = {"Xbox", "PlayStation", "Switch", "GameCube", "other pad", "joystick (no SDL mapping)"};
const char *nnpad_type_label(int t) { return t >= 0 && t < NTYPE ? g_type_label[t] : "?"; }
const char *nnpad_type_key(int t) { return t >= 0 && t < NTYPE ? g_type_key[t] : "?"; }
const char *nnpad_target_name(int t) { return t >= 0 && t < 24 ? g_kbn[t] : ""; }
static Dev *dev_at(int index) { return index >= 0 && index < MAXDEV && g_dev[index].used && !g_dev[index].ignored ? &g_dev[index] : NULL; }
int nnpad_list(NnPadDev *o, int max) {               /* the connected devices (not the ignored ones) */
    int n = 0;
    if (!g_sdl_ok) return 0;
    lock();
    for (int i = 0; i < MAXDEV && n < max; i++) {
        const Dev *d = dev_at(i);
        if (!d) continue;
        o[n].index = i; o[n].player = d->slot; o[n].type = dev_type(d); o[n].gamepad = d->gp != NULL;
        snprintf(o[n].name, sizeof o[n].name, "%s", d->name); snprintf(o[n].guid, sizeof o[n].guid, "%s", d->guid);
        n++;
    }
    unlock();
    return n;
}
static DevCfg *scope_cfg(const Dev *d, int scope, int create) { return scope ? &g_cfg.type[dev_type(d)] : cfg_find(&g_cfg, d->guid, create); }
static void srcs_text(const Src *s, int n, char *out, int len) {
    int o = 0;
    out[0] = 0;
    if (!n) { snprintf(out, (size_t)len, "none"); return; }
    for (int k = 0; k < n && o < len - 1; k++) { char t[32]; src_name(s[k], t, sizeof t); o += snprintf(out + o, (size_t)(len - o), "%s%s", k ? ", " : "", t); if (o >= len) o = len - 1; }
}
int nnpad_target_text(int index, int target, char *out, int n) {   /* what drives the target now; returns 2 device, 1 type profile, 0 built-in, -1 no device */
    int from = -1;
    out[0] = 0;
    if (target < 0 || target > 23) return -1;
    lock();
    const Dev *d = dev_at(index);
    if (d) {
        Src s[4]; int k = prof_map(d, target, s, &from);
        if (target >= 16 && !from) {                       /* a stick direction on its axis */
            static const int axis[8] = {0, 1, 0, 1, 2, 3, 2, 3}, positive[8] = {0, 1, 1, 0, 0, 1, 1, 0};
            int i = axis[target - 16], pos = positive[target - 16] ^ stick_inv(d, i) ^ (i & 1);      /* SDL y: down positive */
            Src a; char t[32];
            if (stick_src(d, i, &a)) { src_name(a, t, sizeof t); snprintf(out, (size_t)n, "%s%s", t, a.kind == SK_AXF ? (pos ? "+" : "-") : ""); } else snprintf(out, (size_t)n, "none");
        } else srcs_text(s, k, out, n);
    }
    unlock();
    return from;
}
static void section_write(const Dev *d, int scope) {        /* lock held: the section of that scope, from memory */
    char head[80], body[4096], t[96];
    int o = 0, any = 0;
    DevCfg *c = scope_cfg(d, scope, 0);
    if (scope) snprintf(head, sizeof head, "[type %s]", g_type_key[dev_type(d)]); else snprintf(head, sizeof head, "[device %s]", d->guid);
    if (c) {
        if (!scope) o += snprintf(body + o, sizeof body - (size_t)o, "name = %s\r\n", d->name);
        else o += snprintf(body + o, sizeof body - (size_t)o, "; every %s controller (written by the settings menu, F1); a [device <GUID>] section wins over this one\r\n", g_type_label[dev_type(d)]);
        if (c->layout >= 0) { o += snprintf(body + o, sizeof body - (size_t)o, "layout = %s\r\n", c->layout == 1 ? "labels" : c->layout == 2 ? "auto" : "positional"); any = 1; }
        if (c->dz >= 0) { o += snprintf(body + o, sizeof body - (size_t)o, "deadzone = %.2f\r\n", c->dz); any = 1; }
        if (c->outer >= 0) { o += snprintf(body + o, sizeof body - (size_t)o, "outer_zone = %.2f\r\n", c->outer); any = 1; }
        if (c->trig >= 0) { o += snprintf(body + o, sizeof body - (size_t)o, "trigger_threshold = %.2f\r\n", c->trig); any = 1; }
        if (c->rumble >= 0) { o += snprintf(body + o, sizeof body - (size_t)o, "rumble = %s\r\n", c->rumble ? "on" : "off"); any = 1; }
        if (c->rstr >= 0) { o += snprintf(body + o, sizeof body - (size_t)o, "rumble_strength = %.2f\r\n", c->rstr); any = 1; }
        for (int b = 0; b < 24; b++) if (c->nmap[b] >= 0) { srcs_text(c->map[b], c->nmap[b], t, sizeof t); o += snprintf(body + o, sizeof body - (size_t)o, "map.%s = %s\r\n", g_kbn[b], t); any = 1; }
        for (int i = 0; i < 4; i++) {
            if (c->has_axis[i]) { src_name(c->axis[i], t, sizeof t); o += snprintf(body + o, sizeof body - (size_t)o, "axis.%s = %s\r\n", g_stk[i], t); any = 1; }
            if (c->inv[i]) { o += snprintf(body + o, sizeof body - (size_t)o, "invert.%s = on\r\n", g_stk[i]); any = 1; }
        }
    }
    ini_replace_section(g_cfg.path, head, any ? body : NULL);       /* nothing left: the section goes */
    g_cfg.mtime = file_mtime(g_cfg.path);
    say("nn::pad: %s of %s %s (settings menu)\n", head, g_cfg.path, any ? "written" : "removed");
}
int nnpad_scope_text(int index, int scope, int target, char *out, int n) {   /* the binding at one scope; 0 = not set there */
    int r = 0;
    out[0] = 0;
    lock();
    const Dev *d = dev_at(index);
    DevCfg *c = d ? scope_cfg(d, scope, 0) : NULL;
    if (c && target >= 0 && target < 24 && c->nmap[target] >= 0) { srcs_text(c->map[target], c->nmap[target], out, n); r = 1; }
    unlock();
    return r;
}
void nnpad_target_set(int index, int scope, int target, const char *srcs) {   /* srcs NULL = back to the next level (default), "none" = no source */
    if (target < 0 || target > 23) return;
    lock();
    const Dev *d = dev_at(index);
    DevCfg *c = d ? scope_cfg(d, scope, 1) : NULL;
    if (c) {
        if (!srcs) c->nmap[target] = -1;
        else {
            char b[160];
            snprintf(b, sizeof b, "%s", srcs);
            c->nmap[target] = 0;
            for (char *tok = b; tok && c->nmap[target] < 4; ) { char *comma = strchr(tok, ','); Src x; if (comma) *comma = 0; if (src_parse(tok, &x) && x.kind) c->map[target][c->nmap[target]++] = x; tok = comma ? comma + 1 : NULL; }
        }
        if (!scope && !c->name[0]) snprintf(c->name, sizeof c->name, "%s", d->name);
        section_write(d, scope);
    }
    unlock();
}
void nnpad_profile_reset(int index, int scope) {          /* every binding and tuning of that scope removed (the next level is used again) */
    lock();
    const Dev *d = dev_at(index);
    DevCfg *c = d ? scope_cfg(d, scope, 0) : NULL;
    if (c) {
        char g[40], nm[80];
        snprintf(g, sizeof g, "%s", c->guid); snprintf(nm, sizeof nm, "%s", c->name);
        cfg_init_dev(c);
        snprintf(c->guid, sizeof c->guid, "%s", g); snprintf(c->name, sizeof c->name, "%s", nm);
        section_write(d, scope);
    }
    unlock();
}
void nnpad_tune_get(int index, float v[3]) {            /* what is used now: dead zone, outer zone, trigger threshold */
    v[0] = v[1] = v[2] = 0;
    lock();
    const Dev *d = dev_at(index);
    if (d) { v[0] = EFF(d, dz); v[1] = EFF(d, outer); v[2] = EFF(d, trig); }
    unlock();
}
void nnpad_tune_set(int index, int scope, int which, float value) {     /* which: 0 dead zone, 1 outer zone, 2 trigger threshold */
    lock();
    const Dev *d = dev_at(index);
    DevCfg *c = d ? scope_cfg(d, scope, 1) : NULL;
    if (c) {
        if (which == 0) c->dz = value; else if (which == 1) c->outer = value; else c->trig = value;
        if (!scope && !c->name[0]) snprintf(c->name, sizeof c->name, "%s", d->name);
        section_write(d, scope);
    }
    unlock();
}
/* "press the button / stick direction you want": first everything on that device must be released, then the first source
 * pressed is the answer. Home / Guide or Capture cancels (they belong to the menu). */
static struct { int index, phase; } g_cap = {-1, 0};
void nnpad_capture_begin(int index) { lock(); g_cap.index = index; g_cap.phase = 0; unlock(); }
void nnpad_capture_end(void) { lock(); g_cap.index = -1; unlock(); }
int nnpad_capture_poll(char *out, int n) {               /* 0 waiting, 1 got one (its name in out), 2 cancelled, -1 the device is gone */
    char cur[8][24];
    int r = 0;
    out[0] = 0;
    nnpad_poll(0);
    lock();
    const Dev *d = dev_at(g_cap.index);
    if (!d) r = -1;
    else {
        int k = active_sources(d, cur, 8), cancel = 0, pick = -1;
        for (int i = 0; i < k; i++) { if (!_stricmp(cur[i], "guide") || !_stricmp(cur[i], "misc1")) cancel = 1; else if (pick < 0) pick = i; }
        if (g_cap.phase == 0) { if (!k) g_cap.phase = 1; }
        else if (cancel) r = 2;
        else if (pick >= 0) { snprintf(out, (size_t)n, "%s", cur[pick]); r = 1; }
    }
    unlock();
    return r;
}
/* [players]: which player a device is. player -1 = auto (its GUID taken out of every playerN line), 0..7 = that slot kept
 * for this device (another device that had it moves to a free one). Used at once (reassign_all). */
int nnpad_player_cfg(int index) {
    int r = -1;
    lock();
    const Dev *d = dev_at(index);
    for (int p = 0; d && p < 8; p++) if (!_stricmp(g_cfg.pguid[p], d->guid)) r = p;
    unlock();
    return r;
}
void nnpad_player_assign(int index, int player) {
    lock();
    const Dev *d = dev_at(index);
    if (d) {
        char body[1024];
        int o = 0;
        for (int p = 0; p < 8; p++) if (!_stricmp(g_cfg.pguid[p], d->guid)) g_cfg.pguid[p][0] = 0;
        if (player >= 0 && player < 8) { snprintf(g_cfg.pguid[player], sizeof g_cfg.pguid[0], "%s", d->guid); g_cfg.pnone[player] = 0; }
        o += snprintf(body + o, sizeof body - (size_t)o, "; written by the settings menu (F1): auto, a device GUID (that slot is kept for it) or none\r\n");
        for (int p = 0; p < 8; p++) o += snprintf(body + o, sizeof body - (size_t)o, "player%d = %s\r\n", p + 1, g_cfg.pnone[p] ? "none" : g_cfg.pguid[p][0] ? g_cfg.pguid[p] : "auto");
        ini_replace_section(g_cfg.path, "[players]", body);
        g_cfg.mtime = file_mtime(g_cfg.path);
        g_mem_n = 0;                                     /* the slots remembered in this run would keep the old order */
        reassign_all();
        say("nn::pad: [players] of %s written (settings menu)\n", g_cfg.path);
    }
    unlock();
}
int nnpad_kb_default(int idx) { return idx >= 0 && idx < 24 ? g_kb_default[idx] : 0; }      /* U1: the built-in key of a target */
