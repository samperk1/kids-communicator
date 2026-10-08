/*
 * BLE signalling — the RAM gate.
 *
 * WHY THIS EXISTS: the walkie-talkie needs a way for one handset to tell the
 * other "switch over, I want to talk", and the two handsets are not always on
 * the same WiFi. The builder's idea, 2026-08-17: BLE can't carry the audio, but it can
 * carry a chirp for almost no power.
 *
 * 🔑 THE POWER SHAPE, WHICH DECIDES THE WHOLE DESIGN:
 * ADVERTISING IS CHEAP, SCANNING IS NOT. A continuous BLE scan runs the radio
 * flat out at ~50-60mA — the same order as WiFi, which would eat the battery
 * life this feature is supposed to protect. A duty-cycled scan does not: the
 * CONTROLLER does the duty cycling in hardware, so a 40ms window every 1000ms
 * costs about 4% of that. So: broadcast bursts + a duty-cycled listen, never a
 * held connection and never a continuous scan.
 *
 * 🔑 WHY THE PAYLOAD IS IN THE ADVERT AND THE TEXT WILL NOT BE.
 * An advert carries 31 bytes total, ~26 usable. That is plenty for "handset
 * xxxx is calling you", and hopeless for a text message — splitting a message
 * across adverts means hand-rolling ordering, retransmit and dedupe. The split
 * is: ADVERT = DOORBELL, GATT CONNECTION = PAYLOAD. This file is the doorbell.
 *
 * ⚠️ THIS BUILD IS A MEASUREMENT, NOT A FEATURE. Every stage logs internal-RAM
 * headroom, because internal RAM — not flash, not PSRAM — is what has broken
 * every network feature on this board so far. See ble_init().
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_log.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include <stdio.h>
#include "applog.h"
#include "ble.h"

/* Mirrors the flag in main.c; test builds advertise a name and skip telemetry. */
#ifndef BLE_ONAIR_TEST
#define BLE_ONAIR_TEST 1
#endif

static const char *TAG = "ble";

/* Advert/scan intervals are in 0.625ms units. Spelling the conversion out
 * beats using a macro from a header whose name changes between IDF releases. */
#define UNITS(ms)          ((ms) * 8 / 5)

#define ADV_ITVL_IDLE_MS   1000    /* standby presence beacon */
#define ADV_ITVL_CHIRP_MS  100     /* a call: fast, so a 4%-duty scanner sees it */
#define CHIRP_MS           8000    /* ...and long enough that it cannot be missed */

/* 🔑 THE SCAN DUTY CYCLE IS THE POWER BUDGET, AND IT SETS THE LATENCY.
 * A scanner listening 40ms in every 1000ms hears a 100ms-interval burst within
 * about a second, and a burst that lasts CHIRP_MS is caught with enormous
 * margin. Widening the window buys latency nobody can feel and costs current
 * linearly — do not widen it without a measurement that justifies it. */
#define SCAN_ITVL_MS       1000
#define SCAN_WIN_MS        40

/* 0xFFFF is the Bluetooth SIG's company ID reserved for testing and internal
 * use, which is exactly what this is. Anything else would be squatting on a
 * real vendor's ID. */
#define COMPANY_ID         0xFFFF
#define PROTO_VER          1

enum { MSG_PRESENCE = 1, MSG_CHIRP = 2 };

/* 8 bytes, wire order:
 *   0-1 company id (LE)   2 proto ver   3 msg type
 *   4-5 sender id         6 seq         7 target (0xFF = everyone) */
#define PAYLOAD_LEN        8
#define TARGET_ALL         0xFF

static uint8_t s_id[2];              /* mac[4], mac[5] — matches comm_XXXX */
static uint8_t s_own_addr_type;
static uint8_t s_seq;
static bool    s_ready;
static char    s_chirp_from[16];   /* sender of a chirp the UI has not shown yet */
static bool    s_chirp_pending;
static bool    s_started;      /* nimble port is initialised */
static bool    s_want_ble;     /* ble_init() succeeded once; resume is allowed */
static esp_timer_handle_t s_chirp_timer;

static int gap_event(struct ble_gap_event *ev, void *arg);

static void heap_log(const char *stage)
{
    applog("[ble] %-22s internal free %u, largest block %u", stage,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

static void set_adv_payload(uint8_t msg_type)
{
    uint8_t p[PAYLOAD_LEN] = {
        COMPANY_ID & 0xFF, COMPANY_ID >> 8,
        PROTO_VER, msg_type,
        s_id[0], s_id[1],
        s_seq, TARGET_ALL,
    };

    struct ble_hs_adv_fields f = { 0 };
    f.mfg_data     = p;
    f.mfg_data_len = sizeof(p);

#if BLE_ONAIR_TEST
    /* 🔎 NAME IT, SO THE DUMBEST SCANNER CAN SEE IT.
     * A phone's Bluetooth SETTINGS screen lists connectable, named devices —
     * it does not show bare beacons, so "my phone didn't see it" is not
     * evidence of a transmit fault until the advert is something a phone would
     * display in the first place. Test builds only; the shipping beacon carries
     * no name, because 9 bytes of "COMM-XXXX" buys nothing over the sender id
     * already in the payload. */
    static char name[10];
    snprintf(name, sizeof(name), "COMM-%02X%02X", s_id[0], s_id[1]);
    f.name             = (uint8_t *)name;
    f.name_len         = strlen(name);
    f.name_is_complete = 1;
#endif

    int rc = ble_gap_adv_set_fields(&f);
    if (rc) applog("[ble] adv_set_fields rc=%d", rc);
}

static void adv_start(uint8_t msg_type, int itvl_ms)
{
    ble_gap_adv_stop();
    set_adv_payload(msg_type);

    /* Non-connectable: this is a broadcast, and there is nothing here for a
     * phone to connect to. It also keeps the controller from holding connection
     * state it will never use.
     *
     * 🚨 BUT DISCOVERABLE — DISC_MODE_NON MAKES THE BEACON INVISIBLE TO
     * ORDINARY SCANNERS.
     *
     * With DISC_MODE_NON, NimBLE emits no Flags AD field at all, and BlueZ
     * simply does not report such adverts through `bluetoothctl scan on` — the
     * board transmits perfectly and nothing sees it. Measured 2026-08-17: the
     * firmware logged "advertising + scanning" with no error while a scanner
     * 30cm away listed four other devices and not this one. That failure looks
     * EXACTLY like a dead antenna, and on this project that is a diagnosis
     * worth being very slow to reach — the WiFi on this same board had
     * associated and POSTed to HA minutes earlier, so the radio was provably
     * fine.
     *
     * GEN costs 3 bytes of the 31 and there is ample room. */
    struct ble_gap_adv_params ap = { 0 };
#if BLE_ONAIR_TEST
    /* 🚨 CONNECTABLE ONLY BECAUSE ORDINARY SCANNERS REFUSE TO SHOW ANYTHING
     * ELSE. Proven on hardware 2026-08-17: with CONN_MODE_NON the board
     * advertises correctly (host logs the procedure, no error) and BlueZ lists
     * nothing; the identical build with CONN_MODE_UND appears instantly as
     * XX:XX:XX:XX:XX:XX. The shipping beacon stays NON — board-to-board works
     * either way, because NimBLE's scanner reports every advert it hears. */
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
#else
    ap.conn_mode = BLE_GAP_CONN_MODE_NON;
#endif
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ap.itvl_min  = UNITS(itvl_ms);
    ap.itvl_max  = UNITS(itvl_ms + itvl_ms / 5);

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &ap,
                               gap_event, NULL);
    if (rc) applog("[ble] adv_start rc=%d", rc);
}

static void scan_start(void)
{
    /* ⚠️ filter_duplicates OFF on purpose. The controller filters by identical
     * payload, and a chirp IS a repeated identical payload — with filtering on,
     * a receiver that missed the first burst would never be told again.
     * Duplicate suppression belongs in software, keyed on (sender, seq). */
    struct ble_gap_disc_params dp = { 0 };
    dp.itvl              = UNITS(SCAN_ITVL_MS);
    dp.window            = UNITS(SCAN_WIN_MS);
    dp.passive           = 1;      /* never scan-request: TX costs power */
    dp.filter_duplicates = 0;

    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &dp, gap_event, NULL);
    if (rc) applog("[ble] disc rc=%d", rc);
}

/* Pull our 8-byte payload out of an advert, or return NULL. Walks the AD
 * structures by hand rather than using ble_hs_adv_parse_fields(), which wants a
 * 100+ byte struct on the stack for one field. */
static const uint8_t *payload_of(const uint8_t *data, uint8_t len)
{
    for (uint8_t i = 0; i + 1 < len; ) {
        uint8_t flen = data[i];
        if (flen == 0 || i + 1 + flen > len) break;
        if (data[i + 1] == BLE_HS_ADV_TYPE_MFG_DATA && flen == 1 + PAYLOAD_LEN) {
            const uint8_t *p = &data[i + 2];
            if (p[0] == (COMPANY_ID & 0xFF) && p[1] == (COMPANY_ID >> 8) &&
                p[2] == PROTO_VER)
                return p;
        }
        i += 1 + flen;
    }
    return NULL;
}

static void heard(const uint8_t *p, int rssi)
{
    /* Dedupe: an advert repeats every ADV_ITVL_CHIRP_MS for CHIRP_MS, so a
     * single call arrives dozens of times. One chirp per (sender, seq). */
    static uint8_t last_id[2], last_seq;
    static bool    have_last;
    if (have_last && last_id[0] == p[4] && last_id[1] == p[5] && last_seq == p[6])
        return;
    last_id[0] = p[4]; last_id[1] = p[5]; last_seq = p[6];
    have_last  = true;

    applog("[ble] heard %s from comm_%02x%02x  rssi %d",
           p[3] == MSG_CHIRP ? "CHIRP" : "presence", p[4], p[5], rssi);

    /* 🚨 HANDED OVER AS A FLAG, NEVER DRAWN FROM HERE. This runs on the NimBLE
     * host task and LVGL is not thread-safe — the same rule msg.c follows. */
    if (p[3] == MSG_CHIRP) {
        snprintf(s_chirp_from, sizeof(s_chirp_from), "comm_%02x%02x", p[4], p[5]);
        s_chirp_pending = true;
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    if (ev->type == BLE_GAP_EVENT_DISC) {
        /* 🔎 RX PROOF. "Nothing can hear us" and "the BLE radio is dead" look
         * identical from the outside, and this project has burned days on
         * exactly that ambiguity before. Counting every advert the board hears
         * separates them in one boot: adverts arriving means the radio, the
         * antenna and the scanner all work, and the fault is on the TX side
         * alone. Rate-limited so a busy 2.4GHz band cannot flood the log. */
        static int64_t last_rx_log;
        static unsigned rx_count;
        rx_count++;
        int64_t now = esp_timer_get_time();
        if (now - last_rx_log > 5000000) {
            last_rx_log = now;
            applog("[ble] RX: %u adverts heard, latest %02x:%02x:%02x:%02x:%02x:%02x rssi %d",
                   rx_count, ev->disc.addr.val[5], ev->disc.addr.val[4],
                   ev->disc.addr.val[3], ev->disc.addr.val[2],
                   ev->disc.addr.val[1], ev->disc.addr.val[0], ev->disc.rssi);
        }

        /* Ignore our own broadcast — the scanner hears the adverts this same
         * board is transmitting. */
        const uint8_t *p = payload_of(ev->disc.data, ev->disc.length_data);
        if (p && !(p[4] == s_id[0] && p[5] == s_id[1])) heard(p, ev->disc.rssi);
    } else if (ev->type == BLE_GAP_EVENT_DISC_COMPLETE) {
        scan_start();                 /* BLE_HS_FOREVER should not end, but if
                                       * it ever does, a deaf handset is worse
                                       * than a restarted scan. */
    }
    return 0;
}

static void chirp_done(void *arg)
{
    adv_start(MSG_PRESENCE, ADV_ITVL_IDLE_MS);
    applog("[ble] chirp over, back to presence");
}

bool ble_take_chirp(char *id_out, size_t len)
{
    if (!s_chirp_pending) return false;
    s_chirp_pending = false;
    strlcpy(id_out, s_chirp_from, len);
    return true;
}

bool ble_chirp(void)
{
    if (!s_ready) return false;
    s_seq++;
    adv_start(MSG_CHIRP, ADV_ITVL_CHIRP_MS);
    esp_timer_stop(s_chirp_timer);
    esp_timer_start_once(s_chirp_timer, (uint64_t)CHIRP_MS * 1000);
    applog("[ble] chirping seq %u for %dms", (unsigned)s_seq, CHIRP_MS);
    return true;
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    if (ble_hs_id_infer_auto(0, &s_own_addr_type)) {
        applog("[ble] no usable address, giving up");
        return;
    }
    adv_start(MSG_PRESENCE, ADV_ITVL_IDLE_MS);
    scan_start();
    s_ready = true;
    heap_log("advertising + scanning");
}

static void on_reset(int reason)
{
    applog("[ble] controller reset, reason %d", reason);
    s_ready = false;
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* 📊 A STEADY-STATE NUMBER, NOT JUST A BOOT NUMBER.
 * Headroom right after init is the easy case. What actually broke Talk on
 * 2026-08-16 was headroom while the audio path was open and WiFi was up, which
 * only shows up minutes into a session. */
static void watch_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        heap_log("steady state");
    }
}

/* 🚨 THE MEASUREMENT THAT DECIDED THE ARCHITECTURE (board #1, 2026-08-17).
 *
 * Internal free, measured at ble_init() with everything else already up:
 *
 *   NimBLE host in INTERNAL RAM   64815 -> 7463   (cost 57.4KB) then ABORTED
 *   NimBLE host in PSRAM          64815 -> 33959  (cost 30.1KB)
 *   ...plus .bss of bt/lwip/pp    78915 -> 48059  (cost 30.9KB, floor +14.1KB)
 *
 * So PSRAM pays for the HOST and nothing else: the ~31KB that remains is the
 * CONTROLLER, whose buffers are DMA-bound and internal-only exactly like WiFi's
 * static RX/TX. On this board WiFi then fails esp_wifi_init() with
 * ESP_ERR_NO_MEM at 29023 bytes free — measured, twice.
 *
 * 🔑 THE TWO RADIOS THEREFORE TAKE TURNS, AND THAT IS NOT A COMPROMISE:
 * BLE's whole job is to reach a handset that is IDLE, and WiFi's whole job is
 * the active features. When WiFi is up the handset is reachable over WiFi
 * anyway, so the coverage gap is empty — each path covers exactly what the
 * other cannot. */
void ble_suspend(void)
{
    if (!s_started) return;
    ble_gap_adv_stop();
    ble_gap_disc_cancel();
    s_ready = false;

    /* Order matters: stop the host's run loop, wait for the host task to fall
     * out of nimble_port_run(), and only then tear the port down. */
    int rc = nimble_port_stop();
    if (rc) { applog("[ble] nimble_port_stop rc=%d — staying up", rc); return; }
    nimble_port_deinit();
    s_started = false;
    heap_log("suspended for wifi");
}

/* 🚨 NEVER RESUME INTO A HEAP THAT CANNOT HOLD THE CONTROLLER.
 *
 * nimble_port_init() needs ~31KB of internal RAM, and when it cannot get it the
 * controller does NOT fail politely — it asserts inside its own init and takes
 * the board down with it:
 *
 *   [radio] starting, internal free 19611
 *   wifi:malloc buffer fail
 *   BLE_INIT: Malloc failed
 *   BLE assert emi.c 164
 *   Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0)
 *
 * The builder, 2026-08-17: "it did reboot when i presed radio". Internet radio holds
 * WiFi up AND takes a stream buffer, so the memory simply is not there. The
 * return value cannot save us because the assert fires first — the only defence
 * is to not call it. Checked against the measured cost with margin; below this
 * the answer is "stay off and try again after the radio closes", which costs
 * nothing because a handset streaming radio is reachable over WiFi anyway. */
#define BLE_RESUME_MIN_FREE  45000

void ble_resume(void)
{
    if (s_started || !s_want_ble) return;

    unsigned freei = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (freei < BLE_RESUME_MIN_FREE) {
        applog("[ble] not resuming, only %u internal free (need %u)",
               freei, (unsigned)BLE_RESUME_MIN_FREE);
        return;
    }

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        applog("[ble] resume failed: %s", esp_err_to_name(err));
        return;
    }
    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gap_device_name_set("comm");
    nimble_port_freertos_init(host_task);
    s_started = true;
    heap_log("resumed after wifi");
}

void ble_init(void)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    s_id[0] = mac[4];
    s_id[1] = mac[5];

    heap_log("before nimble_port_init");

    /* Brings up the controller AND the host. On a failure the board must carry
     * on without BLE rather than reset — a handset that cannot chirp is a
     * working handset; one stuck in a boot loop is not. */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        applog("[ble] nimble_port_init FAILED: %s — continuing without BLE",
               esp_err_to_name(err));
        heap_log("after failed init");
        return;
    }
    heap_log("after nimble_port_init");

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gap_device_name_set("comm");

    const esp_timer_create_args_t targs = {
        .callback = chirp_done, .name = "chirp"
    };
    ESP_ERROR_CHECK(esp_timer_create(&targs, &s_chirp_timer));

    nimble_port_freertos_init(host_task);
    s_started  = true;
    s_want_ble = true;
    heap_log("after host task start");

    xTaskCreatePinnedToCore(watch_task, "blewatch", 2560, NULL, 2, NULL, 0);

    ESP_LOGI(TAG, "BLE up as comm_%02x%02x", s_id[0], s_id[1]);
}
