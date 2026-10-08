/*
 * Board facts for the ES3C28P (Hosyond/LCDwiki 2.8" ESP32-S3).
 *
 * Every value here was either taken from the board's ESPHome config — a
 * working ESPHome config for this same board — or measured on the bench. None
 * of it is guessed.
 */
#pragma once

#include "driver/gpio.h"

/* ---- display: ILI9341 over SPI ---- */
#define PIN_SCLK        GPIO_NUM_12
#define PIN_MOSI        GPIO_NUM_11
#define PIN_MISO        GPIO_NUM_13
#define PIN_LCD_CS      GPIO_NUM_10
#define PIN_LCD_DC      GPIO_NUM_46
#define PIN_LCD_BL      GPIO_NUM_45

/* Darkest the brightness control may go. Not 0, and not 1: at very low duty
 * this panel's backlight is effectively black in a lit room, and a handset that
 * looks dead is a handset that gets reported as broken. 10% measured as "dim
 * but plainly lit" in a dark room, which is the case the control exists for. */
#define BRIGHT_MIN_PCT  10

/* PORTRAIT. The screen lives in the LID of a flip-open communicator and is used
 * tall, mic at the bottom.
 *
 * 🚨 THE ILI9341 ROTATION TABLE, because I got this wrong once and it cost a
 * flash cycle. The four legal values are:
 *
 *     rot   0  portrait           0x48   (MX | BGR)
 *     rot  90  landscape          0x28   (MV | BGR)
 *     rot 180  portrait flipped   0x88   (MY | BGR)
 *     rot 270  landscape flipped  0xE8   (MX | MY | MV | BGR)
 *
 * ANY other combination MIRRORS rather than rotates. 0x08 and 0xC8 look
 * plausible and are both mirrored — text comes out backwards. That is not
 * detectable with colour bars or crosshairs, which is exactly how it survived
 * the display-test gate: that rig never drew a single glyph. **Put text on the
 * screen before trusting an orientation.**
 *
 * 0x88 is the one that keeps the mic at the bottom without mirroring. */
#define LCD_W           240
#define LCD_H           320
#define LCD_MADCTL      0x88
#define LCD_SPI_HZ      (40 * 1000 * 1000)

/* ---- touch: FT6236 on I2C ---- */
#define PIN_I2C_SDA     GPIO_NUM_16
#define PIN_I2C_SCL     GPIO_NUM_15
#define PIN_TOUCH_INT   GPIO_NUM_17
#define FT6236_ADDR     0x38

/* ✅ MEASURED ON TWO BOARDS 2026-09-07. TOUCH_SWAP_XY tracks MADCTL's MV bit:
 * in portrait (MV clear) the controller's axes align with the screen's, so no
 * swap. A fitted slope far from +/-1 means this is wrong, not the presses.
 *
 * 🚨 THE OLD X CONSTANTS WERE NEVER MEASURED, AND THEY WERE 14% WRONG.
 *
 * They read -1.1688 / 248.74, derived algebraically when MADCTL went 0xC8 ->
 * 0x88 (un-mirroring the display) by flipping the OLD fit through
 * new_x = 239 - old_x. Nobody ever put a finger on the glass to check it, and
 * this file said so for a month. The file's own rule three lines up — "a fitted
 * slope far from +/-1 means this is wrong" — was pointing straight at it: X
 * read 1.1688 while the measured Y read 1.0156.
 *
 * The builder, 2026-09-07: "the edge of the screens is less sensitive then the senter
 * it works but objects at the edge are hard to select ... it my take several
 * trys befor it selects." That is what a gain error feels like. It is near zero
 * wherever the wrong line crosses the right one and grows with distance from
 * it, so the middle of the screen behaves and both edges do not:
 *
 *     touch x=32 (left)    old constants reported  16   (-16 px)
 *     touch x=122 (centre) old constants reported 119   ( -3 px)
 *     touch x=208 (right)  old constants reported 222   (+14 px)
 *
 * The mapping stretched outward from the centre, pushing edge presses off the
 * ends of the controls they were aimed at — and past the left edge into
 * negative coordinates, where the clamp piles a 24-count band of raw readings
 * onto x=0.
 *
 * 🔑 MEASUREMENT, so this is never guessed at again. Parent and Kid 4,
 * the 4-point calibration's own raw log at the known CAL_PTS, least-squares
 * fitted, then pooled across both boards (8 points):
 *
 *              ax        bx        ay        by
 *   Parent     -0.9870    228.6    -1.0137    318.9
 *   Kid 4     -1.0417    238.5    -0.9978    314.9
 *   pooled  -1.0124    233.26   -1.0056    316.87   <- these constants
 *
 * Worst error against those 8 readings: X 21.7px -> 6.9px, Y 15.2px -> 4.7px.
 * Y's error was an OFFSET, not a gain — by was 327.57 against a measured
 * 316.87 — which is why Y always felt fine without ever being right.
 *
 * 🔑 TWO BOARDS, NOT ONE, AND THAT WAS THE POINT. One board cannot tell a
 * design constant from a per-unit quirk. These two agree to ~5% (tap accuracy,
 * not silicon), so this is a property of the ES3C28P and the whole fleet can be
 * fixed here rather than calibrated one handset at a time. The FT6236 is a
 * digital controller reporting in the panel's own coordinate space — there is
 * no per-unit analogue spread to fit out, unlike a resistive panel.
 *
 * ⚠️ raw 0 maps to x=233, so the rightmost ~6px cannot be reached by an honest
 * fit. That is deliberately NOT papered over by inflating bx: ui.c's
 * grow_screen_edges() extends the click area of edge controls outward instead,
 * which fixes the reachability without lying about where the finger was. */
#define TOUCH_SWAP_XY   0
#define TOUCH_AX      (-1.0124f)
#define TOUCH_BX        233.26f
#define TOUCH_AY      (-1.0056f)
#define TOUCH_BY        316.87f

/* ---- audio ---- */
#define PIN_AMP_EN      GPIO_NUM_1     /* FM8002E: LOW = on, HIGH = muted */
#define I2S0_MCK        GPIO_NUM_4
#define I2S0_BCK        GPIO_NUM_5
#define I2S0_WS         GPIO_NUM_7
#define I2S0_DO         GPIO_NUM_8     /* -> ES8311 -> speaker */
#define I2S0_DI         GPIO_NUM_6     /* <- ES8311 <- mic     */
#define I2S1_BCK        GPIO_NUM_14
#define I2S1_WS         GPIO_NUM_21
#define I2S1_DO         GPIO_NUM_2     /* -> PCM5102A -> jack  */

/* ES8311's clock-coefficient table has NO entry for 384x at 44.1kHz — that
 * combination produces quiet static. 256x is the only safe ratio here. */
#define I2S_MCLK_MULT   256
/* ⚠️ MEASURED ON THIS BOARD 2026-08-14: 46 dB CLIPS — the capture peak sat
 * pinned at 100% FS through a whole utterance, and clipped audio transcribes
 * badly. The 46 dB figure came from a different rig (display-test) and did not
 * carry over to the ES8311 path opened in WORK_MODE_BOTH. Start conservative
 * and raise it only against a logged peak: speech should land around 30-60% FS,
 * never 100%. */
#define MIC_GAIN_DB     36.0f   /* 30dB measured 8% FS peak on real speech —
                                 * clean, but quiet. 36 should land ~15-20%.
                                 * Raise only against a LOGGED peak. */

/* Used INSTEAD of MIC_GAIN_DB while auto-level is on. Lower on purpose: at 36dB
 * a shout rails the ADC (measured 2026-08-18, five captures at exactly 32768 =
 * 100% FS, ALC on AND off), and CLIPPING CANNOT BE UNDONE IN SOFTWARE — the
 * peaks are already flat by the time any code sees the samples. Auto-level
 * therefore backs the analog gain off first and makes the quiet end up
 * digitally, which is the order that actually works. */
#define MIC_GAIN_AGC_DB 30.0f

/* ---- SD, SDIO 4-bit ---- */
#define PIN_SD_CLK      GPIO_NUM_38
#define PIN_SD_CMD      GPIO_NUM_40
#define PIN_SD_D0       GPIO_NUM_39
#define PIN_SD_D1       GPIO_NUM_41
#define PIN_SD_D2       GPIO_NUM_48
#define PIN_SD_D3       GPIO_NUM_47
#define SD_MOUNT        "/sdcard"
#define MUSIC_DIR       SD_MOUNT "/music"

/* ---- GPS: GOOUUU breakout, ATGM336H (GPS + BeiDou), UART1 ----
 *
 * 🚨 THESE TWO ARE THE OPPOSITE WAY ROUND FROM EVERY PLANNING DOC, AND THAT IS
 * DELIBERATE. The docs said module-TX -> GPIO44. The hardware says otherwise,
 * and the hardware was MEASURED on 2026-09-12: with each pin configured as an
 * input, GPIO44 followed an internal pull-up and then a pull-down (floating, so
 * it is on the module's RX — an input, which drives nothing), while GPIO43 held
 * HIGH against a pull-down and went active once a second (an idle UART
 * transmitter with a 1 Hz NMEA burst). NMEA then read cleanly on GPIO43:
 * 450 sentences, 0 checksum errors.
 *
 * Do not "fix" these to match the documents. The firmware follows the wires.
 *
 * ⚠️ Both were free: no UART driver existed anywhere in this firmware (the
 * console is USB-CDC), so 43/44 were unassigned. They are NOT strapping pins —
 * The test handset boots, associates, sets its clock and runs telemetry with NMEA streaming
 * in the whole time. The only caveat is falling back to UART flashing.
 *
 * ⚠️ THE LID HALL SENSOR NOW HAS NO PIN. The old plan parked it on 43. GPS has
 * both. Pick it somewhere else before the case is closed. */
#define PIN_GPS_RX      GPIO_NUM_43    /* module TX -> here. Measured. */
#define PIN_GPS_TX      GPIO_NUM_44    /* -> module RX. The $PCAS path; unused */
#define GPS_UART_NUM    UART_NUM_1
#define GPS_BAUD        9600

/* ---- misc ---- */
#define PIN_BATTERY     GPIO_NUM_9     /* ADC, 1:1 divider so x2 for volts */

/* ⚠️ Set to 1 when the FM5324GA + pouch cell is actually wired in.
 *
 * With no pack connected, GPIO9 floats and the ADC reads plausible-looking
 * nonsense — it sat at 93-95% on the bench with nothing attached. This board
 * has NO battery-present sense line, so an absent pack and a full one land in
 * the same voltage window and the firmware cannot tell them apart. Rather than
 * guess with a heuristic that would sometimes lie, the build states which is
 * true. While this is 0 the status bar shows a plug instead of a percentage. */
#define BATTERY_FITTED  0
#define PIN_RGB_LED     GPIO_NUM_42
