/*
 * I2S and the ES8311. Lifted out of player.c so the microphone can share it.
 *
 * Two independent sinks, exactly as measured on the bench 2026-08-12:
 *   I2S0 -> ES8311 -> FM8002E -> onboard speaker   (and ES8311 ADC <- mic)
 *   I2S1 -> PCM5102A -> line out
 *
 * ⚠️ MCLK IS 256x, NOT 384x. The ES8311's clock-coefficient table has no entry
 * for 384x at 44.1kHz; the codec then gets dividers that do not match the bit
 * clock it is actually receiving and music comes out as very quiet static — not
 * an error, just garbage. The PCM5102A is immune (SCK tied low, internal PLL),
 * which makes a useful A/B: if the jack plays and the speaker hisses, suspect
 * the MCLK ratio.
 */

#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "hw.h"
#include "settings.h"
#include "driver/i2c_master.h"
#include "display.h"
#include "applog.h"
#include "audio.h"

static const char *TAG = "audio";

/* Defined with the speaker EQ further down; needed by audio_init/audio_set_rate. */
static void eq_configure(int rate);
/* Defined with the ALC block further down; needed by codec_open(). */
static void alc_apply(void);

static i2s_chan_handle_t s_spk, s_rx, s_dac;
static esp_codec_dev_handle_t s_codec;
static int   s_rate;
static int   s_volume = 70;
static float s_mic_gain = MIC_GAIN_DB;
static bool  s_alc;              /* ES8311 hardware ALC; see the block below */
static int   s_alc_level = 0xC8; /* 0x19 target pair; 0xFF is the probe below  */
static float s_agc_gain  = 1.0f; /* software auto-level; see the block below   */
static bool  s_agc_primed;       /* first speech block of this press seen yet?  */

/* 🚨 One lock over every I2S/codec operation.
 *
 * audio_set_rate() disables three channels and reopens the codec. If the
 * player task is inside i2s_channel_write() with portMAX_DELAY on a channel
 * being torn down underneath it, that write can block forever. The voice path
 * changes the rate twice per interaction (44100 -> 16000 to record, back
 * again), so this race is on the main path, not a corner. */
static SemaphoreHandle_t s_lock;
#define LOCK()   do { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_lock) xSemaphoreGiveRecursive(s_lock); } while (0)

static bool s_amp_on;
void audio_amp(bool on) { s_amp_on = on; gpio_set_level(PIN_AMP_EN, on ? 0 : 1); }
bool audio_amp_on(void) { return s_amp_on; }
int  audio_rate(void)   { return s_rate; }

static i2s_std_config_t std_cfg(int rate, bool with_din)
{
    i2s_std_config_t c = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = { .mclk = I2S0_MCK, .bclk = I2S0_BCK, .ws = I2S0_WS,
                      .dout = I2S0_DO,
                      .din  = with_din ? I2S0_DI : I2S_GPIO_UNUSED,
                      .invert_flags = { false, false, false } },
    };
    c.clk_cfg.mclk_multiple = I2S_MCLK_MULT;
    return c;
}

static int curve_volume(int pct);

static void codec_open(int rate)
{
    esp_codec_dev_sample_info_t si = { .bits_per_sample = 16, .channel = 2,
                                       .channel_mask = 0x03, .sample_rate = rate };
    esp_codec_dev_open(s_codec, &si);
    /* Through the curve, like every other volume write — the codec is reopened
     * on every rate change, and a raw value here would undo the mapping twice
     * per voice interaction. */
    esp_codec_dev_set_out_vol(s_codec, curve_volume(s_volume));
    /* 🔑 This mic needs MORE ANALOG GAIN, never digital gain after the fact.
     * 18/24/30 dB were all noise floor on this board and nothing clips even at
     * 46 — the earlier "40 dB is overdriving the ADC" guess was simply wrong. */
    if (esp_codec_dev_set_in_gain(s_codec, s_mic_gain) != ESP_CODEC_DEV_OK)
        ESP_LOGW(TAG, "set_in_gain(%.0f) failed, continuing", s_mic_gain);
    /* AFTER set_in_gain, never before: that call writes 0x16, which is very
     * likely the register the ALC servo works on. */
    alc_apply();
}

void audio_init(int rate)
{
    /* Before the first codec_open(), which applies it. */
    s_alc = settings_alc();
    eq_configure(rate);
    gpio_config_t g = { .pin_bit_mask = 1ULL << PIN_AMP_EN, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&g);
    audio_amp(false);

    s_lock = xSemaphoreCreateRecursiveMutex();
    s_rate = rate;

    /* TX and RX are created as a PAIR on I2S0 — one call, both handles. The
     * ES8311 plays and records over the same bus and the same clocks. */
    i2s_chan_config_t c0 = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    c0.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&c0, &s_spk, &s_rx));
    i2s_std_config_t s0 = std_cfg(rate, true);
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_spk, &s0));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx,  &s0));
    ESP_ERROR_CHECK(i2s_channel_enable(s_spk));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));

    i2s_chan_config_t c1 = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    c1.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&c1, &s_dac, NULL));
    i2s_std_config_t s1 = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        /* No MCLK: the PCM5102A ties SCK low and runs its own PLL. Feeding it
         * one is not merely unnecessary, it is a pin this board does not have
         * spare. */
        .gpio_cfg = { .mclk = I2S_GPIO_UNUSED, .bclk = I2S1_BCK, .ws = I2S1_WS,
                      .dout = I2S1_DO, .din = I2S_GPIO_UNUSED,
                      .invert_flags = { false, false, false } },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_dac, &s1));
    ESP_ERROR_CHECK(i2s_channel_enable(s_dac));

    audio_codec_i2c_cfg_t ic = { .port = I2C_NUM_0,
                                 .addr = ES8311_CODEC_DEFAULT_ADDR,
                                 .bus_handle = display_i2c_bus() };
    const audio_codec_ctrl_if_t *ctrl = audio_codec_new_i2c_ctrl(&ic);
    audio_codec_i2s_cfg_t idc = { .port = I2S_NUM_0,
                                  .tx_handle = s_spk, .rx_handle = s_rx };
    const audio_codec_data_if_t *data = audio_codec_new_i2s_data(&idc);
    es8311_codec_cfg_t ec = {
        .ctrl_if = ctrl, .gpio_if = audio_codec_new_gpio(),
        /* BOTH, not DAC. This is the line that gives the device a microphone. */
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .master_mode = false, .use_mclk = true, .pa_pin = -1,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
        .mclk_div = I2S_MCLK_MULT,
    };
    const audio_codec_if_t *es = es8311_codec_new(&ec);
    esp_codec_dev_cfg_t dc = { .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
                               .codec_if = es, .data_if = data };
    s_codec = esp_codec_dev_new(&dc);
    codec_open(rate);

    ESP_LOGI(TAG, "audio ready at %d Hz, MCLK %dx, speaker+jack+mic",
             rate, I2S_MCLK_MULT);
    audio_dump_codec_regs("boot ");
}

void audio_set_rate(int rate)
{
    /* Biquad coefficients are sample-rate dependent: 44.1k coefficients on a
     * 22.05k stream would put every corner frequency in the wrong place. */
    eq_configure(rate);
    if (rate == s_rate) return;
    LOCK();
    if (rate == s_rate) { UNLOCK(); return; }   /* raced; someone else did it */

    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    clk.mclk_multiple = I2S_MCLK_MULT;

    esp_codec_dev_close(s_codec);

    /* A channel must be disabled before its clock can be reconfigured.
     *
     * ⚠️ Return value deliberately ignored: esp_codec_dev_close() above has
     * ALREADY disabled the two I2S0 channels, so these log
     * "the channel has not been enabled yet" at error level. Harmless, but it
     * looks like a fault in the log at exactly the moment someone is debugging
     * the voice path, so the intent is written down rather than left to be
     * rediscovered. */
    esp_log_level_set("i2s_common", ESP_LOG_NONE);
    i2s_channel_disable(s_spk);
    i2s_channel_disable(s_rx);
    i2s_channel_disable(s_dac);
    esp_log_level_set("i2s_common", ESP_LOG_INFO);

    i2s_channel_reconfig_std_clock(s_spk, &clk);
    i2s_channel_reconfig_std_clock(s_rx,  &clk);
    i2s_std_clk_config_t clk1 = I2S_STD_CLK_DEFAULT_CONFIG(rate);
    i2s_channel_reconfig_std_clock(s_dac, &clk1);   /* no MCLK on this one */

    i2s_channel_enable(s_spk);
    i2s_channel_enable(s_rx);
    i2s_channel_enable(s_dac);

    s_rate = rate;
    codec_open(rate);
    UNLOCK();
    ESP_LOGI(TAG, "rate -> %d Hz", rate);
}

/* 🚨 THE SLIDER WAS PASSED STRAIGHT TO THE CODEC, AND THE CODEC IS IN dB.
 *
 * esp_codec_dev spreads 0-100 across the ES8311's whole attenuation range, and
 * that range is enormous — most of the low end is inaudible. So nearly all of
 * the travel did nothing and the top of it did everything. The builder, 2026-08-15:
 * "the last 2% of the slider controls 90% of the change in volume so that needs
 * to be on a curve not 1 for 1".
 *
 * Map the slider onto the part of the scale that is actually audible instead:
 * 1 -> VOL_FLOOR, 100 -> 100, and 0 stays truly silent. That spreads the real
 * change across the whole travel rather than the last two steps.
 * ⚠️ VOL_FLOOR is a first estimate and wants the builder's ears, not arithmetic — if
 * the bottom of the slider is still too loud, raise it; if the low end goes
 * silent too early, lower it. It is the only number to touch. */
#define VOL_FLOOR 55

static int curve_volume(int pct)
{
    if (pct <= 0)   return 0;
    if (pct >= 100) return 100;
    return VOL_FLOOR + (pct * (100 - VOL_FLOOR)) / 100;
}

/* ------------------------------------------------------------------ *
 * ES8311 DAC gain, poked directly
 *
 * 🔑 WHY THIS EXISTS. The builder, 2026-08-14: as an ESPHome media player THIS SAME
 * BOARD is "way louder" than this firmware. Same speaker, same amp - so the gap
 * is configuration, not hardware.
 *
 * The suspicion: the ES8311's DAC volume register (0x32) spans -95.5dB to
 * +32dB in 0.5dB steps, with 0dB at 0xBF. esp_codec_dev_set_out_vol(100)
 * almost certainly maps 100% to 0dB - the top of the NORMAL range, not the top
 * of the register - which would leave up to 32dB of gain never touched.
 *
 * ⚠️ THIS IS A MEASUREMENT TOOL FIRST. Read the register before changing it:
 * if it already reads near 0xFF the theory is wrong and the limit is elsewhere
 * (amp gain, or the speaker itself). Gain above 0dB WILL clip loud material -
 * the useful setting is the loudest one that still sounds clean, which only
 * the builder's ears can find.
 * ------------------------------------------------------------------ */
#define ES8311_REG_DACVOL   0x32
#define ES8311_0DB          0xBF

static i2c_master_dev_handle_t s_codec_i2c;

static bool codec_i2c_ready(void)
{
    if (s_codec_i2c) return true;
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = ES8311_CODEC_DEFAULT_ADDR >> 1,
        .scl_speed_hz    = 100000,
    };
    return i2c_master_bus_add_device(display_i2c_bus(), &dc, &s_codec_i2c) == ESP_OK;
}

/* Generic single-register access, because the DAC gain is no longer the only
 * thing this firmware has to poke directly - the ALC block below is written the
 * same way. Returns -1 / false on I2C failure. */
static int codec_reg_read(uint8_t reg)
{
    if (!codec_i2c_ready()) return -1;
    uint8_t val = 0;
    if (i2c_master_transmit_receive(s_codec_i2c, &reg, 1, &val, 1, 100) != ESP_OK)
        return -1;
    return val;
}

static bool codec_reg_write(uint8_t reg, uint8_t val)
{
    if (!codec_i2c_ready()) return false;
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_codec_i2c, buf, 2, 100) == ESP_OK;
}

int audio_dac_gain_raw(void)
{
    return codec_reg_read(ES8311_REG_DACVOL);
}

bool audio_set_dac_gain_raw(int val)
{
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return codec_reg_write(ES8311_REG_DACVOL, (uint8_t)val);
}

/* Register counts are 0.5dB steps with 0dB at 0xBF, so this is the honest
 * translation for anything user-facing. */
int audio_dac_gain_db(int raw) { return (raw - ES8311_0DB) / 2; }

void audio_set_volume(int pct)
{
    s_volume = pct;                    /* remember what the USER chose */
    if (s_codec) {
        int hw = curve_volume(pct);
        int r = esp_codec_dev_set_out_vol(s_codec, hw);
        if (r != ESP_CODEC_DEV_OK)
            ESP_LOGW(TAG, "set_out_vol(%d) returned %d", hw, r);

        /* 🚨 ONE REGISTER, ONE OWNER. 0x32 IS BOTH THE GAIN AND THE VOLUME.
         *
         * esp_codec_dev's set_out_vol() writes 0x32, and the per-board gain
         * lives in 0x32, so whoever writes last wins. This code used to write
         * last with the wrong model — it scaled the gain LINEARLY BY PERCENT
         * and put that in the register:
         *
         *     extra = (gain - 0dB) * pct / 100;   raw = 0dB + extra;
         *
         * which pins the whole slider between 0dB and the board's gain. Measured
         * on Kid 4 2026-08-25, whose gain is ~0xCF (+8dB): 20% gave 0xC2 (+1.5dB)
         * and 37% gave 0xC5 (+3dB). 🔑 **THE ENTIRE SLIDER WAS WORTH ABOUT
         * 1.5dB**, which is why the builder heard nothing move. Worse, its quiet end was
         * 0dB — still loud — so the control was useless in both directions.
         *
         * The right model: the tuned gain is where 100% SITS, and the slider is
         * an ATTENUATION below it. Amplitude squared, i.e. 40*log10(pct/100),
         * which is 0.5dB steps of -12dB at half and -40dB at a tenth — the same
         * law soft_volume() uses on the jack, so both outputs behave alike.
         *
         * ⚠️ An untuned board stores 0. It gets 0dB as its reference rather
         * than being treated as "no gain, so no volume either". */
        int gain = settings_dac_gain();
        if (gain <= 0) gain = ES8311_0DB;
        int raw = 0;
        if (pct > 0) {
            float db = 40.0f * log10f((float)pct / 100.0f);
            raw = gain + (int)lrintf(db * 2.0f);      /* register = 0.5dB steps */
            if (raw < 0)   raw = 0;
            if (raw > 255) raw = 255;
        }
        audio_set_dac_gain_raw(raw);
        /* 🔎 READ 0x32 BACK. The speaker half of the builder's report is not explained
         * the way the jack half is — the ES8311 IS in the speaker path — so
         * this stops the next round being a guess. If the register moves with
         * the slider and the sound does not, the codec is not the limit; if it
         * does not move, esp_codec_dev is refusing the write. */
        applog("[vol] %d%% -> DACVOL 0x%02X (%+ddB, board gain 0x%02X)",
               pct, audio_dac_gain_raw(),
               audio_dac_gain_db(audio_dac_gain_raw()), gain);
    } else {
        applog("[vol] %d%% but there is NO CODEC - jack only", pct);
    }
}

/* Push the codec to full for a spoken reply without disturbing the music
 * volume the user chose. Returns the previous setting so it can be put back.
 *
 * The builder, 2026-08-14: as an ESPHome media player this same board is "way louder"
 * than this firmware — so the hardware clearly has more to give and the limit
 * was mine. That config runs the media player at 85% and lets HA scale on top;
 * this was sitting at a fixed 70 for everything. */
int audio_push_volume(int pct)
{
    int was = s_volume;
    audio_set_volume(pct);
    return was;
}

void audio_set_mic_gain(float db)
{
    s_mic_gain = db;
    if (s_codec) esp_codec_dev_set_in_gain(s_codec, db);
}

/* ------------------------------------------------------------------ *
 * ES8311 hardware ALC  —  CAPTURE PATH ONLY
 *
 * 🔑 WHY. The builder: "a kid mite wisper up close or yell from accros the room". That
 * is 30-40dB of range from one talker and no fixed mic gain serves both ends of
 * it. Turning PLAYBACK up to hear a whisper is what makes music distort, so the
 * fix belongs on the ADC and nowhere near the volume slider.
 *
 * 🚨 THESE VALUES ARE DERIVED HERE, AND THE RESEARCH'S OWN SUGGESTED BYTES WERE
 * ALL WRONG. The 2026-08-17 brief (RESEARCH-ASK.md) returned the bit layout
 * from the Linux ASoC es8311 driver - that part corroborates es8311_reg.h and
 * is trusted - but every byte it recommended contradicted its own field table:
 *   0x18 = 0x9B was described as "automute on, winsize 2048" and actually
 *          encodes automute OFF, winsize 4096, and a 1 in a bit that the same
 *          answer lists as reserved;
 *   0x19 = 0x8A put MAXLEVEL (-11.0dBFS) BELOW MINLEVEL (-9.3dBFS), inverted,
 *          and called the 3.2dB between them "about 32 dB" - the level tables
 *          are ALSA TLV, i.e. 0.01dB units, misread as whole dB throughout;
 *   0x1B = 0x00 would have wiped the 0x0A that es8311.c:566 writes on EVERY
 *          codec open (ADC HPF stage 1, per Espressif's own register comment).
 * So only 0x18 and 0x19 are written here. 0x1A/0x1B/0x1C are left exactly as
 * the driver set them.
 *
 * ⚠️ ITS TIMING MATH WAS FOR 48kHz AND CAPTURE RUNS AT MIC_RATE = 16000, so
 * every window is 3x longer than the brief assumed. Its 4096-LRCK window is
 * 256ms per 0.25dB step at 16kHz - roughly 30 SECONDS to travel 30dB, i.e. the
 * servo could never catch a child going from a whisper to a shout inside one
 * sentence. That is the difference between ALC working and ALC being decorative.
 *
 *   0x18 = 0x87  ALC on (b7), automute OFF (b6), winsize 7 = 256 LRCK = 16ms at
 *                16kHz, so ~1.9s to traverse 30dB. Automute stays off on
 *                purpose: a gate that trips between words eats the start of the
 *                next one, and a whisper is exactly what it would gate.
 *   0x19 = 0xC8  MAXLEVEL 12 (-7.8dBFS), MINLEVEL 8 (-11.0dBFS). Max >= min,
 *                and both land where the STT wants speech to sit.
 *
 * ⚠️ A RAISED NOISE FLOOR ON QUIET SPEECH IS NOT A FAULT - it is what ALC does:
 * the whisper is brought up and the room hiss comes up with it. Pumping is the
 * other failure mode and it cannot touch music, because this is capture only.
 *
 * ⚠️ STILL UNCONFIRMED: which gain stage the servo actually drives. 0x14 is
 * named "analog pga gain" in es8311_reg.h and open() pins it at 0x1A, while
 * esp_codec_dev's set_in_gain writes 0x16. If it is 0x16, then MIC_GAIN_DB is
 * the servo's ceiling and the two interact - which is what the register dump
 * below is for. Read 0x16 before and after a loud capture: if ALC is working on
 * it, the value MOVES.
 * ------------------------------------------------------------------ */
#define ES8311_REG_ALC       0x18
#define ES8311_REG_ALCLEVEL  0x19
#define ALC_ON_BYTE          0x87
#define ALC_LEVEL_BYTE       0xC8

/* Re-applied from codec_open() because a rate change reopens the codec and
 * mic.c changes the rate at the START OF EVERY CAPTURE. open() does not touch
 * 0x18/0x19 today, but it DOES rewrite 0x16 through set_in_gain - the same
 * class of trap that already bites the DAC gain in audio_set_volume(), where
 * esp_codec_dev silently overwrites tuning that was applied earlier. */
static void alc_apply(void)
{
    /* 🚨 THE CHIP'S OWN ALC IS LEFT OFF, AND THAT IS A MEASURED RESULT, NOT A
     * PREFERENCE. See the negative result written up below: 0x18 accepted 0x87
     * and read it back every time, and the capture level never compressed. */
    codec_reg_write(ES8311_REG_ALC, 0x00);
    /* Auto-level runs on LESS analog gain, not more - the loud end has to stop
     * clipping before the quiet end can be raised. */
    if (s_codec)
        esp_codec_dev_set_in_gain(s_codec, s_alc ? MIC_GAIN_AGC_DB : s_mic_gain);
    audio_agc_reset();
}

void audio_set_alc(bool on)
{
    s_alc = on;
    s_alc_level = ALC_LEVEL_BYTE;
    alc_apply();
    applog("[alc] %s (%.0fdB analog)", on ? "ON" : "off",
           on ? MIC_GAIN_AGC_DB : s_mic_gain);
}

void audio_agc_reset(void) { s_agc_gain = 1.0f; s_agc_primed = false; }
float audio_agc_gain(void) { return s_agc_gain; }

/* Runs on every captured block, inside audio_read(), so BOTH consumers get it:
 * the walkie (wt.c) and the Talk capture (mic.c). One implementation, and the
 * peak each of them already logs measures the OUTPUT of this, which is what
 * makes it testable at all.
 *
 * Attack is instant and release is slow, which is the only arrangement that
 * does not clip: a sudden shout must be pulled down within the same 20ms block,
 * while the climb back up after it has to be slow enough that room hiss is not
 * swelled between words. AGC_FLOOR is the other half of that - below it the
 * block is treated as silence and the gain is left alone, so a quiet room does
 * not get amplified into a roar waiting for someone to speak. */
#define AGC_TARGET   (0.40f * 32767.0f)  /* aim speech peaks at ~40% FS      */
#define AGC_MAX      8.0f                /* +18dB of lift, no more           */
#define AGC_MIN      0.25f               /* -12dB for a shout                */
#define AGC_FLOOR    250                 /* ~0.8% FS: below this, say nothing */
#define AGC_RELEASE  0.02f               /* per block, ~20ms -> ~1s to climb  */

static void agc_process(int16_t *s16, size_t samples)
{
    int peak = 0;
    for (size_t i = 0; i < samples; i++) {
        int a = s16[i] < 0 ? -s16[i] : s16[i];
        if (a > peak) peak = a;
    }
    if (peak > AGC_FLOOR) {
        float want = AGC_TARGET / (float)peak;
        if (want > AGC_MAX) want = AGC_MAX;
        if (want < AGC_MIN) want = AGC_MIN;
        if (s_agc_primed == false) {
            /* 🚨 PRIME ON THE FIRST BLOCK OF SPEECH, DO NOT CRAWL UP TO IT.
             *
             * Measured 2026-08-18: with only the slow release, a whisper came
             * out at 5% FS instead of 40 — a 2s press is ~30 blocks and 2% per
             * block never arrives, so most of the utterance went out at close
             * to unity gain. A press is a fresh start with a known talker, so
             * the first block that is not silence sets the level outright and
             * the smoothing only handles what changes DURING the press. */
            s_agc_gain = want;
            s_agc_primed = true;
        } else if (want < s_agc_gain) {
            s_agc_gain = want;                                          /* attack */
        } else {
            s_agc_gain += (want - s_agc_gain) * AGC_RELEASE;            /* release */
        }
    }
    for (size_t i = 0; i < samples; i++) {
        int v = (int)(s16[i] * s_agc_gain);
        if (v >  32767) v =  32767;
        if (v < -32768) v = -32768;
        s16[i] = (int16_t)v;
    }
}

/* 🔍 DIAGNOSTIC, NOT A SETTING - deliberately not persisted, so a reboot clears
 * it. The builder's first A/B was "i didnt here any incress in the noice floor", on or
 * off. A working ALC on a whisper MUST raise the noise floor, so hearing no
 * difference means either the servo is not engaging or it is driving a stage
 * that is already where it wants to be. This forces the target to the loudest
 * the level table has (MAXLEVEL = MINLEVEL = 15, -6.0dBFS): if THAT produces no
 * change in the logged peak either, the servo is not running at all and the
 * next suspect is the analog PGA at 0x14, which no code here has ever moved. */
void audio_alc_probe(void)
{
    s_alc = true;
    s_alc_level = 0xFF;
    alc_apply();
    applog("[alc] PROBE, target 0x%02X", s_alc_level);
    audio_dump_codec_regs("probe ");
}

bool audio_alc(void) { return s_alc; }

/* Read-only. The power-on defaults of the ALC block are in no datasheet the
 * research could reach - every field came back NOT FOUND - so the honest way to
 * learn them is to ask THIS board, after the driver has finished with it. */
/* The playback half of the register story: DAC power (0x0D/0x12/0x13), DAC mute
 * (0x31) and volume (0x32). Added 2026-08-18 because walkie audio stops after
 * 5-7s while the frame counters, the amp flag, the DAC volume and every I2S
 * write status all stay healthy - so the next place the sound can be going is
 * inside the codec itself. */
void audio_dac_state(char *out, size_t n)
{
    snprintf(out, n, "0D%02X 12%02X 13%02X 31%02X 32%02X",
             codec_reg_read(0x0D) & 0xFF, codec_reg_read(0x12) & 0xFF,
             codec_reg_read(0x13) & 0xFF, codec_reg_read(0x31) & 0xFF,
             codec_reg_read(0x32) & 0xFF);
}

void audio_dump_codec_regs(const char *when)
{
    static const uint8_t regs[] = { 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C };
    char line[80];
    int n = 0;
    for (int i = 0; i < (int)(sizeof(regs) / sizeof(regs[0])); i++) {
        int v = codec_reg_read(regs[i]);
        if (v < 0) n += snprintf(line + n, sizeof(line) - n, "%02X=?? ", regs[i]);
        else       n += snprintf(line + n, sizeof(line) - n, "%02X=%02X ", regs[i], v);
    }
    ESP_LOGI(TAG, "codec regs %s: %s", when ? when : "", line);
    applog("[reg] %s%s", when ? when : "", line);
}

/* ------------------------------------------------------------------ *
 * Small-speaker EQ  —  internal speaker ONLY
 *
 * 🔑 WHY: the on-board driver is a tiny low-power speaker. It cannot reproduce
 * bass at all, so low-frequency energy does nothing except eat amplifier
 * headroom and push the driver into distortion. Removing it lets everything
 * else go LOUDER before it breaks up - so this is a loudness fix as much as a
 * tone fix. Lifting presence is separately what makes speech intelligible,
 * which is the point for Comms replies and the walkie-talkie.
 *
 * The builder asked for exactly this, 2026-08-16: "lower the bass and up the mid and
 * maybe tame the highs a bit, that would help voice come through better and
 * make it sound louder".
 *
 * 🚨 SPEAKER PATH ONLY, NEVER THE JACK. The PCM5102A line-out feeds headphones
 * and shared listening, which CAN do bass - EQ'ing that would be destroying
 * signal for no reason. This is compensation for one specific driver, not a
 * taste setting.
 *
 * Three biquads at 44.1kHz stereo is ~2.6M ops/sec against a 240MHz core with
 * an FPU: immaterial next to the MP3 decode already running.
 * ------------------------------------------------------------------ */

typedef struct { float b0, b1, b2, a1, a2; float z1[2], z2[2]; } biquad_t;

static biquad_t s_hp, s_mid, s_hi;
static bool     s_eq_on = true;

static void bq_lowshelf_reset(biquad_t *f) { f->z1[0]=f->z1[1]=f->z2[0]=f->z2[1]=0; }

/* RBJ cookbook forms. Kept explicit rather than pulled from a DSP library so
 * the coefficients can be read and argued with here. */
static void bq_highpass(biquad_t *f, float fs, float fc, float q)
{
    float w = 2.0f * (float)M_PI * fc / fs, cw = cosf(w), sw = sinf(w);
    float al = sw / (2.0f * q), a0 = 1.0f + al;
    f->b0 = (1.0f + cw) / 2.0f / a0;
    f->b1 = -(1.0f + cw) / a0;
    f->b2 = (1.0f + cw) / 2.0f / a0;
    f->a1 = (-2.0f * cw) / a0;
    f->a2 = (1.0f - al) / a0;
    bq_lowshelf_reset(f);
}

static void bq_peak(biquad_t *f, float fs, float fc, float q, float db)
{
    float A = powf(10.0f, db / 40.0f);
    float w = 2.0f * (float)M_PI * fc / fs, cw = cosf(w), sw = sinf(w);
    float al = sw / (2.0f * q), a0 = 1.0f + al / A;
    f->b0 = (1.0f + al * A) / a0;
    f->b1 = (-2.0f * cw) / a0;
    f->b2 = (1.0f - al * A) / a0;
    f->a1 = (-2.0f * cw) / a0;
    f->a2 = (1.0f - al / A) / a0;
    bq_lowshelf_reset(f);
}

static void bq_highshelf(biquad_t *f, float fs, float fc, float db)
{
    float A = powf(10.0f, db / 40.0f);
    float w = 2.0f * (float)M_PI * fc / fs, cw = cosf(w), sw = sinf(w);
    float al = sw / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / 0.707f - 1.0f) + 2.0f);
    float ap = A + 1.0f, am = A - 1.0f, tsa = 2.0f * sqrtf(A) * al;
    float a0 = ap - am * cw + tsa;
    f->b0 = A * (ap + am * cw + tsa) / a0;
    f->b1 = -2.0f * A * (am + ap * cw) / a0;
    f->b2 = A * (ap + am * cw - tsa) / a0;
    f->a1 = 2.0f * (am - ap * cw) / a0;
    f->a2 = (ap - am * cw - tsa) / a0;
    bq_lowshelf_reset(f);
}

/* Recomputed on every rate change - coefficients are sample-rate dependent, so
 * a stream at 22050 with 44100 coefficients would put the corners in the wrong
 * place. */
static void eq_configure(int rate)
{
    float fs = (float)(rate ? rate : 44100);
    /* 300Hz HPF: below this the driver produces excursion, not sound. */
    bq_highpass (&s_hp,  fs, 300.0f, 0.707f);
    /* +4dB around 2kHz: consonants live here, and it is where speech
     * intelligibility is won or lost. */
    bq_peak     (&s_mid, fs, 2000.0f, 1.0f, 4.0f);
    /* -3dB above 6kHz: takes the edge off without dulling it. */
    bq_highshelf(&s_hi,  fs, 6000.0f, -3.0f);
}

static inline float bq_run(biquad_t *f, float x, int ch)
{
    float y = f->b0 * x + f->z1[ch];
    f->z1[ch] = f->b1 * x - f->a1 * y + f->z2[ch];
    f->z2[ch] = f->b2 * x - f->a2 * y;
    return y;
}

static void eq_apply(int16_t *pcm, size_t samples)
{
    for (size_t i = 0; i < samples; i++) {
        int ch = i & 1;                       /* interleaved L,R */
        float x = pcm[i];
        x = bq_run(&s_hp,  x, ch);
        x = bq_run(&s_mid, x, ch);
        x = bq_run(&s_hi,  x, ch);
        /* Saturate: the mid lift can push peaks past full scale, and wrapping
         * would turn that into a loud click instead of gentle clipping. */
        if (x >  32767.0f) x =  32767.0f;
        if (x < -32768.0f) x = -32768.0f;
        pcm[i] = (int16_t)x;
    }
}

void audio_set_eq(bool on) { s_eq_on = on; }
bool audio_eq(void)        { return s_eq_on; }

/* 🚨 THE JACK HAS NO VOLUME CONTROL IN HARDWARE, AND NEVER HAS.
 *
 * The builder, 2026-08-25: "on the volume we had it working on bt but i hear no change
 * on jack or speaker". The jack half of that is structural, not a regression:
 * the 3.5mm output is a PCM5102A on its OWN I2S channel (s_dac). That part has
 * no I2C, no registers and no gain pin worth using — it converts whatever
 * samples it is handed at a fixed level. audio_set_volume() writes the ES8311's
 * DAC register over I2C, and the ES8311 is not in the jack's path AT ALL, so
 * the slider was moving a control on a chip nobody was listening to.
 *
 * The only place a volume can exist for that part is in the samples, so that is
 * where it goes. Squared rather than linear because loudness is not: half way
 * up the slider should sound like half as loud, which is about -12dB, not -6.
 * 100% skips the multiply entirely so the loudest setting is bit-perfect.
 *
 * ⚠️ In place, like eq_apply() above, and for the same reason — every caller
 * hands over its own decode scratch and does not reuse it after the write. */
static void soft_volume(int16_t *pcm, size_t n, int pct)
{
    if (pct >= 100) return;
    if (pct <= 0) { memset(pcm, 0, n * sizeof(int16_t)); return; }
    int32_t f = (pct * pct * 32768) / 10000;          /* q15 of (pct/100)^2 */
    for (size_t i = 0; i < n; i++)
        pcm[i] = (int16_t)(((int32_t)pcm[i] * f) >> 15);
}

esp_err_t audio_write(player_sink_t sink, const void *buf, size_t bytes)
{
    size_t w;
    /* ⚠️ Filtered IN PLACE, so the caller's buffer is modified. Every caller
     * passes its own decode scratch and does not reuse it after the write, so
     * this avoids a second buffer on a board with ~10KB of internal RAM free.
     * The const is cast away deliberately and only on the speaker path. */
    if (sink == SINK_SPEAKER && s_eq_on && bytes >= 2)
        eq_apply((int16_t *)buf, bytes / 2);
    if (sink == SINK_LINEOUT && bytes >= 2)
        soft_volume((int16_t *)buf, bytes / 2, s_volume);

    LOCK();
    i2s_chan_handle_t h = (sink == SINK_SPEAKER) ? s_spk : s_dac;
    /* ⚠️ NOT portMAX_DELAY. Holding the lock forever on a channel that is not
     * draining would block the rate change that would fix it. A write that
     * cannot land in 500ms has a real problem worth surfacing. */
    esp_err_t r = i2s_channel_write(h, buf, bytes, &w, pdMS_TO_TICKS(500));
    UNLOCK();
    return r;
}

esp_err_t audio_read(void *buf, size_t bytes, size_t *got, int timeout_ms)
{
    LOCK();
    esp_err_t r = i2s_channel_read(s_rx, buf, bytes, got, pdMS_TO_TICKS(timeout_ms));
    UNLOCK();
    /* 🚨 NOT `r == ESP_OK`. i2s_channel_read() returns ESP_ERR_TIMEOUT whenever
     * it could not fill the WHOLE buffer inside the timeout — and it still
     * hands back the samples it did get in *got. wt.c asks for a full buffer
     * with a 60ms timeout, so almost every read lands on that path. Gating the
     * AGC on ESP_OK meant the analog gain dropped 36->30dB and NOTHING put it
     * back: measured x1.0 on every capture, and the builder heard exactly that — "it
     * made the audio softer not more even". Trust `*got`, never the status. */
    if (s_alc && got && *got >= sizeof(int16_t))
        agc_process((int16_t *)buf, *got / sizeof(int16_t));
    return r;
}

void audio_flush_rx(void)
{
    uint8_t chunk[2048];
    size_t got;
    LOCK();
    /* Bounded: a broken RX that never returns empty must not hang the caller. */
    for (int i = 0; i < 64; i++) {
        if (i2s_channel_read(s_rx, chunk, sizeof(chunk), &got, pdMS_TO_TICKS(20))
            != ESP_OK || got == 0)
            break;
    }
    UNLOCK();
}

/* ---- earcons ----
 *
 * The builder's ask, 2026-08-14: "it would be good to have something audio wise when it
 * fails". He is right, and it matters more here than on a desktop — this is a
 * device a seven-year-old uses while looking at something else, and a silent
 * failure is indistinguishable from a broken device.
 *
 * Deliberately three distinguishable shapes rather than one generic bleep:
 * RISING = heard you and I'm working, FALLING = that didn't work, LOW BUZZ =
 * I couldn't hear anything. A child learns three tones far faster than they
 * read an error message.
 */
void audio_tone(int hz, int ms, int amplitude)
{
    const int rate = s_rate ? s_rate : 44100;
    const int frames = rate * ms / 1000;
    static int16_t buf[512 * 2];
    float phase = 0.0f, step = 2.0f * 3.14159265f * hz / rate;

    for (int done = 0; done < frames; ) {
        int n = frames - done;
        if (n > 512) n = 512;
        for (int i = 0; i < n; i++) {
            /* Ramp the first and last 5ms. A square-edged start on a small
             * speaker is a click that is louder than the tone. */
            int pos = done + i;
            float env = 1.0f;
            int ramp = rate * 5 / 1000;
            if (pos < ramp)                 env = (float)pos / ramp;
            else if (pos > frames - ramp)   env = (float)(frames - pos) / ramp;
            int16_t v = (int16_t)(amplitude * env * sinf(phase));
            buf[i * 2] = v; buf[i * 2 + 1] = v;
            phase += step;
        }
        audio_write(SINK_SPEAKER, buf, n * 2 * sizeof(int16_t));
        done += n;
    }
}

void audio_earcon(audio_cue_t cue)
{
    /* 🔑 RESTORE THE AMP, DO NOT JUST SWITCH IT OFF.
     * The builder, 2026-08-17: sending a message while music was playing worked, but
     * "it didnt go back to music after the send beep". The cue ended with an
     * unconditional audio_amp(false), which silenced the track that was still
     * playing underneath it. A cue is an interruption, not a stop. */
    bool was = audio_amp_on();
    audio_amp(true);
    switch (cue) {
    /* Amplitudes were 5000-6000 of 32767 and the builder reported "a very quiet beep".
     * These have to be audible from a pocket on a small speaker, and they are
     * short enough that loud is not annoying. */
    case CUE_START:  audio_tone(880, 70, 14000); break;
    case CUE_OK:     audio_tone(660, 60, 14000); audio_tone(990, 90, 14000); break;
    case CUE_FAIL:   audio_tone(660, 90, 16000); audio_tone(440, 160, 16000); break;
    case CUE_NOHEAR: audio_tone(300, 120, 16000); audio_tone(300, 120, 16000); break;
    /* 🔔 A DOORBELL: DING-DONG, HIGH THEN LOW.
     *
     * The builder, 2026-08-17, on the first version (three rising notes): "it was load
     * and clear but it shold be maybe a cherp cherp or an updown like bing
     * digng". Rising notes read as a status report; a falling pair reads as
     * someone at the door, which is exactly what an unprompted message is.
     *
     * 1245 -> 932 Hz is a descending fourth, the interval real doorbells use.
     * The second note is nearly twice as long so it rings out rather than
     * stopping dead, which is what makes it sound like a chime instead of two
     * beeps. audio_tone() ramps 5ms at each end, so the notes separate cleanly
     * without a gap between the writes. */
    /* 🔑 THE SILENCE BETWEEN THE NOTES IS WHAT MAKES IT TWO NOTES.
     *
     * The first version ran the two tones back to back and the builder heard it as one
     * sound: "can it be 2 tone insted of just one". Two pitches with no gap read
     * as a single sound that changes pitch — a swoop, not a chime. 90ms of
     * actual silence is what separates them into DING ... DONG.
     *
     * Written as a zero-amplitude tone so the gap goes through the same I2S
     * path with an exact length. A vTaskDelay() here would be approximate
     * instead: i2s_channel_write() returns once the data is QUEUED, so part of
     * the first note is still unplayed at that point and the gap would land in
     * the wrong place. */
    case CUE_MESSAGE: audio_tone(1245, 170, 15000);
                      audio_tone(1000,  90, 0);        /* silence */
                      audio_tone(932,  320, 15000); break;
    }
    /* i2s_channel_write() returns when the data is QUEUED, not when it is
     * heard — cutting the amp immediately swallows the cue entirely. */
    vTaskDelay(pdMS_TO_TICKS(250));
    audio_amp(was);
}
