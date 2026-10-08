/*
 * NMEA 0183 from the ATGM336H. See gps.h for the wiring story — the short
 * version is that the two data pins are the opposite way round from every
 * planning document and that was measured, not guessed.
 *
 * 🔑 NOTHING HERE CONFIGURES THE MODULE. Measured on hardware: the defaults
 * already emit all ten sentences we need at 1 Hz. The chip is CASIC, so its
 * config language is `$PCAS` and not UBX — but since no configuration is
 * needed, the question never arises and a whole class of silent-failure bugs
 * goes with it.
 *
 * 🚨 DO NOT ADD A BAUD SCAN. An earlier throwaway probe scanned 9600 -> 115200
 * looking for the module; on a pin that turned out to be floating, the flood of
 * framing errors at the high rates HUNG THE CHIP and killed the console until a
 * hard reset. Two capture runs were lost to it. 9600 is the documented default
 * and it is confirmed on this hardware. Stay there.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "hw.h"
#include "gps.h"
#include "applog.h"

static const char *TAG = "gps";

#define RX_BUF      2048
/* Not LINE_MAX: POSIX <limits.h> defines that, and it arrives here now that
 * queue.h is included. Same value, unambiguous name. */
#define GPS_LINE_MAX 128

static gps_state_t       s_st;
static SemaphoreHandle_t s_lock;
static TaskHandle_t      s_task;

/* 🔑 UART OVERFLOW IS SILENT UNLESS YOU ASK FOR IT.
 *
 * `uart_driver_install(..., NULL, 0)` installs NO event queue, and without one
 * the driver discards a FIFO overflow without a word — there is no return code
 * to check and no counter to read. So "the map screen starves the GPS reader"
 * was a theory that COULD NOT BE TESTED from any log this firmware produced.
 * The builder raised it 2026-09-18; answering it needed an instrument, not an argument.
 *
 * 2.1 s of headroom: RX_BUF 2048 bytes at 9600 baud = 960 B/s. Anything that
 * blocks this task longer than that loses NMEA. The reader is priority 5 and
 * LVGL is pumped from app_main at priority 1, so the theory is unlikely on
 * paper — which is exactly the kind of reasoning that deserves a counter. */
static volatile uint32_t s_ovf;      /* hardware FIFO overflowed          */
static volatile uint32_t s_full;     /* the 2 KB ring filled              */
static QueueHandle_t     s_evtq;

/* ---- MON-HW binary probe ------------------------------------------------
 * The NMEA path is line-assembled, and a CASIC binary reply has no newlines,
 * so it cannot come back through it. While a probe is in flight the task stops
 * assembling lines and copies raw bytes instead. */
static volatile bool     s_sniff;
static uint8_t           s_sniff_buf[512];
static volatile int      s_sniff_n;
static uint32_t          s_sniff_until;
static volatile bool     s_run;
static uint32_t          s_start_ms;

static gps_raw_cb_t      s_raw_cb;
static void             *s_raw_ctx;

/* GSV accumulates across several sentences AND across constellations. See the
 * note above gsv() for the two traps; the answer to both is one shadow table
 * PER TALKER, rebuilt into the live table whenever any talker completes. */
#define GPS_TALKERS 2       /* GPS_CONST_GPS, GPS_CONST_BEIDOU */
static gps_sat_t s_shadow[GPS_TALKERS][GPS_MAX_SATS];
static uint8_t   s_shadow_n[GPS_TALKERS];
static bool      s_shadow_ready[GPS_TALKERS];

/* GSA's "used in solution" PRNs, also kept per talker. They must survive a GSV
 * rebuild: in this module's actual sentence order GSA arrives BEFORE GSV, so
 * marking the live table directly and letting GSV rebuild over the top would
 * throw every mark away once a second. */
static uint8_t   s_used_prn[GPS_TALKERS][12];
static uint8_t   s_used_prn_n[GPS_TALKERS];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- field splitting ----------------------------------------------------
 *
 * ⚠️ NMEA fields are frequently EMPTY, not zero — "$GNRMC,,V,,,,,,,,,,N" is a
 * perfectly valid sentence and it is what arrives before a fix. Every accessor
 * therefore has to distinguish "absent" from "0.0", or a handset with no fix
 * reports itself as sitting at exactly 0N 0E in the Gulf of Guinea. */

#define MAX_FIELDS 24
typedef struct { const char *f[MAX_FIELDS]; int n; } fields_t;

static void split(char *s, fields_t *out)
{
    out->n = 0;
    out->f[out->n++] = s;
    for (char *p = s; *p && out->n < MAX_FIELDS; p++) {
        if (*p == ',') { *p = 0; out->f[out->n++] = p + 1; }
    }
}

static bool has(const fields_t *f, int i)
{
    return i < f->n && f->f[i] && f->f[i][0];
}
static float   ffield(const fields_t *f, int i) { return has(f, i) ? strtof(f->f[i], NULL) : 0.0f; }
static int     ifield(const fields_t *f, int i) { return has(f, i) ? atoi(f->f[i]) : 0; }

/* NMEA latitude is ddmm.mmmm and longitude dddmm.mmmm — degrees and minutes
 * jammed together with no separator. Splitting on the decimal point does not
 * work; the degree field is a fixed width that differs between lat and lon. */
static double degmin(const char *v, bool is_lon)
{
    if (!v || !v[0]) return 0.0;
    int dwidth = is_lon ? 3 : 2;
    char deg[4] = {0};
    strncpy(deg, v, dwidth);
    return atof(deg) + atof(v + dwidth) / 60.0;
}

static double coord(const fields_t *f, int vi, int hi, bool is_lon)
{
    if (!has(f, vi) || !has(f, hi)) return 0.0;
    double d = degmin(f->f[vi], is_lon);
    char h = f->f[hi][0];
    return (h == 'S' || h == 'W') ? -d : d;
}

/* ---- the placement metric ----------------------------------------------- */

static void recompute_cn0(void)
{
    uint8_t best[4] = {0, 0, 0, 0};
    uint8_t tracked = 0, max = 0;

    for (int i = 0; i < s_st.sat_count; i++) {
        uint8_t c = s_st.sats[i].cn0;
        if (!c) continue;                 /* reported but not being tracked */
        tracked++;
        if (c > max) max = c;
        /* insertion into a top-4 ladder; four elements does not merit a sort */
        for (int k = 0; k < 4; k++) {
            if (c > best[k]) {
                for (int j = 3; j > k; j--) best[j] = best[j - 1];
                best[k] = c;
                break;
            }
        }
    }

    /* Average over however many we actually have. Dividing by a hard 4 when
     * only two satellites are visible would halve the score of a placement
     * that is merely starved of sky, and make a bad case look catastrophic. */
    int n = 0, sum = 0;
    for (int k = 0; k < 4; k++) if (best[k]) { sum += best[k]; n++; }

    s_st.cn0_best4   = n ? (float)sum / n : 0.0f;
    s_st.cn0_max     = max;
    s_st.sats_tracked = tracked;
    s_st.cn0_4th     = best[3];     /* 0 when fewer than four are tracked */

    int dec = 0;
    for (int i = 0; i < s_st.sat_count; i++)
        if (s_st.sats[i].cn0 >= 32) dec++;
    s_st.sats_decodable = dec;
}

/* ---- sentence handlers --------------------------------------------------- */

static void gga(const fields_t *f)
{
    s_st.quality   = ifield(f, 6);
    s_st.sats_used = ifield(f, 7);
    s_st.hdop      = ffield(f, 8);
    s_st.altitude  = ffield(f, 9);

    if (s_st.quality > 0) {
        s_st.lat = coord(f, 2, 3, false);
        s_st.lon = coord(f, 4, 5, true);
        /* 🔑 TTFF is stamped once and never again. It is the number that says
         * whether the RTC backup cell is doing its job: a warm start lands in
         * seconds, a cold one takes minutes. That closes E.1 without opening
         * the case to look for the cell. */
        if (!s_st.ttff_ms) {
            s_st.ttff_ms = now_ms() - s_start_ms;
            applog("[gps] first fix after %lus", (unsigned long)(s_st.ttff_ms / 1000));
        }
    }
}

static void apply_used(void)
{
    for (int i = 0; i < s_st.sat_count; i++) s_st.sats[i].used = false;
    for (int t = 0; t < GPS_TALKERS; t++)
        for (int k = 0; k < s_used_prn_n[t]; k++)
            for (int i = 0; i < s_st.sat_count; i++)
                if (s_st.sats[i].prn == s_used_prn[t][k]) s_st.sats[i].used = true;
}

/* Concatenate every talker that has delivered a complete GSV group. */
static void rebuild_sats(void)
{
    uint8_t n = 0;
    for (int t = 0; t < GPS_TALKERS; t++) {
        if (!s_shadow_ready[t]) continue;
        for (int i = 0; i < s_shadow_n[t] && n < GPS_MAX_SATS; i++)
            s_st.sats[n++] = s_shadow[t][i];
    }
    s_st.sat_count = n;
    apply_used();
    recompute_cn0();
}

/* ⚠️ This module emits $GPGSA and $BDGSA as separate sentences, which is what
 * makes a per-talker list work. A receiver that sent one $GNGSA per
 * constellation would land them all in the same bucket and the last would win.
 * Not a problem on this hardware; it would be on other hardware. */
static void gsa(const fields_t *f, gps_const_t c)
{
    s_st.fix_type = ifield(f, 2);
    s_st.pdop     = ffield(f, 15);
    s_st.hdop     = ffield(f, 16);
    s_st.vdop     = ffield(f, 17);

    /* Fields 3..14 are the PRNs used in the solution. */
    uint8_t n = 0;
    for (int i = 3; i <= 14 && n < 12; i++) {
        if (!has(f, i)) continue;
        s_used_prn[c][n++] = (uint8_t)atoi(f->f[i]);
    }
    s_used_prn_n[c] = n;
    apply_used();
}

/* GSV: "$GPGSV,<total msgs>,<msg #>,<sats in view>,<prn,elev,azim,cn0> x4"
 *
 * 🚨 TWO TRAPS, AND THE SECOND ONE COST A REWRITE. Both produce a
 * plausible-looking wrong answer rather than an obvious failure.
 *
 * 1. It is a MULTI-SENTENCE GROUP. Writing each sentence straight into the
 *    live table makes the UI flicker between half-populated satellite lists
 *    once a second, which reads exactly like a receiver dropping lock.
 *
 * 2. GPS AND BEIDOU SEND SEPARATE GROUPS ($GPGSV and $BDGSV), each numbered
 *    from 1. 🔑 The first attempt here tried to detect a "cycle" and reset one
 *    shared shadow table at the top of it. The host test caught it immediately:
 *    BeiDou's group reset the table and wiped all 11 GPS satellites, leaving 3.
 *    The sky plot would have shown a quarter of the constellation and the
 *    placement metric would have been computed from the wrong four satellites —
 *    silently, and only outdoors.
 *
 *    There is no reliable cycle delimiter in NMEA, so do not look for one:
 *    give each talker its OWN shadow table and rebuild the live table from
 *    every talker that has completed. Order and interleaving stop mattering.
 */
static void gsv(const fields_t *f, gps_const_t c)
{
    int total = ifield(f, 1);
    int msg   = ifield(f, 2);

    if (msg == 1) { s_shadow_n[c] = 0; s_shadow_ready[c] = false; }

    for (int i = 4; i + 3 < f->n && s_shadow_n[c] < GPS_MAX_SATS; i += 4) {
        if (!has(f, i)) continue;
        gps_sat_t *s = &s_shadow[c][s_shadow_n[c]++];
        s->prn           = atoi(f->f[i]);
        s->elevation     = has(f, i + 1) ? atoi(f->f[i + 1]) : 0;
        s->azimuth       = has(f, i + 2) ? atoi(f->f[i + 2]) : 0;
        s->cn0           = has(f, i + 3) ? atoi(f->f[i + 3]) : 0;
        s->constellation = c;
        s->used          = false;
    }

    if (msg == total && total > 0) {
        s_shadow_ready[c] = true;
        rebuild_sats();
    }
}

static void rmc(const fields_t *f)
{
    s_st.valid = has(f, 2) && f->f[2][0] == 'A';
    if (s_st.valid) {
        s_st.lat       = coord(f, 3, 4, false);
        s_st.lon       = coord(f, 5, 6, true);
        s_st.speed_kmh = ffield(f, 7) * 1.852f;   /* knots -> km/h */
        s_st.course    = ffield(f, 8);
    }
    /* Date as DDMMYY in field 9. ZDA is preferred and overwrites this, but the
     * light build cannot afford to depend on a sentence being present. */
    if (!s_st.time_valid && has(f, 9) && strlen(f->f[9]) >= 6 && has(f, 1)) {
        const char *d = f->f[9], *t = f->f[1];
        s_st.day    = (d[0]-'0')*10 + (d[1]-'0');
        s_st.month  = (d[2]-'0')*10 + (d[3]-'0');
        s_st.year   = 2000 + (d[4]-'0')*10 + (d[5]-'0');
        s_st.hour   = (t[0]-'0')*10 + (t[1]-'0');
        s_st.minute = (t[2]-'0')*10 + (t[3]-'0');
        s_st.second = (t[4]-'0')*10 + (t[5]-'0');
        s_st.time_valid = true;
    }
}

/* $GNZDA,hhmmss.ss,dd,mm,yyyy,tz,tzmin — the cleanest date+time there is, and
 * the only clock the light build will ever have. */
static void zda(const fields_t *f)
{
    if (!has(f, 1) || !has(f, 4)) return;
    const char *t = f->f[1];
    if (strlen(t) < 6) return;
    s_st.hour   = (t[0]-'0')*10 + (t[1]-'0');
    s_st.minute = (t[2]-'0')*10 + (t[3]-'0');
    s_st.second = (t[4]-'0')*10 + (t[5]-'0');
    s_st.day    = ifield(f, 2);
    s_st.month  = ifield(f, 3);
    s_st.year   = ifield(f, 4);
    s_st.time_valid = (s_st.year > 2000);
}

/* $GPTXT,01,01,01,ANTENNA OK — worth parsing because "no fix" and "antenna
 * not plugged in" are otherwise indistinguishable from the outside, and the
 * u.FL connector on this breakout is exactly the kind that works loose. */
static void txt(const fields_t *f)
{
    if (f->n < 5) return;
    if (strstr(f->f[4], "ANTENNA OK"))   s_st.antenna_ok = true;
    else if (strstr(f->f[4], "ANTENNA")) s_st.antenna_ok = false;
}

/* ---- line handling ------------------------------------------------------- */

static bool checksum_ok(const char *line, int len)
{
    const char *star = NULL;
    for (int i = len - 1; i > 0 && i > len - 6; i--)
        if (line[i] == '*') { star = &line[i]; break; }
    if (!star) return false;

    uint8_t ck = 0;
    for (const char *p = line + 1; p < star; p++) ck ^= (uint8_t)*p;
    return ck == (uint8_t)strtol(star + 1, NULL, 16);
}

static void handle(char *line, int len)
{
    if (len < 9 || line[0] != '$') return;

    if (!checksum_ok(line, len)) {
        /* The measured baseline on this hardware is ZERO bad checksums in 450
         * sentences. So this counter is not noise to be swallowed — a placement
         * that starts corrupting lines is telling us something about the RF
         * environment, and the logger records it alongside C/N0. */
        s_st.bad_checksums++;
        return;
    }

    if (s_raw_cb) s_raw_cb(line, s_raw_ctx);

    s_st.sentences++;
    s_st.last_rx_ms = now_ms();

    /* Chop the checksum off before splitting, so the last field is clean. */
    char *star = strrchr(line, '*');
    if (star) *star = 0;

    const char *talker = line + 1;      /* e.g. "GP", "GN", "BD" */
    const char *type   = line + 3;      /* e.g. "GGA" */

    fields_t f;
    split(line, &f);

    gps_const_t c = !strncmp(talker, "BD", 2) ? GPS_CONST_BEIDOU : GPS_CONST_GPS;

    if      (!strncmp(type, "GGA", 3)) gga(&f);
    else if (!strncmp(type, "GSA", 3)) gsa(&f, c);
    else if (!strncmp(type, "RMC", 3)) rmc(&f);
    else if (!strncmp(type, "ZDA", 3)) zda(&f);
    else if (!strncmp(type, "TXT", 3)) txt(&f);
    else if (!strncmp(type, "GSV", 3)) gsv(&f, c);
}

/* A heartbeat every 10s, to applog and the console.
 *
 * 🔑 THE SCREEN IS NOT ALWAYS READABLE. This handset is used away from a cable,
 * the display A/B test deliberately blanks the screen for 30s at a time, and a
 * pocket is not a viewing angle. applog() is also what "Show log" in the menu
 * displays, so the same line is readable on the device afterwards.
 *
 * It prints bad_checksums unconditionally: the measured baseline on this
 * hardware is ZERO, so any non-zero value is a real event and not noise. */
static void heartbeat(void)
{
    gps_state_t g;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(&g, &s_st, sizeof(g));
    xSemaphoreGive(s_lock);

    if (!g.sentences) {
        /* Sentences arrive with empty fields long before any fix, so silence is
         * a wiring or power fault -- never "no sky". Say so plainly. */
        applog("[gps] NO DATA on GPIO%d - check power and the TX wire", PIN_GPS_RX);
        return;
    }
    /* 🔑 dec= and 4th= are the pair that say whether a fix is POSSIBLE.
     * Fewer than 4 decodable means no fix, however good cn0/pk look. */
    /* 🔑 gps= and bd= ARE THE PROOF THAT A $PCAS COMMAND LANDED.
     *
     * We have never confirmed the GPIO44 line actually reaches the module's RX.
     * The pin test that found the TX/RX swap read GPIO44 as FLOATING, and I
     * reported that as "connected to an input, which drives nothing" — but an
     * UNCONNECTED pin floats identically. The test cannot separate them.
     *
     * This can: `$PCAS04,1` disables BeiDou, so if bd= falls to 0 the command
     * arrived and the wire is good. If bd= stays non-zero after sending it,
     * the command went nowhere and every configuration test so far was void. */
    int ngps = 0, nbd = 0;
    for (int i = 0; i < g.sat_count; i++) {
        if (!g.sats[i].cn0) continue;
        if (g.sats[i].constellation == GPS_CONST_BEIDOU) nbd++; else ngps++;
    }
    /* ovf/full print ONLY when non-zero. The baseline is zero, so a silent
     * heartbeat means the reader kept up — and a visible one is the answer to
     * "is the UI starving the GPS", stated in events rather than in opinion. */
    char starve[40] = "";
    if (s_ovf || s_full)
        snprintf(starve, sizeof(starve), " ovf %u full %u",
                 (unsigned)s_ovf, (unsigned)s_full);

    applog("[gps] %us trk %u/%u (gps %d bd %d) dec %u 4th %u cn0 %.1f pk %u %s bad %u%s",
           (unsigned)((now_ms() - s_start_ms) / 1000),
           g.sats_tracked, g.sats_used, ngps, nbd,
           g.sats_decodable, g.cn0_4th, g.cn0_best4, g.cn0_max,
           g.quality ? (g.fix_type == 3 ? "3D" : "2D") : "nofix",
           (unsigned)g.bad_checksums, starve);
}

/* 🚨 THE PAYLOAD OFFSETS ARE NOT KNOWN AND ARE NOT GUESSED HERE.
 *
 * A research note (09-14) documents the MON-HW poll frame, the checksum and what
 * every field MEANS, but it never enumerates the byte offsets inside the 56-byte
 * payload — it says only that they are "in the protocol spec". Decoding from a
 * guessed offset would return A NUMBER, and a number always looks like data.
 * That is how a measurement lies.
 *
 * So this prints the raw payload as hex and decodes nothing. The fields are then
 * identified EMPIRICALLY, which is stronger than the spec anyway:
 *
 *   probe with the antenna plugged in, then probe with it unplugged.
 *   - the byte that flips between 2 (normal) and 4 (open) is antStatus.
 *   - the 16-bit word that jumps toward ~62000 unplugged is agcData0.
 *
 * That identifies the fields and proves the reading in the same two captures. */
static void monhw_report(void)
{
    int n = s_sniff_n;
    applog("[monhw] %d bytes back", n);

    int at = -1;
    for (int i = 0; i + 6 <= n; i++)
        if (s_sniff_buf[i] == 0xBA && s_sniff_buf[i + 1] == 0xCE) { at = i; break; }
    if (at < 0) {
        applog("[monhw] no 0xBA 0xCE frame found - the module ignored the poll "
               "or answers NMEA only");
        for (int i = 0; i < n && i < 48; i += 16)
            printf("[monhw] raw %02d: %02x %02x %02x %02x %02x %02x %02x %02x "
                   "%02x %02x %02x %02x %02x %02x %02x %02x\n", i,
                   s_sniff_buf[i+0], s_sniff_buf[i+1], s_sniff_buf[i+2], s_sniff_buf[i+3],
                   s_sniff_buf[i+4], s_sniff_buf[i+5], s_sniff_buf[i+6], s_sniff_buf[i+7],
                   s_sniff_buf[i+8], s_sniff_buf[i+9], s_sniff_buf[i+10], s_sniff_buf[i+11],
                   s_sniff_buf[i+12], s_sniff_buf[i+13], s_sniff_buf[i+14], s_sniff_buf[i+15]);
        return;
    }

    uint16_t len = (uint16_t)(s_sniff_buf[at+2] | (s_sniff_buf[at+3] << 8));
    uint8_t  cls = s_sniff_buf[at+4], id = s_sniff_buf[at+5];
    applog("[monhw] frame at %d: class 0x%02x id 0x%02x len %u", at, cls, id, len);

    if (at + 6 + len + 4 > n) {
        applog("[monhw] frame truncated (have %d, need %d) - raise the window",
               n - at, 6 + len + 4);
        return;
    }

    /* ckSum = (id<<24)|(class<<16)|len, then += each 4-byte LE payload word. */
    uint32_t ck = ((uint32_t)id << 24) | ((uint32_t)cls << 16) | len;
    for (int i = 0; i + 4 <= len; i += 4)
        ck += (uint32_t)s_sniff_buf[at+6+i]
            | ((uint32_t)s_sniff_buf[at+6+i+1] << 8)
            | ((uint32_t)s_sniff_buf[at+6+i+2] << 16)
            | ((uint32_t)s_sniff_buf[at+6+i+3] << 24);
    uint32_t got = (uint32_t)s_sniff_buf[at+6+len]
                 | ((uint32_t)s_sniff_buf[at+6+len+1] << 8)
                 | ((uint32_t)s_sniff_buf[at+6+len+2] << 16)
                 | ((uint32_t)s_sniff_buf[at+6+len+3] << 24);
    applog("[monhw] cksum calc %08x recv %08x %s", (unsigned)ck, (unsigned)got,
           ck == got ? "OK" : "MISMATCH");

    for (int i = 0; i < len; i += 8) {
        printf("[monhw] +%02d:", i);
        for (int j = 0; j < 8 && i + j < len; j++)
            printf(" %02x", s_sniff_buf[at + 6 + i + j]);
        printf("   u16:");
        for (int j = 0; j + 1 < 8 && i + j + 1 < len; j += 2)
            printf(" %5u", (unsigned)(s_sniff_buf[at+6+i+j]
                                    | (s_sniff_buf[at+6+i+j+1] << 8)));
        printf("\n");
    }
}

void gps_monhw_probe(void)
{
    if (!s_run) { applog("[monhw] GPS is not running - open the GPS screen"); return; }
    static const uint8_t poll[10] = {
        0xBA, 0xCE, 0x00, 0x00, 0x0A, 0x09, 0x00, 0x00, 0x0A, 0x09
    };
    s_sniff_n = 0;
    s_sniff_until = now_ms() + 500;
    s_sniff = true;
    uart_write_bytes(GPS_UART_NUM, (const char *)poll, sizeof(poll));
    applog("[monhw] polled, listening 500ms");
}

static void gps_task(void *arg)
{
    static uint8_t rx[256];
    char line[GPS_LINE_MAX];
    int  n = 0;
    uint32_t next_beat = now_ms() + 10000;

    while (s_run) {
        if ((int32_t)(now_ms() - next_beat) >= 0) {
            heartbeat();
            next_beat = now_ms() + 10000;
        }
        int got = uart_read_bytes(GPS_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(100));

        /* Non-blocking: the read above already provided the wait. */
        uart_event_t ev;
        while (s_evtq && xQueueReceive(s_evtq, &ev, 0) == pdTRUE) {
            if (ev.type == UART_FIFO_OVF)          s_ovf++;
            else if (ev.type == UART_BUFFER_FULL)  s_full++;
        }

        /* While a MON-HW probe is in flight, take the bytes RAW. The binary
         * reply has no newline, so the line assembler would swallow it. */
        if (s_sniff) {
            for (int i = 0; i < got; i++)
                if (s_sniff_n < (int)sizeof(s_sniff_buf))
                    s_sniff_buf[s_sniff_n++] = rx[i];
            if ((int32_t)(now_ms() - s_sniff_until) >= 0) {
                s_sniff = false;
                monhw_report();
                n = 0;              /* the half-built NMEA line is now garbage */
            }
            continue;
        }

        for (int i = 0; i < got; i++) {
            char c = (char)rx[i];
            if (c == '\r') continue;
            if (c == '\n') {
                if (n) {
                    line[n] = 0;
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    handle(line, n);
                    xSemaphoreGive(s_lock);
                }
                n = 0;
            } else if (n < GPS_LINE_MAX - 1) {
                line[n++] = c;
            } else {
                n = 0;      /* overlong: drop it rather than split it in two */
            }
        }
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ---- API ----------------------------------------------------------------- */

void gps_start(void)
{
    if (s_run) return;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();

    memset(&s_st, 0, sizeof(s_st));
    memset(s_shadow_n, 0, sizeof(s_shadow_n));
    memset(s_shadow_ready, 0, sizeof(s_shadow_ready));
    memset(s_used_prn_n, 0, sizeof(s_used_prn_n));
    s_start_ms = now_ms();

    uart_config_t cfg = {
        .baud_rate  = GPS_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    s_ovf = s_full = 0;
    if (uart_driver_install(GPS_UART_NUM, RX_BUF, 0, 8, &s_evtq, 0) != ESP_OK) {
        ESP_LOGE(TAG, "uart install failed");
        return;
    }
    uart_param_config(GPS_UART_NUM, &cfg);
    uart_set_pin(GPS_UART_NUM, PIN_GPS_TX, PIN_GPS_RX,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    s_run = true;
    /* PSRAM-capable stack is not available for tasks, but 4KB is ample: this
     * parses text and touches no decoder. */
    xTaskCreate(gps_task, "gps", 4096, NULL, 5, &s_task);
    applog("[gps] started on GPIO%d @ %d", PIN_GPS_RX, GPS_BAUD);
}

void gps_stop(void)
{
    if (!s_run) return;
    s_run = false;
    /* Let the task fall out of its 100ms read before pulling the driver from
     * under it. */
    for (int i = 0; i < 20 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(20));
    uart_driver_delete(GPS_UART_NUM);
    applog("[gps] stopped");
}

bool gps_running(void) { return s_run; }

void gps_snapshot(gps_state_t *out)
{
    if (!out) return;
    if (!s_lock || !s_run) { memset(out, 0, sizeof(*out)); return; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(out, &s_st, sizeof(*out));
    xSemaphoreGive(s_lock);
}

void gps_set_raw_cb(gps_raw_cb_t cb, void *ctx)
{
    s_raw_ctx = ctx;
    s_raw_cb  = cb;     /* set the context first: the task may fire immediately */
}

/* ---- talking BACK to the module -----------------------------------------
 *
 * 🔑 THE WIRE ON GPIO44 EXISTS FOR THIS AND WENT UNUSED ALL DAY. Every planning
 * document called the ESP->GPS line "optional"; it is the only way to change
 * the module's mind about anything.
 *
 * 🚨 CASIC, NOT u-blox: the language is `$PCAS`, never UBX. A UBX command is
 * ignored SILENTLY and reads exactly like a dead module.
 *
 * Checksum is the XOR of everything between '$' and '*', hex, uppercase,
 * terminated CR LF. Verified by hand against published values:
 *     $PCAS04,1*18   GPS only        (disable BeiDou)
 *     $PCAS04,3*1A   GPS + BeiDou    (the factory default)
 *     $PCAS10,0*1C   hot restart
 *     $PCAS10,2*1E   cold restart    (clears almanac, time and position)
 *     $PCAS10,3*1F   factory reset
 *     $PCAS00*01     save current configuration to flash
 *
 * ⚠️ Nothing here saves to flash. A command that only lives until the next
 * power cycle cannot leave the module in a worse permanent state than it is
 * already in, which is the right trade while we are still guessing.
 */
bool gps_send_cmd(const char *body)
{
    if (!s_run || !body || !body[0]) return false;

    uint8_t ck = 0;
    for (const char *p = body; *p; p++) ck ^= (uint8_t)*p;

    char out[64];
    int n = snprintf(out, sizeof(out), "$%s*%02X\r\n", body, ck);
    if (n <= 0 || n >= (int)sizeof(out)) return false;

    uart_write_bytes(GPS_UART_NUM, out, n);
    applog("[gps] sent %s*%02X", body, ck);
    return true;
}
