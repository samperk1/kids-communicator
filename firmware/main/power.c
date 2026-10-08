/*
 * Deep sleep, and the alarm that wakes it.
 *
 * The wake path was verified on hardware before any of this was written
 * (a sleep test rig, 2026-08-16): 4 wakes out of 4 reported EXT0 from the
 * FT6236 touch INT on GPIO17, and GPIO17 is RTC-capable. The FT6236 keeps
 * asserting INT in its own low-power mode, so deep sleep needs NO hardware mod
 * and no power button. GPIO18 remains free if a button is ever wanted.
 *
 * 🚨 THE FOUR RULES, LEARNED THE HARD WAY THE SAME DAY:
 *
 * 1. NEVER AUTO-SLEEP WHILE USB IS CONNECTED. In deep sleep the USB peripheral
 *    is off and /dev/ttyACM* DOES NOT EXIST - esptool cannot see the board,
 *    cannot reset it, cannot do anything. A board that sleeps on the bench is a
 *    board you cannot flash. The builder predicted this and it happened within minutes.
 * 2. ALWAYS LEAVE A GRACE PERIOD AT BOOT, so a freshly flashed board cannot
 *    sleep before a human can react.
 * 3. ALWAYS KEEP A WAKE SOURCE THAT CANNOT FAIL. The timer backstop in the test
 *    rig is the only reason the board stayed recoverable.
 * 4. NOTHING BLOCKING ON THE UI THREAD. power_tick() is called from the LVGL
 *    timer, so it decides and delegates; the shutdown itself is quick and the
 *    sleep call never returns.
 *
 * ⚠️ ANY touch wakes it - ext0 cannot filter by position. A capacitive screen in
 * a pocket will wake on contact, so the wake path checks how long we were asleep
 * and goes straight back down if the screen was not deliberately used. That is
 * cheap insurance against a flat battery in a bag.
 */

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "driver/rtc_io.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <sys/time.h>
#include "applog.h"
#include "hw.h"
#include "display.h"
#include "player.h"
#include "radio.h"
#include "net.h"
#include "battery.h"
#include "settings.h"
#include "power.h"
#include "gps.h"
#include "usbmsc.h"


#define BOOT_GRACE_US   (90 * 1000000)   /* rule 2 */

static bool s_woke_for_alarm;
static bool s_time_valid;

bool power_woke_for_alarm(void) { return s_woke_for_alarm; }
bool power_time_valid(void)     { return s_time_valid; }

void power_init(void)
{
    /* Read the cause BEFORE anything else touches it. A TIMER wake means the
     * alarm fired; EXT0 means a finger; UNDEFINED means a genuine cold boot. */
    esp_sleep_wakeup_cause_t c = esp_sleep_get_wakeup_cause();
    s_woke_for_alarm = (c == ESP_SLEEP_WAKEUP_TIMER);
    applog("[power] wake cause %s",
           c == ESP_SLEEP_WAKEUP_TIMER ? "TIMER (alarm)" :
           c == ESP_SLEEP_WAKEUP_EXT0  ? "touch" : "cold boot");
}

/* ---- clock ----
 *
 * Needed only by the alarm. Deep sleep takes a DURATION, not a wall-clock time,
 * so the board must know what time it is before it can work out how long to
 * sleep for - and an alarm is only ever as accurate as the last sync.
 *
 * ⚠️ Without an external 32.768kHz crystal the RTC runs on an internal RC
 * oscillator and can drift minutes per day. Re-syncing whenever the radio is up
 * is what keeps an overnight alarm honest. */
/* US Eastern. Deliberately the POSIX rule rather than a zone name: no tzdata on
 * the device, and the rule carries the DST transitions. Idempotent — both the
 * SNTP and the HTTP-Date path call it before trusting localtime_r(). */
static void set_local_tz(void)
{
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
}

/* Days since 1970-01-01 from a civil date. Howard Hinnant's algorithm, used
 * because newlib's timegm() is not guaranteed here and mktime() would apply the
 * local timezone to a value that is already UTC. */
static int64_t days_from_civil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

/* 🥇 THE CLOCK COMES FROM AN HTTP HEADER, AND THAT IS DELIBERATE.
 *
 * SNTP could not do it. The radio is only up for the 3-5 seconds of a telemetry
 * report, and measured on 2026-08-23: `pool.ntp.org` never synced in 9 windows
 * (DNS + an internet round trip does not fit), and neither did chrony on the LAN
 * at HA_HOST in 5 windows with a fresh request fired at each one.
 *
 * Meanwhile the board POSTs to HA over HTTP in that same window, every window,
 * and it has worked for months. Every HTTP response carries a `Date:` header
 * (RFC 9110 sec 6.6.1, GMT by definition). So the clock rides in on a request
 * that already succeeds: no DNS, no UDP, no server-side allow list, and not one
 * extra packet.
 *
 * ⚠️ Accuracy is ~1 second — the header has no sub-second field and there is no
 * round-trip correction. The alarm fires on hh:mm, so this is far more than it
 * needs. Do NOT reuse this for anything that wants real time discipline. */
void power_time_from_http_date(const char *date)
{
    static const char MONTHS[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (s_time_valid || date == NULL) return;

    char mon[4] = { 0 };
    int d, y, H, M, S;
    /* "Sun, 23 Aug 2026 14:25:30 GMT" — the day name is not needed. */
    if (sscanf(date, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &H, &M, &S) != 6)
        return;
    const char *pos = strstr(MONTHS, mon);
    if (pos == NULL) return;

    if (d < 1 || d > 31 || y < 2024 || H > 23 || M > 59 || S > 60) return;

    int64_t secs = days_from_civil(y, (unsigned)((pos - MONTHS) / 3 + 1), (unsigned)d) * 86400
                 + (int64_t)H * 3600 + (int64_t)M * 60 + S;

    struct timeval tv = { .tv_sec = (time_t)secs, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return;

    set_local_tz();
    s_time_valid = true;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    applog("[power] clock set from HTTP Date: %02d:%02d local", tm.tm_hour, tm.tm_min);
}

/* Set the clock from UTC seconds handed over by the gateway.
 *
 * 🔑 THE HANDSETS WILL SOON HAVE NO OTHER SOURCE. power_time_from_http_date()
 * reads the clock off a Home Assistant response, which needs WiFi and the HA
 * token — exactly the two things the light build removes. This is the path that
 * survives that, and it is also 30 seconds faster: the HTTP route needs an
 * association first, which takes about 35s from boot.
 *
 * ⚠️ Same "first one wins" rule as the HTTP path, deliberately. Whichever
 * source answers first is trusted for the session; a second opinion arriving
 * later must not step the clock underneath a running alarm. */
void power_time_set_unix(int64_t secs)
{
    if (s_time_valid) return;
    if (secs < 1700000000LL) return;      /* clearly not a real time */

    struct timeval tv = { .tv_sec = (time_t)secs, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) return;

    set_local_tz();
    s_time_valid = true;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int h12 = tm.tm_hour % 12; if (!h12) h12 = 12;
    applog("[power] clock set from the gateway: %d:%02d %s local",
           h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

/* No WiFi and no HA_HOST in this build, so there is no SNTP server to reach.
 * The clock comes from the gateway (gw time service) when one is in range, and
 * from nothing at all when one is not — see the design notes, which
 * is why GPS is the real answer for a handset that lives away from the house.
 * Kept as a symbol because ui.c calls it behind net_is_up(), which is false. */
void power_sync_time(void) { }

/* Seconds until the next occurrence of the configured alarm, or -1 if no alarm
 * is set or the time is unknown. */
static int64_t seconds_to_alarm(void)
{
    int hhmm = settings_alarm();
    if (hhmm < 0 || !s_time_valid) return -1;

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int now_min   = tm.tm_hour * 60 + tm.tm_min;
    int alarm_min = (hhmm / 100) * 60 + (hhmm % 100);
    int delta = alarm_min - now_min;
    if (delta <= 0) delta += 24 * 60;         /* tomorrow */
    return (int64_t)delta * 60 - tm.tm_sec;
}

void power_sleep_now(void)
{
    /* settings_tick() saves 3s after a change; "Sleep now" pressed inside that
     * window would otherwise lose it - a channel change included. */
    settings_flush();
    int64_t alarm_s = seconds_to_alarm();

    applog("[power] sleeping%s", alarm_s > 0 ? " (alarm armed)" : "");
    if (alarm_s > 0)
        applog("[power] alarm in %lld min", alarm_s / 60);

    /* Clean shutdown. An SD card left mounted is the thing that corrupts on an
     * abrupt power change, and the radio holds a socket and the amp. */
    radio_stop();
    player_suspend();
    net_down();
    display_screen_off();
    vTaskDelay(pdMS_TO_TICKS(150));           /* let the amp settle, no click */

    /* 🚨 WAIT FOR THE FINGER TO COME OFF FIRST.
     *
     * ext0 wakes on GPIO17 LOW, which is exactly what a finger still touching
     * the screen is holding it at. Sleeping with the touch active means waking
     * instantly, and since deep sleep restarts the app that presents as a
     * spontaneous REBOOT. The builder hit this the first time he long-pressed "Sleep
     * now" - the finger that triggered the sleep also cancelled it.
     *
     * Bounded, because a stuck-low INT must not make the device unsleepable. */
    for (int i = 0; i < 100; i++) {
        int rx, ry;
        if (!display_touch_raw(&rx, &ry)) {
            vTaskDelay(pdMS_TO_TICKS(150));   /* settle: the line bounces */
            if (!display_touch_raw(&rx, &ry)) break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* Touch wake: the verified path. Pull-up held in the RTC domain, or the pin
     * floats once the main domain powers down and wakes on noise. */
    esp_sleep_enable_ext0_wakeup(PIN_TOUCH_INT, 0);
    rtc_gpio_pullup_en(PIN_TOUCH_INT);
    rtc_gpio_pulldown_dis(PIN_TOUCH_INT);

    if (alarm_s > 0)
        esp_sleep_enable_timer_wakeup(alarm_s * 1000000LL);

    esp_deep_sleep_start();                   /* never returns */
}

/* Called from the LVGL tick. Decides only; see rule 4. */
void power_tick(void)
{
    int mins = settings_sleep_minutes();
    if (mins <= 0) return;                        /* auto-sleep disabled */

    /* Rule 1: never on USB. This is what keeps the board flashable. */
    if (battery_on_usb()) return;
    /* 🚨 Rule 1b: AND NEVER WHILE THE CARD IS ON THE PC.
     *
     * battery_on_usb() is usb_serial_jtag_is_connected(), and USB card mode
     * switches that peripheral OFF — TinyUSB owns the pins. So rule 1 silently
     * stopped applying at exactly the moment the handset was busiest being a
     * USB device, and it deep-slept mid-session. Deep sleep kills the USB
     * device outright, so recovery needed the RESET BUTTON ON THE BOARD, with
     * the case open — which is the one thing card mode exists to avoid.
     * Measured on the builder's handset, 2026-09-13. */
    if (usbmsc_active()) return;
    /* Rule 2: never within the boot grace period. */
    if (esp_timer_get_time() < BOOT_GRACE_US) return;

    /* Doing something counts as being awake, even untouched: music playing, a
     * station streaming, or the radio held up by a screen that needs it. */
    /* 🚨 AND A GPS ACQUISITION COUNTS. Ephemeris download takes minutes of
     * untouched running; without this the handset deep-sleeps mid-session on
     * battery and the walk is wasted. USB hid this — power_tick() never sleeps
     * while plugged in, so every session so far survived by accident. */
    if (player_state()->playing || radio_playing() || net_is_up() ||
        gps_running()) return;

    if (display_idle_ms() < mins * 60 * 1000) return;
    power_sleep_now();
}
