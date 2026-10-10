/*
 * NVS-backed settings.
 *
 * Writes are deliberately lazy: NVS is flash, and the volume slider fires a
 * VALUE_CHANGED for every pixel a finger travels. Committing each one would
 * burn the sector and stall the UI thread mid-drag. Instead a value is held in
 * RAM and flushed by settings_tick() once it has been still for a moment.
 */

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_timer.h"
#include "hw.h"
#include "settings.h"
#include "display.h"

static const char *TAG = "set";
static const char *NS  = "kccomm";   /* see fleet.c: kept apart from the family build */

#define FLUSH_AFTER_MS 3000

/* Bump this to discard every stored touch calibration once, on next boot.
 * 1 = the 2026-08-14 four-point fit, which made touch worse. */
#define CAL_GEN 4

typedef struct {
    int  volume;
    int  sink;
    int  last_album;
    int  last_track;
    int  screen_timeout;
    /* ES8311 DAC volume register, raw. Persisted because it is a property of
     * THIS board's speaker and amp, found by ear - not something to rediscover
     * on every flash. The builder measured 0xD1 (+9dB) as the loudest clean setting on
     * 2026-08-16; esp_codec_dev's own "100%" was leaving it at -3dB. */
    int  dac_gain;
    int  sleep_minutes;      /* 0 = never auto-sleep */
    int  ble_on;             /* 1 = keep BLE resident; see settings.h */
    /* 1 = send the microphone to the GATEWAY instead of straight to Home
     * Assistant. Off by default while the relay is being proven: the direct
     * path works today and a setting nobody turned on cannot break it. The
     * end state is the reverse — see VOICE-RELAY-PLAN.md phase D. */
    int  ask_via_gw;
    int  ask_gw_id;      /* 0 = automatic; else pin this gateway */
    int  alc;                /* 1 = ES8311 capture ALC on; see settings.h */
    char name[16];           /* whose handset this is; "" = fall back to MAC */
    int  alarm_hhmm;         /* -1 = no alarm, else 24h e.g. 730 = 07:30 */
    /* How strongly the album art shows through behind Now Playing, 0-255 as
     * LVGL's opacity. Per board and in NVS for the same reason the speaker gain
     * is: it is a judgement made by eye, on one particular screen, in one
     * particular room, and nobody should need a toolchain to change it. */
    int  art_opa;
    int  brightness;
    int  batt_fitted;
    /* Which radio the Talkie uses. See wt_radio_t in settings.h — 0 is auto and
     * is what everyone should be on; the forced modes exist so the ESP-NOW path
     * can be TESTED AT HOME rather than first discovered in a car park. */
    int  wt_radio;
    int  wt_range;           /* 0 = normal 1Mbps, 1 = LR. See settings.h */
    int  now_channel;        /* ESP-NOW channel; 0 means "never set" = 11 */
    int  calgen;
    touch_cal_t cal;
} cfg_t;

/* Defaults. `last_album = -1` means "nothing to resume". */
static cfg_t s_cfg = {
    .volume = 85, .sink = SINK_SPEAKER,   /* ESPHome runs this board at 85% */
    .last_album = -1, .last_track = 0,
    .screen_timeout = 45, .art_opa = 100, .brightness = 100,
    .dac_gain = 0xD1,          /* +9dB - measured by ear, see above */
    /* 🚨 OFF BY DEFAULT, AND THAT IS A DELIBERATE TRADE, NOT AN OVERSIGHT.
     *
     * The BLE controller holds ~31KB of internal, DMA-bound RAM that cannot go
     * to PSRAM, and on this board that is the difference between esp_wifi_init()
     * succeeding and failing. Measured 2026-08-17: with BLE resident, sends work
     * for a few minutes and then stop with "no network" — messaging, which is
     * finished and useful, broken by a feature that is not finished yet.
     *
     * Turn it on when the doorbell exists and BLE is earning its keep. Until
     * then a handset with BLE off is reachable over WiFi, which is the only
     * path that actually carries messages today. */
    .ble_on = 0,
    .ask_via_gw = 0,
    .ask_gw_id  = 0,
    .sleep_minutes = 0,        /* OFF by default: a board that sleeps unasked
                                * is a board that cannot be flashed */
    .alarm_hhmm = -1,
    .batt_fitted = BATTERY_FITTED,
    .calgen = CAL_GEN,
    .cal = { .valid = 0 },
};

static bool     s_dirty;
static int64_t  s_dirty_since;

static void mark_dirty(void)
{
    s_dirty = true;
    s_dirty_since = esp_timer_get_time();
}

static void get_i32(nvs_handle_t h, const char *k, int *out)
{
    int32_t v;
    if (nvs_get_i32(h, k, &v) == ESP_OK) *out = (int)v;
}

void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no saved settings, using defaults");
        return;
    }
    get_i32(h, "vol",   &s_cfg.volume);
    get_i32(h, "sink",  &s_cfg.sink);
    /* 🚨 A HANDSET LAST USED WITH THE DONGLE HAS sink = 2 SAVED.
     * SINK_BT was removed 2026-09-06 and 2 is no longer a valid player_sink_t.
     * Left alone it would reach audio_write() as an unhandled case — silence,
     * or worse, on a device a child is holding. Clamp it back to the speaker. */
    if (s_cfg.sink != SINK_SPEAKER && s_cfg.sink != SINK_LINEOUT) {
        ESP_LOGW(TAG, "stored sink %d is no longer valid (was BT), using speaker",
                 s_cfg.sink);
        s_cfg.sink = SINK_SPEAKER;
    }
    get_i32(h, "album", &s_cfg.last_album);
    get_i32(h, "track", &s_cfg.last_track);
    get_i32(h, "scrto", &s_cfg.screen_timeout);
    get_i32(h, "artopa", &s_cfg.art_opa);
    get_i32(h, "bright", &s_cfg.brightness);
    get_i32(h, "dacg",  &s_cfg.dac_gain);
    get_i32(h, "slpm",  &s_cfg.sleep_minutes);
    get_i32(h, "bleon", &s_cfg.ble_on);
    get_i32(h, "askgw", &s_cfg.ask_via_gw);
    get_i32(h, "askgwid", &s_cfg.ask_gw_id);
    get_i32(h, "alc",   &s_cfg.alc);
    size_t nlen = sizeof(s_cfg.name);
    if (nvs_get_str(h, "name", s_cfg.name, &nlen) != ESP_OK) s_cfg.name[0] = 0;
    get_i32(h, "alrm",  &s_cfg.alarm_hhmm);
    get_i32(h, "batt",  &s_cfg.batt_fitted);
    get_i32(h, "nowch", &s_cfg.now_channel);   /* see settings_flush() */
    get_i32(h, "wtrad", &s_cfg.wt_radio);
    get_i32(h, "wtrng", &s_cfg.wt_range);
    size_t sz = sizeof(s_cfg.cal);
    if (nvs_get_blob(h, "cal", &s_cfg.cal, &sz) != ESP_OK) s_cfg.cal.valid = 0;

    /* 🚨 THROW AWAY A CALIBRATION FROM AN OLDER GENERATION.
     *
     * The 4-point calibration added 2026-08-14 made touch WORSE, not better:
     * The builder had to press to the right of every control, with the error largest at
     * the left of the screen and shrinking towards the right — a gain error, not
     * an offset. Touch had been fine on hw.h's constants beforehand, so the
     * calibration was a solution to a problem that did not exist, and its result
     * is now sitting in NVS making the device unusable.
     *
     * Bumping CAL_GEN invalidates any stored fit exactly once, on the first boot
     * of the new firmware. A deliberate recalibration still persists after that.
     * Do NOT clear unconditionally — that would make calibrating impossible
     * rather than optional.
     *
     * ⚠️ The line above about "falls back to the hw.h constants" is only half
     * true now: the MAPPING does fall back to hw.h, but ui_init() sees
     * !valid and opens the calibration screen at boot. So a bump means every
     * handset asks to be recalibrated once. That is deliberate.
     *
     * GEN 3, 2026-09-05: Kid 2's stored fit was wrong and there is no way to
     * clear one handset's calibration remotely — this is the only mechanism.
     * It also re-measures TOUCH_AX/BX on every board, which hw.h has always
     * warned were DERIVED ALGEBRAICALLY AND NEVER MEASURED, and which are the
     * leading suspect for the checkers board being hard to hit at the sides.
     *
     * 🔑 GEN 4, 2026-09-07: THE SUSPECT ABOVE WAS GUILTY, AND IS NOW FIXED.
     * TOUCH_AX was measured on Parent and Kid 4 and was 14% too steep;
     * hw.h now carries the real numbers and the whole derivation story. So the
     * compiled-in fallback is finally CORRECT, and a stored fit is no longer
     * automatically the better of the two — Parent's stored ax was -1.2157, which
     * is further from the measured -1.01 than the old broken default was.
     *
     * Every fit in the field was made by a child tapping crosshairs on a
     * mapping we now know was wrong. Dropping them all puts all five handsets
     * on the measured constants, which beat a sloppy fit and cost nobody a
     * calibration session. Anyone who wants better can still recalibrate, and
     * that fit will now be anchored to a correct starting point. */
    int gen = 0;
    get_i32(h, "calgen", &gen);
    if (gen != CAL_GEN) {
        s_cfg.cal.valid = 0;
        s_cfg.calgen    = CAL_GEN;
        s_dirty         = true;
        ESP_LOGW(TAG, "dropped touch calibration from generation %d", (int)gen);
    }
    nvs_close(h);

    ESP_LOGI(TAG, "loaded: vol=%d sink=%d resume=%d/%d timeout=%ds channel=%d",
             s_cfg.volume, s_cfg.sink, s_cfg.last_album, s_cfg.last_track,
             s_cfg.screen_timeout, settings_espnow_channel());
}

void settings_flush(void)
{
    if (!s_dirty) return;
    s_dirty = false;

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, settings not saved");
        return;
    }
    nvs_set_i32(h, "vol",   s_cfg.volume);
    nvs_set_i32(h, "sink",  s_cfg.sink);
    nvs_set_i32(h, "album", s_cfg.last_album);
    nvs_set_i32(h, "track", s_cfg.last_track);
    nvs_set_i32(h, "scrto", s_cfg.screen_timeout);
    nvs_set_i32(h, "artopa", s_cfg.art_opa);
    nvs_set_i32(h, "bright", s_cfg.brightness);
    nvs_set_i32(h, "dacg",  s_cfg.dac_gain);
    nvs_set_i32(h, "slpm",  s_cfg.sleep_minutes);
    nvs_set_i32(h, "bleon", s_cfg.ble_on);
    nvs_set_i32(h, "askgw", s_cfg.ask_via_gw);
    nvs_set_i32(h, "askgwid", s_cfg.ask_gw_id);
    nvs_set_i32(h, "alc",   s_cfg.alc);
    nvs_set_str(h, "name",  s_cfg.name);
    nvs_set_i32(h, "alrm",  s_cfg.alarm_hhmm);
    nvs_set_i32(h, "batt",  s_cfg.batt_fitted);
    nvs_set_i32(h, "calgen", s_cfg.calgen);
    /* 🚨 THESE THREE WERE NEVER SAVED (found 2026-10-08). Each setter
     * marked the config dirty and this function wrote everything EXCEPT them, so
     * a channel picked on the menu worked until the next reboot or deep sleep and
     * then fell back to 6 - every handset quietly on 6 while the family had
     * moved to 11, deaf to each other. The builder: "it resets to ch 6 when the hand set
     * goes to sleep". Talkie radio and Talkie range had the same hole. A new
     * field needs a line HERE and in settings_load(), or it is RAM only. */
    nvs_set_i32(h, "nowch", s_cfg.now_channel);
    nvs_set_i32(h, "wtrad", s_cfg.wt_radio);
    nvs_set_i32(h, "wtrng", s_cfg.wt_range);
    nvs_set_blob(h, "cal", &s_cfg.cal, sizeof(s_cfg.cal));
    nvs_commit(h);
    nvs_close(h);
}

/* Called from the LVGL loop. */
void settings_tick(void)
{
    if (!s_dirty) return;
    if (esp_timer_get_time() - s_dirty_since < FLUSH_AFTER_MS * 1000LL) return;
    settings_flush();
}

int  settings_volume(void) { return s_cfg.volume; }
void settings_set_volume(int pct)
{
    if (pct == s_cfg.volume) return;
    s_cfg.volume = pct;
    mark_dirty();
}

player_sink_t settings_sink(void) { return (player_sink_t)s_cfg.sink; }
void settings_set_sink(player_sink_t s)
{
    if ((int)s == s_cfg.sink) return;
    s_cfg.sink = (int)s;
    mark_dirty();
}

int settings_last_album(void) { return s_cfg.last_album; }
int settings_last_track(void) { return s_cfg.last_track; }
void settings_set_last(int album, int track)
{
    if (album == s_cfg.last_album && track == s_cfg.last_track) return;
    s_cfg.last_album = album;
    s_cfg.last_track = track;
    mark_dirty();
}

int settings_sleep_minutes(void) { return s_cfg.sleep_minutes; }
bool settings_ble(void)          { return s_cfg.ble_on != 0; }
void settings_set_ble(bool on)   { s_cfg.ble_on = on ? 1 : 0; mark_dirty(); }
/* 🔑 There is no other route in this build. net.c, ha.c and the token are not
 * compiled, so "ask via the handset" is not a choice the user can meaningfully
 * make — offering it as a toggle just produces a button that never works.
 * Forced on, and it ignores whatever a previous full build left in NVS. */
bool settings_ask_via_gw(void)        { return true; }
void settings_set_ask_via_gw(bool on) { s_cfg.ask_via_gw = on ? 1 : 0; mark_dirty(); }
uint16_t settings_ask_gw_id(void)          { return (uint16_t)s_cfg.ask_gw_id; }
void settings_set_ask_gw_id(uint16_t id)   { s_cfg.ask_gw_id = id; mark_dirty(); }
/* Empty means "not named yet" and every caller falls back to the MAC-derived
 * id, so an unnamed board is still usable and still unique. */
const char *settings_name(void)  { return s_cfg.name; }
void settings_set_name(const char *n)
{
    strlcpy(s_cfg.name, n ? n : "", sizeof(s_cfg.name));
    mark_dirty();
}
/* 0 in NVS means "never set". A new handset starts on 11, NOT 6: 6 is the
 * channel most home WiFi sits on, and ESP-NOW on a crowded channel loses
 * frames. Filmed 2026-10-09: two new handsets side by side on 6 each said
 * "nobody in range", and both worked the moment they moved to 11. */
int  settings_espnow_channel(void)
{
    int ch = s_cfg.now_channel;
    return (ch == 1 || ch == 6 || ch == 11) ? ch : 11;
}
void settings_set_espnow_channel(int ch)
{
    if (ch != 1 && ch != 6 && ch != 11) return;
    if (ch == s_cfg.now_channel) return;
    s_cfg.now_channel = ch;
    mark_dirty();
}
int  settings_wt_range(void)     { return s_cfg.wt_range; }
void settings_set_wt_range(int r)
{
    if (r < 0 || r > 1 || r == s_cfg.wt_range) return;
    s_cfg.wt_range = r;
    mark_dirty();
}
int  settings_wt_radio(void)     { return s_cfg.wt_radio; }
void settings_set_wt_radio(int r)
{
    if (r < 0 || r > 2 || r == s_cfg.wt_radio) return;
    s_cfg.wt_radio = r;
    mark_dirty();
}
bool settings_alc(void)          { return s_cfg.alc != 0; }
void settings_set_alc(bool on)   { s_cfg.alc = on ? 1 : 0; mark_dirty(); }
void settings_set_sleep_minutes(int m)
{
    if (m == s_cfg.sleep_minutes) return;
    s_cfg.sleep_minutes = m;
    mark_dirty();
}

int settings_alarm(void) { return s_cfg.alarm_hhmm; }
void settings_set_alarm(int hhmm)
{
    if (hhmm == s_cfg.alarm_hhmm) return;
    s_cfg.alarm_hhmm = hhmm;
    mark_dirty();
}

int settings_dac_gain(void) { return s_cfg.dac_gain; }
void settings_set_dac_gain(int raw)
{
    if (raw == s_cfg.dac_gain) return;
    s_cfg.dac_gain = raw;
    mark_dirty();
}

int settings_screen_timeout(void) { return s_cfg.screen_timeout; }

int  settings_art_opa(void) { return s_cfg.art_opa; }
void settings_set_art_opa(int v)
{
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    if (v == s_cfg.art_opa) return;
    s_cfg.art_opa = v;
    mark_dirty();
}
int  settings_brightness(void) { return s_cfg.brightness; }
void settings_set_brightness(int pct)
{
    if (pct < BRIGHT_MIN_PCT) pct = BRIGHT_MIN_PCT;
    if (pct > 100)            pct = 100;
    /* Light it first, store it second. mark_dirty() only queues the NVS write
     * for a few seconds' time (see the volume-slider note above), so applying
     * the level here is what makes the row feel live rather than laggy. */
    display_set_brightness(pct);
    if (pct == s_cfg.brightness) return;
    s_cfg.brightness = pct;
    mark_dirty();
}

void settings_set_screen_timeout(int secs)
{
    if (secs == s_cfg.screen_timeout) return;
    s_cfg.screen_timeout = secs;
    mark_dirty();
}

const touch_cal_t *settings_touch_cal(void) { return &s_cfg.cal; }
void settings_set_touch_cal(const touch_cal_t *c)
{
    s_cfg.cal = *c;
    s_cfg.cal.valid = 1;
    mark_dirty();
    settings_flush();          /* a calibration must survive a crash */
}

/* 🚨 THERE WAS NO WAY TO CLEAR IT, AND THE OBVIOUS ONE IS A TRAP.
 * settings_set_touch_cal() FORCES valid = 1, so passing an all-zero struct
 * stores a calibration that is marked good and maps every touch to (0,0) —
 * strictly worse than the bad calibration it was meant to rescue. Clearing is
 * its own operation. valid = 0 sends display.c back to the compiled defaults. */
void settings_clear_touch_cal(void)
{
    memset(&s_cfg.cal, 0, sizeof(s_cfg.cal));
    mark_dirty();
    settings_flush();
}

bool settings_battery_fitted(void) { return s_cfg.batt_fitted != 0; }
void settings_set_battery_fitted(bool on)
{
    if ((int)on == s_cfg.batt_fitted) return;
    s_cfg.batt_fitted = on;
    mark_dirty();
}
