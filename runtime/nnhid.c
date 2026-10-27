/* nn::hid: Pro Controllers for players 1..4 from XInput pads, plus the keyboard as one more controller
 * (see "controllers" below for the mapping, vibration and what is not supported).
 *
 * Npad state (all styles share the layout, 0x28 bytes):
 *   int64 samplingNumber; uint64 buttons; int32 lx, ly, rx, ry; uint32 attributes (bit0 connected, bit1 wired)
 * TouchScreenState<16> (0x290 bytes): int64 samplingNumber; int32 count; int32 pad; TouchState[16].
 *
 * Keyboard: arrows = left stick, Z/X/C/V = A/B/X/Y, Q/E = L/R, A/S = ZL/ZR,
 *           Enter = Plus, Backspace = Minus, I/J/K/L = d-pad up/left/down/right. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "nnpad.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))

enum { BTN_A = 0, BTN_B, BTN_X, BTN_Y, BTN_STICK_L, BTN_STICK_R, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
       BTN_LEFT, BTN_UP, BTN_RIGHT, BTN_DOWN, BTN_SL_LEFT, BTN_SL_UP, BTN_SL_RIGHT, BTN_SL_DOWN };
#define STYLE_FULLKEY 1u
#define STYLE_GC 0x20u                                      /* NpadStyleTag_NpadGc = bit 5 (libnx hid.h) */
#define STICK_MAX 32767

static volatile LONG64 g_sampling;
/* Test aid: SSBU_PRESS=<key>,<first second>,<period seconds> holds that key for 0.15 s at each period,
 * whether or not the game window has the focus (e.g. SSBU_PRESS=Z,40,3 presses A every 3 s from second 40). */
static int scripted(int vk) {
    static int parsed, want, first, period;
    static ULONGLONG start;
    if (!parsed) {
        const char *s = getenv("SSBU_PRESS");
        char k = 0;
        parsed = 1;
        start = GetTickCount64();
        if (s && sscanf(s, "%c,%d,%d", &k, &first, &period) == 3 && period > 0) want = k == '!' ? VK_RETURN : k;
    }
    if (!want || vk != want) return 0;
    long long ms = (long long)(GetTickCount64() - start) - first * 1000LL;
    return ms >= 0 && ms % (period * 1000LL) < 150;
}

/* Test aid: SSBU_SCRIPT is a timed input sequence, independent of window focus. Space-separated steps
 * "<second>[+<duration>]:<keys>", duration 0.15 s by default, keys held together. Key characters are the
 * keyboard keys of the mapping above (Z X C V Q E A S I J K L), '!' = Enter (Plus), '-' = Backspace (Minus),
 * and lower-case u d l r = the arrow keys (left stick). Example: "38:Z 41:Z 60+0.6:r 62:Z 70:!".
 * Each step is announced on stdout when it starts, so it can be matched against the frame log. */
#define SCRIPT_MAX 256
static struct { double at, len; char keys[12]; int said; } g_script[SCRIPT_MAX];
static int g_script_count = -1;
static ULONGLONG g_script_start;

static int script_vk(char k) {
    switch (k) {
    case '!': return VK_RETURN;
    case '-': return VK_BACK;
    case 'u': return VK_UP;
    case 'd': return VK_DOWN;
    case 'l': return VK_LEFT;
    case 'r': return VK_RIGHT;
    default: return k;
    }
}

static int script_key(int vk) {
    if (g_script_count < 0) {
        const char *s = getenv("SSBU_SCRIPT");
        g_script_count = 0;
        g_script_start = GetTickCount64();
        while (s && *s && g_script_count < SCRIPT_MAX) {
            double at = 0, len = 0.15;
            char keys[12] = "";
            int used = 0;
            if (sscanf(s, " %lf+%lf:%11[^ ]%n", &at, &len, keys, &used) < 3 && (len = 0.15, sscanf(s, " %lf:%11[^ ]%n", &at, keys, &used) < 2)) break;
            g_script[g_script_count].at = at;
            g_script[g_script_count].len = len;
            strcpy(g_script[g_script_count++].keys, keys);
            s += used;
        }
        if (g_script_count) printf("input script: %d steps\n", g_script_count);
    }
    double t = (double)(GetTickCount64() - g_script_start) / 1000.0;
    for (int i = 0; i < g_script_count; i++) {
        if (t < g_script[i].at || t >= g_script[i].at + g_script[i].len) continue;
        if (!g_script[i].said) { g_script[i].said = 1; printf("input script: t=%.1f s keys %s\n", t, g_script[i].keys); }
        for (const char *k = g_script[i].keys; *k; k++) if (script_vk(*k) == vk) return 1;
    }
    return 0;
}
/* Test aid: SSBU_SEQ is an input sequence counted in pad polls (the game polls player 1 once per frame),
 * so it does not depend on how long this run took to start. Space-separated steps, executed in order:
 *   @movie            wait until the opening movie is created (input_mark("movie") from nnmovie_mf.c)
 *   @module:<name>    wait until nn::ro has loaded that script module (input_mark("module:lua2cpp_mario") from nnro.c)
 *   @draws:<lo>-<hi>[/<n>]  wait until the presented frames had between lo and hi draw calls for n polls in a
 *                     row (default 20): a screen is recognised by its draw count (nvn_gl.c reports it per frame),
 *                     whatever the loading time was
 *   @until:<lo>-<hi>[/<n>]:<keys>/<period>  press the keys (8 polls) every <period> polls until the draw count has
 *                     been in lo..hi for n polls in a row (default 20), e.g. to skip the opening movie, which
 *                     ignores A for a while; period must exceed the transition time plus n
 *   @untilmark:<mark>:<keys>/<period>[/<hold>]  (R3 pub2) press the keys (hold polls, default 8) every <period> polls until
 *                     input_mark(<mark>) has been raised, e.g. the room host's L + R until the mark setup-2p (debug_mode.c): the room's match is set up
 *   <n>               wait n polls
 *   <n>:<keys>[/<h>]  wait n polls, then hold the keys for h polls (default 8); keys as in SSBU_SCRIPT
 *   0:+<keys>[/<h>]   the keys from the poll the previous step's hold ended in: no poll without keys between the two
 *                     (a key held across both steps stays down, e.g. "0:r/20 0:+rZ/4" = right, then A while right is held)
 * Example: "@movie 90:Z 120:Z @draws:150-200 300:u/40 10:Z". Each step is announced with the poll number and the
 * draw count of the last frame. tests\seq_battle.txt is the sequence from boot to a battle (tests\run_battle.sh). */
#define SEQ_MAX 256
#define MARK_MAX 128
enum { SEQ_KEYS = 0, SEQ_MARK, SEQ_DRAWS, SEQ_UNTIL, SEQ_UNTILMARK };
static struct { int wait, hold, kind, lo, hi, need, join; char keys[12]; char mark[40]; } g_seq[SEQ_MAX];
static int g_seq_count = -1, g_seq_index, g_seq_left, g_seq_holding, g_seq_run;
static char g_marks[MARK_MAX][40];
static volatile LONG g_mark_count, g_frame_draws_seen = -1;
static LONG64 g_seq_polls;

void input_mark(const char *what) {                       /* any thread: something the sequence can wait for has happened */
    LONG i = InterlockedIncrement(&g_mark_count) - 1;
    if (i < MARK_MAX) { strncpy(g_marks[i], what, sizeof g_marks[i] - 1); MemoryBarrier(); }
}
void input_frame_draws(int draws) { g_frame_draws_seen = draws; }       /* nvn_gl.c, once per presented frame */

static int mark_seen(const char *what) {
    LONG n = g_mark_count;
    for (LONG i = 0; i < n && i < MARK_MAX; i++) if (!strcmp(g_marks[i], what)) return 1;
    return 0;
}

static void seq_begin(int i) {
    g_seq_index = i;
    g_seq_holding = 0;
    g_seq_run = 0;
    g_seq_left = i < g_seq_count ? g_seq[i].wait : 0;
}

static void seq_tick(void) {                              /* once per poll of player 1 */
    if (g_seq_count < 0) {
        const char *s = getenv("SSBU_SEQ");
        g_seq_count = 0;
        while (s && *s && g_seq_count < SEQ_MAX) {
            char tok[60];
            int used = 0;
            if (sscanf(s, " %59[^ \t\r\n]%n", tok, &used) < 1) break;
            s += used;
            memset(&g_seq[g_seq_count], 0, sizeof g_seq[0]);
            g_seq[g_seq_count].hold = 8;
            if (!strncmp(tok, "@untilmark:", 11)) {
                g_seq[g_seq_count].kind = SEQ_UNTILMARK;
                const char *k = strchr(tok + 11, ':');
                if (!k || (size_t)(k - (tok + 11)) >= sizeof g_seq[0].mark) { printf("input sequence: bad step %s\n", tok); break; }
                memcpy(g_seq[g_seq_count].mark, tok + 11, (size_t)(k - (tok + 11))); g_seq[g_seq_count].mark[k - (tok + 11)] = 0;
                g_seq[g_seq_count].wait = 200;
                if (sscanf(k + 1, "%11[^/]/%d/%d", g_seq[g_seq_count].keys, &g_seq[g_seq_count].wait, &g_seq[g_seq_count].hold) < 1) { printf("input sequence: bad step %s\n", tok); break; }
            } else if (!strncmp(tok, "@until:", 7)) {
                g_seq[g_seq_count].kind = SEQ_UNTIL;
                g_seq[g_seq_count].need = 20;
                g_seq[g_seq_count].wait = 200;
                const char *k = strchr(tok + 7, ':');
                if (sscanf(tok + 7, "%d-%d/%d", &g_seq[g_seq_count].lo, &g_seq[g_seq_count].hi, &g_seq[g_seq_count].need) < 2 || !k ||
                    sscanf(k + 1, "%11[^/]/%d", g_seq[g_seq_count].keys, &g_seq[g_seq_count].wait) < 1) { printf("input sequence: bad step %s\n", tok); break; }
            } else if (!strncmp(tok, "@draws:", 7)) {
                g_seq[g_seq_count].kind = SEQ_DRAWS;
                g_seq[g_seq_count].need = 20;
                if (sscanf(tok + 7, "%d-%d/%d", &g_seq[g_seq_count].lo, &g_seq[g_seq_count].hi, &g_seq[g_seq_count].need) < 2) { printf("input sequence: bad step %s\n", tok); break; }
            } else if (tok[0] == '@') {
                g_seq[g_seq_count].kind = SEQ_MARK;
                strncpy(g_seq[g_seq_count].mark, tok + 1, sizeof g_seq[0].mark - 1);
            } else if (sscanf(tok, "%d:%11[^/]/%d", &g_seq[g_seq_count].wait, g_seq[g_seq_count].keys, &g_seq[g_seq_count].hold) < 1) { printf("input sequence: bad step %s\n", tok); break; }
            else if (g_seq[g_seq_count].keys[0] == '+') { g_seq[g_seq_count].join = 1; memmove(g_seq[g_seq_count].keys, g_seq[g_seq_count].keys + 1, strlen(g_seq[g_seq_count].keys)); }
            g_seq_count++;
        }
        if (g_seq_count) printf("input sequence: %d steps\n", g_seq_count);
        seq_begin(0);
    }
    g_seq_polls++;
    {   /* SSBU_SEQ_LOG=<n>: the draw count every n polls, to find the ranges for @draws */
        static int every = -1;
        if (every < 0) every = getenv("SSBU_SEQ_LOG") ? atoi(getenv("SSBU_SEQ_LOG")) : 0;
        if (every > 0 && g_seq_polls % every == 0) printf("input sequence: poll %lld: %ld draws\n", (long long)g_seq_polls, g_frame_draws_seen);
    }
    if (g_seq_index >= g_seq_count) return;
    if (g_seq[g_seq_index].kind == SEQ_MARK) {
        if (mark_seen(g_seq[g_seq_index].mark)) { printf("input sequence: poll %lld step %d: %s seen\n", (long long)g_seq_polls, g_seq_index, g_seq[g_seq_index].mark); seq_begin(g_seq_index + 1); }
        return;
    }
    if (g_seq[g_seq_index].kind == SEQ_UNTILMARK) {
        static int since2;
        if (!g_seq_holding && g_seq_left == g_seq[g_seq_index].wait) { since2 = g_seq[g_seq_index].wait; g_seq_left = 0; }      /* step just began: press at once */
        if (mark_seen(g_seq[g_seq_index].mark) && !g_seq_holding) {
            printf("input sequence: poll %lld step %d: %s seen\n", (long long)g_seq_polls, g_seq_index, g_seq[g_seq_index].mark);
            seq_begin(g_seq_index + 1);
            return;
        }
        since2++;
        if (g_seq_holding && since2 >= g_seq[g_seq_index].hold) g_seq_holding = 0;
        if (!g_seq_holding && since2 >= g_seq[g_seq_index].wait) {
            g_seq_holding = 1; since2 = 0;
            printf("input sequence: poll %lld step %d keys %s for %d (%ld draws), repeated until %s\n", (long long)g_seq_polls, g_seq_index, g_seq[g_seq_index].keys, g_seq[g_seq_index].hold, g_frame_draws_seen, g_seq[g_seq_index].mark);
        }
        return;
    }
    if (g_seq[g_seq_index].kind == SEQ_UNTIL) {
        LONG d = g_frame_draws_seen;
        static int since;                                 /* polls since this step's last press began */
        if (g_seq_run == 0 && !g_seq_holding && g_seq_left == g_seq[g_seq_index].wait) since = g_seq[g_seq_index].wait;      /* step just began: press at once */
        g_seq_left = 0;
        g_seq_run = d >= g_seq[g_seq_index].lo && d <= g_seq[g_seq_index].hi ? g_seq_run + 1 : 0;
        if (g_seq_run >= g_seq[g_seq_index].need && !g_seq_holding) {
            printf("input sequence: poll %lld step %d: %d..%d draws per frame for %d polls (now %ld)\n", (long long)g_seq_polls, g_seq_index, g_seq[g_seq_index].lo, g_seq[g_seq_index].hi, g_seq[g_seq_index].need, d);
            seq_begin(g_seq_index + 1);
            return;
        }
        since++;
        if (g_seq_holding && since >= 8) g_seq_holding = 0;
        if (!g_seq_holding && since >= g_seq[g_seq_index].wait && g_seq_run == 0) {
            g_seq_holding = 1; since = 0;
            printf("input sequence: poll %lld step %d keys %s for 8 (%ld draws), repeated until %d..%d draws\n", (long long)g_seq_polls, g_seq_index, g_seq[g_seq_index].keys, d, g_seq[g_seq_index].lo, g_seq[g_seq_index].hi);
        }
        return;
    }
    if (g_seq[g_seq_index].kind == SEQ_DRAWS) {
        LONG d = g_frame_draws_seen;
        g_seq_run = d >= g_seq[g_seq_index].lo && d <= g_seq[g_seq_index].hi ? g_seq_run + 1 : 0;
        if (g_seq_run >= g_seq[g_seq_index].need) {
            printf("input sequence: poll %lld step %d: %d..%d draws per frame for %d polls (now %ld)\n", (long long)g_seq_polls, g_seq_index, g_seq[g_seq_index].lo, g_seq[g_seq_index].hi, g_seq[g_seq_index].need, d);
            seq_begin(g_seq_index + 1);
        }
        return;
    }
    if (g_seq_left > 0) { g_seq_left--; return; }
    if (!g_seq_holding) {
        if (!g_seq[g_seq_index].keys[0]) { seq_begin(g_seq_index + 1); return; }
        g_seq_holding = 1;
        g_seq_left = g_seq[g_seq_index].hold;
        printf("input sequence: poll %lld step %d keys %s for %d (%ld draws)\n", (long long)g_seq_polls, g_seq_index, g_seq[g_seq_index].keys, g_seq[g_seq_index].hold, g_frame_draws_seen);
        { void nvn_front_shot_step(int step); nvn_front_shot_step(g_seq_index); }      /* track V: SSBU_SHOT_STEPS=1 saves the frame of every key step (renderer comparison) */
        return;
    }
    seq_begin(g_seq_index + 1);                           /* the hold just ended */
    if (g_seq_index < g_seq_count && g_seq[g_seq_index].kind == SEQ_KEYS && g_seq[g_seq_index].join && !g_seq_left && g_seq[g_seq_index].keys[0]) {
        g_seq_holding = 1;                                /* 0:+<keys>: held from this poll on, no poll without keys in between */
        g_seq_left = g_seq[g_seq_index].hold;
        printf("input sequence: poll %lld step %d keys %s for %d, joined to the step before (%ld draws)\n", (long long)g_seq_polls, g_seq_index, g_seq[g_seq_index].keys, g_seq[g_seq_index].hold, g_frame_draws_seen);
    }
}

static int seq_key(int vk) {
    if (g_seq_count <= 0 || g_seq_index >= g_seq_count || !g_seq_holding) return 0;
    for (const char *k = g_seq[g_seq_index].keys; *k; k++) if (script_vk(*k) == vk) return 1;
    return 0;
}
/* The real keyboard counts only while a window of THIS process is the foreground window: GetAsyncKeyState reads the
 * whole desktop, so without this check anything typed in another program (or another running copy of the game, or a
 * test that injects keys) steered the game. With SSBU_SEQ the keyboard is ignored altogether, unless SSBU_SEQ_KEYBOARD
 * is set, so a scripted run is not disturbed by a person at the machine. */
static int keyboard_ours(void) {
    static int mode = -1;
    static LONG64 checked_at = -1;
    static int ours;
    if (mode < 0) mode = (getenv("SSBU_SEQ") || getenv("SSBU_PLAY")) && !getenv("SSBU_SEQ_KEYBOARD") ? 0 : 1;
    if (!mode) return 0;
    if (checked_at != g_sampling) {                       /* once per poll */
        DWORD pid = 0;
        HWND fg = GetForegroundWindow();
        checked_at = g_sampling;
        ours = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
    }
    return ours;
}
static int key(int vk) { return scripted(vk) || script_key(vk) || seq_key(vk) || (keyboard_ours() && (GetAsyncKeyState(vk) & 0x8000) != 0); }

/* Every session's pad input is recorded, so a path played by hand can be replayed exactly as a test.
 * Recording: always, to the file named by SSBU_REC or input_last.rec; one line per change of the pad:
 *     a <anchor> <polls since that anchor> <buttons hex> <lx> <ly>
 * Replay: SSBU_PLAY=<file> feeds a recording back instead of the keyboard.
 *
 * Anchors make the clock independent of loading times. Poll counts alone are not: the time between two screens
 * depends on how long this run took to load them. The run is divided by SCREEN CHANGES, recognised from the number
 * of draw calls per frame (reported by nvn_gl.c), in four classes: black (<= 12 draws: every fade between screens),
 * low (13..60: movie, title logo, loading icon), mid (61..240: character select), high (> 240: menus, stage select,
 * battle). Anchor 0 is the creation of the opening movie; each later anchor is the first poll of a class that then
 * lasted 30 polls (shorter blips while a screen builds up are ignored). An input is replayed when the same anchor
 * has been reached and the same number of polls has passed since it.
 * The older format "<poll> <buttons hex> <lx> <ly>" (polls counted from the movie only) is still read. */
static struct { long long poll; int anchor; uint64_t buttons; int lx, ly; } *g_play;
static int g_play_count = -1, g_play_pos;
static FILE *g_rec;
static long long g_rec_poll, g_rec_mark = -1;
static uint64_t g_rec_buttons = ~0ull;
static int g_rec_lx, g_rec_ly;
static int g_anchor = -1, g_anchor_class = -1, g_anchor_cand = -1, g_anchor_run;
static long long g_anchor_poll, g_anchor_cand_poll;

static int draw_class(long d) { return d <= 12 ? 0 : d <= 60 ? 1 : d <= 240 ? 2 : 3; }

static void anchor_tick(void) {                           /* once per poll */
    if (g_anchor < 0) {
        if (!mark_seen("movie")) return;
        g_anchor = 0; g_anchor_poll = g_rec_poll; g_anchor_class = -1; g_anchor_cand = -1;
        printf("input anchor 0 at poll %lld: opening movie created\n", g_rec_poll);
        return;
    }
    int cls = draw_class(g_frame_draws_seen);
    if (cls == g_anchor_class) { g_anchor_cand = -1; return; }
    if (cls != g_anchor_cand) { g_anchor_cand = cls; g_anchor_run = 0; g_anchor_cand_poll = g_rec_poll; }
    if (++g_anchor_run >= 30) {
        static const char *const names[4] = {"black", "low", "mid", "high"};
        g_anchor_class = cls; g_anchor_cand = -1;
        g_anchor++; g_anchor_poll = g_anchor_cand_poll;
        printf("input anchor %d at poll %lld: %s (%ld draws per frame)\n", g_anchor, g_anchor_poll, names[cls], g_frame_draws_seen);
    }
}

static void record_or_replay(uint64_t *buttons, int *lx, int *ly) {
    if (g_play_count < 0) {
        const char *play = getenv("SSBU_PLAY"), *rec = getenv("SSBU_REC");
        g_play_count = 0;
        FILE *f = play ? fopen(play, "r") : NULL;
        if (f) {
            char line[256];
            int cap = 0;
            while (fgets(line, sizeof line, f)) {
                long long poll; unsigned long long b; int x, y, anchor = -1;
                if (line[0] == '#') continue;
                if (sscanf(line, "a %d %lld %llx %d %d", &anchor, &poll, &b, &x, &y) != 5 && (anchor = -1, sscanf(line, "%lld %llx %d %d", &poll, &b, &x, &y) != 4)) continue;
                if (g_play_count == cap) { cap = cap ? cap * 2 : 1024; g_play = realloc(g_play, (size_t)cap * sizeof *g_play); }
                g_play[g_play_count].poll = poll; g_play[g_play_count].anchor = anchor; g_play[g_play_count].buttons = b; g_play[g_play_count].lx = x; g_play[g_play_count++].ly = y;
            }
            fclose(f);
            printf("input replay: %d changes from %s\n", g_play_count, play);
        } else if (play) printf("input replay: cannot open %s\n", play);
        if (!g_play_count) {
            g_rec = fopen(rec ? rec : "input_last.rec", "w");
            if (g_rec) fprintf(g_rec, "# a <anchor> <polls since the anchor> <buttons hex> <lx> <ly>   (see nnhid.c; replay with SSBU_PLAY=<this file>)\n");
        }
    }
    g_rec_poll++;
    if (g_rec_mark < 0 && mark_seen("movie")) g_rec_mark = g_rec_poll;
    anchor_tick();
    long long rel = g_rec_mark < 0 ? g_rec_poll - 1000000 : g_rec_poll - g_rec_mark;   /* before the movie: large negative */
    long long since = g_rec_poll - g_anchor_poll;
    if (g_play_count) {
        while (g_play_pos < g_play_count) {
            int due = g_play[g_play_pos].anchor < 0 ? g_play[g_play_pos].poll <= rel
                    : g_anchor > g_play[g_play_pos].anchor || (g_anchor == g_play[g_play_pos].anchor && since >= g_play[g_play_pos].poll);
            if (!due) break;
            if (g_play[g_play_pos].buttons || g_play[g_play_pos].lx || g_play[g_play_pos].ly)
                printf("input replay: poll %lld (anchor %d + %lld) buttons %llx stick %d %d\n", g_rec_poll, g_anchor, since, (unsigned long long)g_play[g_play_pos].buttons, g_play[g_play_pos].lx, g_play[g_play_pos].ly);
            g_play_pos++;
        }
        if (g_play_pos > 0 && (g_play[g_play_pos - 1].anchor >= 0 || g_play[g_play_pos - 1].poll >= 0)) { *buttons = g_play[g_play_pos - 1].buttons; *lx = g_play[g_play_pos - 1].lx; *ly = g_play[g_play_pos - 1].ly; }
        else { *buttons = 0; *lx = 0; *ly = 0; }
        return;
    }
    if (g_rec && g_anchor >= 0 && (*buttons != g_rec_buttons || *lx != g_rec_lx || *ly != g_rec_ly)) {
        g_rec_buttons = *buttons; g_rec_lx = *lx; g_rec_ly = *ly;
        fprintf(g_rec, "a %d %lld %llx %d %d\n", g_anchor, since, (unsigned long long)*buttons, *lx, *ly);
        fflush(g_rec);
    }
}

/* ================================================================ controllers
 * Players 1..4 are the XInput controllers 0..3 (Xbox pads and everything that presents itself as one), each
 * reported to the game as a Pro Controller (style NpadStyleFullKey). The keyboard is one more source: it belongs to
 * the player named by SSBU_KEYBOARD_PLAYER (1..8, default 1, 0 = nobody) and is merged with that player's pad, so a
 * machine without a pad still has player 1 and a machine with one pad can have two players (SSBU_KEYBOARD_PLAYER=2).
 * Not supported: more than four pads (XInput's limit; other HID pads would need Raw Input), a GameCube adapter,
 * motion sensors (XInput pads have none), touch (the console is docked: no touch input).
 *
 * Buttons: by POSITION by default (the game's prompts are for a Nintendo pad: its A is the right-hand face button,
 * which is B on an Xbox pad); SSBU_PAD_LAYOUT=label maps by the printed letter instead. Triggers are ZL/ZR from a
 * third of their travel. Start = Plus, Back = Minus. Sticks: -32767..32767, no dead zone added (the game has its
 * own). The "stick as buttons" bits (16..23) are set beyond half deflection (NOT VERIFIED on a console: the value
 * the system uses; 0.5 is what the suyu emulator uses).
 *
 * Vibration: the game drives two linear actuators per Pro Controller (left and right handle, each with a low band
 * around 160 Hz and a high band around 320 Hz). An XInput pad has a heavy low-frequency motor and a light
 * high-frequency one: the low motor gets the larger low-band amplitude of the two handles, the high motor the
 * larger high-band amplitude.
 *
 * Real pads and the keyboard count only while a window of this process has the focus, and never in scripted runs
 * (SSBU_SEQ / SSBU_PLAY without SSBU_SEQ_KEYBOARD), so tests are not disturbed by a pad lying on the desk.
 *
 * What the SDK itself does was read in sdk.elf: GetNpadControllerColor accepts only success, hid 603 and hid 604
 * from the service (anything else aborts) and 604 = 0x4b8ca is "no controller of that kind"; handles are
 * style index | npad id << 8 | device index << 16 (GetVibrationDeviceHandles / GetSixAxisSensorHandles run as
 * lifted SDK code). */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } XGamepad;
typedef struct { DWORD packet; XGamepad pad; } XState;
typedef struct { WORD left, right; } XVibration;
typedef struct { BYTE type, subtype; WORD flags; XGamepad pad; XVibration vib; } XCaps;
static DWORD (WINAPI *x_get_state)(DWORD, XState *);
static DWORD (WINAPI *x_get_state_ex)(DWORD, XState *);      /* U1: XInputGetStateEx (ordinal 100): the same plus the Guide bit 0x0400; absent from some DLLs */
static DWORD (WINAPI *x_set_state)(DWORD, XVibration *);
static DWORD (WINAPI *x_get_caps)(DWORD, DWORD, XCaps *);
static struct { int present, wireless, dropped; ULONGLONG checked, read; XState st; XVibration sent; } g_x[4];
static int g_source[8] = {0, 1, 2, 3, -1, -1, -1, -1};      /* XInput slot of each player, -1 = none */
static int g_keyboard_player = -1;                           /* 0-based, -1 = nobody */
static uint32_t g_supported_styles = 0xffffffffu;            /* until SetSupportedNpadStyleSet */
static uint32_t g_supported_ids[16];
static int g_supported_id_count = -1;                        /* -1 = SetSupportedNpadIdType not called yet: all */
static int g_hid_ready, g_touch_ready, g_label_layout;
static int g_sdl_pad, g_pad_test;                            /* SDL backend (nnpad_sdl.c; SSBU_PAD_BACKEND=xinput switches it off); self-test of it running */
static struct { float v[4]; int set; } g_vib[8][2];
static const float g_vib_default[4] = {0.0f, 160.0f, 0.0f, 320.0f};

/* ---- synthetic XInput pad (test aid, bounded round 2026-10-09; no physical pad was available to test with).
 * The three XInput entry points are replaced AT THE BOUNDARY: the function pointers that hid_setup otherwise takes from
 * xinput1_4.dll. Everything above them (x_present, fill_pad, vib_apply, connection state, the handlers the game calls)
 * is the code a real pad runs.
 *   SSBU_XINPUT_SYNTH=1  slot 0 holds a wired pad whose state is built from the keys of the input sequence (SSBU_SEQ):
 *                        Z X C V = the face buttons at the POSITION of the console's A B X Y (XInput B A Y X),
 *                        Q E = shoulders, A S = triggers at full travel, arrows = left stick at full deflection,
 *                        I J K L = d-pad, ! and - = Start and Back. The sequence's own path into player 1 (and the
 *                        keyboard) is switched off, so every input of the run reaches the game through the XInput mapping.
 *   hid_report (end of run): GetState / SetState call counts and the largest motor values the game's vibration produced.
 * The self-test (nnhid_selftest_xinput) uses the same boundary with a state it sets itself, on slot 1. */
static int g_x_synth;                                         /* 1 = SSBU_XINPUT_SYNTH run, 2 = self-test */
static int sdl_on(void) { return g_sdl_pad && !g_x_synth; }
static struct { XState st; int connected, wireless; long long gets, sets; XVibration last, max; } g_xs[4];
static DWORD WINAPI synth_get_state(DWORD slot, XState *out) {
    if (slot > 3) return 1167;
    g_xs[slot].gets++;
    if (g_x_synth == 1 && slot == 0) {
        static const struct { int vk; WORD x; } m[] = {
            {'Z', 0x2000}, {'X', 0x1000}, {'C', 0x8000}, {'V', 0x4000}, {'Q', 0x0100}, {'E', 0x0200}, {VK_RETURN, 0x0010}, {VK_BACK, 0x0020},
            {'I', 0x0001}, {'K', 0x0002}, {'J', 0x0004}, {'L', 0x0008},
        };
        XGamepad g;
        memset(&g, 0, sizeof g);
        for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (seq_key(m[i].vk)) g.buttons |= m[i].x;
        g.lt = seq_key('A') ? 255 : 0;
        g.rt = seq_key('S') ? 255 : 0;
        g.lx = (SHORT)((seq_key(VK_RIGHT) - seq_key(VK_LEFT)) * 32767);
        g.ly = (SHORT)((seq_key(VK_UP) - seq_key(VK_DOWN)) * 32767);
        if (memcmp(&g, &g_xs[0].st.pad, sizeof g)) { g_xs[0].st.pad = g; g_xs[0].st.packet++; }
        g_xs[0].connected = 1;
    }
    if (!g_xs[slot].connected) return 1167;                   /* ERROR_DEVICE_NOT_CONNECTED */
    *out = g_xs[slot].st;
    return 0;
}
static DWORD WINAPI synth_set_state(DWORD slot, XVibration *v) {
    if (slot > 3 || !g_xs[slot].connected) return 1167;
    g_xs[slot].sets++;
    g_xs[slot].last = *v;
    if (v->left > g_xs[slot].max.left) g_xs[slot].max.left = v->left;
    if (v->right > g_xs[slot].max.right) g_xs[slot].max.right = v->right;
    return 0;
}
static DWORD WINAPI synth_get_caps(DWORD slot, DWORD flags, XCaps *out) {
    (void)flags;
    if (slot > 3 || !g_xs[slot].connected) return 1167;
    memset(out, 0, sizeof *out);
    out->type = 1; out->subtype = 1;                          /* XINPUT_DEVTYPE_GAMEPAD, XINPUT_DEVSUBTYPE_GAMEPAD */
    out->flags = (WORD)(g_xs[slot].wireless ? 0x0002 : 0);    /* XINPUT_CAPS_WIRELESS */
    return 0;
}
void hid_report(FILE *out) {
    if (g_x_synth != 1) return;
    fprintf(out, "nn::hid: synthetic XInput pad on slot 0 (SSBU_XINPUT_SYNTH): %lld XInputGetState calls, %lld state changes, %lld XInputSetState calls "
                 "(vibration), largest motor values left %u right %u of 65535, last left %u right %u\n",
            g_xs[0].gets, (long long)g_xs[0].st.packet, g_xs[0].sets, g_xs[0].max.left, g_xs[0].max.right, g_xs[0].last.left, g_xs[0].last.right);
}

static void hid_setup(void) {
    if (g_hid_ready) return;
    g_hid_ready = 1;
    { void r1_init(void); r1_init(); }                       /* R1 (r1_state.c): nothing without SSBU_R1 */
    if (getenv("SSBU_XINPUT_SYNTH")) {
        g_x_synth = 1;
        x_get_state = synth_get_state; x_set_state = synth_set_state; x_get_caps = synth_get_caps;
        printf("nn::hid: SYNTHETIC XInput pad on slot 0, driven by the input sequence (test aid; the sequence's keyboard path is off)\n");
    }
    static const char *const dlls[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
    for (int i = 0; i < 3 && !x_get_state; i++) {
        HMODULE m = LoadLibraryA(dlls[i]);
        if (!m) continue;
        x_get_state = (void *)GetProcAddress(m, "XInputGetState");
        if (x_get_state) x_get_state_ex = (void *)GetProcAddress(m, (LPCSTR)(uintptr_t)100);
        x_set_state = (void *)GetProcAddress(m, "XInputSetState");
        x_get_caps = (void *)GetProcAddress(m, "XInputGetCapabilities");
    }
    const char *kb = getenv("SSBU_KEYBOARD_PLAYER"), *layout = getenv("SSBU_PAD_LAYOUT"), *backend = getenv("SSBU_PAD_BACKEND");
    int k = kb && kb[0] ? atoi(kb) : 1;
    g_keyboard_player = k >= 1 && k <= 8 ? k - 1 : -1;
    g_label_layout = layout && !strcmp(layout, "label");
    /* default backend = SDL3 (nnpad_sdl.c, controllers.ini); SSBU_PAD_BACKEND=xinput keeps the older XInput path below, and is the fallback if SDL cannot start */
    if (!g_x_synth && !(backend && !_stricmp(backend, "xinput"))) {
        g_sdl_pad = nnpad_init();
        if (g_sdl_pad) { g_keyboard_player = nnpad_keyboard_player(); printf("nn::hid: pads through SDL3; keyboard = player %d\n", g_keyboard_player + 1); return; }
        printf("nn::hid: SDL could not start, falling back to XInput\n");
    }
    printf("nn::hid: XInput %s; keyboard = player %d; buttons by %s\n", x_get_state ? "available" : "NOT available (no pads)",
           g_keyboard_player + 1, g_label_layout ? "label" : "position");
}

static int devices_live(void) { return g_x_synth || g_pad_test || keyboard_ours(); }   /* same rule for pads and keyboard; a synthetic pad is always live */

/* A disconnected XInput slot is expensive to ask, so it is probed once a second; a connected one at most once per ms. */
static int x_present(int slot) {
    if (slot < 0 || slot > 3 || !x_get_state) return 0;
    ULONGLONG now = GetTickCount64();
    if (g_x[slot].present) {
        if (now != g_x[slot].read) {
            g_x[slot].read = now;
            if (x_get_state((DWORD)slot, &g_x[slot].st) != 0) { g_x[slot].present = 0; g_x[slot].checked = now; printf("nn::hid: pad %d disconnected\n", slot + 1); }
        }
    } else if (!g_x[slot].checked || now - g_x[slot].checked >= 1000) {
        g_x[slot].checked = now;
        if (x_get_state((DWORD)slot, &g_x[slot].st) == 0) {
            XCaps caps;
            memset(&caps, 0, sizeof caps);
            g_x[slot].present = 1; g_x[slot].read = now; g_x[slot].dropped = 0;
            g_x[slot].wireless = x_get_caps && x_get_caps((DWORD)slot, 0, &caps) == 0 && (caps.flags & 0x0002);
            memset(&g_x[slot].sent, 0, sizeof g_x[slot].sent);
            printf("nn::hid: pad %d connected (%s)\n", slot + 1, g_x[slot].wireless ? "wireless" : "wired");
        }
    }
    /* DisconnectNpad dropped the link: the pad comes back when one of its buttons is pressed, as on the console */
    if (g_x[slot].present && g_x[slot].dropped) { if (g_x[slot].st.pad.buttons) g_x[slot].dropped = 0; else return 0; }
    return g_x[slot].present;
}

static int scripted_run(void) { static int m = -1; if (m < 0) m = (getenv("SSBU_SEQ") || getenv("SSBU_PLAY") || getenv("SSBU_PRESS") || getenv("SSBU_SCRIPT")) != 0; return m; }
static int id_supported(uint32_t id) {
    if (g_supported_id_count < 0) return 1;
    for (int i = 0; i < g_supported_id_count; i++) if (g_supported_ids[i] == id) return 1;
    return 0;
}
/* Is a controller connected as this npad id? Player 1 always exists in scripted runs (the script is its pad). */
static int player_connected(uint32_t id) {
    hid_setup();
    if (id > 7 || !id_supported(id)) return 0;
    {   /* a GameCube controller needs the game to support its own style or the Pro one it is shown as */
        uint32_t need = STYLE_FULLKEY | (sdl_on() && nnpad_is_gc((int)id) ? STYLE_GC : 0);
        if (!(g_supported_styles & need)) return 0;
    }
    if (g_x_synth == 1 && id == 0) return x_present(g_source[id]);      /* synthetic pad run: player 1 exists only as that pad */
    if (g_x_synth == 2 && id == 1) return x_present(g_source[id]);      /* self-test: player 2 is the test pad, whatever the keyboard player is */
    if (id == 0 && scripted_run()) return 1;
    { int r1_net_on(void); if (id == 1 && r1_net_on()) return 1; }      /* R3 online rollback (SSBU_RB_NET): port 2 is the other game's player */
    if ((int)id == g_keyboard_player) return 1;
    if (sdl_on()) return nnpad_present((int)id);
    return x_present(g_source[id]);
}

int menu_input_blocked(void);                                 /* U1 (menu.cpp) */
/* U1 TEST ONLY (menu.cpp, SSBU_MENU_TEST): a key of the PC keyboard taken as held for the keyboard's player through its
 * configured binding, whatever the focus or the input sequence (the real keyboard is off in scripted runs); what the game
 * then reads for that player is recorded. 0 = off: nothing changes. */
static volatile LONG g_test_vk, g_test_polls;
static volatile LONG64 g_test_seen;
void nnhid_test_key(int vk) { InterlockedExchange(&g_test_polls, 0); InterlockedExchange64(&g_test_seen, 0); InterlockedExchange(&g_test_vk, vk); }
long nnhid_test_result(uint64_t *buttons) { *buttons = (uint64_t)g_test_seen; return g_test_polls; }
/* Player 1 gets the test scripts (SSBU_PRESS / SSBU_SCRIPT / SSBU_SEQ); the keyboard's player gets the real keys. */
static int key_for(uint32_t id, int vk, int kbidx) {         /* vk = the fixed key of the test scripts; kbidx = which configurable binding (controllers.ini) the real keyboard uses */
    int down = 0;
    if (id == 0) down = scripted(vk) || script_key(vk) || seq_key(vk);
    /* U1 (menu.cpp): while the settings menu is open the real keyboard is its own */
    if (!down && (int)id == g_keyboard_player && !menu_input_blocked()) {
        int real = sdl_on() ? nnpad_kb_vk(kbidx) : vk;
        down = real && keyboard_ours() && (GetAsyncKeyState(real) & 0x8000) != 0;
        if (!down && g_test_vk && real == g_test_vk) down = 1;      /* U1 test only (nnhid_test_key) */
    }
    return down;
}

/* Track V session 13, TEST ONLY: SSBU_SHOT_FIGHT=<n>[,<n>...] (ascending): a picture of fight frame n. The fight frames
 * are counted here, at player 1's pad poll, from the input mark "fight" on (with SSBU_DETERMINISTIC the number of the
 * first fighter's frame-line calls: fight_seq_ticks; without it one per poll). When the count reaches n the front end is
 * told (nvn_front_shot_fight); the next present the game queues is saved as fight_<n>.bmp by the renderer in use. */
static void fight_shot_poll(int ticks) {
    static int list[16], n = -1, next;
    static long long ff;
    void nvn_front_shot_fight(int frame);
    if (n < 0) { const char *e = getenv("SSBU_SHOT_FIGHT"); n = 0; while (e && *e && n < 16) { list[n++] = atoi(e); e = strchr(e, ','); if (e) e++; } }
    if (next >= n || !mark_seen("fight")) return;
    ff += ticks;
    if (ff >= list[next]) { printf("shot: fight frame %lld reached at poll %lld (wanted %d): the next queued present is saved\n", ff, (long long)g_seq_polls, list[next]); fflush(stdout); nvn_front_shot_fight(list[next]); next++; }
}
/* [keyboard] movement = ssf2 (controllers.ini; SSBU_KEYBOARD_MOVEMENT=ssf2 overrides it): the keys of the left stick read the
 * way Super Smash Flash 2 (McLeodGaming, beta 1.4.0.1, a keyboard game) reads its direction keys. With movement = full a stick
 * key pushes the stick fully in one poll, which the game takes as a flick: a key always dashes, down always drops through a
 * platform, and walking, crouching on a platform and tilt attacks need an analog stick. SSF2's own rules (its Character.as:
 * checkDoubleTap, the smash timer, the jump squat; it runs at 30 frames a second, so its frame counts are doubled here)
 * decide when a key pushes part way and when fully:
 *  - a horizontal key walks: `walk` for `walk_full` polls, then fully (full walking speed and air drift, still no dash);
 *  - down crouches: `crouch`, which also fast-falls in the air but does not drop through a platform;
 *  - the same key again within `dtap` polls of letting it go pushes fully: a dash, for down a drop through a platform;
 *  - while a horizontal key pushes fully, or within `dance` polls of letting it go, the other one pushes fully too (dash
 *    dance, run turnaround); a key pressed while the shield (ZL / ZR) is held pushes fully (roll, spot dodge);
 *  - A within `smash_x` polls of a horizontal key (`smash_y` of down) is a smash attack: in the first `flick` polls of the key
 *    the key pushes fully in the same poll (from `walk` / `crouch` the game still takes that as a flick); later the stick goes
 *    back to the middle for that poll and A comes in the next one, with the full push (measured: from `walk` to full with A
 *    3 polls after the key = smash, 6 polls after = tilt; back to `walk` for one poll before it = tilt as well);
 *  - up: below (kb_ssf2).
 * Measured with SSBU_FIGHT_HASH (status and motion of the fighter per frame; Mario on Battlefield, 13.0.4, one run per value;
 * other fighters NOT measured, the thresholds are expected to be common to all): down at 0.60 / 0.62 / 0.63 / 0.64 does
 * nothing; 0.65 / 0.655 / 0.66 crouch on a platform without dropping and fast-fall from the top of a jump; 0.67 and more drop
 * through. A horizontal key at 0.75 / 0.8 walks, 0.85 / 0.9 / 0.95 dashes; 0.5 / 0.6 / 0.7 for 4 polls and then 1.0 walk
 * (walk_slow .. walk_fast), no dash. The clock is this player's poll count: the game polls each player once a frame (SSBU_SEQ
 * counts the same polls). */
static struct { float walk, crouch; int walk_full, dtap, dance, smash_x, smash_y, flick, up_wait, up_keep; } g_kbt =
    {0.60f, 0.655f, 4, 12, 12, 9, 7, 3, 4, 3};
static void kb_tune(void) {         /* TEST ONLY: SSBU_KB_SSF2_TUNE="walk=0.6,crouch=0.655,walkfull=4,dtap=12,dance=12,smashx=9,..." */
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("SSBU_KB_SSF2_TUNE");
    char k[16]; double v; int n;
    while (e && sscanf(e, " %15[a-z_]=%lf%n", k, &v, &n) == 2) {
        if (!strcmp(k, "walk")) g_kbt.walk = (float)v; else if (!strcmp(k, "crouch")) g_kbt.crouch = (float)v;
        else if (!strcmp(k, "walkfull")) g_kbt.walk_full = (int)v; else if (!strcmp(k, "dtap")) g_kbt.dtap = (int)v;
        else if (!strcmp(k, "dance")) g_kbt.dance = (int)v; else if (!strcmp(k, "smashx")) g_kbt.smash_x = (int)v;
        else if (!strcmp(k, "smashy")) g_kbt.smash_y = (int)v; else if (!strcmp(k, "upwait")) g_kbt.up_wait = (int)v;
        else if (!strcmp(k, "upkeep")) g_kbt.up_keep = (int)v; else if (!strcmp(k, "flick")) g_kbt.flick = (int)v;
        e += n;
        if (*e == ',') e++;
    }
    printf("nn::hid: keyboard movement ssf2: walk %.3f crouch %.3f walk_full %d dtap %d dance %d smash_x %d smash_y %d flick %d up_wait %d up_keep %d\n",
           g_kbt.walk, g_kbt.crouch, g_kbt.walk_full, g_kbt.dtap, g_kbt.dance, g_kbt.smash_x, g_kbt.smash_y, g_kbt.flick, g_kbt.up_wait, g_kbt.up_keep);
}
typedef struct {
    long long t;                    /* this player's polls */
    int kx, ky;                     /* the keys' direction at the last poll: -1, 0, 1 */
    int x_full, y_full;             /* the held horizontal / vertical key pushes fully */
    long long x_at, y_at;           /* poll its key went down */
    long long x_up[2], y_up[2];     /* poll a key was last let go: [0] left / down, [1] right / up */
    long long run_up;               /* poll a horizontal key that pushed fully was let go */
    int smash_a;                    /* A held back one poll for the flick of a smash attack: given in the next poll */
    int up_pend;                    /* up is held back (up_wait) */
    long long up_keep;              /* up stays pushed until this poll (a full hop through the jump squat) */
    uint64_t prev;                  /* buttons at the last poll */
} KbMove;
static KbMove g_kbm[8];
static void kb_ssf2(uint32_t id, uint64_t *buttons, int *lx, int *ly) {
    KbMove *m = &g_kbm[id];
    kb_tune();
    long long t = ++m->t;
    if (t == 1) m->x_up[0] = m->x_up[1] = m->y_up[0] = m->y_up[1] = m->run_up = -1000;
    int kx = (*lx > 0) - (*lx < 0), ky = (*ly > 0) - (*ly < 0);
    uint64_t pressed = *buttons & ~m->prev;
    int shield = (*buttons & (1ull << BTN_ZL | 1ull << BTN_ZR)) != 0, attack = pressed >> BTN_A & 1, special = pressed >> BTN_B & 1;
    m->prev = *buttons;
    if (m->smash_a) { *buttons |= 1ull << BTN_A; m->smash_a = 0; }
    if (kx != m->kx) {
        if (m->kx) { m->x_up[m->kx > 0] = t; if (m->x_full) m->run_up = t; }
        m->x_full = kx && (t - m->x_up[kx > 0] <= g_kbt.dtap || t - m->run_up <= g_kbt.dance || shield);
        m->x_at = t;
        m->kx = kx;
    }
    if (ky != m->ky) {
        if (m->ky) m->y_up[m->ky > 0] = t;
        m->y_full = ky < 0 && (t - m->y_up[0] <= g_kbt.dtap || shield);
        if (ky > 0) m->up_pend = g_kbt.up_wait > 0;
        m->y_at = t;
        m->ky = ky;
    }
    int ramped = g_kbt.walk_full && t - m->x_at >= g_kbt.walk_full, back = 0;      /* back: 1 x, 2 y to the middle this poll */
    if (attack && kx && t - m->x_at < g_kbt.smash_x) {
        if (!m->x_full && (ramped || t - m->x_at > g_kbt.flick)) back |= 1;
        m->x_full = 1;
    }
    if (attack && ky < 0 && t - m->y_at < g_kbt.smash_y) {
        if (!m->y_full && t - m->y_at > g_kbt.flick) back |= 2;
        m->y_full = 1;
    }
    if (back) { *buttons &= ~(1ull << BTN_A); m->smash_a = 1; }
    if (back & 1) *lx = 0;
    else if (kx && !m->x_full && !ramped) *lx = (int)(kx * g_kbt.walk * STICK_MAX);
    if (back & 2) *ly = 0;
    else if (ky < 0 && !m->y_full) *ly = -(int)(g_kbt.crouch * STICK_MAX);
    /* Up: SSF2's tap jump leaves the ground only after the jump squat (2 of its frames: 4 here), and A or B pressed in it is an up
     * smash or an up special on the ground. The game takes A in its own jump squat as an aerial (measured: up, then up + A two
     * polls later = attack_air_hi), so up is held back for up_wait polls: A / B in them goes out with up in one poll (up smash,
     * up special), up let go in them is one poll of up (a short hop), up still held after them is the jump, kept up_keep polls
     * (the game's jump squat) so that it is a full hop. */
    int up = ky > 0;
    if (m->up_pend) {
        if (attack || special || ky <= 0) up = 1, m->up_pend = 0;
        else if (t - m->y_at >= g_kbt.up_wait) up = 1, m->up_pend = 0, m->up_keep = t + g_kbt.up_keep;
        else up = 0;
    } else if (t < m->up_keep) up = 1;
    if (up) *ly = STICK_MAX; else if (ky > 0) *ly = 0;
}

/* kind: 0 = a style the controller is not reported in (zeroed), 1 = NpadFullKeyState (Pro Controller, 0x28 bytes), 2 = NpadGcState (0x30 bytes:
 * the same first 0x24 bytes, then analog trigger L at +0x24 and R at +0x28, 0..0x7fff; the layout follows libnx / suyu, NOT VERIFIED on a console). */
static void fill_pad(uint64_t out, uint32_t id, int kind) {
    size_t size = kind == 2 ? 0x30 : 0x28;
    { void r1_wrote(uint64_t, uint64_t); r1_wrote(out, size); }   /* R1: marked (nothing without SSBU_R1_MARK) */
    memset((void *)(uintptr_t)out, 0, size);
    wr64(out, (uint64_t)InterlockedIncrement64(&g_sampling));
    if (id > 7 || !kind) return;                              /* the other styles are never filled */
    unsigned trig_l = 0, trig_r = 0;
    static long long l1_ticks;                               /* L1: fight-frame ticks seen at player 1's polls (SSBU_L1 report) */
    int r1_on(void), r1_poll(int ticks); void r1_pad(uint64_t *b, int *lx, int *ly, int *rx, int *ry);    /* R1 (r1_state.c, SSBU_R1 only) */
    if (id == 0) { int fight_seq_ticks(void); int nt = fight_seq_ticks();
                   if (r1_on() && r1_poll(mark_seen("fight") ? nt : 0)) nt = 0;                  /* R1 re-run: the sequence waits, the recorded values are given (R1 run-ahead: the speculative poll) */
                   fight_shot_poll(nt); if (mark_seen("fight")) l1_ticks += nt;
                   for (int n = nt; n > 0; n--) seq_tick(); }     /* S11: 1 per poll; SSBU_DETERMINISTIC in a fight: per fight frame (fighter_sdk\fighter_host.c) */
    if (!player_connected(id)) return;
    static const struct { int vk, bit; } map[] = {
        {'Z', BTN_A}, {'X', BTN_B}, {'C', BTN_X}, {'V', BTN_Y}, {'Q', BTN_L}, {'E', BTN_R}, {'A', BTN_ZL}, {'S', BTN_ZR},
        {VK_RETURN, BTN_PLUS}, {VK_BACK, BTN_MINUS}, {'J', BTN_LEFT}, {'I', BTN_UP}, {'L', BTN_RIGHT}, {'K', BTN_DOWN},
        {0, BTN_STICK_L}, {0, BTN_STICK_R},                      /* no fixed key for the test scripts; bindable keys only */
    };
    uint64_t buttons = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0, wired = 1;
    if (!(g_x_synth == 1 && id == 0) && !(g_x_synth == 2 && id == 1) &&
        ((int)id == g_keyboard_player || (id == 0 && scripted_run()))) {    /* keyboard, and the test scripts on player 1 */
        for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (key_for(id, map[i].vk, map[i].bit)) buttons |= 1ull << map[i].bit;
        lx = (key_for(id, VK_RIGHT, 18) - key_for(id, VK_LEFT, 16)) * STICK_MAX;
        ly = (key_for(id, VK_UP, 17) - key_for(id, VK_DOWN, 19)) * STICK_MAX;
        if (sdl_on()) { rx = (key_for(id, 0, 22) - key_for(id, 0, 20)) * STICK_MAX; ry = (key_for(id, 0, 21) - key_for(id, 0, 23)) * STICK_MAX; }
        if (nnpad_kb_movement()) kb_ssf2(id, &buttons, &lx, &ly);
    }
    int slot = g_source[id];
    if (sdl_on()) {
        NnPadState ps;
        if (nnpad_get((int)id, &ps)) {
            wired = ps.wired;
            if (devices_live() && !menu_input_blocked()) {      /* U1: not while the settings menu is open (menu.cpp) */
                buttons |= ps.buttons;
                if (ps.lx || ps.ly) { lx = ps.lx; ly = ps.ly; }
                if (ps.rx || ps.ry || !rx) { rx = ps.rx; ry = ps.ry; }
                if (kind == 2 && ps.gc) { trig_l = (unsigned)ps.trig_l; trig_r = (unsigned)ps.trig_r; }
            }
        }
    } else if (x_present(slot) && devices_live() && !menu_input_blocked()) {
        const XGamepad *g = &g_x[slot].st.pad;
        static const struct { WORD x; int pos, label; } xmap[] = {
            {0x1000, BTN_B, BTN_A}, {0x2000, BTN_A, BTN_B}, {0x4000, BTN_Y, BTN_X}, {0x8000, BTN_X, BTN_Y},
            {0x0100, BTN_L, BTN_L}, {0x0200, BTN_R, BTN_R}, {0x0010, BTN_PLUS, BTN_PLUS}, {0x0020, BTN_MINUS, BTN_MINUS},
            {0x0040, BTN_STICK_L, BTN_STICK_L}, {0x0080, BTN_STICK_R, BTN_STICK_R},
            {0x0001, BTN_UP, BTN_UP}, {0x0002, BTN_DOWN, BTN_DOWN}, {0x0004, BTN_LEFT, BTN_LEFT}, {0x0008, BTN_RIGHT, BTN_RIGHT},
        };
        for (size_t i = 0; i < sizeof xmap / sizeof xmap[0]; i++)
            if (g->buttons & xmap[i].x) buttons |= 1ull << (g_label_layout ? xmap[i].label : xmap[i].pos);
        if (g->lt >= 85) buttons |= 1ull << BTN_ZL;
        if (g->rt >= 85) buttons |= 1ull << BTN_ZR;
        if (g->lx || g->ly) { lx = g->lx < -STICK_MAX ? -STICK_MAX : g->lx; ly = g->ly < -STICK_MAX ? -STICK_MAX : g->ly; }
        rx = g->rx < -STICK_MAX ? -STICK_MAX : g->rx;
        ry = g->ry < -STICK_MAX ? -STICK_MAX : g->ry;
        wired = !g_x[slot].wireless;
    }
    if (id == 0) record_or_replay(&buttons, &lx, &ly);
    if (id == 0 && r1_on()) r1_pad(&buttons, &lx, &ly, &rx, &ry);
    if (id == 1 && r1_on()) { void r1_pad_id(int id, uint64_t *b, int *lx, int *ly, int *rx, int *ry); r1_pad_id(1, &buttons, &lx, &ly, &rx, &ry); }      /* R3: port 2 in an online rollback match */
    if (lx < -STICK_MAX / 2) buttons |= 1ull << BTN_SL_LEFT;
    if (lx > STICK_MAX / 2) buttons |= 1ull << BTN_SL_RIGHT;
    if (ly > STICK_MAX / 2) buttons |= 1ull << BTN_SL_UP;
    if (ly < -STICK_MAX / 2) buttons |= 1ull << BTN_SL_DOWN;
    if (rx < -STICK_MAX / 2) buttons |= 1ull << (BTN_SL_LEFT + 4);
    if (ry > STICK_MAX / 2) buttons |= 1ull << (BTN_SL_UP + 4);
    if (rx > STICK_MAX / 2) buttons |= 1ull << (BTN_SL_RIGHT + 4);
    if (ry < -STICK_MAX / 2) buttons |= 1ull << (BTN_SL_DOWN + 4);
    wr64(out + 8, buttons);
    if (g_test_vk && (int)id == g_keyboard_player) { InterlockedIncrement(&g_test_polls); InterlockedOr64(&g_test_seen, (LONG64)buttons); }      /* U1 test only */
    if (id == 0) { void display_l1_poll(uint64_t buttons, long long fight_ticks, long long poll); int r1_ra_phase(void); if (r1_ra_phase() != 1) display_l1_poll(buttons, l1_ticks, (long long)g_seq_polls); }      /* (R1 run-ahead: the speculative pass's poll is not a poll of the real timeline) */      /* L1 (input latency): the time of player 1's poll, the press it saw */
    wr32(out + 0x10, (uint32_t)lx);
    wr32(out + 0x14, (uint32_t)ly);
    wr32(out + 0x18, (uint32_t)rx);
    wr32(out + 0x1c, (uint32_t)ry);
    wr32(out + 0x20, wired ? 3 : 1);                          /* NpadAttribute: bit 0 connected, bit 1 wired */
    if (kind == 2) { wr32(out + 0x24, trig_l); wr32(out + 0x28, trig_r); }
}

/* Our own menus and the on-screen keyboard (osk.c): the buttons of EVERY connected pad together, in the Npad bit order
 * above, the left stick as the d-pad bits; bit 16 = a Home / Guide button, bit 17 = a Capture / Share button (U1, the
 * settings menu: those two are never part of the game's state). No side effect on the game's own polls (no sampling number, no input sequence,
 * no recording). The keyboard is not here: the menus read keys from the window (display.c). 0 in scripted runs. */
/* U1, the Controls page: the keyboard player chosen in the menu is used at once */
void nnhid_kb_player_refresh(void) { if (sdl_on()) { g_keyboard_player = nnpad_keyboard_player(); printf("nn::hid: keyboard = player %d (settings menu)\n", g_keyboard_player + 1); } }
int nnhid_kb_player(void) { hid_setup(); return g_keyboard_player; }
/* U1, the Controls page's test area: what player <id>'s controller and keyboard hold right now, as the game would get it
 * (configured bindings), without any effect on the game (no sampling number, no sequence) and also while the menu is open.
 * Pro bits 0..15; st = lx ly rx ry. */
uint64_t nnhid_preview(int id, int st[4]) {
    uint64_t b = 0;
    int lx = 0, ly = 0, rx = 0, ry = 0;
    hid_setup();
    st[0] = st[1] = st[2] = st[3] = 0;
    if (id < 0 || id > 7 || scripted_run()) return 0;
    if (id == g_keyboard_player && sdl_on() && keyboard_ours()) {
        int k[24];
        for (int i = 0; i < 24; i++) { int vk = nnpad_kb_vk(i); k[i] = vk && (GetAsyncKeyState(vk) & 0x8000) != 0; }
        for (int i = 0; i < 16; i++) if (k[i]) b |= 1ull << i;
        lx = (k[18] - k[16]) * STICK_MAX; ly = (k[17] - k[19]) * STICK_MAX; rx = (k[22] - k[20]) * STICK_MAX; ry = (k[21] - k[23]) * STICK_MAX;
    }
    if (sdl_on()) {
        NnPadState ps;
        if (nnpad_get(id, &ps)) { b |= ps.buttons; if (ps.lx || ps.ly) { lx = ps.lx; ly = ps.ly; } if (ps.rx || ps.ry) { rx = ps.rx; ry = ps.ry; } }
    }
    st[0] = lx; st[1] = ly; st[2] = rx; st[3] = ry;
    return b & 0xffff;
}

uint64_t nnhid_menu_buttons(void) {
    hid_setup();
    if (scripted_run() || !devices_live()) return 0;
    uint64_t b = 0;
    int lx = 0, ly = 0;
    for (int id = 0; id < 8; id++) {
        if (sdl_on()) {
            NnPadState ps;
            if (nnpad_get(id, &ps)) { b |= ps.buttons; if (abs(ps.lx) > abs(lx)) lx = ps.lx; if (abs(ps.ly) > abs(ly)) ly = ps.ly; }
            continue;
        }
        int slot = g_source[id];
        if (!x_present(slot)) continue;
        const XGamepad *g = &g_x[slot].st.pad;
        static const struct { WORD x; int pos, label; } xmap[] = {
            {0x1000, BTN_B, BTN_A}, {0x2000, BTN_A, BTN_B}, {0x4000, BTN_Y, BTN_X}, {0x8000, BTN_X, BTN_Y},
            {0x0100, BTN_L, BTN_L}, {0x0200, BTN_R, BTN_R}, {0x0010, BTN_PLUS, BTN_PLUS}, {0x0020, BTN_MINUS, BTN_MINUS},
            {0x0040, BTN_STICK_L, BTN_STICK_L}, {0x0080, BTN_STICK_R, BTN_STICK_R},
            {0x0001, BTN_UP, BTN_UP}, {0x0002, BTN_DOWN, BTN_DOWN}, {0x0004, BTN_LEFT, BTN_LEFT}, {0x0008, BTN_RIGHT, BTN_RIGHT},
        };
        for (size_t i = 0; i < sizeof xmap / sizeof xmap[0]; i++)
            if (g->buttons & xmap[i].x) b |= 1ull << (g_label_layout ? xmap[i].label : xmap[i].pos);
        if (g->lt >= 85) b |= 1ull << BTN_ZL;
        if (g->rt >= 85) b |= 1ull << BTN_ZR;
        if (abs(g->lx) > abs(lx)) lx = g->lx;
        if (abs(g->ly) > abs(ly)) ly = g->ly;
        { XState ex; if (x_get_state_ex && !g_x_synth && x_get_state_ex((DWORD)slot, &ex) == 0 && (ex.pad.buttons & 0x0400)) b |= 1ull << 16; }      /* U1: Guide */
    }
    if (sdl_on()) { unsigned nnpad_menu_keys(void); unsigned k = nnpad_menu_keys(); b |= (uint64_t)(k & 1) << 16 | (uint64_t)(k >> 1 & 1) << 17; }
    if (lx < -STICK_MAX / 2) b |= 1ull << BTN_LEFT;
    if (lx > STICK_MAX / 2) b |= 1ull << BTN_RIGHT;
    if (ly > STICK_MAX / 2) b |= 1ull << BTN_UP;
    if (ly < -STICK_MAX / 2) b |= 1ull << BTN_DOWN;
    return b;
}

static void hid_InitializeNpad(Ctx *c) { (void)c; hid_setup(); }
static void hid_SetSupportedNpadStyleSet(Ctx *c) { g_supported_styles = (uint32_t)A0; }             /* (NpadStyleSet by value) */
static void hid_SetSupportedNpadIdType(Ctx *c) {            /* (const NpadIdType *ids, size_t count) */
    int n = A1 > 16 ? 16 : (int)A1;
    for (int i = 0; i < n; i++) g_supported_ids[i] = rd32(A0 + (uint64_t)i * 4);
    g_supported_id_count = n;
}
/* The style a connected controller is shown in: the adapter's GameCube controllers as NpadStyleGc when the game asked for that
 * style (and controllers.ini gamecube_style is native, the default), every other controller as a Pro Controller. */
/* (R3 lwr) a Local Wireless room rollback match: ports 1 / 2 are shown in the kind (GameCube or Pro) of each player's OWN
 * controller on its own PC (sent in the room handshake, debug_mode.c), so both PCs pick the same control set of each player */
static int g_lwr_style_on, g_lwr_gc[2];
void nnhid_lwr_styles(int on, int gc0, int gc1) { g_lwr_gc[0] = gc0; g_lwr_gc[1] = gc1; g_lwr_style_on = on; }
int nnhid_native_gc0(void) { return sdl_on() && nnpad_is_gc(0) && nnpad_gc_native() && (g_supported_styles & STYLE_GC); }
static uint32_t npad_style(uint32_t id) {
    if (!player_connected(id)) return 0;
    if (g_lwr_style_on && id < 2) return g_lwr_gc[id] && (g_supported_styles & STYLE_GC) ? STYLE_GC : STYLE_FULLKEY;
    if (sdl_on() && nnpad_is_gc((int)id) && nnpad_gc_native() && (g_supported_styles & STYLE_GC)) return STYLE_GC;
    return STYLE_FULLKEY;
}
static void hid_GetNpadStyleSet(Ctx *c) {   /* (const NpadIdType &) */
    uint32_t id = rd32(A0);
    uint32_t r = npad_style(id);
    {   /* D2 debug (SSBU_HIDLOG): each distinct (id, caller, answer) once */
        static int on = -1, n; static uint64_t seen[64];
        if (on < 0) on = getenv("SSBU_HIDLOG") != NULL;
        if (on) { uint64_t k = ((uint64_t)id << 40) ^ (c->x[30] << 1) ^ (uint64_t)r; int i; for (i = 0; i < n; i++) if (seen[i] == k) break;
            if (i == n && n < 64) { seen[n++] = k; printf("hidlog: GetNpadStyleSet(id %x) from %llx -> %#x\n", id, (unsigned long long)c->x[30] - 4, r); } }
    }
    RET(r);
}
static void hid_GetNpadState_FullKey(Ctx *c) { fill_pad(A0, rd32(A1), 1); }
static void hid_GetNpadState_Gc(Ctx *c) { fill_pad(A0, rd32(A1), 2); }
static void hid_GetNpadState_Other(Ctx *c) { fill_pad(A0, rd32(A1), 0); }
static void hid_GetPlayerLedPattern(Ctx *c) {               /* the console's patterns for players 1..8 */
    static const uint8_t led[8] = {1, 3, 7, 15, 9, 5, 13, 6};
    uint32_t id = rd32(A0);
    RET(id < 8 && player_connected(id) ? led[id] : 0);
}
/* [sdk] 0x2398a0: per npad the service publishes a colour attribute (0 ok, 1 read error -> hid 603, 2 no such
 * controller -> hid 604) and two RGBA colours. A Pro Controller's own colours are body 2D2D2D, buttons E6E6E6. */
#define HID_COLOR_NO_CONTROLLER 0x4b8caull
static void hid_ControllerColor1(Ctx *c) {                  /* (NpadControllerColor *out, const NpadIdType &) */
    if (!player_connected(rd32(A1))) { RET(HID_COLOR_NO_CONTROLLER); return; }
    wr32(A0, 0xff2d2d2du);
    wr32(A0 + 4, 0xffe6e6e6u);
    RET(0);
}
static void hid_ControllerColor2(Ctx *c) { RET(HID_COLOR_NO_CONTROLLER); }   /* (left, right, id): Joy-Con colours; no Joy-Con is ever attached */
static void hid_InitializeTouchScreen(Ctx *c) { (void)c; g_touch_ready = 1; }
static void hid_GetTouchScreenState(Ctx *c) {               /* docked console: the screen is in the dock, no touch can arrive */
    memset((void *)(uintptr_t)A0, 0, 0x290);
    wr64(A0, (uint64_t)InterlockedIncrement64(&g_sampling));
}
/* Motion: XInput pads have no sensor. The state written is that of a controller lying still, face up: no rotation,
 * identity attitude, gravity on -z (NOT VERIFIED on a console: the sign convention), attribute "connected". */
/* The SDK side, read in sdk.elf:
 * [sdk] 0x250ad0 GetSixAxisSensorState -> 0x241390 -> 0x2413f0 GetSixAxisSensorStates(&count, out, 1, handle): the
 *   handle's low byte selects the reader (jump table 0xa45098): 3 (full key) and 8 read the npad's full-key ring
 *   with the npad id of byte 1; 0x23c030 copies the NEWEST record of that ring in the hid shared memory
 *   (npad entry + 0x1758: tail @+0x10, count @+0x18, 17 records of 0x68 bytes @+0x20 = sampling number + the
 *   0x60-byte state) and stores how many it copied. A ring nothing was ever written to copies NOTHING: the
 *   caller's structure is left as it was, and the public function still returns normally.
 * [sdk] 0x2505d0 StartSixAxisSensor -> 0x23b4c0: one request to the hid module (IHidServer slot +0xb8, applet
 *   resource user id + handle); any failure aborts.
 * The hid module's side (not in the dump), after [doc] suyu hid_core/resources/six_axis/six_axis.cpp and
 * resource_manager.cpp: every 5 ms it appends one record per CONNECTED controller, sampling number = previous + 1;
 * nothing is appended for an npad without a controller (its ring keeps the last record). A controller whose
 * sensor was not started, or that delivers no motion, gets the record of a sensor at rest: delta time 5 ms,
 * acceleration (0, 0, -1), no angular velocity, no angle, identity direction, attribute 1 (connected).
 * StartSixAxisSensor sets the per-controller "sensor enabled" flag; an invalid handle is an error.
 * Hardware limit (row "controllers" of the audit): XInput delivers no motion, so the record stays the one at rest. */
static struct { int ever, connected, started; ULONGLONG since; int64_t base, last; } g_six[8];
static void hid_GetSixAxisSensorState(Ctx *c) {             /* (SixAxisSensorState *out, const SixAxisSensorHandle &) */
    uint64_t o = A0;
    uint32_t handle = rd32(A1), kind = handle & 0xff, id = (handle >> 8) & 0xff;
    float f;
    hid_setup();
    if ((kind != 3 && kind != 8) || id > 7) return;          /* rings that no controller of this port ever writes */
    if (player_connected(id)) {
        ULONGLONG now = GetTickCount64();
        if (!g_six[id].connected) { g_six[id].connected = 1; g_six[id].since = now; g_six[id].base = g_six[id].last; }
        g_six[id].last = g_six[id].base + 1 + (int64_t)((now - g_six[id].since) / 5);
        g_six[id].ever = 1;
    } else g_six[id].connected = 0;
    if (!g_six[id].ever) return;                             /* empty ring: nothing is copied */
    { void r1_wrote(uint64_t, uint64_t); r1_wrote(o, 0x60); }   /* R1 (r1_state.c): the page is marked (nothing without SSBU_R1_MARK) */
    memset((void *)(uintptr_t)o, 0, 0x60);
    wr64(o, 5000000);                                        /* delta time, ns */
    wr64(o + 8, (uint64_t)g_six[id].last);                   /* sampling number */
    {   NnPadState ps;                                       /* a pad with a gyro (SDL): its acceleration (G) at +0x10 and angular velocity (revolutions/s) at +0x1c; angle and direction stay at rest */
        if (sdl_on() && nnpad_get((int)id, &ps) && ps.has_motion) {
            memcpy((void *)(uintptr_t)(o + 0x10), ps.accel, 12); memcpy((void *)(uintptr_t)(o + 0x1c), ps.gyro, 12);
        } else { f = -1.0f; memcpy((void *)(uintptr_t)(o + 0x18), &f, 4); }          /* acceleration z at rest */
    }
    f = 1.0f;
    memcpy((void *)(uintptr_t)(o + 0x34), &f, 4); memcpy((void *)(uintptr_t)(o + 0x44), &f, 4); memcpy((void *)(uintptr_t)(o + 0x54), &f, 4);   /* direction = identity */
    wr32(o + 0x58, 1);                                                             /* attribute: connected */
}
static void hid_StartSixAxisSensor(Ctx *c) {                /* (const SixAxisSensorHandle &) */
    uint32_t handle = rd32(A0), kind = handle & 0xff, id = (handle >> 8) & 0xff, dev = (handle >> 16) & 0xff;
    hid_setup();
    /* [doc] suyu hid_core/hid_util.h IsSixaxisHandleValid: npad id 0..7 / 0x10 / 0x20, device index below 3 */
    (void)kind;
    if (!(id < 8 || id == 0x10 || id == 0x20) || dev > 2)
        guest_trap("nn::hid::StartSixAxisSensor with an invalid handle (the hid module refuses it and the SDK aborts, sdk 0x2505f4)", c->x[30] - 4);
    if (id < 8) g_six[id].started = 1;
}
int nnhid_selftest_close4(Ctx *c);
int nnhid_selftest(Ctx *c) {
    uint8_t st[0x60];
    uint32_t handle = 3 | 6 << 8;                            /* full key, npad 7 (player 7: no pad in a test), device 0 */
    uint64_t lr = c->x[30];
    int ok = 1;
    memset(st, 0xa5, sizeof st);
    c->x[0] = (uint64_t)(uintptr_t)&handle; hid_StartSixAxisSensor(c);
    c->x[0] = (uint64_t)(uintptr_t)st; c->x[1] = (uint64_t)(uintptr_t)&handle; hid_GetSixAxisSensorState(c);
    if (!player_connected(6)) { for (int i = 0; i < 0x60; i++) ok &= st[i] == 0xa5; }
    g_six[6].ever = 1; g_six[6].last = 41;                   /* a controller that was connected and left */
    hid_GetSixAxisSensorState(c);
    if (!player_connected(6)) ok &= rd64((uint64_t)(uintptr_t)st + 8) == 41;
    ok &= rd64((uint64_t)(uintptr_t)st) == 5000000 && rd32((uint64_t)(uintptr_t)st + 0x58) == 1 && rd32((uint64_t)(uintptr_t)st + 0x18) == 0xbf800000u && rd32((uint64_t)(uintptr_t)st + 0x34) == 0x3f800000u;
    memset(&g_six[6], 0, sizeof g_six[6]);
    c->x[30] = lr;
    printf("  hid: six-axis state: empty ring copies nothing, last record kept after disconnect, record at rest: %s\n", ok ? "ok" : "FAILED");
    { int nnhid_selftest_xinput(Ctx *c); return !ok + nnhid_selftest_close4(c) + nnhid_selftest_xinput(c); }
}
/* IsSixAxisSensorAtRest(const SixAxisSensorHandle &) [sdk 0x250b80 -> 0x23beb0]: one request to the hid module (IHidServer
 * slot +0x138 = command 82, [doc] suyu hid_server.cpp) with the handle; a failure aborts (0x250b9c), else the module's bool
 * is returned.  The module ([doc] suyu hid_core/resources/six_axis/six_axis.cpp IsSixAxisSensorAtRest + OnUpdate line 80):
 * an invalid handle (IsSixaxisHandleValid) is an error; otherwise the controller's "at rest" flag, which starts true and
 * is recomputed from the sensor only while it is started and delivers motion.  XInput pads deliver none (see
 * GetSixAxisSensorState above: the record at rest), so the flag keeps its initial value: true. */
static int g_seven_ready;                                   /* InitializeSevenSixAxisSensor was called */
static int g_gc_test_mounted;                               /* self-test only: pretend npad 7 holds a GameCube controller */
static void hid_InitializeSevenSixAxisSensor(Ctx *c);
static void hid_IsSixAxisSensorAtRest(Ctx *c) {
    uint32_t handle = rd32(A0), id = (handle >> 8) & 0xff, dev = (handle >> 16) & 0xff;
    hid_setup();
    if (!(id < 8 || id == 0x10 || id == 0x20) || dev > 2)
        guest_trap("nn::hid::IsSixAxisSensorAtRest with an invalid handle (the hid module refuses it and the SDK aborts, sdk 0x250b9c)", c->x[30] - 4);
    {   NnPadState ps;                                       /* a pad with a gyro: at rest while it turns slower than ~7 degrees/s (NOT VERIFIED: the module's threshold) */
        if (sdl_on() && id < 8 && nnpad_get((int)id, &ps) && ps.has_motion) { RET(fabsf(ps.gyro[0]) + fabsf(ps.gyro[1]) + fabsf(ps.gyro[2]) < 0.02f); return; }
    }
    RET(1);
}
/* IsSevenSixAxisSensorAtRest() [sdk 0x24a380 -> 0x232b30]: NO request: the SDK reads the byte at +8 of the console
 * six-axis block of the hid shared memory (accessor object 0xc8a080, mapped address 0xc8a0b8).  That address is only set
 * by InitializeSevenSixAxisSensor (0x2325f0 Activate); without it the detail function returns 0x872ca and the public one
 * aborts (0x24a398).  The byte is written by the hid module ([doc] suyu hid_core/resources/six_axis/console_six_axis.cpp:
 * is_seven_six_axis_sensor_at_rest = the console sensor's "at rest"; shared_memory_format.h: u64 sampling number, then
 * this bool).  The console modelled sits in its dock and does not move: at rest. */
static void hid_IsSevenSixAxisSensorAtRest(Ctx *c) {
    if (!g_seven_ready)
        guest_trap("nn::hid::IsSevenSixAxisSensorAtRest before InitializeSevenSixAxisSensor (the SDK aborts with 0x872ca, sdk 0x232be4 / 0x24a398)", c->x[30] - 4);
    RET(1);
}
static void hid_GetSevenSixAxisSensorStates(Ctx *c) { RET(0); }                           /* (states, count) -> number written: the console's own sensor is not sampled while docked */

/* ---- vibration. Handle = style index (3 = full key) | npad id << 8 | device index << 16. */
static int vib_slot(uint32_t handle, uint32_t *id, uint32_t *dev) {
    *id = (handle >> 8) & 0xff; *dev = (handle >> 16) & 0xff;
    return (handle & 0xff) == 3 && *id < 8 && *dev < 2;
}
static void vib_apply(uint32_t id) {
    int slot = g_source[id];
    if (sdl_on()) {                                         /* SDL: the low band to the low-frequency (large) motor, the high band to the high-frequency one */
        float lo = 0, hi = 0;
        for (int d = 0; d < 2; d++) if (g_vib[id][d].set) { if (g_vib[id][d].v[0] > lo) lo = g_vib[id][d].v[0]; if (g_vib[id][d].v[2] > hi) hi = g_vib[id][d].v[2]; }
        if (!devices_live()) lo = hi = 0;
        nnpad_rumble((int)id, lo, hi);
        return;
    }
    if (!x_set_state || !x_present(slot)) return;
    float low = 0, high = 0;
    for (int d = 0; d < 2; d++) if (g_vib[id][d].set) { if (g_vib[id][d].v[0] > low) low = g_vib[id][d].v[0]; if (g_vib[id][d].v[2] > high) high = g_vib[id][d].v[2]; }
    if (!devices_live()) low = high = 0;                    /* no rumble for a window in the background */
    XVibration v;
    v.left = (WORD)(low >= 1.0f ? 65535 : low <= 0 ? 0 : (int)(low * 65535.0f));
    v.right = (WORD)(high >= 1.0f ? 65535 : high <= 0 ? 0 : (int)(high * 65535.0f));
    if (v.left == g_x[slot].sent.left && v.right == g_x[slot].sent.right) return;
    g_x[slot].sent = v;
    x_set_state((DWORD)slot, &v);
}
static void hid_InitializeVibrationDevice(Ctx *c) {         /* (const VibrationDeviceHandle &) */
    uint32_t id, dev;
    hid_setup();
    uint32_t handle = rd32(A0);
    if (vib_slot(handle, &id, &dev)) { memcpy(g_vib[id][dev].v, g_vib_default, sizeof g_vib_default); g_vib[id][dev].set = 1; }
}
static void hid_GetVibrationDeviceInfo(Ctx *c) {            /* (VibrationDeviceInfo *out, handle): { type, position } */
    uint32_t id, dev;
    uint32_t handle = rd32(A1);
    uint32_t style = handle & 0xff;
    uint32_t gid = (handle >> 8) & 0xff;
    int gc_mounted = style == 8 && gid < 8 && ((gid == 7 && g_gc_test_mounted) || (sdl_on() && nnpad_is_gc((int)gid)));
    if (gc_mounted) {
        wr32(A0, 2);                                        /* VibrationDeviceType_GcErm: the GameCube controller's eccentric motor */
        wr32(A0 + 4, (handle >> 16 & 0xff) == 0 ? 1 : 2);
        return;
    }
    if (!vib_slot(handle, &id, &dev)) { wr32(A0, 0); wr32(A0 + 4, 0); return; }
    wr32(A0, 1);                                            /* VibrationDeviceType_LinearResonantActuator */
    wr32(A0 + 4, dev == 0 ? 1 : 2);                         /* VibrationDevicePosition_Left / _Right */
}
static void hid_SendVibrationValue(Ctx *c) {                /* (handle, const VibrationValue &{ampLow, freqLow, ampHigh, freqHigh}) */
    uint32_t id, dev;
    { int r1_ra_phase(void); if (r1_ra_phase() == 1) return; }      /* R1 round 5, run-ahead: a speculative pass's rumble is not felt (the real pass sends it) */
    uint32_t handle = rd32(A0);
    if (!vib_slot(handle, &id, &dev)) return;
    memcpy(g_vib[id][dev].v, (void *)(uintptr_t)A1, 16);
    g_vib[id][dev].set = 1;
    vib_apply(id);
}
static void hid_GetActualVibrationValue(Ctx *c) {           /* (VibrationValue *out, handle): what the actuator is playing */
    uint32_t id, dev;
    const float *v = g_vib_default;
    if (vib_slot(rd32(A1), &id, &dev) && g_vib[id][dev].set && player_connected(id)) v = g_vib[id][dev].v;
    memcpy((void *)(uintptr_t)A0, v, 16);
}
/* GameCube controllers: an eccentric motor with the commands 0 Stop, 1 Start, 2 StopHard (u64; [game] 0x710366a4f0 reads
 * 8 bytes and compares with 1).
 * SendVibrationGcErmCommand(handle, command) [sdk 0x2537c0 -> 0x248ca0]: one request (IHidServer slot +0x258 = command 207)
 * with the u32 handle and the u64 command; a failure aborts.  GetActualVibrationGcErmCommand(out, handle) [sdk 0x253810 ->
 * 0x248d30]: slot +0x260 = command 208, the module writes the u64; a failure aborts.
 * The module ([doc] suyu core/hle/service/hid/hid_server.cpp 987 / 1013, hid_core/hid_util.h IsVibrationHandleValid,
 * hid_core/resources/vibration/gc_vibration_device.cpp, hid_core/frontend/emulated_controller.cpp 1236): the handle must
 * be valid (style 3..8, 13, 32, 33; npad id 0..7, 0x10, 0x20; device index < 3), else an error; only a handle of the
 * GameCube style (8) has a GameCube device, and only a mounted one (a GameCube controller on that npad) does anything:
 * Start runs the motor at full amplitude, Stop / StopHard stop it, a command above 2 is ignored.  Without a mounted
 * device Send does nothing and GetActual answers Stop.
 * Mounted devices are the controllers of the GameCube adapter (nnpad_gca.c: Start turns the adapter's motor on through the pad's
 * rumble); on an npad without one that is the path taken. The XInput code below is what a self-test's mounted device does. */
static uint64_t g_gc_erm[8];                                /* the command a mounted device is executing */
static int gc_handle_check(Ctx *c, uint32_t handle, const char *what) {      /* 1 = the handle names a mounted GameCube device */
    uint32_t style = handle & 0xff, id = (handle >> 8) & 0xff, dev = (handle >> 16) & 0xff;
    if (!((style >= 3 && style <= 8) || style == 13 || style == 32 || style == 33) || !(id < 8 || id == 0x10 || id == 0x20) || dev > 2)
        guest_trap(what, c->x[30] - 4);
    return style == 8 && id < 8 && ((id == 7 && g_gc_test_mounted) || (sdl_on() && nnpad_is_gc((int)id)));      /* a mounted device: a controller of the GameCube adapter (or the self-test's) */
}
static void hid_SendVibrationGcErmCommand(Ctx *c) {         /* (const VibrationDeviceHandle &, const VibrationGcErmCommand &) */
    uint32_t handle = rd32(A0), id = (handle >> 8) & 0xff;
    uint64_t command = rd64(A1);
    { int r1_ra_phase(void); if (r1_ra_phase() == 1) return; }      /* R1 round 5, run-ahead: not in a speculative pass */
    hid_setup();
    int mounted = gc_handle_check(c, handle, "nn::hid::SendVibrationGcErmCommand with an invalid handle (the hid module refuses it and the SDK aborts, sdk 0x2537d4)");
    if (!mounted || command > 2) return;
    g_gc_erm[id] = command;
    int slot = g_source[id];
    if (sdl_on() && nnpad_is_gc((int)id)) {                  /* the adapter's rumble motor: Start = on, Stop / StopHard = off */
        float v = command == 1 && devices_live() ? 1.0f : 0.0f;
        nnpad_rumble((int)id, v, v);
        return;
    }
    if (x_set_state && x_present(slot)) {
        XVibration v;
        v.left = v.right = (WORD)(command == 1 && devices_live() ? 65535 : 0);
        g_x[slot].sent = v;
        x_set_state((DWORD)slot, &v);
    }
}
static void hid_GetActualVibrationGcErmCommand(Ctx *c) {    /* (VibrationGcErmCommand *out, const VibrationDeviceHandle &) */
    uint32_t handle = rd32(A1), id = (handle >> 8) & 0xff;
    uint64_t out = A0;
    hid_setup();
    int mounted = gc_handle_check(c, handle, "nn::hid::GetActualVibrationGcErmCommand with an invalid handle (the hid module refuses it and the SDK aborts, sdk 0x253840)");
    wr64(out, mounted ? g_gc_erm[id] : 0);
}
int nnhid_selftest_close4(Ctx *c) {
    uint32_t pro = 3 | 6 << 8, gc = 8 | 7 << 8, gc_other = 8 | 2 << 8, six = 3 | 6 << 8 | 1 << 16;
    uint64_t cmd = 1, got = 0x1111111111111111ull, lr = c->x[30];
    int bad = 0, ok, was_ready = g_seven_ready;
    c->x[0] = (uint64_t)(uintptr_t)&six; hid_IsSixAxisSensorAtRest(c);
    ok = c->x[0] == 1;
    g_seven_ready = 0;
    c->x[0] = 0; c->x[1] = 0x80000; hid_InitializeSevenSixAxisSensor(c);
    hid_IsSevenSixAxisSensorAtRest(c);
    ok = ok && c->x[0] == 1 && g_seven_ready;
    printf("  hid: IsSixAxisSensorAtRest (valid handle) and IsSevenSixAxisSensorAtRest after Initialize: at rest: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    c->x[0] = (uint64_t)(uintptr_t)&pro; c->x[1] = (uint64_t)(uintptr_t)&cmd; hid_SendVibrationGcErmCommand(c);
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = (uint64_t)(uintptr_t)&pro; hid_GetActualVibrationGcErmCommand(c);
    ok = got == 0;
    got = 0x1111111111111111ull;
    c->x[0] = (uint64_t)(uintptr_t)&gc_other; c->x[1] = (uint64_t)(uintptr_t)&cmd; hid_SendVibrationGcErmCommand(c);
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = (uint64_t)(uintptr_t)&gc_other; hid_GetActualVibrationGcErmCommand(c);
    ok = ok && got == 0 && g_gc_erm[2] == 0;
    printf("  hid: GC ERM Start to a Pro Controller handle and to a GameCube handle without controller: nothing, actual command Stop (8 bytes): %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    g_gc_test_mounted = 1;
    c->x[0] = (uint64_t)(uintptr_t)&gc; c->x[1] = (uint64_t)(uintptr_t)&cmd; hid_SendVibrationGcErmCommand(c);
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = (uint64_t)(uintptr_t)&gc; hid_GetActualVibrationGcErmCommand(c);
    ok = got == 1;
    cmd = 7; c->x[0] = (uint64_t)(uintptr_t)&gc; c->x[1] = (uint64_t)(uintptr_t)&cmd; hid_SendVibrationGcErmCommand(c);
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = (uint64_t)(uintptr_t)&gc; hid_GetActualVibrationGcErmCommand(c);
    ok = ok && got == 1;
    cmd = 2; c->x[0] = (uint64_t)(uintptr_t)&gc; c->x[1] = (uint64_t)(uintptr_t)&cmd; hid_SendVibrationGcErmCommand(c);
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = (uint64_t)(uintptr_t)&gc; hid_GetActualVibrationGcErmCommand(c);
    ok = ok && got == 2;
    g_gc_test_mounted = 0; g_gc_erm[7] = 0;
    c->x[0] = (uint64_t)(uintptr_t)&got; c->x[1] = (uint64_t)(uintptr_t)&gc; hid_GetActualVibrationGcErmCommand(c);
    ok = ok && got == 0;
    printf("  hid: GC ERM on a (test-only) mounted GameCube device: Start kept, command 7 ignored, StopHard kept, unmounted -> Stop: %s\n", ok ? "ok" : "FAILED");
    bad += !ok;
    g_seven_ready = was_ready;
    c->x[30] = lr;
    return bad;
}

/* ---- Joy-Con management: there are no Joy-Con here, every controller is a Pro Controller, for which the console
 * itself does nothing in these calls (they regroup or split Joy-Con pairs). */
static void hid_joycon_only(Ctx *c) { hid_setup(); (void)c; }
static void hid_GetNpadJoyAssignment(Ctx *c) { RET(0); }    /* NpadJoyAssignmentMode_Dual, the value of every non-Joy-Con npad */
static void hid_MergeSingleJoyAsDualJoy(Ctx *c) { RET(0x4b2caull); }   /* hid 601..602 family: the two npads are not single Joy-Con (NOT VERIFIED: the exact code for "not a Joy-Con") */
static void hid_SwapNpadAssignment(Ctx *c) {                /* (const NpadIdType &a, const NpadIdType &b): the two players exchange controllers */
    uint32_t a = rd32(A0), b = rd32(A1);
    if (a > 7 || b > 7) return;
    int t = g_source[a]; g_source[a] = g_source[b]; g_source[b] = t;
    if (sdl_on()) nnpad_swap((int)a, (int)b);
    if (g_keyboard_player == (int)a) g_keyboard_player = (int)b; else if (g_keyboard_player == (int)b) g_keyboard_player = (int)a;
    for (int d = 0; d < 2; d++) { float tmp[4]; int ts = g_vib[a][d].set; memcpy(tmp, g_vib[a][d].v, 16); g_vib[a][d] = g_vib[b][d]; memcpy(g_vib[b][d].v, tmp, 16); g_vib[b][d].set = ts; }
}
static void hid_DisconnectNpad(Ctx *c) {                    /* drops the wireless link; the pad returns when a button is pressed */
    uint32_t id = rd32(A0);
    if (id > 7) return;
    int slot = g_source[id];
    if (sdl_on()) { nnpad_disconnect((int)id); return; }
    if (slot >= 0 && slot < 4 && g_x[slot].present) {
        XVibration off = {0, 0};
        if (x_set_state) x_set_state((DWORD)slot, &off);
        g_x[slot].sent = off;
        g_x[slot].dropped = 1;
    }
}
static void hid_InitializeSevenSixAxisSensor(Ctx *c) { (void)c; hid_setup(); g_seven_ready = 1; }       /* (void *work, size_t) [sdk 0x232540] */
static void hid_seven_start_stop(Ctx *c) { (void)c; hid_setup(); }

/* Self-test of the XInput pad path with a synthetic pad on slot 1 = player 2 (see "synthetic XInput pad" above): the
 * state is set here, the three XInput entry points are the synthetic ones, and the handlers the game calls
 * (GetNpadStyleSet, GetNpadState, vibration, DisconnectNpad) are called as the game calls them. The two time gates of
 * x_present (a connected pad is read at most once per millisecond, a missing one probed once per second) are reset
 * between cases; they are caches, not part of the mapping. No physical pad is involved. */
static uint64_t xt_buttons, xt_attr;
static int32_t xt_lx, xt_ly, xt_rx, xt_ry;
static uint32_t xt_style;
static void xt_poll(Ctx *c) {
    static uint8_t st[0x28];
    static uint32_t id;
    id = 1;
    g_x[1].read = 0; g_x[1].checked = 0;
    c->x[0] = (uint64_t)(uintptr_t)&id; hid_GetNpadStyleSet(c); xt_style = (uint32_t)c->x[0];
    g_x[1].read = 0; g_x[1].checked = 0;
    c->x[0] = (uint64_t)(uintptr_t)st; c->x[1] = (uint64_t)(uintptr_t)&id; hid_GetNpadState_FullKey(c);
    xt_buttons = rd64((uint64_t)(uintptr_t)st + 8);
    xt_lx = (int32_t)rd32((uint64_t)(uintptr_t)st + 0x10); xt_ly = (int32_t)rd32((uint64_t)(uintptr_t)st + 0x14);
    xt_rx = (int32_t)rd32((uint64_t)(uintptr_t)st + 0x18); xt_ry = (int32_t)rd32((uint64_t)(uintptr_t)st + 0x1c);
    xt_attr = rd32((uint64_t)(uintptr_t)st + 0x20);
}
int nnhid_selftest_xinput(Ctx *c) {
    DWORD (WINAPI *save_get)(DWORD, XState *) = x_get_state;
    DWORD (WINAPI *save_set)(DWORD, XVibration *) = x_set_state;
    DWORD (WINAPI *save_caps)(DWORD, DWORD, XCaps *) = x_get_caps;
    int save_synth = g_x_synth, save_layout = g_label_layout, save_ready = g_hid_ready, save_count = g_supported_id_count, bad = 0, ok;
    uint32_t save_styles = g_supported_styles;
    uint64_t lr = c->x[30];
    if (save_synth == 1) return 0;                            /* a synthetic-pad run owns the boundary */
    g_hid_ready = 1; g_x_synth = 2; g_supported_styles = 0xffffffffu; g_supported_id_count = -1;
    x_get_state = synth_get_state; x_set_state = synth_set_state; x_get_caps = synth_get_caps;
    memset(&g_xs[1], 0, sizeof g_xs[1]); memset(&g_x[1], 0, sizeof g_x[1]); memset(g_vib[1], 0, sizeof g_vib[1]);
    XGamepad *g = &g_xs[1].st.pad;

    /* 1. no pad: no style, an all-zero state */
    xt_poll(c);
    ok = xt_style == 0 && xt_buttons == 0 && xt_attr == 0 && xt_lx == 0 && xt_ly == 0;
    printf("  hid/XInput: empty slot -> style set 0, state all zero: %s\n", ok ? "ok" : "FAILED"); bad += !ok;

    /* 2. connect, wired and wireless */
    g_xs[1].connected = 1; g_xs[1].wireless = 0;
    xt_poll(c);
    ok = xt_style == STYLE_FULLKEY && xt_attr == 3 && xt_buttons == 0;
    g_xs[1].connected = 0; xt_poll(c); ok = ok && xt_style == 0;                 /* unplugged: GetState fails -> gone at once */
    g_xs[1].connected = 1; g_xs[1].wireless = 1; xt_poll(c);
    ok = ok && xt_style == STYLE_FULLKEY && xt_attr == 1;
    g_xs[1].connected = 0; xt_poll(c); g_xs[1].connected = 1; g_xs[1].wireless = 0; xt_poll(c);
    ok = ok && xt_attr == 3;
    printf("  hid/XInput: connect (wired: attribute 3), unplug (style 0), reconnect wireless (attribute 1), wired again: %s\n", ok ? "ok" : "FAILED"); bad += !ok;

    /* 3. the 14 digital buttons, by position and by label */
    static const struct { WORD x; int pos, label; const char *name; } t[] = {
        {0x1000, BTN_B, BTN_A, "A"}, {0x2000, BTN_A, BTN_B, "B"}, {0x4000, BTN_Y, BTN_X, "X"}, {0x8000, BTN_X, BTN_Y, "Y"},
        {0x0100, BTN_L, BTN_L, "LB"}, {0x0200, BTN_R, BTN_R, "RB"}, {0x0010, BTN_PLUS, BTN_PLUS, "Start"}, {0x0020, BTN_MINUS, BTN_MINUS, "Back"},
        {0x0040, BTN_STICK_L, BTN_STICK_L, "LS"}, {0x0080, BTN_STICK_R, BTN_STICK_R, "RS"},
        {0x0001, BTN_UP, BTN_UP, "up"}, {0x0002, BTN_DOWN, BTN_DOWN, "down"}, {0x0004, BTN_LEFT, BTN_LEFT, "left"}, {0x0008, BTN_RIGHT, BTN_RIGHT, "right"},
    };
    ok = 1;
    for (int layout = 0; layout < 2; layout++) {
        g_label_layout = layout;
        for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
            memset(g, 0, sizeof *g); g->buttons = t[i].x;
            xt_poll(c);
            int want = layout ? t[i].label : t[i].pos;
            if (xt_buttons != 1ull << want) { ok = 0; printf("    XInput %s (layout %s): buttons %llx, expected bit %d\n", t[i].name, layout ? "label" : "position", (unsigned long long)xt_buttons, want); }
        }
    }
    g_label_layout = 0;
    memset(g, 0, sizeof *g); g->buttons = 0xf3ff; xt_poll(c);                    /* every defined XInput button at once */
    ok = ok && xt_buttons == 0xfcffull;                                          /* npad bits 0..7 and 10..15 */
    printf("  hid/XInput: 14 buttons one by one, by position and by label, and all together: %s\n", ok ? "ok" : "FAILED"); bad += !ok;

    /* 4. triggers: ZL / ZR from a third of the travel (85 of 255) */
    memset(g, 0, sizeof *g); g->lt = 84; g->rt = 84; xt_poll(c); ok = xt_buttons == 0;
    g->lt = 85; xt_poll(c); ok = ok && xt_buttons == 1ull << BTN_ZL;
    g->lt = 0; g->rt = 85; xt_poll(c); ok = ok && xt_buttons == 1ull << BTN_ZR;
    g->lt = 255; g->rt = 255; xt_poll(c); ok = ok && xt_buttons == (1ull << BTN_ZL | 1ull << BTN_ZR);
    printf("  hid/XInput: triggers 84 -> nothing, 85 -> ZL / ZR, 255 both: %s\n", ok ? "ok" : "FAILED"); bad += !ok;

    /* 5. sticks: values passed through, -32768 clamped, stick-as-button bits beyond half deflection */
    memset(g, 0, sizeof *g); g->lx = 32767; g->ly = -32768; g->rx = -32768; g->ry = 32767; xt_poll(c);
    ok = xt_lx == 32767 && xt_ly == -32767 && xt_rx == -32767 && xt_ry == 32767 &&
         xt_buttons == (1ull << BTN_SL_RIGHT | 1ull << BTN_SL_DOWN | 1ull << (BTN_SL_LEFT + 4) | 1ull << (BTN_SL_UP + 4));
    memset(g, 0, sizeof *g); g->lx = -12345; g->ly = 4321; g->rx = 777; g->ry = -9999; xt_poll(c);
    ok = ok && xt_lx == -12345 && xt_ly == 4321 && xt_rx == 777 && xt_ry == -9999 && xt_buttons == 0;
    memset(g, 0, sizeof *g); g->lx = 16383; g->ry = -16383; xt_poll(c); ok = ok && xt_buttons == 0;
    g->lx = 16384; g->ry = -16384; xt_poll(c); ok = ok && xt_buttons == (1ull << BTN_SL_RIGHT | 1ull << (BTN_SL_DOWN + 4));
    printf("  hid/XInput: sticks (full, clamp of -32768, arbitrary values, stick-as-button threshold 16383 / 16384): %s\n", ok ? "ok" : "FAILED"); bad += !ok;

    /* 6. vibration: low band -> left (heavy) motor, high band -> right (light) motor, the larger of the two handles */
    uint32_t hl = 3 | 1 << 8, hr = 3 | 1 << 8 | 1 << 16;
    float v1[4] = {0.5f, 160.0f, 0.25f, 320.0f}, v2[4] = {0.1f, 160.0f, 1.5f, 320.0f}, v0[4] = {0.0f, 160.0f, 0.0f, 320.0f}, got[4];
    memset(g, 0, sizeof *g); xt_poll(c);
    c->x[0] = (uint64_t)(uintptr_t)&hl; hid_InitializeVibrationDevice(c);
    c->x[0] = (uint64_t)(uintptr_t)&hr; hid_InitializeVibrationDevice(c);
    long long sets0 = g_xs[1].sets;
    c->x[0] = (uint64_t)(uintptr_t)&hl; c->x[1] = (uint64_t)(uintptr_t)v1; hid_SendVibrationValue(c);
    ok = g_xs[1].sets == sets0 + 1 && g_xs[1].last.left == 32767 && g_xs[1].last.right == 16383;
    c->x[0] = (uint64_t)(uintptr_t)&hl; c->x[1] = (uint64_t)(uintptr_t)v1; hid_SendVibrationValue(c);
    ok = ok && g_xs[1].sets == sets0 + 1;                                        /* unchanged value: no second call to the device */
    c->x[0] = (uint64_t)(uintptr_t)&hr; c->x[1] = (uint64_t)(uintptr_t)v2; hid_SendVibrationValue(c);
    ok = ok && g_xs[1].sets == sets0 + 2 && g_xs[1].last.left == 32767 && g_xs[1].last.right == 65535;
    c->x[0] = (uint64_t)(uintptr_t)got; c->x[1] = (uint64_t)(uintptr_t)&hr; hid_GetActualVibrationValue(c);
    ok = ok && got[0] == 0.1f && got[2] == 1.5f;
    c->x[0] = (uint64_t)(uintptr_t)&hl; c->x[1] = (uint64_t)(uintptr_t)v0; hid_SendVibrationValue(c);
    c->x[0] = (uint64_t)(uintptr_t)&hr; c->x[1] = (uint64_t)(uintptr_t)v0; hid_SendVibrationValue(c);
    ok = ok && g_xs[1].last.left == 0 && g_xs[1].last.right == 0;
    printf("  hid/XInput: vibration values reach XInputSetState (0.5 -> 32767, 0.25 -> 16383, 1.5 -> 65535, larger handle wins, repeat not resent, stop -> 0): %s\n", ok ? "ok" : "FAILED"); bad += !ok;

    /* 7. DisconnectNpad: motors off, the pad is gone until one of its buttons is pressed */
    c->x[0] = (uint64_t)(uintptr_t)&hl; c->x[1] = (uint64_t)(uintptr_t)v1; hid_SendVibrationValue(c);
    uint32_t id1 = 1;
    c->x[0] = (uint64_t)(uintptr_t)&id1; hid_DisconnectNpad(c);
    ok = g_xs[1].last.left == 0 && g_xs[1].last.right == 0;
    xt_poll(c); ok = ok && xt_style == 0;
    g->buttons = 0x1000; xt_poll(c); ok = ok && xt_style == STYLE_FULLKEY && xt_buttons == 1ull << BTN_B;
    printf("  hid/XInput: DisconnectNpad stops the motors and drops the pad; it returns on a button press: %s\n", ok ? "ok" : "FAILED"); bad += !ok;

    memset(&g_xs[1], 0, sizeof g_xs[1]); memset(&g_x[1], 0, sizeof g_x[1]); memset(g_vib[1], 0, sizeof g_vib[1]);
    x_get_state = save_get; x_set_state = save_set; x_get_caps = save_caps;
    g_x_synth = save_synth; g_label_layout = save_layout; g_hid_ready = save_ready; g_supported_styles = save_styles; g_supported_id_count = save_count;
    c->x[30] = lr;
    return bad;
}

/* ---- self-test of the SDL pad backend (SSBU_PAD_SELFTEST=1, run from boot.c main before anything else): nnpad_sdl.c drives SDL virtual
 * joysticks and reads the result through these hooks, which call the handlers exactly as the game does. */
static uint32_t th_style(uint32_t id) { Ctx c; memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)&id; hid_GetNpadStyleSet(&c); return (uint32_t)c.x[0]; }
static void th_state(uint32_t id, uint64_t *b, int32_t st[4], uint32_t *attr) {
    uint8_t buf[0x28];
    Ctx c;
    memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)buf; c.x[1] = (uint64_t)(uintptr_t)&id;
    hid_GetNpadState_FullKey(&c);
    *b = rd64((uint64_t)(uintptr_t)buf + 8);
    for (int i = 0; i < 4; i++) st[i] = (int32_t)rd32((uint64_t)(uintptr_t)buf + 0x10 + 4 * (uint64_t)i);
    *attr = rd32((uint64_t)(uintptr_t)buf + 0x20);
}
static void th_vibrate(uint32_t id, float low, float high) {
    uint32_t hl = 3 | id << 8, hr = 3 | id << 8 | 1 << 16;
    float v[4] = {low, 160.0f, high, 320.0f}, z[4] = {0, 160.0f, 0, 320.0f};
    Ctx c;
    memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)&hl; hid_InitializeVibrationDevice(&c);
    c.x[0] = (uint64_t)(uintptr_t)&hr; hid_InitializeVibrationDevice(&c);
    c.x[0] = (uint64_t)(uintptr_t)&hl; c.x[1] = (uint64_t)(uintptr_t)v; hid_SendVibrationValue(&c);
    c.x[0] = (uint64_t)(uintptr_t)&hr; c.x[1] = (uint64_t)(uintptr_t)z; hid_SendVibrationValue(&c);
}
static int th_six(uint32_t id, float accel[3], float gyro[3], int *rest) {
    uint32_t handle = 3 | id << 8;
    uint8_t buf[0x60];
    Ctx c;
    memset(&c, 0, sizeof c); memset(buf, 0, sizeof buf);
    c.x[0] = (uint64_t)(uintptr_t)&handle; hid_StartSixAxisSensor(&c);
    c.x[0] = (uint64_t)(uintptr_t)buf; c.x[1] = (uint64_t)(uintptr_t)&handle; hid_GetSixAxisSensorState(&c);
    memcpy(accel, buf + 0x10, 12); memcpy(gyro, buf + 0x1c, 12);
    c.x[0] = (uint64_t)(uintptr_t)&handle; hid_IsSixAxisSensorAtRest(&c);
    *rest = (int)c.x[0];
    return rd32((uint64_t)(uintptr_t)buf + 0x58) == 1;
}
static void th_test_mode(int on) { g_pad_test = on; if (on) { _putenv("SSBU_REC=NUL"); hid_setup(); } }
static uint32_t th_led(uint32_t id) { Ctx c; memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)&id; hid_GetPlayerLedPattern(&c); return (uint32_t)c.x[0]; }
static void th_gc_state(uint32_t id, uint64_t *b, uint32_t trig[2], uint32_t *attr) {
    uint8_t buf[0x30];
    Ctx c;
    memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)buf; c.x[1] = (uint64_t)(uintptr_t)&id;
    hid_GetNpadState_Gc(&c);
    *b = rd64((uint64_t)(uintptr_t)buf + 8);
    trig[0] = rd32((uint64_t)(uintptr_t)buf + 0x24); trig[1] = rd32((uint64_t)(uintptr_t)buf + 0x28);
    *attr = rd32((uint64_t)(uintptr_t)buf + 0x20);
}
static void th_gc_erm(uint32_t id, uint64_t command) {
    uint32_t handle = 8 | id << 8;
    Ctx c;
    memset(&c, 0, sizeof c); c.x[0] = (uint64_t)(uintptr_t)&handle; c.x[1] = (uint64_t)(uintptr_t)&command;
    hid_SendVibrationGcErmCommand(&c);
}
int nnhid_pad_selftest(void) {
    NnHidHooks h = {th_style, th_state, th_vibrate, th_six, th_test_mode, th_led, th_gc_state, th_gc_erm};
    return nnpad_selftest(&h);
}

#define H(n) "_ZN2nn3hid" n
static const ImportDef hid_imports[] = {
    {H("14InitializeNpadEv"), hid_InitializeNpad}, {H("24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE"), hid_SetSupportedNpadStyleSet},
    {H("22SetSupportedNpadIdTypeEPKjm"), hid_SetSupportedNpadIdType}, {H("18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE"), hid_joycon_only},
    {H("20GetNpadJoyAssignmentERKj"), hid_GetNpadJoyAssignment}, {H("30SetNpadJoyAssignmentModeSingleERKjNS0_17NpadJoyDeviceTypeE"), hid_joycon_only},
    {H("30SetNpadJoyAssignmentModeSingleERKj"), hid_joycon_only}, {H("28SetNpadJoyAssignmentModeDualERKj"), hid_joycon_only},
    {H("23MergeSingleJoyAsDualJoyERKjS2_"), hid_MergeSingleJoyAsDualJoy}, {H("18SwapNpadAssignmentERKjS2_"), hid_SwapNpadAssignment}, {H("14DisconnectNpadERKj"), hid_DisconnectNpad},
    {H("19GetPlayerLedPatternERKj"), hid_GetPlayerLedPattern}, {H("15GetNpadStyleSetERKj"), hid_GetNpadStyleSet},
    {H("12GetNpadStateEPNS0_16NpadFullKeyStateERKj"), hid_GetNpadState_FullKey},
    {H("12GetNpadStateEPNS0_16NpadJoyDualStateERKj"), hid_GetNpadState_Other},
    {H("12GetNpadStateEPNS0_16NpadJoyLeftStateERKj"), hid_GetNpadState_Other},
    {H("12GetNpadStateEPNS0_17NpadJoyRightStateERKj"), hid_GetNpadState_Other},
    {H("12GetNpadStateEPNS0_11NpadGcStateERKj"), hid_GetNpadState_Gc},
    {H("12GetNpadStateEPNS0_17NpadHandheldStateERKj"), hid_GetNpadState_Other},
    {H("22GetNpadControllerColorEPNS0_19NpadControllerColorERKj"), hid_ControllerColor1},
    {H("22GetNpadControllerColorEPNS0_19NpadControllerColorES2_RKj"), hid_ControllerColor2},
    {H("21InitializeTouchScreenEv"), hid_InitializeTouchScreen}, {H("19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE"), hid_GetTouchScreenState},
    {H("21StartLrAssignmentModeEv"), hid_joycon_only}, {H("20StopLrAssignmentModeEv"), hid_joycon_only},
    {H("21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE"), hid_GetSixAxisSensorState},
    {H("21IsSixAxisSensorAtRestERKNS0_19SixAxisSensorHandleE"), hid_IsSixAxisSensorAtRest},
    {H("18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE"), hid_StartSixAxisSensor},
    {H("22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE"), hid_GetVibrationDeviceInfo},
    {H("18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE"), hid_SendVibrationValue},
    {H("25SendVibrationGcErmCommandERKNS0_21VibrationDeviceHandleERKNS0_21VibrationGcErmCommandE"), hid_SendVibrationGcErmCommand},
    {H("23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE"), hid_GetActualVibrationValue},
    {H("30GetActualVibrationGcErmCommandEPNS0_21VibrationGcErmCommandERKNS0_21VibrationDeviceHandleE"), hid_GetActualVibrationGcErmCommand},
    {H("25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE"), hid_InitializeVibrationDevice},
    {H("23StartSevenSixAxisSensorEv"), hid_seven_start_stop}, {H("22StopSevenSixAxisSensorEv"), hid_seven_start_stop},
    {H("27GetSevenSixAxisSensorStatesEPNS0_23SevenSixAxisSensorStateEi"), hid_GetSevenSixAxisSensorStates},
    {H("26IsSevenSixAxisSensorAtRestEv"), hid_IsSevenSixAxisSensorAtRest}, {H("28InitializeSevenSixAxisSensorEPvm"), hid_InitializeSevenSixAxisSensor},
};

void register_hid_imports(void) {
    register_imports(hid_imports, (int)(sizeof hid_imports / sizeof hid_imports[0]));
    { void r1_register_heap(void); r1_register_heap(); }     /* R1 (r1_state.c): the guest C heap in snapshotted memory, SSBU_R1 only */
}
