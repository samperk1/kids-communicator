/*
 * Communicator #2 — ES3C28P.
 *
 * Boot order matters: the panel comes up first so a slow card scan has
 * something to show progress on, and the codec reuses the I2C bus the touch
 * controller opened.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "hw.h"
#include "display.h"
#include "battery.h"
#include "library.h"
#include "faves.h"
#include "player.h"
#include "settings.h"
#include "applog.h"
#include "voice.h"
#include "wt.h"
#include "radio.h"
#include "power.h"
#include "net.h"
#include "telemetry.h"
#include "esp_app_desc.h"
#include "t9.h"
#include "espnow.h"
#include "fleet.h"
#include "ui.h"
#include "usbmsc.h"

static const char *TAG = "main";

/* On-air test build: no telemetry, so WiFi never steals the radio from BLE. */
#define BLE_ONAIR_TEST 0

/* 🔎 THE TEST THAT DECIDES WHETHER net_deinit() IS SAFE TO KEEP.
 *
 * Cycling the WiFi driver is the one thing standing between BLE and the ~30KB
 * it needs, and a teardown that leaks even 200 bytes a cycle is a slow death
 * that would only surface hours into an unattended battery run — by which point
 * it looks like anything except a WiFi teardown bug. Twenty cycles, printing
 * internal free at each stage: the "after" column must not drift. */
#define NET_CYCLE_TEST 0

/* Shown while the card is walked, so several seconds of scanning does not look
 * like a dead device. */
static lv_obj_t *boot_msg;

static void boot_screen(const char *text)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x080A0E), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(scr);
    lv_obj_set_style_text_font(t, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(0xF0A830), 0);
    lv_label_set_text(t, "COMMUNICATOR");
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -30);

    boot_msg = lv_label_create(scr);
    lv_obj_set_style_text_font(boot_msg, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(boot_msg, lv_color_hex(0x7C8798), 0);
    lv_label_set_text(boot_msg, text);
    lv_obj_align(boot_msg, LV_ALIGN_CENTER, 0, 6);

    lv_scr_load(scr);
    lv_timer_handler();
}

static void scan_progress(int artists, int albums, int tracks)
{
    if (!boot_msg) return;
    lv_label_set_text_fmt(boot_msg, "%d artists  %d albums  %d tracks",
                          artists, albums, tracks);
    lv_timer_handler();
}

#if NET_CYCLE_TEST
static void net_cycle_test(void *arg)
{
    /* Let BLE settle first — the numbers are meaningless while the port is
     * still initialising. */
    vTaskDelay(pdMS_TO_TICKS(8000));
    applog("[cycle] 20 x net_up/net_down, watching internal RAM");

    unsigned first_after = 0;
    for (int i = 1; i <= 20; i++) {
        unsigned before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        bool     up     = net_up();
        unsigned during = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        net_down();
        vTaskDelay(pdMS_TO_TICKS(1000));       /* let ble_resume() finish */
        unsigned after  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

        if (i == 1) first_after = after;
        applog("[cycle] %2d  before %6u  up=%d  during %6u  after %6u  drift %+d",
               i, before, (int)up, during, after, (int)after - (int)first_after);
    }
    applog("[cycle] done");
    vTaskDelete(NULL);
}
#endif

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Before anything can log: applog's mutex must exist before two tasks can
     * race to create it. See applog_init(). */
    applog_init();
    /* Before anything wants the USB console: if a card mode session left the
     * USB pads with the OTG controller, this handset is invisible to every PC
     * and USB is the only way to reflash it. See usbmsc.h. */
    usbmsc_boot_recover();

    /* Before telemetry_init() or ui_init(): both start tasks that raise the
     * radio, and they must find the lock already there. */
    net_init();
    /* Before anything else touches the sleep state - the wake cause is only
     * readable once, and the alarm needs to know it fired. */
    power_init();

    ESP_LOGI(TAG, "=== Communicator #2 ===");
    ESP_LOGI(TAG, "PSRAM %u bytes, internal free %u",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    settings_load();

    /* 🔑 SAY WHICH BUILD THIS IS, EVERY BOOT.
     *
     * Three handsets get flashed at different moments in a session and there is
     * no way to ask a board what it is running - so "did Guest get the
     * calibration guard?" became a question answered by reading back through a
     * transcript. esp_app_get_description() is filled in by the build system,
     * so it cannot drift the way a hand-edited version string does. */
    {
        /* ⚠️ d->date/d->time ARE NOT RELIABLE HERE - observed reading 17:23 on a
         * binary linked at 22:13, because that translation unit does not
         * recompile on every build. A stamp that lies is worse than none.
         * The ELF SHA-256 is derived from the actual image, so it cannot be
         * stale: same 8 characters = same firmware, different = different. */
        const esp_app_desc_t *d = esp_app_get_description();
        char sha[17] = { 0 };
        for (int i = 0; i < 4; i++)
            sprintf(sha + i * 2, "%02x", d->app_elf_sha256[i]);
        applog("[boot] build %s (%s %s)", sha, d->date, d->time);
    }

    display_init();

    /* 🚨 HOLD A FINGER ON THE SCREEN WHILE IT BOOTS = RESET THE TOUCH
     * CALIBRATION. The escape hatch for a handset that has been locked out.
     *
     * The builder, 2026-08-21: "it jsut got locked out of the boys comunicater". A bad
     * calibration lives in NVS, so it survives a power cycle AND a reflash, and
     * a device that does not know where fingers land cannot be driven to the
     * tool that fixes it. Every other way in needs the broken mapping to work.
     * This one needs nothing but the raw controller.
     *
     * ⚠️ Costs one read on a normal boot. The 4-second hold only happens if
     * something is already touching the panel at start-up, and it must be
     * CONTINUOUS - a board sitting face-down in a bag lets go at some point,
     * and a brief knock is not four seconds. */
    {
        int rx, ry;
        if (display_touch_raw(&rx, &ry)) {
            bool held = true;
            for (int i = 0; i < 40 && held; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
                held = display_touch_raw(&rx, &ry);
            }
            if (held) {
                settings_clear_touch_cal();    /* flushes immediately: a rescue */
                applog("[cal] RESET by boot-time hold - back to the default mapping");
            }
        }
    }
    /* Before the backlight comes up, not after: settings_load() has already run
     * and knows the stored level, and lighting at 100% first would flash the
     * screen bright for a frame on a handset someone deliberately dimmed. */
    display_set_brightness(settings_brightness());
    display_backlight(true);
    display_note_activity();          /* start the blank timer from boot */
    boot_screen("starting");

    battery_init();

    /* 🔑 STARTED BEFORE THE CARD, ON PURPOSE.
     *
     * Both branches below `return` out of app_main — a board with no SD card or
     * no music stops dead at a boot screen. Started after them, an unattended
     * battery run on a board whose card was missing would report NOTHING and
     * the whole session would be wasted with no way to tell why. Up here it
     * reports regardless, and a run with no card is still a valid power
     * measurement of a board sitting at a boot screen.
     *
     * Safe this early: player_state() reads a statically initialised struct, so
     * it answers "not playing" correctly before player_init() has run. */
    /* ⚠️ TEMPORARY, FOR THE ON-AIR CHECK ONLY — REVERT BEFORE ANY BATTERY RUN.
     *
     * Telemetry brings WiFi up 30s after boot, and net_up() suspends BLE to
     * make room for it. Since net_down() does not yet deinit the WiFi driver,
     * BLE never resumes — so the board advertises for ~28 seconds and then goes
     * silent forever, which is far too narrow a window to catch with a phone.
     * Leaving telemetry off keeps the advert on the air indefinitely. */
#if !BLE_ONAIR_TEST
    telemetry_init();
#else
    applog("[main] ON-AIR TEST BUILD: telemetry off, BLE stays up");
#endif

    /* 🚨 A CARD IS NOT REQUIRED TO BOOT, AND USED TO BE.
     *
     * Both branches here used to `return` out of app_main, so a board with no
     * card or no music stopped dead at a boot screen — BEFORE player_init(),
     * which is what calls audio_init(). That made the codec, the amp, the
     * walkie and Comms all untestable on any board without a music card, which
     * is exactly the situation when bringing up NEW HARDWARE: the third S3
     * (2026-08-18) has an amp being replaced and a card full of Retro-Go, and
     * the one thing needed was to hear the new amp.
     *
     * Music is the only feature that actually needs the card. Everything else —
     * walkie, Comms, radio (which already falls back to 8 built-in stations),
     * every earcon — works without it. So log it and carry on. */
    boot_screen("mounting card");
    bool have_card = (library_mount() == ESP_OK);
    bool have_music = false;
    if (!have_card) {
        applog("[main] NO SD CARD - music off, everything else runs");
    } else {
        boot_screen("reading library");
        have_music = (library_scan(scan_progress) == ESP_OK);
        if (!have_music) applog("[main] card has no music - music off");
        /* AFTER the scan: favourites are stored as paths and resolved against
         * whatever this card actually holds — see faves.h. */
        if (have_music) faves_load();
    }

    player_init();
    /* Only with a library behind it: restoring a stored album index into an
     * empty library indexes an array that was never allocated. */
    if (have_music) player_restore();

    /* 🔔 THE ALARM. Woken by the timer rather than a finger, so start the music
     * the alarm exists for. Needs no network at all - the music is on the card,
     * which is the whole reason this is a usable alarm clock. */
    if (have_music && power_woke_for_alarm()) {
        applog("[power] alarm wake - starting music");
        display_note_activity();
        player_play(player_state()->album >= 0 ? player_state()->album : 0, 1);
    }
    /* Before ui_init(): the radio screen builds one row per station, so the
     * list has to be loaded from the card first. */
    radio_init();
    voice_init();
    wt_init();
    /* Before ui_init(): the keypad asks the dictionary for candidates as soon
     * as it is built. ⚠️ A failure here is NOT fatal - the keypad falls back to
     * showing raw digits, which is why this logs and carries on rather than
     * refusing to boot a handset over a missing word list. */
    if (!t9_begin()) applog("[t9] no dictionary: %s", t9_error());
    ui_init();

    /* 🚨 ESP-NOW HOLDS THE WIFI DRIVER UP FOR GOOD. That is the whole point —
     * a handset that only listens during its 60s telemetry window is not a
     * messaging device — but it means the driver's internal RAM never comes
     * back, so this runs LAST, after LVGL, the player and the radio have taken
     * theirs. If it fails here it would have failed anyway, and the handset
     * carries on without peer-to-peer messaging. */
    fleet_init();          /* roster and PMK before any peer is added */
    /* 🔎 IDF 6.0 CRYPTO PORT — OFF BY DEFAULT, AND IT MUST STAY OFF.
     *
     * 🚨 MEASURED 2026-09-19 ON THE TEST HANDSET: THIS COSTS ~14 SECONDS OF BOOT.
     * `[fleet]` printed at 17.4s and `[kdf]` at 31.6s. FLEET_KDF_ITERS is
     * 100,000, and one PBKDF2-SHA256 pass at that count is genuinely that slow
     * here — which also tells us what entering a fleet code costs the user.
     * Never ship it enabled; turn it on only to re-run the comparison. */
#if FLEET_KDF_SELFTEST
    fleet_kdf_selftest();
#endif
    if (!espnow_begin()) applog("[now] not running");




    /* LAST, ON PURPOSE. Everything else has already taken its internal RAM by
     * this point — LVGL, the player, the radio — so if BLE can start here it
     * can start in the real firmware. Starting it early would prove nothing and
     * would let it take memory the audio path needs. */
    /* Off unless asked for — see the note on .ble_on in settings.c. */
    /* Off unless asked for — see the note on .ble_on in settings.c. */
    /* 🚨 BLE AND A PERMANENTLY-RESIDENT WIFI DRIVER DO NOT COEXIST ON THIS
     * BOARD - AND IT IS NOT A MEMORY SQUEEZE, IT IS A CONTROLLER ASSERT.
     *
     * Measured 2026-08-21 on Kid 2, which had BLE switched on:
     *
     *     [now] up as Kid 2 on channel 6
     *     BLE assert emi.c 164, param 00000000 00008400
     *     Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)
     *     rst:0xc (RTC_SW_CPU_RST)
     *
     * ...on a 3.5-second loop. The old design dodged this by construction:
     * net_up() suspended BLE and net_down() resumed it, so the two radios took
     * turns and were never resident together. ESP-NOW holds the driver up for
     * good, so the turn-taking is gone and they meet.
     *
     * 🔑 ESP-NOW WINS, because BLE has no consumer left: its only one was the
     * Talkie screen's CALL button (removed 2026-08-21) and it is off by default.
     * Said loudly rather than silently, or "I turned BLE on and nothing
     * happened" becomes the next mystery. */
    /* 🔑 BLE IS NOT STARTED AT ALL ANY MORE (2026-09-14). Its last consumer
     * went with the Talkie CALL button, the setting row went out of Settings
     * with it, and a stored "on" from months ago would otherwise still bring
     * the controller up on some handsets and not others. ble.c stays in the
     * tree; nothing calls it. */
    else applog("[ble] disabled in settings (frees ~31KB for WiFi)");

#if NET_CYCLE_TEST
    xTaskCreatePinnedToCore(net_cycle_test, "cycle", 4096, NULL, 3, NULL, 0);
#endif

    ESP_LOGI(TAG, "ready. internal free %u, PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* LVGL is not thread-safe; everything that touches it runs from here.
     *
     * 🔑 THE TICK IS INSTRUMENTED, AND THE INSTRUMENT CAN SAY I AM WRONG.
     *
     * The theory under the unresponsive Back button is that one long callback
     * (map_draw() walking up to nine SD-backed tiles) keeps lv_timer_handler()
     * busy long enough that the indev timer never runs, so the tap is never
     * sampled and is lost outright. Changing the event type cannot fix that,
     * and neither can a faster UI tick — LVGL 8.4 sends button events from the
     * indev timer, not from our tick.
     *
     * So measure the two things the theory actually claims:
     *   - how long a single lv_timer_handler() call took, and
     *   - whether touch was polled even once while it ran.
     *
     * A long tick that still polled touch is harmless. A long tick with ZERO
     * polls is a window in which a real tap would have been dropped, and
     * `starved` counts them. If the builder reports a missed Back press while
     * `starved` stays 0, the redraw is not the cause and the theory is dead. */
    uint32_t worst_ms = 0, starved = 0;
    int64_t  last_report_us = esp_timer_get_time();
    while (1) {
        uint32_t polls0 = display_indev_polls();
        int64_t  t0     = esp_timer_get_time();

        uint32_t next = lv_timer_handler();

        uint32_t took = (uint32_t)((esp_timer_get_time() - t0) / 1000);
        uint32_t polled = display_indev_polls() - polls0;

        /* LV_INDEV_DEF_READ_PERIOD is 30ms, so a tick longer than that had the
         * opportunity to poll and is only interesting when it did not. */
        if (took > 30 && polled == 0) {
            starved++;
            applog("[lvgl] tick %ums, touch NOT polled (starved %u)",
                   (unsigned)took, (unsigned)starved);
        }
        if (took > worst_ms) worst_ms = took;

        /* One line a minute, and only when something was slow: a quiet log is
         * itself the result. */
        if (esp_timer_get_time() - last_report_us > 60 * 1000000LL) {
            last_report_us = esp_timer_get_time();
            if (worst_ms > 30)
                applog("[lvgl] worst tick %ums in the last minute, starved %u",
                       (unsigned)worst_ms, (unsigned)starved);
            worst_ms = 0;
        }

        if (next > 30) next = 30;
        vTaskDelay(pdMS_TO_TICKS(next ? next : 5));
    }
}
