#pragma once
#include <stdbool.h>
#include "player.h"

/* Everything the device should still know after a power cycle.
 *
 * Unit #1 taught this the hard way: the STT path was set over serial, never
 * persisted, and the first button press after a reflash 404'd. Anything a user
 * or a serial command can change belongs in here. */

void settings_load(void);

/* Call from the LVGL loop. Commits a changed value once it has been still for
 * a few seconds — see the note in settings.c about the volume slider. */
void settings_tick(void);
void settings_flush(void);      /* commit immediately */

int  settings_volume(void);
void settings_set_volume(int pct);

player_sink_t settings_sink(void);
void settings_set_sink(player_sink_t s);

/* Where playback was when the device was last switched off. album < 0 means
 * nothing to resume. */
int  settings_last_album(void);
int  settings_last_track(void);
void settings_set_last(int album, int track);

/* Seconds of no touch before the backlight goes out. 0 disables. */
/* Touch calibration, fitted on-device. hw.h's constants are only the fallback
 * for a board that has never been calibrated — X there was DERIVED, not
 * measured, and measurement showed both axes were wrong. */
typedef struct { float ax, bx, ay, by; int swap; int valid; } touch_cal_t;
const touch_cal_t *settings_touch_cal(void);
void settings_set_touch_cal(const touch_cal_t *c);
/* Back to the compiled defaults. NOT set_touch_cal() with an empty struct —
 * that one forces valid = 1. See the note in settings.c. */
void settings_clear_touch_cal(void);

/* No battery-present sense line exists on this board, so this is a statement of
 * fact by whoever wired it, not a detection. Runtime so a pack can be added or
 * removed without a reflash. */
bool settings_battery_fitted(void);
void settings_set_battery_fitted(bool on);

/* ES8311 DAC volume register, raw. Persisted: it is a property of this board's
 * speaker and amp, found by ear. 0xBF is 0dB, each count is 0.5dB. */
/* Auto-sleep after N minutes idle; 0 = never. Default 0 on purpose - a board
 * that sleeps unasked cannot be flashed (see power.h). */
/* Keep the BLE controller resident. OFF by default — it costs ~31KB of internal
 * RAM and, until the doorbell is built, buys nothing that WiFi does not already
 * do. Takes effect on the next boot. */
bool settings_ble(void);
void settings_set_ble(bool on);

/* ES8311 capture ALC. Off by default: it is a trade (a quiet talker arrives at
 * a usable level, the room noise under them comes up too), so it is the builder's ear
 * that decides, not this file. */
/* Whose handset this is - "Boy", "Girl", "Guest". Shown on the home screen and
 * used as the walkie id, so the far end says "Boy is talking" rather than
 * "comm-XXXX". Empty until set; callers fall back to the MAC. */
const char *settings_name(void);
void settings_set_name(const char *n);

/* Which radio the walkie-talkie uses.
 *
 * 🔑 AUTO IS THE ONLY ONE ANYBODY SHOULD NEED, AND IT PREFERS ESP-NOW.
 *
 * The first version preferred wifi, to keep the CYD (which speaks only the UDP
 * `WT1` format) in the conversation at home. It SPLIT THE FLEET: wifi
 * association is flaky and each handset decided for itself, so on 2026-08-26
 * Kid 1's association failed and it fell back to ESP-NOW while Kid 2's succeeded
 * and it stayed on UDP. Two boards on one shelf, on two different radios,
 * neither screen saying anything was wrong.
 *
 * 🔑 THE DECISION IS PER-HANDSET BUT THE CHANNEL IS SHARED, so the transport
 * must be something every handset independently reaches the same answer about.
 * ESP-NOW is the only one that qualifies: espnow_begin() holds the driver up
 * from boot and it cannot fail intermittently the way an association can.
 *
 * ⚠️ WT_RADIO_WIFI NO LONGER HAS A REAL USER. It was the CYD's escape hatch and
 * The builder retired the CYD on 2026-08-26. It stays as a fallback while the ESP-NOW
 * path is still new, and as a way to prove which radio a symptom belongs to.
 * Using it while others are on auto IS the split described above — the Talkie
 * screen calls that out rather than showing a bare "0 peers".
 *
 * ⚠️ THE FORCED MODES ARE ALSO A TEST HARNESS, not just a preference: without
 * them a transport would run for the first time in the field, which is the
 * mistake the away-mode passive scan already made once. */
typedef enum {
    WT_RADIO_AUTO   = 0,
    WT_RADIO_ESPNOW = 1,
    WT_RADIO_WIFI   = 2,
} wt_radio_t;

int  settings_wt_radio(void);
void settings_set_wt_radio(int r);

/* 🛰 LONG RANGE. Espressif's proprietary LR PHY, ~500 kbps, worth a large amount
 * of link budget through walls. Voice is 67 kbps so it fits with room over.
 *
 * 🚨 BOTH ENDS MUST BE SET TO LONG OR THEY ARE COMPLETELY DEAF TO EACH OTHER.
 * This is NOT like the 1 Mbps drop, which every 802.11 receiver can decode and
 * which therefore rolled out one handset at a time. LR is a different PHY: a
 * handset on `normal` cannot hear a handset on `long` AT ALL, in either
 * direction. Setting it on some of the fleet splits the fleet — the same
 * failure the Talkie transport had on 2026-08-26.
 *
 * ⏭ THE MANUAL SWITCH IS FOR TESTING WITH TWO HANDSETS IN HAND. The real
 * answer is to advertise LR capability in the 1s beacon and only transmit LR
 * once EVERY visible peer claims it — derived, not negotiated, so nobody has to
 * remember what has been flashed. Build that before this ships to the kids. */
int  settings_wt_range(void);        /* 0 = normal, 1 = long */
void settings_set_wt_range(int r);

/* Which channel ESP-NOW sits on. 1, 6 or 11 — the three non-overlapping ones.
 *
 * 🔑 DEFAULT 6, SO FLASHING THIS CHANGES NOTHING. The migration to a quieter
 * channel is a deliberate act on the menu, not a side effect of an update; a
 * handset that comes back from a reflash on a different channel from its
 * siblings is a handset that has gone silent for no reason the child can see.
 *
 * ⚠️ HANDSETS ON DIFFERENT CHANNELS ARE COMPLETELY DEAF TO EACH OTHER. The
 * recovery sweep in espnow.c is the safety net for that, not a licence to leave
 * the fleet mismatched — see the note over espnow_channel_sweep(). */
int  settings_espnow_channel(void);
void settings_set_espnow_channel(int ch);

bool settings_alc(void);
void settings_set_alc(bool on);

int  settings_sleep_minutes(void);
void settings_set_sleep_minutes(int m);
/* Alarm as 24h HHMM (730 = 07:30), or -1 for none. Wakes from deep sleep and
 * starts music - which needs no network, since the music is on the card. */
int  settings_alarm(void);
void settings_set_alarm(int hhmm);

int  settings_dac_gain(void);
void settings_set_dac_gain(int raw);

/* Album art strength behind Now Playing, 0-255 (LVGL opacity). 0 hides it. */
int  settings_art_opa(void);
void settings_set_art_opa(int v);

/* Backlight level, BRIGHT_MIN_PCT..100. Per board and in NVS for the same
 * reason the speaker gain is: it is a judgement made by eye, in one particular
 * room, by whoever is holding it. The setter applies the level immediately and
 * lets the usual lazy write carry it to flash. */
int  settings_brightness(void);
void settings_set_brightness(int pct);

int  settings_screen_timeout(void);
void settings_set_screen_timeout(int secs);

/* Route the assistant through the gateway instead of Home Assistant directly.
 * Off by default until the relay is proven. */
bool settings_ask_via_gw(void);
void settings_set_ask_via_gw(bool on);

/* Which gateway to ask. 0 = automatic (prefer one that says it can reach HA,
 * then the strongest). Persisted so a deliberate choice survives a reboot. */
uint16_t settings_ask_gw_id(void);
void     settings_set_ask_gw_id(uint16_t id);

