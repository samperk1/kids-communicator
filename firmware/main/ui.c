/*
 * The interface. Portrait 240x320, built to the mockups the builder approved:
 * amber on black, a persistent status bar with the battery meter, six list
 * rows, and a now-playing screen that leads with content rather than controls.
 *
 * Two screens do everything. The list screen is reused for artists, albums and
 * tracks — same widget tree, different data and title — because on a 240px
 * column three near-identical screens would only be three places for the same
 * bug to hide.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audio.h"
#include "lvgl.h"
#include "hw.h"
#include "display.h"
#include "library.h"
#include "player.h"
#include "faves.h"
#include "battery.h"
#include "net.h"
#include "settings.h"
#include "applog.h"
#include "voice.h"
#include "wt.h"
#include "radio.h"
#include "gps.h"
#include "gpslog.h"
#include "usbmsc.h"
#include "map.h"
#include <math.h>
#include "power.h"
#include "gw_wire.h"
#include "ask.h"
#include "msg.h"
#include "ble.h"
#include "t9.h"
#include "espnow.h"
#include "fleet.h"
#include "ui.h"
#include "games.h"
#include "art.h"

static const char *TAG = "ui";

/* ---- the approved palette ---- */
#define C_BG      lv_color_hex(0x080A0E)
#define C_PANEL   lv_color_hex(0x121722)
#define C_LINE    lv_color_hex(0x232C3A)
#define C_INK     lv_color_hex(0xE8EDF5)
#define C_MUTED   lv_color_hex(0x7C8798)
#define C_AMBER   lv_color_hex(0xF0A830)
#define C_BLUE    lv_color_hex(0x6E9BD1)
#define C_GREEN   lv_color_hex(0x58B368)
#define C_VIOLET  lv_color_hex(0xA78BFA)
#define C_RED     lv_color_hex(0xD9534F)
#define C_ONAMBER lv_color_hex(0x16120A)

#define ROW_H         45
#define BAR_H         24
#define FOOT_H        30
#define UI_LIST_H        (LCD_H - BAR_H - FOOT_H)
#define IDX_W         22                       /* the A-Z strip */
#define IDX_N         27                       /* '#' then A..Z */
#define IDX_ROW       (UI_LIST_H / IDX_N)
/* The up-next row: 14px left inset, then the text, then the favourite mark and
 * the shuffle button hard against the right edge. 240 - 14 - 26 (shuffle, at
 * -12) - 14 (mark, at -46) - 8 gap. */
#define PLNEXT_W      140

/* LEVEL_ALL_ALBUMS is a second ROOT, not a step below LEVEL_ARTIST: it lists
 * every album on the card by name and drops straight into its tracks. A
 * compilation has no single artist to file it under, so artist-first browsing
 * could only reach it through whoever sings track one. */
typedef enum { LEVEL_ARTIST = 0, LEVEL_ALBUM, LEVEL_TRACK, LEVEL_ALL_ALBUMS } level_t;

static lv_obj_t *scr_list, *scr_play, *scr_term, *scr_talk, *scr_wt, *scr_cal;
static lv_obj_t *scr_home, *scr_menu;
static lv_obj_t *scr_t9, *t9_title, *t9_batt;
/* Forward-declared: the 500ms tick refreshes the radio screen, and both live
 * further down the file than tick() does. */
static lv_obj_t *scr_radio, *radio_batt;
static lv_obj_t *scr_fleet;
static lv_obj_t *scr_gps, *gps_title, *gps_batt;
/* The map screen is built far below but the 2Hz tick up here has to know about
 * it — it both refreshes it and exempts it from screen blanking. */
static lv_obj_t *scr_map;
static void map_refresh(void);
/* The display-desense A/B lives with the GPS screen, far below, but the menu
 * row that starts it is built before it. */
static bool ab_run;
/* 🚨 THE BASELINE IS QUIET NOW, AND THAT IS THE WHOLE CHANGE.
 *
 * The first version toggled ONE subsystem with everything else still running,
 * so it could only ever measure a marginal cost on top of a noise floor it
 * never removed. Three legs, four runs, and the total the awake handset costs
 * was never once measured — because no block in any of them ever had the
 * handset quiet.
 *
 * What forced the rethink: The builder, 2026-09-15 — "the gps locks only come back if
 * i put the device in deep sleep, i have not had it come back after it loses
 * the conection unless it goes into deep sleep". The GPS module has no
 * power-enable pin (hw.h), so it stays powered and keeps tracking through deep
 * sleep while the ESP32, the panel, the SPI bus and PSRAM all go down. Deep
 * sleep is the ONLY condition that has ever silenced this handset around the
 * receiver — and it is exactly the set of things the old test held constant.
 *
 * So TOTAL runs first and is the headline: everything vs nothing. The other
 * three add ONE thing back to the quiet baseline, which is where a decode is
 * actually possible and therefore where a dB means something. */
typedef enum { AB_TOTAL = 0, AB_PANEL = 1, AB_SDCARD = 2, AB_RADIO = 3,
               AB_LEGS = 4 } ab_mode_t;
static ab_mode_t ab_mode;

/* Block geometry lives up here because the menu row's label needs it and that
 * row is built long before the experiment code below. */
#define AB_BLOCK_MS 30000
#define AB_BLOCKS   12                 /* six each way, 6 minutes per leg */

static void ab_start(void);
static const char *leg_name(ab_mode_t m);
static bool seq_running(void);
static int  seq_secs_left(void);
static void seq_begin(void);
static void seq_abort(const char *why);
static void fleet_refresh(void);
static void radio_refresh(void);
/* 🔑 ONE 1 Hz GATE, SHARED BY BOTH GPS SCREENS.
 *
 * ⚠️ It lived inside gps_refresh() as a local static, and the map screen — added
 * 2026-09-13 — therefore logged NOTHING. A ten-minute stationary test on the map
 * would have produced no file at all, which is indistinguishable from a test
 * nobody ran. The screen refresh is 2 Hz and the receiver only solves once a
 * second, so logging per refresh would write every solution twice and halve the
 * meaning of every samples= count in a MARK summary.
 *
 * The static is at file scope now so that opening one screen and then the other
 * cannot double-log either. */
static void gps_log_1hz(void)
{
    static uint32_t last_log;
    uint32_t nowms = (uint32_t)(esp_timer_get_time() / 1000);
    if ((int32_t)(nowms - last_log) >= 1000) {
        last_log = nowms;
        gpslog_tick();
    }
}

static void gps_refresh(void);
static lv_obj_t *home_title, *home_batt, *menu_title, *menu_batt;
static lv_obj_t *list_title, *list_batt, *list_rows, *list_foot, *list_back;
static lv_obj_t *list_index, *list_bubble;   /* the A-Z strip and its preview */
static lv_obj_t *play_title_lbl, *play_batt;
/* The cover, and which album it belongs to. -2 rather than -1: -1 is a REAL
 * player state ("nothing loaded") and would make an empty player look like it
 * already had the right art. */
static lv_obj_t *pl_art;
static int       pl_art_album = -2;
static int       pl_fav_album = -2, pl_fav_track = -2;
static lv_obj_t *pl_artist, *pl_track, *pl_album, *pl_bar, *pl_elapsed,
                *pl_remain, *pl_playbtn, *pl_next, *pl_sink, *pl_shuf, *pl_fav;
static lv_obj_t *term_title, *term_batt, *term_body;

static level_t s_level = LEVEL_ARTIST;
static char    s_letter = 0;      /* which A-Z bucket the artist list shows */
static bool    s_from_albums;     /* the flat list opened this track list */
static char    s_alb_letter = 0;  /* ... and the album list, kept separately so
                                   * switching mode does not lose your place */
static int s_artist = -1;         /* selected artist index          */
static int s_album  = -1;         /* selected GLOBAL album index    */

static void show_list(void);
static void list_title_clicked(lv_event_t *e);

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Edge-reaching click areas
 * ------------------------------------------------------------------ *
 *
 * The builder, 2026-09-07: "the edge of the screens is less sensitive than the centre
 * — objects at the edge are hard to select, it may take several trys before it
 * selects." Two things cause that, and this fixes the second one.
 *
 * The first is the mapping. hw.h has always said TOUCH_AX/BX were DERIVED
 * ALGEBRAICALLY AND NEVER MEASURED, and the fitted X slope is -1.1688 against a
 * measured Y slope of -1.0156. hw.h's own rule, eight lines above the constant,
 * is "a fitted slope far from +/-1 means this is wrong, not the presses" — and
 * a gain error is zero somewhere near the middle and grows towards both edges,
 * which is the reported symptom exactly. That needs measuring, not guessing;
 * it is not what this helper is for.
 *
 * The second is that a control at the edge has NO MARGIN FOR ERROR. LVGL's
 * LV_EVENT_CLICKED needs the press AND the release inside the object, and the
 * FT6236's reading wanders by a few pixels as a finger settles and lifts. In
 * the middle of the screen a stray pixel or two costs nothing. On a Back button
 * sitting 10px off the border there is nowhere for the error to go, the release
 * lands outside, and the tap is silently dropped — you press again, which is
 * what "several trys" is.
 *
 * 🚨 SO WHY NOT JUST PAD EVERY BUTTON? Because in LVGL 8.4
 * lv_obj_set_ext_click_area() takes ONE value and applies it to all four sides.
 * A blanket margin would make adjacent controls overlap, and where they overlap
 * the later-created object wins — so on the checkers board, the T9 keypad and
 * the games grids, neighbouring cells would start stealing each other's taps.
 * That is a worse bug than the one being fixed, and it would look identical to
 * a touch fault.
 *
 * So the growth is bounded by REAL EMPTY SPACE: the gap between the button and
 * the screen border it is already close to. That gap is by definition unoccupied
 * — nothing can be out there to steal from — and using it as the (symmetric)
 * grow amount keeps the inward growth just as small. A button that is not near
 * an edge is left completely alone, which is what keeps the grids safe. */
#define EDGE_NEAR_PX  20     /* "near an edge" — beyond this, do nothing */
#define EDGE_GROW_MAX 12     /* never grow more than this, however big the gap */

/* Gap between two rectangles along whichever axis they are separated on.
 * 0 if they already touch or overlap. */
static int area_gap(const lv_area_t *a, const lv_area_t *b)
{
    int dx = 0, dy = 0;
    if (b->x1 > a->x2) dx = b->x1 - a->x2;
    else if (a->x1 > b->x2) dx = a->x1 - b->x2;
    if (b->y1 > a->y2) dy = b->y1 - a->y2;
    else if (a->y1 > b->y2) dy = a->y1 - b->y2;

    if (dx && dy) return dx > dy ? dx : dy;   /* diagonal: the larger separation */
    return dx ? dx : dy;                      /* 0 means they touch or overlap */
}

/* Grow one control's click area into the empty space beside it.
 *
 * 🚨 THE SIBLING CLAMP IS THE WHOLE SAFETY OF THIS. The screen-edge gap says
 * how much room there is on the OUTSIDE; it says nothing about what sits on the
 * inside. On the T9 keypad and the games grids the leftmost column IS near the
 * left border, so an edge-gap-only rule would happily grow it 10px to the right
 * as well — straight over its neighbour. Halving the smallest sibling gap means
 * two adjacent controls growing towards each other still meet in the middle
 * and never overlap. */
static void grow_to_edge(lv_obj_t *b)
{
    lv_obj_t *parent = lv_obj_get_parent(b);
    if (!parent) return;

    lv_area_t a;
    lv_obj_get_coords(b, &a);

    int gaps[4] = {
        a.x1,                 /* to the left border   */
        LCD_W - 1 - a.x2,     /* to the right border  */
        a.y1,                 /* to the top border    */
        LCD_H - 1 - a.y2,     /* to the bottom border */
    };

    /* The smallest gap decides: that is the edge this control is up against,
     * and the free space there is all we may safely claim. */
    int grow = EDGE_NEAR_PX + 1;
    for (int i = 0; i < 4; i++)
        if (gaps[i] >= 0 && gaps[i] < grow) grow = gaps[i];

    if (grow > EDGE_NEAR_PX) return;               /* not an edge control */
    if (grow > EDGE_GROW_MAX) grow = EDGE_GROW_MAX;

    uint32_t n = lv_obj_get_child_cnt(parent);
    for (uint32_t i = 0; i < n && grow > 0; i++) {
        lv_obj_t *sib = lv_obj_get_child(parent, i);
        if (sib == b || !lv_obj_has_flag(sib, LV_OBJ_FLAG_CLICKABLE)) continue;
        lv_area_t sa;
        lv_obj_get_coords(sib, &sa);
        int half = area_gap(&a, &sa) / 2;
        if (half < grow) grow = half;
    }

    if (grow <= 0) return;    /* flush to the border, or shoulder to shoulder */
    lv_obj_set_ext_click_area(b, grow);
}

/* Walk a finished screen and grow every clickable thing on it.
 *
 * 🚨 CALLED AFTER THE SCREEN IS BUILT, NOT FROM THE BUTTON HELPERS. Three of
 * the four helpers align the button themselves, but mk_tbtn() does not — its
 * callers align afterwards. Growing inside the helper would therefore read
 * mk_tbtn's buttons at their pre-alignment coordinates, usually 0,0, and grow
 * the wrong ones by the wrong amount. Doing it in one pass at the end, after
 * lv_obj_update_layout(), is the only point at which every control on the
 * screen is where it will actually be when a finger arrives. */
static void grow_screen_edges(lv_obj_t *scr)
{
    if (!scr) return;
    lv_obj_update_layout(scr);

    uint32_t n = lv_obj_get_child_cnt(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) grow_to_edge(c);
        /* One level down catches buttons inside panels and rows. Deeper than
         * that is scrolling list content, where an ext click area would follow
         * the object off the visible area and is not wanted. */
        uint32_t m = lv_obj_get_child_cnt(c);
        for (uint32_t j = 0; j < m; j++) {
            lv_obj_t *g = lv_obj_get_child(c, j);
            if (lv_obj_has_flag(g, LV_OBJ_FLAG_CLICKABLE)) grow_to_edge(g);
        }
    }
}

static void clock_refresh(bool force);   /* defined with the status bar below */

/* Every screen change goes through here so no screen can be added later and
 * quietly miss the edge pass. Idempotent: the grow is recomputed from live
 * coordinates each time and lv_obj_get_coords() does not include a previously
 * set ext click area, so re-showing a screen cannot ratchet the margins up. */
static void ui_load(lv_obj_t *scr)
{
    lv_scr_load(scr);
    grow_screen_edges(scr);
    /* Forced: this screen's clock label may have been built before the time was
     * known, and a once-a-minute refresh would leave it blank for up to a
     * minute after it appears. */
    clock_refresh(true);
}

static void flat(lv_obj_t *o)
{
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}

/* Track names come off the card as filenames, so they carry ".mp3". Stripped
 * for DISPLAY ONLY — library_track_path() still needs the real filename to open
 * the file, so the stored name must not be touched. */
static const char *pretty(const char *name, char *buf, size_t len)
{
    strlcpy(buf, name, len);
    size_t n = strlen(buf);
    if (n > 4 && !strcasecmp(buf + n - 4, ".mp3")) buf[n - 4] = 0;
    return buf;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font,
                          lv_color_t colour, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, colour, 0);
    lv_label_set_text(l, text);
    return l;
}

/* The battery meter: colour carries the state as well as the number, so a kid
 * who skips the percentage still sees when it is nearly flat. */
/* 🔑 RADIO STATE IN THE STATUS BAR.
 *
 * The builder, 2026-08-15, after a whole evening of guessing: "there is nothing to tell
 * me if its not working". Talk brings WiFi up silently, and if association
 * fails the only symptom is a long pause and then a failure message that blames
 * the microphone. The RSSI number matters as much as the symbol, because this
 * board's antenna is loaded by anything placed near it and the whole question
 * has been "is the radio actually usable right now".
 *
 * Colour carries the verdict so it can be read at a glance:
 *   green  better than -60  comfortable
 *   amber  -60 to -75       works, but TCP uploads will suffer
 *   red    worse than -75    expect failures
 * Blank when the radio is down — which is most of the time, by design. */
static void wifi_refresh(lv_obj_t *lbl)
{
    if (!lbl) return;
    if (!net_is_up()) { lv_label_set_text(lbl, ""); return; }

    int rssi = net_rssi();
    lv_color_t col = rssi > -60 ? C_GREEN : (rssi > -75 ? C_AMBER : C_RED);
    lv_obj_set_style_text_color(lbl, col, 0);
    if (rssi == 0) lv_label_set_text(lbl, LV_SYMBOL_WIFI);
    else           lv_label_set_text_fmt(lbl, LV_SYMBOL_WIFI " %d", rssi);
}

static void batt_refresh(lv_obj_t *lbl)
{
    if (!lbl) return;
    /* The radio indicator rides along with the battery refresh — see
     * mk_statusbar(), which hangs it off this label's user data. */
    lv_obj_t *wifi = (lv_obj_t *)lv_obj_get_user_data(lbl);
    wifi_refresh(wifi);

    /* 🔌 USB FIRST, because it is the only one of the two that can be known for
     * certain. A floating GPIO9 on USB reads ~4150mV, i.e. exactly like a full
     * pack - so asking the ADC first would show a confident, wrong percentage.
     * usb_serial_jtag_is_connected() is a real answer; see battery_on_usb(). */
    if (battery_on_usb()) {
        lv_obj_set_style_text_color(lbl, C_BLUE, 0);
        lv_label_set_text(lbl, LV_SYMBOL_USB);
    } else if (!battery_fitted()) {
        /* No pack fitted: show a plug, not a number. The pin floats and reads a
         * confident-looking 93-95%, and a wrong percentage is worse than none. */
        lv_obj_set_style_text_color(lbl, C_MUTED, 0);
        lv_label_set_text(lbl, LV_SYMBOL_USB);
    } else {
        int pct = battery_percent();
        if (pct < 0) {
            lv_label_set_text(lbl, "--");
        } else {
            const char *icon;
            lv_color_t col;
            if (battery_charging())  { icon = LV_SYMBOL_CHARGE;        col = C_BLUE;  }
            else if (pct > 80)       { icon = LV_SYMBOL_BATTERY_FULL;  col = C_GREEN; }
            else if (pct > 60)       { icon = LV_SYMBOL_BATTERY_3;     col = C_GREEN; }
            else if (pct > 40)       { icon = LV_SYMBOL_BATTERY_2;     col = C_AMBER; }
            else if (pct > 20)       { icon = LV_SYMBOL_BATTERY_1;     col = C_AMBER; }
            else                     { icon = LV_SYMBOL_BATTERY_EMPTY; col = C_RED;   }
            lv_obj_set_style_text_color(lbl, col, 0);
            lv_label_set_text_fmt(lbl, "%s %d%%", icon, pct);
        }
    }

    /* 🚨 RE-ANCHOR THE WIFI LABEL EVERY TIME, NOT ONCE AT CREATION.
     *
     * mk_statusbar() aligns wifi to the LEFT of the battery label - but it does
     * that while the battery label is still EMPTY, i.e. zero width. The battery
     * text is right-anchored, so as it grows ("USB" -> "100%") it grows
     * leftwards, straight underneath the wifi indicator. The builder, 2026-08-16: "the
     * wifi indicator and battery indicator overlap each other".
     *
     * The label has its final text by this point, so realigning here is the one
     * place the width is actually known. */
    if (wifi) {
        lv_obj_update_layout(lbl);
        lv_obj_align_to(wifi, lbl, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    }
}

/* The log screen is reached by LONG-PRESSING the status bar title.
 *
 * Hidden on purpose. The log is a diagnostic for the builder, not a feature for a
 * seven-year-old — a visible button would be tapped constantly and would take
 * space from controls that matter. A long press is discoverable if you know,
 * and invisible if you don't. */
static void title_long_pressed(lv_event_t *e) { ui_show_term(); }
/* Long-press the BATTERY side to recalibrate the touchscreen. Hidden like the
 * log and for the same reason: a repair tool, not a feature. */
/* 🚨 LVGL'S LONG PRESS IS 400ms (LV_INDEV_DEF_LONG_PRESS_TIME), AND THIS TARGET
 * IS THE RIGHT HALF OF THE STATUS BAR ON EVERY SCREEN.
 *
 * That is the answer to the builder's "i just dont know why it keeps comming up":
 * resting a thumb on the top-right of the panel for four tenths of a second
 * opened a full-screen calibration that had no way out. Four tenths of a second
 * is not a deliberate gesture, it is a grip.
 *
 * LONG_PRESSED_REPEAT fires every ~100ms after that, so counting them is a
 * longer hold without touching the global threshold every other widget shares.
 * ~1.5s to ask, and the question still has to be answered. */
static int batt_hold_ticks;
static void batt_long_pressed(lv_event_t *e)
{
    (void)e;
    /* 🚨 1.5s WAS STILL A GRIP. The builder, 2026-09-05: the calibration "keeps
     * getting pressed on them when they have a good tuch calabration". A child
     * carrying the handset holds the top of the panel for a second and a half
     * without thinking about it. Raised to ~3.8s, which nobody does by
     * accident, and the question that follows now has to be HELD to answer. */
    if (++batt_hold_ticks == 35) ui_ask_cal();     /* 400ms + 34 x 100ms */
}
static void batt_released(lv_event_t *e) { (void)e; batt_hold_ticks = 0; }

/* ------------------------------------------------------------------ *
 * The clock in the status bar
 * ------------------------------------------------------------------ *
 *
 * ⚠️ REGISTERED, NOT PER-SCREEN — and that is a direct response to the warning
 * in ui_tick(). The battery and wifi indicators need a hand-written line there
 * for every screen, and forgetting one left home, menu and radio with blank
 * headers for days ("the battery and wifi header is only in the music menu").
 * A clock has exactly the same shape and would have caught exactly the same
 * bug, so mk_statusbar() registers each label as it builds it and one refresh
 * walks the lot. A screen added later gets a working clock for nothing.
 *
 * 🔑 ONCE A MINUTE, NOT TWICE A SECOND. ui_tick() runs at 2Hz. Repainting HH:MM
 * 120 times a minute is SPI traffic and CPU on a battery device, for a value
 * that changes 60 times an hour. The minute number is the guard.
 *
 * 🚨 BLANK, NOT 00:00, WHEN THERE IS NO CLOCK. Time comes from Home Assistant's
 * HTTP Date header, so a handset that has not reached HA since boot does not
 * know what time it is. Showing midnight would be a confident lie on the one
 * indicator people trust without thinking — and the whole point of the gateway
 * work is that handsets will soon get their time from somewhere else. */
static lv_obj_t *s_clocks[12];
static int       s_clocks_n;

/* 12-hour with AM/PM, by request. ⚠️ "12:34 PM" is 8 characters plus the
 * terminator, so every buffer holding this needs 9 — the 24-hour version needed
 * 6 and snprintf would have truncated the meridiem silently. */
/* ---- asking the gateway for the time ----
 *
 * 🚨 ONLY WHEN WE DO NOT ALREADY KNOW. A handset that has reached Home
 * Assistant has a clock already and must not pester the gateway; a handset in
 * the light build never will, and this is its only source. Either way the
 * question stops the moment it is answered.
 *
 * ⚠️ EVERY 30 SECONDS, NOT EVERY TICK. ui_tick() runs at 2Hz. Asking 120 times
 * a minute would put a unicast frame on the air twice a second, competing with
 * the kids' voice, to learn something that changes once a minute. */
#define GW_TIME_ASK_MS  30000

/* 🔬 Blast `frames` chunks at the gateway as fast as the radio will take them.
 *
 * ⚠️ esp_now_send() only QUEUES — the driver has a handful of buffers and
 * returns ESP_ERR_ESPNOW_NO_MEM once they are full. A loop that ignored that
 * would measure how fast this CPU can call a function that fails, which is a
 * very large and completely meaningless number. Retrying on NO_MEM is what
 * makes this a measurement of the RADIO rather than of the loop.
 *
 * Runs on its own task: it deliberately saturates the link for a second or two
 * and must not sit in the LVGL tick while it does. */
static void speed_blast_task(void *arg)
{
    uint16_t to    = (uint16_t)((uintptr_t)arg & 0xFFFF);
    uint16_t total = (uint16_t)((uintptr_t)arg >> 16);

    gw_speed_chunk_t c = { .ver = GW_VER, .op = GW_OP_SPEED };
    memset(c.fill, 0xA5, sizeof(c.fill));

    int64_t t0 = esp_timer_get_time();
    int sent = 0, refused = 0;

    for (uint16_t i = 0; i < total; i++) {
        c.index = i;
        int tries = 0;
        while (!espnow_gw_request(to, &c, sizeof(c))) {
            refused++;
            if (++tries > 2000) break;          /* the peer has gone */
            vTaskDelay(1);                      /* let the driver drain */
        }
        sent++;
    }

    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    int bytes = sent * (int)sizeof(gw_speed_chunk_t);
    applog("[speed] sent %d frames / %d bytes in %d ms = %d B/s "
           "(%d queue refusals)",
           sent, bytes, ms, ms ? (bytes * 1000) / ms : 0, refused);
    vTaskDelete(NULL);
}

static void gw_request_from_gateway(uint16_t from_id, const uint8_t *p, int len)
{
    if (len < 2 || p[0] != GW_VER || p[1] != GW_OP_SPEED) return;
    if (len < (int)sizeof(gw_speed_req_t)) return;
    gw_speed_req_t q;
    memcpy(&q, p, sizeof(q));
    if (!q.frames) return;
    applog("[speed] gateway asked for %u frames", q.frames);
    uintptr_t arg = ((uintptr_t)q.frames << 16) | from_id;
    xTaskCreate(speed_blast_task, "speed", 4096, (void *)arg, 4, NULL);
}

static void gw_time_response(uint16_t from_id, const uint8_t *p, int len)
{
    /* A speed request arrives as a RESPONSE frame because the gateway uses
     * espnow_gw_request() for it; both directions share one callback here. */
    if (len >= 2 && p[0] == GW_VER && p[1] == GW_OP_SPEED) {
        gw_request_from_gateway(from_id, p, len);
        return;
    }
    /* The voice relay's frames share this one callback too — route them before
     * the TIME decode, which would otherwise reject them on a size check and
     * lose them silently. */
    if (len >= 2 && p[0] == GW_VER && p[1] == GW_OP_ASK) {
        ask_on_response(from_id, p, len);
        return;
    }
    if (len < (int)sizeof(gw_time_rsp_t)) return;
    gw_time_rsp_t r;
    memcpy(&r, p, sizeof(r));
    if (r.ver != GW_VER || r.op != GW_OP_TIME) return;
    if (!r.ok) { applog("[gw] %04X does not know the time either", from_id); return; }
    power_time_set_unix(r.unix_secs);
    clock_refresh(true);           /* show it now, not at the next minute */
}

static void gw_time_ask(void)
{
    static int64_t last_us;

    if (power_time_valid()) return;
    int64_t now = esp_timer_get_time();
    if (last_us && now - last_us < (int64_t)GW_TIME_ASK_MS * 1000) return;

    uint16_t gw = espnow_gateway_id();
    if (!gw) return;               /* no gateway in range: nothing to ask */
    last_us = now;

    gw_req_t q = { .ver = GW_VER, .op = GW_OP_TIME, .seq = (uint8_t)(now / 1000) };
    if (espnow_gw_request(gw, &q, sizeof(q)))
        applog("[gw] asked %04X for the time", gw);
}

static void clock_text(char *out, size_t n)
{
    if (!power_time_valid()) { out[0] = 0; return; }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    /* tm_hour is 0-23. Midnight and noon are the two that catch people out:
     * 0 reads as 12 AM and 12 reads as 12 PM, neither of which falls out of a
     * plain modulo. */
    int h12 = tm.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    snprintf(out, n, "%d:%02d %s", h12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
}

/* force: repaint even if the minute has not turned. Used when a screen is
 * shown, because its label was built empty and would otherwise stay blank for
 * up to a minute. */
static void clock_refresh(bool force)
{
    static int  last_min   = -1;
    static bool last_valid = false;

    bool valid = power_time_valid();
    struct tm tm = { 0 };
    if (valid) { time_t now = time(NULL); localtime_r(&now, &tm); }

    if (!force && valid == last_valid && (!valid || tm.tm_min == last_min))
        return;
    last_valid = valid;
    last_min   = valid ? tm.tm_min : -1;

    char t[12];                 /* "12:34 PM" + NUL, with room to spare */
    clock_text(t, sizeof(t));
    for (int i = 0; i < s_clocks_n; i++)
        if (s_clocks[i]) lv_label_set_text(s_clocks[i], t);
}

/* One status bar, created identically on every screen. */
static void mk_statusbar(lv_obj_t *parent, lv_obj_t **title, lv_obj_t **batt,
                         const char *text)
{
    lv_obj_t *bar = lv_obj_create(parent);
    flat(bar);
    lv_obj_set_size(bar, LCD_W, BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, C_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, C_LINE, 0);
    lv_obj_set_style_border_width(bar, 1, 0);

    *title = mk_label(bar, &lv_font_montserrat_10, C_AMBER, text);
    lv_obj_align(*title, LV_ALIGN_LEFT_MID, 8, 0);

    /* A transparent hit area over the left half of the bar, rather than making
     * the label itself clickable: a label sized up to 120x24 would render its
     * text at the top of that box instead of centred. This keeps the layout
     * exactly as drawn and still gives a target worth aiming at. */
    lv_obj_t *hit = lv_obj_create(bar);
    flat(hit);
    lv_obj_set_size(hit, LCD_W / 2, BAR_H);
    lv_obj_align(hit, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, title_long_pressed, LV_EVENT_LONG_PRESSED, NULL);

    /* Centred: the title grows rightwards from the left and the battery block
     * grows leftwards from the right, so the middle is the only place a
     * fixed-width HH:MM cannot collide with either. */
    if (s_clocks_n < (int)(sizeof(s_clocks) / sizeof(s_clocks[0]))) {
        lv_obj_t *clk = mk_label(bar, &lv_font_montserrat_12, C_MUTED, "");
        lv_obj_align(clk, LV_ALIGN_CENTER, 0, 0);
        s_clocks[s_clocks_n++] = clk;
    }

    *batt = mk_label(bar, &lv_font_montserrat_12, C_GREEN, "");
    lv_obj_align(*batt, LV_ALIGN_RIGHT_MID, -8, 0);

    /* Radio indicator, immediately left of the battery. Hung off the battery
     * label's user data rather than threading another out-parameter through
     * mk_statusbar and all five of its callers — batt_refresh() already runs on
     * every screen at 500ms, so it is the natural place to refresh this too. */
    lv_obj_t *wifi = mk_label(bar, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_align_to(wifi, *batt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
    lv_obj_set_user_data(*batt, wifi);

    lv_obj_t *bhit = lv_obj_create(bar);
    flat(bhit);
    lv_obj_set_size(bhit, LCD_W / 2, BAR_H);
    lv_obj_align(bhit, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_flag(bhit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bhit, batt_long_pressed, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(bhit, batt_long_pressed, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_add_event_cb(bhit, batt_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(bhit, batt_released, LV_EVENT_PRESS_LOST, NULL);
}

/* ------------------------------------------------------------------ *
 * list screen
 * ------------------------------------------------------------------ */

static void row_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (s_level == LEVEL_ARTIST) {
        s_artist = idx;
        s_level = LEVEL_ALBUM;
        show_list();
    } else if (s_level == LEVEL_ALL_ALBUMS) {
        /* straight to the tracks — this list IS the album list */
        s_album = library_album_sorted(idx);
        if (s_album < 0) return;
        s_from_albums = true;
        s_level = LEVEL_TRACK;
        show_list();
    } else if (s_level == LEVEL_ALBUM) {
        const lib_artist_t *ar = library_artist(s_artist);
        s_album = ar->first_album + idx;      /* albums are addressed globally */
        s_from_albums = false;
        s_level = LEVEL_TRACK;
        show_list();
    } else {
        player_play(s_album, idx);
        ui_show_player();
    }
}

static void back_clicked(lv_event_t *e)
{
    /* 🔑 TRACKS HAVE TWO PARENTS NOW. Reached through an artist, Back goes to
     * that artist's albums; reached from the flat album list, it goes back to
     * the flat album list. s_from_albums remembers which door was used —
     * deriving it from s_artist would send you somewhere you never were. */
    if (s_level == LEVEL_TRACK)
        s_level = s_from_albums ? LEVEL_ALL_ALBUMS : LEVEL_ALBUM;
    else if (s_level == LEVEL_ALBUM)  s_level = LEVEL_ARTIST;
    else if (s_level == LEVEL_ALL_ALBUMS) { ui_show_player(); return; }
    else { ui_show_player(); return; }
    show_list();
}

/* ---- the A-Z index strip -------------------------------------------------
 *
 * 🚨 NOT 27 LITTLE BUTTONS. 27 buckets over 266 px of list height is a 9 px
 * target, and this file already documents at length why a small control near an
 * edge silently eats taps: LV_EVENT_CLICKED needs press AND release inside the
 * object, and the FT6236 wanders a few pixels as a finger lifts. A column of
 * 9 px buttons on the screen border would be the worst case of that bug we
 * have ever shipped.
 *
 * So the strip is ONE big clickable object and the letter is DERIVED FROM THE
 * Y COORDINATE. There is no small target to miss, a finger can slide to correct
 * itself, and the bubble shows which letter is under it the whole time.
 *
 * 🔑 THE LIST REBUILDS ON RELEASE, NOT WHILE DRAGGING. Each rebuild constructs
 * every row of a bucket; doing that on every PRESSING event would rebuild the
 * list dozens of times during one swipe down the strip. While the finger is
 * down only the bubble moves, which costs one label update. */

static char idx_letter_at(lv_coord_t y_abs)
{
    lv_area_t a;
    lv_obj_get_coords(list_index, &a);
    int slot = (y_abs - a.y1) / IDX_ROW;
    if (slot < 0) slot = 0;
    if (slot >= IDX_N) slot = IDX_N - 1;
    return slot == 0 ? '#' : (char)('A' + slot - 1);
}

/* One strip, two lists. Everything below asks the CURRENT root which letters
 * it has, so the same control indexes artists or albums without a second copy. */
static bool idx_on_albums(void) { return s_level == LEVEL_ALL_ALBUMS; }
static char idx_sel(void)       { return idx_on_albums() ? s_alb_letter : s_letter; }
static void idx_set(char c)     { if (idx_on_albums()) s_alb_letter = c; else s_letter = c; }
static bool idx_range(char c, int *first, int *count)
{
    return idx_on_albums() ? library_album_letter_range(c, first, count)
                           : library_letter_range(c, first, count);
}

static void idx_paint(void)
{
    uint32_t n = lv_obj_get_child_cnt(list_index);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *l = lv_obj_get_child(list_index, i);
        char c = (i == 0) ? '#' : (char)('A' + i - 1);
        int cnt = 0;
        idx_range(c, NULL, &cnt);
        /* an empty letter stays visible but dim, so the alphabet does not jump
         * around as the card changes */
        lv_obj_set_style_text_color(l,
            c == idx_sel() ? C_AMBER : (cnt ? C_INK : C_LINE), 0);
    }
}

static void idx_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *dev = lv_indev_get_act();
    if (!dev) return;
    lv_point_t p;
    lv_indev_get_point(dev, &p);
    char c = idx_letter_at(p.y);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        char txt[2] = { c, 0 };
        lv_label_set_text(list_bubble, txt);
        lv_obj_clear_flag(list_bubble, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    /* RELEASED and PRESS_LOST both end the gesture. PRESS_LOST matters: a
     * finger that slides off the strip still deserves the letter it was on. */
    lv_obj_add_flag(list_bubble, LV_OBJ_FLAG_HIDDEN);
    int cnt = 0;
    idx_range(c, NULL, &cnt);
    /* An empty letter does nothing at all rather than showing an empty list -
     * sliding past Q and X should not blank the screen. */
    if (cnt == 0 || c == idx_sel()) return;
    idx_set(c);
    idx_paint();
    show_list();
}

/* Builds every row into a scrolling container.
 *
 * This replaced a 6-row paged list. Paging was the wrong model: the natural
 * gesture on a touchscreen is a swipe, and a swipe on a paged list just
 * selected whatever row it started on — you cannot page a list you cannot
 * scroll. LVGL distinguishes a drag from a tap by its own scroll threshold, so
 * rows stay tappable while the list scrolls under a finger.
 *
 * The counts are small enough that building them all is cheap: 62 artists is
 * the largest list in the builder's library, and a track list is one album's worth. */
static void show_list(void)
{
    lv_obj_clean(list_rows);

    int total, base = 0;
    char head[64];
    const char *heading;
    if (s_level == LEVEL_ARTIST) {
        /* 🔑 THE STRIP FILTERS, IT DOES NOT JUST SCROLL. This card carries 519
         * artists and every row here is four LVGL objects built up front - the
         * comment below still says 62 because that is what the list was written
         * for. Showing one letter keeps a build at ~41 rows instead of 519,
         * which is the only reason this screen stays instant. */
        int first, count;
        if (!library_letter_range(s_letter, &first, &count)) { first = 0; count = 0; }
        base = first; total = count;
        snprintf(head, sizeof(head), "Artists %c " LV_SYMBOL_REFRESH, s_letter);
        heading = head;
    } else if (s_level == LEVEL_ALL_ALBUMS) {
        int first, count;
        if (!library_album_letter_range(s_alb_letter, &first, &count)) { first = 0; count = 0; }
        base = first; total = count;
        snprintf(head, sizeof(head), "Albums %c " LV_SYMBOL_REFRESH, s_alb_letter);
        heading = head;
    } else if (s_level == LEVEL_ALBUM) {
        total = library_album_count(s_artist);
        heading = library_artist(s_artist)->name;
    } else {
        total = library_track_count(s_album);
        heading = "Tracks";
    }
    lv_label_set_text(list_title, heading);

    bool idx_on = (s_level == LEVEL_ARTIST || s_level == LEVEL_ALL_ALBUMS);
    if (list_index) {
        if (idx_on) lv_obj_clear_flag(list_index, LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_add_flag(list_index, LV_OBJ_FLAG_HIDDEN);
    }
    /* rows stop short of the strip so the chevron does not sit under it */
    lv_coord_t row_w = idx_on ? (LCD_W - IDX_W) : LCD_W;

    lv_obj_scroll_to_y(list_rows, 0, LV_ANIM_OFF);

    for (int k = 0; k < total; k++) {
        int idx = base + k;          /* artist rows carry the GLOBAL index, so
                                      * row_clicked() needs no translation */
        char name[128], sub[64];
        if (s_level == LEVEL_ARTIST) {
            const lib_artist_t *a = library_artist(idx);
            strlcpy(name, a->name, sizeof(name));
            snprintf(sub, sizeof(sub), "%d albums  %d tracks",
                     a->n_albums, a->n_tracks);
        } else if (s_level == LEVEL_ALL_ALBUMS) {
            int g = library_album_sorted(idx);
            const lib_album_t *b = library_album_by_index(g);
            if (!b) continue;
            strlcpy(name, b->name, sizeof(name));
            /* the artist is the useful second line here, because the whole
             * point of this list is that you did not come in through one */
            const lib_artist_t *a = library_artist(b->artist);
            /* sub[] is 64 and an artist name is 128 — bound it explicitly
             * rather than let the row silently lose its track count. */
            snprintf(sub, sizeof(sub), "%.40s  %d tracks",
                     a ? a->name : "", b->n_tracks);
        } else if (s_level == LEVEL_ALBUM) {
            const lib_album_t *b = library_album(s_artist, idx);
            strlcpy(name, b->name, sizeof(name));
            snprintf(sub, sizeof(sub), "%d tracks", b->n_tracks);
        } else {
            const lib_track_t *t = library_track(s_album, idx);
            pretty(t->name, name, sizeof(name));
            sub[0] = 0;
        }

        lv_obj_t *row = lv_obj_create(list_rows);
        flat(row);
        lv_obj_set_size(row, row_w, ROW_H);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_color(row, C_LINE, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        /* Pressed state is an amber left edge, not a filled row, so the label
         * keeps full contrast while a finger is on it. */
        lv_obj_set_style_bg_color(row, C_PANEL, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_LEFT,
                                     LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, C_AMBER, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(row, 3, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)idx);

        lv_obj_t *n = mk_label(row, &lv_font_montserrat_14, C_INK, name);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_width(n, row_w - 34);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 12, sub[0] ? -8 : 0);

        if (sub[0]) {
            lv_obj_t *s = mk_label(row, &lv_font_montserrat_10, C_MUTED, sub);
            lv_obj_align(s, LV_ALIGN_LEFT_MID, 12, 9);
        }

        lv_obj_t *c = mk_label(row, &lv_font_montserrat_12, C_MUTED,
                               s_level == LEVEL_TRACK ? LV_SYMBOL_PLAY
                                                      : LV_SYMBOL_RIGHT);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -10, 0);
    }

    const char *noun = (s_level == LEVEL_ARTIST) ? "artists"
                     : (s_level == LEVEL_ALL_ALBUMS) ? "albums"
                     : (s_level == LEVEL_ALBUM)  ? "albums" : "tracks";
    lv_label_set_text_fmt(list_foot, "%d %s", total, noun);
}

static void build_list_screen(void)
{
    scr_list = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_list, C_BG, 0);
    lv_obj_clear_flag(scr_list, LV_OBJ_FLAG_SCROLLABLE);

    mk_statusbar(scr_list, &list_title, &list_batt, "Artists");
    /* The heading doubles as the Artists/Albums switch — see
     * list_title_clicked(). A generous hit area because it is a text label in
     * the status bar, and the bar's right half is the calibration hold, which
     * this must not reach: the title is left-aligned and the grow is 10px. */
    lv_obj_add_flag(list_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(list_title, 10);
    lv_obj_add_event_cb(list_title, list_title_clicked, LV_EVENT_CLICKED, NULL);

    list_rows = lv_obj_create(scr_list);
    flat(list_rows);
    lv_obj_set_size(list_rows, LCD_W, UI_LIST_H);
    lv_obj_align(list_rows, LV_ALIGN_TOP_MID, 0, BAR_H);
    lv_obj_set_flex_flow(list_rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list_rows, 0, 0);
    /* flat() clears SCROLLABLE — put it back, vertical only, with a scrollbar
     * so it is visible that there is more below. */
    lv_obj_add_flag(list_rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list_rows, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list_rows, C_AMBER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list_rows, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list_rows, 4, LV_PART_SCROLLBAR);

    /* The A-Z strip, over the right edge of the list. One clickable object; the
     * letters inside it are decoration and must never take a press themselves,
     * or we are back to 9 px targets. */
    list_index = lv_obj_create(scr_list);
    flat(list_index);
    lv_obj_set_size(list_index, IDX_W, UI_LIST_H);
    lv_obj_align(list_index, LV_ALIGN_TOP_RIGHT, 0, BAR_H);
    lv_obj_set_style_bg_color(list_index, C_PANEL, 0);
    lv_obj_set_style_bg_opa(list_index, LV_OPA_COVER, 0);
    lv_obj_add_flag(list_index, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < IDX_N; i++) {
        char t[2] = { i == 0 ? '#' : (char)('A' + i - 1), 0 };
        lv_obj_t *l = mk_label(list_index, &lv_font_montserrat_10, C_MUTED, t);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, i * IDX_ROW);
    }
    lv_obj_add_event_cb(list_index, idx_event, LV_EVENT_PRESSED,    NULL);
    lv_obj_add_event_cb(list_index, idx_event, LV_EVENT_PRESSING,   NULL);
    lv_obj_add_event_cb(list_index, idx_event, LV_EVENT_RELEASED,   NULL);
    lv_obj_add_event_cb(list_index, idx_event, LV_EVENT_PRESS_LOST, NULL);

    /* The preview. Big, centred, and NOT clickable so it cannot swallow the
     * drag that is drawing it. */
    list_bubble = mk_label(scr_list, &lv_font_montserrat_40, C_AMBER, "");
    lv_obj_set_style_bg_color(list_bubble, C_PANEL, 0);
    lv_obj_set_style_bg_opa(list_bubble, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(list_bubble, 14, 0);
    lv_obj_set_style_radius(list_bubble, 8, 0);
    lv_obj_set_style_border_color(list_bubble, C_AMBER, 0);
    lv_obj_set_style_border_width(list_bubble, 1, 0);
    lv_obj_clear_flag(list_bubble, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(list_bubble, LV_ALIGN_CENTER, -IDX_W / 2, 0);
    lv_obj_add_flag(list_bubble, LV_OBJ_FLAG_HIDDEN);

    /* Back and paging live in their own bar, away from the list, so a thumb
     * sliding down the rows never lands on Back. */
    lv_obj_t *foot = lv_obj_create(scr_list);
    flat(foot);
    lv_obj_set_size(foot, LCD_W, FOOT_H);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot, C_PANEL, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, C_LINE, 0);
    lv_obj_set_style_border_width(foot, 1, 0);

    list_back = lv_btn_create(foot);
    flat(list_back);
    lv_obj_set_size(list_back, 80, 26);
    lv_obj_align(list_back, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(list_back, back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(list_back, &lv_font_montserrat_12, C_BLUE,
                            LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    list_foot = mk_label(foot, &lv_font_montserrat_10, C_MUTED, "");
    lv_obj_align(list_foot, LV_ALIGN_RIGHT_MID, -8, 0);
}

/* ------------------------------------------------------------------ *
 * now playing
 * ------------------------------------------------------------------ */

static void play_clicked(lv_event_t *e)  { player_toggle(); }
static void next_clicked(lv_event_t *e)  { player_next(); }
static void prev_clicked(lv_event_t *e)  { player_prev(); }
static void browse_clicked(lv_event_t *e){ ui_show_list(); }
/* The player page's middle button. It used to be a second way into
 * Comms, which already has its own tile on the home grid - so the slot
 * was spent on a duplicate. Games live here instead, reachable only
 * from the music screen, and their Back comes straight back to it. */
static void games_clicked(lv_event_t *e) { games_show_menu(); }
/* ⚠️ There used to be a button here called "Radio" that opened the WALKIE-TALKIE.
 * Talkie now lives on the home grid and this slot returns to the hub instead;
 * "Radio" from here on means INTERNET radio and nothing else. */
static void home_from_play_clicked(lv_event_t *e) { ui_show_home(); }

static void sink_clicked(lv_event_t *e)
{
    const player_state_t *st = player_state();
    player_set_sink(st->sink == SINK_SPEAKER ? SINK_LINEOUT : SINK_SPEAKER);
}

/* The heart. LVGL 8's symbol font has no heart glyph and adding one means
 * rebuilding a font, so the STATE IS THE COLOUR - the same language the shuffle
 * button already uses, which this file notes "reads as on/off from across a
 * room". Amber filled = in favourites. */
static void fav_paint(void)
{
    if (!pl_fav) return;
    const player_state_t *st = player_state();
    bool on = st && st->album >= 0 && faves_contains(st->album, st->track);
    /* The box is always dark so the mark is always legible over the artwork;
     * only the CHECK changes colour — amber when the song is a favourite, muted
     * when it is not. */
    lv_obj_t *l = lv_obj_get_child(pl_fav, 0);
    /* 🚨 GREY-TO-AMBER WAS NOT A DIFFERENCE YOU COULD SEE. The builder: "it just stays
     * gray ... so you dont know if a song has been faverated or not." At 14px
     * on a dark panel, muted and amber read as the same dim mark. Green is the
     * furthest thing on this palette from the off state and means the same
     * here as it does on the shuffle button's Faves scope. */
    if (l) lv_obj_set_style_text_color(l, on ? C_GREEN : C_MUTED, 0);
}

static void fav_clicked(lv_event_t *e)
{
    (void)e;
    const player_state_t *st = player_state();
    if (!st || st->album < 0) return;          /* nothing loaded to heart */
    bool now = faves_toggle(st->album, st->track);
    fav_paint();
    /* ⚠️ The write happens HERE rather than being left to a later flush. The
     * deferred design in faves.c exists to keep writes off the card while audio
     * streams from it, but a favourite the user cannot see survive a power cut
     * is worse than one short write - and this is a single tap, not a loop.
     * player_state()->playing tells us which case we are in. */
    if (!st->playing) faves_flush();
    if (pl_next)
        lv_label_set_text(pl_next, now ? "Added to favourites"
                                       : "Removed from favourites");
    /* The long press that got here also ends as a CLICK on the title, which
     * nothing else uses — but say what happened, because a long press with no
     * feedback reads as a dropped tap. */
}

/* Off -> Album -> Artist -> Card -> Faves -> Off. One button rather than a settings
 * screen: this is the control a child reaches for while music is already
 * playing, and four taps to get back to Off is no worse than finding a menu. */
/* 🔑 THE SCOPE IS THE STATE, SO THE SCOPE GETS THE COLOUR. The builder, 2026-09-21:
 * "the shufful should change when its been activated ... you know if its on
 * aritst or album". On/off was never enough — four scopes that behave very
 * differently all looked identical once lit. The box stays dark and the SYMBOL
 * carries it, the same way the favourite check does, so the two controls read
 * as one family:
 *     off Album Artist Card Faves
 *   grey  blue  amber  violet green
 * Faves is green on purpose: it is the same green as the check, which is what
 * fills that scope. The word is still written into the line beside it, so
 * nobody has to learn the colours to use it. */
static lv_color_t shuf_colour(player_shuffle_t m)
{
    switch (m) {
        case SHUFFLE_ALBUM:  return C_BLUE;
        case SHUFFLE_ARTIST: return C_AMBER;
        case SHUFFLE_CARD:   return C_VIOLET;
        case SHUFFLE_FAVES:  return C_GREEN;
        default:             return C_MUTED;
    }
}

static void shuf_paint(void)
{
    if (!pl_shuf) return;
    lv_obj_t *l = lv_obj_get_child(pl_shuf, 0);
    if (l) lv_obj_set_style_text_color(l, shuf_colour(player_shuffle()), 0);
}

static void shuffle_clicked(lv_event_t *e)
{
    (void)e;
    player_shuffle_t m = player_shuffle();
    m = (m == SHUFFLE_OFF)    ? SHUFFLE_ALBUM
      : (m == SHUFFLE_ALBUM)  ? SHUFFLE_ARTIST
      : (m == SHUFFLE_ARTIST) ? SHUFFLE_CARD
      : (m == SHUFFLE_CARD)   ? SHUFFLE_FAVES
                              : SHUFFLE_OFF;
    /* Skip Faves when there is nothing hearted yet, rather than offering a mode
     * that silently behaves like Off. */
    if (m == SHUFFLE_FAVES && faves_count() == 0) m = SHUFFLE_OFF;
    player_set_shuffle(m);
    shuf_paint();
    /* Name the scope where there is room for the word. The tick rewrites this
     * line, so it is set here for the immediate feedback of the tap and the
     * tick keeps it right afterwards. */
    if (pl_next && m != SHUFFLE_OFF)
        lv_label_set_text_fmt(pl_next, "Shuffle: %s", player_shuffle_name(m));
}

/* ---- the volume row, on every screen that makes a sound -------------------
 *
 * The builder, 2026-08-25: "could we add a volume control to talkie and radio so we can
 * ajust the volume in the other 2 menuse". Right - the volume lived only on Now
 * Playing, so turning the radio down meant leaving the radio, and the walkie
 * talkie could not be turned down at all while somebody was talking through it.
 *
 * 🔑 ONE SETTING, SEVERAL SLIDERS. There is one volume in the hardware and one
 * in NVS, so these are three VIEWS of the same number, not three controls. They
 * are registered here and every one of them is repainted whenever any one of
 * them moves — otherwise the radio would open showing a stale position and the
 * first touch would jump the volume to wherever the knob happened to sit.
 *
 * ⚠️ Safe to hold pointers like this ONLY because these screens are built once
 * and kept. The games screens are freed on exit and must never do this. */
#define VOL_ROWS 3
static struct { lv_obj_t *icon, *slider; } s_vol_rows[VOL_ROWS];
static int s_vol_rows_n;

static void vol_rows_sync(void)
{
    bool m = player_muted();
    int  v = player_state()->volume;
    for (int i = 0; i < s_vol_rows_n; i++) {
        if (s_vol_rows[i].slider)
            lv_slider_set_value(s_vol_rows[i].slider, v, LV_ANIM_OFF);
        if (!s_vol_rows[i].icon) continue;
        lv_label_set_text(s_vol_rows[i].icon,
                          m ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_color(s_vol_rows[i].icon, m ? C_RED : C_MUTED, 0);
    }
}

static void vol_changed(lv_event_t *e)
{
    player_set_volume(lv_slider_get_value(lv_event_get_target(e)));
    vol_rows_sync();                 /* the slider is also the way out of mute */
}

static void vol_icon_clicked(lv_event_t *e)
{
    (void)e;
    player_set_mute(!player_muted());
    vol_rows_sync();
}

/* ---- SPK / JACK, on the screens that now obey it --------------------------
 *
 * Radio and the walkie-talkie follow the sink setting as of today, and the only
 * control for it lived on Now Playing — so hearing the radio in headphones
 * meant leaving the radio to go and change it. Two states only here: the third,
 * BT, is a music-only thing (the dongle wants MP3 frames and these paths carry
 * decoded PCM), and offering it where it cannot work would be a lie. Choosing
 * one of these on a board that was set to BT moves it out of BT, which is what
 * somebody pressing it plainly means. */
#define SINK_BTNS 2
static lv_obj_t *s_sink_btns[SINK_BTNS];
static int s_sink_btns_n;

static void sink_btns_sync(void)
{
    bool jack = player_state()->sink == SINK_LINEOUT;
    for (int i = 0; i < s_sink_btns_n; i++) {
        lv_obj_t *l = lv_obj_get_child(s_sink_btns[i], 0);
        if (l) lv_label_set_text(l, jack ? "JACK" : "SPK");
        lv_obj_set_style_bg_color(s_sink_btns[i], jack ? C_BLUE : C_PANEL, 0);
    }
}

static void sink_btn_clicked(lv_event_t *e)
{
    (void)e;
    player_set_sink(player_state()->sink == SINK_LINEOUT ? SINK_SPEAKER
                                                         : SINK_LINEOUT);
    sink_btns_sync();
}

static void mk_sink_btn(lv_obj_t *parent, int x, int y)
{
    if (s_sink_btns_n >= SINK_BTNS) return;
    lv_obj_t *b = lv_btn_create(parent);
    flat(b);
    lv_obj_set_size(b, 46, 22);
    lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, x, y);
    lv_obj_set_style_bg_color(b, C_PANEL, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_add_event_cb(b, sink_btn_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = mk_label(b, &lv_font_montserrat_12, C_INK, "SPK");
    lv_obj_center(l);
    s_sink_btns[s_sink_btns_n++] = b;
    sink_btns_sync();
}

/* Icon on the left, slider filling the width to its right, both hung off the
 * same corner so a screen only has to say how far up from the bottom it wants
 * them. Returns nothing: the row is found through the registry, not by name. */
static void mk_volume_row(lv_obj_t *parent, int x, int y, int w)
{
    if (s_vol_rows_n >= VOL_ROWS) return;

    lv_obj_t *ic = mk_label(parent, &lv_font_montserrat_12, C_MUTED,
                            LV_SYMBOL_VOLUME_MAX);
    lv_obj_align(ic, LV_ALIGN_BOTTOM_LEFT, x, y);
    /* Tap to mute. 12px of glyph is far too small to hit, so it gets 16px of
     * invisible click area — the Back button lesson from earlier today. CLICKED
     * rather than RELEASED because the slider is right beside it and a drag
     * that starts on the icon must not toggle. */
    lv_obj_add_flag(ic, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(ic, 16);
    lv_obj_add_event_cb(ic, vol_icon_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sl = lv_slider_create(parent);
    lv_obj_set_size(sl, w, 6);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, x + 24, y - 6);
    lv_obj_set_style_bg_color(sl, C_LINE, 0);
    lv_obj_set_style_bg_color(sl, C_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, C_BLUE, LV_PART_KNOB);
    lv_slider_set_range(sl, 0, 100);
    lv_slider_set_value(sl, player_state()->volume, LV_ANIM_OFF);
    lv_obj_add_event_cb(sl, vol_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_vol_rows[s_vol_rows_n].icon   = ic;
    s_vol_rows[s_vol_rows_n].slider = sl;
    s_vol_rows_n++;
}

static lv_obj_t *mk_tbtn(lv_obj_t *parent, int size, bool filled,
                         const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_set_style_bg_color(b, filled ? C_AMBER : C_PANEL, 0);
    lv_obj_set_style_border_color(b, filled ? C_AMBER : C_LINE, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = mk_label(b, filled ? &lv_font_montserrat_16
                                     : &lv_font_montserrat_14,
                           filled ? C_ONAMBER : C_INK, sym);
    lv_obj_center(l);
    return b;
}

static void build_play_screen(void)
{
    scr_play = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_play, C_BG, 0);
    lv_obj_clear_flag(scr_play, LV_OBJ_FLAG_SCROLLABLE);

    mk_statusbar(scr_play, &play_title_lbl, &play_batt, "Now Playing");

    /* 🔑 CREATED FIRST, SO IT IS BEHIND EVERYTHING. LVGL draws siblings in
     * creation order, so the cover has to exist before a single label does —
     * lv_obj_move_background() would work too, but only until something else is
     * added below it and quietly lands underneath. Order is the honest fix. */
    pl_art = lv_img_create(scr_play);
    lv_obj_center(pl_art);
    /* Dim. The art is a wash behind the text, not a picture with text on top:
     * everything on this screen has to stay readable by a six-year-old, and a
     * bright cover behind pale text is unreadable at any size. */
    lv_obj_set_style_img_opa(pl_art, settings_art_opa(), 0);
    lv_obj_add_flag(pl_art, LV_OBJ_FLAG_HIDDEN);

    pl_artist = mk_label(scr_play, &lv_font_montserrat_10, C_BLUE, "");
    lv_obj_align(pl_artist, LV_ALIGN_TOP_LEFT, 14, BAR_H + 14);

    pl_track = mk_label(scr_play, &lv_font_montserrat_22, C_INK, "Nothing playing");
    lv_label_set_long_mode(pl_track, LV_LABEL_LONG_DOT);
    lv_obj_set_width(pl_track, LCD_W - 28);
    lv_obj_align(pl_track, LV_ALIGN_TOP_LEFT, 14, BAR_H + 30);

    pl_album = mk_label(scr_play, &lv_font_montserrat_12, C_MUTED, "");
    lv_label_set_long_mode(pl_album, LV_LABEL_LONG_DOT);
    lv_obj_set_width(pl_album, LCD_W - 28);
    lv_obj_align(pl_album, LV_ALIGN_TOP_LEFT, 14, BAR_H + 62);

    pl_bar = lv_bar_create(scr_play);
    lv_obj_set_size(pl_bar, LCD_W - 28, 4);
    lv_obj_align(pl_bar, LV_ALIGN_TOP_MID, 0, BAR_H + 92);
    lv_obj_set_style_bg_color(pl_bar, C_LINE, 0);
    lv_obj_set_style_bg_color(pl_bar, C_AMBER, LV_PART_INDICATOR);
    lv_bar_set_range(pl_bar, 0, 100);
    lv_bar_set_value(pl_bar, 0, LV_ANIM_OFF);

    pl_elapsed = mk_label(scr_play, &lv_font_montserrat_10, C_MUTED, "0:00");
    lv_obj_align(pl_elapsed, LV_ALIGN_TOP_LEFT, 14, BAR_H + 100);
    pl_remain = mk_label(scr_play, &lv_font_montserrat_10, C_MUTED, "");
    lv_obj_align(pl_remain, LV_ALIGN_TOP_RIGHT, -14, BAR_H + 100);

    /* Portrait's payoff: stacked below the title instead of fighting it for a
     * row, so these can be 58/44px instead of cramped. */
    lv_obj_t *prev = mk_tbtn(scr_play, 44, false, LV_SYMBOL_PREV, prev_clicked);
    lv_obj_align(prev, LV_ALIGN_TOP_MID, -70, BAR_H + 128);

    pl_playbtn = mk_tbtn(scr_play, 58, true, LV_SYMBOL_PLAY, play_clicked);
    lv_obj_align(pl_playbtn, LV_ALIGN_TOP_MID, 0, BAR_H + 121);

    lv_obj_t *nx = mk_tbtn(scr_play, 44, false, LV_SYMBOL_NEXT, next_clicked);
    lv_obj_align(nx, LV_ALIGN_TOP_MID, 70, BAR_H + 128);

    /* This row now carries the shuffle button and the favourite mark on its
     * right, so the text gets what is left and SCROLLS rather than being cut.
     * A track name is the one string here that is regularly too long, and "…"
     * hides exactly the end that tells two versions apart. */
    pl_next = mk_label(scr_play, &lv_font_montserrat_10, C_MUTED, "");
    lv_label_set_long_mode(pl_next, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(pl_next, PLNEXT_W);
    lv_obj_align(pl_next, LV_ALIGN_TOP_LEFT, 14, BAR_H + 196);

    /* 🚨 THE BOTTOM ROW HAS A BUDGET: 240px, four controls, and no overlap.
     * The first version put Browse at LEFT+8, Talk at MID, Radio at MID+62 and
     * the sink chip at RIGHT-8 — which drew Radio straight on top of the sink
     * chip (The builder's photo shows "...ER" from SPEAKER bleeding out behind it) and
     * both over the volume slider. Now: three equal 74px buttons on one row,
     * sink and volume on the row beneath. 6 + 74*3 + 4*2 + 6 = 242, so 3px
     * margins and 4px gutters. Anything added here needs a row, not a nudge. */
    lv_obj_t *browse = lv_btn_create(scr_play);
    flat(browse);
    lv_obj_set_size(browse, 74, 30);
    lv_obj_align(browse, LV_ALIGN_BOTTOM_LEFT, 3, -46);
    lv_obj_set_style_bg_color(browse, C_PANEL, 0);
    lv_obj_set_style_bg_opa(browse, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(browse, 4, 0);
    lv_obj_add_event_cb(browse, browse_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *brl = mk_label(browse, &lv_font_montserrat_12, C_BLUE, "Browse");
    lv_obj_center(brl);

    pl_sink = lv_btn_create(scr_play);
    flat(pl_sink);
    lv_obj_set_size(pl_sink, 66, 24);
    lv_obj_align(pl_sink, LV_ALIGN_BOTTOM_LEFT, 3, -12);
    lv_obj_set_style_bg_color(pl_sink, C_PANEL, 0);
    lv_obj_set_style_bg_opa(pl_sink, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pl_sink, 4, 0);
    lv_obj_add_event_cb(pl_sink, sink_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = mk_label(pl_sink, &lv_font_montserrat_12, C_BLUE, "SPEAKER");
    lv_obj_center(sl);

    /* 🚨 NOT ON THE BOTTOM ROW — THE VOLUME SLIDER OWNS IT.
     *
     * The first attempt put this at BOTTOM_RIGHT and landed straight on top of
     * the volume row, which spans x 76..204 (`mk_volume_row(scr_play, 76, -14,
     * 128)`). The builder: "the button is under the volume key it needs to be some
     * where that its not in the way".
     *
     * The bottom of this screen is fully committed: Browse/Games/Radio at y-46
     * and SPEAKER + volume at y-12. The only genuinely free space is the 28px
     * gaps flanking the transport buttons — prev occupies x 28..72, play
     * 91..149, next 168..212 — so this sits in the right-hand gap, level with
     * NEXT, where the thumb already is.
     *
     * ⚠️ 28px is too narrow for "Artist", so the BUTTON carries only the symbol
     * and the SCOPE IS NAMED IN THE `pl_next` LINE below it, which already had
     * a full screen width of text doing nothing when shuffle is on (there is no
     * meaningful "up next" once the order is random). Nothing is hidden: a
     * child sees a lit button AND a word.
     *
     * RELEASED not CLICKED, plus a 14px invisible margin: a small target loses
     * presses when a finger rolls on the way up, and small fingers roll more —
     * see "lvgl clicked vs released". */
    /* 🚨 THERE IS NO ROOM ON THIS SCREEN FOR ANOTHER PADDED BUTTON, AND
     * MEASURING IT SAID SO.
     *
     * The left gap looked like the mirror of shuffle's right gap. It is not:
     * prev spans x 28..72, so a 30px button at x=2 overlaps it by 4px before
     * any margin, and the 14px ext_click_area shuffle uses pushes the hit area
     * to x 46 — eating the left third of PREV. This file's own warning says the
     * LATER-CREATED object wins an overlap, which pl_fav would be.
     *
     * ⚠️ AND THE SAME IS ALREADY TRUE OF SHUFFLE: pl_shuf at x 208..238 with
     * ext 14 covers x 194..252 against next's 168..212 — about 41% of NEXT.
     * That is a live bug, not a new one, and a plausible part of "the buttons
     * need several presses".
     *
     * The rest of the column is full: artist 38, title 54, album 86, bar 116,
     * elapsed 124, transport 152. The free bands are 16 and 17px tall. So the
     * heart is NOT a button:
     *   - a non-interactive INDICATOR on the artist row, which needs no touch
     *     target at all and so can be tucked anywhere; and
     *   - a LONG PRESS ON THE SONG TITLE, a 212x26 target that collides with
     *     nothing and is the thing you are already looking at.
     * A long press also cannot be hit while reaching for the transport row,
     * which a 30px corner button demonstrably can. */
    /* 🚨 A BARE GLYPH HAS NOTHING TO SIT ON. The builder, 2026-09-21: "the fav button is
     * almost invisable ... the transparint box is not visable on most
     * backgrounds." The album cover is drawn behind this whole row, so an
     * unbacked label competes with whatever the artwork happens to be — and at
     * the muted colour it vanished entirely. Same opaque panel the shuffle
     * button has always had, which is exactly why THAT one has always been
     * readable. Box stays dark; the CHECK carries the state in colour. */
    /* ⚠️ IT LOOKS LIKE A BUTTON, SO IT HAD BETTER BE ONE. Once the check got an
     * opaque panel it became indistinguishable from the shuffle button beside
     * it, and the builder pressed it — the long press on the title was the only way to
     * favourite anything and nothing happened, which reads as "the check is
     * broken". Both work now: tap the check, or long-press the title.
     * Moved from -44 to -50 so its 6px margin only TOUCHES shuffle's instead of
     * overlapping it — where two hit areas overlap the later object wins, and
     * shuffle is created after this one. */
    pl_fav = lv_obj_create(scr_play);
    flat(pl_fav);
    lv_obj_set_size(pl_fav, 26, 26);
    lv_obj_align(pl_fav, LV_ALIGN_TOP_RIGHT, -50, BAR_H + 184);
    lv_obj_add_flag(pl_fav, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(pl_fav, 6);
    lv_obj_add_event_cb(pl_fav, fav_clicked, LV_EVENT_RELEASED, NULL);
    lv_obj_set_style_bg_color(pl_fav, C_PANEL, 0);
    lv_obj_set_style_bg_opa(pl_fav, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pl_fav, 4, 0);
    lv_obj_center(mk_label(pl_fav, &lv_font_montserrat_14, C_MUTED,
                           LV_SYMBOL_OK));

    lv_obj_add_flag(pl_track, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pl_track, fav_clicked, LV_EVENT_LONG_PRESSED, NULL);

    /* 🚨 MOVED OFF THE TRANSPORT ROW, 2026-09-21, AND THAT IS A BUG FIX.
     * At TOP_RIGHT/-2/BAR_H+135 this button spanned x 208..238 against next's
     * 168..212 — a 4px overlap at rest, and 194..252 once the 14px
     * ext_click_area is counted, which is about 41% of NEXT. Where two objects
     * overlap the later-created one wins, and that is this button. It has sat
     * there swallowing part of NEXT, which is the shape of the long-standing
     * "the buttons need several presses".
     * Down here on the up-next row it clears the transport buttons entirely,
     * and the margin is 6 rather than 14 so it also clears the Radio button
     * below (y 244) with room to spare. */
    pl_shuf = lv_btn_create(scr_play);
    flat(pl_shuf);
    lv_obj_set_size(pl_shuf, 26, 26);
    lv_obj_align(pl_shuf, LV_ALIGN_TOP_RIGHT, -12, BAR_H + 184);
    lv_obj_set_style_bg_color(pl_shuf, C_PANEL, 0);
    lv_obj_set_style_bg_opa(pl_shuf, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(pl_shuf, 4, 0);
    lv_obj_set_ext_click_area(pl_shuf, 6);
    lv_obj_add_event_cb(pl_shuf, shuffle_clicked, LV_EVENT_RELEASED, NULL);
    lv_obj_t *shl = mk_label(pl_shuf, &lv_font_montserrat_14, C_BLUE,
                             LV_SYMBOL_SHUFFLE);
    lv_obj_center(shl);

    /* Games sits between Browse and Home. Comms is not lost - it keeps its
     * own tile on the home grid; this slot was a duplicate of it. */
    lv_obj_t *talkb = lv_btn_create(scr_play);
    flat(talkb);
    lv_obj_set_size(talkb, 74, 30);
    lv_obj_align(talkb, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_set_style_bg_color(talkb, C_AMBER, 0);
    lv_obj_set_style_bg_opa(talkb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(talkb, 4, 0);
    lv_obj_add_event_cb(talkb, games_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *tkl = mk_label(talkb, &lv_font_montserrat_12, C_ONAMBER, "Games");
    lv_obj_center(tkl);

    lv_obj_t *radiob = lv_btn_create(scr_play);
    flat(radiob);
    lv_obj_set_size(radiob, 74, 30);
    lv_obj_align(radiob, LV_ALIGN_BOTTOM_RIGHT, -3, -46);
    lv_obj_set_style_bg_color(radiob, C_GREEN, 0);
    lv_obj_set_style_bg_opa(radiob, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(radiob, 4, 0);
    lv_obj_add_event_cb(radiob, home_from_play_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rdl = mk_label(radiob, &lv_font_montserrat_12, C_ONAMBER, "Home");
    lv_obj_center(rdl);

    /* ⚠️ NO POINTER KEPT. The old pl_vol handle is gone, and deliberately: the
     * screens are built radio, play, then walkie-talkie, so "the player's
     * slider" is NOT s_vol_rows[0] and anything reaching for it by index would
     * have been quietly driving the radio's. vol_rows_sync() repaints them all
     * and nothing needs to name one. */
    mk_volume_row(scr_play, 76, -14, 128);
}

/* ------------------------------------------------------------------ *
 * talk screen
 *
 * One big button that is held, not tapped. Hold-to-talk is the right model for
 * a child: there is no "am I still recording?" state to misread, and letting go
 * is the same gesture as finishing a sentence.
 *
 * 🚨 THE PRESS AND RELEASE HANDLERS MUST NOT BLOCK. Recording spans the whole
 * press and the network round trip is 8-15s; doing either here freezes LVGL and
 * the device looks crashed at exactly the moment a kid is waiting. They only
 * flip flags — voice.c does the work on its own task.
 * ------------------------------------------------------------------ */

static lv_obj_t *talk_title, *talk_batt, *talk_btn, *talk_btn_lbl,
                *talk_stage, *talk_heard, *talk_reply, *talk_scroll;

static void talk_pressed(lv_event_t *e)  { voice_press(); }
static void talk_released(lv_event_t *e) { voice_release(); }
static void talk_back_clicked(lv_event_t *e) { ui_show_home(); }

static void build_talk_screen(void)
{
    scr_talk = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_talk, C_BG, 0);
    lv_obj_clear_flag(scr_talk, LV_OBJ_FLAG_SCROLLABLE);

    mk_statusbar(scr_talk, &talk_title, &talk_batt, "Comms");

    talk_stage = mk_label(scr_talk, &lv_font_montserrat_14, C_MUTED,
                          "Hold the button and ask");
    lv_label_set_long_mode(talk_stage, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(talk_stage, LCD_W - 28);
    lv_obj_set_style_text_align(talk_stage, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(talk_stage, LV_ALIGN_TOP_MID, 0, BAR_H + 12);

    /* 140px. Nothing else on this screen competes with it, and a child aiming
     * one-handed at a device in the other hand is not a stylus user. */
    talk_btn = lv_btn_create(scr_talk);
    lv_obj_set_size(talk_btn, 140, 140);
    lv_obj_set_style_radius(talk_btn, 70, 0);
    lv_obj_set_style_bg_color(talk_btn, C_AMBER, 0);
    lv_obj_set_style_bg_color(talk_btn, C_RED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(talk_btn, 0, 0);
    lv_obj_set_style_shadow_width(talk_btn, 0, 0);
    lv_obj_align(talk_btn, LV_ALIGN_TOP_MID, 0, BAR_H + 52);
    lv_obj_add_event_cb(talk_btn, talk_pressed,  LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(talk_btn, talk_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(talk_btn, talk_released, LV_EVENT_PRESS_LOST, NULL);

    talk_btn_lbl = mk_label(talk_btn, &lv_font_montserrat_16, C_ONAMBER, "HOLD\nTO TALK");
    lv_obj_set_style_text_align(talk_btn_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(talk_btn_lbl);

    /* 🚨 THE ANSWER WAS BEING CUT OFF, NOT MERELY UNSCROLLABLE.
     *
     * The screen is 240x320 and the HOLD TO TALK button takes 140 of it, so the
     * reply label sat at y=256 with a 30px footer underneath — about two lines.
     * A perfectly ordinary answer ("Yes, I can hear you loud and clear! How can
     * I help you today?") wraps to four or five, and the rest simply was not
     * on the display. The builder asked to be able to scroll it; it also needed
     * somewhere to scroll TO.
     *
     * Both texts now live in one scrollable column, so a long answer is read by
     * dragging rather than lost. ⚠️ Only this container scrolls — the talk
     * button is NOT inside it. A scrollable parent eats a slightly-dragged
     * press before the child ever sees CLICKED, which is exactly what stopped
     * the Fleet row unpairing on 2026-09-08; the button must stay outside. */
    talk_scroll = lv_obj_create(scr_talk);
    flat(talk_scroll);
    lv_obj_set_size(talk_scroll, LCD_W, LCD_H - FOOT_H - (BAR_H + 200));
    lv_obj_align(talk_scroll, LV_ALIGN_TOP_MID, 0, BAR_H + 200);
    lv_obj_add_flag(talk_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(talk_scroll, LV_DIR_VER);
    lv_obj_set_flex_flow(talk_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(talk_scroll, 6, 0);
    lv_obj_set_style_pad_hor(talk_scroll, 14, 0);

    talk_heard = mk_label(talk_scroll, &lv_font_montserrat_12, C_BLUE, "");
    lv_label_set_long_mode(talk_heard, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(talk_heard, LCD_W - 28);

    talk_reply = mk_label(talk_scroll, &lv_font_montserrat_12, C_INK, "");
    lv_label_set_long_mode(talk_reply, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(talk_reply, LCD_W - 28);

    lv_obj_t *foot = lv_obj_create(scr_talk);
    flat(foot);
    lv_obj_set_size(foot, LCD_W, FOOT_H);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot, C_PANEL, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, C_LINE, 0);
    lv_obj_set_style_border_width(foot, 1, 0);

    lv_obj_t *b = lv_btn_create(foot);
    flat(b);
    lv_obj_set_size(b, 80, 26);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(b, talk_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(b, &lv_font_montserrat_12, C_BLUE,
                            LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
}

/* Runs at 60ms. Cheap on purpose: one state read, and a call that is a no-op
 * unless a press is genuinely outstanding. */
static void talk_poll(lv_timer_t *t)
{
    if (voice_stage() == VOICE_LISTENING &&
        !lv_obj_has_state(talk_btn, LV_STATE_PRESSED))
        voice_release();
}

static void talk_refresh(void)
{
    static const char *TEXT[] = {
        [VOICE_IDLE]      = "Hold the button and ask",
        [VOICE_LISTENING] = "Listening...",
        [VOICE_THINKING]  = "Thinking...",
        [VOICE_SPEAKING]  = "Speaking",
        [VOICE_ERROR]     = "Didn't catch that - try again",
    };
    voice_stage_t st = voice_stage();
    const char *note = voice_note();
    lv_label_set_text(talk_stage, note[0] ? note : (TEXT[st] ? TEXT[st] : ""));
    lv_obj_set_style_text_color(talk_stage,
                                st == VOICE_ERROR ? C_RED : C_MUTED, 0);
    lv_label_set_text(talk_heard, voice_heard());
    lv_label_set_text(talk_reply, voice_reply());
    /* A new answer starts at the top rather than wherever the last one was
     * dragged to — otherwise the second question looks like it produced nothing. */
    static char last_reply_seen[32];
    if (talk_scroll && strncmp(last_reply_seen, voice_reply(), sizeof(last_reply_seen) - 1)) {
        strlcpy(last_reply_seen, voice_reply(), sizeof(last_reply_seen));
        lv_obj_scroll_to_y(talk_scroll, 0, LV_ANIM_OFF);
    }

    /* 🚨 POLL THE BUTTON'S ACTUAL STATE — do not trust the RELEASE EVENT alone.
     *
     * Measured on hardware 2026-08-14: The builder let go and the device stayed in
     * LISTENING indefinitely, because LV_EVENT_RELEASED / PRESS_LOST never
     * arrived. An event that does not fire cannot be waited for, and the whole
     * interaction hangs off it. LVGL's own LV_STATE_PRESSED is authoritative
     * and is re-evaluated every frame from the touch driver, so checking it
     * here recovers from a dropped event instead of hanging on one.
     *
     * voice_release() is idempotent — it returns immediately unless a press is
     * actually outstanding — so calling it every 500ms is free. */
    if (st == VOICE_LISTENING && !lv_obj_has_state(talk_btn, LV_STATE_PRESSED))
        voice_release();

    /* The screen must not blank while the assistant is still working — the
     * user is holding still and waiting, which is exactly what the idle timer
     * reads as inactivity. */
    if (st != VOICE_IDLE) display_note_activity();
}

/* ------------------------------------------------------------------ *
 * touch calibration
 *
 * 🚨 THIS EXISTS BECAUSE DERIVED CONSTANTS WERE WRONG. hw.h carried
 * "X IS DERIVED, NOT MEASURED" — computed when the display was un-mirrored and
 * never checked against a finger. Measured 2026-08-14: taps on the bottom
 * button row mapped to y=313-327 when those buttons live at y=244-274, and the
 * mapped x barely moved across the whole width. It stayed hidden for so long
 * because everything that worked was either full-width (list rows) or dead
 * centre (play/pause, the big talk button).
 *
 * Four corner targets. The fit also DECIDES THE AXIS SWAP rather than assuming
 * it: whichever raw axis moves more between the left and right targets is the
 * one driving screen x. Assuming that relationship is exactly what produced
 * calibration slopes of -32.7 and 17.3 from perfectly good presses during the
 * first display gate.
 * ------------------------------------------------------------------ */

#define CAL_INSET 32
/* How far a tap may sit from its own crosshair once the fit is applied. Four
 * honest taps land within a few pixels; 25 is loose enough not to reject a
 * shaky child's hand and tight enough that the panel is definitely usable. */
#define CAL_MAX_RESID 25

static const lv_point_t CAL_PTS[4] = {
    { CAL_INSET,         CAL_INSET },
    { LCD_W - CAL_INSET, CAL_INSET },
    { CAL_INSET,         LCD_H - CAL_INSET },
    { LCD_W - CAL_INSET, LCD_H - CAL_INSET },
};
/* 🚨 CALIBRATION MUST BE ABLE TO GIVE THE PANEL BACK ON ITS OWN.
 *
 * The builder, 2026-08-21: "it just got locked out of the boys comunicater". This
 * screen had NO way out - no back button, no timeout - and the only exit was
 * four successful taps. So a calibration entered by accident, or one where the
 * readings are bad ("Bad reading / Tap to retry" loops for ever), takes the
 * handset with it. Worse, a mis-tapped calibration is SAVED, so one bad session
 * bricks the UI permanently.
 *
 * 🔑 THE CYD ALREADY HAD THIS RULE AND IT WAS NEVER CARRIED ACROSS: "a
 * calibration started by accident leaves the handset stuck on a black screen
 * with no way back that does not involve a reboot" — and a reboot does not help
 * here, because the bad mapping is in NVS.
 *
 * Three ways out now, none of which need the mapping under repair to work:
 *   - a 45s deadline that restores whatever was stored before,
 *   - a 3-second press ANYWHERE, read from the RAW controller,
 *   - and it is never entered without a confirmation in the first place.
 */
#define CAL_TIMEOUT_US   (45 * 1000000LL)
#define CAL_ESCAPE_US    (3  * 1000000LL)

static int cal_step;
static int cal_raw[4][2];
static lv_obj_t *cal_target, *cal_msg;
static int64_t cal_deadline;
static int64_t cal_down_since;

static void cal_abort(const char *why)
{
    applog("[cal] aborted (%s) - the stored calibration is unchanged", why);
    cal_step = 0;
    cal_down_since = 0;
    ui_show_home();
}

static void cal_draw(void)
{
    if (cal_step >= 4) return;
    lv_obj_align(cal_target, LV_ALIGN_TOP_LEFT,
                 CAL_PTS[cal_step].x - 15, CAL_PTS[cal_step].y - 15);
    /* The countdown line is written by cal_tick(), which runs every 50ms. */
}

static void cal_finish(void)
{
    touch_cal_t c = { 0 };

    float lx = (cal_raw[0][0] + cal_raw[2][0]) / 2.0f;
    float rx = (cal_raw[1][0] + cal_raw[3][0]) / 2.0f;
    float ly = (cal_raw[0][1] + cal_raw[2][1]) / 2.0f;
    float ry = (cal_raw[1][1] + cal_raw[3][1]) / 2.0f;
    c.swap = (fabsf(rx - lx) < fabsf(ry - ly)) ? 1 : 0;

    float u_l = c.swap ? ly : lx;
    float u_r = c.swap ? ry : rx;
    float t_t = (c.swap ? (cal_raw[0][0] + cal_raw[1][0])
                        : (cal_raw[0][1] + cal_raw[1][1])) / 2.0f;
    float t_b = (c.swap ? (cal_raw[2][0] + cal_raw[3][0])
                        : (cal_raw[2][1] + cal_raw[3][1])) / 2.0f;

    float dx = u_r - u_l, dy = t_b - t_t;
    if (fabsf(dx) < 5.0f || fabsf(dy) < 5.0f) {
        lv_label_set_text(cal_msg, "Bad reading\nTap to retry");
        cal_step = 0;
        cal_draw();
        return;
    }
    c.ax = (float)(CAL_PTS[1].x - CAL_PTS[0].x) / dx;
    c.bx = CAL_PTS[0].x - c.ax * u_l;
    c.ay = (float)(CAL_PTS[2].y - CAL_PTS[0].y) / dy;
    c.by = CAL_PTS[0].y - c.ay * t_t;

    /* 🚨 FINISHING A CALIBRATION IS NOT THE SAME AS GETTING A GOOD ONE, AND
     * THIS IS THE GUARD THAT ACTUALLY MATTERS.
     *
     * Everything above only protects the ENTRY. Once four taps land, the fit was
     * written to NVS unconditionally - so a session entered by accident, or
     * completed by a child mashing the screen, permanently replaced a good
     * calibration with a wrong one. That is the fault the builder reported: not that the
     * tool opens, but that it DESTROYS what was there. The old degenerate check
     * below (a 5-unit span) rejects only a fit that is mathematically
     * impossible, not one that is merely absurd.
     *
     * hw.h measured this panel at ax=-1.1688, ay=-1.0156, so a real fit has a
     * slope near 1 on both axes. Anything outside 0.45..2.60 is not a panel
     * reading, it is noise - and the earlier note in this file records slopes of
     * -32.7 and 17.3 coming out of "perfectly good presses". Refuse those and
     * KEEP WHAT WAS STORED. A calibration that cannot be finished leaves the
     * handset exactly as it was, which is always the safer outcome. */
    if (fabsf(c.ax) < 0.45f || fabsf(c.ax) > 2.60f ||
        fabsf(c.ay) < 0.45f || fabsf(c.ay) > 2.60f) {
        applog("[cal] REFUSED ax=%.3f ay=%.3f - outside 0.45..2.60, "
               "the stored calibration is unchanged", c.ax, c.ay);
        lv_label_set_text(cal_msg, "Bad reading\nTap to retry");
        cal_step = 0;
        cal_draw();
        return;
    }

    /* 🚨 THE SLOPE CHECK ABOVE NEVER LOOKED AT bx/by, AND THE OFFSET IS HALF
     * THE MAPPING. Kid 2's handset, 2026-09-21: touch stopped working and the builder
     * had to recalibrate it by hand — "the screen calabration was messed up
     * again". A fit can have two perfectly sane slopes and offsets that put
     * every press a hundred pixels from the finger, and the band check waves it
     * straight through into NVS.
     *
     * 🔑 SO CHECK THE FIT AGAINST THE TAPS THAT PRODUCED IT. Map all four raw
     * points through the new mapping and compare each to the crosshair it was
     * supposed to be. That single test catches a bad slope, a bad offset AND
     * four inconsistent taps — which is what a child mashing the screen
     * produces — where the band check catches only the first, and only when it
     * is mathematically absurd rather than merely wrong.
     *
     * The fit comes from AVERAGED pairs, so four honest taps leave residuals of
     * a few pixels; the tolerance is set well above that and far below "the
     * press lands somewhere else entirely". */
    for (int i = 0; i < 4; i++) {
        float u = c.swap ? cal_raw[i][1] : cal_raw[i][0];
        float t = c.swap ? cal_raw[i][0] : cal_raw[i][1];
        float mx = c.ax * u + c.bx, my = c.ay * t + c.by;
        float ex = fabsf(mx - CAL_PTS[i].x), ey = fabsf(my - CAL_PTS[i].y);
        if (ex > CAL_MAX_RESID || ey > CAL_MAX_RESID) {
            applog("[cal] REFUSED point %d off by %.0f,%.0f px (limit %d) - "
                   "the stored calibration is unchanged", i, ex, ey,
                   CAL_MAX_RESID);
            lv_label_set_text(cal_msg, "Taps don't agree\nTap to retry");
            cal_step = 0;
            cal_draw();
            return;
        }
    }
    c.valid = 1;

    settings_set_touch_cal(&c);
    applog("[cal] swap=%d ax=%.4f bx=%.1f ay=%.4f by=%.1f",
           c.swap, c.ax, c.bx, c.ay, c.by);
    ui_show_player();
}

/* Samples the RAW controller, not LVGL — the mapping is the thing under
 * repair, so it cannot be part of the repair. */
static int     cal_hit_x, cal_hit_y;
static int64_t cal_press_len;
/* 🚨 THE FINGER IS ALREADY DOWN WHEN THIS SCREEN OPENS.
 *
 * You reach calibration by HOLDING the battery indicator and then HOLDING Yes,
 * so the screen appears mid-press every single time. Recording that press —
 * on its down edge or its up edge, it makes no difference — banks point 1 at
 * the Yes button's position. The builder, 2026-09-06: "it makes you hold the button for
 * 4 sec and then jumps strait to the wrong spot." Every calibration entered by
 * the hold gesture was poisoned before the first crosshair was ever shown.
 *
 * So nothing counts until the screen has seen the screen go UNTOUCHED once. */
static bool    cal_armed;

static void cal_tick(lv_timer_t *t)
{
    static bool was_down;
    int rx, ry;
    if (lv_scr_act() != scr_cal) {
        was_down = false; cal_down_since = 0; cal_press_len = 0; cal_armed = false;
        return;
    }

    int64_t now = esp_timer_get_time();
    if (cal_deadline && now > cal_deadline) { cal_abort("timed out"); return; }

    bool down = display_touch_raw(&rx, &ry);

    /* 🔑 THE ESCAPE IS READ FROM THE RAW CONTROLLER, not through LVGL. The
     * mapping is the thing under repair, so it cannot be part of the way out —
     * a "Cancel" button would be exactly as unreachable as everything else. */
    if (down) {
        if (!cal_down_since) cal_down_since = now;
        else if (now - cal_down_since > CAL_ESCAPE_US) { cal_abort("held to cancel"); return; }
    } else {
        /* Measure at the moment of release, before the start time is cleared.
         * Computing it while still down gives 0 for a tap that spans a single
         * timer tick, and a 0-length press would be discarded as a non-tap. */
        if (cal_down_since) cal_press_len = now - cal_down_since;
        cal_down_since = 0;
    }

    /* Count down out loud, so it is obvious the handset will rescue itself. */
    if (cal_step < 4) {
        int left = (int)((cal_deadline - now) / 1000000);
        lv_label_set_text_fmt(cal_msg,
                              "Tap the circle\n%d of 4\n\nhold to cancel  -  %ds",
                              cal_step + 1, left < 0 ? 0 : left);
    }
    /* 🚨 RECORD ON RELEASE, NOT ON PRESS — AND ONLY IF IT WAS A TAP.
     *
     * Recording on touch-DOWN meant the screen advertised "hold to cancel" and
     * then banked your finger's position the instant it landed: press anywhere
     * to cancel and that spot became the next calibration point, so point 1 was
     * wherever you started the hold instead of the corner. The two gestures
     * were fighting and the press always won.
     *
     * On release, a short press is a tap and a long one is the escape, which is
     * what the label has been promising all along. The position captured at
     * touch-down is the one used: a finger rolls slightly as it lifts, and the
     * landing point is what the user aimed at. */
    if (down && !was_down) {
        cal_hit_x = rx;
        cal_hit_y = ry;
    }
    if (!down && was_down && cal_armed && cal_step < 4 &&
        cal_press_len && cal_press_len < CAL_ESCAPE_US) {
        cal_raw[cal_step][0] = cal_hit_x;
        cal_raw[cal_step][1] = cal_hit_y;
        applog("[cal] %d: raw %d,%d", cal_step + 1, cal_hit_x, cal_hit_y);
        cal_step++;
        if (cal_step >= 4) cal_finish();
        else               cal_draw();
    }
    /* Armed only AFTER the record check, so the release that ends the entry
     * press cannot arm and be counted on the same tick. */
    if (!down) cal_armed = true;
    was_down = down;
}

static void build_cal_screen(void)
{
    scr_cal = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_cal, C_BG, 0);
    lv_obj_clear_flag(scr_cal, LV_OBJ_FLAG_SCROLLABLE);

    cal_msg = mk_label(scr_cal, &lv_font_montserrat_14, C_INK, "Tap the circle");
    lv_obj_set_style_text_align(cal_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(cal_msg, LCD_W - 20);
    lv_obj_align(cal_msg, LV_ALIGN_CENTER, 0, 0);

    cal_target = lv_obj_create(scr_cal);
    flat(cal_target);
    lv_obj_set_size(cal_target, 30, 30);
    lv_obj_set_style_radius(cal_target, 15, 0);
    lv_obj_set_style_bg_color(cal_target, C_AMBER, 0);
    lv_obj_set_style_bg_opa(cal_target, LV_OPA_COVER, 0);
}

void ui_show_cal(void)
{
    cal_step = 0;
    cal_down_since = 0;
    cal_deadline = esp_timer_get_time() + CAL_TIMEOUT_US;
    applog("[cal] starting - 45s to finish, or hold to cancel");
    ui_load(scr_cal);
    cal_draw();
}

/* --- and it is not entered without being asked for twice -------------------
 *
 * 🚨 THE TWO ENTRY POINTS WERE BOTH HAIR TRIGGERS. A long press on the battery
 * half of the status bar - which exists on EVERY screen - went straight into
 * calibration, and so did the menu row. A child resting a thumb on the top of
 * the panel is a long press.
 */
/* A question raised by accident must go away by itself. Left up, it waits for
 * SOMETHING to tap it - and a handset in a bag taps itself. */
#define CAL_ASK_TIMEOUT_MS  12000

static lv_obj_t  *cal_ask_box;
static lv_timer_t *cal_ask_timer;
static int         cal_yes_ticks;

static void cal_ask_close(void)
{
    if (cal_ask_timer) { lv_timer_del(cal_ask_timer); cal_ask_timer = NULL; }
    if (cal_ask_box)   { lv_obj_del(cal_ask_box);     cal_ask_box   = NULL; }
    cal_yes_ticks = 0;
}
static void cal_ask_timeout(lv_timer_t *t)
{
    (void)t;
    cal_ask_timer = NULL;          /* one-shot: LVGL is deleting it itself */
    applog("[cal] the question timed out - nothing changed");
    cal_ask_close();
}
static void cal_ask_no(lv_event_t *e)  { (void)e; cal_ask_close(); }
/* 🔑 HELD, NOT TAPPED. A single stray press must not be able to answer
 * yes - especially on a handset whose mapping is already wrong, where taps land
 * somewhere other than where the child aimed. */
static void cal_ask_yes(lv_event_t *e)
{
    (void)e;
    if (++cal_yes_ticks < 9) return;           /* ~1.3s of holding */
    cal_ask_close();
    ui_show_cal();
}
static void cal_ask_yes_up(lv_event_t *e) { (void)e; cal_yes_ticks = 0; }

void ui_ask_cal(void)
{
    cal_ask_close();
    cal_yes_ticks = 0;
    /* On the TOP LAYER, so it appears over whatever screen asked. */
    cal_ask_box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(cal_ask_box, LCD_W - 40, 150);
    lv_obj_center(cal_ask_box);
    lv_obj_set_style_bg_color(cal_ask_box, C_PANEL, 0);
    lv_obj_set_style_border_color(cal_ask_box, C_AMBER, 0);
    lv_obj_set_style_border_width(cal_ask_box, 2, 0);
    lv_obj_clear_flag(cal_ask_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = mk_label(cal_ask_box, &lv_font_montserrat_14, C_INK,
                           "Redo the touch\ncalibration?");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *no = lv_btn_create(cal_ask_box);
    lv_obj_set_size(no, 76, 40);
    lv_obj_align(no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(no, C_LINE, 0);
    lv_obj_add_event_cb(no, cal_ask_no, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(no, &lv_font_montserrat_14, C_INK, "No"));

    lv_obj_t *yes = lv_btn_create(cal_ask_box);
    lv_obj_set_size(yes, 76, 40);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, C_AMBER, 0);
    lv_obj_add_event_cb(yes, cal_ask_yes, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_add_event_cb(yes, cal_ask_yes_up, LV_EVENT_RELEASED, NULL);
    lv_obj_center(mk_label(yes, &lv_font_montserrat_14, C_ONAMBER, "Hold"));

    cal_ask_timer = lv_timer_create(cal_ask_timeout, CAL_ASK_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(cal_ask_timer, 1);
}

/* ------------------------------------------------------------------ *
 * walkie-talkie screen
 *
 * A shared channel, not a call. Press to talk, everyone hears — the model a
 * child already has from the toy. The screen therefore shows only two things
 * that matter: am I heard, and is anyone there.
 * ------------------------------------------------------------------ */

static lv_obj_t *wt_title, *wt_batt, *wt_btn, *wt_btn_lbl, *wt_stat, *wt_who;

/* Defined with the messaging code further down; the walkie needs the same
 * id -> index resolution and for the same reason. */
static int peer_idx_for_id(uint16_t id);

static lv_obj_t *wt_to_btn, *wt_to_lbl;

/* The walkie's To: row. Deliberately the same control, wording and cycle as the
 * message screen's — the kids asked for one-to-one on both, and two different
 * gestures for one idea is how you get a child talking confidently into a
 * handset nobody is listening to. */
/* Cycle a "who am I addressing" target: Everyone -> each PLAYABLE handset ->
 * Everyone. Shared by the Talkie and T9 so the gesture is identical.
 *
 * 🚨 GATEWAYS ARE SKIPPED. They beacon like a handset, so from 2026-09-07 they
 * appeared in these cycles — offering to send a text or talk to a board with
 * nobody behind it. Same fault that stopped Uno; see espnow.h.
 *
 * A target that has aged out leaves `pos` at -1 and so restarts the cycle,
 * which is the existing behaviour and deliberately not a silent re-address. */
static uint16_t play_cycle_id(uint16_t cur_id)
{
    int n = espnow_play_count();
    if (n <= 0) return 0;

    int pos = -1;                       /* -1 = currently "Everyone" */
    if (cur_id)
        for (int k = 0; k < n; k++) {
            int i = espnow_play_idx(k);
            if (i >= 0 && espnow_peer_id(i) == cur_id) { pos = k; break; }
        }

    int nxt = pos + 1;
    if (nxt >= n) return 0;             /* wrap back to Everyone */
    int i = espnow_play_idx(nxt);
    return i >= 0 ? espnow_peer_id(i) : 0;
}

static void wt_to_draw(void)
{
    if (!wt_to_lbl) return;

    uint16_t t = wt_target();
    if (t && peer_idx_for_id(t) < 0) { t = 0; wt_set_target(0); }

    if (t) {
        lv_label_set_text_fmt(wt_to_lbl, LV_SYMBOL_RIGHT " Talk to: %s (only)",
                              espnow_peer_name(peer_idx_for_id(t)));
        lv_obj_set_style_text_color(wt_to_lbl, C_GREEN, 0);
    } else if (espnow_play_count() > 0) {
        lv_label_set_text(wt_to_lbl, LV_SYMBOL_RIGHT " Talk to: Everyone");
        lv_obj_set_style_text_color(wt_to_lbl, C_AMBER, 0);
    } else {
        lv_label_set_text(wt_to_lbl, LV_SYMBOL_RIGHT " Talk to: nobody in range");
        lv_obj_set_style_text_color(wt_to_lbl, C_MUTED, 0);
    }
}

static void wt_to_clicked(lv_event_t *e)
{
    (void)e;
    wt_set_target(play_cycle_id(wt_target()));
    wt_to_draw();
}

static void wt_pressed(lv_event_t *e)  { wt_ptt(true); }
static void wt_released(lv_event_t *e) { wt_ptt(false); }

static void wt_back_clicked(lv_event_t *e)
{
    wt_close();                      /* radio down on the way out: it is the
                                      * single biggest battery cost here */
    ui_show_home();
}

static void build_wt_screen(void)
{
    scr_wt = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_wt, C_BG, 0);
    lv_obj_clear_flag(scr_wt, LV_OBJ_FLAG_SCROLLABLE);

    mk_statusbar(scr_wt, &wt_title, &wt_batt, "Talkie");

    wt_to_btn = lv_btn_create(scr_wt);
    flat(wt_to_btn);
    lv_obj_set_size(wt_to_btn, LCD_W - 8, 26);
    lv_obj_align(wt_to_btn, LV_ALIGN_TOP_LEFT, 4, BAR_H + 2);
    lv_obj_set_style_bg_color(wt_to_btn, C_LINE, 0);
    lv_obj_set_style_bg_opa(wt_to_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wt_to_btn, 4, 0);
    lv_obj_add_event_cb(wt_to_btn, wt_to_clicked, LV_EVENT_CLICKED, NULL);
    wt_to_lbl = mk_label(wt_to_btn, &lv_font_montserrat_12, C_AMBER, "");
    lv_obj_center(wt_to_lbl);

    wt_stat = mk_label(scr_wt, &lv_font_montserrat_12, C_MUTED, "off");
    lv_obj_set_width(wt_stat, LCD_W - 28);
    lv_obj_set_style_text_align(wt_stat, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(wt_stat, LV_ALIGN_TOP_MID, 0, BAR_H + 34);

    wt_who = mk_label(scr_wt, &lv_font_montserrat_16, C_BLUE, "");
    /* ⚠️ DOT, NOT WRAP. This label sits at BAR_H+32 and the 150px talk button
     * starts at BAR_H+62 — thirty pixels of room, which is ONE line. It used to
     * hold only "X is talking" and never needed two; it now also carries the
     * nearby list, which can be three names long. Wrapping would push the
     * second line straight through the button. */
    lv_label_set_long_mode(wt_who, LV_LABEL_LONG_DOT);
    lv_obj_set_width(wt_who, LCD_W - 28);
    lv_obj_set_style_text_align(wt_who, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(wt_who, LV_ALIGN_TOP_MID, 0, BAR_H + 56);

    wt_btn = lv_btn_create(scr_wt);
    lv_obj_set_size(wt_btn, 132, 132);
    lv_obj_set_style_radius(wt_btn, 66, 0);
    lv_obj_set_style_bg_color(wt_btn, C_GREEN, 0);
    lv_obj_set_style_bg_color(wt_btn, C_RED, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(wt_btn, 0, 0);
    lv_obj_set_style_shadow_width(wt_btn, 0, 0);
    lv_obj_align(wt_btn, LV_ALIGN_TOP_MID, 0, BAR_H + 86);
    lv_obj_add_event_cb(wt_btn, wt_pressed,  LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(wt_btn, wt_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(wt_btn, wt_released, LV_EVENT_PRESS_LOST, NULL);

    wt_btn_lbl = mk_label(wt_btn, &lv_font_montserrat_16, C_ONAMBER, "HOLD\nTO TALK");
    lv_obj_set_style_text_align(wt_btn_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(wt_btn_lbl);

    /* Above the footer, below the big button: the walkie-talkie could not be
     * turned down AT ALL while somebody was talking through it, which is
     * exactly when you want to. */
    mk_volume_row(scr_wt, 12, -(FOOT_H + 14), 122);
    mk_sink_btn(scr_wt, -8, -(FOOT_H + 10));

    lv_obj_t *foot = lv_obj_create(scr_wt);
    flat(foot);
    lv_obj_set_size(foot, LCD_W, FOOT_H);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot, C_PANEL, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, C_LINE, 0);
    lv_obj_set_style_border_width(foot, 1, 0);

    lv_obj_t *b = lv_btn_create(foot);
    flat(b);
    lv_obj_set_size(b, 80, 26);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(b, wt_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(b, &lv_font_montserrat_12, C_BLUE, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    /* 🚫 THE "CALL" BUTTON IS GONE. It rang the BLE doorbell - the way to get
     * the other child onto this screen without WiFi. The builder, 2026-08-21: "i dont
     * think we need the call button for the blue tooth on the talkie screen any
     * more as we are not going donw that road right". Correct: the doorbell was
     * already deferred in favour of UDP push, and the direction now is ESP-NOW.
     *
     * ⚠️ Only the BUTTON is removed. ble.c, the BLE menu toggle and the advert
     * are untouched, so nothing else changes behaviour - and if a doorbell is
     * ever wanted again it is a button, not a subsystem, that has to come back.
     */
}

/* Who this handset can hear over ESP-NOW, as a list a child can read.
 *
 * 🔑 THIS IS NOT wt_peer_count(). Two different questions, and confusing them
 * is what made the off-grid test so hard to read:
 *   - wt_peer_count() = who is on the UDP channel, i.e. who will actually HEAR
 *     me if I press Talk. Needs the house AP. Already in wt_status().
 *   - espnow_peer_count() = who is within RADIO range of this handset, AP or
 *     no AP. Always true, always available, and until now shown nowhere.
 *
 * Capped at three names: the label is one line of 12pt in 212px, and a fourth
 * name would ellipsise the list rather than lengthen it. */
static void wt_nearby(char *o, size_t n)
{
    int cnt = espnow_play_count();
    if (cnt <= 0) { snprintf(o, n, "Nobody nearby"); return; }

    int shown = cnt > 3 ? 3 : cnt;
    int len = snprintf(o, n, "Nearby: ");
    /* ⚠️ Walk PLAYABLE positions, not raw indices — the count above is filtered
     * and a raw walk would name a gateway while claiming it was a handset. */
    for (int i = 0; i < shown && len < (int)n; i++)
        len += snprintf(o + len, n - len, "%s%s",
                        i ? ", " : "", espnow_peer_name(espnow_play_idx(i)));
    if (cnt > shown && len < (int)n)
        snprintf(o + len, n - len, " +%d", cnt - shown);
}

static void wt_refresh(void)
{
    const int nearby = espnow_play_count();

    /* 🚨 "no wifi" IS TRUE AND USELESS, AND IT COST THE BUILDER A FIELD TEST.
     *
     * 2026-08-26: three handsets, five feet apart, away from the house AP. All
     * three could see each other perfectly over ESP-NOW. The Talkie is UDP over
     * the house wifi, so it said "no wifi" — and that is the whole story the
     * product told him. He reasonably concluded ESP-NOW was broken; it was not,
     * it was simply never wired to this screen.
     *
     * 🔑 SAY WHAT WORKS, NOT ONLY WHAT DOES NOT. Text and games are ESP-NOW and
     * DO work with no AP anywhere. Pointing at them is the difference between a
     * dead end and a signpost. Only shown when somebody is actually nearby —
     * with nobody in range, Text would not help either and the plain status is
     * the honest answer. */
    if (wt_mode() == WT_OFF && nearby > 0)
        lv_label_set_text(wt_stat, "voice needs wifi - use Text");
    else
        lv_label_set_text(wt_stat, wt_status());

    wt_to_draw();

    const char *who = wt_hearing();
    if (wt_talking()) {
        lv_obj_set_style_text_font(wt_who, &lv_font_montserrat_16, 0);
        lv_label_set_text(wt_who, "ON AIR");
        lv_obj_set_style_text_color(wt_who, C_RED, 0);
    } else if (who[0]) {
        lv_obj_set_style_text_font(wt_who, &lv_font_montserrat_16, 0);
        lv_label_set_text_fmt(wt_who, "%s is talking", who);
        lv_obj_set_style_text_color(wt_who, C_GREEN, 0);
    } else {
        /* 12pt for the list: three names at 16pt do not fit 212px, and this is
         * standing information rather than the thing to look at. */
        char nb[48];
        wt_nearby(nb, sizeof(nb));
        lv_obj_set_style_text_font(wt_who, &lv_font_montserrat_12, 0);
        lv_label_set_text(wt_who, nb);
        lv_obj_set_style_text_color(wt_who, nearby > 0 ? C_BLUE : C_MUTED, 0);
    }
    /* The channel being open is activity — do not blank the screen mid-chat. */
    if (wt_mode() != WT_OFF) display_note_activity();
}

/* ------------------------------------------------------------------ *
 * terminal panel
 *
 * A scrolling view of applog(). It exists for the voice chain: every failure
 * there (no WiFi, wrong STT path, empty transcript, bad TTS engine) looks the
 * same from outside — press talk, nothing happens — and this device is used
 * nowhere near a serial cable.
 * ------------------------------------------------------------------ */

static void term_back_clicked(lv_event_t *e) { ui_show_home(); }

static void build_term_screen(void)
{
    scr_term = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_term, C_BG, 0);
    lv_obj_clear_flag(scr_term, LV_OBJ_FLAG_SCROLLABLE);

    mk_statusbar(scr_term, &term_title, &term_batt, "Log");

    lv_obj_t *box = lv_obj_create(scr_term);
    flat(box);
    lv_obj_set_size(box, LCD_W, UI_LIST_H);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, BAR_H);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(box, C_AMBER, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(box, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(box, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_all(box, 6, 0);

    /* ONE label, not one per line. 64 labels would cost 64 objects and a
     * rebuild on every write; a single label with newlines repaints as one
     * item and scrolls the same way. */
    term_body = mk_label(box, &lv_font_montserrat_10, C_INK, "");
    lv_label_set_long_mode(term_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(term_body, LCD_W - 18);

    lv_obj_t *foot = lv_obj_create(scr_term);
    flat(foot);
    lv_obj_set_size(foot, LCD_W, FOOT_H);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot, C_PANEL, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, C_LINE, 0);
    lv_obj_set_style_border_width(foot, 1, 0);

    lv_obj_t *b = lv_btn_create(foot);
    flat(b);
    lv_obj_set_size(b, 80, 26);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(b, term_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(b, &lv_font_montserrat_12, C_BLUE,
                            LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
}

/* Rebuilt only when applog's sequence number moves — otherwise this would
 * reassemble a 64-line string twice a second for nothing. */
static void term_refresh(bool force)
{
    static unsigned seen = (unsigned)-1;
    unsigned now = applog_seq();
    if (!force && now == seen) return;
    seen = now;

    static char buf[(APPLOG_COLS + 1) * APPLOG_LINES + 1];
    size_t used = 0;
    buf[0] = 0;
    for (int i = 0; i < applog_count(); i++) {
        const char *l = applog_line(i);
        if (!l) break;
        int n = snprintf(buf + used, sizeof(buf) - used, "%s\n", l);
        if (n < 0 || (size_t)n >= sizeof(buf) - used) break;
        used += n;
    }
    lv_label_set_text(term_body, buf);
    lv_obj_scroll_to_y(lv_obj_get_parent(term_body), LV_COORD_MAX, LV_ANIM_OFF);
}

/* ------------------------------------------------------------------ */

/* Defined with the low-battery warning, far below. */
static bool lowbat_flashing(void);

static void tick(lv_timer_t *t)
{
    const player_state_t *st = player_state();

    /* Screen blanking. Music keeps playing with the panel dark — that is the
     * point — but the screen is held awake while the user is on the log screen,
     * because reading it is the one thing you do without touching anything. */
    /* ⚠️ The GPS screen is exempt for exactly the reason the log screen is:
     * watching it IS the activity, and it is done without touching anything.
     * A GPS session runs for many minutes untouched, and on battery the panel
     * would otherwise blank part way through an acquisition. */
    int to = settings_screen_timeout();
    if (to > 0 && display_screen_on() && lv_scr_act() != scr_term &&
        lv_scr_act() != scr_gps && lv_scr_act() != scr_map &&
        /* ⚠️ And never while the card is on the PC: the only way out of card
         * mode is the Reboot button on that panel, and a blanked screen makes
         * it unfindable. A dark screen is how "I had to open the case" starts. */
        !usbmsc_active() &&
        !lowbat_flashing() && display_idle_ms() > to * 1000) {
        display_screen_off();
    }

    if (lv_scr_act() == scr_radio) radio_refresh();
    if (lv_scr_act() == scr_gps)   gps_refresh();
    if (lv_scr_act() == scr_map)   map_refresh();
    /* 🚨 THE ROSTER MOVES UNDER YOU. Peers go stale after 8s unheard and a
     * handset doing its telemetry association drops and re-adds them in cycles,
     * so a screen drawn once on entry shows a snapshot that is wrong seconds
     * later — a handset you want to pair with looks absent when it is simply
     * mid-cycle. The Bluetooth screen had this same tick and it went out with
     * the Bluetooth code. */
    if (lv_scr_act() == scr_fleet) fleet_refresh();
    /* A board from another handset, wherever the player happens to be. */
    games_poll_net();

    /* Deep sleep and the clock. power_tick() decides only; it will not sleep on
     * USB, inside the boot grace period, or while anything is playing. */
    power_tick();
    if (net_is_up()) power_sync_time();

    settings_tick();

    /* Nothing below this line is visible with the backlight off, and repainting
     * it is real SPI traffic and CPU on a battery device. */
    if (!display_screen_on()) return;

    /* One call, every screen — see the note above clock_refresh() about why
     * this is NOT a per-screen line like the battery ones below. */
    clock_refresh(false);
    gw_time_ask();

    if (lv_scr_act() == scr_term) { term_refresh(false); batt_refresh(term_batt); }
    if (lv_scr_act() == scr_talk) { talk_refresh();       batt_refresh(talk_batt); }
    if (lv_scr_act() == scr_wt)   { wt_refresh();         batt_refresh(wt_batt);   }

    batt_refresh(list_batt);
    batt_refresh(play_batt);
    /* ⚠️ EVERY SCREEN WITH A STATUS BAR NEEDS A LINE HERE. Adding a screen and
     * forgetting this leaves its battery and wifi indicators permanently blank,
     * which is what happened to home/menu/radio when the 2x2 hub was added
     * (The builder: "the battery and wifi header is only in the music menu"). */
    batt_refresh(home_batt);
    batt_refresh(menu_batt);
    batt_refresh(radio_batt);

    lv_obj_t *pl = lv_obj_get_child(pl_playbtn, 0);
    if (pl) lv_label_set_text(pl, st->playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    lv_obj_t *sk = lv_obj_get_child(pl_sink, 0);
    if (sk) lv_label_set_text(sk, st->sink == SINK_SPEAKER ? "SPEAKER" : "JACK");

    /* 🔑 ONCE PER ALBUM, NOT PER TRACK OR PER TICK. This runs at 2Hz and the
     * cover does not change between tracks of the same album; decoding it again
     * would be tens of milliseconds of SD reads competing with the decoder for
     * the card, twice a second, forever. */
    /* Same discipline as the cover below: the heart only changes when the song
     * does, and faves_contains() builds a path and walks the list. At 2Hz,
     * forever, that is worth not doing. */
    if (st->album != pl_fav_album || st->track != pl_fav_track) {
        pl_fav_album = st->album; pl_fav_track = st->track;
        fav_paint();
    }
    /* A no-op unless a heart was tapped mid-song. faves.c defers the write to
     * keep it off the card while the decoder is reading from it; this is the
     * moment that is safe. */
    if (!st->playing) faves_flush();

    if (st->album != pl_art_album) {
        pl_art_album = st->album;
        bool have = false;
        if (st->album >= 0) {
            char path[300];
            library_track_path(st->album, st->track, path, sizeof(path));
            have = art_load(path);
        } else {
            art_clear();
        }
        if (have && art_img()) {
            lv_img_set_src(pl_art, art_img());
            /* Cover the screen. The decoder already picked the smallest scale
             * that is still at least screen-width, so most covers need no zoom
             * at all; a small one gets stretched rather than floating in the
             * middle of a black screen. */
            lv_coord_t w = art_img()->header.w;
            lv_img_set_zoom(pl_art, w >= LCD_W ? 256 : (256 * LCD_W) / w);
            lv_obj_set_style_img_opa(pl_art, settings_art_opa(), 0);
            lv_obj_center(pl_art);
            lv_obj_clear_flag(pl_art, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* No art is normal — plenty of albums have none, and one of them
             * (Weezer) went on the card today. Hide it and leave the plain dark
             * background, which is what every screen looked like yesterday. */
            lv_obj_add_flag(pl_art, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (st->album >= 0) {
        const lib_album_t *al = library_album_by_index(st->album);
        if (al) {
            const lib_artist_t *ar = library_artist(al->artist);
            lv_label_set_text(pl_artist, ar ? ar->name : "");
            lv_label_set_text(pl_album, al->name);
        }
        char pt[160];
        lv_label_set_text(pl_track, pretty(st->title, pt, sizeof(pt)));
        lv_bar_set_value(pl_bar, st->progress, LV_ANIM_OFF);
        lv_label_set_text_fmt(pl_elapsed, "%d:%02d",
                              st->elapsed_ms / 60000, (st->elapsed_ms / 1000) % 60);

        int n = library_track_count(st->album);
        lv_label_set_text_fmt(pl_remain, "track %d of %d", st->track + 1, n);

        /* 🔑 "Up next" IS A LIE WHILE SHUFFLING — the next track is chosen when
         * this one ends, so naming track+1 would be wrong far more often than
         * right. That frees this full-width line to carry the shuffle scope
         * instead, which is the word the 30px button has no room for.
         * "Last track" is equally wrong under CARD/ARTIST shuffle, which rolls
         * on past the end of an album rather than stopping. */
        player_shuffle_t sm = player_shuffle();
        if (sm != SHUFFLE_OFF) {
            lv_label_set_text_fmt(pl_next, "Shuffle: %s", player_shuffle_name(sm));
        } else {
            const lib_track_t *nx = library_track(st->album, st->track + 1);
            if (nx) {
                char np[160];
                lv_label_set_text_fmt(pl_next, "Up next  %s",
                                      pretty(nx->name, np, sizeof(np)));
            }
            else    lv_label_set_text(pl_next, "Last track");
        }
    }
}

/* Tap the heading to swap roots. It is the one control on this screen with
 * nothing else to do, it is nowhere near the rows or the strip, and the heading
 * already names which list you are in — so the affordance and the label are the
 * same object. */
static void list_title_clicked(lv_event_t *e)
{
    (void)e;
    if (s_level == LEVEL_ARTIST)          s_level = LEVEL_ALL_ALBUMS;
    else if (s_level == LEVEL_ALL_ALBUMS) s_level = LEVEL_ARTIST;
    else return;                       /* only the two roots swap */
    if (!s_alb_letter) s_alb_letter = library_album_first_letter();
    int cnt = 0;
    if (!idx_range(idx_sel(), NULL, &cnt) || cnt == 0)
        idx_set(idx_on_albums() ? library_album_first_letter()
                                : library_first_letter());
    idx_paint();
    show_list();
}

void ui_show_list(void)
{
    /* The letter is chosen the first time only. library_first_letter() rather
     * than a hardcoded 'A': a card whose first artist is "2Pac" opens on '#',
     * and one with nothing under A does not open on an empty list. */
    if (!s_letter) s_letter = library_first_letter();
    int cnt = 0;
    if (!library_letter_range(s_letter, NULL, &cnt) || cnt == 0)
        s_letter = library_first_letter();
    ui_load(scr_list);
    idx_paint();
    show_list();
}
void ui_show_player(void) { ui_load(scr_play); }
void ui_show_term(void)   { ui_load(scr_term); term_refresh(true); }
void ui_show_talk(void)   { ui_load(scr_talk); talk_refresh(); }
void ui_show_wt(void)     { ui_load(scr_wt); wt_open(); wt_refresh(); vol_rows_sync(); sink_btns_sync(); }
void ui_show_home(void)   { ui_load(scr_home); }
/* Leaving and re-entering the menu DISARMS Restart. Without this the row keeps
 * saying "tap again" long after the confirmation window closed, which is worse
 * than no prompt: it advertises a one-tap restart that is not actually armed. */
static void menu_restart_disarm(void);
/* ⚠️ REPAINT ON ENTRY. The rows are built once in ui_init(), so every label was
 * a snapshot of boot-time values - a gain changed by the console, or a name set
 * over the walkie, showed the old text until the next reboot. */
static void menu_refresh_all(void);
void ui_show_menu(void)
{
    menu_restart_disarm();
    menu_refresh_all();
    ui_load(scr_menu);
}

/* ------------------------------------------------------------------ *
 * home screen — the 2x2 grid
 *
 * 🔑 THE NAMES ARE THE BUILDER'S AND THEY RESOLVE A REAL COLLISION (2026-08-16).
 * The old play screen had a button called "Radio" that opened the WALKIE-TALKIE,
 * which left nothing to call actual internet radio. Now:
 *     MUSIC  = the player / library      RADIO  = internet radio
 *     COMMS  = the AI assistant          TALKIE = the walkie-talkie
 * "Comms" for the assistant and "Talkie" for the two-way radio are the whole
 * point of the rename - do not quietly revert either.
 *
 * Equal tiles, deliberately: they are the biggest touch targets that fit, which
 * matters both for a seven-year-old and for a panel whose calibration has drifted
 * before. Anything that needs a smaller target belongs behind the hamburger.
 * ------------------------------------------------------------------ */

#define TILE_W  ((LCD_W - 6) / 2)              /* 117 */
#define TILE_H  ((LCD_H - BAR_H - 6) / 2)      /* 145 */

static void home_text_clicked(lv_event_t *e)   { ui_show_t9(); }
static void home_music_clicked(lv_event_t *e)  { ui_show_player(); }
static void home_comms_clicked(lv_event_t *e)  { ui_show_talk(); }
static void home_talkie_clicked(lv_event_t *e) { ui_show_wt(); }
static void home_menu_clicked(lv_event_t *e)   { ui_show_menu(); }

static lv_obj_t *mk_tile(lv_obj_t *parent, lv_align_t align, int dx, int dy,
                         const char *icon, const char *text, lv_color_t colour,
                         lv_event_cb_t cb)
{
    lv_obj_t *b = lv_btn_create(parent);
    flat(b);
    lv_obj_set_size(b, TILE_W, TILE_H);
    lv_obj_align(b, align, dx, dy);
    lv_obj_set_style_bg_color(b, C_PANEL, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 6, 0);
    /* A pressed state that is actually visible on a dim panel — without it there
     * is no feedback at all that a tap registered, and the natural response to
     * that is to tap again. */
    lv_obj_set_style_bg_color(b, C_LINE, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ic = mk_label(b, &lv_font_montserrat_22, colour, icon);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -18);
    lv_obj_t *tx = mk_label(b, &lv_font_montserrat_14, C_INK, text);
    lv_obj_align(tx, LV_ALIGN_CENTER, 0, 20);
    return b;
}

static void build_home_screen(void)
{
    scr_home = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_home, C_BG, 0);
    lv_obj_clear_flag(scr_home, LV_OBJ_FLAG_SCROLLABLE);
    /* The device name lives here: three identical handsets in one house are
     * otherwise indistinguishable on screen, and a child cannot read a MAC. */
    mk_statusbar(scr_home, &home_title, &home_batt, settings_name());

    /* ☰ sits on top of the status bar's own left-half hit area. Created after
     * the bar, so it is above it and takes the click; the bar's LONG press
     * (the hidden log screen) still works everywhere else. Kids know the
     * hamburger - the builder's reasoning, and it keeps power/config off the home grid
     * instead of stealing a tile from something they actually use. */
    lv_obj_t *burger = lv_btn_create(scr_home);
    flat(burger);
    lv_obj_set_size(burger, 34, BAR_H);
    lv_obj_align(burger, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(burger, home_menu_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(burger, &lv_font_montserrat_14, C_AMBER, LV_SYMBOL_LIST);
    lv_obj_center(bl);

    /* The ☰ (34px wide, at x=0) is drawn over the status bar's name, which
     * mk_statusbar aligns at x=8 — so the first few letters of a name sat under
     * the hamburger (a clipped name instead of the full one on the home screen, and only
     * there, since this is the only screen with a burger). Shift the name clear
     * of the button. The other four callers keep x=8; they have no burger. */
    if (home_title) lv_obj_align(home_title, LV_ALIGN_LEFT_MID, 38, 0);

    mk_tile(scr_home, LV_ALIGN_TOP_LEFT,   2, BAR_H + 2, LV_SYMBOL_AUDIO,
            "MUSIC",  C_BLUE,  home_music_clicked);
    mk_tile(scr_home, LV_ALIGN_TOP_RIGHT, -2, BAR_H + 2, LV_SYMBOL_CALL,
            "COMMS",  C_AMBER, home_comms_clicked);
    /* 🚨 ON THE LIGHT BUILD, RADIO IS A DEAD QUARTER OF THE HOME SCREEN.
     * light_stubs.c answers radio_count() with 0 and radio_play() with false —
     * honestly, but the TILE still sits there promising something, and four of
     * the five handsets run light. A child taps it and gets an empty list.
     * Games was reachable only from the player's bottom row, so the swap costs
     * nothing and fills the hole with the thing they actually use.
     * 🔑 the builder's call, and it is the UI half of a lesson already in the notes: a
     * stub that fails honestly still leaves a control that lies. */
    mk_tile(scr_home, LV_ALIGN_TOP_LEFT,   2, BAR_H + TILE_H + 4, LV_SYMBOL_VIDEO,
            "GAMES",  C_GREEN, games_clicked);
    mk_tile(scr_home, LV_ALIGN_TOP_RIGHT, -2, BAR_H + TILE_H + 4, LV_SYMBOL_BELL,
            "TALKIE", C_INK,   home_talkie_clicked);

    /* ✉️ TEXT, as a circle on the seam where the four tiles meet.
     *
     * The builder, 2026-08-21: "could we have it be a cercal in the midel of the for
     * tiles that sys text". It was in the hamburger before, which is the wrong
     * place for the one thing on this device a child will use most - the menu is
     * where power and config live.
     *
     * 🔑 CREATED LAST, SO IT DRAWS ON TOP. LVGL hit-tests the topmost clickable
     * child first, so the circle takes its own taps and the four tiles keep
     * theirs everywhere the circle is not. The ring in the background colour is
     * what makes it read as sitting ON the grid rather than as a fifth tile
     * squeezed between them.
     *
     * ⚠️ A circle is a SQUARE with a radius of half its side. Set one without
     * the other and it silently becomes a rounded rectangle. */
    lv_obj_t *tx = lv_btn_create(scr_home);
    flat(tx);
    lv_obj_set_size(tx, 86, 86);
    lv_obj_set_style_radius(tx, 43, 0);
    lv_obj_align(tx, LV_ALIGN_TOP_MID, 0, BAR_H + TILE_H + 3 - 43);
    lv_obj_set_style_bg_color(tx, C_AMBER, 0);
    lv_obj_set_style_bg_opa(tx, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tx, C_INK, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(tx, C_BG, 0);
    lv_obj_set_style_border_width(tx, 4, 0);
    lv_obj_add_event_cb(tx, home_text_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *txi = mk_label(tx, &lv_font_montserrat_16, C_ONAMBER,
                             LV_SYMBOL_ENVELOPE);
    lv_obj_align(txi, LV_ALIGN_CENTER, 0, -13);
    lv_obj_t *txl = mk_label(tx, &lv_font_montserrat_14, C_ONAMBER, "TEXT");
    lv_obj_align(txl, LV_ALIGN_CENTER, 0, 12);
}

/* ------------------------------------------------------------------ *
 * hamburger menu — power, config, and the repair tools
 * ------------------------------------------------------------------ */

static void menu_back_clicked(lv_event_t *e)  { ui_show_home(); }
/* "Sleep now" blanks the screen. Deep sleep on demand is deliberately NOT here:
 * on USB it would make the board unflashable, and off USB the idle timer already
 * handles it. Long-press for the real thing. */
static void menu_sleep_clicked(lv_event_t *e) { display_screen_off(); ui_show_home(); }
static void menu_sleep_long(lv_event_t *e)    { power_sleep_now(); }
static void menu_log_clicked(lv_event_t *e)   { ui_show_term(); }
/* 🚨 A TAP IS NOT ENOUGH FOR THIS ROW. The battery-hold route to calibration
 * costs a deliberate 3.8 seconds precisely because a grip must not reach it —
 * but this row sat one tap away the whole time, and then offers a button
 * labelled "Hold", which is an instruction a child will follow. Kid 2's
 * handset came back with its calibration wrong and nobody had gone looking for
 * the tool. Same treatment as the Sleep row: the tap says what it is, the hold
 * does it. */
static void menu_cal_clicked(lv_event_t *e)   { (void)e; /* hold, don't tap */ }
static void menu_cal_long(lv_event_t *e)      { (void)e; ui_ask_cal(); }

/* 🔊 SPEAKER GAIN TUNING — a measurement tool, not a normal setting.
 *
 * Steps the ES8311 DAC volume register and plays a test tone at each step, so
 * the loudest CLEAN setting can be found by ear. The builder's note (audio.c): as an
 * ESPHome media player this same board is "way louder", so the gap is
 * configuration - this is how we find out where it went.
 *
 * The label shows the RAW REGISTER and dB relative to 0dB (0xBF). If it reads
 * 0xBF at start, esp_codec_dev's 100% really is 0dB and there IS headroom above;
 * if it reads near 0xFF the theory is wrong and the limit is the amp or speaker.
 *
 * Steps of 6 = 3dB, about the smallest change that is obvious by ear. */
static void beep_task(void *arg);          /* defined with the battery warning */

#define ES8311_0DB_UI 0xBF

/* The inline +/- buttons that used to sit ON the gain row are gone: they cost
 * that row 76px of its label and existed on no other row, so the same tap meant
 * "adjust" here and "advance" everywhere else. One shared pair now lives in the
 * menu footer - see THE SETTINGS MENU below. */



/* ------------------------------------------------------------------
 * THE SETTINGS MENU — one selected row, ONE shared pair of +/- buttons.
 *
 * The builder, 2026-08-21: the +/- belong in a fixed footer beside Home, "so if on one
 * of the menus that had you select like speaker gain or eq on you change it with
 * the + or - ... that why it wont block the line".
 *
 * 🔑 THE OLD SHAPE PUT THE BUTTONS *ON* THE ROW, AND IT COST THE ROW ITS TEXT.
 * Two 38px buttons on a 224px row leave 140px for a label like
 * "Speaker gain  0x1A  +6dB". Worse, only the gain row had buttons: every other
 * value row cycled on tap, so the same gesture meant "adjust this" on one row
 * and "advance that" on the next.
 *
 * Now: tapping a VALUE row SELECTS it, the footer's - and + change whatever is
 * selected, and ACTION rows still just act on tap. The label owns the full width.
 *
 * ⚠️ AND THE HOME BUTTON USED TO SCROLL AWAY, because it was a child of the
 * scrolling screen — the builder: "you have the home button as a floting menu it should
 * be a bottom header". The rows now live in their own scrolling container and
 * the footer is a sibling of it, so Home and +/- are always on screen.
 * ------------------------------------------------------------------ */

#define MENU_FOOT_H 44
#define MENU_ROW_H  40
#define MENU_ROW_DY 46

/* Palette as plain hex, because lv_color_hex() is an inline function and cannot
 * initialise a static table. */
#define H_INK    0xE8EDF5
#define H_AMBER  0xF0A830
#define H_MUTED  0x7C8798

typedef void (*mi_label_fn)(char *out, size_t n);
typedef void (*mi_adjust_fn)(int delta);

typedef struct {
    mi_label_fn   label;      /* how the row renders itself, always           */
    mi_adjust_fn  adjust;     /* NULL => an action row, not a value row       */
    lv_event_cb_t action;     /* value rows use this for nothing              */
    lv_event_cb_t long_press; /* optional, either kind                        */
    uint32_t      colour;
    lv_obj_t     *row, *lbl;
} menu_item_t;

static lv_obj_t *menu_list, *menu_minus, *menu_plus;
static int       menu_sel = -1;

/* ---- the value rows: one label fn and one adjust fn each ---------------- */

/* A helper for every "cycle through a short list of steps" setting, which is
 * most of them. delta of -1 goes backwards, which the old cycle-on-tap could
 * not do at all - the builder had to go all the way round. */
static int step_index(const int *steps, int n, int cur, int delta)
{
    int i = 0;
    for (; i < n; i++) if (steps[i] == cur) break;
    if (i >= n) i = 0;
    i = (i + delta) % n;
    if (i < 0) i += n;
    return i;
}

/* 🔑 PEOPLE, NOT BOARDS. These used to be "Boy"/"Girl"/"Guest"/"Kid 4" — the
 * names of the HANDSETS. But the handsets move between the kids constantly, so
 * "Girl is talking" tells a child which piece of plastic is speaking and not
 * WHO. The builder, 2026-08-26: "the kids can put there names on the one they are using
 * so they now who has what handset".
 *
 * ⚠️ NINE CHARACTERS MAX. msg_frame_t.name is char[10] on the wire and is
 * byte-compatible with the CYD's msg_core.cpp — a longer name arrives
 * truncated at the far end. Every name here is well inside it.
 *
 * ⚠️ ASCII ONLY. A name with an apostrophe in it MUST use the plain ASCII
 * one (U+0027). A word processor or a paste from a phone
 * will happily turn it into a typographic quote (U+2019), which is NOT in
 * LVGL's built-in Montserrat subset — the screen would show a hollow box, and
 * it would ride the wire into every sibling's peer list looking the same. wt.c
 * already carries this scar over a "·" in the status line.
 *
 * "" means unnamed and falls back to the MAC-derived id, which keeps a board
 * that has never been set both usable and unique. "Guest" stays for a visiting
 * child.
 *
 * Generic names for now. A first-boot name ENTRY screen (reusing the T9
 * keyboard) is the better long-term answer and is planned. */
static const char *NAMES[] = { "", "Kid 1", "Kid 2", "Kid 3", "Kid 4", "Parent", "Guest" };

static void name_label(char *o, size_t n)
{
    snprintf(o, n, "This handset  %s",
             settings_name()[0] ? settings_name() : "unnamed");
}
/* 🚨 A RENAME NOW TAKES EFFECT AT ONCE, ON BOTH TRANSPORTS.
 *
 * This used to save the name and stop, because espnow.c and wt.c each read
 * settings_name() ONCE at start-up. So the handset kept announcing its old name
 * to every sibling until it was power-cycled, and this very function logged
 * "reboot to rename on the walkie" — a known bug with a label on it rather than
 * a fix.
 *
 * 🔑 THAT WAS TOLERABLE WHEN THE NAMES WERE BOARDS AND SURVIVABLE ONCE. It is
 * not tolerable now they are people: the kids swap handsets, so renaming is a
 * daily act, and an in-range list is worth nothing if the names in it are
 * yesterday's. Both setters are cheap and neither touches the radio. */
static void name_adjust(int d)
{
    const int cnt = (int)(sizeof(NAMES) / sizeof(NAMES[0]));
    const char *cur = settings_name();
    int i = 0;
    for (; i < cnt; i++) if (strcmp(NAMES[i], cur) == 0) break;
    if (i >= cnt) i = 0;
    i = (i + d) % cnt;
    if (i < 0) i += cnt;
    settings_set_name(NAMES[i]);
    if (home_title) lv_label_set_text(home_title, NAMES[i]);
    /* Both transports, together. They must never disagree about who this is:
     * the same child would show up as two different people depending on
     * whether the sibling heard them over ESP-NOW or over the wifi channel. */
    espnow_set_name(NAMES[i]);
    wt_set_name(NAMES[i]);
    applog("[name] this handset is %s", NAMES[i][0] ? NAMES[i] : "unnamed");
}

/* 🎙 WHICH RADIO THE TALKIE USES. Auto is right for everyone; the forced modes
 * are a test harness, so the ESP-NOW voice path can be proved at the kitchen
 * table instead of first running for real in a car park. See wt_radio_t. */
static void wtradio_label(char *o, size_t n)
{
    static const char *W[] = { "auto", "esp-now", "wifi" };
    int r = settings_wt_radio();
    if (r < 0 || r > 2) r = 0;
    snprintf(o, n, "Talkie radio  %s", W[r]);
}

/* Takes effect on the next channel open — the transport is chosen once, in
 * wt_do_open(), and cannot change under a session that is already running.
 * Close the Talkie and come back rather than expecting a live switch. */
static void wtradio_adjust(int d)
{
    int r = settings_wt_radio() + d;
    while (r < 0) r += 3;
    settings_set_wt_radio(r % 3);
}

/* 🛰 LONG RANGE, AS A TEST SWITCH. See the note on settings_wt_range().
 * 🚨 BOTH HANDSETS MUST BE SET THE SAME OR THEY CANNOT HEAR EACH OTHER AT ALL —
 * that is the nature of LR, not a bug, and it is why the row says so. */
static void wtrange_label(char *o, size_t n)
{
    snprintf(o, n, "Talkie range  %s%s",
             settings_wt_range() ? "LONG" : "normal",
             settings_wt_range() ? "  (both ends!)" : "");
}

static void wtrange_adjust(int d)
{
    (void)d;
    settings_set_wt_range(settings_wt_range() ? 0 : 1);
    /* Peers were rate-configured when they were learned, so re-apply now
     * rather than making this need a reboot. */
    espnow_apply_rate();
}

/* 📻 WHICH CHANNEL ESP-NOW SITS ON. 1, 6 or 11 — the three non-overlapping
 * ones. 🚨 CHANGE ALL THE HANDSETS, not one: they cannot hear across channels.
 * A survey on 2026-08-26 found SIX access points on 6, all the builder's own mesh, and
 * nothing at all on 1 or 11. */
static void nowchan_label(char *o, size_t n)
{
    int ch = settings_espnow_channel();
    snprintf(o, n, "Radio channel  %d%s", ch,
             ch == 6 ? "  (busy here)" : "  (all handsets!)");
}

static void nowchan_adjust(int d)
{
    static const int CH[] = { 1, 6, 11 };
    int cur = settings_espnow_channel(), i = 1;
    for (int k = 0; k < 3; k++) if (CH[k] == cur) i = k;
    i = (i + d) % 3;
    if (i < 0) i += 3;
    espnow_set_channel(CH[i]);
}


static void timeout_label(char *o, size_t n)
{
    int to = settings_screen_timeout();
    if (to) snprintf(o, n, "Screen off after  %ds", to);
    else    snprintf(o, n, "Screen off after  never");
}
static void timeout_adjust(int d)
{
    /* 0 disables blanking entirely: useful on the bench, terrible on a battery,
     * hence last in the list rather than first. */
    static const int steps[] = { 15, 30, 45, 90, 0 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    settings_set_screen_timeout(
        steps[step_index(steps, n, settings_screen_timeout(), d)]);
}

/* 🚨 READ THE STORED GAIN, NEVER THE LIVE REGISTER.
 *
 * This row used to show — and edit — whatever 0x32 happened to hold. That was
 * already shaky, because the old volume code put `0dB + gain*pct/100` in there,
 * so the number was only the gain at 100% volume. The 2026-08-25 volume fix
 * makes it far worse: 0x32 is now `gain + attenuation`, so at a quarter volume
 * the row would read ~24dB LOW, and one press of + would SAVE that wrong number
 * as the board's gain. The tuning would quietly walk down every time it was
 * touched at anything but full volume.
 *
 * The stored value is the truth. It is what audio_set_volume() builds from and
 * what survives a reboot; the register is a derived, moment-to-moment thing.
 *
 * ⚠️ An untuned board stores 0 (Kid 1, measured 2026-08-25) — show that as 0dB,
 * which is what the volume code falls back to, rather than as "0x00, -95dB". */
/* 🔑 A JUDGEMENT MADE BY EYE BELONGS ON THE DEVICE, NOT IN A #define.
 *
 * The builder saw the first cover at 60/255 and said "i think you could have it stand
 * out a bit more" — which is exactly the kind of thing that would otherwise be
 * a rebuild-and-reflash per guess, on a screen I cannot see. Same reasoning as
 * the speaker gain: per board, in NVS, adjustable where it is being looked at.
 * Steps of 20 give twelve usable stops between invisible and full. */
static void artopa_label(char *o, size_t n)
{
    int v = settings_art_opa();
    if (v <= 0) snprintf(o, n, "Album art  off");
    else        snprintf(o, n, "Album art  %d%%", (v * 100) / 255);
}
static void artopa_adjust(int d)
{
    int v = settings_art_opa() + d * 20;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    settings_set_art_opa(v);
}

static void bright_label(char *o, size_t n)
{
    snprintf(o, n, "Brightness  %d%%", settings_brightness());
}
static void bright_adjust(int d)
{
    /* 10% steps, and the floor is BRIGHT_MIN_PCT rather than 0 — the setter
     * clamps, so stepping down past the bottom parks at the dimmest usable
     * level instead of blanking the screen the user is reading. */
    settings_set_brightness(settings_brightness() + d * 10);
}

/* Phase A of the voice relay. Deliberately ADJACENT TO Fleet rather than the
 * audio rows: what it changes is which board holds the Home Assistant token,
 * not how the assistant sounds. */
/* 🔑 A CYCLE, NOT A TOGGLE — corrected 2026-09-09.
 *
 * This was a two-state boolean, so with two gateways on the shelf there was no
 * way to say "ask via this gateway, not that one": espnow_gateway_id() decided on signal
 * strength alone and the row merely reported the outcome. It also froze solid
 * in the light build, where settings_ask_via_gw() is forced true and the
 * toggle wrote a value the getter ignored.
 *
 * Now it steps: [HANDSET ->] AUTO -> each gateway in range -> back. AUTO is
 * kept and kept FIRST because it is the right answer nearly always — it now
 * prefers a gateway that says it can reach HA, not merely a close one. */
static void askgw_gateways(uint16_t *ids, int *count)
{
    *count = 0;
    for (int i = 0; i < espnow_peer_count() && *count < 6; i++)
        if (espnow_peer_is_gateway(i)) ids[(*count)++] = espnow_peer_id(i);
}

static void askgw_label(char *o, size_t n)
{
    uint16_t pin = settings_ask_gw_id();
    if (pin) {
        /* Say when a pinned gateway is not here — otherwise the row claims a
         * choice that espnow_gateway_id() is quietly overriding. */
        int cnt; uint16_t ids[6];
        askgw_gateways(ids, &cnt);
        for (int i = 0; i < cnt; i++)
            if (ids[i] == pin) {
                snprintf(o, n, "Ask via  GW-%04X%s", (unsigned)pin,
                         espnow_gateway_ha_ok(pin) ? "" : "  (no HA)");
                return;
            }
        snprintf(o, n, "Ask via  GW-%04X - not in range", (unsigned)pin);
        return;
    }
    uint16_t gw = espnow_gateway_id();
    if (gw) snprintf(o, n, "Ask via  AUTO (GW-%04X)%s", (unsigned)gw,
                     espnow_gateway_ha_ok(gw) ? "" : "  (no HA)");
    else    snprintf(o, n, "Ask via  AUTO - no gateway in range");
}

static void askgw_adjust(int d)
{
    (void)d;
    int cnt; uint16_t ids[6];
    askgw_gateways(ids, &cnt);

    uint16_t pin = settings_ask_gw_id();

    if (!pin) {                       /* AUTO -> first gateway */
        if (cnt) settings_set_ask_gw_id(ids[0]);
        return;
    }
    for (int i = 0; i < cnt; i++) {   /* this gateway -> the next one */
        if (ids[i] != pin) continue;
        if (i + 1 < cnt) settings_set_ask_gw_id(ids[i + 1]);
        else {
            settings_set_ask_gw_id(0);
        }
        return;
    }
    settings_set_ask_gw_id(0);        /* pinned one has gone: back to AUTO */
}

static void gain_label(char *o, size_t n)
{
    int raw = settings_dac_gain();
    if (raw <= 0) raw = ES8311_0DB_UI;
    snprintf(o, n, "Speaker gain  0x%02X  %+ddB", raw, audio_dac_gain_db(raw));
}
static void gain_adjust(int d)
{
    int raw = settings_dac_gain();
    if (raw <= 0) raw = ES8311_0DB_UI;
    raw += d * 6;
    if (raw > 255) raw = 255;
    if (raw < ES8311_0DB_UI) raw = ES8311_0DB_UI;     /* floor at 0dB */
    settings_set_dac_gain(raw);
    /* Re-apply through the volume, so the new gain is heard at the CURRENT
     * slider position — writing the register directly would be undone by the
     * next volume change anyway. */
    audio_set_volume(player_state()->volume);
    applog("[gain] DACVOL 0x%02X (%+ddB)", raw, audio_dac_gain_db(raw));
    /* A beep, because a gain change you cannot hear is a gain change you cannot
     * judge. */
    xTaskCreatePinnedToCore(beep_task, "gainbeep", 3072, NULL, 4, NULL, 1);
}

/* A/B toggle. The EQ is a judgement about one specific speaker, and the boards
 * now have DIFFERENT speakers, which may well want different answers. */
static void eq_label(char *o, size_t n)
{
    snprintf(o, n, "Speaker EQ  %s", audio_eq() ? "ON" : "off");
}
static void eq_adjust(int d) { (void)d; audio_set_eq(!audio_eq());
    applog("[eq] speaker EQ %s", audio_eq() ? "on" : "off"); }

/* Same reasoning as the EQ row: whether ALC helps is a judgement about one
 * child, one room and one mic, made by listening to what the far end receives. */
static void alc_label(char *o, size_t n)
{
    snprintf(o, n, "Mic auto-level  %s", audio_alc() ? "ON" : "off");
}
static void alc_adjust(int d)
{
    (void)d;
    bool on = !audio_alc();
    settings_set_alc(on);
    audio_set_alc(on);
    audio_dump_codec_regs("alc ");
}

static void sleepmin_label(char *o, size_t n)
{
    int sm = settings_sleep_minutes();
    if (sm) snprintf(o, n, "Deep sleep after  %dmin", sm);
    else    snprintf(o, n, "Deep sleep after  never");
}
static void sleepmin_adjust(int d)
{
    /* 0 (never) is FIRST and the default, because a board that sleeps unasked
     * cannot be flashed - see power.h. */
    static const int steps[] = { 0, 5, 15, 30, 60 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    settings_set_sleep_minutes(
        steps[step_index(steps, n, settings_sleep_minutes(), d)]);
}

static void alarm_label(char *o, size_t n)
{
    int al = settings_alarm();
    if (al < 0) snprintf(o, n, "Alarm  off");
    else snprintf(o, n, "Alarm  %02d:%02d%s", al / 100, al % 100,
                  power_time_valid() ? "" : "  (no clock yet)");
}
static void alarm_adjust(int d)
{
    /* Half-hour steps through a sensible morning window, then off. A full time
     * picker is not worth a screen for something set once. */
    static const int steps[] = { -1, 630, 700, 730, 800, 830 };
    const int n = (int)(sizeof(steps) / sizeof(steps[0]));
    settings_set_alarm(steps[step_index(steps, n, settings_alarm(), d)]);
}

/* ---- the action rows ---------------------------------------------------- */

/* 🚨 RESTART, BECAUSE THE HONEST ANSWER TO SOME FAULTS IS A REBOOT.
 *
 * The builder, 2026-08-21: "we need a reboot option in the menu". Two of the failures
 * this firmware hits in the field clear on a restart and nothing else: an
 * address collision with another handset (see IP_TABLE in net.c) and internal
 * RAM sitting too close to the ~44KB WiFi needs after a long session, which
 * presents as "no wifi" with a perfect AP two metres away.
 *
 * ⚠️ TWO TAPS, AND IT DISARMS ITSELF. A one-tap restart in a list a child
 * scrolls would eventually be hit mid-song, and a confirmation that stays armed
 * for ever is only a slower accident.
 *
 * 🔑 FLUSH SETTINGS FIRST. settings_tick() writes NVS lazily, so a restart
 * seconds after a change would throw it away - and it would look like the
 * setting not sticking. */
static uint32_t  s_restart_armed_ms;
static lv_obj_t *menu_restart_lbl;

static void menu_restart_disarm(void)
{
    s_restart_armed_ms = 0;
    if (menu_restart_lbl) lv_label_set_text(menu_restart_lbl, "Restart");
}

static void menu_restart_clicked(lv_event_t *e)
{
    lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
    menu_restart_lbl = l;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (!s_restart_armed_ms || (int32_t)(now - s_restart_armed_ms) > 5000) {
        s_restart_armed_ms = now ? now : 1;
        if (l) lv_label_set_text(l, "Restart  - tap again");
        return;
    }
    if (l) lv_label_set_text(l, "Restarting...");
    applog("[ui] restart from the menu");
    settings_flush();
    player_suspend();
    lv_timer_handler();               /* let that label actually paint */
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

/* Long press on the mic row: force the servo to its loudest target. Diagnostic
 * only, and NOT saved - a reboot returns to whatever the toggle says. It answers
 * "is the servo running at all", which a normal-strength A/B cannot. */
static void menu_alc_long(lv_event_t *e)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    audio_alc_probe();
    if (lbl) lv_label_set_text(lbl, "Mic auto-level  MAX (test)");
}

static void restart_label(char *o, size_t n) { snprintf(o, n, "Restart"); }
static void sleep_label(char *o, size_t n)   { snprintf(o, n, "Sleep now"); }
static void log_label(char *o, size_t n)     { snprintf(o, n, "Show log"); }
static void cal_label(char *o, size_t n)     { snprintf(o, n, "Calibrate touch (hold)"); }
static void fleet_label(char *o, size_t n)   { snprintf(o, n, "Fleet"); }
static void menu_fleet_clicked(lv_event_t *e) { (void)e; ui_show_fleet(); }
static void gps_label(char *o, size_t n)     { snprintf(o, n, "GPS"); }
static void gpsdump_label(char *o, size_t n) { snprintf(o, n, "Dump GPS log"); }

/* 🔑 THE THINGS TO TRY WHEN IT TRACKS PLENTY AND NEVER FIXES.
 *
 * Measured 2026-09-12: 20 satellites individually above 32 dB-Hz, the 4th
 * strongest at 35 dB, 108 unbroken seconds of that, and `used` stayed at 0 for
 * all 743 rows. Signal was never the problem, so the remaining candidates are
 * the module's own state and its constellation mode:
 *
 *  - GPS+BDS together requires resolving the offset between two time systems.
 *    A module that cannot do that tracks both perfectly and still refuses to
 *    produce a position. Dropping to GPS-only removes the problem entirely.
 *  - A corrupted stored almanac looks identical and a cold restart clears it.
 *
 * Neither is saved to flash: both revert on a power cycle, so a wrong guess
 * cannot make anything permanently worse. */
static void gpscold_label(char *o, size_t n)  { snprintf(o, n, "GPS cold restart"); }
static void monhw_label(char *o, size_t n)    { snprintf(o, n, "GPS antenna probe"); }

static void gps_cmd_common(lv_event_t *e, const char *cmd, const char *said)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (!gps_running()) gps_start();      /* the UART must be open to talk */
    bool ok = gps_send_cmd(cmd);
    if (lbl) lv_label_set_text(lbl, ok ? said : "GPS not running");
}
static void menu_gpscold_clicked(lv_event_t *e)
{ gps_cmd_common(e, "PCAS10,2", "GPS cold restart  sent"); }
/* ⚠️ Dumps to the CONSOLE, not the screen: a session is thousands of lines and
 * the on-screen log holds 64. This is for when a cable is attached. */
static void menu_gpsdump_clicked(lv_event_t *e)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (lbl) lv_label_set_text(lbl, "Dump GPS log  ...to serial");
    gpslog_dump(0);
}
static void menu_gps_clicked(lv_event_t *e)  { (void)e; ui_show_gps(); }

/* 🔑 THE MULTIMETER TEST, DONE IN FIRMWARE.
 * The antenna is ACTIVE (32 dB LNA, 3-5 V, 10 mA) and the U.FL connector is too
 * small to probe safely. The AT6558 senses the antenna's feed current itself, so
 * the chip can be ASKED instead of measured.
 * ⚠️ ONE probe proves nothing — the payload offsets are unknown. Probe with the
 * antenna connected, then again with it unplugged, and the bytes that MOVE are
 * the answer. Needs the GPS screen open first: the UART must be up. */
static void menu_monhw_clicked(lv_event_t *e)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_event_get_user_data(e);
    if (!gps_running()) gps_start();
    gps_monhw_probe();
    if (lbl) lv_label_set_text(lbl, "GPS antenna probe  ...to serial");
}

/* 🔑 THE TEST THE A/B CANNOT DO.
 *
 * The A/B alternates every 30 s, which is enough to compare C/N0 between two
 * conditions but NOT enough to see a fix appear: one ephemeris is 18 unbroken
 * seconds and a cold TTFF is minutes. The builder's evidence was a handset ASLEEP in a
 * pocket for the length of a shopping trip — the radio off, continuously, for
 * long enough to matter. So: a plain latch, not an experiment. Turn it off,
 * walk outside, leave it alone.
 *
 * net_down() releases the driver AND its memory, taking ESP-NOW with it, which
 * is as close to the sleeping condition as this firmware gets while still
 * running the receiver and writing the log. */
/* 🚨 THE SD CARD, AND WHY IT WAS NEVER A SUSPECT UNTIL THE SECOND MODULE
 * BEHAVED IDENTICALLY.
 *
 * Two different receivers, same fault: tracks 24 satellites, 9+ decodable, 33 dB,
 * never computes a fix. That is not a bad unit. So the common factor is this
 * handset - and the one thing present in EVERY measured session and absent the
 * single time it fixed (asleep in the builder's pocket outside a shop) is the LOGGER.
 *
 * `library_mount()` runs the card at SDMMC_FREQ_HIGHSPEED - 40 MHz - and its
 * harmonics land at 1560 and 1600 MHz, straddling GPS L1 at 1575.42, exactly
 * the risk the ROADMAP flagged for the display. The display A/B cleared the
 * display; the card was writing a row a second through BOTH halves of it.
 *
 * 🔑 So the instrument may have been destroying the measurement for two days:
 * every session that could record evidence was also the only kind of session
 * that could not succeed. This latch is the test - GPS screen, no card writes. */
static bool s_sdlog_off;
static bool s_radio_hushed;
static void hush_label(char *o, size_t n)
{ snprintf(o, n, s_radio_hushed ? "Radio HUSHED for GPS" : "Hush radio for GPS"); }

static void sdlog_label(char *o, size_t n)
{ snprintf(o, n, s_sdlog_off ? "SD log OFF (quiet)" : "SD log on"); }

static void menu_sdlog_clicked(lv_event_t *e)
{
    lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
    s_sdlog_off = !s_sdlog_off;
    if (s_sdlog_off) {
        gpslog_stop();
        applog("[sdlog] logging OFF - the card is idle while acquiring");
    } else {
        applog("[sdlog] logging back on");
    }
    if (l) lv_label_set_text(l, s_sdlog_off ? "SD log OFF (quiet)" : "SD log on");
}

static void menu_hush_clicked(lv_event_t *e)
{
    lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
    s_radio_hushed = !s_radio_hushed;
    if (s_radio_hushed) {
        espnow_beacon_quiet(true);
        net_down();
        applog("[hush] radio down - GPS has the band to itself");
        gpslog_set_display_state("hushed");
    } else {
        net_retry_reset();
        net_start_radio();
        applog("[hush] radio back up");
        gpslog_set_display_state("quiet");
    }
    if (l) lv_label_set_text(l, s_radio_hushed ? "Radio HUSHED for GPS"
                                              : "Hush radio for GPS");
}
static void map_label(char *o, size_t n)     { snprintf(o, n, "Map"); }
static void menu_map_clicked(lv_event_t *e)  { (void)e; ui_show_map(); }

/* 🔑 ONE ROW, THREE EXPERIMENTS, ONE WALK. The builder, 2026-09-14: "make one test
 * that would run down all 3 test we wont to do with one button". The legs must
 * be interleaved anyway to survive the sky moving, so there was never a good
 * reason for three separate rows and three separate trips. */
static void field_label(char *o, size_t n)
{
    if (seq_running()) {
        int t = seq_secs_left();
        snprintf(o, n, "FIELD TEST %s %d:%02d - tap to stop",
                 leg_name(ab_mode), t / 60, t % 60);
    } else {
        snprintf(o, n, "GPS field test  %d min",
                 (AB_LEGS * AB_BLOCKS * AB_BLOCK_MS) / 60000);
    }
}
static void menu_field_clicked(lv_event_t *e)
{
    (void)e;
    if (seq_running()) { seq_abort("stopped from the menu"); return; }
    seq_begin();
}

/* ---- USB card mode: the SD card, on the PC, case closed ----------------
 *
 * 🔑 WHY IT IS A MENU ROW AND NOT A BOOT MODE. Entering at runtime means the
 * ESP32 never resets to get here, and the way OUT is the reset — so the GPS
 * module, whose 3V3 rail the ESP32 does not switch, keeps its almanac across
 * the whole trip. The builder's overnight fix on 2026-09-13 is exactly the thing this
 * must not throw away.
 *
 * ⚠️ TWO TAPS, like Restart, and for the same reason: this stops the music and
 * takes the card away from the firmware, and it sits in a list a child scrolls.
 *
 * ⚠️ THE CONSOLE DIES THE MOMENT THIS RUNS. USB-Serial-JTAG and USB-OTG share
 * the S3's two USB pins and TinyUSB takes them, so the log you are reading
 * stops mid-line and /dev/ttyACM0 disappears. That is expected. It is also why
 * the panel below has to say everything on the SCREEN — after this point
 * nothing the firmware prints can reach a PC. */
static uint32_t  s_usb_armed_ms;
static lv_obj_t *usb_panel;

static void usbmsc_label(char *o, size_t n) { snprintf(o, n, "USB card mode"); }

/* 🚨 RELEASE THE USB HARDWARE FIRST. esp_restart() on its own leaves the pins
 * with the OTG controller, so the handset reboots into a perfectly healthy
 * state that no PC can see — which is exactly how card mode came to need the
 * reset button and an open case. See usbmsc.h. */
static void usb_reboot_clicked(lv_event_t *e)
{
    (void)e;
    settings_flush();
    usbmsc_release();
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void usb_panel_show(const char *headline, const char *detail, bool ok)
{
    if (usb_panel) { lv_obj_del(usb_panel); usb_panel = NULL; }

    usb_panel = lv_obj_create(lv_layer_top());
    lv_obj_set_size(usb_panel, LCD_W, LCD_H);
    lv_obj_center(usb_panel);
    lv_obj_set_style_bg_color(usb_panel, C_PANEL, 0);
    lv_obj_set_style_border_width(usb_panel, 0, 0);
    lv_obj_clear_flag(usb_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = mk_label(usb_panel, &lv_font_montserrat_14,
                           ok ? C_AMBER : C_INK, headline);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *d = mk_label(usb_panel, &lv_font_montserrat_14, C_INK, detail);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d, LCD_W - 30);
    lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 46);

    /* The instruction is half the feature: an SD card yanked out from under a
     * PC that has not flushed its writes is how a music library dies. */
    lv_obj_t *h = mk_label(usb_panel, &lv_font_montserrat_14, C_MUTED,
                           ok ? "Copy your files, then EJECT the drive\n"
                                "on the PC before tapping Reboot."
                              : "Nothing was changed. Reboot and try again.");
    lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(h, LCD_W - 30);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 120);

    lv_obj_t *b = lv_btn_create(usb_panel);
    lv_obj_set_size(b, 150, 48);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_color(b, C_AMBER, 0);
    lv_obj_add_event_cb(b, usb_reboot_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(b, &lv_font_montserrat_14, C_ONAMBER, "Reboot"));
}

/* 🚨 UNPLUGGING IN CARD MODE LEAVES THE HANDSET MUTE, AND IT LOOKS BROKEN.
 *
 * Measured 2026-09-13, on the builder: the cable came out while the card was still
 * handed to the PC, and on the next plug-in nothing enumerated at all — no
 * chime, no serial port, no disk. TinyUSB owns the USB pins until a reset, so a
 * handset left in this state has no console to say so and no obvious way back.
 *
 * Card mode only exists WHILE a cable is attached, so losing the cable IS the
 * exit. Rebooting on our own restores the console and costs nothing — and a
 * software restart does not drop the GPS module's 3V3 rail, so it does not
 * throw away an almanac either. Three seconds of grace so a wobbly connector
 * cannot trigger it. */
static void usbmsc_unplug_tick(lv_timer_t *t)
{
    (void)t;
    static uint32_t gone_since;
    if (!usbmsc_active()) { gone_since = 0; return; }

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    /* ⚠️ usbmsc_host_present(), NOT battery_on_usb() — see the note in
     * usbmsc.h. The obvious signal is the one card mode switches off. */
    if (usbmsc_host_present()) { gone_since = 0; return; }
    if (!gone_since) { gone_since = now ? now : 1; return; }
    if ((int32_t)(now - gone_since) < 5000) return;

    applog("[usb] cable gone in card mode - rebooting to get the console back");
    settings_flush();
    usbmsc_release();
    esp_restart();
}

static void menu_usbmsc_clicked(lv_event_t *e)
{
    lv_obj_t *l = (lv_obj_t *)lv_event_get_user_data(e);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    if (!s_usb_armed_ms || (int32_t)(now - s_usb_armed_ms) > 5000) {
        s_usb_armed_ms = now ? now : 1;
        if (l) lv_label_set_text(l, "USB card mode  - tap again");
        return;
    }
    s_usb_armed_ms = 0;
    if (l) lv_label_set_text(l, "USB card mode  ...handing over");
    settings_flush();
    lv_timer_handler();                /* paint that before the card goes */

    esp_err_t err = usbmsc_enter();
    if (err == ESP_OK) {
        usb_panel_show("USB card mode", usbmsc_status(), true);
    } else {
        usb_panel_show("USB card mode FAILED", usbmsc_status(), false);
    }
}

/* ---- the table --------------------------------------------------------- */

static menu_item_t MENU[] = {
    /* Restart is second-from-top on purpose: it is what gets reached for when
     * the handset says "no wifi", and this list scrolls. */
    { restart_label,  NULL,            menu_restart_clicked, NULL,           H_AMBER, NULL, NULL },
    /* Third row: reached for when the headphones stop, which is often. */
    { name_label,     name_adjust,     NULL,                 NULL,           H_AMBER, NULL, NULL },
    { wtradio_label,  wtradio_adjust,  NULL,                 NULL,           H_AMBER, NULL, NULL },
    { wtrange_label,  wtrange_adjust,  NULL,                 NULL,           H_AMBER, NULL, NULL },
    { nowchan_label,  nowchan_adjust,  NULL,                 NULL,           H_AMBER, NULL, NULL },
    { fleet_label,    NULL,            menu_fleet_clicked,   NULL,           H_AMBER, NULL, NULL },
    /* ⚠️ In the LIGHT build this belongs on the home hub in the Radio tile's
     * place, not buried in Settings — that build has no WiFi, so its Radio
     * screen can never show anything, and GPS is its only clock. See
     * GPS-FIELD-TEST-PLAN.md section 4. Doing that is a separate change; it
     * lives here first so the instrument is reachable in both builds. */
    { gps_label,      NULL,            menu_gps_clicked,     NULL,           H_AMBER, NULL, NULL },
    { map_label,      NULL,            menu_map_clicked,     NULL,           H_AMBER, NULL, NULL },
    { field_label,    NULL,            menu_field_clicked,   NULL,           H_AMBER, NULL, NULL },
    { hush_label,     NULL,            menu_hush_clicked,    NULL,           H_AMBER, NULL, NULL },
    { sdlog_label,    NULL,            menu_sdlog_clicked,   NULL,           H_AMBER, NULL, NULL },
    { gpsdump_label,  NULL,            menu_gpsdump_clicked, NULL,           H_MUTED, NULL, NULL },
    { gpscold_label,  NULL,            menu_gpscold_clicked, NULL,           H_MUTED, NULL, NULL },
    { monhw_label,    NULL,            menu_monhw_clicked,   NULL,           H_MUTED, NULL, NULL },
    { askgw_label,    askgw_adjust,    NULL,                 NULL,           H_AMBER, NULL, NULL },
    { gain_label,     gain_adjust,     NULL,                 NULL,           H_AMBER, NULL, NULL },
    { artopa_label,   artopa_adjust,   NULL,                 NULL,           H_AMBER, NULL, NULL },
    /* Next to the display rows below, not the audio ones above: it is reached
     * for at bedtime, alongside Timeout. */
    { bright_label,   bright_adjust,   NULL,                 NULL,           H_INK, NULL, NULL },
    { eq_label,       eq_adjust,       NULL,                 NULL,           H_AMBER, NULL, NULL },
    { alc_label,      alc_adjust,      NULL,                 menu_alc_long,  H_AMBER, NULL, NULL },
    { timeout_label,  timeout_adjust,  NULL,                 NULL,           H_INK, NULL, NULL },
    { sleepmin_label, sleepmin_adjust, NULL,                 NULL,           H_INK, NULL, NULL },
    { alarm_label,    alarm_adjust,    NULL,                 NULL,           H_INK, NULL, NULL },
    /* Long press = real deep sleep, so it is reachable for testing without
     * being a one-tap way to make a USB-connected board vanish. */
    { sleep_label,    NULL,            menu_sleep_clicked,   menu_sleep_long, H_INK, NULL, NULL },
    /* Next to Show log and Calibrate: the maintenance end of the list, not
     * anywhere a thumb lands while scrolling for the volume rows. */
    { usbmsc_label,   NULL,            menu_usbmsc_clicked,  NULL,           H_MUTED, NULL, NULL },
    { log_label,      NULL,            menu_log_clicked,     NULL,           H_MUTED, NULL, NULL },
    { cal_label,      NULL,            menu_cal_clicked,     menu_cal_long,  H_MUTED, NULL, NULL },
};
#define MENU_N ((int)(sizeof(MENU) / sizeof(MENU[0])))

static void menu_paint(int i)
{
    char buf[64];
    if (i < 0 || i >= MENU_N || !MENU[i].lbl) return;
    MENU[i].label(buf, sizeof(buf));
    lv_label_set_text(MENU[i].lbl, buf);
}

static void menu_refresh_all(void)
{
    for (int i = 0; i < MENU_N; i++) menu_paint(i);
}

static void menu_select(int i)
{
    if (i < 0 || i >= MENU_N || !MENU[i].adjust) return;
    if (menu_sel >= 0 && MENU[menu_sel].row) {
        lv_obj_set_style_bg_color(MENU[menu_sel].row, C_PANEL, 0);
        lv_obj_set_style_border_width(MENU[menu_sel].row, 0, 0);
    }
    menu_sel = i;
    lv_obj_set_style_bg_color(MENU[i].row, C_LINE, 0);
    lv_obj_set_style_border_color(MENU[i].row, C_AMBER, 0);
    lv_obj_set_style_border_width(MENU[i].row, 2, 0);
    /* The +/- are dead until something adjustable is selected, and they should
     * look it. */
    lv_obj_clear_state(menu_minus, LV_STATE_DISABLED);
    lv_obj_clear_state(menu_plus,  LV_STATE_DISABLED);
}

static void menu_row_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= MENU_N) return;
    if (MENU[i].adjust) menu_select(i);
}

static void menu_adjust_clicked(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (menu_sel < 0 || !MENU[menu_sel].adjust) return;
    MENU[menu_sel].adjust(d);
    menu_paint(menu_sel);
}

static lv_obj_t *mk_foot_btn(lv_obj_t *parent, lv_align_t al, int dx, int w,
                             const lv_font_t *font, uint32_t colour,
                             const char *txt, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    flat(b);
    lv_obj_set_size(b, w, 34);
    lv_obj_align(b, al, dx, 0);
    lv_obj_set_style_bg_color(b, C_LINE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_color(b, C_AMBER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = mk_label(b, font, lv_color_hex(colour), txt);
    lv_obj_center(l);
    return b;
}

static void build_menu_screen(void)
{
    scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_menu, C_BG, 0);
    lv_obj_clear_flag(scr_menu, LV_OBJ_FLAG_SCROLLABLE);
    mk_statusbar(scr_menu, &menu_title, &menu_batt, "Settings");

    /* 🔑 THE LIST SCROLLS, THE SCREEN DOES NOT. That one change is what keeps
     * Home and +/- on screen; before this the whole screen scrolled and they
     * went with it. */
    menu_list = lv_obj_create(scr_menu);
    flat(menu_list);
    /* 🚨 flat() CLEARS LV_OBJ_FLAG_SCROLLABLE — ITS LAST LINE — SO PUT IT BACK.
     *
     * The builder, 2026-08-21: "the menu wont scroll so the stuff that not on the screen
     * you cant get 2". Setting the scroll direction and the scrollbar mode on an
     * object that has been told it cannot scroll does exactly nothing, and it
     * reads as correct code: three scroll-related calls in a row, one of which
     * silently undoes the other two. flat() is right to clear it for the panels
     * it was written for; this is the one place that wants it back. */
    lv_obj_add_flag(menu_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(menu_list, LCD_W, LCD_H - BAR_H - MENU_FOOT_H);
    lv_obj_align(menu_list, LV_ALIGN_TOP_MID, 0, BAR_H);
    lv_obj_set_style_bg_opa(menu_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(menu_list, 0, 0);
    lv_obj_set_scroll_dir(menu_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(menu_list, LV_SCROLLBAR_MODE_AUTO);

    int y = 4;
    for (int i = 0; i < MENU_N; i++) {
        lv_obj_t *b = lv_btn_create(menu_list);
        flat(b);
        lv_obj_set_size(b, LCD_W - 16, MENU_ROW_H);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
        lv_obj_set_style_bg_color(b, C_PANEL, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_border_color(b, C_AMBER, 0);
        lv_obj_set_style_bg_color(b, C_LINE, LV_STATE_PRESSED);

        char buf[64];
        MENU[i].label(buf, sizeof(buf));
        lv_obj_t *l = mk_label(b, &lv_font_montserrat_14,
                               lv_color_hex(MENU[i].colour), buf);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);

        MENU[i].row = b;
        MENU[i].lbl = l;

        /* An action row runs its callback (and gets the label as user data, so
         * Restart can rewrite itself); a value row only selects. */
        if (MENU[i].adjust)
            lv_obj_add_event_cb(b, menu_row_clicked, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
        else if (MENU[i].action)
            lv_obj_add_event_cb(b, MENU[i].action, LV_EVENT_CLICKED, l);
        if (MENU[i].long_press)
            lv_obj_add_event_cb(b, MENU[i].long_press, LV_EVENT_LONG_PRESSED, l);

        y += MENU_ROW_DY;
    }
    /* The container clips at its own height, so the last row needs the pad or it
     * cannot be scrolled fully into view. */
    lv_obj_set_style_pad_bottom(menu_list, 8, 0);

    /* ---- the fixed footer: Home, and the one pair of +/- ---- */
    lv_obj_t *foot = lv_obj_create(scr_menu);
    flat(foot);
    lv_obj_set_size(foot, LCD_W, MENU_FOOT_H);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot, C_PANEL, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, C_LINE, 0);
    lv_obj_set_style_border_width(foot, 1, 0);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_SCROLLABLE);

    mk_foot_btn(foot, LV_ALIGN_LEFT_MID, 6, 96, &lv_font_montserrat_14,
                0x6E9BD1, LV_SYMBOL_LEFT " Home", menu_back_clicked, NULL);
    menu_minus = mk_foot_btn(foot, LV_ALIGN_RIGHT_MID, -52, 44,
                             &lv_font_montserrat_22, H_INK, "-",
                             menu_adjust_clicked, (void *)(intptr_t)-1);
    menu_plus  = mk_foot_btn(foot, LV_ALIGN_RIGHT_MID, -6, 44,
                             &lv_font_montserrat_22, H_INK, "+",
                             menu_adjust_clicked, (void *)(intptr_t)+1);

    /* Start with the first adjustable row selected, so +/- always mean
     * something the moment the screen opens. */
    for (int i = 0; i < MENU_N; i++)
        if (MENU[i].adjust) { menu_select(i); break; }
}


/* ------------------------------------------------------------------ *
 * internet radio
 *
 * A list of stations and one status line. Deliberately not a second player
 * screen: there is no seeking, no track list and no progress on a live stream,
 * so the only controls that mean anything are "which station" and "stop".
 * ------------------------------------------------------------------ */

static lv_obj_t *radio_title, *radio_rows, *radio_stat;

static void radio_back_clicked(lv_event_t *e) { ui_show_home(); }
static void radio_stop_clicked(lv_event_t *e) { radio_stop(); }

static void radio_row_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    /* Tapping the station that is already playing stops it - the same control
     * doing the obvious opposite thing, rather than a separate mode. */
    if (radio_playing() && radio_current() == idx) radio_stop();
    else                                            radio_play(idx);
}

static void radio_refresh(void)
{
    if (!radio_stat) return;
    const char *st = radio_status();
    int cur = radio_current();
    if (radio_playing() && cur >= 0)
        lv_label_set_text_fmt(radio_stat, "%s  -  %s", radio_name(cur), st);
    else
        lv_label_set_text(radio_stat, st[0] ? st : "pick a station");

    /* Highlight whichever row is live, so the screen answers "what am I
     * listening to" without reading the status line. */
    uint32_t n = lv_obj_get_child_cnt(radio_rows);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(radio_rows, i);
        bool live = radio_playing() && cur == (int)i;
        lv_obj_set_style_bg_color(row, live ? C_GREEN : C_PANEL, 0);
        lv_obj_t *l = lv_obj_get_child(row, 0);
        if (l) lv_obj_set_style_text_color(l, live ? C_ONAMBER : C_INK, 0);
    }
}

static void build_radio_screen(void)
{
    scr_radio = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_radio, C_BG, 0);
    lv_obj_clear_flag(scr_radio, LV_OBJ_FLAG_SCROLLABLE);
    mk_statusbar(scr_radio, &radio_title, &radio_batt, "Radio");

    radio_stat = mk_label(scr_radio, &lv_font_montserrat_12, C_MUTED, "");
    lv_label_set_long_mode(radio_stat, LV_LABEL_LONG_DOT);
    lv_obj_set_width(radio_stat, LCD_W - 16);
    lv_obj_align(radio_stat, LV_ALIGN_TOP_LEFT, 8, BAR_H + 4);

    radio_rows = lv_obj_create(scr_radio);
    flat(radio_rows);
    /* 26px shorter than it was — that is the volume row's space, and a list that
     * scrolls loses far less by being one row shorter than a radio loses by
     * having no volume at all. */
    lv_obj_set_size(radio_rows, LCD_W, LCD_H - BAR_H - 22 - FOOT_H - 8 - 26);
    lv_obj_align(radio_rows, LV_ALIGN_TOP_LEFT, 0, BAR_H + 22);
    lv_obj_add_flag(radio_rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(radio_rows, LV_DIR_VER);

    for (int i = 0; i < radio_count(); i++) {
        lv_obj_t *row = lv_btn_create(radio_rows);
        flat(row);
        lv_obj_set_size(row, LCD_W - 16, 38);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, i * 42);
        lv_obj_set_style_bg_color(row, C_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_color(row, C_LINE, LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, radio_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = mk_label(row, &lv_font_montserrat_14, C_INK, radio_name(i));
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
    }

    mk_volume_row(scr_radio, 12, -48, 122);
    mk_sink_btn(scr_radio, -8, -44);

    lv_obj_t *stop = lv_btn_create(scr_radio);
    flat(stop);
    lv_obj_set_size(stop, 80, 32);
    lv_obj_align(stop, LV_ALIGN_BOTTOM_LEFT, 8, -10);
    lv_obj_set_style_bg_color(stop, C_PANEL, 0);
    lv_obj_set_style_bg_opa(stop, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(stop, 4, 0);
    lv_obj_add_event_cb(stop, radio_stop_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sl = mk_label(stop, &lv_font_montserrat_14, C_RED, "Stop");
    lv_obj_center(sl);

    lv_obj_t *back = lv_btn_create(scr_radio);
    flat(back);
    lv_obj_set_size(back, 90, 32);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -8, -10);
    lv_obj_set_style_bg_color(back, C_PANEL, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 4, 0);
    lv_obj_add_event_cb(back, radio_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(back, &lv_font_montserrat_14, C_BLUE,
                            LV_SYMBOL_LEFT " Home");
    lv_obj_center(bl);
}

void ui_show_radio(void) { ui_load(scr_radio); radio_refresh(); vol_rows_sync(); sink_btns_sync(); }


/* ------------------------------------------------------------------ *
 * GPS — the sky view
 *
 * 🔑 THIS SCREEN IS AN INSTRUMENT, NOT A MAP. It exists to answer one question:
 * is this antenna placement any good? A map with a pin answers that with one
 * bit of information, minutes late. Carrier-to-noise answers it in numbers, in
 * about thirty seconds, and — the part that makes the whole workflow possible —
 * IT DOES NOT NEED A FIX. GSV reports C/N0 for every satellite being TRACKED,
 * and tracking starts long before a position solution. So two case placements
 * can be compared back to back while the sky overhead is effectively unchanged,
 * instead of waiting out a cold start for each one.
 *
 * ⚠️ The plot is built from plain objects rather than an lv_canvas on purpose:
 * a 240x200 RGB565 canvas is 96KB, and this board's internal RAM sits near the
 * WiFi threshold whenever audio is open. Thirty-two 7px circles cost nothing
 * and LVGL already knows how to redraw them.
 * ------------------------------------------------------------------ */

#define SKY_R   88                      /* horizon radius, px */
#define SKY_CX  (LCD_W / 2)
#define SKY_CY  (BAR_H + 8 + SKY_R)

static lv_obj_t *gps_dot[GPS_MAX_SATS];
static lv_obj_t *gps_l1, *gps_l2, *gps_l3, *gps_l4, *gps_mark_lbl;

static void gps_mark_clicked(lv_event_t *e)
{
    (void)e;
    char c = gpslog_mark();
    if (!c) applog("[gpslog] no session - is the card in?");
}

/* ------------------------------------------------------------------
 * THE DISPLAY DESENSE A/B — §1.5 of the plan, finally built
 *
 * 🔑 THE BUILDER'S FIELD OBSERVATION, 2026-09-13, AND IT IS THE BEST LEAD OF THE WHOLE
 * GPS SAGA. He slept the handset outside a shop, woke it on the way out and it
 * had EIGHT satellites on the used line — a fix, the thing ~2 hours of awake
 * outdoor testing never produced — and *"as soon as i picked it up and tried to
 * use it the fix satalites all whent away"*.
 *
 * What is different in deep sleep is everything: the display and its 40 MHz SPI
 * clock, WiFi, ESP-NOW, the CPU at full speed. The ROADMAP already predicted the
 * display: 40 MHz harmonics land near 1560 and 1600 MHz and STRADDLE GPS L1 at
 * 1575.42. Nobody has ever measured it — every outdoor session so far ran with
 * the screen on, so the "bad placement" and "faulty module" conclusions were
 * both drawn from a condition that was never varied.
 *
 * 🔑 THE FALSIFYING OBSERVATION, NAMED BEFORE THE RUN: if the delta between
 * loud and quiet is under ~1 dB the display is innocent and I go back to the
 * antenna. If it is 3-5 dB it is the answer, and it costs no hardware to fix.
 * ------------------------------------------------------------------ */

static int      ab_block;
static uint32_t ab_next_ms;

/* 🔑 THE SECOND EXPERIMENT, AND IT IS THE ONE THAT MATCHES WHAT THE BUILDER SAW.
 *
 * The display A/B came back +0.05 dB over ~180 samples each way — the screen is
 * innocent, and placement and shielding go with it. But that test varied the
 * DISPLAY and nothing else: WiFi and ESP-NOW ran in both halves. The condition
 * that actually produced a fix was DEEP SLEEP, where the radio is off too.
 *
 * So: same six minutes, same blocks, same log tags — the variable is the radio.
 * quiet = net_down(), the whole driver torn down, which is the closest this
 * firmware gets to the state the handset was in inside the builder's pocket. */
static uint32_t ab_cn0[2], ab_dec[2], ab_n[2];   /* [0] = quiet, [1] = loud */

/* The radio is expensive to cycle (~30KB of driver memory) and three of the
 * four legs hold it in the same state for their whole six minutes, so only
 * act on a real change. -1 = unknown, forced on the first call of a run. */
static int s_radio_state = -1;
static int s_radio_cycles;

static bool ab_is_loud(void) { return (ab_block & 1) == 0; }

/* ------------------------------------------------------------------ *
 * THE CARD AS AN AGGRESSOR — the leg that had never been run
 *
 * 🔑 THE DISPLAY A/B CLEARED THE DISPLAY (+0.05 dB over ~180 samples each way)
 * AND THE CARD WAS WRITING THROUGH BOTH HALVES OF IT. Every measured session
 * on this handset has had the logger running, so the one thing never varied is
 * the one thing the map screen does hardest: 40 MHz 4-bit SDMMC, whose
 * harmonics land at 1560 and 1600 MHz either side of L1 at 1575.42.
 *
 * "Quiet" here is a full UNMOUNT, not merely an idle card — a mounted host
 * still has a peripheral and a clock tree. That is also why the session log
 * goes to PSRAM for the duration: there is nowhere to write for half the run.
 * ------------------------------------------------------------------ */

static volatile bool s_card_load;      /* hammer while true */
static volatile bool s_card_busy;      /* inside a read right now */
static volatile bool s_card_task_run;
static TaskHandle_t  s_card_task;
static uint32_t      s_card_kb;        /* read during the loud halves */
static int           s_card_unmounts;  /* quiet halves that really unmounted */
static bool          s_card_mounted = true;

static void card_load_task(void *arg)
{
    (void)arg;
    /* ⚠️ SELF-TERMINATING, AND THE ONLY EXIT IS ITS OWN FLAG. A waiter that
     * outlives the thing it was waiting for holds the card open past the
     * unmount and the unmount fails for no visible reason. */
    while (s_card_task_run) {
        if (!s_card_load) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
        s_card_busy = true;
        size_t n = s_card_load ? map_card_probe() : 0;
        s_card_busy = false;
        if (n) s_card_kb += n / 1024;
        else   vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_card_task = NULL;
    vTaskDelete(NULL);
}

static bool card_quiet(void)
{
    if (!s_card_mounted) return true;
    s_card_load = false;
    /* Let the read in flight finish: unmounting under an open FILE* is how a
     * "card removed" corruption story starts. */
    for (int i = 0; i < 100 && s_card_busy; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (s_card_busy) { applog("[field] card probe would not stop"); return false; }

    esp_err_t e = library_unmount();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        applog("[field] unmount FAILED: %s", esp_err_to_name(e));
        return false;
    }
    s_card_mounted = false;
    s_card_unmounts++;
    return true;
}

static bool card_loud(void)
{
    if (!s_card_mounted) {
        esp_err_t e = library_mount();
        if (e != ESP_OK) {
            applog("[field] remount FAILED: %s", esp_err_to_name(e));
            return false;
        }
        s_card_mounted = true;
    }
    s_card_load = true;
    return true;
}

/* ------------------------------------------------------------------ *
 * THE ONE-BUTTON SEQUENCE: display, then radio, then card
 *
 * 🔑 INTERLEAVED, NOT THREE TRIPS. Satellites move and so does whoever is
 * holding this, so a leg run at 10:00 and another at 10:20 differ by the sky as
 * much as by the condition. 30-second blocks alternating within each leg is
 * what makes the comparison mean anything — and it is why one walk outside
 * settles all three questions instead of three.
 *
 * ⚠️ THE METRIC IS cn0_4th AND decodable, NEVER used/fix. A receiver coasts on
 * ephemeris it already holds: the 5:55 AM screenshot showed a 3D fix on 7
 * satellites with `dec 0`, which is a receiver decoding NOTHING and still
 * reporting a position. `used` would have called that condition healthy.
 * ------------------------------------------------------------------ */

#define SEQ_LOG_BYTES (384 * 1024)     /* 4 legs x 12 blocks x 30s = 1,440 rows
                                        * at ~110 B = ~158 KB. The old 256 KB was
                                        * sized for THREE legs; a fourth leg would
                                        * have run it to 1.6x headroom. */

static bool     seq_run;
static int      seq_leg;
static float    seq_cn0[AB_LEGS][2];   /* [leg][0]=quiet [1]=loud */
static float    seq_dec[AB_LEGS][2];
static uint32_t seq_nn[AB_LEGS][2];
static bool     seq_done[AB_LEGS];
static bool     seq_sdlog_was;
/* 🔑 EVERY LEG PROVES ITS CONDITION CHANGED, OR ITS RESULT IS VOID. A "no
 * difference" number means the subsystem is innocent OR the toggle silently did
 * nothing, and the dB cannot tell those apart. Snapshotted at leg start and
 * leg end; a zero delta fails the leg out loud. */
static uint32_t seq_blk0[AB_LEGS], seq_blk1[AB_LEGS];
static int      seq_rad0[AB_LEGS], seq_rad1[AB_LEGS];

static bool seq_running(void) { return seq_run; }

static int seq_secs_left(void)
{
    if (!seq_run) return 0;
    int per  = (AB_BLOCKS * AB_BLOCK_MS) / 1000;
    int left = (AB_LEGS - 1 - seq_leg) * per;
    left += (AB_BLOCKS - ab_block) * (AB_BLOCK_MS / 1000);
    return left;
}

static const char *leg_name(ab_mode_t m)
{
    static const char *N[AB_LEGS] = { "total", "panel", "card", "radio" };
    return (m < AB_LEGS) ? N[m] : "?";
}

static void seq_teardown(void)
{
    s_card_load = false;
    s_card_task_run = false;            /* the task deletes itself */
    if (!s_card_mounted) {
        if (library_mount() == ESP_OK) s_card_mounted = true;
        else applog("[field] card did NOT come back - reboot to remount");
    }
    /* 🔑 THE PANEL COMES BACK FIRST. Everything after this point is only
     * visible if it does, and a test that ends with a dark unresponsive-looking
     * handset reads as a crash. */
    display_quiet(false);
    display_backlight(true);
    s_radio_state = -1;
    net_retry_reset();
    net_start_radio();
    s_sdlog_off = seq_sdlog_was;
}

static void seq_report(void)
{
    printf("\n===== GPS FIELD TEST =====\n");
    for (int L = 0; L < AB_LEGS; L++) {
        if (!seq_done[L]) { printf("%-8s NOT RUN\n", leg_name((ab_mode_t)L)); continue; }
        float dcn = seq_cn0[L][0] - seq_cn0[L][1];    /* quiet - loud */
        float ddec = seq_dec[L][0] - seq_dec[L][1];
        printf("%-8s cn0_4th loud %.2f quiet %.2f  delta %+.2f dB (n=%u/%u)\n"
               "         decodable loud %.2f quiet %.2f  delta %+.2f\n"
               "         %s\n",
               leg_name((ab_mode_t)L), seq_cn0[L][1], seq_cn0[L][0], dcn,
               (unsigned)seq_nn[L][1], (unsigned)seq_nn[L][0],
               seq_dec[L][1], seq_dec[L][0], ddec,
               dcn >= 1.0f ? "*** THIS IS COSTING SIGNAL ***"
                           : "not the limit");
        applog("[field] %s %+.2f dB, dec %+.2f", leg_name((ab_mode_t)L), dcn, ddec);
    }

    /* 🔑 THE CARD LEG HAS TO PROVE IT DID SOMETHING. A "no difference" result
     * means one of two completely opposite things - the card is innocent, or
     * the probe read nothing and the unmount never happened - and the numbers
     * alone cannot tell them apart. So the run reports the two facts that
     * separate them, and calls its own result VOID when they are missing.
     * (The builder has been bitten by a check that could only ever agree with me.) */
    if (seq_done[AB_SDCARD] || seq_done[AB_TOTAL]) {
        printf("card:  %u KB read loud, %d quiet blocks unmounted (want >= %d)\n",
               (unsigned)s_card_kb, s_card_unmounts, AB_BLOCKS / 2);
        if (!s_card_kb || s_card_unmounts < AB_BLOCKS / 2)
            printf("*** CARD CONDITION VOID - it never actually changed ***\n");
    }
    /* The panel leg's proof is flushes DROPPED. If the bus was never silenced
     * this is zero, and a zero here means the same mistake as the first four
     * runs — a backlight measured instead of a bus. */
    for (int L = 0; L < AB_LEGS; L++) {
        if (!seq_done[L]) continue;
        if (L == AB_PANEL || L == AB_TOTAL) {
            uint32_t d = seq_blk1[L] - seq_blk0[L];
            printf("%-6s flushes dropped while quiet: %u\n", leg_name((ab_mode_t)L), (unsigned)d);
            if (!d) printf("*** %s LEG VOID - the SPI bus was never silenced ***\n",
                           leg_name((ab_mode_t)L));
        }
        if (L == AB_RADIO || L == AB_TOTAL) {
            int d = seq_rad1[L] - seq_rad0[L];
            printf("%-6s radio transitions: %d (want >= %d)\n",
                   leg_name((ab_mode_t)L), d, AB_BLOCKS - 1);
            if (d < AB_BLOCKS - 1)
                printf("*** %s LEG VOID - the radio did not cycle ***\n",
                       leg_name((ab_mode_t)L));
        }
    }
    if (seq_done[AB_TOTAL]) {
        float t = seq_cn0[AB_TOTAL][0] - seq_cn0[AB_TOTAL][1];
        printf("\nTOTAL is the headline: the whole awake handset costs %+.2f dB\n"
               "and %+.2f decodable satellites. The other three legs say how it\n"
               "splits; they cannot add up to more than this.\n",
               t, seq_dec[AB_TOTAL][0] - seq_dec[AB_TOTAL][1]);
        printf("decode needs cn0_4th >= 32 sustained; quiet reached %.2f\n",
               seq_cn0[AB_TOTAL][0]);
    }
    printf("==========================\n\n");
}

static void seq_abort(const char *why)
{
    applog("[field] ABORTED: %s", why);
    ab_run  = false;
    seq_run = false;
    seq_teardown();
    seq_report();
    gpslog_ram_commit();
    gpslog_set_display_state("on");
}

/* Put the handset into this leg's loud or quiet condition. False means the
 * condition could not be established, and a leg whose condition did not
 * actually change is not evidence — so that aborts rather than carrying on. */
static void ab_set_radio(bool up)
{
    if (s_radio_state == (int)up) return;
    if (up) { net_retry_reset(); net_start_radio(); }
    else      net_down();
    s_radio_state = up;
    s_radio_cycles++;
}

static bool ab_apply(bool loud)
{
    /* Every leg starts from the quiet baseline and adds back only its own
     * subsystem. TOTAL adds all three at once. */
    bool panel = false, card = false, radio = false;

    switch (ab_mode) {
    case AB_TOTAL:  panel = card = radio = loud; break;
    case AB_PANEL:  panel = loud; break;
    case AB_SDCARD: card  = loud; break;
    case AB_RADIO:  radio = loud; break;
    default: return false;
    }

    /* ⚠️ Order matters on the way INTO quiet: stop the bus before unmounting,
     * so a redraw cannot be sitting on the SPI bus while the card leaves it. */
    display_quiet(!panel);
    if (!(card ? card_loud() : card_quiet())) return false;
    ab_set_radio(radio);
    return true;
}

static void ab_finish(void)
{
    ab_run = false;

    seq_cn0[ab_mode][1] = ab_n[1] ? (float)ab_cn0[1] / ab_n[1] : 0;
    seq_cn0[ab_mode][0] = ab_n[0] ? (float)ab_cn0[0] / ab_n[0] : 0;
    seq_dec[ab_mode][1] = ab_n[1] ? (float)ab_dec[1] / ab_n[1] : 0;
    seq_dec[ab_mode][0] = ab_n[0] ? (float)ab_dec[0] / ab_n[0] : 0;
    seq_nn[ab_mode][1]  = ab_n[1];
    seq_nn[ab_mode][0]  = ab_n[0];
    seq_blk1[ab_mode]   = display_flushes_blocked();
    seq_rad1[ab_mode]   = s_radio_cycles;
    seq_done[ab_mode]   = true;

    applog("[field] %s done: cn0_4th %+.2f dB", leg_name(ab_mode),
           seq_cn0[ab_mode][0] - seq_cn0[ab_mode][1]);

    if (seq_run && seq_leg + 1 < AB_LEGS) {
        seq_leg++;
        ab_mode = (ab_mode_t)seq_leg;
        ab_start();
        return;
    }

    seq_run = false;
    seq_teardown();
    seq_report();
    if (gpslog_ram_active()) {
        size_t drop = gpslog_ram_dropped();
        gpslog_ram_commit();
        if (drop) applog("[field] ⚠️ %u log rows dropped", (unsigned)drop);
    }
    gpslog_set_display_state("on");
}

static void ab_tick(lv_timer_t *t)
{
    (void)t;
    if (!ab_run) return;

    gps_state_t g;
    gps_snapshot(&g);
    int i = ab_is_loud() ? 1 : 0;
    ab_cn0[i] += g.cn0_4th;
    ab_dec[i] += g.sats_decodable;
    ab_n[i]++;

    /* "Loud" means the SPI bus as busy as this firmware ever makes it, not
     * merely a lit backlight - the clock is the suspect, not the LED. */
    if ((ab_mode == AB_PANEL || ab_mode == AB_TOTAL) && i && lv_scr_act())
        lv_obj_invalidate(lv_scr_act());

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((int32_t)(now - ab_next_ms) < 0) return;

    if (++ab_block >= AB_BLOCKS) { ab_finish(); return; }
    ab_next_ms = now + AB_BLOCK_MS;

    bool loud = ab_is_loud();
    if (!ab_apply(loud)) { seq_abort("could not set the condition"); return; }

    char tag[12];
    snprintf(tag, sizeof(tag), "%.4s-%s", leg_name(ab_mode),
             loud ? "loud" : "quiet");
    gpslog_set_display_state(tag);
    applog("[field] %s block %d/%d %s", leg_name(ab_mode), ab_block + 1,
           AB_BLOCKS, loud ? "loud" : "quiet");
}

/* ------------------------------------------------------------------ *
 * Starting the sequence.
 *
 * 🚨 EVERYTHING THAT READS THE CARD MUST STOP FIRST. The card leg unmounts,
 * and a player streaming MP3 off the same bus turns that into a failed unmount
 * at best. usbmsc.c does exactly this before it hands the card to a PC.
 * ------------------------------------------------------------------ */
static void seq_begin(void)
{
    if (seq_run || ab_run) return;

    if (usbmsc_active()) { applog("[field] not while the card is on the PC"); return; }

    player_suspend();

    memset(seq_cn0,  0, sizeof(seq_cn0));
    memset(seq_dec,  0, sizeof(seq_dec));
    memset(seq_nn,   0, sizeof(seq_nn));
    memset(seq_done, 0, sizeof(seq_done));
    memset(seq_blk0, 0, sizeof(seq_blk0));
    memset(seq_blk1, 0, sizeof(seq_blk1));
    memset(seq_rad0, 0, sizeof(seq_rad0));
    memset(seq_rad1, 0, sizeof(seq_rad1));
    s_radio_cycles = 0;
    s_radio_state = -1;          /* force the first ab_apply() to act */
    s_card_kb = 0;
    s_card_unmounts = 0;
    s_card_mounted = true;
    map_card_probe_reset();

    /* The receiver has to be running and ticking the logger, and the GPS
     * screen is what starts both. ⚠️ Suppress its card-backed log on the way
     * in: this run buffers to PSRAM instead, and letting gpslog_start() open a
     * file first would leave a header-only stub on the card every single run. */
    seq_sdlog_was = s_sdlog_off;
    s_sdlog_off = true;
    ui_show_gps();

    if (!gpslog_ram_begin(SEQ_LOG_BYTES)) {
        applog("[field] no PSRAM for the log - running without one");
    }

    s_card_task_run = true;
    if (!s_card_task &&
        xTaskCreate(card_load_task, "cardload", 4096, NULL, 3, &s_card_task)
            != pdPASS) {
        s_card_task_run = false;
        applog("[field] could not start the card probe - card leg will be void");
    }

    seq_run = true;
    seq_leg = 0;
    ab_mode = AB_TOTAL;
    applog("[field] START: %d legs x %d min, metric is cn0_4th and decodable",
           AB_LEGS, (AB_BLOCKS * AB_BLOCK_MS) / 60000);
    ab_start();
}

static void ab_start(void)
{
    /* ab_mode is set by the caller before this runs. */
    memset(ab_cn0, 0, sizeof(ab_cn0));
    memset(ab_dec, 0, sizeof(ab_dec));
    memset(ab_n,   0, sizeof(ab_n));
    seq_blk0[ab_mode] = display_flushes_blocked();
    seq_rad0[ab_mode] = s_radio_cycles;
    ab_block = 0;
    ab_run   = true;
    ab_next_ms = (uint32_t)(esp_timer_get_time() / 1000) + AB_BLOCK_MS;

    if (!ab_apply(true)) { seq_abort("could not start the first block"); return; }

    char tag[12];
    snprintf(tag, sizeof(tag), "%.4s-loud", leg_name(ab_mode));
    gpslog_set_display_state(tag);
    applog("[field] leg %d/%d %s: %d blocks of %ds, loud first",
           ab_mode + 1, AB_LEGS, leg_name(ab_mode), AB_BLOCKS,
           AB_BLOCK_MS / 1000);
}

static void gps_back_clicked(lv_event_t *e)
{
    (void)e;
    /* ⚠️ 25-50mA while tracking, backlight-class. The receiver goes down with
     * the screen, the same discipline the radio follows. */
    gps_stop();
    gpslog_stop();
    espnow_beacon_quiet(false);     /* presence resumes with the screen */
    /* 🔑 BACK TO THE MENU, NOT HOME. The builder, 2026-09-13: these screens are reached
     * from Settings and are used in a loop with the rows around them — hush the
     * radio, open GPS, come back, toggle the log, open it again. Dropping to the
     * home hub every time meant re-opening Settings and scrolling back down the
     * list for every single step of a test. */
    ui_show_menu();
}

static lv_obj_t *mk_ring(lv_obj_t *parent, int r)
{
    lv_obj_t *o = lv_obj_create(parent);
    flat(o);
    lv_obj_set_size(o, r * 2, r * 2);
    lv_obj_align(o, LV_ALIGN_TOP_LEFT, SKY_CX - r, SKY_CY - r);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, C_LINE, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void build_gps_screen(void)
{
    scr_gps = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_gps, C_BG, 0);
    lv_obj_clear_flag(scr_gps, LV_OBJ_FLAG_SCROLLABLE);
    mk_statusbar(scr_gps, &gps_title, &gps_batt, "GPS");

    /* horizon, 60 deg, 30 deg. Centre is the zenith. */
    mk_ring(scr_gps, SKY_R);
    mk_ring(scr_gps, SKY_R * 2 / 3);
    mk_ring(scr_gps, SKY_R / 3);

    lv_obj_t *n = mk_label(scr_gps, &lv_font_montserrat_12, C_MUTED, "N");
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, SKY_CX - 4, SKY_CY - SKY_R - 14);

    for (int i = 0; i < GPS_MAX_SATS; i++) {
        lv_obj_t *d = lv_obj_create(scr_gps);
        flat(d);
        lv_obj_set_size(d, 7, 7);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 1, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        gps_dot[i] = d;
    }

    int y = SKY_CY + SKY_R + 6;
    gps_l1 = mk_label(scr_gps, &lv_font_montserrat_14, C_INK,   "starting...");
    lv_obj_align(gps_l1, LV_ALIGN_TOP_LEFT, 8, y);
    gps_l2 = mk_label(scr_gps, &lv_font_montserrat_14, C_GREEN, "");
    lv_obj_align(gps_l2, LV_ALIGN_TOP_LEFT, 8, y + 17);
    gps_l3 = mk_label(scr_gps, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_align(gps_l3, LV_ALIGN_TOP_LEFT, 8, y + 35);
    gps_l4 = mk_label(scr_gps, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_align(gps_l4, LV_ALIGN_TOP_LEFT, 8, y + 50);

    /* 🔑 MARK is deliberately the BIGGEST target on this screen. It is pressed
     * outdoors, one-handed, while holding the handset in the exact position
     * being measured — which is the whole point, so it must not need aiming. */
    lv_obj_t *mark = lv_btn_create(scr_gps);
    flat(mark);
    lv_obj_set_size(mark, 120, 40);
    lv_obj_align(mark, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_obj_set_style_bg_color(mark, C_AMBER, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(mark, 4, 0);
    lv_obj_add_event_cb(mark, gps_mark_clicked, LV_EVENT_CLICKED, NULL);
    gps_mark_lbl = mk_label(mark, &lv_font_montserrat_14, C_ONAMBER, "MARK");
    lv_obj_center(gps_mark_lbl);

    lv_obj_t *back = lv_btn_create(scr_gps);
    flat(back);
    lv_obj_set_size(back, 90, 32);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
    lv_obj_set_style_bg_color(back, C_PANEL, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 4, 0);
    lv_obj_add_event_cb(back, gps_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(back, &lv_font_montserrat_14, C_BLUE, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
}

static void gps_refresh(void)
{
    gps_state_t g;
    gps_snapshot(&g);
    char buf[72];

    gps_log_1hz();                 /* the 1 Hz gate lives there now */

    int left = gpslog_window_left();
    if (gps_mark_lbl) {
        if (left) { snprintf(buf, sizeof(buf), "%ds", left);
                    lv_label_set_text(gps_mark_lbl, buf); }
        else        lv_label_set_text(gps_mark_lbl, "MARK");
    }

    for (int i = 0; i < GPS_MAX_SATS; i++) {
        if (i >= g.sat_count || g.sats[i].elevation > 90) {
            lv_obj_add_flag(gps_dot[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const gps_sat_t *s = &g.sats[i];

        /* Zenith at the centre, horizon at the rim. */
        float rr  = (90.0f - (float)s->elevation) / 90.0f * SKY_R;
        float rad = (float)s->azimuth * (float)M_PI / 180.0f;
        int   x   = SKY_CX + (int)(rr * sinf(rad));
        int   y   = SKY_CY - (int)(rr * cosf(rad));

        lv_color_t c = s->cn0 >= 35 ? C_GREEN : s->cn0 >= 20 ? C_AMBER : C_MUTED;
        lv_obj_set_style_border_color(gps_dot[i], c, 0);
        lv_obj_set_style_bg_color(gps_dot[i], c, 0);
        /* Filled = actually in the position solution, hollow = merely visible.
         * A sky full of hollow dots is the signature of a bad placement. */
        lv_obj_set_style_bg_opa(gps_dot[i], s->used ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        /* 🔑 BeiDou is drawn as a square (radius 1) and GPS as a circle, so the
         * two constellations stay distinguishable without a legend. */
        lv_obj_set_style_radius(gps_dot[i],
            s->constellation == GPS_CONST_BEIDOU ? 1 : LV_RADIUS_CIRCLE, 0);
        lv_obj_align(gps_dot[i], LV_ALIGN_TOP_LEFT, x - 3, y - 3);
        lv_obj_clear_flag(gps_dot[i], LV_OBJ_FLAG_HIDDEN);
    }

    if (!g.sentences) {
        /* ⚠️ Sentences arrive with EMPTY fields long before any fix, so silence
         * here is a wiring or power fault and never "no sky". Say which. */
        lv_label_set_text(gps_l1, "no data on GPIO43");
        lv_obj_set_style_text_color(gps_l1, C_RED, 0);
        lv_label_set_text(gps_l2, "");
        lv_label_set_text(gps_l3, "check power and the TX wire");
        lv_label_set_text(gps_l4, "");
        return;
    }

    snprintf(buf, sizeof(buf), "%u trk  %u used%s", g.sats_tracked, g.sats_used,
             espnow_beacon_quiet_get() ? "  [quiet]" : "");
    lv_label_set_text(gps_l1, buf);
    lv_obj_set_style_text_color(gps_l1, C_INK, 0);

    /* 🔑 THE LINE THAT SAYS WHETHER A FIX IS POSSIBLE.
     *
     * A fix needs ephemeris from FOUR satellites, each decoded from 18 seconds
     * of clean 50 bps data needing ~30-32 dB-Hz on THAT satellite. So the
     * question is never "is the signal good" — it is "are there four decodable
     * satellites". `dec` counts them; `4th` is the weakest of the top four and
     * is the one that has to clear the bar.
     *
     * 🚨 The old line showed cn0_best4, an AVERAGE, which hid exactly this:
     * 40/38/28/26 averages to 33 and reads healthy while only two satellites
     * can actually be decoded. Green here means dec >= 4 and a fix should
     * follow; amber means it never will, however long you wait. */
    snprintf(buf, sizeof(buf), "dec %u  4th %u dB  pk %u",
             g.sats_decodable, g.cn0_4th, g.cn0_max);
    lv_label_set_text(gps_l2, buf);
    lv_obj_set_style_text_color(gps_l2,
        g.sats_decodable >= 4 ? C_GREEN : g.sats_decodable >= 2 ? C_AMBER : C_RED, 0);

    if (g.quality) {
        snprintf(buf, sizeof(buf), "%dD fix  HDOP %.1f  TTFF %us",
                 g.fix_type, g.hdop, (unsigned)(g.ttff_ms / 1000));
    } else {
        snprintf(buf, sizeof(buf), "no fix  %u sentences", (unsigned)g.sentences);
    }
    lv_label_set_text(gps_l3, buf);

    if (g.quality) {
        snprintf(buf, sizeof(buf), "%.5f, %.5f", g.lat, g.lon);
    } else if (!g.antenna_ok) {
        snprintf(buf, sizeof(buf), "ANTENNA FAULT");
    } else {
        snprintf(buf, sizeof(buf), "antenna ok%s",
                 g.bad_checksums ? "  (errors!)" : "");
    }
    /* A finished MARK result outranks the coordinates: it is the number the
     * walk was done to collect, and it is only on screen until the next one. */
    if (gpslog_last_result()[0]) {
        lv_label_set_text(gps_l4, gpslog_last_result());
        lv_obj_set_style_text_color(gps_l4, C_AMBER, 0);
    } else {
        lv_label_set_text(gps_l4, buf);
        lv_obj_set_style_text_color(gps_l4, g.antenna_ok ? C_MUTED : C_RED, 0);
    }
}

void ui_show_gps(void)
{
    /* 🚨 STAND THE 1 Hz PRESENCE BEACON DOWN WHILE ACQUIRING. A GPS navigation
     * subframe is 6 seconds and an ephemeris needs three consecutive clean ones;
     * a transmission every second lands inside every last one of them. Measured
     * 2026-09-12: 25 satellites at 38 dB for 218 unbroken seconds, and not one
     * ever reached "used". See the long note in espnow.c. */
    espnow_beacon_quiet(true);
    gps_start();
    if (!s_sdlog_off) {
        gpslog_start();
        /* ⚠️ This used to overwrite the tag the hush toggle had just set, so a
         * hushed run logged itself as "quiet" and the rows could not prove the
         * radio was down. Keep whatever the operator chose. */
        if (!s_radio_hushed) gpslog_set_display_state("quiet");
    }
    ui_load(scr_gps);
    gps_refresh();
}


/* ------------------------------------------------------------------ *
 * THE MAP — roads drawn from vectors off the SD card
 *
 * 🔑 THIS IS THE DEMO; THE SKY VIEW IS THE INSTRUMENT. A pin either appears or
 * it does not — one bit of information, minutes late. What makes this screen
 * worth having anyway is the TRAIL: it shows where the receiver dropped out
 * along a walk, which is the case-placement evidence the numbers cannot give.
 *
 * ⚠️ THE ATTRIBUTION IS NOT DECORATION. The tiles come from a Geofabrik OSM
 * extract, ODbL, and "© OpenStreetMap contributors" on the screen is the
 * licence condition that makes using it legitimate at all. Do not remove it to
 * win back 12 pixels.
 * ------------------------------------------------------------------ */

#define MAP_FOOT_H   52
#define MAP_CANVAS_H (LCD_H - BAR_H - MAP_FOOT_H)

static lv_obj_t   *map_title, *map_batt, *map_canvas;
static lv_obj_t   *map_l1, *map_follow_btn, *map_follow_lbl;
static lv_color_t *map_buf;
static bool        map_following = true;
static bool        map_first_fix;

/* Somewhere to look before there is a fix. Roughly the middle of the Lower
 * Peninsula: the screen shows real roads on the first open instead of a black
 * rectangle that is indistinguishable from a broken reader. */
#define MAP_HOME_LAT  44.35
#define MAP_HOME_LON -85.40

/* What the canvas currently shows, so an identical redraw can be skipped.
 * See map.h's map_generation(): a redraw is the longest single thing on the
 * LVGL task, and while it runs the indev timer cannot sample touch. */
static uint32_t map_drawn_gen;
static bool     map_drawn_fix;
static bool     map_force_redraw;

static void map_refresh(void)
{
    if (!map_canvas) return;

    gps_state_t g;
    gps_snapshot(&g);

    /* 🚨 `used` IS NOT EVIDENCE OF A POSITION. Measured on Parent after a
     * drive, 2026-09-19: the screen read `u6 d0 h17.4` — six satellites claimed
     * in the solution, **zero** of them decodable, and a geometry factor of
     * 17.4. A 3D fix needs FOUR satellites whose 50 bps navigation message is
     * actually being decoded (~32 dB-Hz held for 18 s each). At d0 the receiver
     * is not solving anything; it is propagating stale ephemeris, and the bias
     * in that drifts slowly — which is why the builder's track came back as a clean line
     * PARALLEL to the road he drove rather than as scatter. Noise scatters; a
     * biased solution translates.
     *
     * So two different questions, and they must not share one flag:
     *   have_pos — the receiver is emitting a position
     *   trusted  — there is a measurement underneath it
     *
     * 🔑 4 is arithmetic, not taste: four unknowns (x, y, z, clock) need four
     * decoded satellites. The HDOP backstop uses the standard DOP scale, where
     * >10 is rated poor/unusable — it is there to catch the case where four
     * satellites are decodable but sitting on top of each other in the sky. A
     * receiver that does not report HDOP is not punished for it. */
#define MAP_HDOP_MAX 10.0f
    bool have_pos = g.valid && g.quality > 0 && g.sats_used >= 3;
    bool trusted  = have_pos && g.sats_decodable >= 4 &&
                    (g.hdop <= 0.0f || g.hdop <= MAP_HDOP_MAX);

    if (trusted) {
        if (!map_first_fix) {
            map_first_fix = true;
            map_pin_set(g.lat, g.lon);
            applog("[map] first fix %.6f %.6f - pin dropped", g.lat, g.lon);
        }
        /* ⚠️ THE TRAIL IS A RECORD, AND AN UNTRUSTED POINT POISONS IT
         * PERMANENTLY. The builder's drive wrote a biased track into the trail that no
         * later good fix can remove. Only measured positions get recorded. */
        map_trail_add(g.lat, g.lon);
        /* Follow holds the last TRUSTED centre rather than chasing a solution
         * with nothing behind it — this is what stopped the map sliding across
         * the state while the handset sat still. */
        if (map_following) map_center_set(g.lat, g.lon);
    }

    gps_log_1hz();

    /* 🔑 REDRAW ONLY WHAT CHANGED. The status line below is cheap and always
     * updates — the satellite counts must keep moving or the screen looks dead
     * while acquiring. The CANVAS is the expensive part and is skipped when the
     * picture would be identical.
     *
     * The fix flag is part of the comparison because the position ring is drawn
     * solid or hollow from it; losing a fix changes the picture without moving
     * anything. */
    uint32_t gen = map_generation();
    if (map_force_redraw || gen != map_drawn_gen || trusted != map_drawn_fix) {
        map_force_redraw = false;
        map_drawn_gen = gen;
        map_drawn_fix = trusted;

        /* 🔑 THE RING IS DRAWN FROM `trusted`, NOT `have_pos`. map.c's own
         * comment says it is "hollow while there is no fix, so the screen never
         * implies it knows where it is when it does not" — that intent was
         * being violated, because the old flag was true at `u6 d0 h17.4` and
         * the ring was drawn SOLID over a position with no measurement in it. */
        map_draw(map_canvas, LCD_W, MAP_CANVAS_H, trusted);

        /* 🔎 A slow redraw is either card I/O or the feature walk, and the two
         * want opposite fixes. Print the split when it is slow enough to matter
         * — this is what decides whether a tile-loader worker task is worth
         * building, rather than assuming it is. */
        uint32_t tot, io; uint16_t miss;
        map_draw_stats(&tot, &io, &miss);
        if (tot > 30)
            applog("[map] redraw %ums (io %ums, %u misses, walk %ums)",
                   (unsigned)tot, (unsigned)io, (unsigned)miss,
                   (unsigned)(tot > io ? tot - io : 0));
    }

    /* 🚨 SHOW `dec` AND `hdop` WHILE IT CLAIMS A FIX — that is exactly when they
     * matter, and this line used to hide them precisely then.
     *
     * The builder, 2026-09-19: *"it says that i am using 8 to 10 satalites... it starts
     * out good on the right spot then it starts to drift... moving across the
     * map at brak neck speed without moving the divice at all and it still
     * showing using satalites"*.
     *
     * `used` counts satellites in the solution and says NOTHING about whether
     * their navigation data is still being decoded. A receiver that has lost
     * the signal keeps emitting a "valid" 3D fix from stored ephemeris and its
     * own motion model — and a motion model that has latched a false velocity
     * EXTRAPOLATES SMOOTHLY, which is why the marker glides in a straight line
     * at constant speed and occasionally snaps to a new heading. Noise
     * scatters; a filter glides. See the note "gps placement metric":
     * `used` > 0 with `dec 0` is coasting.
     *
     * So the fix branch now prints the two numbers that tell real from coasting:
     * `dec` (satellites individually >= 32 dB-Hz, four needed) and HDOP. A line
     * reading `used 9 dec 0 hdop 9.2` is the answer to the question above, and
     * it is readable at a glance without a cable. */
    char buf[80];
    if (have_pos) {
        /* Still prints the raw lat/lon the receiver gave, trusted or not — the
         * screen must never become the only place the truth is hidden. `HOLD`
         * says the map is deliberately not following this one. */
        snprintf(buf, sizeof(buf), "%.5f %.5f u%u d%u h%.1f z%d%s",
                 g.lat, g.lon, (unsigned)g.sats_used,
                 (unsigned)g.sats_decodable, g.hdop, map_zoom(),
                 trusted ? "" : " HOLD");
    } else {
        snprintf(buf, sizeof(buf), "no fix  tracked %u  dec %u  z%d",
                 (unsigned)g.sats_tracked, (unsigned)g.sats_decodable, map_zoom());
    }
    if (map_l1) lv_label_set_text(map_l1, buf);
}

static void map_zoom_in_clicked(lv_event_t *e)
{ (void)e; map_zoom_set(map_zoom() + 1); map_refresh(); }
static void map_zoom_out_clicked(lv_event_t *e)
{ (void)e; map_zoom_set(map_zoom() - 1); map_refresh(); }

/* Panning turns FOLLOWING off, because the two fight: a map that re-centres a
 * second after you drag it cannot be looked at. */
static void map_follow_clicked(lv_event_t *e)
{
    (void)e;
    map_following = !map_following;
    if (map_follow_lbl)
        lv_label_set_text(map_follow_lbl, map_following ? "Follow" : "Free");
    map_refresh();
}

static void map_back_clicked(lv_event_t *e) { (void)e; ui_show_menu(); }

/* 🔑 DRAG TO PAN — the control the first version forgot.
 *
 * Without it the map can only ever look at where the receiver says you are, and
 * this receiver mostly does not know. The builder, 2026-09-13, looking at the default
 * centre: *"that may be lansing but i cant tell"* — he had no way to travel to
 * anywhere he recognised.
 *
 * ⚠️ Throttled to 100 ms. A redraw re-walks every feature in up to four tiles,
 * and LVGL delivers PRESSING at its refresh rate — redrawing on every one makes
 * the drag lag behind the finger instead of following it. */
/* 🚨 A TAP IS NOT A DRAG, AND ONE PIXEL USED TO BE ENOUGH.
 *
 * This handler used to act on ANY non-zero movement vector, and its first act
 * was to turn Follow off. A finger never lands perfectly still, so simply
 * TOUCHING the map — to look at something, to check where you were — silently
 * stopped it following. The builder, 2026-09-18: *"it does seem to hold location for a
 * while but after some time it starts to wander on the map screen ... but it
 * looks like it holds steady on the gps screen"*. That is the whole bug: the
 * position was never lost, the map just quietly stopped re-centring, and the
 * GPS screen looked fine because it has no drag handler at all.
 *
 * 🔑 So a drag must EARN the name: nothing happens until the finger has moved
 * DRAG_SLOP pixels from where it landed. Below that it is a tap and the map
 * keeps following. The accumulator is reset on PRESSED, not on RELEASED — a
 * release that never came (finger slid onto the footer) must not leave the next
 * touch pre-charged.
 *
 * ⚠️ The slop is measured from the PRESS POINT, not summed per-event. Summing
 * |v| makes a slow circular wobble cross any threshold eventually, which is
 * exactly the jitter this is meant to ignore. */
#define DRAG_SLOP 8

static lv_point_t map_press_pt;
static bool       map_dragging;

static void map_drag(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *id = lv_indev_get_act();
        if (id) lv_indev_get_point(id, &map_press_pt);
        map_dragging = false;
        return;
    }

    /* The throttle can swallow the last few pixels of a drag, so the release
     * always gets a redraw — otherwise the map settles a fingerwidth away from
     * where it was let go. Only for a real drag: a tap has moved nothing and a
     * redraw on every tap is a needless four-tile re-walk. */
    if (code == LV_EVENT_RELEASED) {
        if (map_dragging) map_refresh();
        map_dragging = false;
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t v;
    lv_indev_get_vect(indev, &v);
    if (!v.x && !v.y) return;

    if (!map_dragging) {
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        int32_t dx = p.x - map_press_pt.x, dy = p.y - map_press_pt.y;
        if (dx * dx + dy * dy < DRAG_SLOP * DRAG_SLOP) return;   /* still a tap */
        map_dragging = true;
    }

    /* Dragging and following fight each other: a map that re-centres a second
     * after you move it cannot be read. Moving it means you want it still. */
    if (map_following) {
        map_following = false;
        if (map_follow_lbl) lv_label_set_text(map_follow_lbl, "Free");
    }
    map_pan_px(-v.x, -v.y);

    static uint32_t last;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if ((int32_t)(now - last) < 100) return;
    last = now;
    map_refresh();
}

static void build_map_screen(void)
{
    scr_map = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_map, C_BG, 0);
    lv_obj_clear_flag(scr_map, LV_OBJ_FLAG_SCROLLABLE);
    mk_statusbar(scr_map, &map_title, &map_batt, "Map");

    /* ⚠️ PSRAM, and it has to be. A 240x268 true-colour canvas is 128 KB —
     * internal RAM on this board is what WiFi and the audio path already
     * compete for, and lwIP allocating from an exhausted internal pool is what
     * turns an upload into 1 KB/s. */
    size_t need = LV_CANVAS_BUF_SIZE_TRUE_COLOR(LCD_W, MAP_CANVAS_H);
    map_buf = heap_caps_malloc(need, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (map_buf) {
        map_canvas = lv_canvas_create(scr_map);
        lv_canvas_set_buffer(map_canvas, map_buf, LCD_W, MAP_CANVAS_H,
                             LV_IMG_CF_TRUE_COLOR);
        lv_obj_align(map_canvas, LV_ALIGN_TOP_LEFT, 0, BAR_H);
        lv_obj_add_flag(map_canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(map_canvas, map_drag, LV_EVENT_PRESSED,  NULL);
        lv_obj_add_event_cb(map_canvas, map_drag, LV_EVENT_PRESSING, NULL);
        lv_obj_add_event_cb(map_canvas, map_drag, LV_EVENT_RELEASED, NULL);
        lv_canvas_fill_bg(map_canvas, lv_color_make(0x0D, 0x12, 0x1A), LV_OPA_COVER);
    } else {
        lv_obj_t *w = mk_label(scr_map, &lv_font_montserrat_14, C_AMBER,
                               "no PSRAM for the map");
        lv_obj_align(w, LV_ALIGN_CENTER, 0, 0);
    }

    /* The licence condition, over the bottom of the map itself. */
    lv_obj_t *attr = mk_label(scr_map, &lv_font_montserrat_12, C_MUTED,
                              "(c) OpenStreetMap contributors");
    lv_obj_align(attr, LV_ALIGN_TOP_LEFT, 6, BAR_H + MAP_CANVAS_H - 15);

    map_l1 = mk_label(scr_map, &lv_font_montserrat_12, C_INK, "starting...");
    lv_obj_align(map_l1, LV_ALIGN_TOP_LEFT, 6, BAR_H + MAP_CANVAS_H + 2);

    struct { const char *t; lv_event_cb_t cb; int x, w; } B[] = {
        { "-",      map_zoom_out_clicked, 4,   40 },
        { "+",      map_zoom_in_clicked,  48,  40 },
        { "Follow", map_follow_clicked,   92,  70 },
        { LV_SYMBOL_LEFT " Back", map_back_clicked, 166, 70 },
    };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_btn_create(scr_map);
        flat(b);
        lv_obj_set_size(b, B[i].w, 30);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, B[i].x, LCD_H - 34);
        lv_obj_set_style_bg_color(b, C_PANEL, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 4, 0);
        /* 🚨 RELEASED, NOT CLICKED — and on THIS screen it matters most.
         *
         * CLICKED needs the press AND the release to land on the same object.
         * These are 30 px tall next to a surface whose whole purpose is to
         * interpret finger movement as panning, so a release that drifts a few
         * pixels off the button is silently swallowed and the tap does nothing.
         * The builder, 2026-09-18: *"it seems to have a hard time with the back button
         * on that screen"*. Same defect as the games menu in August; see the
         * CLICKED-vs-RELEASED note. All four buttons, not just Back — they are
         * the same size on the same screen and share the fault. */
        lv_obj_add_event_cb(b, B[i].cb, LV_EVENT_RELEASED, NULL);
        lv_obj_t *l = mk_label(b, &lv_font_montserrat_14,
                               i == 3 ? C_BLUE : C_INK, B[i].t);
        lv_obj_center(l);
        if (i == 2) { map_follow_btn = b; map_follow_lbl = l; }
    }
}

void ui_show_map(void)
{
    /* Same three things the sky view does on the way in, and for the same
     * reasons: the beacon stands down while acquiring, the receiver has to be
     * running, and the session is worth logging. */
    espnow_beacon_quiet(true);
    gps_start();
    if (!s_sdlog_off && !gpslog_active()) gpslog_start();

    if (!map_available())
        applog("[map] %s - the screen will draw the walk but no roads",
               map_status());

    gps_state_t g;
    gps_snapshot(&g);
    if (g.valid && g.quality > 0) map_center_set(g.lat, g.lon);
    else                          map_center_set(MAP_HOME_LAT, MAP_HOME_LON);

    ui_load(scr_map);
    /* Entering the screen always paints once: the canvas may still hold the
     * view from the last visit, and "unchanged" is only meaningful against
     * something the user can currently see. */
    map_force_redraw = true;
    map_refresh();
}

/* ------------------------------------------------------------------ *
 * Fleet — who this handset shares keys with, and the fleet code
 *
 * 🔑 THE CODE IS TYPED ONCE PER HANDSET AND MUST BE IDENTICAL ON ALL OF THEM.
 * It seeds the ESP-NOW PMK, and a handset whose PMK differs cannot pass a
 * directed message to any other — measured on Parent and Kid 4,
 * 2026-09-06. That is why this screen exists at all rather than a key baked
 * into the firmware: the firmware is published, so a key in it is public.
 *
 * ⚠️ Entering a DIFFERENT code later re-keys this handset away from the rest of
 * the fleet. The screen says so before it accepts one.
 * ------------------------------------------------------------------ */

static lv_obj_t *fl_title, *fl_batt, *fl_stat, *fl_rows;
static lv_obj_t *scr_code,  *cd_title, *cd_batt, *cd_entry, *cd_hint;
static char      cd_buf[FLEET_CODE_LEN + 1];

static void fl_back_clicked(lv_event_t *e)  { (void)e; ui_show_menu(); }

/* ⚠️ Mode-taking, so it must never be reachable by accident and must say what
 * it costs. Migration is what lets an encrypted handset still talk to one that
 * has not been flashed yet — useful for days, dangerous left on for ever.
 *
 * 🚨 THE COMMENT ABOVE SAID ALL THAT AND THE CODE DID NONE OF IT. This was a
 * plain button that flipped the fleet's downgrade path instantly, both ways,
 * with no confirmation and nothing to announce it but a colour change on a
 * screen nobody is looking at. A stray tap silently RE-OPENED cleartext for the
 * whole handset. Same shape as the stray serial byte that started a
 * calibration, and the same rule applies: a mode-taking action needs a
 * confirmation AND a self-recovery timeout.
 *
 * 🔑 THE TWO DIRECTIONS ARE NOT THE SAME RISK, so they are not treated the
 * same. CLOSING is the safe direction — it can only ever tighten things, and
 * making it hard would discourage the one action we actually want people to
 * take at the end of a migration. OPENING is the dangerous one, so that is the
 * side that has to be held for, and it says what it costs first. */
#define MIG_ASK_TIMEOUT_MS  12000

static lv_obj_t   *mig_ask_box;
static lv_timer_t *mig_ask_timer;
static int         mig_yes_ticks;

static void mig_ask_close(void)
{
    if (mig_ask_timer) { lv_timer_del(mig_ask_timer); mig_ask_timer = NULL; }
    if (mig_ask_box)   { lv_obj_del(mig_ask_box);     mig_ask_box   = NULL; }
    mig_yes_ticks = 0;
}
/* A question raised by accident must go away by itself — a handset in a bag
 * taps itself, and the default has to be the safe answer. */
static void mig_ask_timeout(lv_timer_t *t)
{
    (void)t;
    mig_ask_timer = NULL;          /* one-shot: LVGL is deleting it itself */
    applog("[fleet] migration question timed out - stayed closed");
    mig_ask_close();
}
static void mig_ask_no(lv_event_t *e) { (void)e; mig_ask_close(); }
static void mig_ask_yes(lv_event_t *e)
{
    (void)e;
    if (++mig_yes_ticks < 9) return;          /* ~1.3s of holding */
    mig_ask_close();
    fleet_migration_set(true);
    fleet_refresh();
}
static void mig_ask_yes_up(lv_event_t *e) { (void)e; mig_yes_ticks = 0; }

static void mig_ask_open(void)
{
    mig_ask_close();
    mig_yes_ticks = 0;
    mig_ask_box = lv_obj_create(lv_layer_top());   /* over whatever asked */
    lv_obj_set_size(mig_ask_box, LCD_W - 40, 160);
    lv_obj_center(mig_ask_box);
    lv_obj_set_style_bg_color(mig_ask_box, C_PANEL, 0);
    lv_obj_set_style_border_color(mig_ask_box, C_AMBER, 0);
    lv_obj_set_style_border_width(mig_ask_box, 2, 0);
    lv_obj_clear_flag(mig_ask_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = mk_label(mig_ask_box, &lv_font_montserrat_14, C_INK,
                           "Allow unpaired\nhandsets to talk\nin the CLEAR?\n"
                           "Hold Yes");
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *no = lv_btn_create(mig_ask_box);
    lv_obj_set_size(no, 76, 40);
    lv_obj_align(no, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(no, C_LINE, 0);
    lv_obj_add_event_cb(no, mig_ask_no, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(no, &lv_font_montserrat_14, C_INK, "No"));

    lv_obj_t *yes = lv_btn_create(mig_ask_box);
    lv_obj_set_size(yes, 76, 40);
    lv_obj_align(yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(yes, C_AMBER, 0);
    lv_obj_add_event_cb(yes, mig_ask_yes,    LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_add_event_cb(yes, mig_ask_yes_up, LV_EVENT_RELEASED, NULL);
    lv_obj_center(mk_label(yes, &lv_font_montserrat_14, C_ONAMBER, "Yes"));

    mig_ask_timer = lv_timer_create(mig_ask_timeout, MIG_ASK_TIMEOUT_MS, NULL);
    lv_timer_set_repeat_count(mig_ask_timer, 1);
}

static void fl_migrate_clicked(lv_event_t *e)
{
    (void)e;
    if (fleet_migration_open()) {
        fleet_migration_set(false);      /* tightening: no ceremony required */
        fleet_refresh();
        return;
    }
    mig_ask_open();
}

static void cd_refresh(void)
{
    char shown[FLEET_CODE_LEN + 1];
    size_t n = strlen(cd_buf);
    for (size_t i = 0; i < FLEET_CODE_LEN; i++) shown[i] = i < n ? cd_buf[i] : '-';
    shown[FLEET_CODE_LEN] = 0;
    lv_label_set_text(cd_entry, shown);
    /* 🚨 SAY IT BEFORE OK, NOT AFTER. The code is run through PBKDF2 and only
     * the derived key is stored — it is NOT recoverable from the handset, by
     * design, or anyone holding one could read the fleet key off it. The builder had
     * already forgotten the first code he set within the hour. */
    lv_label_set_text(cd_hint,
        n < FLEET_CODE_LEN ? "same code on every handset"
                           : "OK - write it down first, it cannot be shown again");
}

static void cd_key_clicked(lv_event_t *e)
{
    const char *d = (const char *)lv_event_get_user_data(e);
    size_t n = strlen(cd_buf);
    if (d[0] == '<') {                      /* backspace */
        if (n) cd_buf[n - 1] = 0;
    } else if (d[0] == 'K') {               /* OK */
        if (n == FLEET_CODE_LEN) {
            /* 🚨 SAY SOMETHING BEFORE BLOCKING THE UI THREAD.
             *
             * fleet_set_code() runs 100,000 PBKDF2 rounds on this thread, so
             * LVGL cannot repaint until it returns and OK looks like it did
             * nothing at all — the builder's first reaction was "it took a while". The
             * work is deliberate (it is what makes an 8-digit code expensive to
             * guess offline), so the fix is to SHOW it, not to shorten it.
             * lv_refr_now() forces the frame out before we stall. */
            lv_label_set_text(cd_hint, "working...");
            lv_obj_set_style_text_color(cd_hint, C_AMBER, 0);
            lv_refr_now(NULL);

            fleet_set_code(cd_buf);
            lv_obj_set_style_text_color(cd_hint, C_MUTED, 0);
            memset(cd_buf, 0, sizeof(cd_buf));   /* never leave it on screen */
            ui_show_fleet();
            return;
        }
    } else if (n < FLEET_CODE_LEN) {
        cd_buf[n] = d[0];
        cd_buf[n + 1] = 0;
    }
    cd_refresh();
}

static void cd_back_clicked(lv_event_t *e)
{
    (void)e;
    memset(cd_buf, 0, sizeof(cd_buf));
    ui_show_fleet();
}

static void fl_code_clicked(lv_event_t *e)
{
    (void)e;
    memset(cd_buf, 0, sizeof(cd_buf));
    cd_refresh();
    ui_load(scr_code);
}

/* Tapping a row. An unpaired handset in range pairs; a paired one is removed.
 *
 * ⚠️ Remove is not a nicety — it is the ONLY recovery when a pair offer is lost
 * and the two ends disagree about whether they share a key. See espnow.h. */
static void fl_row_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0) {                       /* an in-range peer, not yet paired */
        if (!fleet_has_pmk()) return;     /* nothing to derive a key from */
        espnow_pair_with(idx);
    } else {                              /* -(slot+1) => a roster entry */
        const fleet_peer_t *p = fleet_at(-idx - 1);
        if (p) fleet_remove(p->mac);
    }
    fleet_refresh();
}

static void fleet_refresh(void)
{
    if (!scr_fleet) return;
    char line[96];

    if (!fleet_has_pmk())
        snprintf(line, sizeof(line), "no fleet code yet - nothing is encrypted");
    else if (fleet_migration_open())
        snprintf(line, sizeof(line), "%d paired - MIGRATION OPEN",
                 fleet_count());
    else
        snprintf(line, sizeof(line), "%d paired - closed", fleet_count());
    lv_label_set_text(fl_stat, line);
    lv_obj_set_style_text_color(fl_stat,
        !fleet_has_pmk()        ? C_MUTED :
        fleet_migration_open()  ? C_AMBER : C_GREEN, 0);

    lv_obj_clean(fl_rows);
    int shown = 0;

    /* Paired first — these are the fleet. Tap to remove. */
    for (int i = 0; i < FLEET_MAX; i++) {
        const fleet_peer_t *p = fleet_at(i);
        if (!p) continue;
        lv_obj_t *row = lv_btn_create(fl_rows);
        flat(row);
        lv_obj_set_size(row, LCD_W - 16, 34);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 8, shown * 38);
        lv_obj_set_style_bg_color(row, C_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(row, fl_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(-i - 1));
        char t[64];
        snprintf(t, sizeof(t), "%u  %s  %s", p->device_no, p->name,
                 p->seen_encrypted ? LV_SYMBOL_OK " locked" : "clear");
        lv_obj_t *l = mk_label(row, &lv_font_montserrat_12,
                               p->seen_encrypted ? C_GREEN : C_AMBER, t);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        shown++;
    }

    /* Then whoever else is in range and not yet paired. Tap to pair. */
    int n = espnow_peer_count();
    for (int i = 0; i < n; i++) {
        uint16_t id = espnow_peer_id(i);
        bool already = false;
        for (int j = 0; j < FLEET_MAX; j++) {
            const fleet_peer_t *p = fleet_at(j);
            if (p && p->id == id) { already = true; break; }
        }
        if (already) continue;
        lv_obj_t *row = lv_btn_create(fl_rows);
        flat(row);
        lv_obj_set_size(row, LCD_W - 16, 34);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 8, shown * 38);
        lv_obj_set_style_bg_color(row, C_PANEL, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_event_cb(row, fl_row_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        char t[64];
        snprintf(t, sizeof(t), "%s   %s", espnow_peer_name(i),
                 fleet_has_pmk() ? "tap to pair" : "set a code first");
        lv_obj_t *l = mk_label(row, &lv_font_montserrat_12,
                               fleet_has_pmk() ? C_BLUE : C_MUTED, t);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 8, 0);
        shown++;
    }

    if (!shown) {
        lv_obj_t *l = mk_label(fl_rows, &lv_font_montserrat_12, C_MUTED,
                               "nothing in range");
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 8, 4);
    }

    /* 🔑 SCROLLABLE ONLY WHEN THERE IS SOMETHING TO SCROLL. A scrollable parent
     * converts a slightly-dragged press into a scroll and swallows the child's
     * CLICKED (see the scroll_limit note in display.c). Raising the threshold
     * makes that rare; removing the scroll when every row already fits makes it
     * IMPOSSIBLE, which is what the Fleet screen deserves — remove is the only
     * recovery when two ends disagree about a key.
     * ⚠️ Must stay dynamic: a long roster still has to be reachable, which is
     * the trap the flat() note above this container warns about. */
    if (shown * 38 > lv_obj_get_height(fl_rows))
        lv_obj_add_flag(fl_rows, LV_OBJ_FLAG_SCROLLABLE);
    else
        lv_obj_clear_flag(fl_rows, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *fl_button(lv_obj_t *par, int w, int x, lv_color_t col,
                           const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(par);
    flat(b);
    lv_obj_set_size(b, w, 32);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, x, -10);
    lv_obj_set_style_bg_color(b, C_PANEL, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_style_bg_color(b, C_LINE, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = mk_label(b, &lv_font_montserrat_12, col, text);
    lv_obj_center(l);
    return b;
}

static void build_fleet_screen(void)
{
    scr_fleet = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_fleet, C_BG, 0);
    lv_obj_clear_flag(scr_fleet, LV_OBJ_FLAG_SCROLLABLE);
    mk_statusbar(scr_fleet, &fl_title, &fl_batt, "Fleet");

    fl_stat = mk_label(scr_fleet, &lv_font_montserrat_12, C_MUTED, "");
    lv_label_set_long_mode(fl_stat, LV_LABEL_LONG_DOT);
    lv_obj_set_width(fl_stat, LCD_W - 16);
    lv_obj_align(fl_stat, LV_ALIGN_TOP_LEFT, 8, BAR_H + 4);

    fl_rows = lv_obj_create(scr_fleet);
    flat(fl_rows);
    /* 🚨 flat() clears LV_OBJ_FLAG_SCROLLABLE — put it back or a long roster is
     * unreachable. Same trap as the settings menu on 2026-08-21. */
    lv_obj_add_flag(fl_rows, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(fl_rows, LCD_W, LCD_H - BAR_H - 22 - FOOT_H - 8);
    lv_obj_align(fl_rows, LV_ALIGN_TOP_LEFT, 0, BAR_H + 22);
    lv_obj_set_scroll_dir(fl_rows, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(fl_rows, LV_SCROLLBAR_MODE_AUTO);

    fl_button(scr_fleet, 68, 8,  C_BLUE,  "Code",    fl_code_clicked,    NULL);
    fl_button(scr_fleet, 84, 82, C_AMBER, "Migrate", fl_migrate_clicked, NULL);

    lv_obj_t *back = lv_btn_create(scr_fleet);
    flat(back);
    lv_obj_set_size(back, 60, 32);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -8, -10);
    lv_obj_set_style_bg_color(back, C_PANEL, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(back, 4, 0);
    lv_obj_add_event_cb(back, fl_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(back, &lv_font_montserrat_12, C_BLUE,
                            LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
}

/* The keypad. Digits only — the code is numeric so the T9 dictionary and its
 * whole letter-cycling model would be in the way here. */
static const char *CD_KEYS[12] = { "1","2","3","4","5","6","7","8","9",
                                   "<","0","K" };

static void build_code_screen(void)
{
    scr_code = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_code, C_BG, 0);
    lv_obj_clear_flag(scr_code, LV_OBJ_FLAG_SCROLLABLE);
    mk_statusbar(scr_code, &cd_title, &cd_batt, "Fleet code");

    cd_entry = mk_label(scr_code, &lv_font_montserrat_22, C_INK, "--------");
    lv_obj_align(cd_entry, LV_ALIGN_TOP_MID, 0, BAR_H + 8);

    cd_hint = mk_label(scr_code, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_align(cd_hint, LV_ALIGN_TOP_MID, 0, BAR_H + 44);

    for (int i = 0; i < 12; i++) {
        lv_obj_t *b = lv_btn_create(scr_code);
        flat(b);
        lv_obj_set_size(b, 64, 40);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT,
                     14 + (i % 3) * 72, BAR_H + 70 + (i / 3) * 46);
        lv_obj_set_style_bg_color(b, C_PANEL, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_color(b, C_LINE, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, cd_key_clicked, LV_EVENT_CLICKED,
                            (void *)CD_KEYS[i]);
        const char *face = CD_KEYS[i][0] == '<' ? LV_SYMBOL_BACKSPACE :
                           CD_KEYS[i][0] == 'K' ? LV_SYMBOL_OK : CD_KEYS[i];
        lv_color_t col = CD_KEYS[i][0] == 'K' ? C_GREEN :
                         CD_KEYS[i][0] == '<' ? C_AMBER : C_INK;
        lv_obj_t *l = mk_label(b, &lv_font_montserrat_16, col, face);
        lv_obj_center(l);
    }

    lv_obj_t *back = lv_btn_create(scr_code);
    flat(back);
    lv_obj_set_size(back, 60, 30);
    lv_obj_align(back, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_set_style_bg_color(back, C_PANEL, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(back, cd_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = mk_label(back, &lv_font_montserrat_12, C_BLUE,
                            LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);
}

void ui_show_fleet(void) { ui_load(scr_fleet); fleet_refresh(); }

/* ------------------------------------------------------------------ *
 * low battery warning
 *
 * The builder's spec, 2026-08-16: "a flashing battery full screen for a few sec every
 * min or so, or just a beep beep - I don't think it needs to make an
 * announcement."
 *
 * 🔑 WHY THIS IS BETTER THAN THE SPOKEN WARNING I PROPOSED: it needs no WAV on
 * the card, no wifi, and no working audio path. A dying battery and a failing
 * radio go together, so the warning that matters most is the one with the
 * fewest dependencies.
 *
 * It lives on LVGL's TOP LAYER, so it covers whatever screen is showing without
 * any screen needing to know about it - including mid-song on the player and
 * mid-call on Talkie.
 * ------------------------------------------------------------------ */

#define LOWBAT_PERIOD_US  (60 * 1000000)   /* nag once a minute */
#define LOWBAT_FLASHES    6                /* x300ms = ~2s of flashing */

static lv_obj_t *lowbat_box;
static int       s_lowbat_left;            /* flashes remaining, 0 = idle */

/* The screen must not blank underneath a flash in progress, or the warning
 * is invisible. Forward-declared above tick(). */
static bool lowbat_flashing(void) { return s_lowbat_left > 0; }
static int64_t   s_lowbat_last;

/* audio_tone() writes to I2S synchronously, so it CANNOT run on the UI thread -
 * that is the same mistake that froze the interface with net_up() tonight. One
 * short-lived task per warning, once a minute, is cheap. */
static void beep_task(void *arg)
{
    audio_amp(true);
    audio_tone(2200, 90, 6000);
    vTaskDelay(pdMS_TO_TICKS(90));
    audio_tone(2200, 90, 6000);
    vTaskDelete(NULL);
}

static void lowbat_tick(lv_timer_t *t)
{
    if (s_lowbat_left > 0) {
        s_lowbat_left--;
        /* Flash by toggling: visible on odd counts, hidden on even. */
        if (s_lowbat_left & 1) lv_obj_clear_flag(lowbat_box, LV_OBJ_FLAG_HIDDEN);
        else                   lv_obj_add_flag(lowbat_box, LV_OBJ_FLAG_HIDDEN);
        if (s_lowbat_left == 0) lv_obj_add_flag(lowbat_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (!battery_low()) return;

    /* 🚨 NEVER NAG A DEVICE THAT IS ALREADY IDLE - THIS WAS A REAL BUG.
     *
     * This used to call display_note_activity() to light the panel, reasoning
     * that a warning nobody can see is not a warning. The reasoning was wrong
     * and it cost real battery, in the one situation where battery is all that
     * is left:
     *
     *   display_note_activity() resets the idle timer, and power_tick() only
     *   deep-sleeps once display_idle_ms() passes the auto-sleep setting -
     *   which is measured in MINUTES. Nagging once a MINUTE therefore held the
     *   idle timer permanently below the sleep threshold, so a handset could
     *   NEVER auto-sleep once its battery was low. It sat awake with the
     *   backlight on, flashing red and beeping every minute, until the pack was
     *   flat. The builder reported it as "it wakes from deep sleep to alert about the
     *   battery being low" - the alert was not waking it, it was never letting
     *   it sleep in the first place, and any touch wake then stuck the same way.
     *
     * A warning nobody is holding is not a warning either. So: nag only while
     * the screen is already on - somebody is using the device and can act on
     * it. Once the panel blanks, go quiet and let it sleep. */
    if (!display_screen_on()) {
        /* Forget the last nag, so picking it up warns IMMEDIATELY rather than
         * up to a minute later. */
        s_lowbat_last = 0;
        return;
    }

    int64_t now = esp_timer_get_time();
    if (s_lowbat_last && now - s_lowbat_last < LOWBAT_PERIOD_US) return;
    s_lowbat_last = now;
    s_lowbat_left = LOWBAT_FLASHES;
    xTaskCreatePinnedToCore(beep_task, "lowbat", 3072, NULL, 4, NULL, 1);
}

static void build_lowbat(void)
{
    lowbat_box = lv_obj_create(lv_layer_top());
    flat(lowbat_box);
    lv_obj_set_size(lowbat_box, LCD_W, LCD_H);
    lv_obj_align(lowbat_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(lowbat_box, C_RED, 0);
    lv_obj_set_style_bg_opa(lowbat_box, LV_OPA_COVER, 0);
    /* Must not eat touches: the user may well be trying to use the device, and
     * a full-screen overlay that swallows presses would look like a freeze. */
    lv_obj_clear_flag(lowbat_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(lowbat_box, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ic = mk_label(lowbat_box, &lv_font_montserrat_22, C_INK,
                            LV_SYMBOL_BATTERY_EMPTY);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -30);
    lv_obj_t *t1 = mk_label(lowbat_box, &lv_font_montserrat_22, C_INK, "LOW");
    lv_obj_align(t1, LV_ALIGN_CENTER, 0, 6);
    lv_obj_t *t2 = mk_label(lowbat_box, &lv_font_montserrat_14, C_INK,
                            "please charge me");
    lv_obj_align(t2, LV_ALIGN_CENTER, 0, 40);
}

/* ───────────────────────── incoming message ─────────────────────────
 *
 * 🚨 THE OVERLAY IS BUILT AND SHOWN FROM THE UI THREAD ONLY.
 * msg_poll() runs on the telemetry task, and LVGL is not thread-safe — touching
 * a widget from there is the same class of bug that froze the interface with
 * net_up(). So msg.c only sets a flag, and this timer, which runs on the UI
 * thread, is what actually puts anything on screen.
 */
static lv_obj_t *msg_box, *msg_body;

/* audio_earcon() writes to I2S synchronously and must not run on the UI
 * thread — the same rule the low-battery beep follows. */
static void chirp_beep_task(void *arg)
{
    audio_earcon(CUE_MESSAGE);
    audio_earcon(CUE_MESSAGE);      /* twice: a call is more urgent than a text */
    vTaskDelete(NULL);
}

static void msg_dismiss(lv_event_t *e)
{
    lv_obj_add_flag(msg_box, LV_OBJ_FLAG_HIDDEN);
}

/* 🚨 SENDING BLOCKS ON THE NETWORK, SO IT CANNOT RUN ON THE UI THREAD.
 * msg_send() brings the radio up and does an HTTP round trip — seconds, not
 * milliseconds. Done from the button callback it would freeze the interface,
 * which is exactly the mistake net_up() already made here once. One
 * short-lived task per reply is cheap. */
static char s_reply[128];   /* must hold a full 100-char T9 message */

/* 🔑 THE RESULT HAS TO BE VISIBLE, NOT JUST AUDIBLE.
 * The first version beeped and jumped straight home, so a send took "several
 * seconds" of nothing followed by a beep the user had to interpret — and a
 * failure was indistinguishable from a success unless you knew the two cues
 * apart. The builder heard the beep and still had to ask whether it had worked.
 * 0 idle, 1 sending, 2 sent, 3 failed. Written by the reply task, read by the
 * UI timer — one int, one writer, so no lock is needed. */
static volatile int s_send_state;

/* Called from the espnow task when a message is acked or gives up. Only sets a
 * flag: the UI thread owns the drawing, and this is not the UI thread. */
static void ui_msg_status(const char *what)
{
    if (!what) return;
    if (!strcmp(what, "delivered"))          s_send_state = 2;
    else if (!strcmp(what, "not delivered")) s_send_state = 3;
    else                                     s_send_state = 1;
    audio_earcon(s_send_state == 2 ? CUE_OK :
                 s_send_state == 3 ? CUE_FAIL : CUE_OK);
}

/* 🚨 WHO THIS MESSAGE IS FOR: 0 = everyone in range, otherwise ONE handset.
 *
 * AN ID, NOT A PEER INDEX. Peer indices shift as handsets come and go, so an
 * index remembered while a picker sat open addresses whoever happens to occupy
 * that slot at send time — the silent mis-delivery the CYD hit once there were
 * three handsets. The id IS the identity; the index is derived from it, at the
 * moment of sending, and a target that has walked out of range resolves to
 * "gone" rather than to somebody else. */
static volatile uint16_t s_reply_to;

/* id -> peer index right now, or -1 if that handset is no longer in range. */
static int peer_idx_for_id(uint16_t id)
{
    int n = espnow_peer_count();
    for (int i = 0; i < n; i++)
        if (espnow_peer_id(i) == id) return i;
    return -1;
}

static void reply_task(void *arg)
{
    /* Latch the target: the picker is on the UI thread and may move while this
     * task is blocked on the radio. */
    const uint16_t to = s_reply_to;

    /* 🚨 HOLD THE LINK ACROSS THE WHOLE SEND. net_hold() stops the telemetry
     * task's net_down() from pulling the radio out from under this request —
     * exactly what net.h says it is for. The mutex in net.c stops the two
     * tasks racing to BUILD the link; this stops one destroying it while the
     * other is using it. Both are needed. */
    /* 🚨 ESP-NOW FIRST, WHENEVER ANYONE IS IN RANGE.
     *
     * This used to go straight to Home Assistant, which needs the house AP —
     * so away from home a child could type a message, watch it fail, and have
     * no other way to reach their sibling standing next to them. The builder found
     * that in a shop with all three handsets and everything in range of each
     * other.
     *
     * ESP-NOW is also simply better when it applies: no 2-8s radio bring-up,
     * no HTTP round trip, no dependency on a server being awake. HA stays as
     * the fallback for the case ESP-NOW genuinely cannot cover — the other
     * handset out of radio range but reachable across the house network. */
    /* 🚨 A PRIVATE MESSAGE NEVER FALLS BACK TO HOME ASSISTANT, AND THAT IS THE
     * WHOLE POINT OF THE FEATURE.
     *
     * The HA path posts into `input_text.kids_message` — ONE shared entity that
     * every handset polls. Falling back to it would take a message a child
     * chose to send to one sibling and hand it to all of them, silently, at
     * exactly the moment the radio was too weak to do it properly. Better to
     * say "NOT sent" and let them move closer or pick Everyone on purpose. */
    if (to) {
        int idx = peer_idx_for_id(to);
        if (idx >= 0 && espnow_msg_send(idx, s_reply)) {
            applog("[msg] sent privately to %s (%04X)",
                   espnow_peer_name(idx), (unsigned)to);
            /* Finished by the delivery callback, same as below. */
            vTaskDelete(NULL);
            return;
        }
        applog(idx < 0 ? "[msg] %04X is not in range - not sent"
                       : "[msg] private send to %04X failed", (unsigned)to);
        s_send_state = 3;
        audio_earcon(CUE_FAIL);
        vTaskDelete(NULL);
        return;
    }

    if (espnow_peer_count() > 0 && espnow_msg_send(-1, s_reply)) {
        applog("[msg] sent over ESP-NOW to %d peer(s)", espnow_peer_count());
        /* State is finished by the delivery callback: an ack means it actually
         * ARRIVED, which is more than the HTTP path ever knew. */
        vTaskDelete(NULL);
        return;
    }

    applog("[msg] nobody in radio range - falling back to Home Assistant");
    net_hold();
    bool up = net_up();
    /* ⚠️ Do not POST into a link that never came up. The old code ignored
     * net_up()'s result and called msg_send() regardless, which produced
     * "send FAILED http 0" milliseconds later and looked like an HA problem. */
    bool ok = up && msg_send(s_reply);
    if (!up) applog("[msg] send abandoned: no network");
    net_release();
    net_down();
    s_send_state = ok ? 2 : 3;

    /* The confirmation is the point: a child who taps "Coming" and sees nothing
     * happen will tap it four more times. */
    audio_earcon(ok ? CUE_OK : CUE_FAIL);
    vTaskDelete(NULL);
}

/* 🔑 CANNED REPLIES, BECAUSE THERE IS NO KEYBOARD YET — AND BECAUSE THEY ARE
 * PROBABLY WHAT GETS USED ANYWAY. "Coming" answers most of what a parent
 * actually sends, in one tap, with no typing on a 2.8" panel. The T9 screen is
 * for the messages these three do not cover, not a replacement for them. */
static const char *const REPLIES[] = { "OK", "Coming", "Can't right now" };

/* Who the popup on screen came from over the radio, 0 if it arrived via Home
 * Assistant or was a BLE chirp — neither of which carries an ESP-NOW id. */
static uint16_t s_popup_from;

static void reply_clicked(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    strlcpy(s_reply, REPLIES[idx], sizeof(s_reply));
    /* 🔑 A REPLY GOES BACK TO WHOEVER ASKED, NOT TO THE WHOLE FLEET. That is
     * what "reply" means everywhere else, and it is the tap the kids actually
     * use — the T9 keyboard is for the messages the three canned ones do not
     * cover. A broadcast original still gets a private answer: the question was
     * asked by one person and the answer is for them.
     *
     * 0 for an HA message, which keeps today's behaviour on that path — there
     * is no radio id to reply to, so it goes back the way it came. */
    s_reply_to = s_popup_from;
    lv_obj_add_flag(msg_box, LV_OBJ_FLAG_HIDDEN);
    xTaskCreatePinnedToCore(reply_task, "reply", 4096, NULL, 4, NULL, 0);
}

static void build_msg_box(void)
{
    msg_box = lv_obj_create(lv_layer_top());
    flat(msg_box);
    lv_obj_set_size(msg_box, LCD_W, LCD_H);
    lv_obj_align(msg_box, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(msg_box, C_PANEL, 0);
    lv_obj_set_style_bg_opa(msg_box, LV_OPA_COVER, 0);
    lv_obj_add_flag(msg_box, LV_OBJ_FLAG_HIDDEN);
    /* Clickable, unlike the low-battery flash: this one has to be dismissed, or
     * a message would cover the screen until the next reboot. */
    lv_obj_add_flag(msg_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(msg_box, msg_dismiss, LV_EVENT_CLICKED, NULL);

    lv_obj_t *h = mk_label(msg_box, &lv_font_montserrat_16, C_AMBER,
                           LV_SYMBOL_BELL "  MESSAGE");
    lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 26);

    /* Wrapped, not clipped: 100 characters will not fit on one line and a
     * message you can only read half of is not a message. */
    msg_body = mk_label(msg_box, &lv_font_montserrat_22, C_INK, "");
    lv_label_set_long_mode(msg_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg_body, LCD_W - 32);
    lv_obj_set_style_text_align(msg_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(msg_body, LV_ALIGN_CENTER, 0, 0);

    /* Reply buttons along the bottom. They sit ON the overlay, so they must be
     * created after it and they steal the tap from msg_dismiss() — which is
     * what we want: tapping "OK" should answer, not merely close. */
    const int bw = (LCD_W - 4 * 8) / 3;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = lv_btn_create(msg_box);
        flat(b);
        lv_obj_set_size(b, bw, 34);
        lv_obj_set_style_bg_color(b, C_LINE, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 8 + i * (bw + 8), -30);
        lv_obj_add_event_cb(b, reply_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = mk_label(b, &lv_font_montserrat_12, C_INK, REPLIES[i]);
        lv_obj_center(l);
    }

    lv_obj_t *f = mk_label(msg_box, &lv_font_montserrat_10, C_MUTED,
                           "or tap the background to dismiss");
    lv_obj_align(f, LV_ALIGN_BOTTOM_MID, 0, -8);
}

static void msg_tick(lv_timer_t *t)
{
    /* A chirp borrows the message overlay rather than getting a screen of its
     * own: it is the same event from the user's side — someone wants you — and
     * the popup already wakes the panel and can be dismissed with a tap. */
    char id[16];
    if (ble_take_chirp(id, sizeof(id))) {
        s_popup_from = 0;                 /* BLE chirp: no ESP-NOW id to reply to */
        lv_label_set_text_fmt(msg_body, "%s\n\nwants to talk", msg_name_for(id));
        lv_obj_clear_flag(msg_box, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(msg_box);
        display_note_activity();
        xTaskCreatePinnedToCore(chirp_beep_task, "chirp", 3072, NULL, 4, NULL, 1);
        return;
    }

    /* 🔑 A RADIO MESSAGE GETS THE SAME POPUP AS AN HA ONE. It is the same event
     * to a child, and off-grid it is the ONLY one that can happen — so it must
     * not be the quieter of the two. Checked first for exactly that reason. */
    if (espnow_inbox_take_dirty()) {
        const espnow_msg_t *m = espnow_inbox(0);
        if (m) {
            s_popup_from = m->from_id;
            /* Say when a message was for this handset alone. A child who cannot
             * tell a private message from a group one has no reason to trust
             * that the private ones really are. */
            lv_label_set_text_fmt(msg_body, m->priv ? "%s\n\n- %s (to you)"
                                                    : "%s\n\n- %s",
                                  m->text, m->from);
            lv_obj_clear_flag(msg_box, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(msg_box);
            display_note_activity();
            xTaskCreatePinnedToCore(chirp_beep_task, "chirp", 3072, NULL, 4, NULL, 1);
            return;
        }
    }

    if (!msg_have_new()) return;
    msg_mark_read();
    s_popup_from = 0;                     /* came via HA, so reply the same way */

    /* If several piled up while the handset was asleep or out of range, say so
     * — otherwise the newest silently stands in for all of them and nobody
     * knows anything was missed. */
    int missed = msg_missed();
    if (missed > 1) lv_label_set_text_fmt(msg_body, "%s\n\n(+%d earlier)",
                                          msg_text(), missed - 1);
    else            lv_label_set_text(msg_body, msg_text());
    lv_obj_clear_flag(msg_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(msg_box);
    /* Wake the panel — the chime already sounded, and a message that arrives on
     * a blanked screen would otherwise be a noise with nothing behind it. */
    display_note_activity();
}

/* ------------------------------------------------------------------ *
 * T9 text entry — PREDICTIVE, backed by the dictionary in flash
 *
 * 🔑 THE MULTI-TAP VERSION'S OWN COMMENT PREDICTED THIS SWAP: "the KEYPAD and
 * the BUFFER know nothing about how a keypress becomes a letter, so adding a
 * predictive resolver later is a swap, not a rewrite." It was right — the map,
 * the send path and the screen are unchanged; only the resolver moved.
 *
 * Multi-tap is gone. The builder, 2026-08-21: "it should have the prodictive messaging
 * like the cyd this one wont do that its old". One press per letter, the
 * dictionary decides which letter it was:
 *
 *     4663  ->  good, home, gone       74663 -> phone, shone       843 -> the
 *
 * 🚨 AND IT DELETES THE HARDEST PART OF MULTI-TAP: the 900ms commit timeout,
 * which was the only way to type two letters from one key ("ab" = 2, pause,
 * 2 2). Too short and it committed mid-word for a slow child, too long and
 * typing felt stuck. Predictive has no such window and no such tuning.
 *
 * ⚠️ NO DICTIONARY MEANS RAW DIGITS, NOT A DEAD KEYPAD. If t9_begin() failed,
 * the pending word shows "4663" and everything else still works — a keypad that
 * swallows presses looks broken, one that shows what it heard does not.
 * ------------------------------------------------------------------ */

#define T9_MAX  100                /* input_text.kids_message caps at 100 */
#define T9_DIG  10                 /* longest word we will predict        */
#define T9_CAND 4                  /* candidates shown at once            */

static char        t9_buf[T9_MAX + 1];      /* committed text            */
static int         t9_len;
static char        t9_digits[T9_DIG + 1];   /* the word being typed      */
static int         t9_dlen;
static const char *t9_cands[12];
static int         t9_ncand, t9_sel;
static lv_obj_t   *t9_text, *t9_cand_mx, *t9_keys_mx;

/* 🔑 WHO THE MESSAGE IS FOR — A VISIBLE ROW, NOT A HIDDEN GESTURE.
 *
 * The obvious place for this was a tap on the status-bar title, which costs no
 * pixels. It is also exactly the mistake this project already paid for once:
 * a hidden long-press on that bar opened a full-screen calibration nobody meant
 * to open, and "i just dont know why it keeps comming up" took a while to
 * explain. A child cannot trust a private message they cannot see is private,
 * so the recipient gets a row of its own that always says who it is.
 *
 * 0 = everyone in range. Otherwise a peer id — see s_reply_to on ids vs
 * indices. */
static lv_obj_t   *t9_to_btn, *t9_to_lbl;
static uint16_t    t9_to;

static void t9_to_draw(void)
{
    if (!t9_to_lbl) return;

    /* Re-validate every repaint: the chosen handset may have walked out of
     * range since it was picked, and silently addressing whoever took its slot
     * is the failure this is written to avoid. */
    if (t9_to && peer_idx_for_id(t9_to) < 0) t9_to = 0;

    if (t9_to) {
        int idx = peer_idx_for_id(t9_to);
        lv_label_set_text_fmt(t9_to_lbl, LV_SYMBOL_RIGHT " To: %s (only)",
                              espnow_peer_name(idx));
        lv_obj_set_style_text_color(t9_to_lbl, C_GREEN, 0);
    } else if (espnow_play_count() > 0) {
        lv_label_set_text(t9_to_lbl, LV_SYMBOL_RIGHT " To: Everyone");
        lv_obj_set_style_text_color(t9_to_lbl, C_AMBER, 0);
    } else {
        lv_label_set_text(t9_to_lbl, LV_SYMBOL_RIGHT " To: Home Assistant");
        lv_obj_set_style_text_color(t9_to_lbl, C_MUTED, 0);
    }
}

/* Everyone -> each handset in range -> Everyone. The same cycle the games use
 * to choose an opponent, so it is a gesture the kids have already learned. */
static void t9_to_clicked(lv_event_t *e)
{
    (void)e;
    t9_to = play_cycle_id(t9_to);
    t9_to_draw();
}

/* The candidate row is ONE lv_btnmatrix with a rebuilt map, not four buttons.
 * Same reason as the keypad below: every LVGL object on this board comes out of
 * INTERNAL RAM, and internal RAM is what WiFi needs. */
static char        t9_cand_txt[T9_CAND][14];
static const char *t9_cand_map[T9_CAND + 1];

/* ⚠️ NOT const, and the last caption is a BUFFER: the inbox cell carries a
 * count that changes while the screen exists, and lv_btnmatrix keeps the map
 * pointers rather than copying the strings. */
static char t9_in_txt[12] = LV_SYMBOL_ENVELOPE " 0";
static const char *T9_MAP[] = {
    "1 .,?",  "2 abc",  "3 def",  "\n",
    "4 ghi",  "5 jkl",  "6 mno",  "\n",
    "7 pqrs", "8 tuv",  "9 wxyz", "\n",
    "*next",  "0 _",    "#del",   "\n",
    LV_SYMBOL_LEFT " Back", LV_SYMBOL_UPLOAD " SEND", t9_in_txt, ""
};

/* What the pending keystrokes currently mean. Falls back to the raw digits so
 * the keypad is never silent about what it heard. */
static const char *t9_pending_word(void)
{
    if (!t9_dlen) return "";
    if (t9_ncand)  return t9_cands[t9_sel];
    return t9_digits;
}

static void t9_cands_draw(void)
{
    int n = t9_ncand > T9_CAND ? T9_CAND : t9_ncand;
    int m = 0;
    for (int i = 0; i < n; i++) {
        snprintf(t9_cand_txt[i], sizeof(t9_cand_txt[i]), "%s%s",
                 i == t9_sel ? "*" : "", t9_cands[i]);
        t9_cand_map[m++] = t9_cand_txt[i];
    }
    if (!m) {           /* an empty map is not legal; one blank cell is */
        strcpy(t9_cand_txt[0], " ");
        t9_cand_map[m++] = t9_cand_txt[0];
    }
    t9_cand_map[m] = "";
    lv_btnmatrix_set_map(t9_cand_mx, t9_cand_map);
}

static void t9_draw(void)
{
    if (s_send_state) {
        static const char *const MSG[] = { "", "sending" LV_SYMBOL_UPLOAD,
                                           "sent " LV_SYMBOL_OK,
                                           "NOT sent " LV_SYMBOL_WARNING };
        lv_label_set_text(t9_text, MSG[s_send_state]);
        lv_obj_set_style_text_color(t9_text,
            s_send_state == 3 ? C_RED : s_send_state == 2 ? C_GREEN : C_AMBER, 0);
        return;
    }
    char shown[T9_MAX + T9_DIG + 8];
    snprintf(shown, sizeof(shown), "%s%s%s", t9_buf, t9_pending_word(),
             t9_dlen ? "_" : "");
    lv_label_set_text(t9_text, (t9_len || t9_dlen) ? shown : "type a message");
    lv_obj_set_style_text_color(t9_text, (t9_len || t9_dlen) ? C_INK : C_MUTED, 0);
    t9_cands_draw();
    t9_to_draw();

    static uint8_t shown_in = 255;
    uint8_t in_total = (uint8_t)(msg_inbox_count() + espnow_inbox_count());
    if (in_total != shown_in) {
        shown_in = in_total;
        snprintf(t9_in_txt, sizeof(t9_in_txt), LV_SYMBOL_ENVELOPE " %u", shown_in);
        if (t9_keys_mx) lv_btnmatrix_set_map(t9_keys_mx, T9_MAP);
    }
}

/* Fold the pending word into the committed text. */
static void t9_commit_word(bool add_space)
{
    const char *w = t9_pending_word();
    for (const char *p = w; *p && t9_len < T9_MAX; p++) t9_buf[t9_len++] = *p;
    if (add_space && t9_len < T9_MAX) t9_buf[t9_len++] = ' ';
    t9_buf[t9_len] = 0;
    t9_dlen = 0; t9_digits[0] = 0;
    t9_ncand = 0; t9_sel = 0;
}

static void t9_digit(char d)
{
    if (t9_dlen >= T9_DIG) return;
    t9_digits[t9_dlen++] = d;
    t9_digits[t9_dlen]   = 0;
    t9_ncand = t9_lookup(t9_digits, t9_cands, 12);
    t9_sel   = 0;
    t9_draw();
}

/* '*' — the next word that matches the same keys. */
static void t9_next_cand(void)
{
    if (t9_ncand > 1) t9_sel = (t9_sel + 1) % t9_ncand;
    t9_draw();
}

/* '#' — one digit off the pending word, or one character off the committed
 * text once there is no pending word left. */
static void t9_del(void)
{
    if (t9_dlen) {
        t9_digits[--t9_dlen] = 0;
        t9_ncand = t9_dlen ? t9_lookup(t9_digits, t9_cands, 12) : 0;
        t9_sel = 0;
    } else if (t9_len) {
        t9_buf[--t9_len] = 0;
    }
    t9_draw();
}

/* Key 1 cycles punctuation in place, rather than adding a fifth thing to the
 * bottom row. */
static void t9_punct(void)
{
    static const char P[] = ". , ? !";
    if (t9_dlen) t9_commit_word(false);
    if (t9_len) {
        for (int i = 0; P[i]; i += 2) {
            if (t9_buf[t9_len - 1] == P[i]) {
                t9_buf[t9_len - 1] = P[(i + 2) % 8];
                t9_draw();
                return;
            }
        }
    }
    if (t9_len < T9_MAX) { t9_buf[t9_len++] = '.'; t9_buf[t9_len] = 0; }
    t9_draw();
}

static void t9_send_clicked(lv_event_t *e)
{
    (void)e;
    if (t9_dlen) t9_commit_word(false);      /* never send a half-typed word */
    if (!t9_len) { applog("[t9] send pressed with nothing typed"); return; }
    applog("[t9] sending \"%s\"", t9_buf);
    strlcpy(s_reply, t9_buf, sizeof(s_reply));
    /* Resolved once more inside reply_task, from the id, at the moment it goes
     * on the air. */
    s_reply_to = t9_to;
    s_send_state = 1;
    t9_draw();
    /* Same task as the canned replies — msg_send() blocks on the network and
     * must never run on the UI thread.
     * ⚠️ Deliberately does NOT jump home: the radio takes 2-8s to come up, and
     * leaving the screen mid-send means the outcome lands somewhere the user is
     * no longer looking. */
    xTaskCreatePinnedToCore(reply_task, "reply", 4096, NULL, 4, NULL, 0);
}

/* Runs on the UI timer. With multi-tap gone this only watches the send. */
static void t9_tick(lv_timer_t *t)
{
    (void)t;
    static int shown_state;
    if (s_send_state != shown_state) {
        shown_state = s_send_state;
        /* Clear only once it has actually landed — a failed message the user
         * has to retype is a worse outcome than a stale one on screen. */
        if (s_send_state == 2) { t9_len = 0; t9_buf[0] = 0; }
        t9_draw();
        return;
    }
    if (s_send_state >= 2) {
        static int ticks;
        if (++ticks > 20) { ticks = 0; s_send_state = 0; shown_state = 0; t9_draw(); }
    }
}

/* 🚨 ONE lv_btnmatrix, NOT THIRTY-SIX OBJECTS — THIS IS A MEMORY FIX.
 *
 * The first version built 12 lv_btn plus 24 lv_label, and every LVGL object on
 * this board comes out of INTERNAL RAM (small allocations never reach PSRAM).
 * That cost ~14KB, and the symptom was not "the screen is big" — it was WIFI
 * REFUSING TO ASSOCIATE:
 *
 *   [wifi] disconnected, reason 202
 *   [wifi] FAILED to associate (internal free 6995)
 *
 * The builder, 2026-08-17: "sending takes a long time it still is not sending". The
 * send path was never at fault; the keypad had eaten the memory the radio
 * needed. 🔑 THE PROJECT RULE HOLDS AGAIN: association failing on a board that
 * associated fine an hour ago is MEMORY, never RF.
 */

/* Matrix button id -> what it does. 0 is the 1 key, 1-8 are digits 2-9. */
#define T9_ID_NEXT   9
#define T9_ID_SPACE  10
#define T9_ID_DEL    11
#define T9_ID_BACK   12
#define T9_ID_SEND   13
#define T9_ID_INBOX  14

static void t9_matrix_event(lv_event_t *e)
{
    uint32_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    switch (id) {
    case T9_ID_NEXT:  t9_next_cand();       break;
    case T9_ID_SPACE: t9_commit_word(true); t9_draw(); break;
    case T9_ID_DEL:   t9_del();             break;
    case T9_ID_BACK:  ui_show_home();       break;
    case T9_ID_SEND:  t9_send_clicked(NULL); break;
    case T9_ID_INBOX: ui_show_inbox();      break;
    default:
        if (id == 0)     t9_punct();
        else if (id < 9) t9_digit((char)('1' + id));
        break;
    }
}

/* Tapping a candidate picks it, the same as the CYD's row. */
static void t9_cand_event(lv_event_t *e)
{
    uint32_t id = lv_btnmatrix_get_selected_btn(lv_event_get_target(e));
    if ((int)id < t9_ncand) { t9_sel = (int)id; t9_draw(); }
}

static void build_t9_screen(void)
{
    scr_t9 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_t9, C_BG, 0);
    lv_obj_clear_flag(scr_t9, LV_OBJ_FLAG_SCROLLABLE);
    mk_statusbar(scr_t9, &t9_title, &t9_batt, "Message");

    /* The line being typed, wrapped: 100 characters do not fit on one line and
     * a message you can only see the end of is not much use. */
    /* The recipient row, above everything: it is read before the message is
     * typed, not after. Full width so it is hard to miss and easy to hit. */
    t9_to_btn = lv_btn_create(scr_t9);
    flat(t9_to_btn);
    lv_obj_set_size(t9_to_btn, LCD_W - 8, 26);
    lv_obj_align(t9_to_btn, LV_ALIGN_TOP_LEFT, 4, BAR_H + 2);
    lv_obj_set_style_bg_color(t9_to_btn, C_LINE, 0);
    lv_obj_set_style_bg_opa(t9_to_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t9_to_btn, 4, 0);
    lv_obj_add_event_cb(t9_to_btn, t9_to_clicked, LV_EVENT_CLICKED, NULL);
    t9_to_lbl = mk_label(t9_to_btn, &lv_font_montserrat_12, C_AMBER, "");
    lv_obj_center(t9_to_lbl);

    t9_text = mk_label(scr_t9, &lv_font_montserrat_14, C_MUTED, "type a message");
    lv_label_set_long_mode(t9_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(t9_text, LCD_W - 12);
    lv_obj_align(t9_text, LV_ALIGN_TOP_LEFT, 6, BAR_H + 32);

    /* The candidates, above the keys and below the text: the eye is already
     * there, and it is the row that changes on every press. */
    t9_cand_mx = lv_btnmatrix_create(scr_t9);
    t9_cand_txt[0][0] = ' '; t9_cand_txt[0][1] = 0;
    t9_cand_map[0] = t9_cand_txt[0];
    t9_cand_map[1] = "";
    lv_btnmatrix_set_map(t9_cand_mx, t9_cand_map);
    lv_obj_set_size(t9_cand_mx, LCD_W - 8, 34);
    lv_obj_align(t9_cand_mx, LV_ALIGN_TOP_LEFT, 4, BAR_H + 66);
    flat(t9_cand_mx);
    lv_obj_set_style_pad_all(t9_cand_mx, 2, 0);
    lv_obj_set_style_pad_column(t9_cand_mx, 3, 0);
    lv_obj_set_style_bg_opa(t9_cand_mx, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(t9_cand_mx, C_LINE, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(t9_cand_mx, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(t9_cand_mx, C_AMBER, LV_PART_ITEMS);
    lv_obj_set_style_text_font(t9_cand_mx, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_radius(t9_cand_mx, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_width(t9_cand_mx, 0, LV_PART_ITEMS);
    lv_btnmatrix_set_one_checked(t9_cand_mx, false);
    lv_obj_add_event_cb(t9_cand_mx, t9_cand_event, LV_EVENT_VALUE_CHANGED, NULL);

    /* ⚠️ 26px went to the recipient row, so the five key rows are ~37px each
     * instead of ~42. Still bigger than the 34px candidate row above them,
     * which has never been a problem to hit. If it turns out to be, this is the
     * one constant to change. */
    const int top = BAR_H + 104;
    lv_obj_t *m = lv_btnmatrix_create(scr_t9);
    t9_keys_mx = m;
    lv_btnmatrix_set_map(m, T9_MAP);
    lv_obj_set_size(m, LCD_W - 8, LCD_H - top - 6);
    lv_obj_align(m, LV_ALIGN_TOP_LEFT, 4, top);
    flat(m);
    lv_obj_set_style_pad_all(m, 2, 0);
    lv_obj_set_style_pad_row(m, 3, 0);
    lv_obj_set_style_pad_column(m, 3, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);

    /* Keys. A pressed state that is actually visible matters: with no feedback
     * the natural response is to tap again. */
    lv_obj_set_style_bg_color(m, C_PANEL, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(m, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, C_LINE, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(m, C_INK, LV_PART_ITEMS);
    lv_obj_set_style_text_font(m, &lv_font_montserrat_14, LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 4, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 0, LV_PART_ITEMS);

    /* ⚠️ NO CHECKABLE / TOGGLE BEHAVIOUR. A keypad where a key looks stuck
     * reads as a crash. */
    lv_btnmatrix_set_one_checked(m, false);
    lv_obj_add_event_cb(m, t9_matrix_event, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ------------------------------------------------------------------ *
 * The inbox
 *
 * 🚨 ONE LABEL, NOT ONE ROW PER MESSAGE. Ten rows would be ten lv_obj plus ten
 * lv_label, and every LVGL object on this board comes out of INTERNAL RAM - the
 * same accounting that made the keypad a single btnmatrix after the first
 * version cost ~14KB and stopped WiFi associating. The messages are read, not
 * interacted with, so they do not need to be objects.
 * ------------------------------------------------------------------ */

static lv_obj_t *scr_inbox, *inbox_title, *inbox_batt, *inbox_body;

static void inbox_back_clicked(lv_event_t *e) { (void)e; ui_show_t9(); }
static void inbox_clear_clicked(lv_event_t *e)
{
    (void)e;
    msg_inbox_clear();
    espnow_inbox_clear();
    lv_label_set_text(inbox_body, "nothing yet");
    lv_obj_set_style_text_color(inbox_body, C_MUTED, 0);
    applog("[msg] inbox cleared");
}

static void build_inbox_screen(void)
{
    scr_inbox = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_inbox, C_BG, 0);
    lv_obj_clear_flag(scr_inbox, LV_OBJ_FLAG_SCROLLABLE);
    mk_statusbar(scr_inbox, &inbox_title, &inbox_batt, "Inbox");

    lv_obj_t *box = lv_obj_create(scr_inbox);
    flat(box);
    /* ⚠️ flat() clears LV_OBJ_FLAG_SCROLLABLE as its last line - the same trap
     * that stopped the settings menu scrolling. Ten messages do not fit. */
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, LCD_W, LCD_H - BAR_H - FOOT_H);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, BAR_H);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(box, LV_SCROLLBAR_MODE_AUTO);

    inbox_body = mk_label(box, &lv_font_montserrat_14, C_INK, "nothing yet");
    lv_label_set_long_mode(inbox_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(inbox_body, LCD_W - 20);
    lv_obj_align(inbox_body, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *foot = lv_obj_create(scr_inbox);
    flat(foot);
    lv_obj_set_size(foot, LCD_W, FOOT_H);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(foot, C_PANEL, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(foot, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(foot, C_LINE, 0);
    lv_obj_set_style_border_width(foot, 1, 0);

    lv_obj_t *b = lv_btn_create(foot);
    flat(b);
    lv_obj_set_size(b, 80, 26);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_add_event_cb(b, inbox_back_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(b, &lv_font_montserrat_12, C_BLUE,
                           LV_SYMBOL_LEFT " Back"));

    lv_obj_t *c = lv_btn_create(foot);
    flat(c);
    lv_obj_set_size(c, 80, 26);
    lv_obj_align(c, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_add_event_cb(c, inbox_clear_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_center(mk_label(c, &lv_font_montserrat_12, C_MUTED, "Clear"));
}

void ui_show_inbox(void)
{
    /* 🔑 BOTH SOURCES, RADIO FIRST. A message that arrived over ESP-NOW came
     * from a handset in range and needed no router; one from Home Assistant
     * came across the house network. The child does not care which, but the
     * radio ones are the ones that work away from home, so they go on top —
     * and they carry a sender name, which the HA box does not. */
    uint8_t hn = msg_inbox_count();
    int     en = espnow_inbox_count();

    if (!hn && !en) {
        lv_label_set_text(inbox_body, "nothing yet");
        lv_obj_set_style_text_color(inbox_body, C_MUTED, 0);
    } else {
        static char buf[MSG_INBOX_MAX * 112 + ESPNOW_INBOX_N * 140];
        size_t off = 0;
        bool   first = true;
        for (int i = 0; i < en && off < sizeof(buf) - 4; i++) {
            const espnow_msg_t *m = espnow_inbox(i);
            if (!m) continue;
            off += snprintf(buf + off, sizeof(buf) - off, "%s%s: %s",
                            first ? "" : "\n\n", m->from, m->text);
            first = false;
        }
        for (uint8_t i = 0; i < hn && off < sizeof(buf) - 4; i++) {
            off += snprintf(buf + off, sizeof(buf) - off, "%s%s",
                            first ? "" : "\n\n", msg_inbox_text(i));
            first = false;
        }
        lv_label_set_text(inbox_body, buf);
        lv_obj_set_style_text_color(inbox_body, C_INK, 0);
    }
    ui_load(scr_inbox);
}

void ui_show_t9(void)
{
    t9_len = 0; t9_buf[0] = 0;
    t9_dlen = 0; t9_digits[0] = 0;
    t9_ncand = 0; t9_sel = 0;
    t9_draw();
    ui_load(scr_t9);
}


void ui_init(void)
{
    /* The gateway's answer can arrive at any time; register before anything
     * asks. See gw_time_ask() above. */
    espnow_gw_on_response(gw_time_response);

    build_home_screen();
    build_menu_screen();
    build_inbox_screen();
    build_radio_screen();
    build_gps_screen();
    build_map_screen();
    build_fleet_screen();
    build_code_screen();
    espnow_msg_on_status(ui_msg_status);
    build_list_screen();
    build_play_screen();
    build_term_screen();
    build_talk_screen();
    build_wt_screen();
    build_cal_screen();
    lv_timer_create(tick, 500, NULL);
    /* Separate, fast timer purely for the talk button. The 500ms UI tick is
     * fine for a battery meter and far too slow for "did they let go yet". */
    lv_timer_create(talk_poll, 60, NULL);
    lv_timer_create(cal_tick, 40, NULL);
    build_lowbat();
    lv_timer_create(lowbat_tick, 300, NULL);
    lv_timer_create(usbmsc_unplug_tick, 500, NULL);
    lv_timer_create(ab_tick, 1000, NULL);
    build_msg_box();
    lv_timer_create(msg_tick, 400, NULL);
    build_t9_screen();
    lv_timer_create(t9_tick, 150, NULL);
    /* 🚨 CALIBRATE BEFORE ANYTHING ELSE IF WE NEVER HAVE.
     *
     * The calibration screen was originally reachable only by long-pressing the
     * status bar — which is useless, because a device whose touch mapping is
     * wrong cannot be driven to the tool that fixes the mapping. The builder could not
     * reach it, and every button diagnosis after that was guesswork.
     * A device that does not know where fingers land has exactly one job. */
    if (!settings_touch_cal()->valid) {
        applog("no touch calibration - starting it");
        ui_show_cal();
    } else {
        ui_show_home();
    }
    applog("ready: %d artists", library_artist_count());
    ESP_LOGI(TAG, "ui up: %d artists", library_artist_count());
}
