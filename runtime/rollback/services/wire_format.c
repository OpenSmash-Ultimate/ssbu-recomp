/* Rollback netcode: the wire format of the input messages (rb.h), little-endian, every length checked.
 *   PADS  type 1 | player u8 | count u16 | newest i32 | ack i32 | check_frame i32 | check u32 | count x pad (16 bytes, newest first)
 *   ACK   type 2 | frame i32
 *   PING  type 3 | frame i32 | time_us u64
 *   PONG  type 4 | frame i32 | time_us u64 | echo_us u64
 *   SYNC  type 5 | frame i32 | margin i32 (in ack; older builds count it as a bad message and ignore it)
 *   pad   buttons u64 | lx i16 | ly i16 | rx i16 | ry i16
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <string.h>
#include "../rollback.h"

/* Little-endian writers: the value's bytes, least significant first, at p. */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
/* Little-endian readers: the value stored at p by the matching put. */
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

/* Writes message `m` (type chosen by m->type) into `b`, which holds `cap` bytes. Returns the number of bytes written,
 * or 0 when the message does not fit, its type is unknown, or a PADS message carries too many pads. */
size_t rb_msg_build(uint8_t *b, size_t cap, const RbMsg *m) {
    switch (m->type) {
    case RB_MSG_PADS: {
        if (m->count < 0 || m->count > RB_MAX_PADS_PER_MSG) return 0;
        size_t n = RB_PADS_HEAD + (size_t)m->count * RB_PAD_WIRE;
        if (n > cap) return 0;
        b[0] = RB_MSG_PADS; b[1] = (uint8_t)m->player; put16(b + 2, (uint16_t)m->count);
        put32(b + 4, (uint32_t)m->newest); put32(b + 8, (uint32_t)m->ack); put32(b + 12, (uint32_t)m->check_frame);
        put32(b + 16, m->check);
        for (int i = 0; i < m->count; i++) {
            uint8_t *q = b + RB_PADS_HEAD + i * RB_PAD_WIRE;
            put64(q, m->pads[i].buttons);
            put16(q + 8, (uint16_t)m->pads[i].lx); put16(q + 10, (uint16_t)m->pads[i].ly);
            put16(q + 12, (uint16_t)m->pads[i].rx); put16(q + 14, (uint16_t)m->pads[i].ry);
        }
        return n;
    }
    case RB_MSG_ACK: if (cap < 5) return 0; b[0] = RB_MSG_ACK; put32(b + 1, (uint32_t)m->frame); return 5;
    case RB_MSG_PING: if (cap < 13) return 0; b[0] = RB_MSG_PING; put32(b + 1, (uint32_t)m->frame); put64(b + 5, m->time_us); return 13;
    case RB_MSG_PONG: if (cap < 21) return 0; b[0] = RB_MSG_PONG; put32(b + 1, (uint32_t)m->frame); put64(b + 5, m->time_us); put64(b + 13, m->echo_us); return 21;
    case RB_MSG_SYNC: if (cap < 9) return 0; b[0] = RB_MSG_SYNC; put32(b + 1, (uint32_t)m->frame); put32(b + 5, (uint32_t)m->ack); return 9;
    }
    return 0;
}

/* Reads the `len` bytes at `b` into *m (zeroed first). Returns 1 for a well-formed message and 0 for anything else: too short,
 * wrong length for its type, a player or pad count out of range, or frame numbers that cannot exist (frames count from 1).
 * Nothing in the message is trusted before it has been checked, so it is safe to call on bytes from the network. */
int rb_msg_parse(const uint8_t *b, size_t len, RbMsg *m) {
    memset(m, 0, sizeof *m);
    if (len < 1) return 0;
    m->type = b[0];
    switch (m->type) {
    case RB_MSG_PADS: {
        if (len < RB_PADS_HEAD) return 0;
        m->player = b[1]; m->count = get16(b + 2);
        if (m->player >= RB_MAX_PLAYERS || m->count > RB_MAX_PADS_PER_MSG || len != RB_PADS_HEAD + (size_t)m->count * RB_PAD_WIRE) return 0;
        m->newest = (int32_t)get32(b + 4); m->ack = (int32_t)get32(b + 8); m->check_frame = (int32_t)get32(b + 12);
        m->check = get32(b + 16);
        if (m->newest < 0 || m->newest - m->count + 1 < 1) return 0;   /* frames count from 1 */
        for (int i = 0; i < m->count; i++) {
            const uint8_t *q = b + RB_PADS_HEAD + i * RB_PAD_WIRE;
            m->pads[i].buttons = get64(q);
            m->pads[i].lx = (int16_t)get16(q + 8); m->pads[i].ly = (int16_t)get16(q + 10);
            m->pads[i].rx = (int16_t)get16(q + 12); m->pads[i].ry = (int16_t)get16(q + 14);
        }
        return 1;
    }
    case RB_MSG_ACK: if (len != 5) return 0; m->frame = (int32_t)get32(b + 1); return 1;
    case RB_MSG_PING: if (len != 13) return 0; m->frame = (int32_t)get32(b + 1); m->time_us = get64(b + 5); return 1;
    case RB_MSG_PONG: if (len != 21) return 0; m->frame = (int32_t)get32(b + 1); m->time_us = get64(b + 5); m->echo_us = get64(b + 13); return 1;
    case RB_MSG_SYNC: if (len != 9) return 0; m->frame = (int32_t)get32(b + 1); m->ack = (int32_t)get32(b + 5); return 1;
    }
    return 0;
}
