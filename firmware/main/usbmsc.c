/* USB card mode: the SD card, mounted on the PC, without opening the case.
 *
 * The sequence matters and each step is here for a reason:
 *
 *   1. stop everything that reads the card (the player holds a FILE* open on a
 *      track; the GPS logger appends a row a second)
 *   2. unmount FATFS  — esp_vfs_fat_sdcard_unmount() tears down the sdmmc host
 *      with it, so the card has to be brought back up from scratch afterwards
 *   3. re-init the card for MSC, with the SAME slot config library.c uses
 *   4. hand it to TinyUSB and install the driver
 *
 * ⚠️ Deliberately NOT calling tinyusb_msc_storage_mount(): that mounts FATFS
 * for the firmware, and the component's own documentation is explicit that
 * while the firmware holds the mount, "host (PC) can't access the storage via
 * MSC". Leaving it unmounted here is what makes the drive appear.
 */
#include "usbmsc.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "tinyusb.h"
/* esp_tinyusb 2.x. `tusb_msc_storage.h` still exists but is deprecated and
 * -Werror=cpp turns its #warning into a build failure. */
#include "tinyusb_msc.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
/* ⚠️ NOT including esp_private/usb_phy.h. Everything this file does to the PHY
 * is done through RTCCNTL registers directly and it deliberately never calls
 * usb_new_phy()/usb_del_phy() (see usbmsc_release()), so the include was
 * vestigial. It lived in the `usb` HOST component, which esp_tinyusb 2.x no
 * longer pulls in and which IDF 6.0 removed from the tree entirely. */
#include "soc/rtc_cntl_struct.h"
#include "soc/usb_serial_jtag_struct.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "hw.h"
#include "applog.h"
#include "library.h"
#include "player.h"
#include "gpslog.h"

static const char *TAG = "usbmsc";

/* 🔑 THE ARBITRATION BIT THAT SURVIVES A SOFTWARE RESET.
 *
 * On the ESP32-S3 the USB pads are shared between the Serial/JTAG controller
 * and the OTG controller, and `RTCCNTL.usb_conf.sw_usb_phy_sel` decides which
 * one owns them: 1 = OTG, 0 = Serial/JTAG. tinyusb_driver_install() sets it to
 * OTG. `usb_del_phy()` does NOT set it back — read it: all it does is clear the
 * D+/D- pull overrides and free the handle. And the bit is in the RTC domain,
 * so `esp_restart()` (a CPU reset, not a chip reset) leaves it exactly where
 * TinyUSB put it. Only the hardware reset button re-runs the ROM.
 *
 * Creating a SERIAL_JTAG phy is the supported way to claim it back — it lands
 * in usb_serial_jtag_hal_phy_set_external(), which sets sw_hw_usb_phy_sel = 1
 * and sw_usb_phy_sel = 0. */
static const char *NVS_NS  = "usbmsc";
static const char *NVS_KEY = "was_on";


static void flag_set(bool on)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_KEY, on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void usbmsc_boot_recover(void)
{
    nvs_handle_t h;
    uint8_t was = 0;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_get_u8(h, NVS_KEY, &was) != ESP_OK || !was) { nvs_close(h); return; }

    /* 🚨 CLEAR THE FLAG BEFORE ACTING ON IT, NOT AFTER.
     *
     * Measured 2026-09-13: the first version cleared it after the reclaim, and
     * the reclaim crashed — so every boot read the flag, crashed, and rebooted.
     * THIRTEEN times before it was caught. A boot-time recovery that leaves its
     * own trigger set while it does the risky thing is a boot loop with extra
     * steps, and the risky thing is risky by definition or it would not need a
     * recovery. Clear first: one failed attempt, then a normal boot. */
    nvs_set_u8(h, NVS_KEY, 0);
    nvs_commit(h);
    nvs_close(h);

    /* Read who owns the pad before touching anything. If the ROM already gave
     * it back there is nothing to do, and doing nothing cannot crash. */
    if (RTCCNTL.usb_conf.sw_usb_phy_sel == 0) {
        ESP_LOGI(TAG, "USB pad already serial/JTAG");
        return;
    }
    {
        /* 🚨 THE SAFETY NET. If the exit path failed for any reason, the pad is
         * still with OTG and this handset is invisible to every PC — with USB
         * being the only way to reflash it, and no OTA in this firmware. So the
         * recovery must not depend on the exit path having worked: it runs at
         * BOOT, and a boot is reachable from the touchscreen alone
         * (Settings -> Restart). Case stays shut. */
        ESP_LOGW(TAG, "card mode left the pad with OTG - reclaiming it");
        /* ⚠️ Registers only. NOT usb_new_phy() — at this point in boot the
         * console driver already holds the Serial/JTAG PHY, and asking for a
         * second one is what aborted. These three writes are what the LL does. */
        RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;
        RTCCNTL.usb_conf.sw_usb_phy_sel    = 0;
        USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
        ESP_LOGW(TAG, "phy_sel now %d", (int)RTCCNTL.usb_conf.sw_usb_phy_sel);
    }
}

static bool          s_active;
static sdmmc_card_t *s_card;
/* esp_tinyusb 2.x hands back a storage handle. Kept so a future release path
 * can call tinyusb_msc_delete_storage() — see usbmsc_release() on why teardown
 * is deliberately not done here. */
static tinyusb_msc_storage_handle_t s_msc;
static char          s_status[72] = "";

bool        usbmsc_active(void) { return s_active; }

bool usbmsc_host_present(void)
{
    static bool seen;
    if (!s_active) return true;
    if (tud_mounted()) { seen = true; return true; }
    return !seen;                 /* not enumerated YET is not "unplugged" */
}
const char *usbmsc_status(void) { return s_status; }

static esp_err_t fail(const char *what, esp_err_t err)
{
    snprintf(s_status, sizeof(s_status), "%s: %s", what, esp_err_to_name(err));
    ESP_LOGE(TAG, "%s", s_status);
    applog("[usb] %s", s_status);
    return err;
}

esp_err_t usbmsc_enter(void)
{
    if (s_active) return ESP_OK;

    applog("[usb] card mode: letting go of the card");
    player_suspend();
    if (gpslog_active()) gpslog_stop();

    esp_err_t err = library_unmount();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return fail("unmount", err);
    }

    /* Same numbers as library_mount(). If these two ever disagree the card
     * will mount at one speed for the firmware and another for the PC, and the
     * difference will look like a flaky card. */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = PIN_SD_CLK;
    slot.cmd = PIN_SD_CMD;
    slot.d0  = PIN_SD_D0;
    slot.d1  = PIN_SD_D1;
    slot.d2  = PIN_SD_D2;
    slot.d3  = PIN_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    err = sdmmc_host_init();
    if (err != ESP_OK) return fail("host init", err);
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
    if (err != ESP_OK) return fail("slot init", err);

    s_card = calloc(1, sizeof(sdmmc_card_t));
    if (!s_card) return fail("card alloc", ESP_ERR_NO_MEM);
    err = sdmmc_card_init(&host, s_card);
    if (err != ESP_OK) { free(s_card); s_card = NULL; return fail("card init", err); }

    /* esp_tinyusb 2.x splits what 1.7 did in one call: install the MSC driver,
     * then create a storage instance on it. */
    const tinyusb_msc_driver_config_t dcfg = {
        .user_flags = { .val = 0 },     /* automatic remount on connect/disconnect */
        .callback = NULL,
        .callback_arg = NULL,
    };
    err = tinyusb_msc_install_driver(&dcfg);
    if (err != ESP_OK) return fail("msc driver", err);

    const tinyusb_msc_storage_config_t scfg = {
        .medium = { .card = s_card },
        /* ⚠️ format_if_mount_failed FALSE, for the same reason library.c says
         * so: the examples default it to true and that would reformat a 7 GB
         * music library over a bad mount. 2.x adds `do_not_format`, which says
         * the same thing explicitly — set BOTH, so neither a default change nor
         * a missed field can ever turn formatting back on. */
        .fat_fs = {
            .base_path = NULL,
            .config = {
                .format_if_mount_failed = false,
                .max_files = 4,
                .allocation_unit_size = 16 * 1024,
            },
            .do_not_format = true,
            .format_flags = 0,
        },
        /* 🔑 THE CARD GOES STRAIGHT TO THE HOST, NOT TO US. This preserves the
         * 1.7 behaviour noted at the top of this file: we deliberately never
         * mounted FATFS locally here. MOUNT_APP would mount it on the device
         * and is the wrong owner while the PC has the card. */
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    err = tinyusb_msc_new_storage_sdmmc(&scfg, &s_msc);
    if (err != ESP_OK) return fail("msc storage", err);

    /* 2.x replaces the hand-built all-NULL descriptor struct with a macro that
     * fills in the full/high-speed defaults for this target. Descriptors still
     * come from Kconfig, where CONFIG_TINYUSB_MSC_ENABLED puts the MSC
     * interface. */
    const tinyusb_config_t tcfg = TINYUSB_DEFAULT_CONFIG();
    err = tinyusb_driver_install(&tcfg);
    if (err != ESP_OK) return fail("usb install", err);

    s_active = true;
    flag_set(true);
    uint64_t mb = ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024);
    snprintf(s_status, sizeof(s_status), "%llu MB on the PC", mb);
    ESP_LOGI(TAG, "card handed to USB: %s", s_status);
    applog("[usb] card mode ON - %s", s_status);
    return ESP_OK;
}

esp_err_t usbmsc_release(void)
{
    if (!s_active) return ESP_OK;

    /* 🚨 NO tinyusb_driver_uninstall() HERE, AND NO usb_new_phy().
     *
     * Measured on the builder's handset 2026-09-13, and it cost a second case opening:
     * uninstall stops the TinyUSB task and WAITS for teardown. Called from the
     * LVGL event handler — which is where a button press runs — it never
     * returned. The UI froze with the Reboot button painted and dead, the host
     * dropped the drive because MSC stopped answering mid-teardown, and the pad
     * stayed with OTG. One blocking call, all three symptoms.
     *
     * The insight that makes it simple: WE ARE ABOUT TO esp_restart(). A polite
     * USB teardown buys nothing across a reset. Take the pad back with three
     * register writes that cannot block, clear the flag, and go. */
    RTCCNTL.usb_conf.sw_hw_usb_phy_sel = 1;
    RTCCNTL.usb_conf.sw_usb_phy_sel    = 0;      /* 0 = Serial/JTAG            */
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;

    s_active = false;
    flag_set(false);

    ESP_LOGW(TAG, "USB pad reclaimed, phy_sel now %d",
             (int)RTCCNTL.usb_conf.sw_usb_phy_sel);

    ESP_LOGI(TAG, "USB released back to serial/JTAG");
    applog("[usb] card mode off - USB handed back");
    return ESP_OK;
}
