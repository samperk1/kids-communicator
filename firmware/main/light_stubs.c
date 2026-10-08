/*
 * COMM_LIGHT — honest failures in place of net.c, ha.c, voice.c, radio.c and
 * telemetry.c.
 *
 * WHY A STUB FILE AND NOT ~110 #ifdefs AT THE CALL SITES:
 * The plan originally called for guarding every UI call site. Measured, that is
 * ~110 sites in ui.c alone (radio_ 38, msg_ 49, voice_ 10, net_ 13), and every
 * one of them is a chance to get a guard subtly wrong in a file nobody wants to
 * re-audit. The API surface of the five modules is only 41 functions, so
 * replacing the MODULES is an order of magnitude less code and — the part that
 * matters — it is verifiable by the linker instead of by reading.
 *
 * 🔑 The security property comes from the modules being ABSENT FROM THE BUILD,
 * not from these stubs. WIFI_SSID, WIFI_PASS and HA_TOKEN are
 * referenced only by net.c and ha.c; neither is compiled here, so the strings
 * are never in the image to be found by `esptool read_flash`. Nothing in this
 * file includes wifi_creds.h or ha_creds.h, and it must stay that way.
 *
 * Every stub fails the way a real one fails when the house is out of reach —
 * that path is already exercised and handled, because the handsets have always
 * had to work away from home.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "applog.h"

#include "net.h"
#include "ha.h"
#include "voice.h"
#include "radio.h"
#include "telemetry.h"

/* ---- net: no ASSOCIATION, but the radio still has to come up ----
 *
 * 🚨 THIS IS THE TRAP THAT BROKE THE FIRST LIGHT BUILD, CAUGHT ON HARDWARE
 * 2026-09-09: stubbing net_start_radio() to false killed ESP-NOW outright —
 * `[now] the wifi driver would not start` — and with it text, the walkie,
 * games AND the gateway path. Everything the kids actually use.
 *
 * 🔑 ESP-NOW needs the WiFi DRIVER STARTED. It does not need an SSID, a
 * password, an association, a netif or lwIP. Those are two different things,
 * and only the second one carries credentials. So this brings the driver up
 * exactly as far as ESP-NOW requires and no further — which is also why the
 * light build never associates with anything, even by accident: there is no
 * esp_wifi_set_config() call here to associate WITH. */
static bool s_radio_started;

void net_init(void)          { }
bool net_up(void)            { return false; }   /* no association, ever */
void net_retry_reset(void)   { }
void net_down(void)          { }                 /* the radio stays up for ESP-NOW */
void net_deinit(void)        { }
int  net_rssi(void)          { return 0; }       /* AP signal; there is no AP */
void net_hold(void)          { }
void net_release(void)       { }
bool net_is_up(void)         { return false; }
const char *net_ip(void)     { return ""; }
void net_radio_hold(void)    { }
void net_radio_release(void) { }
bool net_radio_held(void)    { return true; }    /* nothing may stop it */

bool net_start_radio(void)
{
    if (s_radio_started) return true;

    applog("[wifi] driver up for ESP-NOW only, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* Both are idempotent-ish: they return an "already done" error that is
     * success here, exactly as net.c treats esp_wifi_start(). */
    esp_netif_init();
    esp_event_loop_create_default();

    /* ⚠️ Deliberately NO esp_netif_create_default_wifi_sta(). That is the lwIP
     * side, which ESP-NOW never touches, and skipping it keeps its internal
     * RAM — the scarcest thing on this board. */

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e != ESP_OK) {
        applog("[wifi] esp_wifi_init FAILED: %s", esp_err_to_name(e));
        return false;
    }

    /* RAM-backed: no NVS calibration writes, same reasoning as net.c. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    e = esp_wifi_start();
    if (e != ESP_OK && e != ESP_ERR_WIFI_CONN) {
        applog("[wifi] esp_wifi_start FAILED: %s", esp_err_to_name(e));
        return false;
    }

    /* Power save off — a dozing radio misses ESP-NOW frames it is not expecting,
     * and there is no association here to negotiate a listen interval with. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    /* Max TX power, read back and LOGGED — same reasoning as net.c, and it
     * matters more here: Kid 4 measures -83..-89 dBm from everything. Only
     * valid once the driver is STARTED, which is why it is down here. */
    esp_wifi_set_max_tx_power(84);
    int8_t pwr = 0;
    if (esp_wifi_get_max_tx_power(&pwr) == ESP_OK)
        applog("[wifi] tx power %d (%.1f dBm)", pwr, pwr / 4.0);

    s_radio_started = true;
    return true;
}

/* ---- ha: no radio to Home Assistant at all, so every request fails here ----
 *
 * msg.c is RETAINED and calls these for its HA broadcast fallback. Failing
 * here is correct and already handled: a private message never falls back to
 * HA anyway (it would hand a message meant for one sibling to all of them),
 * and a broadcast simply stays on ESP-NOW.
 *
 * ⚠️ A STUB GATES MORE THAN THE FUNCTION IT REPLACES — three features once
 * broke in one evening because net_* stubs returned failure. ha_post_webhook()
 * is safe to fail: its two callers (telemetry.c, msg.c) both treat a false as
 * "HA did not hear us" and carry on, and the light build has no WiFi to reach
 * HA with in the first place. It is NOT a credential gate — a webhook carries
 * no credential — so nothing here is load-bearing for the light build's
 * security property. */
bool ha_post_json(const char *path, const char *body)       { (void)path; (void)body; return false; }
bool ha_post_webhook(const char *hook, const char *body)    { (void)hook; (void)body; return false; }
bool ha_get_binary(const char *path)                        { (void)path; return false; }
bool ha_get_json(const char *path, bool admin)              { (void)path; (void)admin; return false; }
int  ha_status(void)                                        { return 0; }
const char *ha_body(void)                                   { return ""; }
void ha_free_binary(void)                                   { }

uint8_t *ha_take_binary(size_t *len)
{
    if (len) *len = 0;
    return NULL;
}

bool ha_json_str(const char *json, const char *key, char *out, size_t len)
{
    (void)json; (void)key;
    if (out && len) out[0] = '\0';
    return false;
}

/* ⚠️ Must still NUL-terminate. msg.c escapes into a buffer and only then
 * discovers the POST fails; leaving `out` untouched would hand it a stack
 * buffer full of garbage to log. */
void ha_json_escape(const char *in, char *out, size_t len)
{
    (void)in;
    if (out && len) out[0] = '\0';
}

/* ---- voice: NOT stubbed. voice.c is compiled in this build; only its
 * direct-to-HA legs are #if'd out. See the note in main/CMakeLists.txt. ---- */

/* ---- radio: internet radio needs the house AP ---- */
void        radio_init(void)           { }
void        radio_load_stations(void)  { }
int         radio_count(void)          { return 0; }
const char *radio_name(int i)          { (void)i; return ""; }
int         radio_current(void)        { return -1; }
bool        radio_playing(void)        { return false; }
const char *radio_status(void)         { return ""; }
bool        radio_play(int idx)        { (void)idx; return false; }
void        radio_stop(void)           { }

/* ---- telemetry: reported battery to HA using the ADMIN token ---- */
void telemetry_init(void) { }
