/*
 * Microphone capture, ES8311 ADC -> PSRAM -> WAV.
 *
 * Split into begin/pump/end rather than one blocking call, because the capture
 * spans a finger being held on a button and LVGL event callbacks MUST NOT
 * BLOCK. Unit #1 learned this the expensive way: recording inside the press
 * handler froze the interface for the length of the utterance plus the network
 * round trip.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "hw.h"
#include "audio.h"
#include "applog.h"
#include "mic.h"

static const char *TAG = "mic";

#define WAV_HDR   44
#define MAX_PCM   (MIC_RATE * 2 * MIC_MAX_SECS)      /* 16-bit mono */

static uint8_t *s_buf;            /* WAV header then PCM, one allocation */
static size_t   s_pcm_len;
static int      s_peak;
static bool     s_active;
static size_t   s_discard;        /* bytes still to throw away after open */

/* 🚨 THE ES8311 EMITS A FULL-SCALE DC TRANSIENT FOR THE FIRST FRAMES AFTER
 * OPEN, and it looks exactly like a successful loud capture. Measured here
 * 2026-08-14: peak jumped from 0% to 32768 (100% FS, the negative rail) within
 * one second of every capture and stayed pinned there, which read as "the gain
 * is far too high" and sent gain-tuning in the wrong direction entirely.
 * Throw the first 300ms away before believing any level. */
#define DISCARD_BYTES  (MIC_RATE * 2 * 300 / 1000)

static void write_wav_header(uint8_t *h, uint32_t pcm_bytes)
{
    uint32_t rate = MIC_RATE, byte_rate = MIC_RATE * 2;
    uint16_t ch = 1, bits = 16, align = 2, fmt = 1;
    uint32_t riff = 36 + pcm_bytes, sub1 = 16;

    memcpy(h + 0,  "RIFF", 4);      memcpy(h + 4,  &riff, 4);
    memcpy(h + 8,  "WAVEfmt ", 8);  memcpy(h + 16, &sub1, 4);
    memcpy(h + 20, &fmt, 2);        memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4);       memcpy(h + 28, &byte_rate, 4);
    memcpy(h + 32, &align, 2);      memcpy(h + 34, &bits, 2);
    memcpy(h + 36, "data", 4);      memcpy(h + 40, &pcm_bytes, 4);
}

bool mic_begin(void)
{
    if (!s_buf) {
        s_buf = heap_caps_malloc(WAV_HDR + MAX_PCM, MALLOC_CAP_SPIRAM);
        if (!s_buf) {
            applog("[mic] no PSRAM for capture");
            return false;
        }
    }
    s_pcm_len = 0;
    s_peak    = 0;
    s_discard = DISCARD_BYTES;

    /* 16 kHz mono for STT. Music runs at 44100 and the codec has to be moved
     * between them — see audio_set_rate(). */
    audio_set_rate(MIC_RATE);

    /* 🚨 Drain first. The RX DMA has been filling the whole time the UI was
     * doing something else, and whatever is sitting in it belongs to the past,
     * not to this recording. */
    audio_flush_rx();
    /* Start each capture from unity, not from wherever the last talker left the
     * gain — otherwise a shout followed by a whisper begins attenuated. */
    audio_agc_reset();
    s_active = true;
    return true;
}

void mic_pump(void)
{
    if (!s_active || !s_buf) return;
    if (s_pcm_len >= MAX_PCM) return;

    /* Stereo in, mono out: the codec is opened 2-channel, and on this board the
     * mono mic is on the LEFT slot with the right reading exactly 0. Taking
     * every other sample is the channel selection. */
    static int16_t chunk[512 * 2];
    size_t got = 0;

    /* 🚨 USE WHATEVER ARRIVED, EVEN ON ESP_ERR_TIMEOUT.
     *
     * This buffer is 2048 bytes = 32ms of 16kHz stereo. The first version asked
     * for it with a 20ms timeout and then THREW THE DATA AWAY unless the return
     * was ESP_OK — but i2s_channel_read() returns ESP_ERR_TIMEOUT when it could
     * not fill the WHOLE buffer in time, while still reporting the bytes it did
     * read. So every single read timed out, every partial buffer was discarded,
     * and the capture ended with exactly zero bytes: a held button that recorded
     * nothing, reported as "too short" no matter how long it was held.
     * `got` is the truth here, not the return code. */
    audio_read(chunk, sizeof(chunk), &got, 100);
    if (got == 0) return;

    /* Drop the codec's start-up transient before it poisons the peak. */
    if (s_discard) {
        size_t drop = (got < s_discard) ? got : s_discard;
        s_discard -= drop;
        if (drop == got) return;
        memmove(chunk, (uint8_t *)chunk + drop, got - drop);
        got -= drop;
    }

    int16_t *out = (int16_t *)(s_buf + WAV_HDR + s_pcm_len);
    size_t frames = got / (2 * sizeof(int16_t));
    size_t room   = (MAX_PCM - s_pcm_len) / sizeof(int16_t);
    if (frames > room) frames = room;

    for (size_t i = 0; i < frames; i++) {
        int16_t v = chunk[i * 2];              /* LEFT slot */
        out[i] = v;
        int a = v < 0 ? -v : v;
        if (a > s_peak) s_peak = a;
    }
    s_pcm_len += frames * sizeof(int16_t);
}

bool mic_full(void)
{
    return s_buf && s_pcm_len >= MAX_PCM;
}

size_t mic_end(void)
{
    s_active = false;
    if (!s_buf || s_pcm_len == 0) return 0;
    write_wav_header(s_buf, (uint32_t)s_pcm_len);

    /* Peak as a percentage of full scale is the number worth logging: it
     * separates "the mic is deaf" from "nothing was said" from "clipping",
     * which all look identical in a failed transcript. */
    applog("[mic] %.1fs peak %d (%d%% FS) agc x%.1f",
           s_pcm_len / (float)(MIC_RATE * 2), s_peak, s_peak * 100 / 32767,
           audio_agc_gain());
    ESP_LOGI(TAG, "captured %u bytes, peak %d", (unsigned)s_pcm_len, s_peak);
    /* With ALC running, the question is whether it moved a gain register at
     * all - 0x16 is the candidate. Compare against the boot dump. */
    if (audio_alc()) audio_dump_codec_regs("post-cap ");
    return WAV_HDR + s_pcm_len;
}

void mic_free(void)
{
    if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; }
    s_pcm_len = 0;
}

const uint8_t *mic_wav(void)     { return s_buf; }
size_t         mic_wav_len(void) { return s_pcm_len ? WAV_HDR + s_pcm_len : 0; }
int            mic_peak(void)    { return s_peak; }
int            mic_seconds(void) { return (int)(s_pcm_len / (MIC_RATE * 2)); }
size_t         mic_bytes(void)   { return s_pcm_len; }
