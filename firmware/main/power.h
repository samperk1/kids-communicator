#pragma once
#include <stdbool.h>

/* Deep sleep and the alarm.
 *
 * Wake path verified on hardware before this was written: touch on the FT6236
 * INT (GPIO17, RTC-capable) wakes reliably, so no power button is needed.
 *
 * 🚨 power_tick() will NOT sleep while USB is connected. That is deliberate and
 * load-bearing: a sleeping board has no /dev/ttyACM* at all and cannot be
 * flashed. Do not "fix" it. */

void power_init(void);        /* read the wake cause; call before anything else */
void power_tick(void);        /* from the UI tick; decides whether to sleep */
void power_sleep_now(void);   /* immediate, never returns */

/* True when this boot was caused by the alarm timer rather than a finger. */
bool power_woke_for_alarm(void);

/* SNTP. Deep sleep takes a duration, not a wall-clock time, so an alarm needs
 * the real time first - and is only as accurate as the last sync. Safe to call
 * repeatedly; does nothing unless the link is up. */
void power_sync_time(void);

/* Set the clock from an HTTP `Date:` header ("Sun, 23 Aug 2026 14:25:30 GMT").
 * ha.c calls this for every response; it is a no-op once the clock is valid. */
void power_time_from_http_date(const char *date);
bool power_time_valid(void);

/* Set the clock from UTC seconds. The gateway path — see gw_wire.h. First
 * source to answer wins; a later one is ignored rather than stepping the clock. */
void power_time_set_unix(int64_t secs);
