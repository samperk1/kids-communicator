/*
 * GPS session logging to the card. See gpslog.h for why the MARK window is the
 * point of the whole file.
 *
 * 🚨 THE CARD IS SHARED WITH THE AUDIO PATH. The player streams MP3 off the
 * same SDIO bus in real time, and a write that blocks it produces an audible
 * dropout. So this flushes every ~10 seconds rather than every line, and never
 * writes more than one short row per second. 29k rows over an eight-hour walk
 * is about 3MB, which is nothing on a 34GB card.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "hw.h"
#include "gps.h"
#include "gpslog.h"
#include "applog.h"
#include "espnow.h"

#define LOG_DIR   SD_MOUNT "/gpslog"
#define FLUSH_MS  10000

static FILE    *s_f;
static char     s_name[48];
static char     s_disp[12] = "on";
static uint32_t s_next_flush;
static uint32_t s_t0;
static uint32_t s_rows;
static uint32_t s_marks;

/* ---- the RAM sink. See gpslog.h. ---- */
static char    *s_ram;
static size_t   s_ram_cap;
static size_t   s_ram_len;
static size_t   s_ram_drop;
static bool     s_ram_mode;

/* the open MARK window */
static char     s_label;            /* 0 when none is running */
static char     s_next_label = 'A';
static uint32_t s_win_end;
static double   s_win_cn0;
static uint32_t s_win_n;
static uint32_t s_win_trk;
static char     s_result[64];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* One row goes EITHER to the open file or to the PSRAM buffer, never both, and
 * every writer in this file goes through here. Splitting the two paths at each
 * call site is how a row ends up in one sink and not the other. */
static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_ram_mode) {
        /* Leave room for the NUL vsnprintf always writes. */
        size_t room = (s_ram && s_ram_cap > s_ram_len) ? s_ram_cap - s_ram_len : 0;
        if (room > 1) {
            int n = vsnprintf(s_ram + s_ram_len, room, fmt, ap);
            if (n < 0)                    s_ram_drop++;
            else if ((size_t)n >= room) { s_ram[s_ram_len] = 0; s_ram_drop++; }
            else                          s_ram_len += (size_t)n;
        } else {
            s_ram_drop++;
        }
    } else if (s_f) {
        vfprintf(s_f, fmt, ap);
    }
    va_end(ap);
}


bool gpslog_active(void) { return s_f != NULL || s_ram_mode; }
const char *gpslog_name(void) { return s_name; }
const char *gpslog_last_result(void) { return s_result; }
void gpslog_set_display_state(const char *s)
{
    if (s) { strncpy(s_disp, s, sizeof(s_disp) - 1); s_disp[sizeof(s_disp) - 1] = 0; }
}

/* 🔑 THE FILE IS NUMBERED, NOT TIMESTAMPED. A session usually starts with no
 * fix and therefore no clock — that is the normal case, not the exception, and
 * naming a file from a zeroed clock would give every session the same name and
 * silently overwrite the last one. The UTC column inside the file carries the
 * time once the receiver has it. */
static void pick_name(void)
{
    for (int i = 1; i < 1000; i++) {
        snprintf(s_name, sizeof(s_name), "gps%03d.csv", i);
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, s_name);
        FILE *t = fopen(path, "r");
        if (!t) return;                 /* free */
        fclose(t);
    }
    snprintf(s_name, sizeof(s_name), "gps999.csv");
}

/* The same three comment lines and the same column list whichever sink is
 * open, so a RAM-buffered session and a card-written one parse identically. */
static void write_header(void)
{
    emit("# communicator GPS session log\n");
    emit("# cn0_best4 is the mean C/N0 of the four strongest satellites;\n");
    emit("# it is the placement metric and it does NOT require a fix.\n");
    emit("ms,utc,fix,type,trk,used,cn0_best4,cn0_max,hdop,lat,lon,alt,"
         "display,mark,bad,cn0_4th,decodable,beacon\n");
}

bool gpslog_start(void)
{
    if (s_f) return true;

    /* fopen() into a directory that does not exist just fails, so make it
     * first. Harmless when it already exists. */
    mkdir(LOG_DIR, 0777);

    pick_name();
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, s_name);
    s_f = fopen(path, "w");
    if (!s_f) {
        applog("[gpslog] cannot open %s - no card?", s_name);
        return false;
    }

    write_header();
    s_t0 = now_ms();
    s_next_flush = s_t0 + FLUSH_MS;
    s_label = 0;
    s_rows  = 0;
    s_marks = 0;
    s_result[0] = 0;
    applog("[gpslog] %s", s_name);
    return true;
}

void gpslog_stop(void)
{
    if (!s_f) return;
    fflush(s_f);
    fclose(s_f);
    s_f = NULL;

    /* 🔑 A GLANCE AT THE SCREEN IS NOT A SESSION, AND LETTING IT BECOME ONE
     * COST A REAL WALK. Opening the GPS screen opens a file, so every quick
     * look left a near-empty gpsNNN.csv behind — and since "newest" means
     * "highest numbered", the first dump after a driveway walk returned the
     * two-row file made by glancing at the screen indoors afterwards, not the
     * walk. Throw away anything that recorded no MARK and barely any rows. */
    if (s_marks == 0 && s_rows < 10) {
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, s_name);
        remove(path);
        applog("[gpslog] discarded %s (%u rows, no marks)", s_name, (unsigned)s_rows);
        return;
    }
    applog("[gpslog] closed %s (%u rows, %u marks)", s_name,
           (unsigned)s_rows, (unsigned)s_marks);
}

char gpslog_mark(void)
{
    if (!s_f) return 0;
    s_label   = s_next_label;
    s_win_end = now_ms() + GPSLOG_WINDOW_MS;
    s_win_cn0 = 0;
    s_win_n   = 0;
    s_win_trk = 0;
    if (s_next_label < 'Z') s_next_label++;
    snprintf(s_result, sizeof(s_result), "%c: measuring...", s_label);
    return s_label;
}

int gpslog_window_left(void)
{
    if (!s_label) return 0;
    int32_t left = (int32_t)(s_win_end - now_ms());
    return left > 0 ? (left + 999) / 1000 : 0;
}

void gpslog_tick(void)
{
    if (!s_f && !s_ram_mode) return;

    gps_state_t g;
    gps_snapshot(&g);

    char utc[32] = "";
    if (g.time_valid)
        snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02uZ",
                 g.year, g.month, g.day, g.hour, g.minute, g.second);

    /* ⚠️ `beacon` is READ BACK from the radio, not a label written by whoever
     * opened the screen. The first version stamped a constant "quiet" string
     * and so could never record the change failing — a verification field that
     * cannot fail is worse than none, and it made a whole 15-minute driveway
     * run unusable as evidence.
     *
     * 🔑 cn0_4th and decodable are the columns that say whether a fix is even
     * POSSIBLE. cn0_best4 is an average and hides the answer. */
    emit("%u,%s,%u,%u,%u,%u,%.2f,%u,%.2f,%.6f,%.6f,%.1f,%s,%c,%u,%u,%u,%s\n",
            (unsigned)(now_ms() - s_t0), utc,
            g.quality, g.fix_type, g.sats_tracked, g.sats_used,
            g.cn0_best4, g.cn0_max, g.hdop,
            g.lat, g.lon, g.altitude,
            s_disp, s_label ? s_label : '-', (unsigned)g.bad_checksums,
            g.cn0_4th, g.sats_decodable,
            espnow_beacon_quiet_get() ? "quiet" : "ON");
    s_rows++;

    /* ⚠️ A sample with nothing tracked is not a zero, it is an ABSENCE, and
     * averaging it in would drag a placement's score down in proportion to how
     * long it took to acquire rather than how good it is. */
    if (s_label && g.cn0_best4 > 0.0f) {
        s_win_cn0 += g.cn0_best4;
        s_win_trk += g.sats_tracked;
        s_win_n++;
    }

    if (s_label && gpslog_window_left() == 0) {
        if (s_win_n) {
            double mean = s_win_cn0 / s_win_n;
            double trk  = (double)s_win_trk / s_win_n;
            snprintf(s_result, sizeof(s_result), "%c: %.1f dB, %.1f sats (%us)",
                     s_label, mean, trk, (unsigned)s_win_n);
            /* The summary row is what gets compared later. Prefixed with # so
             * it is a comment to anything that parses the CSV, and still sits
             * in line order next to the samples it came from. */
            emit("# MARK %c mean_cn0=%.2f mean_trk=%.2f samples=%u display=%s\n",
                    s_label, mean, trk, (unsigned)s_win_n, s_disp);
        } else {
            snprintf(s_result, sizeof(s_result), "%c: nothing tracked", s_label);
            emit("# MARK %c NO SATELLITES TRACKED\n", s_label);
        }
        applog("[gpslog] %s", s_result);
        s_marks++;
        s_label = 0;
    }

    if (s_f && (int32_t)(now_ms() - s_next_flush) >= 0) {
        fflush(s_f);
        s_next_flush = now_ms() + FLUSH_MS;
    }
}

/* ---- reading a session back off the card ---------------------------------
 *
 * 🔑 THE CARD LIVES INSIDE THE HANDSET. Getting a walk's results out should not
 * mean opening a case and finding a card reader, so the session prints to the
 * console (and therefore to any serial capture) on demand.
 *
 * The MARK summaries are printed FIRST and on their own. They are the answer;
 * the per-second rows are the evidence behind it, and on a long walk there are
 * thousands of them. Printing the conclusion after the evidence would mean
 * scrolling back through a wall of CSV to find the four lines that matter. */
void gpslog_dump(int session)
{
    char path[96];
    char line[256];

    /* ⚠️ Dumping only "the newest" was wrong: see the note in gpslog_stop().
     * An index first, then every MARK from every session, then the rows —
     * because the summaries are the answer and the rows are only the evidence,
     * and on a walk there are far more of the latter. */
    printf("\n===== GPSDUMP BEGIN =====\n--- sessions ---\n");
    int found = 0;
    for (int i = 1; i < 1000; i++) {
        if (session > 0 && i != session) continue;
        snprintf(path, sizeof(path), "%s/gps%03d.csv", LOG_DIR, i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        int rows = 0, marks = 0;
        while (fgets(line, sizeof(line), f)) {
            rows++;
            if (!strncmp(line, "# MARK", 6)) marks++;
        }
        fclose(f);
        printf("gps%03d.csv  %4d rows  %d marks\n", i, rows, marks);
        found++;
    }
    if (!found) { printf("(no sessions on the card)\n===== GPSDUMP END =====\n\n"); return; }

    printf("--- every MARK, all sessions ---\n");
    int total_marks = 0;
    for (int i = 1; i < 1000; i++) {
        if (session > 0 && i != session) continue;
        snprintf(path, sizeof(path), "%s/gps%03d.csv", LOG_DIR, i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "# MARK", 6)) {
                printf("gps%03d %s", i, line);
                total_marks++;
            }
        fclose(f);
    }
    if (!total_marks) printf("(none completed)\n");

    /* 🚨 NEWEST FIRST, AND THE CAP IS WHY.
     *
     * This loop used to run 1 -> 999 with the same 6000-row ceiling, so the
     * console filled up with the OLDEST sessions and stopped. On 2026-09-13
     * that hid the only session that had ever produced a fix: 31 sessions were
     * on the card, the dump printed gps001..gps023 and cut off, and the answer
     * was in gps030. The row cap has to eat the sessions nobody is asking
     * about, not the one that just happened. */
    printf("--- rows, NEWEST SESSION FIRST ---\n");
    int printed = 0;
    for (int i = 999; i >= 1 && printed < 6000; i--) {
        if (session > 0 && i != session) continue;
        snprintf(path, sizeof(path), "%s/gps%03d.csv", LOG_DIR, i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        printf("### gps%03d.csv\n", i);
        while (fgets(line, sizeof(line), f) && printed < 6000) {
            printf("%s", line);
            printed++;
        }
        fclose(f);
    }
    printf("===== GPSDUMP END (%d sessions, %d marks, %d rows) =====\n\n",
           found, total_marks, printed);
}

/* ---- the RAM sink --------------------------------------------------------
 *
 * See gpslog.h. The card A/B unmounts the card for half its run, so there is
 * no file to write to; rows land here and the session is written in one pass
 * once the card is back and mounted.
 */

bool gpslog_ram_active(void)  { return s_ram_mode; }
size_t gpslog_ram_dropped(void) { return s_ram_drop; }

bool gpslog_ram_begin(size_t cap)
{
    if (s_ram_mode) return true;
    /* A card-backed session and a RAM one at the same time would double every
     * row and put the two sinks out of step. One at a time. */
    if (s_f) gpslog_stop();

    s_ram = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ram) {
        applog("[gpslog] RAM sink: no %u bytes of PSRAM", (unsigned)cap);
        return false;
    }
    s_ram[0]   = 0;
    s_ram_cap  = cap;
    s_ram_len  = 0;
    s_ram_drop = 0;
    s_ram_mode = true;

    s_t0    = now_ms();
    s_label = 0;
    s_rows  = 0;
    s_marks = 0;
    s_result[0] = 0;
    write_header();
    applog("[gpslog] RAM sink open, %u KB", (unsigned)(cap / 1024));
    return true;
}

void gpslog_ram_abort(void)
{
    if (!s_ram_mode) return;
    s_ram_mode = false;
    free(s_ram);
    s_ram = NULL;
    s_ram_cap = s_ram_len = 0;
    applog("[gpslog] RAM sink discarded");
}

bool gpslog_ram_commit(void)
{
    if (!s_ram_mode) return false;

    /* 🔑 STOP BUFFERING BEFORE WRITING. pick_name() reads the card, and any
     * row emitted between here and the fclose() would go to a buffer nobody
     * is going to write out. */
    s_ram_mode = false;

    bool ok = false;
    mkdir(LOG_DIR, 0777);
    pick_name();
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, s_name);

    FILE *f = fopen(path, "w");
    if (!f) {
        /* ⚠️ Say it loudly and KEEP THE BUFFER. An 18-minute run that reports
         * "done" while its only copy was freed is worse than one that fails. */
        applog("[gpslog] RAM commit FAILED to open %s - buffer kept", s_name);
        s_ram_mode = true;
        return false;
    }
    size_t wrote = s_ram_len ? fwrite(s_ram, 1, s_ram_len, f) : 0;
    if (s_ram_drop)
        fprintf(f, "# ⚠️ TRUNCATED: %u rows dropped, the RAM buffer filled\n",
                (unsigned)s_ram_drop);
    fflush(f);
    fclose(f);

    ok = (wrote == s_ram_len);
    applog("[gpslog] %s written, %u KB%s", s_name, (unsigned)(wrote / 1024),
           s_ram_drop ? " (TRUNCATED)" : "");
    if (!ok) applog("[gpslog] short write: %u of %u bytes",
                    (unsigned)wrote, (unsigned)s_ram_len);

    free(s_ram);
    s_ram = NULL;
    s_ram_cap = s_ram_len = 0;
    return ok;
}
