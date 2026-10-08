#pragma once
#include <stdint.h>

/* IMA ADPCM, 4 bits per sample — the codec that makes voice fit ESP-NOW.
 *
 * 🔑 WHY A CODEC EXISTS AT ALL. The walkie-talkie frame is 20ms of 16kHz
 * 16-bit mono: 320 samples, 640 bytes, plus a header. That is 33 KB/s, and it
 * does not fit down this pipe twice over:
 *
 *   - ESP-NOW's payload is 250 bytes. 640 would need three fragments and a
 *     reassembly buffer per talker. (250 is fixed for us: ESP-NOW v2's 1428
 *     bytes needs IDF 5.4 and this project is on 5.3.)
 *   - Measured ESP-NOW throughput on this hardware is ~25 KB/s. 33 is over it
 *     before headers.
 *
 * At 4 bits a sample, 320 samples become 160 bytes. One 20ms frame fits in ONE
 * ESP-NOW packet with room to spare, at 8.5 KB/s — a third of the proven link.
 * No fragmentation, no reassembly, and a lost packet is 20ms of silence.
 *
 * 🚨 THE STATE TRAVELS WITH EVERY FRAME, AND THAT IS THE WHOLE TRICK.
 *
 * Textbook ADPCM is a running state machine: each nibble is decoded relative to
 * the predictor the one before it left behind. Over a lossless file that is
 * fine. Over a radio it is a disaster — ONE dropped packet and the decoder's
 * predictor is wrong FOREVER, so a single lost frame turns into permanent
 * distortion rather than a 20ms gap.
 *
 * So the encoder snapshots its state into each frame's header and the decoder
 * loads it before decoding. Every frame is then independently decodable and a
 * loss costs exactly the audio it carried. It is the same rule the game
 * protocol already runs on — the whole board travels, never the move — and it
 * is here for exactly the same reason: out of range of any router there is
 * nobody to ask for a resend.
 *
 * ⚠️ Cost of that choice is 3 bytes per frame (150 bytes/s) and a hair of
 * quality at frame boundaries, because the encoder cannot let its predictor
 * drift as freely. Both are cheap next to unrecoverable corruption.
 */

/* Encoder/decoder state. Three bytes on the wire, so keep it packed and small.
 * `index` is a position in the 89-entry step table and MUST stay 0..88 — a
 * corrupted one read off the air would index out of bounds, so both directions
 * clamp rather than trust. */
typedef struct __attribute__((packed)) {
    int16_t predictor;   /* last decoded sample */
    uint8_t index;       /* step table position, 0..88 */
} adpcm_state_t;

/* Encode `n` samples into `n/2` bytes (two nibbles each, low nibble first).
 * `st` is read as the starting state and updated to the ending state, so the
 * CALLER snapshots it BEFORE the call to put in the frame header.
 * ⚠️ `n` must be even. */
void adpcm_encode(adpcm_state_t *st, const int16_t *in, uint8_t *out, int n);

/* Decode `n` samples from `n/2` bytes. `st` is loaded from the frame header;
 * it is updated but the caller normally discards it, since the next frame
 * carries its own. */
void adpcm_decode(adpcm_state_t *st, const uint8_t *in, int16_t *out, int n);
