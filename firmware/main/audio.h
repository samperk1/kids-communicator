#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "player.h"

/* The I2S + codec layer, owned here rather than in player.c because the mic
 * needs the same hardware.
 *
 * 🚨 THE ES8311 IS ONE CODEC DOING TWO JOBS. Playback is DAC, the mic is ADC,
 * and both ride I2S0 — so opening it DAC-only (which is what player.c used to
 * do) means there is no capture path at all, not merely a quiet one. It is
 * opened in WORK_MODE_BOTH here, matching a mic test sketch which is the rig
 * that proved the mic works on this board.
 *
 * ⚠️ Music is 44100 Hz stereo and speech-to-text wants 16000 Hz mono, so the
 * rate has to move between them. That is audio_set_rate(), and it is not free —
 * it stops both channels and reopens the codec, so do it around a capture, not
 * inside a loop. */

void audio_init(int rate);

int  audio_rate(void);
/* Reconfigures both directions and the codec. Safe to call with the same rate
 * (returns immediately). */
void audio_set_rate(int rate);

void audio_amp(bool on);
bool audio_amp_on(void);   /* so a cue can restore it instead of killing it */
void audio_set_volume(int pct);          /* 0-100 */
/* Temporarily override the volume (for a spoken reply); returns the old value
 * so the caller can restore the user's music setting afterwards. */
int  audio_push_volume(int pct);
void audio_set_mic_gain(float db);

/* ES8311 hardware ALC (automatic level control) on the CAPTURE path — a child
 * whispering close and shouting from across the room is 30-40dB of range that
 * no fixed mic gain covers. Off by default so it can be A/B'd from Settings
 * against the fixed MIC_GAIN_DB; the derivation of the register values, and
 * why the researched ones were not used, is in audio.c. */
void audio_set_alc(bool on);
bool audio_alc(void);
/* Call at the start of a press/capture so the previous talker's gain is not
 * carried into the next one. */
void audio_agc_reset(void);
/* What the auto-level actually did, for the log — inferring it from the peak
 * alone cost a whole test round. */
float audio_agc_gain(void);
/* Diagnostic: ALC on with the most aggressive target the level table allows.
 * Not persisted. Long-press the Settings row. See audio.c for why it exists. */
void audio_alc_probe(void);

/* Logs 0x14/0x16/0x17/0x18-0x1C to the serial log AND the on-screen log. The
 * ALC power-on defaults are undocumented, so this board is the only source of
 * truth for them. Read-only. */
void audio_dump_codec_regs(const char *when);
/* One-line DAC-side snapshot for the per-second walkie stats. */
void audio_dac_state(char *out, size_t n);

/* ES8311 DAC volume register (0x32), read and written directly. 0xBF is 0dB and
 * the register runs to 0xFF (+32dB) - esp_codec_dev's 100% maps to 0dB, so
 * there may be real headroom above it. Diagnostic/tuning tool: gain above 0dB
 * clips loud material, so the right value is the loudest that stays clean.
 * Returns -1 on I2C failure. */
int  audio_dac_gain_raw(void);
bool audio_set_dac_gain_raw(int val);
int  audio_dac_gain_db(int raw);      /* register counts -> dB relative to 0 */

/* Small-speaker EQ, applied on the INTERNAL SPEAKER PATH ONLY (never the
 * PCM5102A jack, which feeds headphones that can actually do bass).
 * 300Hz high-pass + ~2kHz presence lift + gentle top-end shelf: the driver
 * cannot reproduce bass, so removing it frees headroom and the whole thing goes
 * louder as well as clearer. */
void audio_set_eq(bool on);
bool audio_eq(void);

esp_err_t audio_write(player_sink_t sink, const void *buf, size_t bytes);
esp_err_t audio_read(void *buf, size_t bytes, size_t *got, int timeout_ms);

/* 🚨 Drain the RX DMA before any timed capture.
 *
 * The receive DMA keeps filling whether or not anyone is reading, so a
 * recording that starts without flushing begins with whatever was queued while
 * the UI was busy — on unit #1 that meant the cue beep landed inside the
 * "recording" and produced a suspiciously identical peak every attempt. */
void audio_flush_rx(void);

/* Audible feedback. A device a child uses while looking elsewhere must not fail
 * silently — see the note in audio.c. */
typedef enum {
    CUE_START = 0,   /* rising blip: recording started        */
    CUE_OK,          /* rising pair: understood, working on it */
    CUE_FAIL,        /* falling pair: that did not work        */
    CUE_NOHEAR,      /* low double buzz: heard nothing at all  */
    CUE_MESSAGE,     /* three rising notes: a text has arrived  */
} audio_cue_t;

void audio_earcon(audio_cue_t cue);
void audio_tone(int hz, int ms, int amplitude);
