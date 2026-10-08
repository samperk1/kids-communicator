/*
 * Battery gauge on GPIO9.
 *
 * The divider and attenuation come from the board's ESPHome config, which
 * already reads this pin on this board: ADC, attenuation auto, and a x2
 * multiplier because the divider is 1:1. So the volts here are real volts.
 */

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/usb_serial_jtag.h"
#include "hw.h"
#include "battery.h"

static const char *TAG = "batt";

/* GPIO9 is ADC1 channel 8 on the ESP32-S3. */
#define BATT_UNIT     ADC_UNIT_1
#define BATT_CHANNEL  ADC_CHANNEL_8
#define BATT_DIVIDER  2.0f

/* A LiPo is essentially flat between these, and the curve in between is not
 * linear — but a straight line is honest enough for "should I charge it?", and
 * pretending to more precision than an uncalibrated ADC has would be a lie. */
#define BATT_MIN_MV   3300
#define BATT_MAX_MV   4200

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok = false;

void battery_init(void)
{
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BATT_UNIT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));

    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,       /* full ~3.1V range, matching "auto" */
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BATT_CHANNEL, &ccfg));

    adc_cali_curve_fitting_config_t cal = {
        .unit_id = BATT_UNIT,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    s_cali_ok = (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) == ESP_OK);
    ESP_LOGI(TAG, "battery on GPIO9, calibration %s",
             s_cali_ok ? "available" : "UNAVAILABLE (readings approximate)");
}

/* 🚨 THE DISPLAYED PERCENTAGE NEEDS A FILTER, NOT JUST AN AVERAGE.
 *
 * BATT_MIN..BATT_MAX is a 900mV span, so ONE PERCENT IS NINE MILLIVOLTS. The
 * ADC is good to perhaps +/-20mV even after averaging 8 samples, which is +/-2%
 * — so the reading visibly flickers. The builder, 2026-08-16: "it says 8 to 10% left on
 * the screen, it's toggling back and forth".
 *
 * More sampling inside one call barely helps: the noise is not all
 * sample-to-sample, and the UI reads this twice a second. What works is a slow
 * exponential average ACROSS calls. A battery moves by ~27mV an hour (measured
 * on this hardware), so a filter with a ~10 second time constant is invisible
 * to the user and kills the flicker completely.
 *
 * ⚠️ Voltage also SAGS under load — the amp pulling on a track start dips it
 * well below the resting value. The same filter smooths that out, which is why
 * the low-battery warning should read the filtered value and never a raw one. */
#define BATT_EMA_NUM   1        /* new sample weight = 1/16 */
#define BATT_EMA_DEN   16

static int s_ema_mv = -1;

/* Unfiltered, for callers that genuinely want the instantaneous value —
 * battery_fitted() must not be answered from a filter that has not settled. */
int battery_millivolts_raw(void)
{
    int raw = 0, mv = 0;
    /* Average a few: the ADC is noisy and this is read once a minute anyway. */
    int acc = 0;
    for (int i = 0; i < 8; i++) {
        if (adc_oneshot_read(s_adc, BATT_CHANNEL, &raw) != ESP_OK) return -1;
        acc += raw;
    }
    raw = acc / 8;

    if (s_cali_ok) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return -1;
    } else {
        mv = raw * 3100 / 4095;         /* rough fallback */
    }
    return (int)(mv * BATT_DIVIDER);
}

int battery_millivolts(void)
{
    int mv = battery_millivolts_raw();
    if (mv < 0) return s_ema_mv > 0 ? s_ema_mv : -1;

    /* Seed on the first reading rather than ramping up from zero, or the icon
     * shows an empty battery for the first few seconds of every boot. */
    if (s_ema_mv < 0) s_ema_mv = mv;
    else s_ema_mv += (mv - s_ema_mv) * BATT_EMA_NUM / BATT_EMA_DEN;
    return s_ema_mv;
}

/* 🚨 THIS USED TO RETURN A COMPILE-TIME 0, AND THE UI ASKED A SETTING THAT
 * NOTHING EVER WROTE.
 *
 * `hw.h` has `#define BATTERY_FITTED 0` from before a pack existed, and the
 * status bar called settings_battery_fitted() — whose setter,
 * settings_set_battery_fitted(), is not called from ANY file in this project.
 * So the icon was pinned to LV_SYMBOL_USB forever and could not change no
 * matter what the hardware did. The builder, 2026-08-15: "the icon at the top that
 * shows the usb connect sign is not changing when its running on battery
 * power it never changes at all".
 *
 * Measure it instead of being told. A wired pack sits between roughly 3.0V
 * (empty) and 4.3V (charging); a floating ADC pin does not hold a steady
 * reading in that window. */
bool battery_fitted(void)
{
    /* Raw on purpose: this asks "is a pack physically there", which must not be
     * answered from a smoothing filter that has not settled yet. */
    int mv = battery_millivolts_raw();
    return mv > 2500 && mv < 4500;
}

int battery_percent(void)
{
    int mv = battery_millivolts();
    if (mv < 0) return -1;
    /* Log the real millivolts regardless — this is the measurement needed to
     * set sane MIN/MAX once a pack is actually on it. */
    static int64_t last_log;
    int64_t now = esp_timer_get_time();
    if (now - last_log > 30000000) {
        last_log = now;
        /* ⚠️ This used to append "(NO PACK FITTED - pin is floating)" based on
         * the compile-time BATTERY_FITTED, which is 0 and never changes - so it
         * printed that claim even with a pack wired and discharging. Ask the
         * measurement, not the constant. */
        ESP_LOGI(TAG, "pack reads %d mV%s", mv,
                 battery_fitted() ? "" : "  (no pack detected - pin may be floating)");
    }
    if (!battery_fitted()) return -1;
    int pct = (mv - BATT_MIN_MV) * 100 / (BATT_MAX_MV - BATT_MIN_MV);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    /* Hysteresis on top of the filter. Even a smoothed value sitting exactly on
     * a boundary will toggle across it, and a battery meter that flickers reads
     * as broken. Hold the last figure until it has genuinely moved 2 points -
     * a battery moves ~27mV/hour, i.e. about 3 points an hour, so this costs
     * nothing in responsiveness. */
    static int s_shown = -1;
    if (s_shown < 0 || pct <= s_shown - 2 || pct >= s_shown + 2) s_shown = pct;
    return s_shown;
}

/* 🔑 ASK THE USB PERIPHERAL, NOT THE ADC.
 *
 * The builder, 2026-08-16: the plan was a USB symbol when plugged in, "but it never went
 * back to battery so we switched it back". Here is why it could not work:
 * VOLTAGE CANNOT TELL THE TWO APART. On USB with no pack, GPIO9 floats at about
 * 4150mV - inside battery_fitted()'s 2500-4500 window and just under
 * battery_charging()'s 4260 threshold - so USB reads as a nearly-full battery,
 * and a real full pack reads the same. No threshold separates them.
 *
 * The S3 knows directly: usb_serial_jtag_is_connected() reports whether a USB
 * HOST is talking to the built-in peripheral.
 *
 * ⚠️ LIMITATION, AND IT IS REAL: this detects a HOST (start-of-frame packets),
 * not power. A dumb wall charger supplies 5V and no data, so it will read as
 * NOT connected. Correct on the bench and for a computer; a charger-only
 * connection still looks like battery. Do not describe this as "charger
 * detection". */
/* Low-battery threshold, from the measured discharge on this hardware
 * (2026-08-16): idle drain is ~27mV/h, so 3.45V gives roughly 30-60 minutes of
 * warning at rest and less under load.
 *
 * ⚠️ Reads the FILTERED voltage on purpose. The raw pin sags hard when the amp
 * pulls on a track start, so a warning built on it would fire mid-song on a
 * healthy pack. Also false while on USB, where the pin does not read the pack
 * at all. */
#define BATT_LOW_MV 3450

bool battery_low(void)
{
    if (battery_on_usb()) return false;
    int mv = battery_millivolts();
    return mv > 0 && mv < BATT_LOW_MV && battery_fitted();
}

bool battery_on_usb(void)
{
    return usb_serial_jtag_is_connected();
}

/* Above the pack's full voltage means something is pushing charge in. Crude,
 * but this board exposes no charger status line to read instead. */
bool battery_charging(void)
{
    int mv = battery_millivolts();
    return mv > (BATT_MAX_MV + 60);
}
