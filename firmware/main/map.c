/* Vector road map: read .vmp tiles, draw lines. See map.h for the why.
 *
 * Coordinates, once, because everything here depends on getting this right:
 *
 *   Web Mercator, normalised 0..1, scaled by 2^28 = GLOBAL units. A z12 tile is
 *   65536 of those units on a side (~9.8 km at Michigan's latitude, so one unit
 *   is about 15 cm — u16 is nowhere near the limiting factor).
 *
 *   At display zoom z, one screen pixel is 2^(20-z) GLOBAL units. That single
 *   line is the whole zoom implementation.
 */
#include "map.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include <dirent.h>
#include "esp_log.h"
#include "esp_timer.h"

#include "hw.h"
#include "applog.h"

static const char *TAG = "map";

#define VMAP_DIR    SD_MOUNT "/vmap"
#define GLOBAL_BITS 28
#define GLOBAL      (1u << GLOBAL_BITS)

/* ---- state ------------------------------------------------------------- */

static int      s_zoom = 15;
static double   s_cx, s_cy;            /* centre, GLOBAL units (double: panning
                                        * at z17 moves fractions of a unit) */
static bool     s_have_centre;
static char     s_status[48] = "no map on the card";
static bool     s_checked;
static bool     s_present;

#define TRAIL_MAX 1024
static uint32_t s_trail[TRAIL_MAX][2];
static int      s_trail_n;
static bool     s_pin_set;
static uint32_t s_pin[2];

/* ---- tile cache -------------------------------------------------------- */

#define CACHE_N 9                      /* a 240x320 screen spans at most 4 z12
                                        * tiles; 9 leaves room to walk across a
                                        * boundary without re-reading */
typedef struct {
    int      lvl;
    int      tx, ty;
    uint8_t *buf;
    size_t   len;
    size_t   feat_end;                 /* features stop here, not at EOF */
    const uint8_t *strtab;             /* NULL on a nameless tile */
    size_t   strtab_len;
    uint16_t nstr;
    uint64_t used;                     /* LRU stamp */
    bool     valid;                    /* a MISS is cached too: a tile that is
                                        * not on the card must not be re-opened
                                        * every single frame */
} tile_t;

/* Resolve a feature's name index against its own tile's table.
 *
 * ⚠️ Every bound is checked against the file's own length. These bytes came off
 * a removable card that a PC has had write access to; a truncated or
 * half-copied tile must draw nothing, not walk off the end of a PSRAM buffer. */
static const char *tile_name(const tile_t *t, uint16_t idx)
{
    if (!t->strtab || idx == 0 || idx >= t->nstr) return NULL;
    size_t at = 2 + (size_t)idx * 2;
    if (at + 2 > t->strtab_len) return NULL;
    uint16_t off;
    memcpy(&off, t->strtab + at, 2);
    if (off >= t->strtab_len) return NULL;
    const char *sv = (const char *)(t->strtab + off);
    /* must be NUL-terminated inside the table */
    if (!memchr(sv, 0, t->strtab_len - off)) return NULL;
    return sv[0] ? sv : NULL;
}

static tile_t s_cache[CACHE_N];

/* ---- where a redraw actually spends its time ----------------------------
 *
 * 🔑 TWO SUSPECTS, AND THEY WANT OPPOSITE FIXES. A slow redraw is either card
 * I/O (fopen/fread of up to nine tiles, up to 2MB each) or the feature walk
 * (thousands of lv_canvas_draw_line calls). The 2026-09-18 research says move
 * I/O to a worker task; that is the WRONG fix if the walk is the expensive
 * half, because a worker task cannot touch LVGL and the drawing would stay
 * exactly where it is.
 *
 * So split the clock before choosing. map_draw_stats() reports the last
 * redraw: total, the part inside tile_get() (I/O, and only on a cache MISS),
 * and how many tiles missed. Everything else is the walk. */
static uint32_t s_ms_total, s_ms_io;
static uint16_t s_io_misses;

void map_draw_stats(uint32_t *total_ms, uint32_t *io_ms, uint16_t *misses)
{
    if (total_ms) *total_ms = s_ms_total;
    if (io_ms)    *io_ms    = s_ms_io;
    if (misses)   *misses   = s_io_misses;
}

static void merc(double lat, double lon, double *gx, double *gy)
{
    if (lat >  85.05) lat =  85.05;
    if (lat < -85.05) lat = -85.05;
    double s = sin(lat * M_PI / 180.0);
    *gx = (lon + 180.0) / 360.0 * (double)GLOBAL;
    *gy = (0.5 - log((1 + s) / (1 - s)) / (4 * M_PI)) * (double)GLOBAL;
}

static tile_t *tile_get(int lvl, int tx, int ty)
{
    uint64_t now = esp_timer_get_time();
    tile_t *lru = &s_cache[0];
    for (int i = 0; i < CACHE_N; i++) {
        tile_t *t = &s_cache[i];
        if (t->used && t->lvl == lvl && t->tx == tx && t->ty == ty) {
            t->used = now;
            return t;
        }
        if (t->used < lru->used) lru = t;
    }

    if (lru->buf) { free(lru->buf); lru->buf = NULL; }
    lru->lvl = lvl;
    lru->tx = tx; lru->ty = ty; lru->len = 0; lru->valid = false; lru->used = now;
    lru->strtab = NULL; lru->strtab_len = 0; lru->nstr = 0; lru->feat_end = 0;

    /* Past this point the tile was NOT cached and the card is about to be
     * touched. Everything from here to the end of the function is I/O. */
    int64_t io_t0 = esp_timer_get_time();
    s_io_misses++;

    char path[96];
    snprintf(path, sizeof(path), VMAP_DIR "/%d/%d/%d.vmp", lvl, tx, ty);
    FILE *f = fopen(path, "rb");
    if (!f) { s_ms_io += (uint32_t)((esp_timer_get_time() - io_t0) / 1000);
              return lru; }             /* cached miss, deliberately */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 16 || sz > 2 * 1024 * 1024) { fclose(f); s_ms_io += (uint32_t)((esp_timer_get_time() - io_t0) / 1000); return lru; }

    /* PSRAM: a dense tile is hundreds of KB and internal RAM on this board is
     * what WiFi and the audio path are already fighting over. */
    uint8_t *b = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { fclose(f); ESP_LOGW(TAG, "no PSRAM for %s", path); s_ms_io += (uint32_t)((esp_timer_get_time() - io_t0) / 1000); return lru; }
    size_t got = fread(b, 1, sz, f);
    fclose(f);

    if (got != (size_t)sz || memcmp(b, "VMP3", 4) != 0) {
        /* ⚠️ Names arrived in VMP3 (2026-09-13). A VMP2 card is not corrupt,
         * it is OLD, and saying so is the difference between a five-second fix
         * and an afternoon looking for a parser bug. */
        ESP_LOGW(TAG, "%s: %s", path,
                 !memcmp(b, "VMP2", 4) ? "VMP2 tiles - reconvert for names"
                                       : "not a tile");
        free(b);
        s_ms_io += (uint32_t)((esp_timer_get_time() - io_t0) / 1000);
        return lru;
    }

    /* Footer: the last 8 bytes name the string table. See osm2vmp.py for why
     * it is a footer and not a header field. */
    lru->feat_end = got;
    if (got > 24 && !memcmp(b + got - 8, "STRT", 4)) {
        uint32_t at;
        memcpy(&at, b + got - 4, 4);
        if (at >= 16 && at + 2 <= got - 8) {
            lru->strtab     = b + at;
            lru->strtab_len = (got - 8) - at;
            memcpy(&lru->nstr, b + at, 2);
            lru->feat_end   = at;
        }
    }
    lru->buf = b; lru->len = got; lru->valid = true;
    s_ms_io += (uint32_t)((esp_timer_get_time() - io_t0) / 1000);
    return lru;
}

/* ---- places: the city and town names ------------------------------------
 *
 * One flat file for the whole state, ~100 KB, held in PSRAM for the life of the
 * screen. 3,514 fixed-size records is nothing to scan linearly once a redraw,
 * and a flat array needs no index, no parsing and no cache. See osm2vmp.py's
 * write_places() for the layout. */
#define PLACES_PATH VMAP_DIR "/places.vmp"

static uint8_t *s_places;
static uint32_t s_place_n;
static uint32_t s_place_str;       /* offset of the string blob */
static bool     s_places_tried;

static void places_load(void)
{
    if (s_places_tried) return;
    s_places_tried = true;

    FILE *f = fopen(PLACES_PATH, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 16 || sz > 1024 * 1024) { fclose(f); return; }

    uint8_t *b = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) { fclose(f); return; }
    size_t got = fread(b, 1, sz, f);
    fclose(f);

    uint16_t n;
    uint32_t str;
    if (got != (size_t)sz || memcmp(b, "PLC1", 4)) { free(b); return; }
    memcpy(&n,   b + 4, 2);
    memcpy(&str, b + 8, 4);
    /* Every record and the whole blob must fit inside the file we actually
     * read. This came off a removable card. */
    if (str > got || 16 + (size_t)n * 16 > str) { free(b); return; }

    s_places = b; s_place_n = n; s_place_str = str;
    ESP_LOGI(TAG, "places: %u loaded", (unsigned)n);
}

/* ---- public ------------------------------------------------------------ */

bool map_available(void)
{
    if (s_checked) return s_present;
    s_checked = true;

    FILE *f = fopen(VMAP_DIR "/index.txt", "r");
    if (!f) {
        snprintf(s_status, sizeof(s_status), "no " VMAP_DIR);
        s_present = false;
        return false;
    }
    char line[64];
    int tiles = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "tiles %d", &tiles) == 1) break;
    }
    fclose(f);
    s_present = true;
    places_load();
    snprintf(s_status, sizeof(s_status), "%d tiles", tiles);
    applog("[map] %s", s_status);
    return true;
}

const char *map_status(void) { return s_status; }
int  map_zoom(void) { return s_zoom; }

/* ---- has anything on this map actually MOVED? ---------------------------
 *
 * 🚨 THE MAP REDREW TWICE A SECOND WHETHER OR NOT ANYTHING CHANGED, and a
 * redraw is the most expensive thing this firmware does on the UI task — up to
 * nine tiles walked and thousands of canvas lines, all inside
 * lv_timer_handler(), where it blocks touch sampling.
 *
 * Standing still with a fix, the centre is re-set to the same coordinates every
 * tick and the trail deliberately refuses points under ~5m, so most of those
 * redraws painted an identical screen. This counter bumps only when a redraw
 * would actually LOOK different; ui.c skips the draw when it has not moved.
 *
 * ⚠️ Bump on real change only. map_center_set() is called every tick while
 * Follow is on, so assigning unconditionally here would leave the counter
 * always dirty and the guard would silently do nothing — which looks exactly
 * like the optimisation not helping, rather than never running. */
static uint32_t s_gen = 1;

uint32_t map_generation(void) { return s_gen; }

void map_zoom_set(int z)
{
    if (z < MAP_Z_MIN) z = MAP_Z_MIN;
    if (z > MAP_Z_MAX) z = MAP_Z_MAX;
    if (z != s_zoom) { s_zoom = z; s_gen++; }
}

void map_center_set(double lat, double lon)
{
    double gx, gy;
    merc(lat, lon, &gx, &gy);
    /* Sub-pixel moves cannot change the picture. One pixel at the current zoom
     * is `scale` global units; anything smaller is not worth a redraw. */
    const double scale = (double)(1u << (20 - s_zoom));
    if (!s_have_centre ||
        fabs(gx - s_cx) >= scale || fabs(gy - s_cy) >= scale) {
        s_gen++;
    }
    s_cx = gx; s_cy = gy;
    s_have_centre = true;
}

void map_pan_px(int dx, int dy)
{
    if (!dx && !dy) return;
    double scale = (double)(1u << (20 - s_zoom));
    s_cx += dx * scale;
    s_cy += dy * scale;
    s_gen++;
}

void map_trail_add(double lat, double lon)
{
    double gx, gy;
    merc(lat, lon, &gx, &gy);
    uint32_t x = (uint32_t)gx, y = (uint32_t)gy;

    /* Don't record standing still: a stationary handset would fill the trail
     * with one point and lose the walk that came before it. ~5 m at this
     * latitude. */
    if (s_trail_n) {
        int32_t ddx = (int32_t)x - (int32_t)s_trail[s_trail_n - 1][0];
        int32_t ddy = (int32_t)y - (int32_t)s_trail[s_trail_n - 1][1];
        if (ddx * ddx + ddy * ddy < 32 * 32) return;
    }
    if (s_trail_n >= TRAIL_MAX) {       /* drop the oldest half, keep walking */
        memmove(s_trail, s_trail + TRAIL_MAX / 2,
                sizeof(s_trail[0]) * (TRAIL_MAX / 2));
        s_trail_n = TRAIL_MAX / 2;
    }
    s_trail[s_trail_n][0] = x;
    s_trail[s_trail_n][1] = y;
    s_trail_n++;
    s_gen++;
}

void map_trail_clear(void) { s_trail_n = 0; s_gen++; }
int  map_trail_count(void) { return s_trail_n; }

void map_pin_set(double lat, double lon)
{
    double gx, gy;
    merc(lat, lon, &gx, &gy);
    s_pin[0] = (uint32_t)gx; s_pin[1] = (uint32_t)gy;
    s_pin_set = true;
    s_gen++;
}

bool map_pin_get(double *lat, double *lon)
{
    if (!s_pin_set) return false;
    double x = (double)s_pin[0] / GLOBAL;
    double y = (double)s_pin[1] / GLOBAL;
    if (lon) *lon = x * 360.0 - 180.0;
    if (lat) *lat = atan(sinh(M_PI * (1 - 2 * y))) * 180.0 / M_PI;
    return true;
}

/* ---- drawing ----------------------------------------------------------- */

/* Road classes, as osm2vmp.py writes them. Width and colour are the only thing
 * separating a motorway from a driveway on a 240px screen with no labels.
 *
 * ⚠️ lv_color_make(), never a hand-packed uint16. LV_COLOR_16_SWAP=y on this
 * board: packed pixels come out green and magenta with perfect shapes. */
typedef struct { uint8_t w; uint8_t r, g, b; } road_style_t;

static const road_style_t ROAD[10] = {
    { 1, 0x60, 0x68, 0x74 },   /* 0 unused                */
    { 3, 0xF0, 0xA8, 0x30 },   /* 1 motorway              */
    { 3, 0xE0, 0x8A, 0x2A },   /* 2 trunk                 */
    { 2, 0xD8, 0xC0, 0x50 },   /* 3 primary               */
    { 2, 0xC0, 0xC8, 0xD0 },   /* 4 secondary             */
    { 2, 0xA0, 0xA8, 0xB4 },   /* 5 tertiary              */
    { 1, 0x88, 0x90, 0x9C },   /* 6 residential           */
    { 1, 0x60, 0x68, 0x74 },   /* 7 service               */
    { 1, 0x7A, 0x6A, 0x50 },   /* 8 track                 */
    { 1, 0x5A, 0x7A, 0x5A },   /* 9 path / cycleway       */
};

/* ---- labels -------------------------------------------------------------
 *
 * 🔑 HORIZONTAL TEXT AT THE MIDDLE OF THE LONGEST VISIBLE PIECE, not text bent
 * along the curve. On a 240px screen the bent kind is harder to read, not
 * easier, and it is the difference between an afternoon and a week. Each name
 * is drawn ONCE per screen however many features carry it — a road is split
 * into dozens of ways by junctions and speed limits, and labelling every one
 * of them paints the same street name forty times.
 *
 * Candidates are ranked by class first (a motorway's name matters more than a
 * cul-de-sac's) and by how much of it is on screen second. */
#define LBL_MAX      14
#define LBL_MIN_PX   44        /* shorter than this and the label outruns the road */
#define LBL_APART_PX 38        /* keep labels from piling up on one junction */

typedef struct {
    const char *txt;
    int16_t     x, y;
    int16_t     len;                  /* total visible px, summed over fragments */
    int16_t     best;                 /* the longest single fragment, for placing */
    uint8_t     cls;                  /* 0 = a place, 1..9 = a road class */
} label_t;

#define LBL_PLACE 0

/* Bigger than it was: nothing is rejected on arrival any more, so every named
 * road on screen takes a slot until the cap. */
static label_t s_lbl[LBL_MAX * 6];
static int     s_lbl_n;

/* 🚨 ACCUMULATE FIRST, MEASURE LAST. The length test used to live at the top of
 * this function, and that is why zooming out lost every road name.
 *
 * OSM splits one road into a separate way at every junction, speed limit change
 * and bridge. At z10 an interstate crossing the screen arrives as ~50 features
 * of about 4 px each — every one of them failed a 44 px minimum on its own and
 * was discarded BEFORE the dedupe could add them back together. At z12 the
 * fragments are individually long enough, which is exactly why the bug only
 * showed when zoomed out.
 *
 * So every fragment is taken, its length added to its name's running total, and
 * the minimum is applied in labels_draw() once the whole road has been seen. */
static void label_offer(const char *txt, int x, int y, int len, uint8_t cls)
{
    if (!txt || len <= 0) return;
    for (int i = 0; i < s_lbl_n; i++) {
        if (strcmp(s_lbl[i].txt, txt) == 0) {
            int total = s_lbl[i].len + len;
            s_lbl[i].len = (int16_t)(total > 32000 ? 32000 : total);
            if (len > s_lbl[i].best) {      /* place on the longest piece */
                s_lbl[i].best = (int16_t)len;
                s_lbl[i].x = (int16_t)x;
                s_lbl[i].y = (int16_t)y;
            }
            return;
        }
    }
    if (s_lbl_n >= (int)(sizeof(s_lbl) / sizeof(s_lbl[0]))) return;
    s_lbl[s_lbl_n++] = (label_t){ txt, (int16_t)x, (int16_t)y,
                                  (int16_t)len, (int16_t)len, cls };
}

/* Offer every visible place name. They go through the SAME placement pass as
 * the roads, at class 0, so a town name always beats a street name for the same
 * patch of screen — and the keep-apart rule stops a city and the road through
 * it printing on top of each other. */
static void places_offer(double left, double top, double scale, int w, int h)
{
    if (!s_places) return;
    for (uint32_t i = 0; i < s_place_n; i++) {
        const uint8_t *r = s_places + 16 + (size_t)i * 16;
        uint8_t minz = r[12];
        if (minz > s_zoom) continue;

        uint32_t gx, gy, noff;
        memcpy(&gx,   r + 0, 4);
        memcpy(&gy,   r + 4, 4);
        memcpy(&noff, r + 8, 4);

        int x = (int)(((double)gx - left) / scale);
        int y = (int)(((double)gy - top) / scale);
        if (x < 6 || y < 6 || x > w - 6 || y > h - 18) continue;

        size_t at = s_place_str + noff;
        if (at >= (size_t)s_place_str + 1 && noff < 0xFFFFF) {
            const char *nm = (const char *)(s_places + at);
            label_offer(nm, x, y + 7, 9999, LBL_PLACE);
        }
    }
}

static void labels_draw(lv_obj_t *canvas, int w, int h)
{
    /* insertion sort: at most a few dozen, and qsort would drag stdlib in for
     * nothing */
    for (int i = 1; i < s_lbl_n; i++) {
        label_t k = s_lbl[i];
        int j = i - 1;
        while (j >= 0 && (s_lbl[j].cls > k.cls ||
                          (s_lbl[j].cls == k.cls && s_lbl[j].len < k.len))) {
            s_lbl[j + 1] = s_lbl[j];
            j--;
        }
        s_lbl[j + 1] = k;
    }

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font  = &lv_font_montserrat_12;

    lv_draw_line_dsc_t dot;
    lv_draw_line_dsc_init(&dot);
    dot.width = 3;
    dot.color = lv_color_make(0xF0, 0xA8, 0x30);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_make(0x0D, 0x12, 0x1A);
    bg.bg_opa   = LV_OPA_70;
    bg.radius   = 2;

    int placed = 0;
    int16_t px[LBL_MAX], py[LBL_MAX];

    for (int i = 0; i < s_lbl_n && placed < LBL_MAX; i++) {
        /* The minimum belongs HERE, now that a road's fragments have all been
         * added together. A name longer than the road it names reads as noise. */
        if (s_lbl[i].cls != LBL_PLACE && s_lbl[i].len < LBL_MIN_PX) continue;

        lv_point_t sz;
        lv_txt_get_size(&sz, s_lbl[i].txt, dsc.font, 0, 0, LV_COORD_MAX,
                        LV_TEXT_FLAG_NONE);
        int x = s_lbl[i].x - sz.x / 2;
        int y = s_lbl[i].y - sz.y / 2;

        /* 🔑 CLAMP INTO VIEW, DO NOT DROP. The builder, 2026-09-13: *"none of the big
         * roads are labeled like m5 and grandriver or 9 mile rd"* — and they
         * were the two yellow lines along the BOTTOM of his screen. A major
         * road crossing a corner has its midpoint off the edge, so the label
         * was discarded, while every residential street with its midpoint in
         * the middle kept its name. The important roads were being rejected for
         * being important enough to run off the screen. */
        if (x < 2)              x = 2;
        if (y < 2)              y = 2;
        if (x + sz.x > w - 2)   x = w - 2 - sz.x;
        if (y + sz.y > h - 16)  y = h - 16 - sz.y;
        if (x < 2 || y < 2) continue;          /* genuinely too wide to fit */

        bool clash = false;
        for (int k = 0; k < placed; k++) {
            if (abs(px[k] - s_lbl[i].x) < LBL_APART_PX &&
                abs(py[k] - s_lbl[i].y) < 14) { clash = true; break; }
        }
        if (clash) continue;

        lv_area_t a = { x - 2, y - 1, x + sz.x + 2, y + sz.y + 1 };
        lv_canvas_draw_rect(canvas, a.x1, a.y1, a.x2 - a.x1, a.y2 - a.y1, &bg);

        /* A place gets amber text and a marker; a road gets plain white. Two
         * kinds of name on one screen have to be told apart at a glance. */
        if (s_lbl[i].cls == LBL_PLACE) {
            dsc.color = lv_color_make(0xF0, 0xA8, 0x30);
            lv_point_t d[2] = { { s_lbl[i].x - 1, s_lbl[i].y - 8 },
                                { s_lbl[i].x + 1, s_lbl[i].y - 8 } };
            lv_canvas_draw_line(canvas, d, 2, &dot);
        } else {
            dsc.color = lv_color_make(0xF2, 0xF6, 0xFC);
        }
        lv_canvas_draw_text(canvas, x, y, sz.x + 2, &dsc, s_lbl[i].txt);

        px[placed] = s_lbl[i].x;
        py[placed] = s_lbl[i].y;
        placed++;
    }
}

void map_draw(lv_obj_t *canvas, int w, int h, bool have_fix)
{
    const int64_t draw_t0 = esp_timer_get_time();
    s_ms_io = 0; s_io_misses = 0;

    s_lbl_n = 0;
    lv_canvas_fill_bg(canvas, lv_color_make(0x0D, 0x12, 0x1A), LV_OPA_COVER);

    if (!s_have_centre) { s_ms_total = 0; return; }

    const double scale = (double)(1u << (20 - s_zoom));   /* units per pixel */
    const double left = s_cx - (w / 2.0) * scale;
    const double top  = s_cy - (h / 2.0) * scale;
    const double right  = left + w * scale;
    const double bottom = top + h * scale;

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.opa = LV_OPA_COVER;

    /* Below z12 read the coarse level: 16x the ground per tile, major roads
     * only. Everything stays in z12-global units, so a coarse tile's stored
     * u16 is simply worth more of them — one shift, no second coordinate
     * system to get wrong. */
    const int lvl   = (s_zoom >= MAP_TILE_Z) ? MAP_TILE_Z : MAP_OVER_Z;
    const int ushl  = MAP_TILE_Z - lvl;                  /* 0 or 4 */
    const double span = (double)((uint32_t)MAP_SUB << ushl);

    int tx0 = (int)(left / span), tx1 = (int)(right / span);
    int ty0 = (int)(top / span),  ty1 = (int)(bottom / span);

    /* ⚠️ A HARD CEILING ON TILES PER REDRAW, even with the coarse level.
     * The overview keeps the normal cases small, but nothing else stops a
     * future zoom range or a denser extract from asking for thirty tiles again
     * and thrashing a nine-entry cache. Better to draw the middle of the screen
     * than to stop responding. */
    if ((tx1 - tx0 + 1) * (ty1 - ty0 + 1) > CACHE_N) {
        int cx = (int)(s_cx / span), cy = (int)(s_cy / span);
        int r = 1;
        while ((2 * r + 3) * (2 * r + 3) <= CACHE_N) r++;
        tx0 = cx - r; tx1 = cx + r;
        ty0 = cy - r; ty1 = cy + r;
    }

    int drawn = 0, missing = 0;
    for (int tx = tx0; tx <= tx1; tx++) {
        for (int ty = ty0; ty <= ty1; ty++) {
            tile_t *t = tile_get(lvl, tx, ty);
            if (!t->valid) { missing++; continue; }

            const double ox = (double)tx * span;
            const double oy = (double)ty * span;
            size_t o = 16;
            while (o + 6 <= t->feat_end) {
                uint8_t cls  = t->buf[o];
                uint8_t minz = t->buf[o + 1];
                uint16_t nameidx, np;
                memcpy(&nameidx, t->buf + o + 2, 2);
                memcpy(&np,      t->buf + o + 4, 2);
                o += 6;
                size_t bytes = (size_t)np * 4;
                if (o + bytes > t->feat_end) break;         /* truncated tile */

                if (minz > s_zoom || cls == 0 || cls > 9 || np < 2) {
                    o += bytes;
                    continue;
                }
                const char *name = tile_name(t, nameidx);
                int lbl_best = 0, lbl_x = 0, lbl_y = 0, lbl_total = 0;
                const road_style_t *st = &ROAD[cls];
                dsc.width = st->w;
                dsc.color = lv_color_make(st->r, st->g, st->b);

                lv_point_t a, b;
                bool have_a = false;
                for (int i = 0; i < np; i++) {
                    uint16_t px, py;
                    memcpy(&px, t->buf + o + i * 4, 2);
                    memcpy(&py, t->buf + o + i * 4 + 2, 2);
                    double gx = ox + ((uint32_t)px << ushl);
                    double gy = oy + ((uint32_t)py << ushl);
                    /* Clamp rather than skip: a segment with one end far off
                     * screen still has to be DRAWN, or long roads vanish
                     * exactly when you zoom in on them. */
                    double sx = (gx - left) / scale;
                    double sy = (gy - top) / scale;
                    if (sx < -4000) sx = -4000;
                    if (sx >  4000) sx =  4000;
                    if (sy < -4000) sy = -4000;
                    if (sy >  4000) sy =  4000;
                    b.x = (lv_coord_t)sx;
                    b.y = (lv_coord_t)sy;
                    if (have_a) {
                        bool off = (a.x < 0 && b.x < 0) || (a.x > w && b.x > w) ||
                                   (a.y < 0 && b.y < 0) || (a.y > h && b.y > h);
                        if (!off && (a.x != b.x || a.y != b.y)) {
                            lv_point_t seg[2] = { a, b };
                            lv_canvas_draw_line(canvas, seg, 2, &dsc);
                            drawn++;
                            if (name) {
                                int ddx = b.x - a.x, ddy = b.y - a.y;
                                int l2 = ddx * ddx + ddy * ddy;
                                /* Squared, to keep a sqrt out of a loop that
                                 * runs a few thousand times a redraw. */
                                if (l2 > lbl_best) {
                                    lbl_best = l2;
                                    lbl_x = (a.x + b.x) / 2;
                                    lbl_y = (a.y + b.y) / 2;
                                }
                                /* 🔑 RANK BY THE WHOLE VISIBLE ROAD, PLACE ON
                                 * ITS LONGEST PIECE. Ranking by the longest
                                 * SEGMENT is why zooming out lost every label:
                                 * geometry simplified for z13 has ~19 m between
                                 * points, and at z8 that is a fifth of a pixel,
                                 * so an interstate crossing the entire screen
                                 * was a chain of 1px segments and never cleared
                                 * the 44px bar. Its total length always does. */
                                lbl_total += (ddx < 0 ? -ddx : ddx) +
                                             (ddy < 0 ? -ddy : ddy);
                            }
                        }
                    }
                    a = b;
                    have_a = true;
                }
                if (name && lbl_best) {
                    label_offer(name, lbl_x, lbl_y, lbl_total, cls);
                }
                o += bytes;
            }
        }
    }

    /* Labels go on after every road, so a line drawn later cannot strike
     * through a name that was already placed. */
    places_offer(left, top, scale, w, h);
    labels_draw(canvas, w, h);

    /* ---- the walk -------------------------------------------------------
     * Drawn after the roads so it is never hidden by them. */
    if (s_trail_n > 1) {
        dsc.width = 2;
        dsc.color = lv_color_make(0x4C, 0xC2, 0xFF);
        lv_point_t prev = {0};
        bool have_prev = false;
        for (int i = 0; i < s_trail_n; i++) {
            lv_point_t p = {
                (lv_coord_t)(((double)s_trail[i][0] - left) / scale),
                (lv_coord_t)(((double)s_trail[i][1] - top) / scale),
            };
            if (have_prev) {
                lv_point_t seg[2] = { prev, p };
                lv_canvas_draw_line(canvas, seg, 2, &dsc);
            }
            prev = p;
            have_prev = true;
        }
    }

    /* ---- the pin: where the first fix landed ---------------------------- */
    if (s_pin_set) {
        int px = (int)(((double)s_pin[0] - left) / scale);
        int py = (int)(((double)s_pin[1] - top) / scale);
        if (px > -20 && px < w + 20 && py > -20 && py < h + 20) {
            dsc.width = 2;
            dsc.color = lv_color_make(0xFF, 0x5A, 0x5A);
            lv_point_t stem[2] = { { px, py }, { px, py - 14 } };
            lv_canvas_draw_line(canvas, stem, 2, &dsc);
            lv_point_t flag[2] = { { px, py - 14 }, { px + 9, py - 10 } };
            lv_canvas_draw_line(canvas, flag, 2, &dsc);
            lv_point_t flag2[2] = { { px + 9, py - 10 }, { px, py - 6 } };
            lv_canvas_draw_line(canvas, flag2, 2, &dsc);
        }
    }

    /* ---- you ------------------------------------------------------------
     * A ring, not a dot: a dot on a 1px road is indistinguishable from the
     * road. Hollow while there is no fix, so the screen never implies it knows
     * where it is when it does not. */
    {
        int cx = w / 2, cy = h / 2;
        dsc.width = have_fix ? 3 : 2;
        dsc.color = have_fix ? lv_color_make(0x5C, 0xE0, 0x7A)
                             : lv_color_make(0x7C, 0x87, 0x98);
        const int R = 7;
        lv_point_t ring[9];
        for (int i = 0; i < 9; i++) {
            double th = i * (2 * M_PI / 8);
            ring[i].x = (lv_coord_t)(cx + R * cos(th));
            ring[i].y = (lv_coord_t)(cy + R * sin(th));
        }
        for (int i = 0; i < 8; i++) {
            lv_point_t seg[2] = { ring[i], ring[i + 1] };
            lv_canvas_draw_line(canvas, seg, 2, &dsc);
        }
    }

    if (missing && !drawn) {
        ESP_LOGD(TAG, "no tiles for %d,%d..%d,%d", tx0, ty0, tx1, ty1);
    }

    s_ms_total = (uint32_t)((esp_timer_get_time() - draw_t0) / 1000);
}

/* ---- the card as an RF aggressor. See map.h. ----------------------------- */

#define PROBE_PATHS 48
#define PROBE_CHUNK (32 * 1024)

/* ⚠️ COORDINATES, NOT PATHS. A dirent name is 256 bytes wide as far as the
 * compiler is concerned, so storing 48 assembled paths is both huge and an
 * unavoidable -Wformat-truncation error. z12 tile indices fit in a uint16 and
 * the path is rebuilt from them at read time, which is what tile_get() does. */
static uint16_t s_probe_x[PROBE_PATHS];
static uint16_t s_probe_y[PROBE_PATHS];
static int      s_probe_n;
static int      s_probe_at;
static bool     s_probe_scanned;
static uint8_t *s_probe_buf;

void map_card_probe_reset(void)
{
    s_probe_n = s_probe_at = 0;
    s_probe_scanned = false;
}

/* Discover real tiles once. z12 deliberately: those are the big files the map
 * actually pulls (up to 330 KB), so the read pattern matches the screen that
 * provoked this whole investigation rather than some synthetic stream. */
static void probe_scan(void)
{
    s_probe_scanned = true;
    s_probe_n = 0;

    DIR *lvl = opendir(VMAP_DIR "/12");
    if (!lvl) { ESP_LOGW(TAG, "probe: no " VMAP_DIR "/12"); return; }

    struct dirent *dx;
    while (s_probe_n < PROBE_PATHS && (dx = readdir(lvl))) {
        if (dx->d_name[0] < '0' || dx->d_name[0] > '9') continue;
        int tx = atoi(dx->d_name);

        char sub[64];
        if (snprintf(sub, sizeof(sub), VMAP_DIR "/12/%d", tx) >= (int)sizeof(sub))
            continue;
        DIR *d = opendir(sub);
        if (!d) continue;

        struct dirent *dy;
        while (s_probe_n < PROBE_PATHS && (dy = readdir(d))) {
            if (dy->d_name[0] < '0' || dy->d_name[0] > '9') continue;
            s_probe_x[s_probe_n] = (uint16_t)tx;
            s_probe_y[s_probe_n] = (uint16_t)atoi(dy->d_name);
            s_probe_n++;
        }
        closedir(d);
    }
    closedir(lvl);
    ESP_LOGI(TAG, "probe: %d tiles", s_probe_n);
}

size_t map_card_probe(void)
{
    if (!s_probe_scanned) probe_scan();
    if (s_probe_n <= 0) return 0;

    if (!s_probe_buf) {
        s_probe_buf = heap_caps_malloc(PROBE_CHUNK,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_probe_buf) return 0;
    }

    int at = s_probe_at;
    s_probe_at = (s_probe_at + 1) % s_probe_n;

    char path[64];
    snprintf(path, sizeof(path), VMAP_DIR "/12/%u/%u.vmp",
             (unsigned)s_probe_x[at], (unsigned)s_probe_y[at]);

    FILE *f = fopen(path, "rb");
    if (!f) {
        /* A tile that vanished means the card list is stale - rescan next call
         * rather than spinning on a path that will never open again. */
        s_probe_scanned = false;
        return 0;
    }
    size_t total = 0, got;
    while ((got = fread(s_probe_buf, 1, PROBE_CHUNK, f)) > 0) total += got;
    fclose(f);
    return total;
}
