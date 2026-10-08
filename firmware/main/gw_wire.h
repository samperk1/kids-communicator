/* ------------------------------------------------------------------ *
 * Handset <-> Gateway requests — the wire format, shared by BOTH ends.
 *
 * 🔑 WHAT THE GATEWAY IS FOR. The handsets are going to stop carrying the WiFi
 * password and the Home Assistant token. Instead one mains-powered board holds
 * them, and a handset ASKS it over ESP-NOW. Possession of that board is the
 * permission — a handset that leaves the house simply stops being able to ask,
 * with nothing to revoke and no credential in a child's pocket.
 *
 * ⚠️ ONE HEADER, TWO BUILDS, BYTE FOR BYTE — the discipline voice_wire.h and
 * aud_wire.h already follow. The gateway firmware compiles this same file.
 * Every field is fixed-width and every struct is packed; do not add a field
 * without bumping GW_VER.
 *
 * ⚠️ 250-BYTE ESP-NOW PAYLOAD on IDF 5.3 (no ESP-NOW v2 here). Everything below
 * is sized to fit in one frame with room to spare. Nothing is fragmented.
 *
 * 🚨 GATEWAYS ARE ADDRESSED EXPLICITLY, NEVER ASSUMED TO BE THE ONLY ONE.
 * This is the v1 rule from COMMUNICATOR-SECURITY-PLAN.md and it exists so that
 * adding a second gateway later is a configuration change and not a rewrite.
 * A request carries the gateway's id in msg_frame_t.to_id — it is NEVER
 * broadcast — and a gateway answers only what was addressed to it. A handset
 * finds gateways by their beacon flag (F_IS_GATEWAY) rather than by a compiled
 * address, so a replacement board is just another beacon.
 * ------------------------------------------------------------------ */
#ifndef GW_WIRE_H
#define GW_WIRE_H

#include <stdint.h>

/* 3 — 2026-09-09: ASK_REPLY, the agent's answer (phase B of the voice relay).
 * ⚠️ Bumped deliberately rather than added silently. A v2 handset asking a v3
 * gateway is REFUSED with a log line on both sides; a silently-additive change
 * would instead have shown the transcript and no answer, which is what a
 * working relay looks like from arm's length. 2026-09-09 cost hours to a
 * gateway that had quietly OTA'd itself back to v1. */
#define GW_VER   3

/* Biggest payload that fits one ESP-NOW frame on IDF 5.3 (250 bytes total,
 * minus the 28-byte msg_frame_t header). Nothing here is ever fragmented. */
#define GW_CHUNK_MAX  200

enum {
    GW_OP_ASK = 3,      /* 🗣️ THE POINT OF THE WHOLE GATEWAY. A handset sends
                         * microphone audio up and gets an answer back, so it
                         * needs NO Home Assistant token and NO wifi password of
                         * its own. See the sub-types below. */
    GW_OP_SPEED = 2,    /* 🔬 MEASUREMENT ONLY, and it exists because the number
                         * everyone would otherwise assume is wrong. The walkie
                         * sustains 8.4 KB/s, but that is a PACED real-time rate
                         * — 160 bytes every 20ms because that is what speech
                         * needs — not a ceiling. The voice relay will push
                         * ~24KB in a burst, and whether that costs 0.5s or 3s
                         * decides whether it needs flow control at all.
                         * Guessing it would repeat the -75 dBm mistake. */
    GW_OP_TIME = 1,     /* "what time is it?" — the first and simplest op.
                         * Deliberately first: it exercises the whole
                         * request/response path with a payload small enough
                         * that nothing can hide in it, before voice relay
                         * arrives and makes every failure ambiguous. */
};

/* Handset -> gateway. */
typedef struct __attribute__((packed)) {
    uint8_t  ver;       /* GW_VER */
    uint8_t  op;
    uint8_t  seq;       /* echoed back; a late reply is discarded, not applied */
    uint8_t  rsv;
} gw_req_t;

/* GW_OP_SPEED request: "send me `frames` filler frames as fast as you can".
 * The reply is `frames` x gw_speed_chunk_t, then nothing — the receiver times
 * from the first chunk to the last and divides. */
typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  op;
    uint8_t  seq;
    uint8_t  rsv;
    uint16_t frames;    /* how many chunks to send back */
} gw_speed_req_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  op;
    uint16_t index;     /* 0..frames-1, so losses are countable not guessed */
    uint8_t  fill[GW_CHUNK_MAX - 4];
} gw_speed_chunk_t;

/* Gateway -> handset, for GW_OP_TIME.
 *
 * 🔑 UTC SECONDS, NOT A FORMATTED LOCAL TIME. The handset already sets its own
 * TZ (power.c: "EST5EDT,M3.2.0/2,M11.1.0/2") and applies DST itself, so sending
 * a wall-clock string would mean two places deciding what the offset is and
 * eventually disagreeing across a DST boundary. One number, one authority. */
typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  op;
    uint8_t  seq;
    uint8_t  ok;        /* 0 = the gateway does not know the time either */
    int64_t  unix_secs; /* seconds since 1970-01-01 UTC */
} gw_time_rsp_t;


/* ------------------------------------------------------------------ *
 * GW_OP_ASK — voice up, answer down
 *
 * One op with sub-types rather than five ops, so a frame is self-describing
 * and a stray one from an older build cannot be mistaken for a different
 * request. ⚠️ Direction is carried by the FRAME TYPE, not by these: everything
 * the gateway sends is T_GW_RSP. Getting that wrong on 2026-09-07 made a
 * request vanish with no error anywhere and looked exactly like poor range.
 *
 * 🔑 THE GATEWAY DOES NOT BUFFER THE RECORDING. `GW-AAAA` is a plain ESP32
 * with NO PSRAM (see comm-gateway/sdkconfig.defaults), and one binary serves
 * both gateways, so a 384KB WAV cannot be held anywhere. Instead the gateway
 * opens the HA request with a known Content-Length from ASK_BEGIN and writes
 * each chunk through as it arrives — which is what ha_post_audio() on the
 * handset has always done, and which was MEASURED on 2026-09-08 to work at
 * 50 KB/s for the full 12-second worst case (HTTP 200, 7.5s).
 *
 * 🚨 AND THAT IS WHY THERE IS A WINDOW INSTEAD OF A BLAST. A streamed POST
 * cannot rewind: once a byte is written to HA it is committed, so a gap cannot
 * be repaired afterwards. The SPEED test blasts and counts losses at the end;
 * this cannot. The gateway therefore writes only the CONTIGUOUS prefix it
 * holds and acks the next index it needs, and the handset keeps at most
 * GW_ASK_WIN chunks in flight.
 * ------------------------------------------------------------------ */

enum {
    ASK_BEGIN = 0,      /* H->G  how many bytes are coming */
    ASK_DATA  = 1,      /* H->G  one indexed slice of the WAV */
    ASK_ACK   = 2,      /* G->H  "I have everything below `next`" */
    ASK_TEXT  = 3,      /* G->H  what HA heard, in pieces */
    ASK_DONE  = 4,      /* G->H  finished, with a reason if it failed */
    ASK_REPLY = 5,      /* G->H  what the AGENT answered, in pieces.
                         * Same shape as ASK_TEXT; a separate sub so the
                         * handset never has to guess which text it holds. */
};

/* 194 bytes of audio per frame: 200 payload minus the 6-byte header below.
 * The 12-second cap (MIC_MAX_SECS) makes 384,044 bytes = 1,980 chunks, so the
 * index is comfortably 16-bit and nothing here is unbounded. */
#define GW_ASK_DATA_MAX  (GW_CHUNK_MAX - 6)

/* Chunks in flight. 32 x 194 = 6.2KB, which fits a no-PSRAM gateway's internal
 * RAM and is ~110ms of audio at the measured 58 KB/s — comfortably longer than
 * an ack round trip, so the window is never the thing limiting throughput. */
#define GW_ASK_WIN       32

typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  op;        /* GW_OP_ASK */
    uint8_t  seq;       /* rises per ASK; a frame from a stale ask is dropped */
    uint8_t  sub;       /* ASK_BEGIN */
    uint32_t bytes;     /* total WAV bytes, INCLUDING the 44-byte header */
    uint16_t rate;      /* sample rate, so the gateway builds HA's header */
    uint16_t chunks;    /* how many ASK_DATA frames to expect */
} gw_ask_begin_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  op;
    uint8_t  seq;
    uint8_t  sub;       /* ASK_DATA */
    uint16_t index;     /* 0-based; the gateway writes them IN ORDER only */
    uint8_t  data[GW_ASK_DATA_MAX];
} gw_ask_data_t;

/* 🔑 CUMULATIVE, NOT PER-FRAME. `next` is the lowest index the gateway still
 * needs, so one ack that arrives replaces every ack that was lost before it —
 * there is no ack to retransmit and no per-frame bookkeeping to get wrong. */
typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  op;
    uint8_t  seq;
    uint8_t  sub;       /* ASK_ACK */
    uint16_t next;      /* send from here; everything below it is committed */
} gw_ask_ack_t;

typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  op;
    uint8_t  seq;
    uint8_t  sub;       /* ASK_TEXT */
    uint8_t  part;      /* 0..n-1 */
    uint8_t  last;      /* 1 on the final part */
    char     text[GW_CHUNK_MAX - 6];
} gw_ask_text_t;

/* Why it ended. A handset that gets nothing must be able to say WHICH thing
 * failed — "no answer" covers a dead gateway, a dead HA and a dead token, and
 * those are three different jobs for whoever is holding it. */
enum {
    ASK_OK = 0,
    ASK_ERR_HA    = 1,  /* HA answered, but not 200 */
    ASK_ERR_NET   = 2,  /* the gateway could not reach HA at all */
    ASK_ERR_EMPTY = 3,  /* 200, but HA transcribed nothing */
    ASK_ERR_BUSY  = 4,  /* another handset is already asking */
    ASK_ERR_GAP   = 5,  /* the upload never completed; chunks stopped arriving */
};

typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  op;
    uint8_t  seq;
    uint8_t  sub;       /* ASK_DONE */
    uint8_t  err;       /* ASK_OK or one of the above */
    uint8_t  rsv;
    uint16_t status;    /* HA's HTTP status when there was one, else 0 */
} gw_ask_done_t;

#endif /* GW_WIRE_H */
