/* Screen capture over the USB console. See screencap.h and
 * tools/screencap.py. Only compiled into a -DSCREEN_CAPTURE=ON build.
 *
 * Wire format, every packet:
 *   "SCAP" | type u8 | 0 u8 | len u32 LE | payload[len] | crc32 LE of payload
 * type 1 REGION: seq u32 | t_ms u32 | x1 y1 x2 y2 u16 | RGB565 pixels as LVGL
 *                holds them (LV_COLOR_16_SWAP=y, so BIG-endian per pixel)
 * type 2 TOUCH:  t_ms u32 | x i16 | y i16 | pressed u8
 *
 * PC -> handset: 'F' = arm streaming and resend the whole screen.
 *                'S' = stop streaming.
 *
 * ⚠️ Logs share this link. They are turned down to WARN, but anything that
 * still prints can land between two chunks of a packet. The CRC is what makes
 * that survivable: the PC drops the damaged packet, resyncs on "SCAP" and
 * asks for a full frame. */
#if SCREEN_CAPTURE

#include "screencap.h"
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TX_BUF   4096
#define CHUNK    2048
#define WAIT     pdMS_TO_TICKS(50)

static bool     s_ok, s_armed;
static volatile bool s_want_full;
static uint32_t s_seq;

/* Every chunk is all-or-nothing (the driver's xRingbufferSend), so a packet
 * either goes out whole or is abandoned part way — which the PC sees as a bad
 * CRC. On a failure, stop and ask for a full frame once the link drains. */
static bool put(const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        size_t c = n > CHUNK ? CHUNK : n;
        if (usb_serial_jtag_write_bytes(b, c, WAIT) != (int)c) {
            s_want_full = true;
            return false;
        }
        b += c; n -= c;
    }
    return true;
}

static void packet(uint8_t type, const void *hdr, size_t hn,
                   const void *body, size_t bn)
{
    uint8_t h[10] = { 'S', 'C', 'A', 'P', type, 0 };
    uint32_t len = hn + bn;
    memcpy(h + 6, &len, 4);
    uint32_t crc = esp_rom_crc32_le(0, hdr, hn);
    if (bn) crc = esp_rom_crc32_le(crc, body, bn);
    if (put(h, sizeof h) && put(hdr, hn) && (!bn || put(body, bn)))
        put(&crc, 4);
}

static void poll_cb(lv_timer_t *t)
{
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
        if (c == 'F') { s_armed = true; s_want_full = true; }
        if (c == 'S') s_armed = false;
    }
    if (!usb_serial_jtag_is_connected()) s_armed = false;
    if (s_armed && s_want_full) {
        s_want_full = false;
        lv_obj_invalidate(lv_scr_act());
        /* lv_layer_top() holds popups that sit over the screen. */
        lv_obj_invalidate(lv_layer_top());
    }
}

void screencap_init(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .tx_buffer_size = TX_BUF, .rx_buffer_size = 256 };
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) return;
    usb_serial_jtag_vfs_use_driver();
    esp_log_level_set("*", ESP_LOG_WARN);
    lv_timer_create(poll_cb, 100, NULL);
    s_ok = true;
}

void screencap_region(const lv_area_t *a, const lv_color_t *px)
{
    if (!s_ok || !s_armed) return;
    struct __attribute__((packed)) {
        uint32_t seq, t_ms; uint16_t x1, y1, x2, y2;
    } h = { s_seq++, (uint32_t)(esp_timer_get_time() / 1000),
            a->x1, a->y1, a->x2, a->y2 };
    size_t n = (size_t)(a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1) * sizeof(lv_color_t);
    packet(1, &h, sizeof h, px, n);
}

void screencap_touch(int x, int y, bool pressed)
{
    if (!s_ok || !s_armed) return;
    struct __attribute__((packed)) {
        uint32_t t_ms; int16_t x, y; uint8_t pressed;
    } h = { (uint32_t)(esp_timer_get_time() / 1000), x, y, pressed };
    packet(2, &h, sizeof h, NULL, 0);
}

#endif /* SCREEN_CAPTURE */
