/* debug_mode.c - TEST / DEBUG FAST MODE (test-only; every feature is behind an environment variable, none set = no effect).
 * See tests\DEBUG_MODE.md. Called once from boot.c before nnMain (debug_mode_init). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

uint64_t hook_address(const char *name);              /* fighter_sdk\hook_host.c (included by nnro.c) */

static int safe_read(uint64_t addr, void *buf, size_t n) {
    SIZE_T got = 0;
    if (addr < 0x10000) return 0;
    return ReadProcessMemory(GetCurrentProcess(), (const void *)(uintptr_t)addr, buf, n, &got) && got == n;
}
static uint64_t sr64(uint64_t a) { uint64_t v = 0; return safe_read(a, &v, 8) ? v : 0; }

/* ---- the game's fighter entries (layout read off the lua_bind accessors of the game's own binary):
 *   FighterManager singleton -> *mgr = impl; impl + 0x20 + 8*entry_id = FighterEntry*; entry + 0xf0 = FighterInformation;
 *   *(information + 8) = the information record (hit point 0x38, colour 0x51, cpu flag 0x92, stock 0xd8, category 0x3ac). */
static uint64_t fighter_info_record(int id) {
    static uint64_t inst;
    if (!inst) { uint64_t a = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E"); if (!a) return 0; inst = a; }
    uint64_t mgr = sr64(inst), impl = mgr ? sr64(mgr) : 0, entry = impl ? sr64(impl + 0x20 + 8 * (uint64_t)id) : 0;
    uint64_t info = entry ? entry + 0xf0 : 0;
    return info ? sr64(info + 8) : 0;
}

/* SSBU_DEBUG_SCAN=1: once, when entry 1's record exists, every other copy of the pattern in it (HP 999.0 followed by
 * ff bytes, record + 0x40) in committed memory is printed with 0x60 bytes in front of it: finds the structure the record was built from. */
static void scan_copies(uint64_t rec) {
    static const uint8_t pat[16] = {0x00, 0xc0, 0x79, 0x44, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t *p = (uint8_t *)si.lpMinimumApplicationAddress;
    MEMORY_BASIC_INFORMATION mi;
    int hits = 0;
    while (p < (uint8_t *)si.lpMaximumApplicationAddress && VirtualQuery(p, &mi, sizeof mi)) {
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_READONLY)) && mi.RegionSize < (1ull << 31)) {
            const uint8_t *b = mi.BaseAddress;
            for (size_t i = 0; i + 16 <= mi.RegionSize; i += 4)
                if (b[i] == 0x00 && b[i + 1] == 0xc0 && memcmp(b + i, pat, 16) == 0 && (uint64_t)(b + i) != rec + 0x40 && hits < 12) {
                    hits++;
                    printf("debug scan: copy at %p (record base would be %p), region %p+%llx\n", b + i, b + i - 0x40, mi.BaseAddress, (unsigned long long)mi.RegionSize);
                    if (i >= 0x40) for (int k = -0x40; k < 0x40; k += 16) {
                        const uint8_t *q = b + i + k;
                        printf("debug scan:   %+04d %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n", k, q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9], q[10], q[11], q[12], q[13], q[14], q[15]);
                    }
                }
        }
        p = (uint8_t *)mi.BaseAddress + mi.RegionSize;
    }
    printf("debug scan: %d copies besides the record itself%c", hits, 10);
    fflush(stdout);
}

/* SSBU_DEBUG_WATCH=1: hardware write watchpoints (debug registers of every thread) on a few fields of the first two
 * records; each write prints the guest functions on the stack of the writer (finds the code that fills the record). */
#include <tlhelp32.h>
/* SSBU_DEBUG_MATCH="p1=<fighter>,p2=<fighter>,stage=<name>,form=omega,cpu=<level>,items=off|on,time=<min>|stock=<n>" */
static struct { int p[4]; char stage[48]; int omega, cpu, items, time, stock; char text[256]; } g_dm = {{-1, -1, -1, -1}, "", 0, -1, -1, -1, -1, ""};
static char g_dm_name[4][32];                          /* track V session 14: p3 / p4 (CPU players added to the panel states the screens left empty) */
static uint8_t g_dm_state1[0x230]; static int g_dm_state1_ok;      /* player 2's panel state as the game's function saw it: the template for p3 / p4 */
static int g_dm_p2type = -1, g_dm_applied, g_dm_fight, g_dm_form = -1, g_dm_hazards = -1;
static FILE *g_dm_list;
static uint64_t fighter_info_record(int id);
static void hex_lines(const char *tag, uint64_t base, int n) {
    for (int k = 0; k < n; k += 16) {
        uint8_t b[16]; if (!safe_read(base + k, b, 16)) break;
        int any = 0; for (int j = 0; j < 16; j++) any |= b[j];
        if (any) printf("%s +%03x %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n", tag, k, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
    }
}

/* ---- SSBU_DEBUG_END="<n>s" (seconds of fight = n * 60 game frames) or "<n>" (frames): the frames presented while the game's
 * own FighterManager says the fight is on (is_ready_go, impl + 0xd2) are counted (display.c calls debug_mode_frame once per
 * presented frame); at the limit the exit request of a closed window is made (the normal shutdown path). */
int oe_exit_request(void);                             /* nnmisc.c */
static volatile LONG64 g_end_limit, g_end_fight_frames, g_end_fired;
static volatile LONG64 g_end_first_frame;
void debug_mode_frame(long long frame) {
    if (!g_end_limit && !g_dm.text[0]) return;
    uint64_t inst = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
    uint64_t mgr = inst ? sr64(inst) : 0, impl = mgr ? sr64(mgr) : 0;
    uint8_t go = 0;
    static int was_go;
    if (!impl || !safe_read(impl + 0xd2, &go, 1) || !go) {
        if (was_go) { printf("debug match: fight off (ready-go cleared) at presented frame %lld after %lld fight frames\n", frame, (long long)g_end_fight_frames); fflush(stdout); }
        was_go = 0; return;
    }
    if (!was_go) { was_go = 1; InterlockedExchange64(&g_end_fight_frames, 0); }
    LONG64 n = InterlockedIncrement64(&g_end_fight_frames);
    if (n == 1) {
        g_end_first_frame = frame;
        uint64_t sel = GA(0x71053097c0ULL) + 0x20ull * (rd32(GA(0x7105309968ULL)) < 3 ? rd32(GA(0x7105309968ULL)) : 0);
        uint64_t r0 = fighter_info_record(0), r1 = fighter_info_record(1);
        printf("debug match: fight %d on (the game's ready-go) at presented frame %lld: p1 kind %d (%s), p2 kind %d (%s) type %d cpu power %d | stage id 0x%x (match block + 0x420), chosen ui_stage %llx form %d (0 normal, 1 battlefield, 2 omega) flags %x/%x (%s) | rule: stock %d, item appear %d; stocks in the fighters' records %d / %d\n", g_dm_fight, frame,
               (int)(int8_t)rd8(GA(0x71052c4738ULL)), g_dm_name[0], (int)(int8_t)rd8(GA(0x71052c4738ULL) + 0x260), g_dm_name[1], rd8(GA(0x71052c4730ULL) + 0x260 + 0xa), rd8(GA(0x71052c4730ULL) + 0x260 + 0x79),
               rd32(GA(0x71052c41b0ULL) + 0x420), (unsigned long long)rd64(sel), rd32(sel + 8), rd32(sel + 0xc), rd8(GA(0x7105309985ULL)), g_dm.stage,
               rd8(GA(0x71052c41b0ULL) + 0x4c), rd32(GA(0x71052c41b0ULL) + 0x54), r0 ? (int)rd32(r0 + 0xd8) : -1, r1 ? (int)rd32(r1 + 0xd8) : -1);
        hex_lines("debug match:   stage block", GA(0x71052c41b0ULL) + 0x420, 0x30);
        if (getenv("SSBU_DEBUG_CTRLSCAN")) {              /* (R3 lwr, research) =<profile>: every copy of that profile's control bytes + 0x24 .. + 0x4c in memory */
            uint64_t x0 = sr64(GA(0x7105313510ULL)), x = x0 ? sr64(x0) : 0, a = x ? sr64(x + 0x58) : 0, b = a ? sr64(a) : 0, q = b ? sr64(b) : 0;
            uint8_t pat[0x10]; uint64_t src = q + 0xf7d8ull * (uint64_t)atoi(getenv("SSBU_DEBUG_CTRLSCAN")) + 0x30;
            if (q && safe_read(src, pat, sizeof pat)) {
                SYSTEM_INFO si; GetSystemInfo(&si); uint8_t *p = (uint8_t *)si.lpMinimumApplicationAddress; MEMORY_BASIC_INFORMATION mi; int hits = 0;
                while (p < (uint8_t *)si.lpMaximumApplicationAddress && VirtualQuery(p, &mi, sizeof mi)) {
                    if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && mi.RegionSize < (1ull << 31)) {
                        const uint8_t *bb = mi.BaseAddress;
                        for (size_t i = 0; i + sizeof pat <= mi.RegionSize; i++) if (bb[i] == pat[0] && !memcmp(bb + i, pat, sizeof pat) && (uint64_t)(bb + i) != src && hits < 30) { hits++; printf("debug ctrl: scan: copy of the profile's bytes + 0x30 at %p (region %p)\n", bb + i, mi.BaseAddress); hex_lines("debug ctrl: scan:   ", (uint64_t)(bb + i) - 0x40, 0x80); }
                    }
                    p = (uint8_t *)mi.BaseAddress + mi.RegionSize;
                }
                printf("debug ctrl: scan: %d copies (profile bytes at %llx)\n", hits, (unsigned long long)src);
                {   uint64_t base = GA(0x7105338860ULL); for (int s = 0; s < 3; s++) { uint64_t o = sr64(base + 8ull * s); printf("debug ctrl: slot %d obj %llx subs %llx %llx %llx %llx\n", s, (unsigned long long)o, (unsigned long long)sr64(o + 0x10), (unsigned long long)sr64(o + 0x18), (unsigned long long)sr64(o + 0x20), (unsigned long long)sr64(o + 0x28)); } }
                fflush(stdout);
            } }
        if (getenv("SSBU_DEBUG_CTRLWATCH")) hex_lines("debug ctrl: singleton at the fight start", sr64(GA(0x71052c40f0ULL)), 0x120);
        if (getenv("SSBU_DEBUG_P1PROFILE_LATE")) {        /* (R3 lwr, research) =<from>,<to>: profile from's control bytes over profile to's, at the fight start */
            int from = 0, to = 1; sscanf(getenv("SSBU_DEBUG_P1PROFILE_LATE"), "%d,%d", &from, &to);
            uint64_t x0 = sr64(GA(0x7105313510ULL)), x = x0 ? sr64(x0) : 0, a = x ? sr64(x + 0x58) : 0, b = a ? sr64(a) : 0, q = b ? sr64(b) : 0;
            if (q) for (int k = 0x24; k < 0x50; k++) wr8(q + 0xf7d8ull * (uint64_t)to + k, rd8(q + 0xf7d8ull * (uint64_t)from + k));
            printf("debug ctrl: research: at the fight start profile %d's control bytes copied over profile %d\n", from, to); }
        if (getenv("SSBU_DEBUG_MIDUMP")) { hex_lines("debug midump: fight on: FighterManager impl", impl, 0x200); hex_lines("debug midump: fight on: rule", GA(0x71052c41b0ULL), 0x580);      /* (R3 lwr, research) */
            for (int e = 0; e < 2; e++) { char t[64]; uint64_t en = sr64(impl + 0x20 + 8ull * e); snprintf(t, sizeof t, "debug midump: fight on: entry%d", e); hex_lines(t, en, 0x400);
                snprintf(t, sizeof t, "debug midump: fight on: info%d", e); hex_lines(t, fighter_info_record(e), 0x400); } }
        fflush(stdout);
    }
    if (g_end_limit && n >= g_end_limit) InterlockedExchange64(&g_end_fired, frame);
}
static int fighter_kind_by_name(const char *name) {
    char *e; long v = strtol(name, &e, 0);
    if (*name && !*e) return (int)v;
    FILE *f = fopen("..\\reference\\skyline-smash\\src\\lua_const.rs", "r");
    if (!f) return -1;
    char key[96], line[256]; snprintf(key, sizeof key, "FIGHTER_KIND_%s:", name);
    for (char *p = key; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    int kind = -1;
    while (fgets(line, sizeof line, f)) { char *s = strstr(line, key); if (s && s - line < 8) { kind = (int)strtol(strchr(s, ':') + 1, NULL, 0); break; } }
    fclose(f);
    return kind;
}
static void parse_match(const char *s) {
    snprintf(g_dm.text, sizeof g_dm.text, "%s", s);
    char buf[256]; snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
        char *eq = strchr(t, '='); if (!eq) continue;
        *eq = 0; const char *k = t, *v = eq + 1;
        if (!strcmp(k, "p1")) { g_dm.p[0] = fighter_kind_by_name(v); snprintf(g_dm_name[0], sizeof g_dm_name[0], "%s", v); }
        else if (!strcmp(k, "p2") && (!strcmp(v, "cpu") || !strcmp(v, "human"))) g_dm_p2type = !strcmp(v, "cpu");
        else if (!strcmp(k, "p2")) { g_dm.p[1] = fighter_kind_by_name(v); snprintf(g_dm_name[1], sizeof g_dm_name[1], "%s", v); }
        else if (!strcmp(k, "p3") || !strcmp(k, "p4")) { int i = k[1] - '1'; g_dm.p[i] = fighter_kind_by_name(v); snprintf(g_dm_name[i], sizeof g_dm_name[i], "%s", v); }
        else if (!strcmp(k, "stage")) snprintf(g_dm.stage, sizeof g_dm.stage, "%s", v);
        else if (!strcmp(k, "form")) { g_dm.omega = !strcmp(v, "omega"); g_dm_form = !strcmp(v, "omega") ? 2 : !strcmp(v, "battlefield") ? 1 : 0; }
        else if (!strcmp(k, "hazards")) g_dm_hazards = !strcmp(v, "on");
        else if (!strcmp(k, "cpu")) g_dm.cpu = atoi(v);
        else if (!strcmp(k, "items")) g_dm.items = !strcmp(v, "on");
        else if (!strcmp(k, "time")) g_dm.time = atoi(v);
        else if (!strcmp(k, "stock")) g_dm.stock = atoi(v);
    }
    printf("debug match: requested %s -> p1 kind %d, p2 kind %d, stage %s%s\n", s, g_dm.p[0], g_dm.p[1], g_dm.stage, g_dm.omega ? " (omega)" : "");
}
static void match_player_hook(Ctx *c, void *user);
static ImportFn g_orig_player_record;
static volatile LONG g_dm_setups;                      /* match set-ups seen (player 1's record built) */
#ifdef _WIN32
static LONG CALLBACK watch_veh(EXCEPTION_POINTERS *ep) {
    static volatile LONG shown;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT *cx = ep->ContextRecord;
    if (!(cx->Dr6 & 0xf)) return EXCEPTION_CONTINUE_SEARCH;
    int which = 0; while (!((cx->Dr6 >> which) & 1)) which++;
    uint64_t addr = (&cx->Dr0)[which];
    /* Tried and NOT working: overwriting the record's fighter field (record + 0x30 = kind + 1) here changes nothing in the
     * fight (run m1: record said 6 and 9, the fighters spawned were still kinds 0 and 8): the record is a copy made from
     * the select-screen data, not the source. */
    if (InterlockedIncrement(&shown) <= 160) {
        char line[1200]; int n = snprintf(line, sizeof line, "debug watch: write to %llx (reg %d) now %016llx by thread %lu, guest stack:", (unsigned long long)addr, which, (unsigned long long)*(uint64_t *)(uintptr_t)(addr & ~7ull), GetCurrentThreadId());
        CONTEXT c2 = *cx; uint64_t last = 0;
        for (int depth = 0, sh = 0; depth < 400 && sh < 14; depth++) {
            uint64_t g = guest_function_of(c2.Rip);
            if (g && g != last) { n += snprintf(line + n, sizeof line - n, " f_%llx", (unsigned long long)g); sh++; last = g; }
            DWORD64 ib; RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(c2.Rip, &ib, NULL);
            if (!rf) break;
            void *hd; DWORD64 est;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, ib, c2.Rip, rf, &c2, &hd, &est, NULL);
            if (!c2.Rip) break;
        }
        {   /* the guest's own frame chain: one of the host registers holds the Ctx of this thread (x29 = frame pointer, x30 = link) */
            const DWORD64 *regs = &cx->Rax; int found = 0;
            for (int r = 0; r < 16 && !found; r++) {
                uint64_t p = regs[r], x[32];
                if (r == 4 || !safe_read(p, x, sizeof x)) continue;
                uint64_t fp = x[29], sp = x[31], pair[2];
                if (fp < sp || fp - sp > 0x100000 || (x[30] >> 32) < 0x71 || (x[30] >> 32) > 0x72 || !safe_read(fp, pair, 16) || (pair[1] >> 32) < 0x71 || (pair[1] >> 32) > 0x72) continue;
                found = 1;
                n += snprintf(line + n, sizeof line - n, " | guest pc near lr %llx, frames:", (unsigned long long)x[30]);
                for (int d = 0; d < 10 && safe_read(fp, pair, 16) && (pair[1] >> 32) >= 0x71 && (pair[1] >> 32) <= 0x72; d++) { n += snprintf(line + n, sizeof line - n, " %llx", (unsigned long long)pair[1]); if (pair[0] <= fp) break; fp = pair[0]; }
            }
        }
        printf("%s\n", line); fflush(stdout);
    }
    cx->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void watch_set(const uint64_t *addrs, int n) {
    static int veh;
    if (!veh) { veh = 1; AddVectoredExceptionHandler(1, watch_veh); }
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te; te.dwSize = sizeof te;
    int set = 0;
    if (snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te)) do {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId()) continue;
        HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
        if (!th) continue;
        SuspendThread(th);
        CONTEXT cx; memset(&cx, 0, sizeof cx); cx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext(th, &cx)) {
            DWORD64 *dr = &cx.Dr0; DWORD64 d7 = 0;
            for (int i = 0; i < n && i < 4; i++) { dr[i] = addrs[i]; d7 |= (1ull << (2 * i)) | (1ull << (16 + 4 * i)) | (3ull << (18 + 4 * i)); }
            cx.Dr7 = d7;
            if (SetThreadContext(th, &cx)) set++;
        }
        ResumeThread(th); CloseHandle(th);
    } while (Thread32Next(snap, &te));
    CloseHandle(snap);
    printf("debug watch: %d watchpoints on %d threads\n", n, set); fflush(stdout);
}
#else
static void watch_set(const uint64_t *addrs, int n) {      /* Linux build: no debug registers from user space without ptrace */
    (void)addrs;
    printf("debug watch: %d watchpoints requested: not available in the Linux build\n", n); fflush(stdout);
}
#endif

/* research (R3, SSBU_RB_NET_WATCH=1 only): write watchpoints on up to 4 addresses from another file (fighter_host.c) */
static uint64_t g_dw_a[4]; static int g_dw_n;
static DWORD WINAPI dw_thread(LPVOID u) { (void)u; watch_set(g_dw_a, g_dw_n); return 0; }
void debug_watch(const uint64_t *addrs, int n) {        /* from a helper thread, so that the calling thread is watched too */
    g_dw_n = n > 4 ? 4 : n; memcpy(g_dw_a, addrs, sizeof(uint64_t) * (size_t)g_dw_n);
    HANDLE t = CreateThread(NULL, 0, dw_thread, NULL, 0, NULL); if (t) { WaitForSingleObject(t, 5000); CloseHandle(t); }
}
int hook_install(uint64_t addr, void (*repl)(Ctx *, void *), void *user, ImportFn *orig);

/* ---- the match set-up (from the game's own binary; see tests\DEBUG_MODE.md) */
#define MELEE_INFO   GA(0x71052c41b0ULL)                 /* the game's global match block (rule at +0, players at +0x588, 0x260 each) */
#define MI_PLAYER(i) (MELEE_INFO + 0x588 + 0x260ull * (uint64_t)(i))
#define FN_ENTRY_SETUP GA(0x7100653240ULL)               /* (FighterEntry *, init parameter *, flags *): copies 0x70 bytes to entry + 0x70 */
static ImportFn g_orig_entry_setup;
static void research_entry_setup(Ctx *c, void *user) {
    (void)user;
    printf("debug research: entry set-up: entry %llx, parameter %llx, called from %llx\n", (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[30]);
    hex_lines("debug research:   param", c->x[1], 0x70);
    hex_lines("debug research:   rule ", MELEE_INFO, 0x588);
    hex_lines("debug research:   pl 0 ", MI_PLAYER(0), 0x260);
    hex_lines("debug research:   pl 1 ", MI_PLAYER(1), 0x260);
    fflush(stdout);
    g_orig_entry_setup(c);
}

/* ---- SSBU_DEBUG_MATCH, route A: the function that turns one player's choice on the select screens (a panel state) into
 * that player's record of the global match block. x0 = panel state, x1 = player index, x3 = record (MELEE_INFO + 0x580 + 0x260 * i).
 * State: +0x78 player type (0 human, 1 CPU, 2 amiibo, 3 none), +0x88 the ui_chara id (0xc1 << 56 | hash40("ui_chara_<name>")),
 * +0x90 colour, +0x224 CPU level. Record: +0 ui_chara id, +8 fighter kind, +0xa type, +0x19 colour, +0x79 CPU level.
 * The choice is replaced in the STATE before the game's own function runs, so everything it derives is the game's. */
#define FN_PLAYER_RECORD GA(0x7101788260ULL)
static uint64_t hash40(const char *s) {
    uint32_t crc = 0xffffffffu; size_t n = strlen(s);
    for (size_t i = 0; i < n; i++) { crc ^= (uint8_t)s[i]; for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1))); }
    return ((uint64_t)n << 32) | (uint32_t)~crc;
}
/* ---- R3 online rollback (SSBU_RB_NET=net, rb_net.c): the match handshake. At player 1's record each game sends its own
 * player-1 choice (the panel state as the screens and SSBU_DEBUG_MATCH left it); the host adds its stage / items / stocks
 * and a seed; both then write the agreed match into the panel states of players 1 and 2 (type human: port 2 = the other
 * game) and the stage choice, before the game's own function builds the records. Fields read in the record builder
 * 0x7101788260 (Ghidra, 13.0.4): state + 0x90 colour -> record + 0x19 / + 0x1b; the control layout -> record + 0x28 ..
 * + 0x3d comes from state + 0x54 .. + 0x69 when state + 0x454 is set (or + 0x455 with type 0), else for a human from the
 * name-tag profile state + 0x214 (table [[[[0x7105313510] + 0x58]]] + 0xf7d8 * profile + 0xc, default 0x7105313518).
 * Both games force + 0x454 = 1 with the agreed layout of each player, so a player's own controls are the same on both PCs. */
#include "rollback/services/handshake.h"
static int g_hs_wanted, g_hs_ok, g_hs_local, g_hs_lwr;      /* (R3 lwr) g_hs_lwr: a Local Wireless room's match played with rollback */
static RbhMatch g_hs_m;
/* (hs12) player 1's panel state as the game's function got it (before any change here): the template of player 2's state
 * when the select screen had NO panel for player 2 (each player picks only his own fighter; the other game's player is
 * player 2 / 1). Fields read off two panel states (run pd1: player 1 human, player 2 CPU, both built by 0x7101787830 from
 * the screens' per-player table 0x7105307770 + 0x1c8 * i): + 0x70 the player's number (1, 2), + 0x78 type, + 0x7c team,
 * + 0x80 / + 0x88 ui_chara, + 0x90 colour, + 0x91 CPU level, + 0x210 / + 0x214 name-tag fields (player 1: 0 / 1, the CPU:
 * ffffffff / 0), + 0x298 the number again (the second copy of the table entry), + 0x450 "human with a name tag" (1 / 0). */
static uint8_t g_hs_p1[0x470]; static int g_hs_p1_ok;
static void hs_fill_empty(uint64_t st, int idx) {
    for (int k = 0; k < (int)sizeof g_hs_p1; k++) wr8(st + (uint64_t)k, g_hs_p1[k]);
    wr32(st + 0x70, (uint32_t)idx + 1); if (rd32(st + 0x298) == 1) wr32(st + 0x298, (uint32_t)idx + 1);
    wr32(st + 0x7c, 0); wr32(st + 0x210, 0xffffffffu); wr32(st + 0x214, 0); wr8(st + 0x450, 0);
}
/* (R3 lwr) a name-tag profile (0xf7d8 bytes each, 0x3d of them): as the record builder 0x7101788260 finds it (0x71017884 5c..80):
 * [[0x7105313510]] = the save's root (byte + 0 must be 0), [[[root + 0x58]]] + 0xf7d8 * index; a profile whose byte + 0 is 0
 * (not in use) or index -1 / >= 0x3d -> the game's default profile 0x7105313518. Read in runs lwc1v / lwc2v (SSBU_DEBUG_CTRLDUMP):
 * + 0 in use, + 0xc the name (UTF-16), + 0x24 .. + 0x4e the controls (three controller kinds' button maps and options; the
 * builder reads the kind's last byte + 0x2f / + 0x3f / + 0x4b into record + 0x48). (hs12 read one pointer too few: it always
 * got the default profile.) */
static uint64_t prof_ptr(uint32_t prof) {
    uint64_t def = GA(0x7105313518ULL);
    if (prof >= 0x3d) return def;
    uint64_t x0 = sr64(GA(0x7105313510ULL)), x = x0 ? sr64(x0) : 0;
    if (!x || rd8(x) != 0) return def;
    uint64_t a = sr64(x + 0x58), b = a ? sr64(a) : 0, q = b ? sr64(b) : 0;
    uint64_t p = q ? q + 0xf7d8ull * prof : 0;
    return p && rd8(p) ? p : def;
}
/* (R3 lwr, owner 2026-10-22: each player plays with HIS OWN name-tag controls) the controls a match uses: the set-up
 * 0x7101780440 (after the records, from 0x710155ed20) copies, for each human player i, its profile's + 0x24 .. + 0x4e (record
 * + 0x40 = the profile) into the controls object [0x71052c40f0] + 0x18 + 0x50 * i, and binds the player to its controller slot
 * (record + 0x1c, 0x7101754d80); the fight reads only that copy (runs lwc_c0 / lwc_late: profile 0's bytes over player 1's
 * profile at its record change the fight, at the fight start they do not; lwc_w: hardware write watch on the copy). The
 * builder also takes ONE option byte of the controller kind into record + 0x48 (profile + 0x2f GameCube, + 0x3f Pro / two
 * Joy-Cons / handheld, + 0x4b one Joy-Con; kind = [[0x7105338860 + 8 * slot] + 0x10] + 0x9c, if [0x71053388b0] + 8). In a room
 * match each game sends its own player's 0x2b bytes + that byte; both games then hold both players' own controls. */
#define LWR_CTL 0x2b
static uint8_t g_lwr_ctl[2][LWR_CTL + 2];      /* + LWR_CTL: the option byte; + LWR_CTL + 1: 1 = the player's own controller is a GameCube one */ static int g_lwr_ctl_ok, g_lwr_ctl_applied;
static void lwr_own_controls(uint64_t st, uint8_t out[LWR_CTL + 2]) {
    uint32_t prof = rd32(st + 0x214), slot = rd32(st + 0x210);
    uint64_t p = rd8(st + 0x450) && rd32(st + 0x78) == 0 ? prof_ptr(prof) : GA(0x7105313518ULL);
    for (int k = 0; k < LWR_CTL; k++) out[k] = rd8(p + 0x24 + (uint64_t)k);
    uint8_t opt = 0;
    if (rd8(st + 0x450) && rd32(st + 0x78) == 0) {
        uint64_t ctl = GA(0x7105338830ULL), h = sr64(GA(0x71053388b0ULL));
        if (slot < 10 && h && rd8(h + 8)) ctl = sr64(GA(0x7105338860ULL) + 8ull * slot);
        uint64_t sub = ctl ? sr64(ctl + 0x10) : 0; uint32_t kind = sub ? rd32(sub + 0x9c) : 0xffffffffu;
        if (ctl && kind < 7 && ((0x66u >> kind) & 1)) opt = rd8(p + 0x3f); else if (ctl && kind - 3 < 2) opt = rd8(p + 0x4b); else if (ctl && kind == 7) opt = rd8(p + 0x2f);
    }
    out[LWR_CTL] = opt;
    {   int nnhid_native_gc0(void); const char *e = getenv("SSBU_RB_NET_TEST_GC");      /* (TEST ONLY) =1: this game's player's controller taken as a GameCube one */
        out[LWR_CTL + 1] = (uint8_t)(nnhid_native_gc0() || (e && e[0] == '1')); }
    {   const char *e = getenv("SSBU_RB_NET_TEST_CTL");    /* (TEST ONLY) =<0x2b bytes in hex>: this game's own player's controls, as if its profile held them */
        if (e && strlen(e) >= 2 * LWR_CTL) { for (int k = 0; k < LWR_CTL; k++) { unsigned v = 0; sscanf(e + 2 * k, "%2x", &v); out[k] = (uint8_t)v; }
            printf("[rbnet] room match: TEST: this game's player's controls from SSBU_RB_NET_TEST_CTL\n"); fflush(stdout); } }
}
static ImportFn g_orig_controls_fill;
static void controls_fill_hook(Ctx *c, void *user) {
    (void)user;
    g_orig_controls_fill(c);
    if (!g_hs_lwr || !g_lwr_ctl_ok || g_lwr_ctl_applied) return;
    uint64_t o = sr64(GA(0x71052c40f0ULL)); if (!o) return;
    for (int i = 0; i < 2; i++) {
        for (int k = 0; k < LWR_CTL; k++) wr8(o + 0x18 + 0x50ull * (uint64_t)i + (uint64_t)k, g_lwr_ctl[i][k]);
        char t[200]; int n = 0; for (int k = 0; k < LWR_CTL; k++) n += snprintf(t + n, sizeof t - n, "%02x", g_lwr_ctl[i][k]);
        printf("[rbnet] room match: player %d plays with its own name-tag controls (profile + 0x24 .. + 0x4e: %s; record option byte %u)\n", i + 1, t, g_lwr_ctl[i][LWR_CTL]);
    }
    g_lwr_ctl_applied = 1;
    fflush(stdout);
}
static int hs_layout(uint64_t st, uint16_t out[11]) {
    uint32_t type = rd32(st + 0x78);
    if (rd8(st + 0x454) || (rd8(st + 0x455) && type == 0)) { for (int k = 0; k < 11; k++) out[k] = rd16(st + 0x54 + 2ull * k); return 1; }
    uint32_t prof = rd32(st + 0x214);
    if (!rd8(st + 0x455) && prof != 0xffffffffu && type == 0) {
        uint64_t p = prof_ptr(prof);
        for (int k = 0; k < 11; k++) out[k] = rd16(p + 0xc + 2ull * k);
        return 1;
    }
    return 0;
}
static void hs_apply(uint64_t st, int idx, uint32_t type_was) {
    if (idx == 0) {
        RbhMatch me; memset(&me, 0, sizeof me);
        uint32_t cur = rd32(GA(0x7105309968ULL)); uint64_t sel = GA(0x71053097c0ULL) + 0x20ull * (cur < 3 ? cur : 0);
        me.chara[0] = rd64(st + 0x88); me.colour[0] = rd8(st + 0x90); me.has_layout[0] = (uint8_t)hs_layout(st, me.layout[0]);
        safe_read(sel, me.stage, sizeof me.stage);
        me.hazards = rd8(GA(0x7105309985ULL)); me.items = rd32(MELEE_INFO + 0x54); me.rule_kind = rd8(st + 5); me.stock = rd8(st + 0xa);
        me.ctx_seed = rd32(GA(0x710530d380ULL)); safe_read(GA(0x71050c5ad0ULL), me.ctx_hist, sizeof me.ctx_hist); safe_read(MELEE_INFO + 0x420, me.ctx_stage, sizeof me.ctx_stage);   /* (hs12) the stage set-up's inputs (rb_hs.h) */
        if (getenv("SSBU_RB_NET_WATCH")) { uint64_t off = strtoull(getenv("SSBU_RB_NET_WATCH"), NULL, 16); if (off <= 1) off = 0x98;      /* (hs12: =<hex offset in the match block>) */
            uint64_t w[1] = {MELEE_INFO + (off & ~7ull)}; printf("[rbnet] research: match block + 0x%llx at the handshake = %016llx\n", (unsigned long long)(off & ~7ull), (unsigned long long)rd64(w[0])); debug_watch(w, 1); }
        g_hs_ok = rbn_handshake(&me, &g_hs_m); g_hs_local = 0;
        {   int rbn_room_rollback(void); g_hs_lwr = g_hs_ok && rbn_room_rollback(); }
        {   int rbn_match_cancelled(void); if (!g_hs_ok && rbn_match_cancelled() == 2) return; }      /* (R3 pub2) a Local Wireless room fight: nothing changed */
        if (!g_hs_ok) {                                   /* (hs12) not agreed: a local match of this game (player 2 = this player's fighter again), left at once by rb_rollback.c */
            g_hs_m = me; g_hs_m.chara[1] = me.chara[0]; g_hs_m.colour[1] = (uint8_t)((me.colour[0] + 1) & 7); g_hs_m.has_layout[1] = me.has_layout[0];
            memcpy(g_hs_m.layout[1], me.layout[0], sizeof g_hs_m.layout[1]); g_hs_ok = 1; g_hs_local = 1;
            /* a match that ends by itself: Final Destination, one stock; rb_rollback.c walks player 1 off the stage */
            wr64(sel, 0x6900000000000000ull | hash40("ui_stage_end")); wr32(sel + 8, 0);
            if (g_hs_m.rule_kind == 1) g_hs_m.stock = 1;
        }
        if (g_hs_ok && !g_hs_local) {
            { void r1_find_heaps_early(void); r1_find_heaps_early(); }      /* rollback/model/memory_map.c: off the first fight's base save */
            if (!g_hs_lwr) {                              /* (R3 lwr: a room's match: the stage, hazards and items are the room's, the same on both PCs) */
            for (int k = 0; k < 0x20; k++) wr8(sel + (uint64_t)k, g_hs_m.stage[k]);
            wr8(GA(0x7105309985ULL), g_hs_m.hazards); wr32(MELEE_INFO + 0x54, g_hs_m.items);
            }
            /* (hs12) the stage set-up's inputs as the host had them (rb_hs.h: run tw5, a stage variation chosen from each game's own history) */
            {   int d = 0; uint8_t h[0x60], stg[0xb0];
                safe_read(GA(0x71050c5ad0ULL), h, sizeof h); safe_read(MELEE_INFO + 0x420, stg, sizeof stg);
                d = (rd32(GA(0x710530d380ULL)) != g_hs_m.ctx_seed) + (memcmp(h, g_hs_m.ctx_hist, sizeof h) != 0) * 2 + (memcmp(stg, g_hs_m.ctx_stage, sizeof stg) != 0) * 4;
                wr32(GA(0x710530d380ULL), g_hs_m.ctx_seed);
                for (int k = 0; k < (int)sizeof g_hs_m.ctx_hist; k++) wr8(GA(0x71050c5ad0ULL) + (uint64_t)k, g_hs_m.ctx_hist[k]);
                for (int k = 0; k < (int)sizeof g_hs_m.ctx_stage; k++) wr8(MELEE_INFO + 0x420 + (uint64_t)k, g_hs_m.ctx_stage[k]);
                printf("[rbnet] handshake: the stage set-up's inputs taken from the host (here they differed: seed word %s, history %s, stage block %s)\n", d & 1 ? "yes" : "no", d & 2 ? "yes" : "no", d & 4 ? "yes" : "no"); }
            /* the match's random seed: after this hook the game's set-up 0x71014d8620 stores the next value of the global
             * xorshift128 at 0x71052381c0 (4 words; advanced by the menus, so different on the two PCs) in match block
             * + 0x98 (at 0x71014d8f00..2c, when block + 0x9c is set), and the fight's script generators are seeded from it
             * (found with SSBU_RB_NET_WATCH, runs hs4 / hs5: equal + 0x98 at the handshake, different after that store).
             * Both games set that generator from the agreed seed here. */
            {   uint64_t g = GA(0x71052381c0ULL), z = g_hs_m.seed;
                printf("[rbnet] handshake: the global generator %llx was %08x %08x %08x %08x; set from the agreed seed\n", (unsigned long long)g, rd32(g), rd32(g + 4), rd32(g + 8), rd32(g + 12));
                for (int k = 0; k < 4; k++) { z += 0x9E3779B97F4A7C15ull; uint64_t y = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; y = (y ^ (y >> 27)) * 0x94D049BB133111EBull; y ^= y >> 31; wr32(g + 4ull * k, (uint32_t)y | (k == 0)); }
                fflush(stdout); }
            /* (R3 lwr) the room's match made a local versus match: the match block's melee mode (+ 0; FighterManager impl + 0xc0 takes it,
             * app::lua_bind::FighterManager__is_melee_mode_*) is 1 in a Local Wireless room's match and 0 in a local Smash (versus) match,
             * the rest of the rule header the same (runs lwres3 / lwres3v, SSBU_DEBUG_MIDUMP: the room's match and a local versus match
             * at the set-up and at the fight start: rule + 0, + 1 and FighterManager impl + 0xc0 the only differences there). So the game's
             * own online netcode does not play this fight; both games run it as the local match rollback expects. SSBU_RB_NET_ROOM_MODE=<n>:
             * the mode written (test), -1 = not changed. */
            if (g_hs_lwr) {
                const char *e = getenv("SSBU_RB_NET_ROOM_MODE"); int to = e && *e ? atoi(e) : 0; uint8_t was = rd8(MELEE_INFO);
                if (to >= 0) wr8(MELEE_INFO, (uint8_t)to);
                printf("[rbnet] room match: melee mode (match block + 0) %u -> %u: played as a local versus match with rollback (room maker = player 1)\n", was, rd8(MELEE_INFO));
                fflush(stdout);
            }
        }
    }
    if (!g_hs_ok || idx < 0 || idx > 1) return;
    if (g_hs_lwr) {                                     /* (R3 lwr) the players' fighters, colours, rules are the room's own (the same on both PCs) */
        /* Which controller drives a player: state + 0x210 = the controller slot (the record builder reads the controller object
         * 0x7105338860[state + 0x210]). In the room's records this game's own player has slot 0 (its name tag, + 0x450) and the
         * other game's player is a network player (+ 0x451 -> record + 0xa5 = 1, slot -1): made a local match, the network player
         * read no controller and this game's player read port 1 whatever its index (run lwr17, SSBU_RB_NET_DETAIL: the joiner's
         * player 2 moved with port 1's pad; lwr18: with no slot at all neither player moved). So on both PCs player i gets
         * controller slot i (port i, where rb_rollback.c gives player i's pads), no network flag, and no name-tag profile (+ 0x214
         * = -1: the controls are the game's default ones on both PCs; a player's own name-tag controls exist only on his own
         * PC). Run lwr20: both players move, each with its own pads, on both PCs. SSBU_RB_NET_ROOM_NORM=0: the room's states as
         * they are (test). The 11 words at state + 0x54 (record + 0x28, written with + 0x454 = 1) hold the player's name-tag text
         * in these runs ("Anto..."): each player's own, so both records are the same: player 1's from the handshake (the room
         * maker's own), player 2's sent by the joiner at this record (rb_net.c rbn_room_layout). */
        static int norm = -1; if (norm < 0) { const char *e = getenv("SSBU_RB_NET_ROOM_NORM"); norm = e && *e ? atoi(e) : 1; }
        if (norm) {
            int rbn_local_player(void); int rbn_room_layout(const uint16_t mine[11], int has, const uint8_t mine_ctl[LWR_CTL + 2], uint16_t other[11], int *other_has, uint8_t other_ctl[LWR_CTL + 2]);
            int local = rbn_local_player(), has = 0; uint16_t lay[11]; memset(lay, 0, sizeof lay);
            if (idx == 0) { g_lwr_ctl_ok = 0; g_lwr_ctl_applied = 0; }
            if (idx == local) lwr_own_controls(st, g_lwr_ctl[idx]);      /* (before the state is changed below: its own profile and controller) */
            if (idx == 0) { if (local == 0) has = hs_layout(st, lay); else { has = g_hs_m.has_layout[0]; memcpy(lay, g_hs_m.layout[0], sizeof lay); } }
            else {
                uint16_t own[11], other[11]; int own_has, other_has = 0;
                if (local == 1) own_has = hs_layout(st, own); else { own_has = g_hs_m.has_layout[0]; memcpy(own, g_hs_m.layout[0], sizeof own); }
                int got = rbn_room_layout(own, own_has, g_lwr_ctl[local], other, &other_has, g_lwr_ctl[1 - local]);
                g_lwr_ctl_ok = got;
                {   void nnhid_lwr_styles(int on, int gc0, int gc1);       /* (R3 lwr) port i in player i's own controller kind on both PCs */
                    if (got) { nnhid_lwr_styles(1, g_lwr_ctl[0][LWR_CTL + 1], g_lwr_ctl[1][LWR_CTL + 1]);
                        printf("[rbnet] room match: controller kinds: player 1 %s, player 2 %s (ports 1 / 2 shown so on this PC)\n", g_lwr_ctl[0][LWR_CTL + 1] ? "GameCube" : "Pro", g_lwr_ctl[1][LWR_CTL + 1] ? "GameCube" : "Pro"); } }
                if (local == 1) { has = own_has; memcpy(lay, own, sizeof lay); } else if (got) { has = other_has; memcpy(lay, other, sizeof lay); }
            }
            if (!has) { uint64_t p = GA(0x7105313518ULL); for (int k = 0; k < 11; k++) lay[k] = rd16(p + 0xc + 2ull * k); }      /* (the game's default profile) */
            printf("[rbnet] room match: player %d (%s): ui_chara %016llx colour %u; panel state: name tag %d, network player %d, controller slot %d, profile %d -> controller slot %d, a local human\n", idx + 1,
                   idx == local ? "this game's player" : "the other game's player", (unsigned long long)rd64(st + 0x88), rd8(st + 0x90), rd8(st + 0x450), rd8(st + 0x451), (int)rd32(st + 0x210), (int)rd32(st + 0x214), idx);
            wr8(st + 0x450, 1); wr8(st + 0x451, 0); wr32(st + 0x210, (uint32_t)idx); wr32(st + 0x214, 0xffffffffu);
            wr8(st + 0x454, 1); for (int k = 0; k < 11; k++) wr16(st + 0x54 + 2ull * k, lay[k]);
            fflush(stdout);
        }
        return;
    }
    if (type_was == 3) {                                /* (hs12) no panel for this player on this game's select screen: player 1's state is the template */
        if (!g_hs_p1_ok) { printf("[rbnet] handshake: player %d has NO panel on this game's select screen and player 1's state was not read: this match will not be the agreed one\n", idx + 1); fflush(stdout); return; }
        hs_fill_empty(st, idx); wr64(st + 0x80, g_hs_m.chara[idx]);
        printf("[rbnet] handshake: player %d had no panel on this game's select screen: made from player 1's panel state with the %s fighter\n", idx + 1, g_hs_local ? "local" : "agreed"); fflush(stdout);
    }
    wr64(st + 0x88, g_hs_m.chara[idx]); wr8(st + 0x90, g_hs_m.colour[idx]); wr32(st + 0x78, 0);
    if (g_hs_m.has_layout[idx]) { wr8(st + 0x454, 1); for (int k = 0; k < 11; k++) wr16(st + 0x54 + 2ull * k, g_hs_m.layout[idx][k]); }
    else { wr8(st + 0x454, 0); wr8(st + 0x455, 0); wr32(st + 0x214, 0xffffffffu); }
    if (rd8(st + 5) == g_hs_m.rule_kind) { if (g_hs_m.rule_kind == 1) wr8(st + 0xa, g_hs_m.stock); }
    else { printf("[rbnet] handshake: player %d: rule kind %d here, %d agreed (not changed: set the same rules on both)\n", idx + 1, rd8(st + 5), g_hs_m.rule_kind); fflush(stdout); }
}
static void match_player_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t st = c->x[0], rec = c->x[3]; int idx = (int)c->x[1];
    uint64_t was = rd64(st + 0x88); uint32_t type_was = rd32(st + 0x78);
    if (idx == 0) { g_hs_p1_ok = safe_read(st, g_hs_p1, sizeof g_hs_p1); InterlockedIncrement(&g_dm_setups); }
    if (idx >= 0 && idx < 4)                              /* (RB4) every set-up's panel states as the screens left them: "player 2 not picked" after a couple of matches */
        printf("[rbnet] set-up %ld: player %d as the screens left it: type %u, ui_chara %016llx colour %u, name tag %u, network player %u, controller slot %d, profile %d (caller %llx)\n",
               (long)g_dm_setups, idx + 1, type_was, (unsigned long long)was, rd8(st + 0x90), rd8(st + 0x450), rd8(st + 0x451), (int)rd32(st + 0x210), (int)rd32(st + 0x214),
               (unsigned long long)c->x[30]);
    if (idx == 0 && g_dm_list) {                          /* SSBU_DEBUG_MATCH_LIST: every new match set-up takes the next line (the last one stays) */
        char line[256];
        if (fgets(line, sizeof line, g_dm_list)) { line[strcspn(line, "\r\n")] = 0; if (line[0]) { g_dm_fight++; parse_match(line); } }
    }
    if (g_dm.stock > 0 && type_was != 3 && rd8(st + 5) == 1) wr8(st + 0xa, (uint8_t)g_dm.stock);   /* state + 5: rule kind (1 = stock), + 0xa: stocks -> record + 0x18 */
    if ((idx == 2 || idx == 3) && g_dm_name[idx][0] && g_dm_state1_ok) {      /* session 14: p3 / p4 = CPU players. An empty panel (type 3) takes player 2's state as the template */
        static int mode = -1;
        if (mode < 0) mode = getenv("SSBU_DEBUG_P34") ? atoi(getenv("SSBU_DEBUG_P34")) : 1;      /* 1 copy the whole state of player 2 (default), 2 only the fields known */
        if (type_was == 3 || rd32(st + 0x78) != 1) {
            if (mode == 1) for (int k = 0; k < (int)sizeof g_dm_state1; k++) wr8(st + (uint64_t)k, g_dm_state1[k]);
            else { wr8(st + 5, g_dm_state1[5]); wr8(st + 0xa, g_dm_state1[0xa]); }
            wr32(st + 0x78, 1);
        }
        char nm[64]; snprintf(nm, sizeof nm, "ui_chara_%s", g_dm_name[idx]);
        wr64(st + 0x88, 0xc100000000000000ull | hash40(nm));
        wr8(st + 0x90, (uint8_t)idx);                      /* colour: one per player */
        if (g_dm.stock > 0 && rd8(st + 5) == 1) wr8(st + 0xa, (uint8_t)g_dm.stock);
        if (g_dm.cpu > 0) { wr8(st + 0x91, (uint8_t)g_dm.cpu); wr32(st + 0x224, 0xffffffffu); }
        g_dm_applied |= 1 << idx;
        type_was = 1;
    }
    if (getenv("SSBU_DEBUG_STATEDUMP") && idx >= 0 && idx < 2) {       /* S20t: the panel state as the screens left it (find the Mii fields) */
        char tag[48]; snprintf(tag, sizeof tag, "debug state: p%d ", idx); hex_lines(tag, st, 0x480); fflush(stdout);
    }
    if (idx >= 0 && idx < 2 && g_dm_name[idx][0] && type_was != 3 && !(getenv("SSBU_DEBUG_KEEP") && (atoi(getenv("SSBU_DEBUG_KEEP")) >> idx & 1))) {   /* SSBU_DEBUG_KEEP=<mask>: keep the screens' own choice for these players (S20t) */
        char nm[64]; snprintf(nm, sizeof nm, "ui_chara_%s", g_dm_name[idx]);
        wr64(st + 0x88, 0xc100000000000000ull | hash40(nm));
        if (idx == 1 && g_dm_p2type >= 0) wr32(st + 0x78, (uint32_t)g_dm_p2type);
        if (g_dm.cpu > 0 && rd32(st + 0x78) == 1) { wr8(st + 0x91, (uint8_t)g_dm.cpu); wr32(st + 0x224, 0xffffffffu); }   /* level 1..9; the game maps it to the power in record + 0x79 (0, 15, 21, 31, 42, 48, 60, 75, 100) */
        g_dm_applied |= 1 << idx;
    }
    if (idx == 0 && g_dm.text[0]) {
        /* the stage choice: the game reads it right after the player records, in the same function (0x7102311a24..): three
         * entries of 0x20 bytes at 0x71053097c0, the current one's index at 0x7105309968: +0 ui_stage id (0x69 << 56 | hash40),
         * +8 form (0 normal, 1 battlefield, 2 omega), +0xc a flag word, +0x10 a second id. The rule header (match block + 0..) is filled already. */
        uint32_t cur = rd32(GA(0x7105309968ULL)); uint64_t sel = GA(0x71053097c0ULL) + 0x20ull * (cur < 3 ? cur : 0);
        printf("debug match: stage choice of the screens: entry %u: ui_stage %llx form %u flag %x second id %llx; byte 7105309985 = %u; rule: stock %u item appear %u\n", cur,
               (unsigned long long)rd64(sel), rd32(sel + 8), rd32(sel + 0xc), (unsigned long long)rd64(sel + 0x10), rd8(GA(0x7105309985ULL)), rd8(MELEE_INFO + 0x4c), rd32(MELEE_INFO + 0x54));
        if (g_dm.stage[0]) {
            const char *sn = g_dm.stage; char nm[80];
            if (!strcmp(sn, "fd") || !strcmp(sn, "final_destination")) sn = "end"; else if (!strcmp(sn, "battlefield")) sn = "battle_field";
            snprintf(nm, sizeof nm, "ui_stage_%s", sn);
            wr64(sel, 0x6900000000000000ull | hash40(nm));
        }
        if (g_dm_form >= 0) wr32(sel + 8, (uint32_t)g_dm_form);
        if (g_dm_hazards >= 0) wr8(GA(0x7105309985ULL), (uint8_t)g_dm_hazards);
        if (g_dm.items >= 0) wr32(MELEE_INFO + 0x54, g_dm.items ? 2u : 0u);
    }
    if (idx == 0) { void nnhid_lwr_styles(int on, int gc0, int gc1); nnhid_lwr_styles(0, 0, 0); }      /* (R3 lwr) a new set-up: the ports in their own kinds again */
    if (idx == 0) { void rbn_set_setup_waiting(int waiting); rbn_set_setup_waiting(c->x[30] == GA(0x71017911dcULL)); }      /* (R3 lwr) a Local Wireless room's waiting-stage fight: built from its own set-up (runs lwres2 / lwres3: return address 0x71017911dc; the room's match and the select screens: 0x710178f498) */
    if (g_hs_wanted) hs_apply(st, idx, type_was);       /* R3: the match both online games agreed (rb_net.c handshake) */
    if (idx == 1) g_dm_state1_ok = safe_read(st, g_dm_state1, sizeof g_dm_state1);
    g_orig_player_record(c);
    if (idx == 1 && g_hs_lwr && g_lwr_ctl_ok) for (int i = 0; i < 2; i++) wr8(MI_PLAYER(i) - 8 + 0x48, g_lwr_ctl[i][LWR_CTL]);      /* (R3 lwr) each player's own option byte (record + 0x48, see lwr_own_controls) */
    if (idx == 1 && rd32(st + 0x78) == 0) { void input_mark(const char *what); input_mark("setup-2p"); }      /* (R3 pub2, test aid) a match set up with a human player 2 (SSBU_SEQ @untilmark:setup-2p) */
    if (getenv("SSBU_DEBUG_P1PROFILE") && idx == 0) {     /* (R3 lwr, research) player 1's name-tag profile index forced (=<n>), or (=c<n>) profile n's
                                                           * control bytes + 0x24 .. + 0x50 copied over player 1's own profile's (in memory only, this run) */
        const char *e = getenv("SSBU_DEBUG_P1PROFILE");
        uint64_t x0 = sr64(GA(0x7105313510ULL)), x = x0 ? sr64(x0) : 0, a = x ? sr64(x + 0x58) : 0, b = a ? sr64(a) : 0, q = b ? sr64(b) : 0;
        if (e[0] == 'c' && q) { uint32_t from = (uint32_t)atoi(e + 1), own = rd32(st + 0x214);
            if (own < 0x3d) for (int k = 0x24; k < 0x50; k++) wr8(q + 0xf7d8ull * own + k, rd8(q + 0xf7d8ull * from + k));
            printf("debug ctrl: research: profile %u's control bytes copied over player 1's profile %u\n", from, own); }
        else { printf("debug ctrl: research: player 1's profile %d -> %d\n", (int)rd32(st + 0x214), atoi(e)); wr32(st + 0x214, (uint32_t)atoi(e)); }
        fflush(stdout);
    }
    if (getenv("SSBU_DEBUG_W500") && idx == 0) {          /* (R3 lwr, research) who writes match block + 0x500 (the block check's last differing bytes) */
        hex_lines("debug w500: block + 0x4d0 at player 1's record", MELEE_INFO + 0x4d0, 0xb0);
        uint64_t w[1] = {MELEE_INFO + 0x500}; debug_watch(w, 1); fflush(stdout);
    }
    if (getenv("SSBU_DEBUG_CTRLWATCH") && idx == 0) {     /* (R3 lwr, research) who writes the controls singleton's (0x71052c40f0) players 1 / 2 blocks */
        uint64_t o = sr64(GA(0x71052c40f0ULL));
        hex_lines("debug ctrl: singleton at player 1's record", o, 0x120);
        if (o) { uint64_t w[2] = {o + 0x18, o + 0x68}; debug_watch(w, 2); }
        fflush(stdout);
    }
    if (getenv("SSBU_DEBUG_CTRLDUMP") && idx <= 1) {      /* (R3 lwr, research) the player's name-tag profile and its controller slot's objects */
        uint32_t prof = rd32(st + 0x214), slot = rd32(st + 0x210); char t[80];
        uint64_t x0 = sr64(GA(0x7105313510ULL)), x = x0 ? sr64(x0) : 0, a = x ? sr64(x + 0x58) : 0, b = a ? sr64(a) : 0, q = b ? sr64(b) : 0;
        if (idx == 0) hex_lines("debug ctrl:   default profile", GA(0x7105313518ULL), 0x100);
        if (idx == 0) for (uint32_t k = 0; k < 0x3d; k++) { uint64_t pp = prof_ptr(k); if (pp == GA(0x7105313518ULL)) continue; char nm[24]; int j; for (j = 0; j < 10 && rd16(pp + 0xc + 2ull * j); j++) nm[j] = (char)rd16(pp + 0xc + 2ull * j); nm[j] = 0;
            char t[100]; int n = 0; for (int b2 = 0; b2 < 0x2b; b2++) n += snprintf(t + n, sizeof t - n, "%02x", rd8(pp + 0x24 + (uint64_t)b2)); printf("debug ctrl: profile %u \"%s\": %s\n", k, nm, t); }
        printf("debug ctrl: player %d: profile %d (table %llx) slot %d 0x450 %d 0x451 %d\n", idx, (int)prof, (unsigned long long)q, (int)slot, rd8(st + 0x450), rd8(st + 0x451));
        if (q && prof < 0x3d) { snprintf(t, sizeof t, "debug ctrl:   p%d profile", idx); hex_lines(t, q + 0xf7d8ull * prof, 0x100); }
        if (slot < 10) { uint64_t o = sr64(GA(0x7105338860ULL) + 8ull * slot); snprintf(t, sizeof t, "debug ctrl:   p%d slot obj", idx); hex_lines(t, o, 0x40);
            for (int k = 0; k < 4; k++) { snprintf(t, sizeof t, "debug ctrl:   p%d slot sub%d", idx, k); hex_lines(t, sr64(o + 0x10 + 8ull * k), 0x100); } }
        fflush(stdout);
    }
    if (getenv("SSBU_DEBUG_MIDUMP") && idx <= 1) {        /* (R3 lwr, research) the match block after this record: rule header and the two records */
        char tag[64]; snprintf(tag, sizeof tag, "debug midump: after p%d (from %llx) rule", idx, (unsigned long long)c->x[30]);
        if (idx == 1 || atoi(getenv("SSBU_DEBUG_MIDUMP")) > 1) { hex_lines(tag, MELEE_INFO, 0x580); hex_lines("debug midump:   rec0", MI_PLAYER(0) - 8, 0x260); hex_lines("debug midump:   rec1", MI_PLAYER(1) - 8, 0x260); }
        fflush(stdout);
    }
    printf("debug match: player %d: screen choice %llx type %u level %u -> record %llx: ui_chara %llx, fighter kind %d, type %d, colour %d, cpu level %d\n", idx,
           (unsigned long long)was, type_was, rd32(st + 0x224), (unsigned long long)rec, (unsigned long long)rd64(rec), (int)(int8_t)rd8(rec + 8), rd8(rec + 0xa), rd8(rec + 0x19), rd8(rec + 0x79));
    if (getenv("SSBU_DEBUG_RESEARCH") && idx == 0) {
        uint64_t fp = c->x[29], pair[2];
        printf("debug research: player record builder called from %llx, frames:", (unsigned long long)c->x[30]);
        for (int d = 0; d < 12 && safe_read(fp, pair, 16); d++) { printf(" %llx", (unsigned long long)pair[1]); if (pair[0] <= fp) break; fp = pair[0]; }
        printf("\n");
    }
    fflush(stdout);
}

static DWORD WINAPI dump_thread(LPVOID arg) {
    (void)arg;
    uint64_t last[8] = {0};
    for (;;) {
        Sleep(g_dm.text[0] || g_end_limit ? 2 : 250);
        if (g_end_fired) {
            printf("debug end: %lld fight frames reached at presented frame %lld (SSBU_DEBUG_END): exit request, normal shutdown\n", (long long)g_end_fight_frames, (long long)g_end_fired);
            fflush(stdout);
            if (!oe_exit_request()) report_and_exit(0);
            return 0;
        }
        if (getenv("SSBU_DEBUG_WATCHENTRY")) {            /* research: who writes the entry pointers of the FighterManager (impl + 0x20, + 0x28) */
            static int tick;
            uint64_t inst = hook_address("_ZN3lib9SingletonIN3app14FighterManagerEE9instance_E");
            uint64_t mgr = inst ? sr64(inst) : 0, impl = mgr ? sr64(mgr) : 0;
            if (impl && (tick++ % 40) == 0) { uint64_t w[2] = {impl + 0x20, impl + 0x28}; watch_set(w, 2); }
            continue;
        }
        if (getenv("SSBU_DEBUG_RESEARCH")) {              /* research: who fills the global match block (kind of player 0 / 1, first rule bytes) */
            static int tick;
            if (atoi(getenv("SSBU_DEBUG_RESEARCH")) == 3 && (tick++ % 40) == 0) { uint64_t w[4] = {MELEE_INFO + 0x420, MELEE_INFO + 0x428, MELEE_INFO + 0x430, MELEE_INFO + 0x4d0}; watch_set(w, 4); }
            if (atoi(getenv("SSBU_DEBUG_RESEARCH")) == 2 && (tick++ % 40) == 0) { uint64_t w[4] = {MI_PLAYER(0), MI_PLAYER(1), MELEE_INFO, MELEE_INFO + 0x10}; watch_set(w, 4); }
        }
        for (int i = 0; i < 8; i++) {
            uint64_t rec = fighter_info_record(i);
            uint8_t b[0x400];
            if (!rec || !safe_read(rec, b, sizeof b)) continue;
            uint64_t h = 1469598103934665603ull;
            for (int k = 0; k < (int)sizeof b; k++) h = (h ^ b[k]) * 1099511628211ull;
            if (h == last[i]) continue;
            last[i] = h;
            if (getenv("SSBU_DEBUG_DUMP")) {
            printf("debug dump: entry %d record %llx\n", i, (unsigned long long)rec);
            for (int k = 0; k < 0x400; k += 16) {
                int any = 0;
                for (int j = 0; j < 16; j++) any |= b[k + j];
                if (any) printf("debug dump:   +%03x %02x %02x %02x %02x %02x %02x %02x %02x  %02x %02x %02x %02x %02x %02x %02x %02x\n", k, b[k], b[k+1], b[k+2], b[k+3], b[k+4], b[k+5], b[k+6], b[k+7], b[k+8], b[k+9], b[k+10], b[k+11], b[k+12], b[k+13], b[k+14], b[k+15]);
            }
            fflush(stdout);
            }
            if (i == 1 && getenv("SSBU_DEBUG_WATCH")) { static int done; uint64_t r0 = fighter_info_record(0); if (!done && r0) { done = 1; uint64_t w[4] = {r0 + 0x30, rec + 0x30, rec + 0x20, r0 + 0x08}; watch_set(w, 4); } }
            if (i == 1 && getenv("SSBU_DEBUG_SCAN")) { static int done; if (!done) { done = 1; scan_copies(rec); } }
        }
        if (g_dm.text[0]) {                               /* read back from the game's own entries once they have been stable for ~1 s */
            static int printed, stable, prev0 = -1, prev1 = -1;
            uint64_t r0 = fighter_info_record(0), r1 = fighter_info_record(1);
            int k0 = r0 ? (int)sr64(r0 + 0x30 - 0) & 0xffffffff : 0, k1 = r1 ? (int)sr64(r1 + 0x30) & 0xffffffff : 0;
            if (k0 && k1 && k0 == prev0 && k1 == prev1) stable++; else stable = 0;
            prev0 = k0; prev1 = k1;
            if (!printed && stable > 400 && getenv("SSBU_DEBUG_DUMP")) {   /* record + 0x30 is NOT the fighter kind (session 2: the health lines and the match block are) */ printed = 1; printf("debug match: in the fight: entry 0 kind %d, entry 1 kind %d (records at %llx, %llx; kind + 1 at record + 0x30)\n", k0 - 1, k1 - 1, (unsigned long long)r0, (unsigned long long)r1); if (getenv("SSBU_DEBUG_RESEARCH")) hex_lines("debug research:   fight rule ", MELEE_INFO, 0x580); fflush(stdout); }
        }
    }
}

/* (hs12) the select screens' per-player table (0x7105307770 + 0x1c8 * i, 8 entries; read in Ghidra: 0x7101787830 builds a
 * panel state from it, 0x710179dd50 resets it, 0x71017aca10 counts the teams of the entries with type < 3): + 0 controller
 * byte, + 0x28 player number, + 0x30 type (0 human, 1 CPU, 3 none), + 0x34 team, + 0x38 / + 0x40 ui_chara, + 0x48 colour,
 * + 0x49 CPU level. Who writes it (hardware write watch, run pnW): when the character select opens, 0x710179e440(table - 0x10,
 * player, source) copies each entry from the saved choices (a save with a CPU panel brings player 2 = CPU, ui_chara_random,
 * level 3); while the screen runs, the screen writes its own panels back (0x7101831c70 / 0x7101833020, from 0x7101847460).
 * The screen keeps its own panels: an entry changed while it runs is NOT taken (run pnE), and with player 1 alone the match
 * cannot be started (run pnA: "Ready to fight" never came). So the online session (AUTO P2, on with the handshake, off with
 * SSBU_RB_NET_AUTOP2=0) puts a CPU panel in entry 1 right after 0x710179e440 copies a "none" there: the screen opens with
 * player 2 = CPU (random), the player picks only his own fighter, and the handshake replaces player 2 by the other player.
 * Research / test switches (off by default):
 *   SSBU_DEBUG_PANELS=1      a log line whenever entry 0 or 1 changes (=2: also a write watch on their types, guest frames)
 *   SSBU_RB_NET_P2NONE=1     (test) entry 1 copied as "none" whatever the save says (a save without a CPU panel)
 *   SSBU_RB_NET_AUTOP2=0|1   off / on (default: on when the handshake is wanted) */
#define PANEL(i) (GA(0x7105307770ULL) + 0x1c8ull * (uint64_t)(i))
static ImportFn g_orig_panel_copy;
static int g_p2none = -1, g_autop2 = -1; static volatile LONG g_autop2_made;
static void panel_copy_hook(Ctx *c, void *user) {
    (void)user;
    uint64_t base = c->x[0]; uint32_t idx = (uint32_t)c->x[1];
    g_orig_panel_copy(c);
    if (idx != 1) return;
    uint64_t e = base + 0x10 + 0x1c8ull;
    if (g_p2none > 0 && rd32(e + 0x30) != 3) {
        printf("debug panels: entry 1 copied as type %u: made none (SSBU_RB_NET_P2NONE)\n", rd32(e + 0x30));
        wr32(e + 0x30, 3); wr32(e + 0x34, 0xffffffffu); wr64(e + 0x38, 0xc1ffff0000000000ull); wr64(e + 0x40, 0xc1ffff0000000000ull);
    }
    int nnldn_role(void);
    if (g_autop2 > 0 && rd32(e + 0x30) == 3 && !nnldn_role()) {        /* the CPU panel a save brings (run pnB); (R3 pub2) never in a Local Wireless room: its fights are the game's own */
        wr8(e, 0xff); wr32(e + 0x28, 2); wr32(e + 0x34, 0xffffffffu);
        wr64(e + 0x38, 0xc100000fd5f7fa78ull); wr64(e + 0x40, 0xc100000fd5f7fa78ull); wr8(e + 0x48, 0); wr8(e + 0x49, 3);
        wr32(e + 0x30, 1);
        InterlockedIncrement(&g_autop2_made);
        printf("[rbnet] select screen: player 2 had no panel: a CPU panel put there for the online match (the other player takes it at the handshake; %ld so far)\n", (long)g_autop2_made);
    }
    fflush(stdout);
}
static DWORD WINAPI panel_thread(LPVOID arg) {
    (void)arg;
    const char *e1 = getenv("SSBU_DEBUG_PANELS");
    int show = e1 ? atoi(e1) : 0;
    uint8_t last[2][0x50]; memset(last, 0xee, sizeof last);
    { extern int g_game_build; for (int w = 0; g_game_build < 0 && w < 2400; w++) Sleep(50); }      /* (GA needs main.elf read: loose_host.c) */
    ULONGLONG t0 = GetTickCount64();
    if (show == 2) { uint64_t w[2] = {PANEL(1) + 0x30, PANEL(0) + 0x30}; watch_set(w, 2); }      /* research: who writes the types (guest frames printed) */
    for (;;) {
        Sleep(20);
        double t = (GetTickCount64() - t0) / 1000.0;
        uint8_t cur[2][0x50];
        if (!safe_read(PANEL(0), cur[0], 0x50) || !safe_read(PANEL(1), cur[1], 0x50)) continue;
        for (int i = 0; i < 2; i++) if (memcmp(cur[i], last[i], 0x50)) {
            memcpy(last[i], cur[i], 0x50);
            printf("debug panels: at %.2f s (set-ups %ld) entry %d: controller %02x number %d type %u team %d ui_chara %016llx / %016llx colour %u level %u\n", t, (long)g_dm_setups, i, cur[i][0],
                   *(int32_t *)(cur[i] + 0x28), *(uint32_t *)(cur[i] + 0x30), *(int32_t *)(cur[i] + 0x34), (unsigned long long)*(uint64_t *)(cur[i] + 0x38), (unsigned long long)*(uint64_t *)(cur[i] + 0x40), cur[i][0x48], cur[i][0x49]);
            fflush(stdout);
        }
    }
    return 0;
}
void debug_mode_init(void) {
    if (getenv("SSBU_DEBUG_PANELS")) CreateThread(NULL, 0, panel_thread, NULL, 0, NULL);
    const char *end = getenv("SSBU_DEBUG_END"), *list = getenv("SSBU_DEBUG_MATCH_LIST");
    if (end && atof(end) > 0) { g_end_limit = (LONG64)(strchr(end, 's') ? atof(end) * 60.0 : atof(end)); printf("debug end: the run ends after %lld fight frames\n", (long long)g_end_limit); }
    if (list && !(g_dm_list = fopen(list, "r"))) printf("debug match: SSBU_DEBUG_MATCH_LIST: cannot open %s\n", list);
    if (g_dm_list && !getenv("SSBU_DEBUG_MATCH")) snprintf(g_dm.text, sizeof g_dm.text, "(list)");
    g_hs_wanted = rbn_handshake_wanted();
    {   const char *e = getenv("SSBU_RB_NET_P2NONE"), *a = getenv("SSBU_RB_NET_AUTOP2");      /* (hs12) the select screen's player 2 for an online match */
        g_p2none = e && atoi(e) > 0; g_autop2 = a && a[0] ? atoi(a) > 0 : g_hs_wanted;
        if (g_p2none || g_autop2) printf("debug match: hook on the select screen's table copy %llx: id %d (auto player 2 %s%s)\n", GA(0x710179e440ULL), hook_install(GA(0x710179e440ULL), panel_copy_hook, NULL, &g_orig_panel_copy),
                                         g_autop2 ? "on" : "off", g_p2none ? "; test: entry 1 copied as none" : ""); }
    if (getenv("SSBU_DEBUG_MATCH") || g_dm_list || g_hs_wanted) {
        if (getenv("SSBU_DEBUG_MATCH")) parse_match(getenv("SSBU_DEBUG_MATCH"));
        printf("debug match: hook on the player record builder %llx: id %d\n", FN_PLAYER_RECORD, hook_install(FN_PLAYER_RECORD, match_player_hook, NULL, &g_orig_player_record));
        if (g_hs_wanted) printf("debug match: hook on the match controls set-up %llx: id %d (R3 lwr: Local Wireless room matches, each player's own controls)\n", (unsigned long long)GA(0x7101780440ULL), hook_install(GA(0x7101780440ULL), controls_fill_hook, NULL, &g_orig_controls_fill));
    }
    if (getenv("SSBU_DEBUG_RESEARCH")) printf("debug research: hook on the entry set-up: id %d\n", hook_install(FN_ENTRY_SETUP, research_entry_setup, NULL, &g_orig_entry_setup));
    if (getenv("SSBU_DEBUG_DUMP") || g_dm.text[0] || g_end_limit ||getenv("SSBU_DEBUG_RESEARCH") || getenv("SSBU_DEBUG_WATCHENTRY")) CreateThread(NULL, 0, dump_thread, NULL, 0, NULL);
}
