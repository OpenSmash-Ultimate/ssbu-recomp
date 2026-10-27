/* Host side of nn::hid devices: the one interface nnhid.c uses to read pads (nnpad_sdl.c implements it with SDL3).
 * Button numbers are the Pro Controller bits of nnhid.c (0 A, 1 B, 2 X, 3 Y, 4 StickL, 5 StickR, 6 L, 7 R, 8 ZL, 9 ZR,
 * 10 Plus, 11 Minus, 12 Left, 13 Up, 14 Right, 15 Down). Players are 0..7. */
#ifndef NNPAD_H
#define NNPAD_H
#include <stdint.h>

typedef struct {
    uint64_t buttons;           /* bits 0..15 only (the stick-as-button bits are derived by nnhid.c) */
    int lx, ly, rx, ry;         /* -32767..32767, up/right positive, dead zone and circular normalisation applied */
    int wired;                  /* 0 = bluetooth / wireless */
    int has_motion;             /* the device has a gyro and accelerometer */
    float accel[3];             /* in G, Nintendo axes (see nnpad_sdl.c; the frame is NOT VERIFIED against a console) */
    float gyro[3];              /* in revolutions per second, same axes */
    int gc;                     /* a GameCube controller (nnpad_gca.c): nnhid.c reports it as a GameCube controller when the game supports that style */
    int trig_l, trig_r;         /* analog triggers 0..32767 (gc only) */
} NnPadState;

int  nnpad_init(void);                           /* SDL up (no video, no window); 0 = failed. Also loads controllers.ini. */
int  nnpad_is_gc(int player);                    /* the device of this player slot is a GameCube controller (from the adapter) */
int  nnpad_gc_native(void);                      /* controllers.ini gamecube_style: 1 = native GameCube style (default), 0 = as a Pro Controller */
void nnpad_poll(int force);                      /* hot-plug, config reload; cheap when called often (at most once per ms) */
int  nnpad_present(int player);                  /* a device is assigned to this player slot */
int  nnpad_get(int player, NnPadState *out);     /* 0 = none */
void nnpad_rumble(int player, float low, float high);   /* amplitudes 0..1 of the low and high band */
void nnpad_disconnect(int player);               /* DisconnectNpad: hidden until one of its buttons is pressed */
void nnpad_swap(int a, int b);                   /* SwapNpadAssignment */
int  nnpad_keyboard_player(void);                /* 0..7, -1 = nobody; fixed once resolved (see controllers.ini) */
int  nnpad_kb_vk(int idx);                       /* configured virtual key: 0..15 = Pro buttons, 16..19 = left stick left/up/right/down, 20..23 = right stick; 0 = unbound */
int  nnpad_kb_movement(void);                   /* [keyboard] movement: 1 = ssf2 (nnhid.c kb_ssf2), 0 = full (a stick key pushes the stick fully) */
void nnpad_kb_movement_set(int on);             /* the settings menu: [keyboard] movement written (1 = ssf2), used at the next poll */
int  nnpad_tool(int argc, char **argv);          /* --pad-test / --pad-map <n> / SSBU_PAD_TEST: returns the exit code, or -1 if this is no tool request */

/* U1, the Controls page of the settings menu (menu.cpp). Targets 0..15 = the Pro buttons above, 16..23 = left stick
 * left/up/right/down, right stick left/up/right/down. A device is named by its index (stable while it stays connected);
 * scope 0 = this controller only ([device <GUID>]), 1 = every controller of its kind ([type <kind>]). */
typedef struct { int index, player, type, gamepad; char name[80], guid[40]; } NnPadDev;
int  nnpad_list(NnPadDev *out, int max);
const char *nnpad_type_label(int type);          /* "Xbox", "PlayStation", "Switch", "GameCube", "other pad", "joystick (no SDL mapping)" */
const char *nnpad_type_key(int type);            /* the [type ...] name: xbox playstation switch gamecube generic joystick */
const char *nnpad_target_name(int target);       /* A ... DOWN, LSTICK_LEFT ... RSTICK_DOWN */
int  nnpad_target_text(int index, int target, char *out, int n);   /* the sources now: 2 = this controller, 1 = its type profile, 0 = built-in, -1 = no device */
int  nnpad_scope_text(int index, int scope, int target, char *out, int n);
void nnpad_target_set(int index, int scope, int target, const char *srcs);   /* NULL = default again, "none" = unbound */
void nnpad_profile_reset(int index, int scope);
void nnpad_tune_get(int index, float v[3]);      /* dead zone, outer zone, trigger threshold in use */
void nnpad_tune_set(int index, int scope, int which, float value);
void nnpad_capture_begin(int index);
int  nnpad_capture_poll(char *out, int n);       /* 0 waiting, 1 got a source, 2 cancelled (Home / Capture), -1 device gone */
void nnpad_capture_end(void);
int  nnpad_player_cfg(int index);                /* the player kept for this device in [players], -1 = auto */
void nnpad_player_assign(int index, int player); /* -1 auto, 0..7; used at once */
void nnpad_kb_bind(int idx, int vk);
void nnpad_kb_player_set(int p);                 /* -2 auto, -1 nobody, 0..7; used at once */

/* self-test hooks implemented by nnhid.c (they call the handlers the game calls) */
typedef struct {
    uint32_t (*style)(uint32_t id);
    void (*state)(uint32_t id, uint64_t *buttons, int32_t stick[4], uint32_t *attr);
    void (*vibrate)(uint32_t id, float low, float high);
    int (*six)(uint32_t id, float accel[3], float gyro[3], int *at_rest);
    void (*test_mode)(int on);
    uint32_t (*led)(uint32_t id);
    void (*gc_state)(uint32_t id, uint64_t *buttons, uint32_t trig[2], uint32_t *attr);   /* NpadGcState as the game reads it */
    void (*gc_erm)(uint32_t id, uint64_t command);                                        /* SendVibrationGcErmCommand with a GameCube handle */
} NnHidHooks;
int  nnpad_selftest(const NnHidHooks *h);         /* number of failed checks; only virtual devices count while it runs */

/* nnpad_gca.c: the GameCube controller adapter over WinUSB, published to SDL as virtual gamepads (vendor 057e, product 0337) */
int  gca_start(void);                            /* after SDL_Init: 1 = looking for adapters; 0 = off (SSBU_GCA=0) or no WinUSB */
int  gca_port_of(unsigned joy_id);               /* adapter * 4 + port of a virtual gamepad made here, else -1 */
void gca_sample(unsigned joy_id);                /* the game reads this device now (SSBU_GCA_STATS: age of the state) */
unsigned gca_seq(void);                          /* reports applied so far: a change means SDL's state is stale (nnpad_poll) */
#endif
