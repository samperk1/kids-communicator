#pragma once
#include <stdbool.h>
#include <stdint.h>

/* GPS: a GOOUUU breakout carrying an ATGM336H (GPS + BeiDou), on UART1.
 *
 * 🚨 WIRING IS THE OPPOSITE OF THE PLAN, AND THIS WAS MEASURED, NOT ASSUMED.
 * The module's TX lands on GPIO43 and its RX on GPIO44 — see hw.h. The plan
 * documents said 44/43 and the wires say otherwise; the firmware follows the
 * hardware. Diagnosed 2026-09-12 by pulling each pin up then down: GPIO44
 * followed the pull (floating — it is on the module's RX, an input that drives
 * nothing) while GPIO43 held HIGH against a pull-down and went active once a
 * second. A stream of 0x00 bytes is a line held LOW, not data.
 *
 * 🚨 IT IS A CASIC CHIP, NOT u-blox. Configuration is `$PCAS…`, never UBX.
 * Every ESP32 GPS example online sends UBX and this module ignores it SILENTLY,
 * which reads exactly like a dead module. Nothing here sends any configuration:
 * measured on hardware, the defaults already emit everything we need.
 *
 * Defaults, confirmed on the test handset: 9600 8N1, 1 Hz, ten sentences per fix —
 * GNGGA GNGLL GPGSA BDGSA GPGSV BDGSV GNRMC GNVTG GNZDA GPTXT.
 * 450 sentences in 45 s with zero checksum errors.
 *
 * ⚠️ THE MODULE'S LED SAYS NOTHING ABOUT THE DATA WIRE. Steady red = powered,
 * no fix. ~1 Hz blink = fix. It was lit brightly for the whole afternoon that
 * GPIO44 sat floating and unconnected.
 *
 * ⚠️ SENTENCES APPEAR IMMEDIATELY, WITH EMPTY FIELDS, BEFORE ANY FIX. So no
 * sentences at all means power or wiring — never "no sky". Conversely a module
 * that will not fix indoors looks exactly like a dead one; test outdoors before
 * concluding anything. */

#define GPS_MAX_SATS   32   /* tracked across both constellations */

typedef enum {
    GPS_CONST_GPS = 0,      /* $GPGSV */
    GPS_CONST_BEIDOU,       /* $BDGSV */
} gps_const_t;

typedef struct {
    uint8_t     prn;
    uint8_t     elevation;  /* degrees, 0-90 */
    uint16_t    azimuth;    /* degrees true, 0-359 */
    uint8_t     cn0;        /* dB-Hz, 0 = not being tracked */
    gps_const_t constellation;
    bool        used;       /* in the position solution, per GSA */
} gps_sat_t;

typedef struct {
    /* --- fix ------------------------------------------------------------ */
    uint8_t  quality;       /* GGA field 6: 0 none, 1 GPS, 2 DGPS */
    uint8_t  fix_type;      /* GSA field 2: 1 none, 2 = 2D, 3 = 3D */
    bool     valid;         /* RMC says the position is usable */
    double   lat, lon;      /* degrees, +N +E */
    float    altitude;      /* metres */
    float    speed_kmh;
    float    course;        /* degrees true; only meaningful while moving */
    float    hdop, pdop, vdop;

    /* --- time ----------------------------------------------------------- */
    /* 🔑 THE WHOLE REASON THIS FEATURE EARNS ITS PLACE. The light build has no
     * WiFi, so it has no HTTP Date header and no SNTP — GPS is its ONLY clock.
     * ZDA carries a full UTC date and is cleaner than digging the date out of
     * RMC field 9. Both are parsed; ZDA wins when present. */
    bool     time_valid;
    uint16_t year;
    uint8_t  month, day, hour, minute, second;

    /* --- satellites ----------------------------------------------------- */
    uint8_t   sats_used;    /* in the solution */
    uint8_t   sats_tracked; /* reporting a non-zero C/N0 */
    uint8_t   sat_count;    /* entries in sats[] */
    gps_sat_t sats[GPS_MAX_SATS];

    /* --- the placement metric ------------------------------------------- */
    /* 🔑 C/N0 DOES NOT NEED A FIX. GSV reports carrier-to-noise for every
     * satellite being TRACKED, and tracking starts long before a position
     * solution. That is what makes a case placement scoreable in 30 seconds
     * instead of a cold-start wait, so two placements can be compared while
     * the sky overhead is effectively unchanged.
     *
     * Best-4 rather than the mean of all: total count saturates and stops
     * discriminating between a good placement and a great one. The strongest
     * handful keeps moving. */
    float    cn0_best4;
    uint8_t  cn0_max;

    /* 🔑 THE NUMBER THAT DECIDES WHETHER A FIX IS POSSIBLE AT ALL.
     *
     * A fix needs ephemeris from FOUR satellites, and ephemeris is decoded per
     * satellite from an 18-second run of clean 50 bps data needing roughly
     * 30-32 dB-Hz SUSTAINED ON THAT SATELLITE.
     *
     * 🚨 cn0_best4 is an AVERAGE and cannot answer this — 40/38/28/26 averages
     * to 33 and looks healthy while only two satellites are actually
     * decodable. That mistake cost most of an afternoon on 2026-09-12: six
     * sessions, 25+ minutes, "tracked" in the twenties the whole time, `used`
     * never once leaving zero, and three different theories argued from an
     * average that was hiding the answer.
     *
     * cn0_4th is the 4th-strongest satellite. If IT is below ~30, no fix is
     * possible no matter how good the top one looks or how long you wait. */
    uint8_t  cn0_4th;
    uint8_t  sats_decodable;   /* individually >= 32 dB-Hz */

    /* --- health --------------------------------------------------------- */
    bool     antenna_ok;    /* from $GPTXT */
    uint32_t sentences;     /* accepted, checksum verified */
    uint32_t bad_checksums; /* a real event: the baseline is zero */
    uint32_t last_rx_ms;    /* esp_timer ms of the last good sentence */
    uint32_t ttff_ms;       /* start -> first fix; 0 until it happens */
} gps_state_t;

/* Bring the UART up and start the reader task. Idempotent.
 * ⚠️ 25-50 mA while tracking — backlight-class. Run it on the GPS screens only,
 * the same discipline the radio follows. */
void gps_start(void);
void gps_stop(void);
bool gps_running(void);

/* Copy the current state out under the lock. Never parses NMEA in UI code.
 * Always safe to call, running or not; returns a zeroed state when stopped. */
void gps_snapshot(gps_state_t *out);

/* Raw NMEA passthrough for the logger, so a session can be replayed and a
 * surprising number can be settled against the bytes that produced it.
 * The callback runs on the GPS task — copy, do not block. NULL to disable. */
typedef void (*gps_raw_cb_t)(const char *line, void *ctx);
void gps_set_raw_cb(gps_raw_cb_t cb, void *ctx);

/* Send a $PCAS command: pass the body WITHOUT the leading '$' and without the
 * checksum — e.g. "PCAS04,1" or "PCAS10,2". The checksum and CR LF are added.
 * Returns false if the receiver is not running. See gps.c for the command list.
 *
 * 🚨 The module is CASIC. It speaks $PCAS, not UBX, and ignores UBX silently. */
bool gps_send_cmd(const char *body);

/* Poll the CASIC MON-HW message and dump the reply as hex to the console.
 * Answers "is the active antenna actually powered" without a multimeter — but
 * only by COMPARING two probes, antenna in and antenna out. See gps.c. */
void gps_monhw_probe(void);
