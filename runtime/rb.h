/* Rollback netcode, the parts that do not touch the game (plan\phases\N1_rollback_netcode.md, N1b_slippi_rollback_study.md):
 * pad records, the input queues (local delay, remote prediction, mismatch = where a rollback starts), the wire format of
 * the input messages and the time sync decision. Written for this project after the design of Slippi's netplay
 * (project-slippi/Ishiiruka, GPL-2.0-or-later) and of melee-unlocked's mu_net (GPL-2.0-or-later); no code copied.
 * Plain C, no Windows, no game: tests\rb_test.c checks it alone.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RB_H
#define RB_H
#include <stdint.h>
#include <stddef.h>

#define RB_MAX_PLAYERS 8
#define RB_RING 256                                /* frames kept per player (must be a power of two) */
#define RB_MAX_ROLLBACK 7                          /* frames a rollback can reach back (Slippi: 7) */
#define RB_STICK_REST 1600                         /* |stick| at or below this is exactly 0 (noise must not cause rollbacks) */

typedef struct { uint64_t buttons; int16_t lx, ly, rx, ry; } RbPad;      /* as nnhid.c fill_pad builds it */

/* ---------------------------------------------------------------- input queues (rb_input.c) */
void rb_input_reset(int nplayers, int local, int delay);   /* frames are counted from 1 */
/* (2026-11-02, input delay 0 by default) frames 1..RB_LEADIN carry no buttons whatever the delay (each game sends empty pads
 * for them; the guess for the other player is empty too): no wrong guess there, so no rollback reaches the match's base
 * state, and a base made a few passes late on one game (r1 "base state is frame n") stays safe, as frames 1..delay did
 * when the delay was at least 1. 4 frames = 67 ms of the countdown. */
#define RB_LEADIN 4
int rb_input_delay(void);
RbPad rb_pad_rest(RbPad p);                        /* the stick clamp both sides apply before a pad is stored or sent */
int rb_pad_equal(const RbPad *a, const RbPad *b);
/* the local player's pad read at `frame`: kept for frame + delay; returns the pad that applies at `frame` (empty in the first frames) */
RbPad rb_input_local(int frame, RbPad now);
int rb_input_local_latest(void);                   /* newest frame with a local pad (frame + delay of the last call) */
int rb_input_local_get(int frame, RbPad *out);     /* 1 when that frame's local pad is known (to send it again) */
/* a pad of player `p` for `frame` from the network; a frame already known is ignored (redundant copies) */
void rb_input_remote_put(int p, int frame, RbPad pad);
int rb_input_remote_latest(int p);                 /* newest consecutive frame received from player p (0 = none) */
/* the pad player p uses at `frame`: the real one, or the last real one repeated (*predicted = 1, recorded for the check) */
RbPad rb_input_get(int p, int frame, int *predicted);
/* the earliest frame whose prediction was wrong since the last call, or 0; the caller rolls back to it */
int rb_input_take_mismatch(void);
/* the newest frame every player's real pad is known for (the "finalized" frame: no rollback can reach before it) */
int rb_input_finalized(void);

/* ---------------------------------------------------------------- wire format (rb_wire.c), little-endian */
enum { RB_MSG_PADS = 1, RB_MSG_ACK = 2, RB_MSG_PING = 3, RB_MSG_PONG = 4 };
#define RB_PAD_WIRE 16
#define RB_PADS_HEAD 20
#define RB_MAX_PADS_PER_MSG 64
typedef struct {
    int type;
    int player, count;                             /* PADS: who sent, how many pads (newest first) */
    int32_t newest, ack, check_frame;              /* PADS: frame of the first pad; the sender's ack of our pads; checksum frame */
    uint32_t check;
    RbPad pads[RB_MAX_PADS_PER_MSG];
    uint64_t time_us, echo_us;                     /* PING: time_us; PONG: echo_us = the ping's, time_us = the answerer's */
    int32_t frame;                                 /* ACK, PING, PONG: the sender's current frame */
} RbMsg;
size_t rb_msg_build(uint8_t *buf, size_t cap, const RbMsg *m);           /* bytes written, 0 when it does not fit */
int rb_msg_parse(const uint8_t *buf, size_t len, RbMsg *m);              /* 1 = a valid message */

/* ---------------------------------------------------------------- time sync (rb_timesync.c) */
typedef struct { int skip, advance; double speed; } RbSyncAction;
void rb_ts_reset(int frame_us);
/* one sample: how far (microseconds) this side's frame clock is ahead of the peer's (positive = ahead) */
void rb_ts_sample(int32_t offset_us);
/* every 30 frames: what to do (skip one frame, run `advance` extra frames spread out, or a speed factor) */
RbSyncAction rb_ts_decide(int frame);
int32_t rb_ts_offset(void);                        /* the current estimate */

#endif
