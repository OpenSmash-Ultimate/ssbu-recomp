/* R3 (TEST ONLY, default OFF): the ONLINE ROLLBACK SESSION, the parts that do not touch the game's state
 * (plan\phases\R3_online_rollback.md). The game side (snapshot ring, re-run passes, the pads given to the game) is
 * rb_rollback.c, included by r1_state.c; the transport is net_relay.c (packet type 16, through the lobby / the host's
 * game). This file: the input queues (rb_input.c), the wire format (rb_wire.c), the time sync (rb_timesync.c), the
 * receive queue, the RTT / clock-offset measurement, the per-frame state checksums, a test bot and the session report.
 * Design after Slippi's netplay (project-slippi/Ishiiruka, GPL-2.0-or-later) and melee-unlocked's mu_net
 * (GPL-2.0-or-later), N1 / N1b studies; no code copied. No game content in this file.
 *
 *   SSBU_RB_NET=net        two games over net_relay (SSBU_ONLINE host / join / lobby with a private code)
 *   SSBU_RB_NET=loop[,<f>] one game: the other player is a bot whose pads "arrive" <f> frames late (default 4):
 *                          rollback without a network, for the determinism proof (f = 0: never late = reference)
 *   SSBU_RB_NET_PLAYER=0|1 this game's player (default: 0, 1 in SSBU_ONLINE=join mode)
 *   SSBU_RB_NET_DELAY=<n>  input delay in frames (default with the snapshot ring: 0 while SSBU_RB_NET_AHEAD covers the trip,
 *                          then 1 per frame short; auto = the old choice from the measured round trip, 1..4)
 *   SSBU_RB_NET_BOT=<seed> the local player's pads come from a test bot instead of the controller / input sequence
 *   SSBU_RB_NET_LOG=<file> one line per finalized frame: "f <frame> h <checksum>" (compare two games' files)
 *   SSBU_RB_NET_AHEAD=<n>  most frames this game runs ahead of the newest remote pad (default 5; at most 7 = the ring depth)
 *   SSBU_RB_NET_FIND=0     (R3 net3) no opponent search through the lobby (lobby / LAN mode without a room: the old way)
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif
#include "rb.h"
#include "rb_input.c"
#include "rb_wire.c"
#include "rb_timesync.c"
#undef M
#include "net_stats.h"                                  /* (R3 net3) NetMatch, MM_*: the lobby's opponent search (net_relay.c) */

int net_rb_send(const void *p, int n);                 /* net_relay.c: 1 = sent (or queued by the latency simulation) */
int net_rb_ready(void);                                /* net_relay.c: joined a lobby (the host's game or a server) */
int net_rb_no_destination(void);                       /* net_relay.c (pub1): messages not sent: no other player in the room on the public lobby */
int net_mode(void);
int net_online(void);

static int g_on = -1, g_loop, g_loop_lat = 4, g_local, g_delay_cfg = -2, g_ahead = 5, g_started, g_peer_started;
static unsigned g_bot_seed; static int g_bot;
static FILE *g_hlog;
static volatile LONG64 g_cur;                          /* the frame this game is on (read by the receive thread for PONGs) */
static LARGE_INTEGER g_qpf;
static uint64_t now_us(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf); return (uint64_t)(t.QuadPart / (double)g_qpf.QuadPart * 1e6); }

int rbn_on(void) {
    if (g_on >= 0) return g_on;
    const char *e = getenv("SSBU_RB_NET");
    g_on = e && e[0] && e[0] != '0';
    if (!g_on) return 0;
    if (!_strnicmp(e, "loop", 4)) { g_loop = 1; if (e[4] == ',') g_loop_lat = atoi(e + 5); }
    g_local = 0;
    if (!g_loop && net_online() && net_mode() == 3) g_local = 1;      /* NET_MODE_JOIN */
    if ((e = getenv("SSBU_RB_NET_PLAYER"))) g_local = atoi(e) ? 1 : 0;
    if ((e = getenv("SSBU_RB_NET_DELAY")) && *e) g_delay_cfg = !_stricmp(e, "auto") ? -1 : atoi(e);      /* (-2 = the default: 0 with the ring) */
    if ((e = getenv("SSBU_RB_NET_BOT")) && *e) { g_bot = 1; g_bot_seed = (unsigned)strtoul(e, NULL, 0); }
    if ((e = getenv("SSBU_RB_NET_AHEAD")) && atoi(e) > 0) g_ahead = atoi(e);
    if ((e = getenv("SSBU_RB_NET_LOG")) && *e) g_hlog = fopen(e, "w");
    printf("[rbnet] on: %s, local player %d, delay %s, ahead at most %d, bot %s\n", g_loop ? "loop test (no network)" : "over net_relay", g_local + 1, g_delay_cfg >= 0 ? "fixed" : g_delay_cfg == -1 ? "from the round trip" : "0 unless the trip is longer than the rollback window", g_ahead, g_bot ? "on" : "off");
    if (g_loop) printf("[rbnet] loop: remote pads arrive %d frames after the frame that would send them\n", g_loop_lat);
    fflush(stdout);
    return g_on;
}
int rbn_local_player(void) { return g_local; }
int rbn_ahead(void) { return g_ahead; }

/* ---------------------------------------------------------------- the test bot: a pure function of (seed, frame) */
static uint32_t mix32(uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
static RbPad bot_pad(unsigned seed, long long f) {
    /* segments of 6..29 frames; each segment one action. Walking alternates sides around the middle of the stage. */
    RbPad p; memset(&p, 0, sizeof p);
    {   static long long off = -2; if (off == -2) { const char *e = getenv("SSBU_RB_NET_BOT_WALKOFF"); off = e && *e ? atoll(e) : -1; }      /* (R3 lwr, test) from that frame on: left held (the fighter walks off the stage: the match ends) */
        if (off >= 0 && f >= off) { p.lx = -32767; return p; } }
    long long start = 1; uint32_t h = 0; int seg = 0;
    for (;; seg++) { h = mix32(seed * 0x9E3779B9u ^ (uint32_t)seg * 0x85EBCA6Bu); long long len = 6 + (h % 24); if (f < start + len) break; start += len; }
    int act = (int)((h >> 8) % 10);
    int dir = (seg & 1) ? 1 : -1;
    switch (act) {
    case 0: case 1: break;                                            /* nothing */
    case 2: case 3: p.lx = (int16_t)(dir * 32767); break;            /* walk / run */
    case 4: p.buttons = 1ull << 0; break;                            /* A (jab) */
    case 5: p.buttons = 1ull << 1; break;                            /* B (neutral special) */
    case 6: if (((f - start) & 15) < 3) p.buttons = 1ull << 2; break;        /* X (jump), tapped */
    case 7: p.lx = (int16_t)(dir * 32767); p.buttons = 1ull << 0; break;     /* forward A */
    case 8: p.ly = -32767; break;                                     /* crouch */
    case 9: p.buttons = (((f - start) & 7) < 4) ? 1ull << 0 : 0; break;       /* A mashed */
    }
    return p;
}
int rbn_bot_on(void) { return g_bot; }

/* ---------------------------------------------------------------- the match handshake (before the match; rb_hs.h)
 * Over the same channel as the pads (packet type 16), message types 0x48.. (the pad messages are 1..4):
 *   CHOICE  joiner -> host, every 100 ms from its match set-up: its own player-1 choice (fighter, colour, controls)
 *   PROPOSE host -> joiner, every 100 ms once the host is at its set-up and has the joiner's CHOICE: the whole match
 *           (player 1 = the host's choice, player 2 = the joiner's, the host's stage / items / stocks, a new seed)
 *   ACCEPT  joiner -> host (answered on the receive thread to every PROPOSE: a lost ACCEPT is made good)
 *   REFUSE  either way: protocol / game build differ, or the other game stopped waiting (the reason is printed on both sides)
 *   BLOCK   both, at the match start: the game's match block (rules + the two player records) in chunks; each game
 *           compares it with its own and prints the differing ranges (a check, nothing is changed)
 * Header of every message: type, protocol, game build, exe link stamp (a different stamp is only reported), nonce, echo.
 * (hs12) Every wait has its own nonce (a fresh random number, never 0): CHOICE carries the joiner's, PROPOSE the host's and
 * echoes the joiner's, ACCEPT echoes the host's; the host takes only CHOICEs that arrive while it waits. A message of an
 * older wait is ignored, so a wait given up on one side and the next match set-up never mix (two matches in one session).
 * While waiting: a message box over the last picture (menu_notice + nvn_front_osk_redraw: the game itself is held at its
 * set-up), Esc (game window in front) or B held on a controller stops waiting, the wait limit is SSBU_RB_NET_HS_WAIT
 * (seconds, default 120). No agreement = this match is played locally (Final Destination, one stock) and left at once
 * (rb_rollback.c: player 1 walks off the stage), back to the menus; the other game is told (REFUSE) so that it stops waiting. */
#include "rb_hs.h"
#define RBH_PROTO 5                                     /* 3 (hs12): RbhMatch carries the stage set-up context; 4 (hs14): PADS end with a mode byte, the stage is drawn from both picks; 5 (R3 lwr): Local Wireless room matches, LAYOUT */
enum { RBH_CHOICE = 0x48, RBH_PROPOSE = 0x49, RBH_ACCEPT = 0x4a, RBH_REFUSE = 0x4b, RBH_BLOCK = 0x4c, RBH_LAYOUT = 0x4d };      /* (R3 lwr) LAYOUT: a room match, each game's own player's control layout */
#pragma pack(push, 1)
typedef struct { uint8_t type, proto; int16_t game_build; uint32_t exe_stamp; uint32_t nonce, echo; RbhMatch m; } RbhMsg;
typedef struct { uint8_t type, proto; int16_t game_build; uint32_t exe_stamp; uint32_t nonce; uint16_t off, len, total; uint8_t data[768]; } RbhBlock;
#pragma pack(pop)
extern int g_game_build;                                /* hostrt.c */
uint64_t ga(uint64_t addr_13_0_4);                      /* hostrt.c (GA) */
void host_fixed_random_reseed(uint64_t seed);           /* os_threads.c: nn::os::GenerateRandomBytes from the agreed seed on */
void menu_notice(const char *text);                     /* menu.cpp (menu_stub.c: nothing) */
void nvn_front_osk_redraw(void); long long nvn_front_presents(void);      /* nvn_gl.c: the last frame presented again (with the notice) */
uint64_t nnhid_menu_buttons(void);                      /* nnhid.c: every controller's buttons, Npad bit order (B = bit 1), not seen by the game */
static SRWLOCK g_hs_lock = SRWLOCK_INIT;
static volatile LONG g_hs_round;                        /* this game's match set-ups so far (log lines) */
static volatile LONG g_hs_waiting;                      /* rbn_handshake is waiting */
static uint32_t g_hs_nonce;                             /* this game's nonce of the newest wait (0 = none yet) */
static uint32_t g_hs_peer_nonce;                        /* the other game's nonce seen in this wait (0 = none yet) */
static RbhMsg g_hs_peer;  static int g_hs_have_peer;    /* host: the joiner's newest CHOICE of this wait */
static RbhMsg g_hs_prop;  static int g_hs_have_prop;    /* host: the proposal sent; joiner: the proposal accepted (this wait) */
static volatile LONG g_hs_accepted, g_hs_refused; static char g_hs_why[160];
static uint64_t g_hs_seed; static uint32_t g_match_id;  /* the agreed match: its seed, and its id (tag of the pad and block messages) */
static volatile LONG g_hs_cancelled;                    /* this match: 1 = no agreement: played locally and left at once; 2 = (pub2) set up
                                                           * inside a Local Wireless room: the game's own netcode plays it, no rollback, nothing changed */
static int g_match_n;                                   /* (hs12) online matches of this session so far (1 = the first; rbn_start) */
/* (hs12) the tag byte at the end of every PADS message: the agreed match id and the match number of the session. Run tw7:
 * the other game's last pads of match 1 arrived after this game's match-1 reset and were taken at the start of match 2
 * (their acknowledgement 1081 made each game send no pads any more: both waited for ever at frame 7). */
static uint8_t pads_tag(void) { return (uint8_t)(g_match_id ^ ((uint32_t)g_match_n * 0x3bu)); }
static long long g_hs_tx, g_hs_rx;
#define RBH_BLK 0xa48                                   /* match block + 0 .. + 0x580 + 2 * 0x260 + 8 */
static uint8_t g_hs_mine_blk[RBH_BLK], g_hs_peer_blk[RBH_BLK]; static int g_hs_mine_ok, g_hs_peer_got, g_hs_blk_done; static uint8_t g_hs_peer_have[RBH_BLK / 768 + 1];
static uint32_t exe_stamp(void) {
#ifdef _WIN32
    const uint8_t *b = (const uint8_t *)GetModuleHandleA(NULL); if (!b) return 0;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(b + ((const IMAGE_DOS_HEADER *)b)->e_lfanew);
    return nt->FileHeader.TimeDateStamp;
#else                                                   /* POSIX builds: ELF / Mach-O have no link time: the exe's modification time */
    char p[MAX_PATH]; struct stat st;
    DWORD n = GetModuleFileNameA(NULL, p, MAX_PATH);
    return n && n < MAX_PATH && !stat(p, &st) ? (uint32_t)st.st_mtime : 0;
#endif
}
int rbn_handshake_wanted(void) {
    static int w = -1;
    if (w < 0) { const char *e = getenv("SSBU_RB_NET"), *h = getenv("SSBU_RB_NET_HANDSHAKE");
        w = e && e[0] && e[0] != '0' && _strnicmp(e, "loop", 4) && !(h && h[0] == '0'); }
    return w;
}
uint64_t rbn_match_seed(void) {
    if (g_loop && !g_hs_seed) { const char *e = getenv("SSBU_RB_NET_LOOP_SEED"); if (e && *e) return strtoull(e, NULL, 0); }   /* (hs15, test) the loop game's generators from a seed */
    return g_hs_seed;
}
static volatile LONG g_hs_room;                         /* (pub2) the last set-up was inside a Local Wireless room (until the next set-up: the fight-off of the
                                                           * room's waiting-stage fight comes AFTER the room match's set-up, run rm4) */
int rbn_match_cancelled(void) { return g_hs_room ? 2 : (int)g_hs_cancelled; }      /* 0, 1 (not agreed: walk off) or 2 (pub2: a room fight, the game's own netcode) */
/* (R3 lwr) debug_mode.c tells which set-up this is: 1 = a Local Wireless room's waiting-stage fight (the record builder called from
 * the waiting-stage set-up, one player), 0 = any other (the select screens' "ready to fight", also the room's match) */
static int g_setup_waiting, g_hs_lwr;
void rbn_set_setup_waiting(int waiting) { g_setup_waiting = waiting; }
int rbn_room_rollback(void) { return g_hs_lwr; }       /* (R3 lwr) the last agreed match is a Local Wireless room's match played with rollback */
static int rbn_peer_heard_ms(void);                     /* (R3 lwr) ms since the other game's rollback session was last heard, -1 = never */
static volatile LONG g_lay_have_mine, g_lay_have_peer; static uint8_t g_lay_peer_has; static uint16_t g_lay_peer[11]; static uint8_t g_lay_peer_ctl[0x2d];
static RbhMsg g_lay_mine;
const char *rbn_hs_why(void) { return g_hs_why; }      /* (R3 pub1) why the last handshake failed (rb_rollback.c's notice) */
static void rtt_reset(void);                            /* (R3 net3) a new opponent: its round trip is measured afresh */
static void hs_head(RbhMsg *m, int type) { memset(m, 0, sizeof *m); m->type = (uint8_t)type; m->proto = RBH_PROTO; m->game_build = (int16_t)g_game_build; m->exe_stamp = exe_stamp(); m->nonce = g_hs_nonce; }
static void hs_send(const void *p, int n) { if (net_rb_send(p, n)) g_hs_tx++; }
static int hs_check(const RbhMsg *m, char *why, size_t n) {           /* 0 = the other game cannot play this match with us */
    if (m->proto != RBH_PROTO) { snprintf(why, n, "handshake protocol %d here, %d there", RBH_PROTO, m->proto); return 0; }
    if (m->game_build != (int16_t)g_game_build) { snprintf(why, n, "game build %d here, %d there (13.0.4 = 0, 13.0.5 = 1): both players need the same game version", g_game_build, m->game_build); return 0; }
    return 1;
}
static void hs_refuse(uint32_t echo, const char *why) {
    RbhMsg r; hs_head(&r, RBH_REFUSE); r.echo = echo; snprintf((char *)&r.m, sizeof r.m, "%s", why); hs_send(&r, sizeof r);
}
/* (R3 lwr) a room match: each game knows only its OWN player's 11-word text field of the panel state (+ 0x54, record + 0x28:
 * the name-tag text in runs lwr17..20; hs12 took it for the control layout) (the other player is a network player in the room's
 * records). At player 2's record (both games are at the same set-up) each sends its own and waits for the other's
 * (at most SSBU_RB_NET_ROOM_WAIT s / 2; sent again every 50 ms, a received one is answered once). 1 = *other is the other
 * player's layout (*other_has: it had one); 0 = not received (each game keeps what it has: the match may differ, logged). */
/* (owner 2026-10-22) the message also carries the player's own name-tag controls: 0x2b bytes (profile + 0x24 .. + 0x4e) and the
 * option byte of its controller kind (debug_mode.c lwr_own_controls), in RbhMatch.ctx_hist (not used by a LAYOUT otherwise). */
int rbn_room_layout(const uint16_t mine[11], int has, const uint8_t mine_ctl[0x2d], uint16_t other[11], int *other_has, uint8_t other_ctl[0x2d]) {
    AcquireSRWLockExclusive(&g_hs_lock);
    hs_head(&g_lay_mine, RBH_LAYOUT); g_lay_mine.nonce = g_match_id; memcpy(g_lay_mine.m.layout[0], mine, sizeof g_lay_mine.m.layout[0]); g_lay_mine.m.has_layout[0] = (uint8_t)(has != 0);
    memcpy(g_lay_mine.m.ctx_hist, mine_ctl, 0x2d);      /* (+ 0x2c: 1 = a GameCube controller) */
    g_lay_have_mine = 1;
    RbhMsg ans = g_lay_mine; ans.echo = 1;
    ReleaseSRWLockExclusive(&g_hs_lock);
    double limit = (getenv("SSBU_RB_NET_ROOM_WAIT") ? atof(getenv("SSBU_RB_NET_ROOM_WAIT")) : 10.0) * 500.0;
    uint64_t t0 = GetTickCount64(), last = 0;
    while (!g_lay_have_peer && GetTickCount64() - t0 < limit) {
        if (GetTickCount64() - last >= 50) { last = GetTickCount64(); hs_send(&g_lay_mine, sizeof g_lay_mine); }
        Sleep(2);
    }
    for (int k = 0; k < 3; k++) hs_send(&ans, sizeof ans);          /* (the other game may still wait: answers that do not ask for one back) */
    int ok = g_lay_have_peer != 0;
    if (ok) { memcpy(other, g_lay_peer, sizeof g_lay_peer); *other_has = g_lay_peer_has; memcpy(other_ctl, g_lay_peer_ctl, sizeof g_lay_peer_ctl); }
    printf("[rbnet] room match: player name and controls (name tag text, profile controls): this game's player's sent, the other player's %s after %llu ms\n", ok ? "received" : "NOT RECEIVED (the match may differ)", (unsigned long long)(GetTickCount64() - t0));
    fflush(stdout);
    return ok;
}
/* the receive thread: a handshake message (1 = it was one) */
static int hs_rx(const uint8_t *p, int n) {
    if (n < 1 || p[0] < RBH_CHOICE || p[0] > RBH_LAYOUT) return 0;
    g_hs_rx++;
    if (p[0] == RBH_BLOCK) {
        if (n < (int)offsetof(RbhBlock, data)) return 1;
        RbhBlock b; memset(&b, 0, sizeof b); memcpy(&b, p, (size_t)n < sizeof b ? (size_t)n : sizeof b);
        if (b.nonce == g_match_id && b.off + b.len <= RBH_BLK && b.len <= sizeof b.data) { AcquireSRWLockExclusive(&g_hs_lock); memcpy(g_hs_peer_blk + b.off, b.data, b.len); g_hs_peer_have[b.off / 768] = 1; ReleaseSRWLockExclusive(&g_hs_lock); }
        return 1;
    }
    if (n < (int)sizeof(RbhMsg)) return 1;
    RbhMsg m; memcpy(&m, p, sizeof m);
    char why[160];
    AcquireSRWLockExclusive(&g_hs_lock);
    int waiting = g_hs_waiting && g_hs_nonce;
    switch (m.type) {
    case RBH_CHOICE:                                    /* (host, while it waits: an older CHOICE is not for this match) */
        if (g_local != 0 || !waiting) break;
        if (!hs_check(&m, why, sizeof why)) { snprintf(g_hs_why, sizeof g_hs_why, "%s", why); g_hs_refused = 1; hs_refuse(m.nonce, why); break; }
        g_hs_peer = m; g_hs_have_peer = 1; g_hs_peer_nonce = m.nonce;
        if (g_hs_have_prop && g_hs_prop.echo == m.nonce) hs_send(&g_hs_prop, sizeof g_hs_prop);   /* our proposal was lost */
        break;
    case RBH_PROPOSE:                                   /* (joiner: for our newest wait; answered after the wait too, a lost ACCEPT is made good) */
        if (g_local != 1 || !g_hs_nonce || m.echo != g_hs_nonce) break;
        if (!hs_check(&m, why, sizeof why)) { snprintf(g_hs_why, sizeof g_hs_why, "%s", why); g_hs_refused = 1; hs_refuse(m.nonce, why); break; }
        if (g_hs_have_prop && g_hs_prop.nonce != m.nonce) break;     /* (one proposal per wait) */
        if (!g_hs_have_prop && !waiting) break;
        g_hs_prop = m; g_hs_have_prop = 1; g_hs_peer_nonce = m.nonce;
        { RbhMsg a; hs_head(&a, RBH_ACCEPT); a.echo = m.nonce; a.m = m.m; hs_send(&a, sizeof a); }
        break;
    case RBH_ACCEPT:                                    /* (host) */
        if (g_local == 0 && waiting && g_hs_have_prop && m.echo == g_hs_prop.nonce && !memcmp(&m.m, &g_hs_prop.m, sizeof m.m)) g_hs_accepted = 1;
        break;
    case RBH_LAYOUT:                                    /* (R3 lwr) the other game's own player's control layout for this match; answered once with ours */
        if (m.nonce != g_match_id || !g_match_id || m.proto != RBH_PROTO) break;
        memcpy(g_lay_peer, m.m.layout[0], sizeof g_lay_peer); g_lay_peer_has = m.m.has_layout[0]; memcpy(g_lay_peer_ctl, m.m.ctx_hist, sizeof g_lay_peer_ctl); g_lay_have_peer = 1;
        if (g_lay_have_mine && !m.echo) hs_send(&g_lay_mine, sizeof g_lay_mine);
        break;
    case RBH_REFUSE:                                    /* about our wait, or from the other game's wait we deal with */
        if (!waiting || !((m.echo && m.echo == g_hs_nonce) || (g_hs_peer_nonce && m.nonce == g_hs_peer_nonce))) break;
        snprintf(g_hs_why, sizeof g_hs_why, "the other game refused: %.120s", (const char *)&m.m); g_hs_refused = 2;
        break;
    }
    ReleaseSRWLockExclusive(&g_hs_lock);
    return 1;
}
static uint64_t hs_rand(void) {
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    uint64_t s = (uint64_t)t.QuadPart ^ ((uint64_t)GetCurrentProcessId() << 32) ^ GetTickCount64() * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)&t;
    s ^= s >> 29; s *= 0xBF58476D1CE4E5B9ull; s ^= s >> 32; s *= 0x94D049BB133111EBull; s ^= s >> 31;
    return s;
}
static uint64_t hs_new_seed(void) {
    const char *e = getenv("SSBU_RB_NET_SEED");
    uint64_t s = e && *e ? strtoull(e, NULL, 0) : hs_rand();
    return s ? s : 1;
}
static void hs_print(const char *what, const RbhMatch *m) {
    printf("[rbnet] handshake: %s: player 1 ui_chara %016llx colour %d controls %s, player 2 ui_chara %016llx colour %d controls %s; stage entry %016llx form %u flags %x second %016llx; hazards %d, items %u, rule kind %d, stocks %d; seed %016llx\n", what,
           (unsigned long long)m->chara[0], m->colour[0], m->has_layout[0] ? "own layout" : "none", (unsigned long long)m->chara[1], m->colour[1], m->has_layout[1] ? "own layout" : "none",
           (unsigned long long)*(const uint64_t *)m->stage, *(const uint32_t *)(m->stage + 8), *(const uint32_t *)(m->stage + 12), (unsigned long long)*(const uint64_t *)(m->stage + 16),
           m->hazards, m->items, m->rule_kind, m->stock, (unsigned long long)m->seed);
    fflush(stdout);
}
/* Esc (this game's window in front) or B on any controller, held 0.3 s; SSBU_RB_NET_TEST_CANCEL=<s>: as if pressed after s seconds (test) */
static int hs_stop_pressed(double waited_ms) {
    static ULONGLONG since; static double test = -2;
    if (test == -2) { const char *e = getenv("SSBU_RB_NET_TEST_CANCEL"); test = e && *e ? atof(e) * 1000.0 : -1; }
    if (test >= 0 && waited_ms >= test) return 1;
    int down = 0;
    HWND fg = GetForegroundWindow(); DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) down = 1;
    if (waited_ms > 500 && (nnhid_menu_buttons() & 2)) down = 1;
    ULONGLONG now = GetTickCount64();
    if (!down) { since = 0; return 0; }
    if (!since) since = now;
    return now - since >= 300;
}
/* (hs14) the fair rule for what the two players chose differently (host, when it builds the proposal):
 *  - rules (stock / time kind, stocks, items, hazards) and the stage set-up's inputs: the host's (the joiner sees them in the
 *    proposal and its game takes them; a different rule KIND on the joiner's screens is only reported, see debug_mode.c);
 *  - the stage: one of the two players' own picks, drawn with the agreed seed (bit 40 of it: 0 = the host's pick, 1 = the
 *    joiner's), so over many matches each player plays on his own pick half the time and neither game can steer it (the seed
 *    is new for every match). A joiner pick that is not a stage entry (no ui_stage id) leaves the host's.
 *    SSBU_RB_NET_STAGE=host / joiner forces one side (tests). */
static void hs_stage_rule(RbhMatch *m, const RbhMatch *peer) {
    const char *e = getenv("SSBU_RB_NET_STAGE");
    uint64_t hs = *(const uint64_t *)m->stage, js = *(const uint64_t *)peer->stage; uint32_t hf = *(const uint32_t *)(m->stage + 8);
    int valid = (js >> 56) == 0x69, draw = (int)((m->seed >> 40) & 1);
    int joiner = valid && (e && !_stricmp(e, "joiner") ? 1 : e && !_stricmp(e, "host") ? 0 : draw);
    if (joiner) memcpy(m->stage, peer->stage, sizeof m->stage);
    printf("[rbnet] handshake: stage: the host picked %016llx form %u, the joiner %016llx form %u; drawn with the agreed seed (%s): the %s's pick%s\n",
           (unsigned long long)hs, hf,
           (unsigned long long)js, *(const uint32_t *)(peer->stage + 8), e && *e ? e : "random", joiner ? "joiner" : "host", valid ? "" : " (the joiner's is not a stage entry)");
    fflush(stdout);
}
int rbn_handshake(const RbhMatch *mine, RbhMatch *out) {
    if (!rbn_handshake_wanted() || rbn_on() <= 0 || g_loop) return 0;
    LONG round = InterlockedIncrement(&g_hs_round);
    /* (R3 pub2: the Local Wireless "deadlock" of lb1 / lb3 and of the testers 2026-10-20) a fight set up while this game is in a Local
     * Wireless room is either the room's waiting-stage fight (each game alone, one player) or the room's own match, which the game
     * plays with its own online netcode: BOTH games build the records of BOTH players from the room (run nb2: player 1 = the room
     * maker's fighter, player 2 = the joiner's, the same on both PCs). Rollback cannot run under that netcode, and holding one game
     * here stopped its traffic: the other game waited on its loading screen for it, then left the room. So no handshake and no
     * rollback for these fights: they are played exactly as without rollback, whatever the other game's setting; a box says so
     * once. Rollback matches: Smash (versus) with a room code (host / join modes). SSBU_RB_NET_ROOM_MATCH=1: the old way (test). */
    InterlockedExchange(&g_hs_room, 0);
    /* (R3 lwr, owner 2026-10-21: "il rollback deve essere attivo anche nel local wireless con le stanze") the room's MATCH (not its
     * waiting-stage fight) with rollback on both games: the room has already agreed fighters, stage and rules on both PCs, so the
     * handshake here only checks that and agrees on a seed (no menus to wait for: both games reach this set-up together when the
     * room maker starts the match; at most SSBU_RB_NET_ROOM_WAIT s, default 10). debug_mode.c then makes the fight a local versus
     * match (the room's melee mode "local wireless" -> "local", match block + 0), so the game's own online netcode does not play it,
     * and rb_rollback.c runs it as every rollback match (room maker = player 1, port i = player i). It is tried only when the other
     * player of a 1-on-1 room is heard by this game's rollback session (its pings reach only a game with rollback on); otherwise,
     * and whenever the agreement fails, the fight is left exactly as the game set it up (pub2: the game's own netcode), so a
     * player with rollback off is never held. SSBU_RB_NET_ROOM_ROLLBACK=0: the pub2 way for every room fight. */
    int lwr = 0;
    g_hs_lwr = 0;
    InterlockedExchange(&g_lay_have_peer, 0); InterlockedExchange(&g_lay_have_mine, 0);      /* (a LAYOUT of an older match is not this one's) */
    {   int nnldn_role(void), nnldn_players(void); const char *e = getenv("SSBU_RB_NET_ROOM_MATCH"), *rr = getenv("SSBU_RB_NET_ROOM_ROLLBACK");
        if (nnldn_role() && !(e && e[0] == '1')) {
            int heard = rbn_peer_heard_ms(), players = nnldn_players(), want = !(rr && rr[0] == '0');
            if (!g_setup_waiting && want && players == 2 && heard >= 0 && heard < 3000) {
                lwr = 1;
                printf("[rbnet] handshake %ld: the Local Wireless room's match (%s; 2 players; the other game's rollback session heard %d ms ago): agreeing on a rollback match\n", (long)round, nnldn_role() == 1 ? "made here" : "joined", heard);
                fflush(stdout);
            } else {
                static int told; void menu_popup(const char *title, const char *text);
                InterlockedExchange(&g_hs_room, 1);
                printf("[rbnet] handshake %ld: this fight is set up inside a Local Wireless room (%s; %s): the game's own online netcode plays it; no rollback, no handshake, nothing changed\n", (long)round, nnldn_role() == 1 ? "made here" : "joined",
                       g_setup_waiting ? "the room's waiting-stage fight" : !want ? "room rollback off (SSBU_RB_NET_ROOM_ROLLBACK=0)" : players != 2 ? "not a 1-on-1 room" : "the other player's game has rollback off (never heard)");
                fflush(stdout);
                if (!g_setup_waiting && !told++) menu_popup("Rollback netcode", "This room match uses the game's own online netcode: the other player's game has rollback off (or the room has more than 2 players). Rollback is used in a 1-on-1 room when both players have it on.");
                return 0;
            }
        } }
    /* (R3 pub1, testers 2026-10-20) through a lobby server (and on a LAN) NEITHER game is in JOIN mode, so both were "the host":
     * each waited for the other's CHOICE and sent nothing ("messages sent 0, received 0" for 120 s). The roles now come from
     * the Local Wireless room: the game that made the room is player 1 (host of the handshake), the one that joined it player 2.
     * SSBU_RB_NET_PLAYER still forces one (tests). */
    {   int nnldn_role(void); int role = nnldn_role();
        if (role && !getenv("SSBU_RB_NET_PLAYER")) { int want = role == 2; if (want != g_local) printf("[rbnet] handshake: this game %s the Local Wireless room: it is player %d (was %d by the online mode)\n", role == 1 ? "made" : "joined", want + 1, g_local + 1); g_local = want; } }
    /* (R3 net3, goal 3) a rollback match without a room code: this game is in the lobby (or LAN) mode and not in a Local Wireless
     * room, so it does not know the other player. It asks the lobby for an opponent who does the same (lobby 1.4, MATCH op 2:
     * net_relay.c net_match_find_rb); the lobby pairs two such games of the same build and lobby code and says who is player 1
     * (the one that waited longer). Then the handshake below runs as with a room code, with the rollback messages addressed to
     * that opponent. The box shows the search. SSBU_RB_NET_FIND=0: the old way (no search). */
    /* (owner 2026-10-21: "così hai tolto la possibilità alla gente di giocare offline") a Smash fight with rollback on and no room
     * code searched the lobby for an opponent: local fights against friends or CPUs were gone. Now a fight without a room code
     * (online play off, lobby or LAN mode) is a local fight, untouched, like a room fight; the online rollback match comes from
     * a Local Wireless room's match (R3 lwr) or a room code (host / join). SSBU_RB_NET_FIND=1: the lobby search (test). */
    {   const char *e = getenv("SSBU_RB_NET_FIND"); int mode = net_online() > 0 ? net_mode() : -1;
        if (!lwr /* (R3 lwr: a room match) */ && mode != 2 /* NET_MODE_HOST */ && mode != 3 /* NET_MODE_JOIN */ && !(e && e[0] == '1')) {
            InterlockedExchange(&g_hs_room, 1);
            printf("[rbnet] handshake %ld: no room code: a local fight, played as without rollback\n", (long)round);
            fflush(stdout);
            return 0;
        } }
    int find = 0, phase = 1;
    {   int nnldn_role(void); const char *e = getenv("SSBU_RB_NET_FIND"); int mode = net_online() > 0 ? net_mode() : -1;
        find = !nnldn_role() && (mode == 0 /* NET_MODE_LOBBY */ || mode == 1 /* NET_MODE_LAN */) && e && e[0] == '1';
        if (find) { phase = 0; net_match_find_rb(); } }
    double limit = getenv("SSBU_RB_NET_HS_WAIT") ? atof(getenv("SSBU_RB_NET_HS_WAIT")) * 1000.0 : 120000.0;
    if (lwr) limit = getenv("SSBU_RB_NET_ROOM_WAIT") ? atof(getenv("SSBU_RB_NET_ROOM_WAIT")) * 1000.0 : 10000.0;     /* (R3 lwr) both games are at this set-up together */
    uint64_t t0 = GetTickCount64(), last_tx = 0, last_log = t0, last_note = 0, last_redraw = 0;
    long long presents = nvn_front_presents(); uint64_t presents_at = t0, notes = 0, redraws = 0;
    int ok = 0, stopped = 0;
    AcquireSRWLockExclusive(&g_hs_lock);
    do g_hs_nonce = (uint32_t)hs_rand(); while (!g_hs_nonce);
    g_hs_peer_nonce = 0; g_hs_have_peer = g_hs_have_prop = 0; g_hs_accepted = g_hs_refused = 0; g_hs_why[0] = 0; g_hs_cancelled = 0;
    g_hs_waiting = 1;
    ReleaseSRWLockExclusive(&g_hs_lock);
    if (find) printf("[rbnet] handshake %ld: no room code and no Local Wireless room: asking the lobby for an opponent who starts a rollback match too; own choice ui_chara %016llx colour %d (at most %.0f s; nonce %08x)\n", (long)round,
           (unsigned long long)mine->chara[0], mine->colour[0], limit / 1000.0, g_hs_nonce);
    else printf("[rbnet] handshake %ld: this game is the %s; own choice ui_chara %016llx colour %d; waiting for the other game (at most %.0f s; nonce %08x)\n", (long)round, g_local ? "joiner (player 2)" : "host (player 1)",
           (unsigned long long)mine->chara[0], mine->colour[0], limit / 1000.0, g_hs_nonce);
    fflush(stdout);
    NetMatch mm; memset(&mm, 0, sizeof mm);
    for (;;) {
        uint64_t now = GetTickCount64();
        if (g_hs_refused) break;
        if ((double)(now - t0) > limit) { snprintf(g_hs_why, sizeof g_hs_why, phase ? "no answer from the other player after %.0f s" : "no opponent found in %.0f s", limit / 1000.0); break; }
        if (hs_stop_pressed((double)(now - t0))) { snprintf(g_hs_why, sizeof g_hs_why, "stopped waiting (Esc / B) after %.0f s", (now - t0) / 1000.0); stopped = 1; break; }
        if (phase == 0) {                               /* (R3 net3) the lobby's opponent search */
            net_match_get(&mm);
            if (mm.kind == 2 && (mm.state == MM_MATCHED || mm.state == MM_TOGETHER) && mm.peer) {
                int want = mm.role == MM_ROLE_JOIN;
                AcquireSRWLockExclusive(&g_hs_lock); g_local = want; ReleaseSRWLockExclusive(&g_hs_lock);
                rtt_reset();
                printf("[rbnet] handshake %ld: the lobby found an opponent after %.1f s: %s (10.13.%u.%u); this game is the %s\n", (long)round, (now - t0) / 1000.0, mm.peer_name, mm.peer >> 8 & 255, mm.peer & 255, want ? "joiner (player 2)" : "host (player 1)");
                fflush(stdout);
                phase = 1; t0 = now; last_note = 0;
            } else if (mm.state == MM_TIMEOUT || mm.state == MM_OLD_LOBBY || mm.state == MM_PEER_LEFT || mm.state == MM_EXPIRED || mm.state == MM_OFFLINE || (mm.kind != 2 && mm.state != MM_ASKING)) {
                snprintf(g_hs_why, sizeof g_hs_why, "%s", mm.state == MM_TIMEOUT ? "no opponent found in time (nobody with your version and mods searched)" :
                         mm.state == MM_OLD_LOBBY ? "the lobby server is an older version (it cannot find rollback opponents): use a room code" :
                         mm.state == MM_OFFLINE ? "online play is off" : mm.kind != 2 ? "another search was started (F1 > Online)" : "the opponent left");
                break;
            }
        }
        AcquireSRWLockExclusive(&g_hs_lock);
        if (phase == 0) { /* searching: nothing to send yet */ }
        else if (g_local == 0) {                             /* host: proposal = our choice + the joiner's */
            if (!g_hs_have_prop && g_hs_have_peer && lwr) {      /* (R3 lwr) the room's match: both games built it from the room; only the seed is new */
                if (g_hs_peer.exe_stamp != exe_stamp()) printf("[rbnet] handshake: NOTE the other game's exe was linked at another time (stamp %08x here, %08x there): a different build may desync\n", exe_stamp(), g_hs_peer.exe_stamp);
                if (g_hs_peer.m.chara[0] != mine->chara[0] || g_hs_peer.m.rule_kind != mine->rule_kind || g_hs_peer.m.stock != mine->stock) {
                    snprintf(g_hs_why, sizeof g_hs_why, "the room's match differs on the two games (player 1 %016llx here, %016llx there; rule kind %d / %d, stocks %d / %d)", (unsigned long long)mine->chara[0], (unsigned long long)g_hs_peer.m.chara[0], mine->rule_kind, g_hs_peer.m.rule_kind, mine->stock, g_hs_peer.m.stock);
                    g_hs_refused = 1; hs_refuse(g_hs_peer.nonce, g_hs_why);
                } else {
                    RbhMatch m = *mine; m.seed = hs_new_seed();
                    hs_head(&g_hs_prop, RBH_PROPOSE); g_hs_prop.echo = g_hs_peer.nonce; g_hs_prop.m = m; g_hs_have_prop = 1;
                }
            }
            if (!g_hs_have_prop && g_hs_have_peer && !lwr) {
                if (g_hs_peer.exe_stamp != exe_stamp()) printf("[rbnet] handshake: NOTE the other game's exe was linked at another time (stamp %08x here, %08x there): a different build may desync\n", exe_stamp(), g_hs_peer.exe_stamp);
                RbhMatch m = *mine;
                m.chara[1] = g_hs_peer.m.chara[0]; m.colour[1] = g_hs_peer.m.colour[0];
                m.has_layout[1] = g_hs_peer.m.has_layout[0]; memcpy(m.layout[1], g_hs_peer.m.layout[0], sizeof m.layout[1]);
                if (m.chara[1] == m.chara[0] && m.colour[1] == m.colour[0]) m.colour[1] = (uint8_t)((m.colour[0] + 1) & 7);   /* same fighter, same colour: the joiner takes the next one */
                m.seed = hs_new_seed();
                hs_stage_rule(&m, &g_hs_peer.m);
                hs_head(&g_hs_prop, RBH_PROPOSE); g_hs_prop.echo = g_hs_peer.nonce; g_hs_prop.m = m; g_hs_have_prop = 1;
            }
            if (g_hs_accepted && g_hs_have_prop) { *out = g_hs_prop.m; ok = 1; }
            else if (g_hs_have_prop && now - last_tx >= 100) { last_tx = now; hs_send(&g_hs_prop, sizeof g_hs_prop); }
        } else {                                        /* joiner: our choice until the proposal comes */
            if (g_hs_have_prop) {
                if (g_hs_prop.exe_stamp != exe_stamp()) printf("[rbnet] handshake: NOTE the other game's exe was linked at another time (stamp %08x here, %08x there): a different build may desync\n", exe_stamp(), g_hs_prop.exe_stamp);
                *out = g_hs_prop.m; ok = 1;
            } else if (now - last_tx >= 100 && net_rb_ready()) { last_tx = now; RbhMsg c; hs_head(&c, RBH_CHOICE); c.m = *mine; hs_send(&c, sizeof c); }
        }
        if (ok) g_hs_waiting = 0;
        ReleaseSRWLockExclusive(&g_hs_lock);
        if (ok) break;
        if (now - last_note >= 250) {                   /* the message box, and the last frame shown again while the game itself is held here */
            last_note = now; notes++;
            char t[400]; double w = (now - t0) / 1000.0;
            char line[200];
            if (phase == 0) snprintf(line, sizeof line, !net_rb_ready() ? "Online match: connecting to the lobby..." : mm.searching > 0 ? "Online match: finding an opponent (%d of %d players of your version searching)..." : "Online match: finding an opponent...", mm.searching, mm.players);
            else if (lwr) snprintf(line, sizeof line, "Room match: agreeing on a rollback match with the other player...");
            else if (find) snprintf(line, sizeof line, g_hs_peer_nonce ? "Online match with %s: agreeing on the match..." : "Online match: opponent found (%s), waiting for their game...", mm.peer_name);
            else snprintf(line, sizeof line, "%s", !net_rb_ready() ? "Not connected to the other player's game yet (lobby / room not joined)." : net_rb_no_destination() && !g_hs_peer_nonce ? "The other player is not in your Local Wireless room: both players must be in the same room." : g_hs_peer_nonce ? "The other player is here: agreeing on the match..." : "Waiting for the other player to choose a fighter and start...");
            snprintf(t, sizeof t, "%s\nWaiting %.0f s (this game gives up after %.0f s and goes back to the character select).\nPress Esc or hold B to stop waiting.", line, w, limit / 1000.0);
            menu_notice(t);
        }
        {   long long p = nvn_front_presents();
            if (p != presents) { presents = p; presents_at = now; }
            else if (now - presents_at > 100 && now - last_redraw >= 100) { last_redraw = now; redraws++; nvn_front_osk_redraw(); } }
        if (now - last_log >= 5000) { last_log = now; printf("[rbnet] handshake %ld: still waiting (%s%.0f s; lobby %s; other game %s; messages sent %lld, received %lld; notice updates %llu, frames shown again %llu)\n", (long)round, phase ? "" : "finding an opponent, ", (now - t0) / 1000.0,
                                                          net_rb_ready() ? (net_rb_no_destination() ? "joined, no other player in the room" : "joined") : "NOT joined", g_hs_peer_nonce ? "seen" : "not seen", g_hs_tx, g_hs_rx, (unsigned long long)notes, (unsigned long long)redraws); fflush(stdout); }
        Sleep(5);
    }
    if (!ok) {
        AcquireSRWLockExclusive(&g_hs_lock);
        int told = g_hs_refused == 2; uint32_t echo = g_hs_peer_nonce;
        g_hs_waiting = 0; g_hs_cancelled = lwr ? 0 : 1;
        if (lwr) InterlockedExchange(&g_hs_room, 1);          /* (R3 lwr) nothing was changed: the game's own netcode plays the room's match */
        ReleaseSRWLockExclusive(&g_hs_lock);
        if (find) { net_match_get(&mm); if (mm.kind == 2 && (mm.state == MM_ASKING || mm.state == MM_SEARCHING || mm.state == MM_MATCHED)) net_match_cancel(); }   /* (R3 net3) the lobby and the opponent are told */
        if (!told) { char w[160]; snprintf(w, sizeof w, "the other player %s", stopped ? "stopped waiting" : "gave up waiting (time limit)"); for (int k = 0; k < 3; k++) { hs_refuse(echo, w); Sleep(20); } }
        if (lwr) {                                       /* (R3 lwr) */
            menu_notice(NULL);
            printf("[rbnet] handshake %ld FAILED in the Local Wireless room: %s. The room's match is played by the game's own online netcode, as set up (no rollback)\n", (long)round, g_hs_why);
            fflush(stdout); return 0;
        }
        char t[400]; snprintf(t, sizeof t, "No online match: %s.\nThis match is left at once: back to the character select.", g_hs_why); menu_notice(t);
        printf("[rbnet] handshake %ld FAILED: %s. This match is played on this PC only (Final Destination, one stock) and left at once (player 1 walks off): back to the character select (notice updates %llu, frames shown again %llu)\n",
               (long)round, g_hs_why, (unsigned long long)notes, (unsigned long long)redraws);
        fflush(stdout); return 0;
    }
    menu_notice(NULL);
    g_hs_lwr = lwr;
    g_hs_seed = out->seed; g_match_id = (uint32_t)(out->seed ^ (out->seed >> 32)) | 1u;
    host_fixed_random_reseed(g_hs_seed);
    printf("[rbnet] handshake %ld agreed after %.2f s (messages sent %lld, received %lld; match id %08x; notice updates %llu, frames shown again %llu)\n", (long)round, (GetTickCount64() - t0) / 1000.0, g_hs_tx, g_hs_rx, g_match_id,
           (unsigned long long)notes, (unsigned long long)redraws);
    hs_print("the match", out);
    return 1;
}
/* rbn_start: the game's match block of this game, sent to the other in chunks (a check of the agreed start) */
static void hs_block_send(void) {
    if (!g_hs_mine_ok) return;
    for (int off = 0; off < RBH_BLK; off += 768) {
        RbhBlock b; memset(&b, 0, sizeof b); b.type = RBH_BLOCK; b.proto = RBH_PROTO; b.game_build = (int16_t)g_game_build; b.exe_stamp = exe_stamp(); b.nonce = g_match_id;
        b.off = (uint16_t)off; b.len = (uint16_t)(RBH_BLK - off < 768 ? RBH_BLK - off : 768); b.total = RBH_BLK; memcpy(b.data, g_hs_mine_blk + off, b.len);
        hs_send(&b, (int)(offsetof(RbhBlock, data) + b.len));
    }
}
static void hs_block_capture(void) {
    SIZE_T got = 0; uint64_t a = ga(0x71052c41b0ULL);            /* the game's global match block (debug_mode.c MELEE_INFO) */
    g_hs_mine_ok = ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)a, g_hs_mine_blk, RBH_BLK, &got) && got == RBH_BLK;
}
static void hs_block_compare(void) {                    /* main thread (rbn_drain): once both copies are complete */
    if (g_hs_blk_done || !g_hs_mine_ok) return;
    for (int k = 0; k * 768 < RBH_BLK; k++) if (!g_hs_peer_have[k]) return;
    g_hs_blk_done = 1;
    char line[900]; int n = 0, ranges = 0, bytes = 0;
    AcquireSRWLockExclusive(&g_hs_lock);
    for (int i = 0; i < RBH_BLK; i++) {
        if (g_hs_mine_blk[i] == g_hs_peer_blk[i]) continue;
        int j = i; while (j + 1 < RBH_BLK && g_hs_mine_blk[j + 1] != g_hs_peer_blk[j + 1]) j++;
        bytes += j - i + 1;
        if (ranges++ < 24 && n < (int)sizeof line - 80) n += snprintf(line + n, sizeof line - n, " +%03x..%03x (here %02x there %02x)", i, j, g_hs_mine_blk[i], g_hs_peer_blk[i]);
        i = j;
    }
    ReleaseSRWLockExclusive(&g_hs_lock);
    if (!ranges) printf("[rbnet] match block check: IDENTICAL on both games (%d bytes: rules + the two player records)\n", RBH_BLK);
    else printf("[rbnet] match block check: %d ranges (%d bytes) differ (offsets from the match block; players at +0x580 + 0x260 * i):%s\n", ranges, bytes, line);
    fflush(stdout);
}

/* ---------------------------------------------------------------- receive queue (net thread -> main) */
#define RQ_N 1024
#define RQ_MAX 1100
static struct { uint64_t at_us; long long at_frame; int n; uint8_t b[RQ_MAX]; } *g_rq;
static volatile LONG g_rq_head, g_rq_tail; static SRWLOCK g_rq_lock = SRWLOCK_INIT;
static volatile LONG64 g_rx_pkts, g_rx_bad, g_rx_drop, g_tx_pkts, g_tx_fail, g_rx_last_ms, g_rx_oldmatch;
static volatile LONG g_rtt_us = -1, g_rtt_min_us = -1;
static long long g_rtt_n; static double g_rtt_sum;      /* (under g_rq_lock) */
#define TSQ 64
static int32_t g_tsq[TSQ]; static int g_tsq_n;          /* clock-offset samples waiting for the main thread (under g_rq_lock) */
/* (hs14) the mode byte at the end of every PADS message (after the match tag): bit 7 = this game plays the match WITHOUT the
 * ring (delay-based, r1_ring.c g_rg_nomem), bits 0..6 = the delay that covers the one-way trip as this game measured it at
 * its match start (frozen then, so both games choose from the same two numbers). The start barrier ends only with a PADS
 * message of the other game, so both know both bytes before frame 1. When either game has no ring, BOTH use the larger of
 * the two delays (SSBU_RB_NET_PEERDELAY=0: the old way, the game with the ring keeps its own rollback delay and the one
 * without it stalls almost every frame: pm1j 1,913 stalls in 2,400 frames). */
static int g_lockstep, g_my_lockd, g_peerdelay = -1;
static volatile LONG g_peer_mode = -1;                 /* the other game's mode byte of this match (-1 = none seen yet) */
static uint8_t my_mode(void) { return (uint8_t)((g_lockstep ? 0x80 : 0) | (g_my_lockd & 0x7f)); }
static void send_msg(const RbMsg *m) {
    uint8_t b[RB_PADS_HEAD + RB_MAX_PADS_PER_MSG * RB_PAD_WIRE + 2];
    size_t n = rb_msg_build(b, sizeof b - 2, m);
    if (!n || g_loop) return;
    if (m->type == RB_MSG_PADS) { b[n++] = pads_tag(); b[n++] = my_mode(); }      /* (hs12) the match these pads belong to: pads of another match are dropped; (hs14) this game's mode byte */
    if (net_rb_send(b, (int)n)) InterlockedIncrement64(&g_tx_pkts); else InterlockedIncrement64(&g_tx_fail);
}
/* net_relay.c's receive thread: one rollback message from the other player */
void rbn_rx(const uint8_t *p, int n) {
    if (rbn_on() > 0 && n > 0 && hs_rx(p, n)) return;  /* the match handshake */
    if (rbn_on() <= 0 || n <= 0 || n > RQ_MAX) { InterlockedIncrement64(&g_rx_bad); return; }
    if (p[0] == RB_MSG_PADS && n >= RB_PADS_HEAD + 2 && (n - RB_PADS_HEAD) % RB_PAD_WIRE == 2) {     /* (hs12) the match tag, (hs14) then the mode byte, at the end */
        if (!g_started || p[n - 2] != pads_tag()) { InterlockedIncrement64(&g_rx_oldmatch); return; }      /* (between matches: the other game sends them again) */
        InterlockedExchange(&g_peer_mode, p[n - 1]);    /* (before it is queued: rbn_drain sees it with the message that starts the match) */
        n -= 2;
    }
    InterlockedIncrement64(&g_rx_pkts); InterlockedExchange64(&g_rx_last_ms, (LONG64)GetTickCount64());
    if (p[0] == RB_MSG_PING) {                          /* answered at once on this thread (the round trip excludes the frame wait) */
        RbMsg m, r; if (!rb_msg_parse(p, (size_t)n, &m)) { InterlockedIncrement64(&g_rx_bad); return; }
        memset(&r, 0, sizeof r); r.type = RB_MSG_PONG; r.frame = (int32_t)g_cur; r.time_us = now_us(); r.echo_us = m.time_us;
        send_msg(&r); return;
    }
    if (p[0] == RB_MSG_PONG) {                          /* the round trip at once (also before the match: the delay is chosen from it) */
        RbMsg m; if (!rb_msg_parse(p, (size_t)n, &m)) { InterlockedIncrement64(&g_rx_bad); return; }
        long rtt = (long)(now_us() - m.echo_us);
        if (rtt <= 0 || rtt >= 5000000) return;
        InterlockedExchange(&g_rtt_us, rtt); if (g_rtt_min_us < 0 || rtt < g_rtt_min_us) InterlockedExchange(&g_rtt_min_us, rtt);
        AcquireSRWLockExclusive(&g_rq_lock);
        g_rtt_sum += rtt; g_rtt_n++;
        if (g_started && m.frame > 0 && g_cur > 0 && g_tsq_n < TSQ) g_tsq[g_tsq_n++] = (int32_t)((g_cur - m.frame) * 16683 - rtt / 2);   /* > 0: this game runs ahead */
        ReleaseSRWLockExclusive(&g_rq_lock);
        return;
    }
    AcquireSRWLockExclusive(&g_rq_lock);
    if (!g_rq) g_rq = calloc(RQ_N, sizeof *g_rq);
    LONG next = (g_rq_head + 1) % RQ_N;
    if (g_rq && next != g_rq_tail) { g_rq[g_rq_head].at_us = now_us(); g_rq[g_rq_head].at_frame = g_cur; g_rq[g_rq_head].n = n; memcpy(g_rq[g_rq_head].b, p, (size_t)n); g_rq_head = next; }
    else InterlockedIncrement64(&g_rx_drop);
    ReleaseSRWLockExclusive(&g_rq_lock);
}
static int rbn_peer_heard_ms(void) {                    /* (R3 lwr) any rollback message of the other game (its pings come every 100 ms) */
    LONG64 last = g_rx_last_ms;
    return last ? (int)(GetTickCount64() - (ULONGLONG)last) : -1;
}
static void rtt_reset(void) {                           /* (R3 net3) */
    AcquireSRWLockExclusive(&g_rq_lock); g_rtt_sum = 0; g_rtt_n = 0; ReleaseSRWLockExclusive(&g_rq_lock);
    InterlockedExchange(&g_rtt_us, -1); InterlockedExchange(&g_rtt_min_us, -1);
}
/* a thread that pings the other player every 100 ms from the start (the delay is chosen from it when the match begins) */
static DWORD WINAPI ping_thread(void *u) {
    (void)u;
    for (;;) {
        Sleep(100);
        if (!net_rb_ready()) continue;
        RbMsg m; memset(&m, 0, sizeof m); m.type = RB_MSG_PING; m.frame = (int32_t)g_cur; m.time_us = now_us();
        send_msg(&m);
    }
    return 0;
}
void rbn_init(void) {                                   /* r1_state.c ra_init: before the match */
    static int done; if (done++ || rbn_on() <= 0) return;
    if (!g_loop) CloseHandle(CreateThread(NULL, 0, ping_thread, NULL, 0, NULL));
}

/* ---------------------------------------------------------------- the session */
static long long g_f0;                                  /* the base frame (the ring's first capture) */
static int g_delay, g_remote = 1;
static long long g_peer_ack, g_sent_newest;
static long long g_stat_pred, g_stat_padget, g_stat_mis;
static long long g_ts_n; static double g_ts_extra;     /* time sync: extra ms per frame for the next frames */
static int g_ts_left;
static uint64_t g_hacc = 0xcbf29ce484222325ull; static int g_hlines;
#define HR 512
static uint64_t g_hash[HR]; static long long g_hframe[HR], g_hfinal_upto; static long long g_desync, g_check_ok, g_desync_first = -1;
static long long g_peer_check_f; static uint32_t g_peer_check;

/* (hs13) g_lockstep: a match without the rollback ring: every frame waits for the other pad */
void rbn_set_lockstep(int on) { g_lockstep = on; }
int rbn_loop(void) { return g_loop; }
static int lock_delay(void) {                           /* the delay that covers the whole one-way trip (+ 1 frame): the wait is short or none */
    if (g_loop) return g_loop_lat + 1;
    int rt = g_rtt_min_us > 0 ? g_rtt_min_us : g_rtt_us;
    if (rt <= 0) return 4;
    int d = (int)ceil(rt / 2000.0 / (1000.0 / 60.0)) + 1;
    return d < 1 ? 1 : d > 12 ? 12 : d;
}
static int peer_lockstep(void) { return g_peer_mode >= 0 && (g_peer_mode & 0x80) && g_peerdelay; }
static int auto_delay(void) {
    if (g_delay_cfg >= 0) return g_delay_cfg;
    if (g_lockstep && (!g_peerdelay || g_loop)) return lock_delay();     /* (the old way; the loop test) */
    if (g_lockstep || peer_lockstep()) {                /* (hs14) either game has no ring: both take the larger of the two announced delays */
        int d = g_my_lockd, pd = g_peer_mode >= 0 ? (g_peer_mode & 0x7f) : 0;
        return pd > d ? pd : d;
    }
    if (g_loop) return 2;
    int rtt = g_rtt_min_us > 0 ? g_rtt_min_us : g_rtt_us;
    /* (2026-11-02) both games have the ring: no input delay while rollback (at most g_ahead frames ahead, 5 by default)
     * covers the one-way trip + 1.5 frames (pads are sent once a frame, and jitter); above that 1 frame of delay per frame
     * short: 0 up to about 117 ms round trip, 1 up to about 150 ms, 2 up to about 183 ms. Two games, 150 ms, delay 0:
     * 712 / 883 waits for the other player in 1,900 frames, the trip being too close to the 5 frames.
     * SSBU_RB_NET_DELAY=auto: the old choice (the trip minus 2 frames, 1..4) */
    if (g_delay_cfg == -2) {
        if (rtt <= 0) return 0;
        int d = (int)ceil(rtt / 2000.0 / (1000.0 / 60.0) + 1.5 - g_ahead);
        return d < 0 ? 0 : d > 4 ? 4 : d;
    }
    if (rtt <= 0) return 2;
    double one_way = rtt / 2000.0 / (1000.0 / 60.0);     /* frames */
    int d = (int)(one_way - 2.0 + 0.5);                   /* up to 2 frames of lateness covered by rollback */
    return d < 1 ? 1 : d > 4 ? 4 : d;
}
void rbn_start(long long f0) {
    g_f0 = f0; g_started = 1; g_match_n++;
    if (g_hlog) { fprintf(g_hlog, "m %d id %08x\n", g_match_n, g_match_id); fflush(g_hlog); }      /* (rbn_cmp.py: frames are per match) */
    g_remote = 1 - g_local;
    if (g_peerdelay < 0) { const char *e = getenv("SSBU_RB_NET_PEERDELAY"); g_peerdelay = !(e && e[0] == '0'); }
    g_my_lockd = lock_delay();                          /* (hs14) announced in every PADS message of this match (frozen here) */
    g_delay = auto_delay();
    if (g_delay < 1 && g_lockstep) g_delay = 1;          /* (no ring: every frame waits for the other pad; with the ring, frames 1..RB_LEADIN are empty on both sides whatever the delay: rb.h) */
    rb_input_reset(2, g_local, g_delay);
    rb_ts_reset(16683);
    if (!g_loop && rbn_handshake_wanted()) { hs_block_capture(); hs_block_send(); }     /* the match block check */
    printf("[rbnet] match %d (id %08x) start at frame %lld: local player %d, input delay %d frames (round trip %.1f ms, best %.1f ms)\n", g_match_n, g_match_id, f0, g_local + 1, g_delay,
           g_rtt_us > 0 ? g_rtt_us / 1000.0 : -1.0, g_rtt_min_us > 0 ? g_rtt_min_us / 1000.0 : -1.0);
    fflush(stdout);
}
/* after the start barrier: the delay again from the round trip measured meanwhile (no frame has run: the queues start over;
 * pads of the other game already queued are sent again by it, from our acknowledgement on) */
void rbn_choose_delay(void) {
    int d = auto_delay(); if (d < 1 && (g_lockstep || peer_lockstep())) d = 1;      /* (delay 0 only when both games roll back) */
    if (g_lockstep && !g_peerdelay) printf("[rbnet] after the start barrier: this game has no snapshot ring (old way, SSBU_RB_NET_PEERDELAY=0): the other game keeps its own delay\n");
    else if (!g_loop && (g_lockstep || peer_lockstep())) printf("[rbnet] after the start barrier: %s snapshot ring: delay-based pacing, both games use input delay %d (announced here %d, there %d; mode byte here %02x, there %02x)\n",
        g_lockstep && peer_lockstep() ? "neither game has a" : g_lockstep ? "this game has no" : "the other game has no", d, g_my_lockd, g_peer_mode >= 0 ? (int)(g_peer_mode & 0x7f) : -1, my_mode(), (unsigned)(g_peer_mode & 0xff));
    else if (!g_loop) printf("[rbnet] after the start barrier: both games have the ring (mode byte here %02x, there %02x)\n", my_mode(), (unsigned)(g_peer_mode & 0xff));
    fflush(stdout);
    if (d == g_delay) return;
    printf("[rbnet] after the start barrier: input delay %d -> %d frames (round trip %.1f ms, best %.1f ms)\n", g_delay, d, g_rtt_us > 0 ? g_rtt_us / 1000.0 : -1.0, g_rtt_min_us > 0 ? g_rtt_min_us / 1000.0 : -1.0);
    fflush(stdout);
    g_delay = d; rb_input_reset(2, g_local, g_delay);
}
int rbn_started(void) { return g_started; }
int rbn_delay(void) { return g_started ? g_delay : 0; }
static void send_pads(void) {
    long long newest = rb_input_local_latest();
    if (newest < 1 || g_loop) return;
    RbMsg m; memset(&m, 0, sizeof m);
    m.type = RB_MSG_PADS; m.player = g_local; m.newest = (int32_t)newest; m.ack = rb_input_remote_latest(g_remote);
    long long from = g_peer_ack + 1; if (newest - from + 1 > RB_MAX_PADS_PER_MSG) from = newest - RB_MAX_PADS_PER_MSG + 1; if (from < 1) from = 1;
    for (long long f = newest; f >= from; f--) { RbPad p; if (!rb_input_local_get((int)f, &p)) break; m.pads[m.count++] = p; }
    long long cf = g_hfinal_upto;
    if (cf > 0 && g_hframe[cf % HR] == cf) { m.check_frame = (int32_t)cf; m.check = (uint32_t)(g_hash[cf % HR] ^ (g_hash[cf % HR] >> 32)); }
    send_msg(&m); g_sent_newest = newest;
}
/* the local player's pad of a real pass (frame f): kept for f + delay and sent */
void rbn_local_pad(long long f, RbPad now) {
    if (g_bot) now = bot_pad(g_bot_seed, f);
    rb_input_local((int)f, now);
    send_pads();
}
/* (hs10) SSBU_RB_NET_DETAIL=1: the pads each pass gave the game and the fighters' frame lines of the last simulation of each
 * frame, written as "d" lines next to the "f" lines of SSBU_RB_NET_LOG. Always: a local check at the finalization (every read
 * of the final simulation of a frame must have got the final, received pad of both players). */
static int g_detail = -1;
static int detail_on(void) { if (g_detail < 0) { const char *e = getenv("SSBU_RB_NET_DETAIL"); g_detail = e && atoi(e) > 0; } return g_detail; }
static RbPad g_acc_pad[2]; static int g_acc_n[2], g_acc_var[2];          /* reads in the pass now running */
#define HR2 512
static RbPad g_upad[HR2][2]; static int g_un[HR2][2], g_uvar[HR2][2];   /* reads of the last simulation of each frame */
static uint8_t g_ulines[HR2][4][40]; static int g_unl[HR2];
static long long g_padcheck_bad, g_padcheck_first = -1;
RbPad rbn_pad(int player, long long f) {
    int pred = 0; RbPad p = rb_input_get(player, (int)f, &pred);
    g_stat_padget++; g_stat_pred += pred;
    if (player >= 0 && player < 2) { if (g_acc_n[player] && !rb_pad_equal(&g_acc_pad[player], &p)) g_acc_var[player] = 1; g_acc_pad[player] = p; g_acc_n[player]++; }
    return p;
}
static void on_check(long long cf, uint32_t ck) {
    if (cf <= 0 || cf > g_hfinal_upto || g_hframe[cf % HR] != cf) { g_peer_check_f = cf; g_peer_check = ck; return; }      /* not final here yet: kept for later */
    uint32_t mine = (uint32_t)(g_hash[cf % HR] ^ (g_hash[cf % HR] >> 32));
    if (mine == ck) g_check_ok++;
    else { g_desync++; if (g_desync_first < 0) { g_desync_first = cf; printf("[rbnet] DESYNC: frame %lld checksum here %08x, the other game %08x\n", cf, mine, ck); fflush(stdout); } }
}
/* (hs15, test) SSBU_RB_NET_LOOP_BOT2=<seed>: the loop test's other player is the bot of that seed (11 = the joiner of a pair) */
static unsigned loop_bot2(void) {
    static int on = -1; static unsigned seed;
    if (on < 0) { const char *e = getenv("SSBU_RB_NET_LOOP_BOT2"); on = e && *e; if (on) seed = (unsigned)strtoul(e, NULL, 0); }
    return on ? seed : g_bot_seed ^ 0x5bd1e995u;
}
static RbPad loop_remote_pad(long long f) {             /* with SSBU_RB_NET_LOOP_BOT2: exactly what that bot sends as a pair's other game (its input delay applied) */
    if (!getenv("SSBU_RB_NET_LOOP_BOT2")) return bot_pad(loop_bot2(), f);
    RbPad z; memset(&z, 0, sizeof z);
    return f <= g_delay ? z : rb_pad_rest(bot_pad(loop_bot2(), f - g_delay));
}
/* main thread, at the safe point: the messages received since the last call */
void rbn_drain(long long cur) {
    InterlockedExchange64(&g_cur, cur);
    if (g_loop && g_started) {                          /* loop test: the bot's pad of frame f arrives at frame f + delay - lat */
        static long long lat2_from = -2; static int lat2;          /* (hs15, test) SSBU_RB_NET_LOOP_LAT2=<frame>,<lat>: the lateness changes at that frame */
        if (lat2_from == -2) { const char *e = getenv("SSBU_RB_NET_LOOP_LAT2"); lat2_from = -1; if (e && *e) sscanf(e, "%lld,%d", &lat2_from, &lat2); }
        static int jit = -2; static unsigned jseed;                /* (hs15, test) SSBU_RB_NET_LOOP_JIT=<seed>: lateness g_loop_lat or one less, changing every 4..20 frames */
        if (jit == -2) { const char *e = getenv("SSBU_RB_NET_LOOP_JIT"); jit = e && *e; if (jit) jseed = (unsigned)strtoul(e, NULL, 0); }
        int lat = lat2_from >= 0 && cur >= lat2_from ? lat2 : g_loop_lat;
        if (jit) { uint32_t h = mix32(jseed * 0x9E3779B9u ^ (uint32_t)(cur / (4 + (mix32(jseed) % 17))) * 0x85EBCA6Bu); lat -= (int)(h & 1); }
        long long upto = cur + g_delay - lat;
        for (long long f = rb_input_remote_latest(g_remote) + 1; f <= upto; f++) rb_input_remote_put(g_remote, (int)f, loop_remote_pad(f));
        return;
    }
    if (rbn_handshake_wanted()) hs_block_compare();
    {   int32_t ts[TSQ]; int nts;
        AcquireSRWLockExclusive(&g_rq_lock); nts = g_tsq_n; memcpy(ts, g_tsq, sizeof(int32_t) * (size_t)nts); g_tsq_n = 0; ReleaseSRWLockExclusive(&g_rq_lock);
        for (int k = 0; k < nts; k++) rb_ts_sample(ts[k]);
    }
    for (;;) {
        static uint8_t b[RQ_MAX]; int n = 0; uint64_t at = 0; long long atf = 0;
        AcquireSRWLockExclusive(&g_rq_lock);
        if (g_rq && g_rq_tail != g_rq_head) { n = g_rq[g_rq_tail].n; at = g_rq[g_rq_tail].at_us; atf = g_rq[g_rq_tail].at_frame; memcpy(b, g_rq[g_rq_tail].b, (size_t)n); g_rq_tail = (g_rq_tail + 1) % RQ_N; }
        ReleaseSRWLockExclusive(&g_rq_lock);
        if (!n) break;
        RbMsg m;
        if (!rb_msg_parse(b, (size_t)n, &m)) { g_rx_bad++; continue; }
        if (m.type != RB_MSG_PADS || !g_started || m.player != g_remote) continue;
        g_peer_started = 1;
        for (int k = 0; k < m.count; k++) rb_input_remote_put(g_remote, m.newest - k, m.pads[k]);
        if (m.ack > g_peer_ack && m.ack <= rb_input_local_latest()) g_peer_ack = m.ack;      /* (hs12: never an acknowledgement of pads not sent yet) */
        if (m.check_frame > 0) on_check(m.check_frame, m.check);
    }
}
int rbn_take_mismatch(void) { int m = rb_input_take_mismatch(); if (m) g_stat_mis++; return m; }
void rbn_put_mismatch(int m) { rb_input_put_mismatch(m); if (m) g_stat_mis--; }      /* (R3 pub1) r1_ring_load refused: the wrong guess stays pending */
long long rbn_confirmed(void) { return rb_input_remote_latest(g_remote); }
int rbn_peer_gone(void) {
    if (g_loop || !g_started) return 0;
    LONG64 last = g_rx_last_ms;
    return last && GetTickCount64() - (ULONGLONG)last > 5000;
}

/* ---------------------------------------------------------------- checksums: fighter_host.c gives each fighter's frame line */
static uint8_t g_lacc[4][40]; static int g_lacc_n;
void rbn_hash_add(const void *p, size_t n) {
    if (g_on <= 0) return;
    if (g_lacc_n < 4 && n <= 40) { memset(g_lacc[g_lacc_n], 0, 40); memcpy(g_lacc[g_lacc_n], p, n); g_lacc_n++; }
    const uint8_t *b = p; while (n--) { g_hacc ^= *b++; g_hacc *= 0x100000001b3ull; }
    g_hlines++;
}
/* the safe point after pass f: the lines since the last call belong to frame f (a re-run pass overwrites the value) */
/* (hs15, pp4: a frame simulated with a wrong pad can leave the fighters' frame lines unchanged, e.g. a button during an
 * animation that ignores it) the frame's checksum also folds the pads the pass gave the game (the last read of each
 * player, whether the reads varied) and the extra fighter fields of rbn_hash_extra: a frame whose final simulation got a
 * wrong pad now differs from the other game's. SSBU_RB_NET_CKPADS=0 = the old checksum (test). */
static int g_ckpads = -1;
int rbn_hash_wanted(void) { return g_on > 0; }
void rbn_hash_extra(const void *p, size_t n) {          /* folded into the frame's checksum only (not a "d" line field) */
    if (g_on <= 0) return;
    if (g_ckpads < 0) { const char *e = getenv("SSBU_RB_NET_CKPADS"); g_ckpads = !(e && e[0] == '0'); }
    if (!g_ckpads) return;
    const uint8_t *b = p; while (n--) { g_hacc ^= *b++; g_hacc *= 0x100000001b3ull; }
}
static uint64_t hash_pads(uint64_t h) {
    if (g_ckpads < 0) { const char *e = getenv("SSBU_RB_NET_CKPADS"); g_ckpads = !(e && e[0] == '0'); }
    if (!g_ckpads) return h;
    for (int q = 0; q < 2; q++) {
        struct { uint64_t buttons; int16_t lx, ly, rx, ry; uint32_t read, varied; } r;
        memset(&r, 0, sizeof r);
        if (g_acc_n[q]) { r.buttons = g_acc_pad[q].buttons; r.lx = g_acc_pad[q].lx; r.ly = g_acc_pad[q].ly; r.rx = g_acc_pad[q].rx; r.ry = g_acc_pad[q].ry; r.read = 1; r.varied = (uint32_t)g_acc_var[q]; }
        const uint8_t *b = (const uint8_t *)&r; for (size_t n = 0; n < sizeof r; n++) { h ^= b[n]; h *= 0x100000001b3ull; }
    }
    return h;
}
void rbn_pass_hash(long long f) {
    if (g_hlines && f > 0) { g_hash[f % HR] = hash_pads(g_hacc); g_hframe[f % HR] = f; }
    if (f > 0) { int x = (int)(f % HR2);
        for (int q = 0; q < 2; q++) { g_upad[x][q] = g_acc_pad[q]; g_un[x][q] = g_acc_n[q]; g_uvar[x][q] = g_acc_var[q]; }
        memcpy(g_ulines[x], g_lacc, sizeof g_lacc); g_unl[x] = g_lacc_n; }
    memset(g_acc_n, 0, sizeof g_acc_n); memset(g_acc_var, 0, sizeof g_acc_var); g_lacc_n = 0;
    g_hacc = 0xcbf29ce484222325ull; g_hlines = 0;
}
static void detail_frame(long long f) {              /* frame f is final: the pads its last simulation used against the final ones */
    int x = (int)(f % HR2);
    for (int q = 0; q < 2; q++) {
        RbPad fin; int pred = 0;
        if (q == g_local) { if (!rb_input_local_get((int)f, &fin)) memset(&fin, 0, sizeof fin); }
        else fin = rb_input_get(q, (int)f, &pred);
        int bad = g_un[x][q] && (!rb_pad_equal(&g_upad[x][q], &fin) || g_uvar[x][q]);
        if (bad || pred) { g_padcheck_bad++; if (g_padcheck_first < 0) g_padcheck_first = f;
            if (g_padcheck_bad <= 20) { printf("[rbnet] PAD CHECK: frame %lld player %d: its last simulation got %llx %d %d (reads %d, varied %d), the final pad is %llx %d %d%s\n", f, q + 1,
                (unsigned long long)g_upad[x][q].buttons, g_upad[x][q].lx, g_upad[x][q].ly, g_un[x][q], g_uvar[x][q], (unsigned long long)fin.buttons, fin.lx, fin.ly, pred ? " (NOT received)" : ""); fflush(stdout); } }
    }
    if (g_hlog && detail_on()) {
        fprintf(g_hlog, "d %lld p1 %llx %d %d %d %d n%d p2 %llx %d %d %d %d n%d", f, (unsigned long long)g_upad[x][0].buttons, g_upad[x][0].lx, g_upad[x][0].ly, g_upad[x][0].rx, g_upad[x][0].ry, g_un[x][0],
                (unsigned long long)g_upad[x][1].buttons, g_upad[x][1].lx, g_upad[x][1].ly, g_upad[x][1].rx, g_upad[x][1].ry, g_un[x][1]);
        for (int l = 0; l < g_unl[x]; l++) { const uint8_t *b = g_ulines[x][l]; int k, st; uint64_t mot, rh; float mf, px, py, dmg;
            memcpy(&k, b, 4); memcpy(&st, b + 4, 4); memcpy(&mot, b + 8, 8); memcpy(&mf, b + 16, 4); memcpy(&px, b + 20, 4); memcpy(&py, b + 24, 4); memcpy(&dmg, b + 28, 4); memcpy(&rh, b + 32, 8);
            fprintf(g_hlog, " | e%d st %x mot %llx mf %.3f x %.4f y %.4f dmg %.3f rng %016llx", k, st, (unsigned long long)mot, mf, px, py, dmg, (unsigned long long)rh); }
        fputc('\n', g_hlog);
    }
}
/* no rollback pending: every frame up to min(cur, newest remote pad) is final */
void rbn_finalize(long long cur) {
    long long upto = rb_input_remote_latest(g_remote);
    if (upto > cur) upto = cur;
    for (long long f = g_hfinal_upto + 1; f <= upto; f++) {
        if (g_hlog && g_hframe[f % HR] == f) fprintf(g_hlog, "f %lld h %016llx\n", f, (unsigned long long)g_hash[f % HR]);
        detail_frame(f);
        g_hfinal_upto = f;
    }
    if (g_hlog && (upto & 63) == 0) fflush(g_hlog);
    if (g_peer_check_f > 0 && g_peer_check_f <= g_hfinal_upto) { long long cf = g_peer_check_f; g_peer_check_f = 0; on_check(cf, g_peer_check); }
}

/* ---------------------------------------------------------------- time sync: extra wait per frame (ms) */
static double g_tsbias = -1;
double rbn_pace_extra(long long f) {
    if (!g_started || g_loop) return 0;
    if (f % 30 == 0) {
        rb_ts_decide((int)f);
        double est_ms = rb_ts_offset() / 1000.0;
        /* (hs14) delay-based pacing (one game without the ring): that game aims at running half a frame BEHIND the other
         * (SSBU_RB_NET_TSBIAS ms, default 8), so the other player's pad of its frame is there before it needs it (a stall there
         * is a hitch); the game with the ring aims at the same split from its side */
        if (g_tsbias < 0) { const char *e = getenv("SSBU_RB_NET_TSBIAS"); g_tsbias = e ? atof(e) : 8.0; }
        if (g_peerdelay && g_lockstep) est_ms += g_tsbias; else if (peer_lockstep()) est_ms -= g_tsbias;
        /* ahead: half of the lead given back over the next 30 frames (the other game, behind, does nothing; both halves meet) */
        if (est_ms > 2.0) { g_ts_extra = est_ms / 2.0 / 30.0; if (g_ts_extra > 4.0) g_ts_extra = 4.0; g_ts_left = 30; g_ts_n++; }
        else g_ts_left = 0;
    }
    if (g_ts_left > 0) { g_ts_left--; return g_ts_extra; }
    return 0;
}
int rbn_rtt_ms(void) { return g_rtt_us > 0 ? (int)(g_rtt_us / 1000) : -1; }
void rbn_resend(void) { if (g_started) send_pads(); }
/* the match-start barrier: an empty PADS message says "in the match" (count 0, newest 0) */
void rbn_hello(void) {
    if (!g_started || g_loop) return;
    { static int n; if (rbn_handshake_wanted() && n++ < 20 && (n % 4) == 0) hs_block_send(); }   /* (the block check again, a few times) */
    if (rb_input_local_latest() >= 1) { send_pads(); return; }
    RbMsg m; memset(&m, 0, sizeof m); m.type = RB_MSG_PADS; m.player = g_local; m.newest = 0; m.count = 0; send_msg(&m);
}
int rbn_peer_started(void) { return g_loop || g_peer_started; }
void rbn_status(char *out, size_t n) {
    LONG q = (g_rq_head - g_rq_tail + RQ_N) % RQ_N;
    snprintf(out, n, "remote pads up to %d, local up to %d, the other game has ours up to %lld; packets sent %lld received %lld (bad %lld, queue %ld, full %lld)",
             rb_input_remote_latest(g_remote), rb_input_local_latest(), g_peer_ack, (long long)g_tx_pkts, (long long)g_rx_pkts, (long long)g_rx_bad, (long)q, (long long)g_rx_drop);
}
double rbn_offset_ms(void) { return rb_ts_offset() / 1000.0; }

/* (hs12) the end of a match (rb_rollback.c, the first pass after the fight is off): everything of the session that
 * belongs to one match starts over (input queues and frames at the next rbn_start, checksums, counters, the block check,
 * received messages not drained yet); the round trip, the pings and the transport stay. */
void rbn_match_reset(void) {
    g_started = 0; g_peer_started = 0; g_f0 = 0; InterlockedExchange(&g_peer_mode, -1); g_peer_ack = 0; g_sent_newest = 0;
    g_stat_pred = g_stat_padget = g_stat_mis = 0; g_ts_n = 0; g_ts_extra = 0; g_ts_left = 0;
    g_hacc = 0xcbf29ce484222325ull; g_hlines = 0; g_lacc_n = 0;
    memset(g_hash, 0, sizeof g_hash); memset(g_hframe, 0, sizeof g_hframe); g_hfinal_upto = 0;
    g_desync = 0; g_check_ok = 0; g_desync_first = -1; g_peer_check_f = 0; g_peer_check = 0;
    memset(g_acc_n, 0, sizeof g_acc_n); memset(g_acc_var, 0, sizeof g_acc_var); memset(g_un, 0, sizeof g_un); memset(g_unl, 0, sizeof g_unl);
    g_padcheck_bad = 0; g_padcheck_first = -1;
    AcquireSRWLockExclusive(&g_hs_lock); g_hs_mine_ok = 0; g_hs_blk_done = 0; memset(g_hs_peer_have, 0, sizeof g_hs_peer_have); ReleaseSRWLockExclusive(&g_hs_lock);
    AcquireSRWLockExclusive(&g_rq_lock); g_rq_tail = g_rq_head; g_tsq_n = 0; ReleaseSRWLockExclusive(&g_rq_lock);
    InterlockedExchange64(&g_cur, 0);
    if (g_hs_cancelled) menu_notice(NULL);
    InterlockedExchange(&g_hs_cancelled, 0);
    printf("[rbnet] match %d over: the session is ready for the next match (pads of an older match dropped so far: %lld)\n", g_match_n, (long long)g_rx_oldmatch);
    fflush(stdout);
}
int rbn_match_number(void) { return g_match_n; }
void rbn_report(FILE *o, long long frames) {
    if (g_on <= 0) return;
    if (g_hlog) fflush(g_hlog);
    fprintf(o, "[rbnet] report (match %d, id %08x): %lld frames; delay %d; round trip mean %.1f ms (best %.1f, %lld samples); clock offset estimate %.1f ms, time-sync corrections %lld\n",
            g_match_n, g_match_id, frames, g_delay, g_rtt_n ? g_rtt_sum / g_rtt_n / 1000.0 : -1.0, g_rtt_min_us > 0 ? g_rtt_min_us / 1000.0 : -1.0, g_rtt_n, rb_ts_offset() / 1000.0, g_ts_n);
    fprintf(o, "[rbnet] report: pads given %lld, predicted %lld (%.1f%%), mispredictions %lld; finalized frames %lld; checksums compared %lld, DESYNCS %lld (first %lld)\n",
            g_stat_padget, g_stat_pred, g_stat_padget ? 100.0 * g_stat_pred / g_stat_padget : 0.0, g_stat_mis, g_hfinal_upto, g_check_ok + g_desync, g_desync, g_desync_first);
    fprintf(o, "[rbnet] report: pad check of the final simulations: %lld pad values differ from the final ones (first frame %lld)\n", g_padcheck_bad, g_padcheck_first);
    fprintf(o, "[rbnet] report: packets sent %lld (failed %lld), received %lld (bad %lld, queue full %lld); remote pads up to %d, local up to %d\n",
            (long long)g_tx_pkts, (long long)g_tx_fail, (long long)g_rx_pkts, (long long)g_rx_bad, (long long)g_rx_drop, rb_input_remote_latest(g_remote), rb_input_local_latest());
    fflush(o);
}
