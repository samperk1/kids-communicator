#pragma once
#include <stdbool.h>
#include "driver/i2c_master.h"

void display_init(void);
void display_backlight(bool on);

/* Backlight level, 1-100 percent, applied immediately if the screen is on.
 * Clamped to BRIGHT_MIN_PCT at the bottom — see the note in display.c about
 * why a brightness control that reaches zero is a brick. This does NOT
 * persist; settings_set_brightness() is what writes it to NVS. */
void display_set_brightness(int pct);
int  display_brightness(void);

/* Screen blanking. The backlight dominates idle draw, so this is the biggest
 * single battery win available in firmware. The touch driver calls
 * display_note_activity() itself; call it by hand for non-touch activity that
 * should keep the screen alive (a track change, a voice reply arriving). */
/* Raw controller reading, unmapped — for the calibration screen, which cannot
 * trust the mapping it exists to produce. */
bool display_touch_raw(int *rx, int *ry);

/* ---- panel + bus silence, for the GPS A/B --------------------------------
 *
 * 🚨 display_backlight() DARKENS AN LED AND NOTHING ELSE. The panel stays
 * powered and LVGL keeps flushing over SPI, so four rounds of "the display is
 * innocent" (+0.05, +0.47, -0.06, +0.53 dB) compared a full-screen redraw
 * against a partial one with the bus busy in BOTH halves. The bus itself has
 * never been switched off in a measurement.
 *
 * display_quiet(true) stops the SPI transactions and puts the panel to sleep —
 * the condition deep sleep produces, which is the only condition in which this
 * receiver has ever been observed to re-acquire. See ui.c's ab_apply().
 *
 * ⚠️ The screen is genuinely blank and unreadable while quiet. Touch still
 * works (separate controller), so the abort path still functions. */
void     display_quiet(bool quiet);
bool     display_quiet_get(void);
/* Flushes dropped while quiet. The A/B's proof that the condition took: zero
 * means the bus was never actually silenced and the leg is VOID. */
uint32_t display_flushes_blocked(void);

/* ---- did the touch timer actually get to run? ----------------------------
 *
 * 🔑 THE MEASUREMENT THAT DECIDES THE MAP REDRAW ARGUMENT. LVGL 8.4 dispatches
 * every button event from the indev timer inside lv_timer_handler()
 * (lv_indev.c:64 -> indev_pointer_proc:339 -> indev_proc_release:949), NOT from
 * our own 500ms UI tick. So a tap is lost only when a long callback stops
 * lv_timer_handler() from reaching the indev timer at all.
 *
 * This counter increments once per indev poll. main.c compares it either side
 * of lv_timer_handler(): a tick that ran long AND polled touch zero times is a
 * window in which a real tap would have been dropped. That is the falsifying
 * observation — if the Back button misses while this says the poll kept
 * running, the redraw is NOT the cause and I am wrong. */
uint32_t display_indev_polls(void);

void display_note_activity(void);
void display_screen_off(void);
bool display_screen_on(void);
int  display_idle_ms(void);

/* The ES8311 shares the touch controller's I2C bus at 0x18. */
i2c_master_bus_handle_t display_i2c_bus(void);
