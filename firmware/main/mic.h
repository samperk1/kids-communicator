#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Microphone capture into PSRAM, as a complete WAV ready to POST.
 *
 * 16 kHz mono: what HA's speech-to-text wants, and 1/5th the bytes of the
 * 44.1 kHz stereo the music path uses.
 *
 * 🚨 THE MONO MIC LANDS ON THE **LEFT** I2S SLOT ON THIS BOARD. The right slot
 * reads exactly 0. Unit #1 exposes this as a `micslot l|r` command because the
 * answer differs per board; for the ES3C28P it was measured as LEFT. */

#define MIC_RATE      16000
#define MIC_MAX_SECS  12

bool  mic_begin(void);                  /* allocate + flush; false = no memory */
void  mic_pump(void);                   /* drain DMA; call often while held    */
bool  mic_full(void);                   /* capture buffer is at MIC_MAX_SECS   */
size_t mic_end(void);                   /* returns WAV bytes captured          */
void  mic_free(void);

const uint8_t *mic_wav(void);           /* WAV header + PCM, NULL if unused    */
size_t         mic_wav_len(void);
int            mic_peak(void);          /* last capture's peak, 0-32767        */
int            mic_seconds(void);
size_t         mic_bytes(void);   /* raw PCM captured so far */
