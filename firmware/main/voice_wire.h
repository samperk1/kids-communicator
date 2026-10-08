/* ------------------------------------------------------------------ *
 * Voice over ESP-NOW — the wire format, shared by BOTH ends.
 *
 * 🔑 THE FEATURE THIS EXISTS FOR. The Talkie was UDP over the house wifi, so
 * away from home it said "no wifi" and did nothing — while the same three
 * handsets could see each other perfectly over ESP-NOW and text each other over
 * it. The builder found that in a car on 2026-08-26 with Kid 2, Kid 1 and Kid 4 five feet
 * apart. Voice was the one feature that never moved to the peer-to-peer radio.
 *
 * ⚠️ ONE HEADER, TWO TOOLCHAINS, BYTE FOR BYTE — the discipline aud_wire.h and
 * msg_core.cpp already follow. The CYD's Arduino port will include THIS file
 * unchanged. Every field is fixed-width and the struct is packed; do not add a
 * field without changing VOX_MAGIC.
 *
 * ⚠️ 250-BYTE ESP-NOW PAYLOAD. This project's S3 is on IDF 5.3 and cannot do
 * ESP-NOW v2's 1428 bytes. The format is sized so the biggest frame is 172
 * bytes and NOTHING is ever fragmented.
 * ------------------------------------------------------------------ */
#ifndef VOICE_WIRE_H
#define VOICE_WIRE_H

#include <stdint.h>

#define VOX_MAGIC    0x31584F56u    /* "VOX1", little-endian */

/* Same rate and cadence as the UDP path, deliberately: the mic, the codec and
 * audio_set_rate() are all already at 16kHz/20ms, so the two transports differ
 * only in how the bytes leave the board. */
#define VOX_RATE     16000
#define VOX_FRAME_MS 20
#define VOX_SAMPLES  320            /* VOX_RATE * VOX_FRAME_MS / 1000 */
#define VOX_BYTES    160            /* 4 bits a sample */
#define VOX_HDR      12
#define VOX_NAME     10             /* matches msg_frame_t.name — 9 + NUL */
#define VOX_MAX      (VOX_HDR + VOX_BYTES)   /* 172, under the 250 limit */

enum {
    VOX_START = 1,   /* PTT down. Payload is the talker's name, VOX_NAME bytes.
                      * Opens the far end's amp and latches who is speaking. */
    VOX_AUDIO = 2,   /* 160 bytes of ADPCM = 20ms */
    VOX_END   = 3,   /* PTT up. Closes the amp.
                      *
                      * ⚠️ NEVER RELY ON IT ARRIVING. It is one unacknowledged
                      * broadcast; if it is lost the far end would sit "hearing"
                      * forever with its amp on, eating battery and looking
                      * broken. The UDP path already learned this — receivers
                      * MUST also time the talker out on silence. */
};

typedef struct __attribute__((packed)) {
    uint32_t magic;       /* VOX_MAGIC */
    uint8_t  type;        /* VOX_* */
    uint8_t  seq;         /* rolling; the receiver only uses it to COUNT loss */
    uint16_t from_id;     /* last two MAC bytes, same ids the game protocol uses */
    /* 🚨 THE CODEC STATE, AND IT IS THE WHOLE DESIGN. IMA ADPCM is normally a
     * running state machine, so one lost packet corrupts everything after it —
     * forever. Carrying the encoder's state as it was at the START of this
     * frame makes every frame independently decodable, so a loss costs exactly
     * the 20ms it carried. Three bytes per frame, 150 bytes a second, and it
     * buys the difference between a gap and a ruined channel. Same rule the
     * game protocol runs on: the whole state travels, never the delta. */
    int16_t  predictor;
    uint8_t  index;       /* step table position, 0..88 — receivers MUST clamp */
    uint8_t  pad;         /* keeps the payload 4-byte aligned; must be 0 */
} vox_hdr_t;

#endif
