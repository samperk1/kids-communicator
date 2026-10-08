#pragma once
#include <stdbool.h>

/* WiFi, brought up only when it is needed.
 *
 * ⚠️ Unit #1's association was intermittently failing and it was diagnosed as
 * memory. It was NOT — one failure was measured with 83KB free. On that board
 * the cause was one antenna time-shared with a live A2DP link. There is no
 * Bluetooth here at all, so that whole class of failure should be gone; if
 * association still takes 3-8s, that is DHCP, not a fault.
 *
 * The retry shape is kept anyway because it costs nothing: two bounded
 * attempts beat one long one, since anything still down at 12s is stuck rather
 * than slow and a fresh connect clears it. */

/* Create the radio mutex. Must be called from app_main before any task that
 * could bring WiFi up exists — creating it lazily would itself be a race. */
void net_init(void);

bool net_up(void);          /* blocking, ~2-8s. true = got an IP */

/* 🚨 AWAY FROM HOME, TRYING IS WORSE THAN NOT TRYING.
 *
 * A failed net_up() sweeps every channel (WIFI_ALL_CHANNEL_SCAN) three times
 * and then scans once more to log the signal — up to 45 SECONDS during which
 * the radio is anywhere but channel 6 and ESP-NOW hears nothing. Telemetry
 * retries every 30s, so out of range of the house AP the handsets spent
 * essentially ALL of their time deaf. That is why Kid 1 and Kid 2 never saw
 * each other at the builder's work on 2026-08-24.
 *
 * So net_up() now refuses to retry inside a growing backoff window and
 * returns false immediately, leaving the radio on the ESP-NOW channel.
 * net_retry_reset() clears it: the two USER-initiated paths (Talkie, Radio)
 * call it so a deliberate press always gets a real attempt. */
void net_retry_reset(void);
void net_down(void);        /* releases the radio AND its memory, resumes BLE */

/* Tear the driver and its netif all the way down, returning ~30KB of internal
 * RAM. net_down() calls this; it is exposed for tests that cycle the radio. */
void net_deinit(void);

/* Keep the link up across someone else's net_up()/net_down() pair. The Radio
 * screen holds one for as long as it is open, so a Talk round trip inside it
 * cannot pull the radio out from under it. Always pair them. */
int  net_rssi(void);        /* AP signal in dBm, 0 = unknown/down */
void net_hold(void);
void net_release(void);
bool net_is_up(void);
const char *net_ip(void);

/* --- keeping the radio alive for ESP-NOW ------------------------------------
 *
 * 🚨 ESP-NOW receives nothing while the driver is deinitialised, and net_down()
 * deinitialises it by default. A hold keeps the driver STARTED across
 * net_down(): the association still drops, the ~30KB of driver memory does not
 * come back. Take one for as long as ESP-NOW must be able to hear anything.
 */
/* Bring the driver up without associating: ESP-NOW needs a started radio and
 * nothing else. Safe to call when it is already running. */
bool net_start_radio(void);

void net_radio_hold(void);
void net_radio_release(void);
bool net_radio_held(void);
