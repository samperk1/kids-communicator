/*
 * Panel, touch, and the LVGL glue between them.
 *
 * The ILI9341 and FT6236 code is lifted from a display test sketch, which was
 * gated on hardware: colour bars, an I2C probe, a 4-point calibration and a
 * hit-test. Only the LVGL bindings are new.
 *
 * ⚠️ ONE DELIBERATE CHANGE FROM THE TEST RIG. That rig pushed the framebuffer
 * one row per SPI transaction, which measured 97.6ms for a full screen (10.2
 * fps) in portrait — worse than landscape's 81ms, because portrait sends 320
 * short rows instead of 240 long ones and pays the per-transaction overhead 33%
 * more often. Here each flush goes out as ONE transaction covering the whole
 * dirty rectangle. Same pixels, a fraction of the overhead.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"
#include "hw.h"
#include "display.h"
#include "applog.h"
#include "settings.h"
#include "screencap.h"
#include <stdlib.h>

static const char *TAG = "disp";

static spi_device_handle_t s_lcd;
static i2c_master_bus_handle_t s_i2c;
static i2c_master_dev_handle_t s_touch;
static bool s_touch_ok = false;

/* ONE buffer, 32 rows.
 *
 * Was two buffers of 40 rows = 38,400 bytes of INTERNAL RAM. That is memory
 * WiFi and lwIP cannot do without: with it spent, internal free fell to 1,455
 * bytes mid-upload and a 220KB POST to speech-to-text collapsed to 1 KB/s.
 *
 * Double buffering exists so LVGL can render one buffer while the other is
 * being sent by DMA in the background. flush_cb here uses
 * spi_device_polling_transmit(), which BLOCKS until the transfer completes, so
 * there is nothing to overlap with and the second buffer bought nothing. One
 * buffer of 32 rows costs 15,360 bytes instead of 38,400 — 23KB back — and
 * sends in larger pieces than two small ones would. */
#define BUF_ROWS 32
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t *s_buf1, *s_buf2;
static lv_disp_drv_t s_disp_drv;
static lv_indev_drv_t s_indev_drv;

/* ------------------------------------------------------------------ *
 * SPI + ILI9341
 * ------------------------------------------------------------------ */

static void IRAM_ATTR lcd_pre_cb(spi_transaction_t *t)
{
    gpio_set_level(PIN_LCD_DC, (int)(intptr_t)t->user);
}

static void lcd_cmd(uint8_t cmd)
{
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd, .user = (void *)0 };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd, &t));
}

static void lcd_data(const uint8_t *d, int len)
{
    if (!len) return;
    spi_transaction_t t = { .length = len * 8, .tx_buffer = d, .user = (void *)1 };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_lcd, &t));
}

static void lcd_cmd_data(uint8_t cmd, const uint8_t *d, int len)
{
    lcd_cmd(cmd);
    lcd_data(d, len);
}

static void lcd_window(int x0, int y0, int x1, int y1)
{
    uint8_t b[4];
    b[0] = x0 >> 8; b[1] = x0 & 0xFF; b[2] = x1 >> 8; b[3] = x1 & 0xFF;
    lcd_cmd_data(0x2A, b, 4);
    b[0] = y0 >> 8; b[1] = y0 & 0xFF; b[2] = y1 >> 8; b[3] = y1 & 0xFF;
    lcd_cmd_data(0x2B, b, 4);
    lcd_cmd(0x2C);
}

/* ------------------------------------------------------------------ *
 * Backlight — LEDC PWM, not a GPIO level
 * ------------------------------------------------------------------ *
 *
 * This was gpio_set_level(PIN_LCD_BL, on) — fully on or fully off, nothing
 * between. The backlight dominates idle draw (see the blanking note below), so
 * "on" being the only lit state costs battery every second the screen is up,
 * and full brightness in a dark bedroom is unpleasant besides.
 *
 * 🚨 20 kHz, NOT the 5 kHz every example uses. The backlight boost converter's
 * inductor sings at its switching frequency, and this board has a microphone
 * about 3 cm from it. An audible whine here would land in every walkie
 * transmission and every voice command, and it would be blamed on the mic.
 * 20 kHz is above the audible band with margin. LEDC can hold 10-bit
 * resolution up to ~78 kHz off the 80 MHz APB clock, so this costs nothing.
 *
 * 🚨 THE FLOOR IS NOT OPTIONAL. Duty 0 is a black screen that still draws, and
 * a handset whose brightness slider reaches 0 is a brick: you cannot see the
 * slider you need in order to turn it back up. A child WILL drag it to the end.
 * BRIGHT_MIN_PCT is the darkest the UI can ask for; display_screen_off() still
 * goes to a true 0 because that path has display_note_activity() to undo it.
 */
#define BL_TIMER        LEDC_TIMER_0
#define BL_CHANNEL      LEDC_CHANNEL_0
#define BL_MODE         LEDC_LOW_SPEED_MODE
#define BL_RES          LEDC_TIMER_10_BIT
#define BL_MAX_DUTY     ((1 << 10) - 1)
#define BL_FREQ_HZ      20000

static int  s_brightness = 100;   /* percent the UI last asked for */
static bool s_screen_on  = true;  /* declared here, not with the blanking state
                                   * below, because display_backlight() needs it
                                   * and sits above that block. */

static void backlight_apply(int pct, bool on)
{
    if (pct < BRIGHT_MIN_PCT) pct = BRIGHT_MIN_PCT;
    if (pct > 100)            pct = 100;

    /* Perceived brightness is nowhere near linear in duty — the top half of a
     * linear slider all looks the same and the bottom half is a cliff. Squaring
     * spends the slider's travel where the eye can actually see a difference.
     * Integer maths on purpose: no float, and 100*100 fits an int easily. */
    uint32_t duty = on ? (uint32_t)((BL_MAX_DUTY * pct * pct) / (100 * 100)) : 0;

    ledc_set_duty(BL_MODE, BL_CHANNEL, duty);
    ledc_update_duty(BL_MODE, BL_CHANNEL);
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode      = BL_MODE,
        .timer_num       = BL_TIMER,
        .duty_resolution = BL_RES,
        .freq_hz         = BL_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));

    ledc_channel_config_t c = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = BL_MODE,
        .channel    = BL_CHANNEL,
        .timer_sel  = BL_TIMER,
        .duty       = 0,          /* dark until lcd_init has something to show */
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

static void lcd_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_LCD_DC),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    backlight_init();

    spi_bus_config_t bus = {
        .sclk_io_num = PIN_SCLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* One flush = one transaction, so this must cover a whole buffer. */
        .max_transfer_sz = LCD_W * BUF_ROWS * sizeof(lv_color_t) + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev = {
        .clock_speed_hz = LCD_SPI_HZ,
        .mode = 0,
        .spics_io_num = PIN_LCD_CS,
        .queue_size = 4,
        .pre_cb = lcd_pre_cb,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev, &s_lcd));

    lcd_cmd(0x01);                       /* SWRESET — no reset pin on this board */
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd_data(0xCF, (const uint8_t[]){0x00, 0xC1, 0x30}, 3);
    lcd_cmd_data(0xED, (const uint8_t[]){0x64, 0x03, 0x12, 0x81}, 4);
    lcd_cmd_data(0xE8, (const uint8_t[]){0x85, 0x00, 0x78}, 3);
    lcd_cmd_data(0xCB, (const uint8_t[]){0x39, 0x2C, 0x00, 0x34, 0x02}, 5);
    lcd_cmd_data(0xF7, (const uint8_t[]){0x20}, 1);
    lcd_cmd_data(0xEA, (const uint8_t[]){0x00, 0x00}, 2);
    lcd_cmd_data(0xC0, (const uint8_t[]){0x23}, 1);
    lcd_cmd_data(0xC1, (const uint8_t[]){0x10}, 1);
    lcd_cmd_data(0xC5, (const uint8_t[]){0x3E, 0x28}, 2);
    lcd_cmd_data(0xC7, (const uint8_t[]){0x86}, 1);
    lcd_cmd_data(0x36, (const uint8_t[]){LCD_MADCTL}, 1);
    lcd_cmd_data(0x3A, (const uint8_t[]){0x55}, 1);
    lcd_cmd_data(0xB1, (const uint8_t[]){0x00, 0x18}, 2);
    lcd_cmd_data(0xB6, (const uint8_t[]){0x08, 0x82, 0x27}, 3);
    lcd_cmd_data(0xF2, (const uint8_t[]){0x00}, 1);
    lcd_cmd_data(0x26, (const uint8_t[]){0x01}, 1);
    lcd_cmd_data(0xE0, (const uint8_t[]){0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,
                                         0x37,0x07,0x10,0x03,0x0E,0x09,0x00}, 15);
    lcd_cmd_data(0xE1, (const uint8_t[]){0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,
                                         0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F}, 15);
    lcd_cmd(0x21);                       /* INVON — this panel wants inverted */
    lcd_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(20));

    ESP_LOGI(TAG, "ILI9341 up: %dx%d portrait, MADCTL 0x%02X, %d MHz",
             LCD_W, LCD_H, LCD_MADCTL, LCD_SPI_HZ / 1000000);
}

static volatile bool     s_quiet;
static volatile uint32_t s_flushes_blocked;

void display_backlight(bool on)
{
    backlight_apply(s_brightness, on);
}

/* ILI9341: 0x28 DISPOFF, 0x10 SLPIN, 0x11 SLPOUT, 0x29 DISPON. SLPOUT needs
 * 120ms before the panel will accept DISPON — the datasheet's figure, and
 * skipping it gives a white flash on the way back.
 *
 * ⚠️ Called from the UI task, the same task that runs lv_task_handler(), so no
 * flush can be in flight here. Do not call it from anywhere else without
 * thinking about that. */
void display_quiet(bool quiet)
{
    if (quiet == s_quiet) return;

    if (quiet) {
        backlight_apply(s_brightness, false);
        s_quiet = true;              /* set FIRST: a flush racing in behind
                                      * DISPOFF is bus traffic, and bus traffic
                                      * is the thing being measured */
        lcd_cmd(0x28);
        lcd_cmd(0x10);
        applog("[disp] quiet - panel asleep, bus silent");
    } else {
        lcd_cmd(0x11);
        vTaskDelay(pdMS_TO_TICKS(120));
        lcd_cmd(0x29);
        s_quiet = false;
        if (lv_scr_act()) lv_obj_invalidate(lv_scr_act());
        backlight_apply(s_brightness, true);
        applog("[disp] loud - panel awake");
    }
}

bool     display_quiet_get(void)      { return s_quiet; }
uint32_t display_flushes_blocked(void) { return s_flushes_blocked; }

/* Live preview: the menu row calls this as the value changes so the screen
 * dims under the finger. settings.c does the NVS write on its own schedule —
 * this must not touch flash, it is called on every step of an adjust. */
void display_set_brightness(int pct)
{
    if (pct < BRIGHT_MIN_PCT) pct = BRIGHT_MIN_PCT;
    if (pct > 100)            pct = 100;
    s_brightness = pct;
    if (s_screen_on) backlight_apply(s_brightness, true);
}

int display_brightness(void) { return s_brightness; }

/* ------------------------------------------------------------------ *
 * FT6236
 * ------------------------------------------------------------------ */

static bool touch_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &s_i2c));

    if (i2c_master_probe(s_i2c, FT6236_ADDR, 100) != ESP_OK) {
        ESP_LOGE(TAG, "FT6236 not answering at 0x%02X — touch disabled", FT6236_ADDR);
        return false;
    }
    i2c_device_config_t dc = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FT6236_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c, &dc, &s_touch));

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_TOUCH_INT),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_LOGI(TAG, "FT6236 up at 0x%02X", FT6236_ADDR);
    return true;
}

/* The ES8311 shares this bus at 0x18; audio.c reuses the handle. */
i2c_master_bus_handle_t display_i2c_bus(void) { return s_i2c; }

static bool touch_raw(int *rx, int *ry)
{
    uint8_t reg = 0x02, b[5];
    if (!s_touch_ok) return false;
    if (i2c_master_transmit_receive(s_touch, &reg, 1, b, 5, 50) != ESP_OK) return false;
    if ((b[0] & 0x0F) == 0) return false;
    *rx = ((b[1] & 0x0F) << 8) | b[2];
    *ry = ((b[3] & 0x0F) << 8) | b[4];
    return true;
}

/* Unmapped reading, for the calibration screen — which cannot use the mapping
 * it exists to produce. */
bool display_touch_raw(int *rx, int *ry) { return touch_raw(rx, ry); }

/* ------------------------------------------------------------------ *
 * LVGL bindings
 * ------------------------------------------------------------------ */

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    /* 🔑 THE POINT OF THE WHOLE THING: while quiet, not one byte goes down the
     * bus. LVGL still runs and still marks areas dirty — it just gets told the
     * flush is done. Coming back out of quiet invalidates the screen, so
     * nothing that changed in the dark is lost. */
    if (s_quiet) { s_flushes_blocked++; lv_disp_flush_ready(drv); return; }

    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    lcd_window(area->x1, area->y1, area->x2, area->y2);
    /* One transaction for the whole dirty rectangle — see the note at the top
     * of this file about why this is not done row by row. */
    lcd_data((const uint8_t *)px, w * h * sizeof(lv_color_t));
#if SCREEN_CAPTURE
    screencap_region(area, px);      /* panel first: the screen never waits */
#endif

    lv_disp_flush_ready(drv);
}

/* ---- screen blanking ----
 *
 * The backlight is most of the idle draw, so this is the single biggest thing
 * the firmware can do for battery life.
 *
 * 🚨 THE WAKE TAP MUST NOT REACH LVGL. With a dark screen the user cannot see
 * what is under their finger, so passing that first press through means tapping
 * a black screen starts a random track — or worse, hits whatever sits where
 * they happened to touch. The press that wakes the device is swallowed, and
 * `s_eat_touch` stays set until the finger LIFTS, so one tap cannot wake and
 * click. */
static bool    s_touch_down_dbg;
static bool    s_eat_touch;
static int64_t s_last_touch_us;
static int64_t s_touch_seen_us;   /* last poll that actually saw a finger */
/* Every indev poll, finger or not. See display.h for why this is the metric. */
static volatile uint32_t s_indev_polls;

/* 🚨 A SINGLE "NO TOUCH" READING DOES NOT MEAN THE FINGER LIFTED.
 *
 * touch_raw() returns false both when the count register reads 0 AND on any
 * I2C error — and the FT6236 samples on its own schedule, so while a finger is
 * genuinely still down the register intermittently reads 0. LVGL polls every
 * ~30ms, so one physical tap reads true -> false -> true.
 *
 * That flicker used to clear s_eat_touch on the middle sample, and the SAME
 * press then reached LVGL as a real click. The builder, 2026-08-16: tapped a sleeping
 * screen above centre to wake it and "it started to play music" — the wake tap
 * landed on the play button, which is exactly the thing the guard exists to
 * prevent.
 *
 * So the release must be SUSTAINED before the guard is dropped. 200ms is far
 * longer than the controller's dropouts and far shorter than a deliberate
 * second tap. */
#define TOUCH_RELEASE_US  (200 * 1000)

void display_note_activity(void)
{
    s_last_touch_us = esp_timer_get_time();
    if (!s_screen_on) {
        s_screen_on = true;
        display_backlight(true);
    }
}

bool display_screen_on(void) { return s_screen_on; }

void display_screen_off(void)
{
    if (!s_screen_on) return;
    s_screen_on = false;
    s_eat_touch = true;          /* whatever wakes it is consumed */
    display_backlight(false);
}

int display_idle_ms(void)
{
    return (int)((esp_timer_get_time() - s_last_touch_us) / 1000);
}

/* Consecutive presses whose mapping lands well off the panel. */
static int s_cal_wild;

uint32_t display_indev_polls(void) { return s_indev_polls; }

static void indev_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    int rx, ry;
    s_indev_polls++;
    if (touch_raw(&rx, &ry)) {
        s_touch_seen_us = esp_timer_get_time();
        display_note_activity();
        if (s_eat_touch) {           /* woke the screen: report nothing */
            data->state = LV_INDEV_STATE_RELEASED;
            return;
        }
        const touch_cal_t *c = settings_touch_cal();
        int swap = c->valid ? c->swap : TOUCH_SWAP_XY;
        int u = swap ? ry : rx;
        int v = swap ? rx : ry;
        int x, y;
        if (c->valid) {
            x = (int)(c->ax * u + c->bx);
            y = (int)(c->ay * v + c->by);
        } else {
            x = (int)(TOUCH_AX * u + TOUCH_BX);
            y = (int)(TOUCH_AY * v + TOUCH_BY);
        }

        /* 🔎 Raw vs mapped, once per press. hw.h says X IS DERIVED, NOT
         * MEASURED — it was computed when the display was un-mirrored and never
         * checked against a finger. Symptom if it is wrong: centred controls
         * work and edge controls do not, which is exactly what the builder reported
         * (Talk in the middle worked, Browse left and Radio right did not, and
         * the volume slider moved on its own from stray hits). */
        if (!s_touch_down_dbg) {
            applog("[touch] raw %d,%d -> %d,%d", rx, ry, x, y);
            s_touch_down_dbg = true;

            /* 🚨 A CALIBRATION THAT MAPS OFF THE PANEL MUST NOT BE PERMANENT.
             *
             * Kid 1, 2026-09-06: a stored fit mapped every touch to negative
             * coordinates (raw 16,319 -> 243,-134). Nothing on screen could be
             * pressed — including the YES button on the question that offers to
             * recalibrate. You need calibration precisely when touch is broken,
             * so a recovery that depends on touch is circular, and the handset
             * was unrecoverable without a cable.
             *
             * The mapping is checkable against itself: a fit that keeps landing
             * well outside a 240x320 panel is wrong, whatever the finger did.
             * Discard it and fall back to the compiled-in defaults, which are
             * approximate but reachable — enough to open the menu and calibrate
             * properly. Four presses in a row, so an edge touch cannot do it. */
            if (c->valid) {
                bool wild = (x < -30 || y < -30 ||
                             x >= LCD_W + 30 || y >= LCD_H + 30);
                if (wild && ++s_cal_wild >= 4) {
                    settings_clear_touch_cal();
                    s_cal_wild = 0;
                    applog("[touch] stored calibration maps off-screen - "
                           "discarded, using defaults");
                } else if (!wild) {
                    s_cal_wild = 0;
                }
            }
        }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= LCD_W) x = LCD_W - 1;
        if (y >= LCD_H) y = LCD_H - 1;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        /* 🚨 DO NOT CLEAR THE GUARD WHILE THE SCREEN IS STILL OFF.
         *
         * This is the bug that made the wake tap useless. display_screen_off()
         * sets s_eat_touch, and then the VERY NEXT POLL lands here — with no
         * finger anywhere near the glass — and cleared it. LVGL polls every
         * ~30ms, so the guard survived 30ms after blanking and was always gone
         * by the time anyone actually touched the screen. Measured 2026-08-16:
         * 3 of 4 wake taps started music; the 4th only missed because it landed
         * on empty space.
         *
         * s_screen_on is the discriminator. While it is false, nothing has
         * woken the device yet and the guard MUST persist however long the
         * device sits idle. It is only once a press has woken the screen that a
         * sustained release means "that press is finished".
         *
         * The TOUCH_RELEASE_US debounce still matters on top of that: the
         * FT6236 reports 0 touches intermittently while a finger is genuinely
         * down, so without it a single tap could still wake and click. */
        if (s_screen_on &&
            esp_timer_get_time() - s_touch_seen_us > TOUCH_RELEASE_US) {
            s_eat_touch = false;     /* finger really lifted: next press is real */
            s_touch_down_dbg = false;
        }
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Capture builds also report each touch, so the video can draw a tap dot.
 * Only changes are sent: press, release, and moves of 3px or more. */
static void indev_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    indev_read(drv, data);
#if SCREEN_CAPTURE
    static bool down;
    static int lx, ly;
    bool p = data->state == LV_INDEV_STATE_PRESSED;
    if (p != down || (p && (abs(data->point.x - lx) >= 3 ||
                            abs(data->point.y - ly) >= 3))) {
        down = p; lx = data->point.x; ly = data->point.y;
        screencap_touch(lx, ly, p);
    }
#endif
}

static void tick_cb(void *arg) { lv_tick_inc(1); }

void display_init(void)
{
    lcd_init();
    s_touch_ok = touch_init();

    lv_init();

    size_t bytes = LCD_W * BUF_ROWS * sizeof(lv_color_t);

    /* 🚨 MUST BE INTERNAL DMA MEMORY. DO NOT "SAVE RAM" BY MOVING THIS TO PSRAM.
     *
     * I tried exactly that on 2026-08-14 and it crash-looped the board on the
     * first repaint:
     *
     *     ESP_ERROR_CHECK failed: ESP_ERR_NO_MEM
     *       lcd_data -> spi_device_polling_transmit
     *
     * The trap is the word "polling". spi_device_polling_transmit() describes
     * how completion is AWAITED (busy-wait instead of an interrupt) — it says
     * nothing about whether DMA moves the bytes. This bus is initialised with
     * SPI_DMA_CH_AUTO, so DMA is used, and a source buffer in PSRAM forces the
     * driver to allocate an internal bounce buffer of the same size. At 19KB
     * that allocation fails and ESP_ERROR_CHECK aborts.
     *
     * The RAM was reclaimed by making the buffer SMALLER and dropping the
     * second one instead — see the note on BUF_ROWS. */
    s_buf1 = heap_caps_malloc(bytes, MALLOC_CAP_DMA);
    s_buf2 = NULL;                    /* single buffered on purpose */
    if (!s_buf1) {
        ESP_LOGE(TAG, "no DMA memory for draw buffers (%u bytes each)",
                 (unsigned)bytes);
        abort();
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, LCD_W * BUF_ROWS);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = LCD_W;
    s_disp_drv.ver_res  = LCD_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);
#if SCREEN_CAPTURE
    screencap_init();
#endif

    lv_indev_drv_init(&s_indev_drv);
    s_indev_drv.type    = LV_INDEV_TYPE_POINTER;
    s_indev_drv.read_cb = indev_cb;
    /* 🚨 A SCROLLABLE PARENT STEALS ITS CHILDREN'S CLICKS, AND IT LOOKS LIKE A
     * DEAD BUTTON. LVGL turns a press into a scroll gesture once the finger
     * moves `scroll_limit` pixels, and the child then gets PRESS_LOST — so
     * LV_EVENT_CLICKED never fires even though the pressed shading was already
     * painted. The builder, 2026-09-08, on the Fleet screen: "the screen did the shade
     * change when i taped it it just never changed to unpaird".
     *
     * The default is 10px, which a thumb on a 2.8" panel beats easily. 26 is
     * still far short of a deliberate drag, and this is the RIGHT place for the
     * fix: every scrollable list in the app has the same trap, and the Fleet
     * screen's remove is the ONLY recovery when a pair offer is lost. */
    s_indev_drv.scroll_limit = 26;
    lv_indev_drv_register(&s_indev_drv);

    const esp_timer_create_args_t targs = {
        .callback = tick_cb, .name = "lv_tick",
    };
    esp_timer_handle_t th;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &th));
    ESP_ERROR_CHECK(esp_timer_start_periodic(th, 1000));   /* 1 ms */

    ESP_LOGI(TAG, "LVGL %d.%d.%d up, 1x%u-byte draw buffer (internal DMA); "
             "internal free %u",
             LVGL_VERSION_MAJOR, LVGL_VERSION_MINOR, LVGL_VERSION_PATCH,
             (unsigned)bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
