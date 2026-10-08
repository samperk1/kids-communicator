/* ------------------------------------------------------------------ *
 * games.c — tic-tac-toe, brick breaker and snake
 *
 * Reached only from the Now Playing screen, and every Back lands back there.
 * The whole design goal is "something to do while a track plays on a car
 * ride", which drives three rules:
 *
 *   1. 🚨 NO WIFI, EVER. Internal RAM sits on the ~44KB WiFi threshold while
 *      the audio path is open. A game over music is exactly that case, so
 *      nothing here calls net_*, ha_* or anything that might bring the radio up.
 *
 *   2. 🚨 THE DECODER MUST NOT BE STARVED. minimp3 runs on its own task but
 *      LVGL redraws share the CPU, so every game moves a handful of small
 *      objects instead of repainting the screen, and the tick rates are the
 *      slowest that still feel right (40fps for the ball, 5-11fps for snake).
 *      Nothing here allocates a canvas or a framebuffer — a PSRAM draw buffer
 *      would crash spi_device_polling_transmit, which still uses DMA.
 *
 *   3. 🚨 EXACTLY ONE TIMER RUNS AT A TIME, AND LEAVING KILLS IT. A game tick
 *      left running behind the player screen is invisible CPU burn for as long
 *      as the device is awake. tmr_stop() is called on every exit path.
 *
 * The scores live in their own NVS namespace ("games"). They deliberately do
 * NOT go through settings.c — that namespace holds the per-board audio gain and
 * the touch calibration, and a game is not a reason to be anywhere near those.
 * ------------------------------------------------------------------ */

#include <string.h>
#include <stdlib.h>

#include "lvgl.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_log.h"

#include "esp_heap_caps.h"

#include "hw.h"
#include "ui.h"
#include "applog.h"
#include "games.h"
#include "esp_timer.h"
#include "espnow.h"
#include "flamingo.h"
#include "tanks.h"

static const char *TAG = "games";

/* Mirrors the palette in ui.c. Duplicated rather than shared because the two
 * files have no other reason to know about each other. */
#define C_BG      lv_color_hex(0x080A0E)
#define C_PANEL   lv_color_hex(0x121722)
#define C_LINE    lv_color_hex(0x232C3A)
#define C_INK     lv_color_hex(0xE8EDF5)
#define C_MUTED   lv_color_hex(0x7C8798)
#define C_AMBER   lv_color_hex(0xF0A830)
#define C_BLUE    lv_color_hex(0x6E9BD1)
#define C_GREEN   lv_color_hex(0x58B368)
#define C_RED     lv_color_hex(0xD9534F)
#define C_ONAMBER lv_color_hex(0x16120A)

#define HDR_H 26

/* ------------------------------------------------------------------ *
 * small shared helpers
 * ------------------------------------------------------------------ */

static lv_timer_t *s_tmr;          /* the one and only running game tick */

/* Declared up here because every game's Back needs the menu to exist before it
 * can tear its own screen down onto it. */
static lv_obj_t *scr_menu;
static bool scr_menu_ready(void);

static void tmr_stop(void)
{
    if (s_tmr) { lv_timer_del(s_tmr); s_tmr = NULL; }
}

static void tmr_start(lv_timer_cb_t cb, uint32_t ms)
{
    tmr_stop();
    s_tmr = lv_timer_create(cb, ms, NULL);
}

static lv_obj_t *rect(lv_obj_t *p, int w, int h, lv_color_t c)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 2, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *lbl(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *t)
{
    lv_obj_t *o = lv_label_create(p);
    lv_obj_set_style_text_font(o, f, 0);
    lv_obj_set_style_text_color(o, c, 0);
    lv_label_set_text(o, t);
    return o;
}

static lv_obj_t *btn_ev(lv_obj_t *p, int w, int h, lv_color_t bg, lv_color_t fg,
                        const lv_font_t *f, const char *text, lv_event_cb_t cb,
                        lv_event_code_t code, void *ud)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_add_event_cb(b, cb, code, ud);
    lv_obj_t *l = lbl(b, f, fg, text);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *btn(lv_obj_t *p, int w, int h, lv_color_t bg, lv_color_t fg,
                     const lv_font_t *f, const char *text, lv_event_cb_t cb,
                     void *ud)
{
    return btn_ev(p, w, h, bg, fg, f, text, cb, LV_EVENT_CLICKED, ud);
}

/* One header for every screen: Back on the left, title in the middle, a score
 * on the right. Back is a real 44px-wide target because the person using it is
 * six and the alternative is a trapped mode. */
static lv_obj_t *mk_hdr(lv_obj_t *scr, const char *title, lv_event_cb_t back)
{
    lv_obj_t *bar = rect(scr, LCD_W, HDR_H, C_PANEL);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    /* 🚨 BACK USED TO NEED SEVERAL PRESSES. The builder, 2026-08-25: "the back button
     * on the games screen seems unresponsive i have to press it several times
     * to get it to go back."
     *
     * Not the touch driver, and not the calibration. LV_EVENT_CLICKED only
     * fires if the finger is lifted while still INSIDE the object, and this was
     * a 46x20 target hard against the top edge of the panel - the one place a
     * capacitive digitiser is least accurate. A finger that rolls a couple of
     * pixels on the way up leaves the button, so the press is real, is seen,
     * and produces nothing at all. Pressing "harder" makes it worse, because a
     * flatter finger moves further.
     *
     * Three fixes, all cheap:
     *   - LV_EVENT_RELEASED instead of CLICKED. The press still has to START on
     *     the button, so nothing else can trigger it, but where the finger ends
     *     up no longer matters.
     *   - 54x22 rather than 46x20, and one pixel higher.
     *   - 14px of invisible click area all round, which is what actually covers
     *     the drift.
     * The look is unchanged. Leave the other buttons on CLICKED: they are big,
     * and a game cell that fires on release would go off during a drag. */
    lv_obj_t *b = btn_ev(scr, 54, 22, C_LINE, C_INK, &lv_font_montserrat_12,
                         LV_SYMBOL_LEFT " Back", back, LV_EVENT_RELEASED, NULL);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 3, 2);
    lv_obj_set_ext_click_area(b, 14);

    lv_obj_t *t = lbl(scr, &lv_font_montserrat_12, C_MUTED, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *score = lbl(scr, &lv_font_montserrat_12, C_AMBER, "");
    lv_obj_align(score, LV_ALIGN_TOP_RIGHT, -6, 6);
    return score;
}

/* High scores. Their own namespace on purpose - see the file header. */
static int hs_get(const char *key)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open("games", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, key, &v);
        nvs_close(h);
    }
    return (int)v;
}

static void hs_set(const char *key, int v)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, key, v);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "high score %s = %d", key, v);
}

/* 🚨 A GAME SCREEN IS FREED THE MOMENT YOU LEAVE IT.
 *
 * The first version built each screen once and kept it forever. Three games
 * visited = ~120 live LVGL objects, and LV_MEM_CUSTOM sends those to the
 * ordinary heap, which is INTERNAL RAM. That is the same pool the player needs
 * a DMA-capable buffer from to open the next track, so the music died with
 * "esp_dma_capable_malloc: Not enough heap memory" and could never restart.
 *
 * Rebuilding a screen costs a few milliseconds on entry. Holding one costs
 * kilobytes for as long as the device is awake. Rebuild.
 *
 * lv_obj_del_async, not lv_obj_del: we are called from an event handler of a
 * button that lives ON the screen being deleted. */
static void leave_to(lv_obj_t *dest, lv_obj_t **doomed)
{
    lv_scr_load(dest);
    if (*doomed) { lv_obj_del_async(*doomed); *doomed = NULL; }
    applog("[games] left, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static int rnd(int n) { return n > 0 ? (int)(esp_random() % (uint32_t)n) : 0; }

/* 🚨 A SCOREBOARD IS INDEXED BY DEVICE ID, NEVER BY SEAT.
 *
 * Boards in this file are replayed and hold no memory, but a session score is
 * the one thing that genuinely accumulates — and the seat a player occupies is
 * NOT a stable name for them. Hearts alternates the deal every hand, so a total
 * indexed by seat is handed to the other player the moment the deal passes
 * over: from hand two each handset read its own score out of the opposite slot
 * and the two screens disagreed. The builder and Kid 1 found it in a real game.
 *
 * Lower device id is always slot 0. Both handsets compute the same answer from
 * the same two numbers, which is the rule the roles and the shuffle already
 * follow. */
static int id_slot(uint16_t me, uint16_t them) { return me <= them ? 0 : 1; }

/* ------------------------------------------------------------------ *
 * Choosing an opponent — PLAYABLE peers only.
 *
 * 🚨 A GATEWAY IS IN RANGE BUT IT IS NOT A PLAYER. It beacons like a handset,
 * so from 2026-09-07 every picker here started offering `GW-BBBB` and
 * `GW-AAAA` as opponents, and Uno seated them. The builder: "i was trying to play uno
 * with the kids and was not able to as it was waiting for the gatway to play".
 * A gateway will never take its turn, so the game simply stops.
 *
 * ⚠️ THESE WORK IN TRUE PEER INDICES. espnow_play_idx() maps a playable
 * position to the real index; a dense renumbering would address the wrong
 * handset the moment somebody walks out of range, which is the failure
 * ttt/c4/battleship already carry warnings about.
 * ------------------------------------------------------------------ */

/* Keep `*slot` if it still names a playable peer, otherwise take the first one
 * (or -1 when nobody is there). Replaces the old
 * `slot = espnow_peer_count() > 0 ? 0 : -1`, which could land on a gateway. */
static void pick_default_peer(int *slot)
{
    if (*slot >= 0 && *slot < espnow_peer_count() &&
        !espnow_peer_is_gateway(*slot)) return;
    *slot = espnow_play_idx(0);
}

/* The next playable peer after `cur`, wrapping. `cur` not being playable (it
 * aged out, or it was a gateway) starts the cycle from the beginning rather
 * than failing — the button must always do something. */
static int play_next(int cur)
{
    int n = espnow_play_count();
    if (n <= 0) return -1;
    int pos = -1;
    for (int k = 0; k < n; k++) if (espnow_play_idx(k) == cur) { pos = k; break; }
    return espnow_play_idx((pos + 1) % n);
}

/* 🚨 A STARTER WHO IS NOT PLAYING LEAVES BOTH HANDSETS AS PLAYER 2.
 *
 * Every VS game here decides sides with `me == starter ? first : second`, and
 * the starter SURVIVES a change of opponent — it is only re-derived when it is
 * zero. So after a game against a third handset it can still name somebody who
 * is not in the room: neither player matches it, BOTH take the second side,
 * and each sits waiting for the other to move. The builder and the kids hit exactly
 * this on Battleship, on a board that had last been played against Kid 1.
 *
 * A starter is only meaningful if it names one of the two people playing.
 * Anything else is corrupt and gets re-derived from the ids, which both ends
 * do identically with nothing exchanged.
 *
 * Hearts survived this only by accident: its deal alternates with
 * `(starter == me) ? them : me`, so a stranger's id falls through to `me`,
 * which at least is somebody real. */
static uint16_t starter_fix(uint16_t s, uint16_t a, uint16_t b)
{
    return (s == a || s == b) ? s : (a < b ? a : b);
}

/* 🔑 A SCORE NEEDS A REPAIR PATH LIKE EVERYTHING ELSE ON THIS RADIO.
 *
 * A lost board costs nothing because the next one replaces it, but a lost
 * WIN was gone forever: the handset that never saw the final frame simply
 * never counted that game, and the builder lost about four of them to this. So both
 * totals ride in every frame and merge element-wise by max. That is
 * idempotent and order-insensitive — a frame arriving twice, late, or after a
 * minute out of range all land in the same place — and it only converges
 * because each generation is counted at most once at each end. Keep the
 * per-generation guards; without them max() would ratchet a double count. */
static void score_merge(int *mine, const int *theirs)
{
    for (int i = 0; i < 2; i++) if (theirs[i] > mine[i]) mine[i] = theirs[i];
}

/* One result banner for every game. The status line is a single row of small
 * text at the bottom; a child holding the handset at arm's length is looking at
 * the board, and "who won" has to be readable from there without hunting.
 *
 * Never clickable: tapping the board is what starts the next game, and a
 * banner that swallowed that tap would read as a frozen device. */
static void banner_set(lv_obj_t **slot, lv_obj_t *scr, const char *text,
                       lv_color_t c)
{
    if (!text) {
        if (*slot) { lv_obj_del(*slot); *slot = NULL; }
        return;
    }
    if (!scr) return;
    if (!*slot) {
        *slot = rect(scr, 204, 52, C_PANEL);
        lv_obj_set_style_border_color(*slot, c, 0);
        lv_obj_set_style_border_width(*slot, 2, 0);
        lv_obj_set_style_radius(*slot, 6, 0);
        lv_obj_clear_flag(*slot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(*slot, LV_ALIGN_CENTER, 0, 0);
        lv_obj_t *l = lbl(*slot, &lv_font_montserrat_16, C_INK, text);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, 188);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
        lv_obj_move_foreground(*slot);
        return;
    }
    lv_obj_set_style_border_color(*slot, c, 0);
    lv_obj_t *l = lv_obj_get_child(*slot, 0);
    if (l) lv_label_set_text(l, text);
    lv_obj_move_foreground(*slot);
}

/* ------------------------------------------------------------------ *
 * tic-tac-toe
 *
 * The reason this has difficulty levels at all: tic-tac-toe is a solved game,
 * so a correct opponent NEVER loses. A kid who can only ever draw stops playing
 * on day two. Easy plays at random, Med blocks but does not look for forks, and
 * only Hard is the unbeatable minimax.
 * ------------------------------------------------------------------ */

#define TTT_CELL 70
#define TTT_GAP  4

static lv_obj_t *scr_ttt, *ttt_mark[9], *ttt_status, *ttt_score, *ttt_mbtn[5];
static char ttt_b[9];
static int  ttt_mode;                  /* 0 = 2 player, 1 = easy, 2 = med, 3 = hard */
static char ttt_turn;
static bool ttt_over;
static int  ttt_x_wins, ttt_o_wins;    /* session only, for 2-player */

/* ---- handset against handset ------------------------------------------
 *
 * Mode 4. The two boards agree on a very small contract and nothing else:
 * whoever taps NEW is X and moves first, the other is O. Every move sends the
 * WHOLE BOARD, so a lost frame costs a moment and not the game — out of range
 * of any router there is nobody to ask for a resend.
 *
 * ⚠️ THE PEER IS PICKED, NEVER ASSUMED. With three handsets and a fourth
 * coming, "peer 0" is a silent mis-delivery: a child would be playing someone
 * they did not choose while the person opposite waits. Same lesson the CYD
 * learned when it grew a picker. */
#define TTT_VS 4
/* Connect 4 lives further down the file but the network dispatcher, which sits
 * with the tic-tac-toe code, has to reach it. */
static void c4_apply(const espnow_game_t *g);
static void c4_set_role(void);
static void c4_heartbeat(void);
static bool c4_net_active(void);
static int  c4_peer;
/* Checkers sits further down still. Same four entry points as the rest. */
static void ck_apply(const espnow_game_t *g);
static void ck_set_role(void);
static void ck_heartbeat(void);
static bool ck_net_active(void);
static int  ck_peer;
/* Battleship and Hearts sit further down again, and the dispatcher has to be
 * able to reach all four. */
static void bs_apply(const espnow_game_t *g);
static void bs_set_role(void);
static void bs_heartbeat(void);
static bool bs_net_active(void);
static int  bs_peer;
static void hr_apply(const espnow_game_t *g);
static void hr_set_role(void);
static void hr_heartbeat(void);
static bool hr_net_active(void);
static int  hr_peer;
/* Uno needs no peer of its own — it carries a roster instead. */
static void uno_apply(const espnow_game_t *g);
static bool uno_net_active(void);

/* Defined below; the VS code sits above them so it can be read as one piece. */
static void ttt_paint(void);
static char ttt_winner(const char *b);
static void ttt_finish(char w);
static int  ttt_marks(const char *b);
static char ttt_turn_from_board(const char *b);
static void ttt_set_role(void);
static void ttt_next_starter(char w);
static void ttt_pick_default_peer(void);
static void ttt_vs_broadcast(bool is_new);
static void ttt_vs_status(void);
static int  ttt_peer = -1;             /* index into the ESP-NOW peer list */
static char ttt_me;                    /* 'X' or 'O' — which marks are mine */
static uint8_t ttt_seq;
static uint8_t ttt_gen;                /* which GAME; higher always wins */
static uint16_t ttt_starter;           /* device id that moves first this game */
static int64_t ttt_last_tx_us;
static lv_obj_t *ttt_peer_btn;
static lv_obj_t *ttt_banner;

static void ttt_vs_broadcast(bool is_new)
{
    if (ttt_peer < 0) return;
    if (!scr_ttt) return;
    espnow_game_t g = { 0 };
    g.game    = GAME_TTT;
    g.seq     = ++ttt_seq;
    g.flags   = is_new ? GF_NEW : 0;
    g.cells   = 9;
    g.gen     = ttt_gen;
    g.starter = ttt_starter;
    /* turn = 1 means "you move next", from the receiver's point of view. */
    g.turn  = (ttt_turn != ttt_me) ? 1 : 0;
    memcpy(g.board, ttt_b, 9);
    ttt_last_tx_us = esp_timer_get_time();
    applog("[ttt] TX to %s: marks %d turn %c me %c new %d",
           espnow_peer_name(ttt_peer) ? espnow_peer_name(ttt_peer) : "?",
           ttt_marks(ttt_b), ttt_turn, ttt_me, is_new);
    if (!espnow_game_send(ttt_peer, &g))
        lv_label_set_text(ttt_status, "send failed");
}

/* 🔑 CHOOSE SOMEBODY AUTOMATICALLY, THEN LET IT BE CHANGED.
 *
 * The first version opened VS with nobody selected and the words "pick a
 * handset", next to a small button most people would not read as a control.
 * The builder hit exactly that: "it just says pick a handset vs- on both". With one
 * other handset in range there is nothing to choose anyway, and with several
 * the first is as good a starting point as any — the button is then a change,
 * not a required step. A dead end is never the right opening state. */
/* 🚨 ROLES ARE DERIVED, NEVER NEGOTIATED. LOWER ID IS X AND MOVES FIRST.
 *
 * The first version said "whoever presses NEW is X" and had the far end demote
 * itself to O on receiving that. Both handsets enter VS by pressing the same
 * button, so both made themselves X and each waited for the other to give way:
 * The builder got "both x and no o". Auto-selecting the peer would only have moved the
 * bug — both would have sent NEW, both demoted, and both been O.
 *
 * Two numbers the two boards already agree on settle it with no messages at
 * all, no ordering, and nothing to lose in flight. */
/* 🚨 WHOSE TURN IT IS, FROM THE BOARD ALONE — NOT FROM THE FRAME.
 *
 * The first version shipped a `turn` field and each side believed what it was
 * told. Two handsets then managed to disagree: Kid 2 held 1 X and 2 O, Guest
 * held 1 X and 1 O, and BOTH said "not your turn" — a deadlock with no way out
 * but a restart. The builder hit it on the first real game.
 *
 * X always moves first, so equal counts means X is to play. That is derivable
 * by both sides from the one thing they share, and it cannot disagree with
 * itself. The `turn` field stays on the wire for readability but nothing reads
 * it any more.
 *
 * 🔑 THE SAME COUNT IS ALSO THE CLOCK. Marks are only ever ADDED, so a board
 * with fewer marks than the one on screen is older — which is the staleness
 * guard this design claimed to have and did not. A late or duplicated frame
 * used to be applied blindly and could hand a turn back to someone who had
 * already taken it. */
static int ttt_marks(const char *b)
{
    int n = 0;
    for (int i = 0; i < 9; i++) if (b[i]) n++;
    return n;
}

static char ttt_turn_from_board(const char *b)
{
    int xs = 0, os = 0;
    for (int i = 0; i < 9; i++) {
        if (b[i] == 'X') xs++;
        else if (b[i] == 'O') os++;
    }
    return (xs == os) ? 'X' : 'O';
}

static void ttt_set_role(void)
{
    if (ttt_peer < 0) { ttt_me = 'X'; return; }
    /* First game of a session: the lower id starts, purely so both sides pick
     * the same answer with nothing exchanged. After that ttt_starter is
     * whoever LOST, per the builder — the winner should not also get first move. */
    ttt_starter = starter_fix(ttt_starter, espnow_my_id(),
                              espnow_peer_id(ttt_peer));
    ttt_me = (espnow_my_id() == ttt_starter) ? 'X' : 'O';
}

/* Work out who starts the NEXT game, from the board that just finished. Both
 * sides run this, but the answer also travels in the frame, so a handset that
 * missed the last move cannot end up disagreeing about who is X. */
static void ttt_next_starter(char w)
{
    if (ttt_peer < 0) return;
    uint16_t me = espnow_my_id(), them = espnow_peer_id(ttt_peer);
    if (w == 'D') {
        /* A draw has no loser: alternate, so the same handset does not keep
         * the advantage of moving first. */
        ttt_starter = (ttt_starter == me) ? them : me;
    } else {
        /* The loser starts. */
        ttt_starter = (w == ttt_me) ? them : me;
    }
}

static void ttt_pick_default_peer(void)
{
    pick_default_peer(&ttt_peer);
}

static void ttt_vs_status(void)
{
    if (!ttt_status) return;            /* screen not built yet */
    if (ttt_peer < 0) {
        lv_label_set_text(ttt_status, "nobody in range");
        return;
    }
    if (ttt_over) return;
    const char *pn = espnow_peer_name(ttt_peer);
    lv_label_set_text_fmt(ttt_status, "%s  -  %c vs %s",
                          ttt_turn == ttt_me ? "Your turn" : "Their turn",
                          ttt_me, pn ? pn : "?");
}

/* Adopt a board that arrived. No merging and no cleverness: the sender's board
 * IS the game. */
static void ttt_vs_apply(const espnow_game_t *g)
{
    if (g->game != GAME_TTT || g->cells != 9) return;
    ttt_mode = TTT_VS;

    /* 🔑 GENERATION FIRST, MARKS SECOND. A new game legitimately has FEWER
     * marks than the one it replaces, so mark count alone cannot tell "stale"
     * from "next game" — which is how a lost reset used to leave one handset
     * on an empty board and the other on a finished one, both waiting. */
    if (g->gen < ttt_gen) {
        applog("[ttt] RX old game: gen %u < %u, dropped", g->gen, ttt_gen);
        return;
    }
    if (g->gen == ttt_gen &&
        ttt_marks((const char *)g->board) < ttt_marks(ttt_b)) {
        applog("[ttt] RX stale: theirs %d < mine %d, dropped",
               ttt_marks((const char *)g->board), ttt_marks(ttt_b));
        return;
    }
    ttt_gen     = g->gen;
    ttt_starter = (ttt_peer >= 0)
                  ? starter_fix(g->starter, espnow_my_id(), espnow_peer_id(ttt_peer))
                  : g->starter;

    memcpy(ttt_b, g->board, 9);
    ttt_seq  = g->seq;
    /* The frame's starter is authoritative — that is what it is for. */
    ttt_me   = (espnow_my_id() == ttt_starter) ? 'X' : 'O';
    ttt_turn = ttt_turn_from_board(ttt_b);
    ttt_over = false;
    banner_set(&ttt_banner, scr_ttt, NULL, C_INK);
    applog("[ttt] RX applied: marks %d turn %c me %c peer %s",
           ttt_marks(ttt_b), ttt_turn, ttt_me,
           espnow_peer_name(ttt_peer) ? espnow_peer_name(ttt_peer) : "?");
    ttt_paint();

    char w = ttt_winner(ttt_b);
    if (w) { ttt_next_starter(w); ttt_finish(w); }
    else   ttt_vs_status();
}

/* Polled from the games tick while the VS board is on screen. */
/* Live = the VS board is the current screen and an opponent is chosen. Not
 * merely "mode is VS": a handset left on the games menu must not keep the
 * radio locked for the rest of the evening. */
bool games_net_active(void)
{
    if (uno_net_active()) return true;
    if (tanks_net_active()) return true;
    if (c4_net_active()) return true;
    if (ck_net_active()) return true;
    if (bs_net_active()) return true;
    if (hr_net_active()) return true;
    return ttt_mode == TTT_VS && ttt_peer >= 0 &&
           scr_ttt && lv_scr_act() == scr_ttt;
}

/* 🚨 A LOST FRAME NEEDS A REPAIR PATH, AND "THE NEXT BOARD FIXES IT" IS NOT ONE.
 *
 * The design says a dropped board costs nothing because the next one repairs
 * it — true only while somebody is still moving. When the lost frame is the one
 * that would have told the other side it is THEIR turn, both sides sit waiting
 * and no next board is ever sent. The builder hit it repeatedly: Kid 2 on an empty
 * board waiting for X, Guest on a five-mark board waiting for O, forever.
 *
 * So re-announce the board every couple of seconds while the game is on screen
 * and unfinished. It is 69 bytes, it is idempotent, and it turns every
 * desynchronisation in this protocol — lost move, lost reset, a handset that
 * walked out of range and came back — into something that heals itself within
 * about two seconds without anyone noticing. */
static void ttt_vs_heartbeat(void)
{
    if (!games_net_active() || ttt_over) return;
    if (esp_timer_get_time() - ttt_last_tx_us < 2000000) return;
    ttt_vs_broadcast(ttt_marks(ttt_b) == 0);
}

void games_poll_net(void)
{
    ttt_vs_heartbeat();
    c4_heartbeat();
    ck_heartbeat();
    bs_heartbeat();
    hr_heartbeat();
    tanks_heartbeat();      /* 🚨 the one that was missing — see tanks.h */

    espnow_game_t g;
    char from[10];
    int  idx = -1;
    if (!espnow_game_take(&g, from, sizeof(from), &idx)) return;

    /* 🚨 PLAY WHOEVER IS ACTUALLY SENDING BOARDS.
     *
     * Auto-picking "peer 0" chose whichever handset sat first in the list, and
     * a third handset left powered on a shelf is still a peer. The builder played
     * Guest against Kid 2 with Kid 1 idle in the room: both were unicasting
     * their moves at Kid 1, so each side sat waiting for a move the other had
     * already made and neither could take a turn.
     *
     * The first board that arrives settles it, in both directions, with no
     * pairing step for anyone to get wrong. */
    /* Route by game id. Two handsets can be on different screens — one still
     * on the Connect 4 board while the other has gone back to tic-tac-toe —
     * and a frame must never be applied to the wrong game's state. */
    if (g.game == GAME_C4) {
        if (idx >= 0) { c4_peer = idx; c4_set_role(); }
        c4_apply(&g);
        return;
    }
    if (g.game == GAME_CHECKERS) {
        if (idx >= 0) { ck_peer = idx; ck_set_role(); }
        ck_apply(&g);
        return;
    }
    if (g.game == GAME_BATTLE) {
        if (idx >= 0) { bs_peer = idx; bs_set_role(); }
        bs_apply(&g);
        return;
    }
    if (g.game == GAME_TANKS) {
        /* Tanks keeps its own peer and role — every board carries the whole
         * game, so there is nothing for the dispatcher to reconcile. */
        tanks_net_take(&g, idx);
        return;
    }
    if (g.game == GAME_HEARTS) {
        if (idx >= 0) { hr_peer = idx; hr_set_role(); }
        hr_apply(&g);
        return;
    }
    /* Uno carries its own roster, so unlike the two-player games there is no
     * peer to latch here — the frame says who is playing and the seats fall out
     * of that. A handset that was out of range when the deal went out picks the
     * game up from the next frame it sees. */
    if (g.game == GAME_UNO) { uno_apply(&g); return; }
    if (g.game != GAME_TTT) return;

    applog("[ttt] RX from %s (idx %d, was %d)", from, idx, ttt_peer);
    if (idx >= 0 && idx != ttt_peer) {
        ttt_peer = idx;
        ttt_set_role();
        applog("[ttt] paired with %s, I am %c (my id %04X, theirs %04X)",
               from, ttt_me, espnow_my_id(), espnow_peer_id(ttt_peer));
    }
    if (!scr_ttt || lv_scr_act() != scr_ttt) {
        /* A board arrived while we are elsewhere. Keep it: opening the screen
         * shows the game rather than an empty grid. */
        ttt_mode = TTT_VS;
    }
    ttt_vs_apply(&g);
}

static const int TTT_LINES[8][3] = {
    {0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}
};

static char ttt_winner(const char *b)
{
    for (int i = 0; i < 8; i++) {
        char a = b[TTT_LINES[i][0]];
        if (a && a == b[TTT_LINES[i][1]] && a == b[TTT_LINES[i][2]]) return a;
    }
    for (int i = 0; i < 9; i++) if (!b[i]) return 0;
    return 'D';                         /* draw */
}

static int ttt_minimax(char *b, char turn, int depth)
{
    char w = ttt_winner(b);
    if (w == 'O') return 10 - depth;    /* the machine is O */
    if (w == 'X') return depth - 10;
    if (w == 'D') return 0;

    int best = (turn == 'O') ? -100 : 100;
    for (int i = 0; i < 9; i++) {
        if (b[i]) continue;
        b[i] = turn;
        int s = ttt_minimax(b, turn == 'O' ? 'X' : 'O', depth + 1);
        b[i] = 0;
        if (turn == 'O') { if (s > best) best = s; }
        else             { if (s < best) best = s; }
    }
    return best;
}

/* Returns the cell the machine wants, or -1 if the board is full. */
static int ttt_ai_pick(void)
{
    int empty[9], n = 0;
    for (int i = 0; i < 9; i++) if (!ttt_b[i]) empty[n++] = i;
    if (!n) return -1;

    if (ttt_mode == 1) return empty[rnd(n)];

    if (ttt_mode == 2) {
        /* win if you can, block if you must, otherwise anywhere. Beatable by
         * a fork, which is exactly the point. */
        for (int pass = 0; pass < 2; pass++) {
            char who = pass == 0 ? 'O' : 'X';
            for (int i = 0; i < 9; i++) {
                if (ttt_b[i]) continue;
                ttt_b[i] = who;
                char w = ttt_winner(ttt_b);
                ttt_b[i] = 0;
                if (w == who) return i;
            }
        }
        return empty[rnd(n)];
    }

    int best = -1000, pick = empty[0];
    for (int i = 0; i < 9; i++) {
        if (ttt_b[i]) continue;
        ttt_b[i] = 'O';
        int s = ttt_minimax(ttt_b, 'X', 0);
        ttt_b[i] = 0;
        if (s > best) { best = s; pick = i; }
    }
    return pick;
}

static void ttt_paint(void)
{
    /* Same trap as c4_paint(): a board can arrive from the other handset before
     * this screen has ever been built, and ttt_mark[] would be NULL. */
    if (!scr_ttt) return;
    for (int i = 0; i < 9; i++) {
        lv_label_set_text(ttt_mark[i], ttt_b[i] == 'X' ? "X" :
                                       ttt_b[i] == 'O' ? "O" : "");
        lv_obj_set_style_text_color(ttt_mark[i],
                                    ttt_b[i] == 'X' ? C_AMBER : C_BLUE, 0);
    }
    for (int m = 0; m < 5; m++)
        lv_obj_set_style_bg_color(ttt_mbtn[m], m == ttt_mode ? C_BLUE : C_PANEL, 0);
    if (ttt_peer_btn) {
        lv_obj_t *pl = lv_obj_get_child(ttt_peer_btn, 0);
        const char *pn = (ttt_peer >= 0) ? espnow_peer_name(ttt_peer) : NULL;
        if (pl) lv_label_set_text_fmt(pl, "vs: %s", pn ? pn : "-");
        if (ttt_mode == TTT_VS) lv_obj_clear_flag(ttt_peer_btn, LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_add_flag(ttt_peer_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (ttt_mode == 0)
        lv_label_set_text_fmt(ttt_score, "X %d  O %d", ttt_x_wins, ttt_o_wins);
    else
        lv_label_set_text_fmt(ttt_score, "Wins %d", hs_get("ttt_w"));
}

static void ttt_reset(void)
{
    memset(ttt_b, 0, sizeof(ttt_b));
    ttt_turn = 'X';
    ttt_over = false;
    banner_set(&ttt_banner, scr_ttt, NULL, C_INK);
    if (ttt_mode == TTT_VS) {
        ttt_pick_default_peer();
        ttt_gen++;                      /* a new game outranks the old one */
        ttt_set_role();
        ttt_turn = 'X';                 /* empty board: X starts, both agree */
        ttt_paint();
        ttt_vs_broadcast(true);
        ttt_vs_status();
        return;
    }
    lv_label_set_text(ttt_status, ttt_mode == 0 ? "X's turn" : "Your turn");
    ttt_paint();
}

static void ttt_finish(char w)
{
    ttt_over = true;
    if (!ttt_status) return;
    if (w == 'D') {
        lv_label_set_text(ttt_status, "Draw - tap to play again");
        banner_set(&ttt_banner, scr_ttt, "A DRAW\ntap to play again", C_AMBER);
        return;
    }
    /* Who that is depends on the mode: two children on one handset, the
     * machine, or the handset across the room. */
    bool mine = (ttt_mode == TTT_VS) ? (w == ttt_me) : (w == 'X');
    const char *they = (ttt_mode == TTT_VS && ttt_peer >= 0)
                       ? espnow_peer_name(ttt_peer) : "THE MACHINE";
    char t[48];
    if (ttt_mode == 0) {
        if (w == 'X') ttt_x_wins++; else ttt_o_wins++;
        lv_label_set_text_fmt(ttt_status, "%c wins - tap to play again", w);
        snprintf(t, sizeof(t), "%c WINS\ntap to play again", w);
        banner_set(&ttt_banner, scr_ttt, t, C_BLUE);
    } else if (mine) {
        if (ttt_mode != TTT_VS) hs_set("ttt_w", hs_get("ttt_w") + 1);
        lv_label_set_text(ttt_status, "You win! - tap to play again");
        banner_set(&ttt_banner, scr_ttt, "YOU WIN\ntap to play again", C_GREEN);
    } else {
        lv_label_set_text(ttt_status, "I win - tap to play again");
        snprintf(t, sizeof(t), "%s WINS\ntap to play again", they);
        banner_set(&ttt_banner, scr_ttt, t, C_RED);
    }
    ttt_paint();
}

static void ttt_cell_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    /* A finished board turns the whole grid into "play again" - a kid should
     * not have to find a separate button. */
    if (ttt_over) { ttt_reset(); return; }
    if (ttt_b[i]) return;

    if (ttt_mode == TTT_VS) {
        ttt_pick_default_peer();
        if (ttt_peer < 0) { lv_label_set_text(ttt_status, "nobody in range"); return; }
        /* ⚠️ Refuse a move out of turn rather than sending it and letting the
         * far end reject it. Two boards that disagree about whose turn it is
         * is the one state this design cannot repair itself out of. */
        if (ttt_turn != ttt_me) {
            applog("[ttt] refused: turn %c me %c marks %d",
                   ttt_turn, ttt_me, ttt_marks(ttt_b));
            lv_label_set_text(ttt_status, "not your turn");
            return;
        }
        ttt_b[i] = ttt_me;
        ttt_turn = ttt_turn_from_board(ttt_b);
        ttt_paint();
        ttt_vs_broadcast(false);
        char w = ttt_winner(ttt_b);
        if (w) { ttt_next_starter(w); ttt_finish(w); } else ttt_vs_status();
        return;
    }

    ttt_b[i] = ttt_turn;
    char w = ttt_winner(ttt_b);
    if (w) { ttt_paint(); ttt_finish(w); return; }

    if (ttt_mode == 0) {
        ttt_turn = (ttt_turn == 'X') ? 'O' : 'X';
        lv_label_set_text_fmt(ttt_status, "%c's turn", ttt_turn);
        ttt_paint();
        return;
    }

    int m = ttt_ai_pick();
    if (m >= 0) ttt_b[m] = 'O';
    ttt_paint();
    w = ttt_winner(ttt_b);
    if (w) ttt_finish(w);
}

static void ttt_peer_clicked(lv_event_t *e)
{
    (void)e;
    ttt_peer = play_next(ttt_peer);
    if (ttt_peer < 0) lv_label_set_text(ttt_status, "nobody in range");
    ttt_set_role();          /* a different opponent can mean a different mark */
    ttt_paint();
    ttt_vs_status();
}

static void ttt_mode_clicked(lv_event_t *e)
{
    ttt_mode = (int)(intptr_t)lv_event_get_user_data(e);
    ttt_x_wins = ttt_o_wins = 0;
    ttt_reset();
}

static void ttt_back(lv_event_t *e)
{
    tmr_stop();
    if (!scr_menu_ready()) return;
    ttt_banner = NULL;
    leave_to(scr_menu, &scr_ttt);
}

static void ttt_build(void)
{
    ttt_banner = NULL;
    scr_ttt = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_ttt, C_BG, 0);
    lv_obj_clear_flag(scr_ttt, LV_OBJ_FLAG_SCROLLABLE);
    ttt_score = mk_hdr(scr_ttt, "Tic-Tac-Toe", ttt_back);

    static const char *names[5] = { "2P", "Easy", "Med", "Hard", "VS" };
    for (int m = 0; m < 5; m++) {
        ttt_mbtn[m] = btn(scr_ttt, 44, 24, C_PANEL, C_INK, &lv_font_montserrat_12,
                          names[m], ttt_mode_clicked, (void *)(intptr_t)m);
        lv_obj_align(ttt_mbtn[m], LV_ALIGN_TOP_LEFT, 6 + m * 46, HDR_H + 6);
    }

    /* Who you are playing. Tapping cycles the handsets in range — a picker
     * rather than a guess, because "peer 0" with four boards delivers a game
     * to whoever happens to be first in the list. */
    ttt_peer_btn = btn(scr_ttt, 100, 22, C_PANEL, C_BLUE, &lv_font_montserrat_12,
                       "vs: -", ttt_peer_clicked, NULL);
    lv_obj_align(ttt_peer_btn, LV_ALIGN_TOP_RIGHT, -8, HDR_H + 34);

    ttt_status = lbl(scr_ttt, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_align(ttt_status, LV_ALIGN_TOP_MID, 0, HDR_H + 38);

    int x0 = (LCD_W - (3 * TTT_CELL + 2 * TTT_GAP)) / 2;
    int y0 = HDR_H + 58;
    for (int i = 0; i < 9; i++) {
        int cx = x0 + (i % 3) * (TTT_CELL + TTT_GAP);
        int cy = y0 + (i / 3) * (TTT_CELL + TTT_GAP);
        lv_obj_t *c = rect(scr_ttt, TTT_CELL, TTT_CELL, C_PANEL);
        lv_obj_set_pos(c, cx, cy);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, ttt_cell_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        ttt_mark[i] = lbl(c, &lv_font_montserrat_40, C_AMBER, "");
        lv_obj_center(ttt_mark[i]);
    }
}

static void ttt_show(lv_event_t *e)
{
    if (!scr_ttt) ttt_build();
    tmr_stop();                 /* tic-tac-toe is event driven - no tick at all */
    ttt_reset();
    lv_scr_load(scr_ttt);
    applog("[games] tic-tac-toe, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}


/* ------------------------------------------------------------------ *
 * Connect 4 — handset against handset
 *
 * The classic rack: a blue frame, seven columns, discs falling to the lowest
 * free slot. Red moves first, black second.
 *
 * Everything network here is the same shape tic-tac-toe arrived at, and for
 * the same reasons — the whole board travels, roles come from a carried
 * `starter` rather than a negotiation, whose turn it is is DERIVED from the
 * discs on the board, a generation counter separates "next game" from "stale",
 * and an unfinished board is re-announced every couple of seconds so a lost
 * frame heals itself. Each of those replaced a bug found on hardware; none of
 * them are worth rediscovering here.
 * ------------------------------------------------------------------ */

#define C4_COLS 7
#define C4_ROWS 6
#define C4_N    (C4_COLS * C4_ROWS)     /* 42, inside GAME_BOARD_MAX */
#define C4_CELL 30
#define C4_GAP  2

static lv_obj_t *scr_c4, *c4_hole[C4_N], *c4_status, *c4_peer_btn, *c4_rack;
/* 0 empty, 'R' red, 'Y' yellow.
 *
 * ⚠️ YELLOW, NOT BLACK. The first version used black discs, which is a real
 * Connect 4 colour scheme and unreadable here: a near-black disc on a dark
 * screen inside a blue rack is almost invisible at arm's length. The builder called it
 * immediately. Red and yellow is the other classic pairing and it is the one
 * that survives a small panel. */
static char      c4_b[C4_N];
static char      c4_me, c4_turn;
static lv_obj_t *c4_banner;
static bool      c4_over;
/* 0 = two players on this handset, 1-3 = the machine, 4 = the other handset.
 *
 * The builder, 2026-08-25: "on the connect 4 you should have the vs and easy - hard
 * options like on tic tac toe". Connect 4 shipped as handset-against-handset
 * only, which makes it the one game on the menu that does nothing at all when
 * there is nobody else in range - on a device whose whole point is a car ride.
 * Same five modes, same row of buttons, same place on the screen. */
#define C4_VS 4
static int       c4_mode;
static lv_obj_t *c4_mbtn[5], *c4_score_lbl;
static int       c4_peer = -1;   /* declared above for the dispatcher */
static uint8_t   c4_gen;
static uint16_t  c4_starter;
static int64_t   c4_last_tx_us;
/* Against another handset this is indexed by DEVICE ID, like Hearts and
 * Battleship: c4_me flips with the starter, so a total indexed by colour is a
 * total that changes owner between games. In the local modes there is no second
 * device and colour IS the identity, so 0 = Red and 1 = Yellow there. */
static int       c4_wins[2];
static int       c4_won_gen = -1;
static int       c4_my_slot(void);

#define C4_AT(r, c) c4_b[(r) * C4_COLS + (c)]

static int c4_discs(const char *b)
{
    int n = 0;
    for (int i = 0; i < C4_N; i++) if (b[i]) n++;
    return n;
}

/* Red always moves first, so equal counts means red is to play. Same
 * derivation tic-tac-toe uses, and for the same reason: two handsets cannot
 * disagree about something neither of them is told. */
static char c4_turn_from_board(const char *b)
{
    int r = 0, k = 0;
    for (int i = 0; i < C4_N; i++) {
        if (b[i] == 'R') r++;
        else if (b[i] == 'Y') k++;
    }
    return (r == k) ? 'R' : 'Y';
}

/* Four in a line, any direction. Walks from every cell in four directions
 * only — right, down, and the two diagonals — because the reverse of each is
 * covered by starting from the other end. */
static char c4_winner(const char *b)
{
    static const int DR[4] = { 0, 1, 1, 1 };
    static const int DC[4] = { 1, 0, 1, -1 };
    for (int r = 0; r < C4_ROWS; r++) {
        for (int c = 0; c < C4_COLS; c++) {
            char a = b[r * C4_COLS + c];
            if (!a) continue;
            for (int d = 0; d < 4; d++) {
                int rr = r + DR[d] * 3, cc = c + DC[d] * 3;
                if (rr < 0 || rr >= C4_ROWS || cc < 0 || cc >= C4_COLS) continue;
                int k = 1;
                while (k < 4 &&
                       b[(r + DR[d] * k) * C4_COLS + (c + DC[d] * k)] == a) k++;
                if (k == 4) return a;
            }
        }
    }
    for (int i = 0; i < C4_N; i++) if (!b[i]) return 0;
    return 'D';
}

/* Where a disc dropped into this column would land, or -1 if it is full. */
static int c4_drop_row(const char *b, int c)
{
    for (int r = C4_ROWS - 1; r >= 0; r--) if (!b[r * C4_COLS + c]) return r;
    return -1;
}

/* Every four-in-a-row window on the board, scored from yellow's point of view.
 * Three of yours with the fourth square empty is worth far more than two, and a
 * window with both colours in it is worth nothing to anybody — which is the
 * whole of Connect 4 strategy in one loop. */
static int c4_shape(const char *b)
{
    static const int DR[4] = { 0, 1, 1, 1 };
    static const int DC[4] = { 1, 0, 1, -1 };
    int score = 0;
    for (int r = 0; r < C4_ROWS; r++) {
        for (int c = 0; c < C4_COLS; c++) {
            for (int d = 0; d < 4; d++) {
                int rr = r + DR[d] * 3, cc = c + DC[d] * 3;
                if (rr < 0 || rr >= C4_ROWS || cc < 0 || cc >= C4_COLS) continue;
                int y = 0, x = 0;
                for (int k = 0; k < 4; k++) {
                    char a = b[(r + DR[d] * k) * C4_COLS + (c + DC[d] * k)];
                    if (a == 'Y') y++;
                    else if (a == 'R') x++;
                }
                if (y && x) continue;              /* blocked, worth nothing */
                if (y) score += (y == 3) ? 60 : (y == 2) ? 12 : 1;
                if (x) score -= (x == 3) ? 60 : (x == 2) ? 12 : 1;
            }
        }
    }
    /* The centre column is on more lines than any other, so owning it is worth
     * something on its own. */
    for (int r = 0; r < C4_ROWS; r++) {
        char a = C4_AT(r, C4_COLS / 2);
        if (a == 'Y') score += 6; else if (a == 'R') score -= 6;
    }
    return score;
}

/* Yellow maximises. Alpha-beta with the centre searched first, which is what
 * keeps the cut-offs cheap enough to run on the UI task: the decoder has its
 * own task and its own priority, so a few milliseconds here cannot starve it —
 * but tens of milliseconds would still show up as a stutter in the buttons. */
static int c4_search(char *b, char turn, int depth, int alpha, int beta)
{
    char w = c4_winner(b);
    if (w == 'Y') return  10000 + depth;    /* +depth: win SOONER, not later */
    if (w == 'R') return -10000 - depth;
    if (w == 'D' || depth == 0) return c4_shape(b);

    static const int ORDER[C4_COLS] = { 3, 2, 4, 1, 5, 0, 6 };
    int best = (turn == 'Y') ? -30000 : 30000;
    for (int k = 0; k < C4_COLS; k++) {
        int c = ORDER[k], r = c4_drop_row(b, c);
        if (r < 0) continue;
        b[r * C4_COLS + c] = turn;
        int sc = c4_search(b, turn == 'Y' ? 'R' : 'Y', depth - 1, alpha, beta);
        b[r * C4_COLS + c] = 0;
        if (turn == 'Y') {
            if (sc > best) best = sc;
            if (best > alpha) alpha = best;
        } else {
            if (sc < best) best = sc;
            if (best < beta) beta = best;
        }
        if (alpha >= beta) break;
    }
    return best;
}

/* The column the machine wants, or -1 if the board is full.
 *
 * The same ladder tic-tac-toe uses, and for the same reason: a machine that
 * never loses is a machine nobody plays twice. Easy is genuinely random, Med
 * only sees one move ahead, and Hard is the search. */
static int c4_ai_pick(void)
{
    int legal[C4_COLS], n = 0;
    for (int c = 0; c < C4_COLS; c++)
        if (c4_drop_row(c4_b, c) >= 0) legal[n++] = c;
    if (!n) return -1;

    if (c4_mode == 1) return legal[rnd(n)];

    if (c4_mode == 2) {
        /* Take the win, then stop the loss. Blind to anything set up two moves
         * out, which is exactly what makes it beatable. */
        for (int pass = 0; pass < 2; pass++) {
            char who = pass == 0 ? 'Y' : 'R';
            for (int i = 0; i < n; i++) {
                int c = legal[i], r = c4_drop_row(c4_b, c);
                c4_b[r * C4_COLS + c] = who;
                char w = c4_winner(c4_b);
                c4_b[r * C4_COLS + c] = 0;
                if (w == who) return c;
            }
        }
        return legal[rnd(n)];
    }

    int best = -30000, pick = legal[0];
    static const int ORDER[C4_COLS] = { 3, 2, 4, 1, 5, 0, 6 };
    for (int k = 0; k < C4_COLS; k++) {
        int c = ORDER[k], r = c4_drop_row(c4_b, c);
        if (r < 0) continue;
        c4_b[r * C4_COLS + c] = 'Y';
        int sc = c4_search(c4_b, 'R', 5, -30000, 30000);
        c4_b[r * C4_COLS + c] = 0;
        if (sc > best) { best = sc; pick = c; }
    }
    return pick;
}

static void c4_paint(void)
{
    /* 🚨 A BOARD CAN ARRIVE BEFORE THIS SCREEN HAS EVER BEEN BUILT.
     *
     * games_poll_net() runs on the UI tick no matter what the player is
     * looking at, so the far handset can push a game at a board whose screen
     * objects do not exist yet — c4_hole[] is all NULL and painting it is a
     * LoadProhibited reboot. Guest boot-looped every time Kid 2 opened Connect
     * 4; the backtrace was c4_paint -> lv_obj_set_style_bg_color.
     *
     * The STATE is still updated by the caller, so opening the screen later
     * shows the game in progress rather than an empty rack. */
    if (!scr_c4) return;
    for (int i = 0; i < C4_N; i++) {
        lv_color_t col = c4_b[i] == 'R' ? lv_color_hex(0xE03B3B) :
                         c4_b[i] == 'Y' ? lv_color_hex(0xF2C21A) : C_BG;
        lv_obj_set_style_bg_color(c4_hole[i], col, 0);
    }
    for (int m = 0; m < 5; m++)
        if (c4_mbtn[m])
            lv_obj_set_style_bg_color(c4_mbtn[m], m == c4_mode ? C_BLUE : C_PANEL, 0);

    if (c4_peer_btn) {
        lv_obj_t *pl = lv_obj_get_child(c4_peer_btn, 0);
        const char *pn = (c4_peer >= 0) ? espnow_peer_name(c4_peer) : NULL;
        if (pl) lv_label_set_text_fmt(pl, "vs: %s", pn ? pn : "-");
        /* Only meaningful against another handset, and a control that does
         * nothing is worse than no control. */
        if (c4_mode == C4_VS) lv_obj_clear_flag(c4_peer_btn, LV_OBJ_FLAG_HIDDEN);
        else                  lv_obj_add_flag(c4_peer_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (c4_score_lbl) {
        if (c4_mode == 0)
            lv_label_set_text_fmt(c4_score_lbl, "R %d  Y %d", c4_wins[0], c4_wins[1]);
        else if (c4_mode == C4_VS)
            lv_label_set_text_fmt(c4_score_lbl, "%d-%d",
                                  c4_wins[c4_my_slot()], c4_wins[1 - c4_my_slot()]);
        else
            lv_label_set_text_fmt(c4_score_lbl, "Wins %d", hs_get("c4_w"));
    }
}

static void c4_set_role(void)
{
    if (c4_peer < 0) { c4_me = 'R'; return; }
    c4_starter = starter_fix(c4_starter, espnow_my_id(), espnow_peer_id(c4_peer));
    c4_me = (espnow_my_id() == c4_starter) ? 'R' : 'Y';
}

static void c4_next_starter(char w)
{
    if (c4_peer < 0) return;
    uint16_t me = espnow_my_id(), them = espnow_peer_id(c4_peer);
    if (w == 'D') c4_starter = (c4_starter == me) ? them : me;
    else          c4_starter = (w == c4_me) ? them : me;
}

static void c4_pick_default_peer(void)
{
    pick_default_peer(&c4_peer);
}

static void c4_status_set(void)
{
    if (!c4_status) return;             /* screen not built yet - see c4_paint() */
    if (c4_over) return;
    if (c4_mode != C4_VS) {
        lv_label_set_text(c4_status,
                          c4_mode == 0 ? (c4_turn == 'R' ? "Red's turn" : "Yellow's turn")
                                       : (c4_turn == 'R' ? "Your turn" : "thinking..."));
        return;
    }
    if (c4_peer < 0) { lv_label_set_text(c4_status, "nobody in range"); return; }
    const char *pn = espnow_peer_name(c4_peer);
    lv_label_set_text_fmt(c4_status, "%s  -  %s vs %s",
                          c4_turn == c4_me ? "Your turn" : "Their turn",
                          c4_me == 'R' ? "red" : "yellow", pn ? pn : "?");
}

static void c4_broadcast(bool is_new)
{
    if (c4_mode != C4_VS) return;       /* a local game is nobody else's business */
    if (c4_peer < 0) return;
    if (!scr_c4) return;                /* nothing to send about */
    espnow_game_t g = { 0 };
    g.game    = GAME_C4;
    g.flags   = is_new ? GF_NEW : 0;
    g.cells   = C4_N;
    g.gen     = c4_gen;
    g.starter = c4_starter;
    memcpy(g.board, c4_b, C4_N);
    /* 42 cells of a 64-byte board, so everything from 42 up is free. */
    g.board[42] = (uint8_t)c4_wins[0];
    g.board[43] = (uint8_t)(c4_wins[0] >> 8);
    g.board[44] = (uint8_t)c4_wins[1];
    g.board[45] = (uint8_t)(c4_wins[1] >> 8);
    c4_last_tx_us = esp_timer_get_time();
    if (!espnow_game_send(c4_peer, &g))
        lv_label_set_text(c4_status, "send failed");
}

static int c4_my_slot(void)
{
    if (c4_mode != C4_VS || c4_peer < 0) return 0;
    return id_slot(espnow_my_id(), espnow_peer_id(c4_peer));
}

static void c4_finish(char w)
{
    c4_over = true;
    if (!c4_status) return;
    if (w == 'D') {
        lv_label_set_text(c4_status, "Draw - tap to play again");
        banner_set(&c4_banner, scr_c4, "A DRAW\ntap to play again", C_AMBER);
        c4_paint();
        return;
    }
    {
        bool mine = (c4_mode == C4_VS) ? (w == c4_me) : (w == 'R');
        const char *they = (c4_mode == C4_VS && c4_peer >= 0)
                           ? espnow_peer_name(c4_peer) : "THE MACHINE";
        char t[48];
        if (c4_mode == 0) {
            snprintf(t, sizeof(t), "%s WINS\ntap to play again",
                     w == 'R' ? "RED" : "YELLOW");
            banner_set(&c4_banner, scr_c4, t, C_BLUE);
        } else if (mine) {
            banner_set(&c4_banner, scr_c4, "YOU WIN\ntap to play again", C_GREEN);
        } else {
            snprintf(t, sizeof(t), "%s WINS\ntap to play again", they);
            banner_set(&c4_banner, scr_c4, t, C_RED);
        }
    }
    if (c4_mode != C4_VS) {
        if (w == 'R') c4_wins[0]++; else c4_wins[1]++;
    } else if (c4_won_gen != (int)c4_gen) {
        c4_won_gen = (int)c4_gen;       /* once per game, whichever end saw it */
        int mine = c4_my_slot();
        c4_wins[(w == c4_me) ? mine : 1 - mine]++;
    }
    if (c4_mode == 0)
        lv_label_set_text_fmt(c4_status, "%s wins - tap to play again",
                              w == 'R' ? "Red" : "Yellow");
    else if (c4_mode == C4_VS)
        lv_label_set_text_fmt(c4_status, "%s wins - tap to play again",
                              w == c4_me ? "You" : "They");
    else if (w == 'R') {
        hs_set("c4_w", hs_get("c4_w") + 1);
        lv_label_set_text(c4_status, "You win! - tap to play again");
    } else {
        lv_label_set_text(c4_status, "I win - tap to play again");
    }
    c4_paint();
}

static void c4_reset(void)
{
    memset(c4_b, 0, sizeof(c4_b));
    c4_over = false;
    banner_set(&c4_banner, scr_c4, NULL, C_INK);
    c4_turn = 'R';                      /* red always opens */
    if (c4_mode == C4_VS) {
        c4_pick_default_peer();
        c4_gen++;                       /* a new game outranks the old one */
        c4_set_role();
        c4_paint();
        c4_broadcast(true);
        c4_status_set();
        return;
    }
    /* Locally the player is red and moves first, so the machine never opens on
     * a board the player has not seen. */
    c4_me = 'R';
    if (c4_status)
        lv_label_set_text(c4_status, c4_mode == 0 ? "Red's turn" : "Your turn");
    c4_paint();
}

static void c4_apply(const espnow_game_t *g)
{
    if (g->game != GAME_C4 || g->cells != C4_N) return;
    c4_mode = C4_VS;                    /* somebody is playing us - same as ttt */
    if (g->gen < c4_gen) return;
    if (g->gen == c4_gen && c4_discs((const char *)g->board) < c4_discs(c4_b)) return;
    c4_gen     = g->gen;
    c4_starter = (c4_peer >= 0)
                 ? starter_fix(g->starter, espnow_my_id(), espnow_peer_id(c4_peer))
                 : g->starter;
    memcpy(c4_b, g->board, C4_N);
    int theirs[2] = { g->board[42] | (g->board[43] << 8),
                      g->board[44] | (g->board[45] << 8) };
    score_merge(c4_wins, theirs);
    c4_me   = (espnow_my_id() == c4_starter) ? 'R' : 'Y';
    c4_turn = c4_turn_from_board(c4_b);
    c4_over = false;
    c4_paint();
    char w = c4_winner(c4_b);
    if (w) { c4_next_starter(w); c4_finish(w); }
    else   c4_status_set();
}

/* A tap anywhere in a column drops there — aiming at an individual hole on a
 * 30px grid is not something small fingers should have to do. */
/* The machine's reply. Guarded on every side: leaving the screen, going back to
 * the menu or switching mode mid-think must not drop a disc on a board nobody
 * is looking at. */
static void c4_ai_tick(lv_timer_t *t)
{
    (void)t;
    tmr_stop();
    if (c4_mode == 0 || c4_mode == C4_VS || c4_over) return;
    if (!scr_c4 || lv_scr_act() != scr_c4) return;
    if (c4_turn != 'Y') return;

    int m = c4_ai_pick();
    if (m >= 0) {
        int r = c4_drop_row(c4_b, m);
        if (r >= 0) c4_b[r * C4_COLS + m] = 'Y';
    }
    c4_turn = c4_turn_from_board(c4_b);
    c4_paint();
    char w = c4_winner(c4_b);
    if (w) c4_finish(w);
    else   c4_status_set();
}

static void c4_col_clicked(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);

    if (c4_over) { c4_reset(); return; }

    int row = c4_drop_row(c4_b, c);
    if (row < 0) { lv_label_set_text(c4_status, "column is full"); return; }

    if (c4_mode == C4_VS) {
        c4_pick_default_peer();
        if (c4_peer < 0) { lv_label_set_text(c4_status, "nobody in range"); return; }
        c4_set_role();
        if (c4_turn != c4_me) { lv_label_set_text(c4_status, "not your turn"); return; }
        C4_AT(row, c) = c4_me;
        c4_turn = c4_turn_from_board(c4_b);
        c4_paint();
        c4_broadcast(false);
        char w = c4_winner(c4_b);
        if (w) { c4_next_starter(w); c4_finish(w); }
        else   c4_status_set();
        return;
    }

    /* Two players on one handset: the colour to play comes from the board, the
     * same derivation the network game uses. */
    C4_AT(row, c) = c4_turn;
    char w = c4_winner(c4_b);
    if (w) { c4_paint(); c4_finish(w); return; }
    c4_turn = c4_turn_from_board(c4_b);

    if (c4_mode == 0) { c4_paint(); c4_status_set(); return; }

    /* 🔑 THE MACHINE MOVES ON A TIMER, NOT INSIDE THE TOUCH HANDLER.
     *
     * Hard mode is an alpha-beta search. Run inline it would hold the event
     * handler for as long as it takes, so the player's own disc would not be
     * drawn until the reply was already decided — the handset would look like
     * it had ignored the tap and then dropped two discs at once. Painting and
     * returning lets the normal refresh put the disc on screen, and the reply
     * lands on the next tick. The pause is honest: the status line says
     * "thinking...". */
    c4_paint();
    c4_status_set();
    tmr_start(c4_ai_tick, 120);
}

static void c4_mode_clicked(lv_event_t *e)
{
    c4_mode = (int)(intptr_t)lv_event_get_user_data(e);
    c4_wins[0] = c4_wins[1] = 0;
    c4_won_gen = -1;
    c4_reset();
}

static void c4_peer_clicked(lv_event_t *e)
{
    (void)e;
    c4_peer = play_next(c4_peer);
    if (c4_peer < 0) lv_label_set_text(c4_status, "nobody in range");
    c4_set_role();
    c4_paint();
    c4_status_set();
}

static void c4_back(lv_event_t *e)
{
    (void)e;
    tmr_stop();
    if (!scr_menu_ready()) return;
    c4_banner = NULL;
    leave_to(scr_menu, &scr_c4);
}

static void c4_build(void)
{
    c4_banner = NULL;
    scr_c4 = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_c4, C_BG, 0);
    lv_obj_clear_flag(scr_c4, LV_OBJ_FLAG_SCROLLABLE);
    c4_score_lbl = mk_hdr(scr_c4, "Connect 4", c4_back);

    static const char *names[5] = { "2P", "Easy", "Med", "Hard", "VS" };
    for (int m = 0; m < 5; m++) {
        c4_mbtn[m] = btn(scr_c4, 44, 24, C_PANEL, C_INK, &lv_font_montserrat_12,
                         names[m], c4_mode_clicked, (void *)(intptr_t)m);
        lv_obj_align(c4_mbtn[m], LV_ALIGN_TOP_LEFT, 6 + m * 46, HDR_H + 6);
    }

    c4_peer_btn = btn(scr_c4, 100, 22, C_PANEL, C_BLUE, &lv_font_montserrat_12,
                      "vs: -", c4_peer_clicked, NULL);
    lv_obj_align(c4_peer_btn, LV_ALIGN_TOP_RIGHT, -8, HDR_H + 34);

    /* Narrower than the screen on purpose: full width ran straight under the
     * vs: button, which now sits on the same line. */
    c4_status = lbl(scr_c4, &lv_font_montserrat_12, C_MUTED, "");
    lv_label_set_long_mode(c4_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(c4_status, LCD_W - 124);
    lv_obj_align(c4_status, LV_ALIGN_TOP_LEFT, 8, HDR_H + 38);

    /* The rack itself — one blue slab with the holes punched out of it, which
     * is what makes it read as Connect 4 rather than as a grid of circles. */
    int rw = C4_COLS * C4_CELL + (C4_COLS + 1) * C4_GAP;
    int rh = C4_ROWS * C4_CELL + (C4_ROWS + 1) * C4_GAP;
    int x0 = (LCD_W - rw) / 2;
    int y0 = HDR_H + 58;        /* the mode row took the space above */

    c4_rack = rect(scr_c4, rw, rh, lv_color_hex(0x1F4FA8));
    lv_obj_set_pos(c4_rack, x0, y0);
    lv_obj_set_style_radius(c4_rack, 6, 0);

    for (int r = 0; r < C4_ROWS; r++) {
        for (int c = 0; c < C4_COLS; c++) {
            lv_obj_t *h = rect(c4_rack, C4_CELL - 4, C4_CELL - 4, C_BG);
            lv_obj_set_pos(h, C4_GAP + c * (C4_CELL + C4_GAP) + 2,
                              C4_GAP + r * (C4_CELL + C4_GAP) + 2);
            lv_obj_set_style_radius(h, LV_RADIUS_CIRCLE, 0);
            c4_hole[r * C4_COLS + c] = h;
        }
    }

    /* Invisible full-height strips over each column: the tap target is the
     * column, not the hole. */
    for (int c = 0; c < C4_COLS; c++) {
        lv_obj_t *strip = rect(scr_c4, C4_CELL + C4_GAP, rh, C_BG);
        lv_obj_set_style_bg_opa(strip, LV_OPA_TRANSP, 0);
        lv_obj_set_pos(strip, x0 + C4_GAP + c * (C4_CELL + C4_GAP), y0);
        lv_obj_add_flag(strip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(strip, c4_col_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)c);
    }
}

static void c4_show(lv_event_t *e)
{
    (void)e;
    if (!scr_c4) c4_build();
    tmr_stop();
    c4_reset();
    lv_scr_load(scr_c4);
    applog("[games] connect 4, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static bool c4_net_active(void)
{
    return c4_mode == C4_VS && c4_peer >= 0 && scr_c4 && lv_scr_act() == scr_c4;
}

static void c4_heartbeat(void)
{
    if (!c4_net_active() || c4_over) return;
    if (esp_timer_get_time() - c4_last_tx_us < 2000000) return;
    c4_broadcast(c4_discs(c4_b) == 0);
}

/* ------------------------------------------------------------------ *
 * brick breaker
 *
 * The paddle tracks the finger directly, which is the one control this
 * hardware does perfectly. Ball speed is per-level and deliberately gentle at
 * level 1 - a 40fps tick with a 2.2px step is about as fast as a small screen
 * stays readable.
 * ------------------------------------------------------------------ */

#define BRK_COLS   6
#define BRK_ROWS   5
#define BRK_BW     36
#define BRK_BH     12
#define BRK_GAP    3
#define BRK_PAD_W  52
#define BRK_PAD_H  8
#define BRK_PAD_Y  292
#define BRK_BALL   8
#define BRK_FLOOR  318

static lv_obj_t *scr_brk, *brk_paddle, *brk_ball, *brk_brick[BRK_COLS * BRK_ROWS],
                *brk_score_lbl, *brk_msg;
static float brk_x, brk_y, brk_vx, brk_vy;
static int   brk_score, brk_lives, brk_left, brk_level;
static bool  brk_running, brk_dead;

static void brk_layout_bricks(void)
{
    int x0 = (LCD_W - (BRK_COLS * BRK_BW + (BRK_COLS - 1) * BRK_GAP)) / 2;
    int y0 = HDR_H + 12;
    for (int r = 0; r < BRK_ROWS; r++) {
        for (int c = 0; c < BRK_COLS; c++) {
            lv_obj_t *b = brk_brick[r * BRK_COLS + c];
            lv_obj_set_pos(b, x0 + c * (BRK_BW + BRK_GAP),
                              y0 + r * (BRK_BH + BRK_GAP));
            lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
    }
    brk_left = BRK_COLS * BRK_ROWS;
}

static void brk_park_ball(void)
{
    brk_x = lv_obj_get_x(brk_paddle) + BRK_PAD_W / 2 - BRK_BALL / 2;
    brk_y = BRK_PAD_Y - BRK_BALL - 1;
    brk_vx = 0;
    brk_vy = 0;
    brk_running = false;
    lv_obj_set_pos(brk_ball, (int)brk_x, (int)brk_y);
    lv_label_set_text(brk_msg, "tap to serve");
    lv_obj_clear_flag(brk_msg, LV_OBJ_FLAG_HIDDEN);
}

static void brk_reset(bool full)
{
    if (full) {
        brk_score = 0;
        brk_lives = 3;
        brk_level = 1;
        brk_dead  = false;
    }
    brk_layout_bricks();
    lv_obj_set_x(brk_paddle, (LCD_W - BRK_PAD_W) / 2);
    brk_park_ball();
    lv_label_set_text_fmt(brk_score_lbl, "%d  " LV_SYMBOL_BELL "%d",
                          brk_score, brk_lives);
}

static void brk_serve(void)
{
    float speed = 2.2f + 0.25f * (brk_level - 1);
    brk_vx = (rnd(2) ? 1.0f : -1.0f) * speed * 0.6f;
    brk_vy = -speed;
    brk_running = true;
    lv_obj_add_flag(brk_msg, LV_OBJ_FLAG_HIDDEN);
}

static bool brk_hits(float bx, float by, lv_obj_t *o)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    int ox = lv_obj_get_x(o), oy = lv_obj_get_y(o);
    return bx + BRK_BALL > ox && bx < ox + BRK_BW &&
           by + BRK_BALL > oy && by < oy + BRK_BH;
}

static void brk_tick(lv_timer_t *t)
{
    if (!brk_running) return;

    brk_x += brk_vx;
    brk_y += brk_vy;

    if (brk_x <= 0)                { brk_x = 0;                brk_vx = -brk_vx; }
    if (brk_x >= LCD_W - BRK_BALL) { brk_x = LCD_W - BRK_BALL; brk_vx = -brk_vx; }
    if (brk_y <= HDR_H)            { brk_y = HDR_H;            brk_vy = -brk_vy; }

    /* paddle: the bounce angle comes from where on the paddle it lands, which
     * is the only thing that makes the game steerable rather than luck. */
    int px = lv_obj_get_x(brk_paddle);
    if (brk_vy > 0 && brk_y + BRK_BALL >= BRK_PAD_Y &&
        brk_y + BRK_BALL <= BRK_PAD_Y + BRK_PAD_H &&
        brk_x + BRK_BALL > px && brk_x < px + BRK_PAD_W) {
        float hit = ((brk_x + BRK_BALL / 2.0f) - (px + BRK_PAD_W / 2.0f))
                    / (BRK_PAD_W / 2.0f);
        float speed = 2.2f + 0.25f * (brk_level - 1);
        brk_vx = hit * speed;
        brk_vy = -speed;
        brk_y  = BRK_PAD_Y - BRK_BALL - 1;
    }

    for (int i = 0; i < BRK_COLS * BRK_ROWS; i++) {
        if (!brk_hits(brk_x, brk_y, brk_brick[i])) continue;
        lv_obj_add_flag(brk_brick[i], LV_OBJ_FLAG_HIDDEN);
        brk_vy = -brk_vy;
        brk_score += 10;
        brk_left--;
        lv_label_set_text_fmt(brk_score_lbl, "%d  " LV_SYMBOL_BELL "%d",
                              brk_score, brk_lives);
        break;                          /* one brick per tick keeps it honest */
    }

    if (brk_left == 0) {
        brk_level++;
        lv_label_set_text_fmt(brk_msg, "Level %d - tap", brk_level);
        lv_obj_clear_flag(brk_msg, LV_OBJ_FLAG_HIDDEN);
        brk_layout_bricks();
        brk_park_ball();
        return;
    }

    if (brk_y > BRK_FLOOR) {
        brk_lives--;
        lv_label_set_text_fmt(brk_score_lbl, "%d  " LV_SYMBOL_BELL "%d",
                              brk_score, brk_lives);
        if (brk_lives <= 0) {
            brk_running = false;
            brk_dead = true;
            if (brk_score > hs_get("brk_hi")) hs_set("brk_hi", brk_score);
            lv_label_set_text_fmt(brk_msg, "Game over  %d\nbest %d - tap",
                                  brk_score, hs_get("brk_hi"));
            lv_obj_clear_flag(brk_msg, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        brk_park_ball();
        return;
    }

    lv_obj_set_pos(brk_ball, (int)brk_x, (int)brk_y);
}

static void brk_pressing(lv_event_t *e)
{
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    int x = p.x - BRK_PAD_W / 2;
    if (x < 0) x = 0;
    if (x > LCD_W - BRK_PAD_W) x = LCD_W - BRK_PAD_W;
    lv_obj_set_x(brk_paddle, x);
    if (!brk_running && !brk_dead) {
        brk_x = x + BRK_PAD_W / 2 - BRK_BALL / 2;
        lv_obj_set_pos(brk_ball, (int)brk_x, (int)brk_y);
    }
}

static void brk_clicked(lv_event_t *e)
{
    if (brk_dead)          { brk_reset(true); return; }
    if (!brk_running)      { brk_serve(); }
}

static void brk_back(lv_event_t *e)
{
    tmr_stop();
    if (!scr_menu_ready()) return;
    leave_to(scr_menu, &scr_brk);
}

static void brk_build(void)
{
    scr_brk = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_brk, C_BG, 0);
    lv_obj_clear_flag(scr_brk, LV_OBJ_FLAG_SCROLLABLE);
    brk_score_lbl = mk_hdr(scr_brk, "Brick Breaker", brk_back);

    for (int r = 0; r < BRK_ROWS; r++) {
        lv_color_t c = (r == 0) ? C_RED : (r == 1) ? C_AMBER :
                       (r == 2) ? C_GREEN : (r == 3) ? C_BLUE : C_MUTED;
        for (int i = 0; i < BRK_COLS; i++)
            brk_brick[r * BRK_COLS + i] = rect(scr_brk, BRK_BW, BRK_BH, c);
    }

    brk_paddle = rect(scr_brk, BRK_PAD_W, BRK_PAD_H, C_INK);
    lv_obj_set_pos(brk_paddle, (LCD_W - BRK_PAD_W) / 2, BRK_PAD_Y);

    brk_ball = rect(scr_brk, BRK_BALL, BRK_BALL, C_AMBER);
    lv_obj_set_style_radius(brk_ball, BRK_BALL / 2, 0);

    brk_msg = lbl(scr_brk, &lv_font_montserrat_14, C_INK, "");
    lv_obj_set_style_text_align(brk_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(brk_msg, LV_ALIGN_CENTER, 0, 40);

    /* The whole screen is the control surface - there is nothing else on it. */
    lv_obj_add_flag(scr_brk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_brk, brk_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr_brk, brk_clicked, LV_EVENT_CLICKED, NULL);
}

static void brk_show(lv_event_t *e)
{
    if (!scr_brk) brk_build();
    brk_reset(true);
    lv_scr_load(scr_brk);
    applog("[games] brick breaker, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    tmr_start(brk_tick, 25);
}

/* ------------------------------------------------------------------ *
 * snake
 *
 * Steering is TAP WHERE YOU WANT IT TO GO, not swipe. Swipe works too (LVGL
 * gives us a gesture), but a tap is the gesture this touch panel reports most
 * reliably, and it is the one a small child performs on purpose.
 * ------------------------------------------------------------------ */

#define SNK_COLS 20
#define SNK_ROWS 22
#define SNK_CELL 12
#define SNK_X0   0
#define SNK_Y0   (HDR_H + 4)
#define SNK_MAX  40

static lv_obj_t *scr_snk, *snk_seg[SNK_MAX], *snk_food_obj, *snk_score_lbl, *snk_msg;
static int8_t snk_x[SNK_MAX], snk_y[SNK_MAX];
static int    snk_len, snk_dx, snk_dy, snk_pdx, snk_pdy, snk_score;
static int8_t snk_fx, snk_fy;
static bool   snk_dead;

static void snk_place_food(void)
{
    for (int guard = 0; guard < 200; guard++) {
        int8_t fx = rnd(SNK_COLS), fy = rnd(SNK_ROWS);
        bool clash = false;
        for (int i = 0; i < snk_len; i++)
            if (snk_x[i] == fx && snk_y[i] == fy) { clash = true; break; }
        if (clash) continue;
        snk_fx = fx; snk_fy = fy;
        lv_obj_set_pos(snk_food_obj, SNK_X0 + fx * SNK_CELL,
                                     SNK_Y0 + fy * SNK_CELL);
        return;
    }
}

static void snk_paint(void)
{
    for (int i = 0; i < SNK_MAX; i++) {
        if (i < snk_len) {
            lv_obj_set_pos(snk_seg[i], SNK_X0 + snk_x[i] * SNK_CELL,
                                       SNK_Y0 + snk_y[i] * SNK_CELL);
            lv_obj_set_style_bg_color(snk_seg[i], i == 0 ? C_INK : C_GREEN, 0);
            lv_obj_clear_flag(snk_seg[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(snk_seg[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void snk_reset(void)
{
    snk_len = 4;
    for (int i = 0; i < snk_len; i++) { snk_x[i] = 8 - i; snk_y[i] = 11; }
    snk_dx = 1; snk_dy = 0; snk_pdx = 1; snk_pdy = 0;
    snk_score = 0;
    snk_dead = false;
    lv_obj_add_flag(snk_msg, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(snk_score_lbl, "0  best %d", hs_get("snk_hi"));
    snk_place_food();
    snk_paint();
}

static void snk_tick(lv_timer_t *t)
{
    if (snk_dead) return;

    snk_dx = snk_pdx; snk_dy = snk_pdy;
    int8_t nx = snk_x[0] + snk_dx, ny = snk_y[0] + snk_dy;

    /* Walls kill. Wrapping would be kinder but it makes the game unlosable
     * and therefore pointless. */
    if (nx < 0 || ny < 0 || nx >= SNK_COLS || ny >= SNK_ROWS) goto dead;
    for (int i = 0; i < snk_len; i++)
        if (snk_x[i] == nx && snk_y[i] == ny) goto dead;

    bool ate = (nx == snk_fx && ny == snk_fy);
    if (ate && snk_len < SNK_MAX) snk_len++;

    for (int i = snk_len - 1; i > 0; i--) { snk_x[i] = snk_x[i-1]; snk_y[i] = snk_y[i-1]; }
    snk_x[0] = nx; snk_y[0] = ny;

    if (ate) {
        snk_score += 10;
        lv_label_set_text_fmt(snk_score_lbl, "%d  best %d",
                              snk_score, hs_get("snk_hi"));
        snk_place_food();
        /* Speed up gently, and stop at 90ms - past that a 12px cell moves
         * faster than a child can retarget a tap. */
        uint32_t p = 200 - (uint32_t)(snk_score / 10) * 6;
        if (p < 90) p = 90;
        if (s_tmr) lv_timer_set_period(s_tmr, p);
    }
    snk_paint();
    return;

dead:
    snk_dead = true;
    if (snk_score > hs_get("snk_hi")) hs_set("snk_hi", snk_score);
    lv_label_set_text_fmt(snk_msg, "Game over  %d\nbest %d - tap",
                          snk_score, hs_get("snk_hi"));
    lv_obj_clear_flag(snk_msg, LV_OBJ_FLAG_HIDDEN);
}

static void snk_steer(int dx, int dy)
{
    if (dx == -snk_dx && dy == -snk_dy) return;   /* no reversing into yourself */
    snk_pdx = dx; snk_pdy = dy;
}

static void snk_clicked(lv_event_t *e)
{
    if (snk_dead) { snk_reset(); if (s_tmr) lv_timer_set_period(s_tmr, 200); return; }

    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    int hx = SNK_X0 + snk_x[0] * SNK_CELL + SNK_CELL / 2;
    int hy = SNK_Y0 + snk_y[0] * SNK_CELL + SNK_CELL / 2;
    int dx = p.x - hx, dy = p.y - hy;
    if (abs(dx) > abs(dy)) snk_steer(dx > 0 ? 1 : -1, 0);
    else                   snk_steer(0, dy > 0 ? 1 : -1);
}

static void snk_gesture(lv_event_t *e)
{
    switch (lv_indev_get_gesture_dir(lv_indev_get_act())) {
    case LV_DIR_LEFT:  snk_steer(-1, 0); break;
    case LV_DIR_RIGHT: snk_steer( 1, 0); break;
    case LV_DIR_TOP:   snk_steer(0, -1); break;
    case LV_DIR_BOTTOM:snk_steer(0,  1); break;
    default: break;
    }
}

static void snk_back(lv_event_t *e)
{
    tmr_stop();
    if (!scr_menu_ready()) return;
    leave_to(scr_menu, &scr_snk);
}

static void snk_build(void)
{
    scr_snk = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_snk, C_BG, 0);
    lv_obj_clear_flag(scr_snk, LV_OBJ_FLAG_SCROLLABLE);
    snk_score_lbl = mk_hdr(scr_snk, "Snake", snk_back);

    snk_food_obj = rect(scr_snk, SNK_CELL - 2, SNK_CELL - 2, C_AMBER);
    lv_obj_set_style_radius(snk_food_obj, (SNK_CELL - 2) / 2, 0);

    for (int i = 0; i < SNK_MAX; i++) {
        snk_seg[i] = rect(scr_snk, SNK_CELL - 1, SNK_CELL - 1, C_GREEN);
        lv_obj_add_flag(snk_seg[i], LV_OBJ_FLAG_HIDDEN);
    }

    snk_msg = lbl(scr_snk, &lv_font_montserrat_14, C_INK, "");
    lv_obj_set_style_text_align(snk_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(snk_msg, LV_ALIGN_CENTER, 0, 0);

    lv_obj_add_flag(scr_snk, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr_snk, snk_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr_snk, snk_gesture, LV_EVENT_GESTURE, NULL);
}

static void snk_show(lv_event_t *e)
{
    if (!scr_snk) snk_build();
    snk_reset();
    lv_scr_load(scr_snk);
    applog("[games] snake, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    tmr_start(snk_tick, 200);
}

/* ------------------------------------------------------------------ *
 * Battleship — handset against handset
 *
 * 🚨 THIS IS THE FIRST GAME HERE WITH A SECRET, AND THE SECRET NEVER TRAVELS.
 *
 * Every other game sends the whole board because a lost frame then costs
 * nothing. Ships cannot go on the wire at all — the receiving handset would be
 * holding the answers — so what travels instead is the pair of SHOT GRIDS:
 * where each side has fired and what came back. Those are public knowledge by
 * definition (you are told hit or miss the moment you fire), they are still the
 * whole state, and they are still idempotent.
 *
 * 🔑 THE OWNER OF A SQUARE IS THE ONLY ONE WHO CAN ANSWER FOR IT. A shot goes
 * out marked PENDING; the far handset matches it against its own ships, writes
 * MISS or HIT into that same grid and sends the grid straight back. Nobody is
 * ever asked to take the other side's word for a square they own, and a lost
 * frame leaves a shot sitting PENDING — which the two-second heartbeat resends
 * until it is answered. The repair path is the same one the other games have.
 *
 * ⚠️ WHY A LADDER OF 2*resolved + pending AND NOT A PLAIN CELL COUNT. Marks in
 * tic-tac-toe only ever increase, so counting them orders two boards. Here a
 * cell goes UNKNOWN -> PENDING -> resolved, so the count of non-empty cells
 * does NOT change when a shot is answered — the reply to my own shot would
 * score equal to the shot and could be dropped as stale. Weighting a resolved
 * cell double makes every single step, fire and answer alike, strictly
 * increase.
 * ------------------------------------------------------------------ */

#define BS_N      6                       /* 6x6 — a 10x10 grid on a 240px
                                             panel gives 22px squares, which is
                                             below what a six-year-old can hit */
#define BS_CELLS  (BS_N * BS_N)           /* 36, and 2 bits each = 9 bytes */
#define BS_PACK   ((BS_CELLS + 3) / 4)
#define BS_CELL   24                      /* 6*24+5*2 = 154, and both grids fit */
#define BS_GAP    2
#define BS_MINI   9                       /* my own waters, shown small */

enum { BS_UNKNOWN = 0, BS_PENDING = 1, BS_MISS = 2, BS_HIT = 3 };

/* 3 + 2 + 2 + 1 = 8 squares of 36. Fewer and the hunt drags; more and a game
 * is over before the music changes track. */
static const uint8_t BS_SHIPS[] = { 3, 2, 2, 1 };
#define BS_SHIP_CELLS 8

static lv_obj_t *scr_bs, *bs_status, *bs_peer_btn, *bs_score_lbl,
                *bs_cell[BS_CELLS], *bs_mini[BS_CELLS];

/* [0] is the starter's shots, [1] the other side's. Indexed by SIDE, not by
 * "mine"/"theirs", so the two handsets hold identical arrays. */
static uint8_t  bs_shot[2][BS_CELLS];
static uint8_t  bs_mine[BS_CELLS];        /* my ships. Never sent. */
/* 0 easy, 1 med, 2 hard, 3 = the other handset. Same ladder as every other
 * game here, and for the same reason Hearts needed one: a game that only works
 * with a second handset in range is a game nobody can play in a car. */
#define BS_VS 3
static int      bs_mode;
static lv_obj_t *bs_mbtn[4];
static uint8_t  bs_ai_ships[BS_CELLS];    /* the machine's fleet, when local */
static uint8_t  bs_gen, bs_ships_gen;
static uint16_t bs_starter;
static int      bs_peer = -1;             /* declared above for the dispatcher */
static int      bs_me;                    /* 0 if I am the starter, else 1 */
static bool     bs_over;
static int64_t  bs_last_tx_us;
/* Indexed by device id (see id_slot), NOT by bs_me — the side a player takes
 * flips with the starter. `bs_won_gen` makes a game count exactly once no
 * matter which end noticed the fleet go down. */
static int      bs_wins[2];
static int      bs_won_gen = -1;
static lv_obj_t *bs_banner;

static void bs_paint(void);
static void bs_status_set(void);
static void bs_reset(void);
static void bs_ai_arm(void);
static void bs_finish(int w);
static int  bs_winner(void);
static int  bs_turn(void);

static void bs_pack(uint8_t *dst, const uint8_t *cells)
{
    memset(dst, 0, BS_PACK);
    for (int i = 0; i < BS_CELLS; i++)
        dst[i >> 2] |= (uint8_t)((cells[i] & 3) << ((i & 3) * 2));
}

static void bs_unpack(uint8_t *cells, const uint8_t *src)
{
    for (int i = 0; i < BS_CELLS; i++)
        cells[i] = (src[i >> 2] >> ((i & 3) * 2)) & 3;
}

static int bs_resolved(const uint8_t *g)
{
    int n = 0;
    for (int i = 0; i < BS_CELLS; i++)
        if (g[i] == BS_MISS || g[i] == BS_HIT) n++;
    return n;
}

static bool bs_pending(const uint8_t *g)
{
    for (int i = 0; i < BS_CELLS; i++) if (g[i] == BS_PENDING) return true;
    return false;
}

static int bs_hits(const uint8_t *g)
{
    int n = 0;
    for (int i = 0; i < BS_CELLS; i++) if (g[i] == BS_HIT) n++;
    return n;
}

/* The staleness ladder — see the file header for why resolved counts double. */
static int bs_ladder(const uint8_t g[2][BS_CELLS])
{
    int n = 0;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < BS_CELLS; i++) {
            if (g[s][i] == BS_PENDING) n += 1;
            else if (g[s][i] != BS_UNKNOWN) n += 2;
        }
    return n;
}

/* Whose shot it is: the starter fires first, and after that whoever has fired
 * fewer answered shots. -1 while a shot is still waiting to be answered —
 * derived, like every other turn in this file, so the two handsets cannot be
 * told different things. */
static int bs_turn(void)
{
    if (bs_pending(bs_shot[0]) || bs_pending(bs_shot[1])) return -1;
    return bs_resolved(bs_shot[0]) == bs_resolved(bs_shot[1]) ? 0 : 1;
}

static int bs_winner(void)
{
    if (bs_hits(bs_shot[0]) >= BS_SHIP_CELLS) return 0;
    if (bs_hits(bs_shot[1]) >= BS_SHIP_CELLS) return 1;
    return -1;
}

/* Placed locally, kept locally, and re-placed for every new generation. The
 * retry loop is not decoration: an unplaceable ship would leave fewer than
 * BS_SHIP_CELLS squares on the board and the opponent could never win. */
static void bs_place_ships(void)
{
    for (int attempt = 0; attempt < 40; attempt++) {
        memset(bs_mine, 0, sizeof(bs_mine));
        bool ok = true;
        for (unsigned s = 0; s < sizeof(BS_SHIPS) && ok; s++) {
            int len = BS_SHIPS[s];
            ok = false;
            for (int try = 0; try < 200 && !ok; try++) {
                int horiz = rnd(2);
                int r = rnd(horiz ? BS_N : BS_N - len + 1);
                int c = rnd(horiz ? BS_N - len + 1 : BS_N);
                bool free_ = true;
                for (int k = 0; k < len; k++) {
                    int i = horiz ? r * BS_N + c + k : (r + k) * BS_N + c;
                    if (bs_mine[i]) { free_ = false; break; }
                }
                if (!free_) continue;
                for (int k = 0; k < len; k++) {
                    int i = horiz ? r * BS_N + c + k : (r + k) * BS_N + c;
                    bs_mine[i] = 1;
                }
                ok = true;
            }
        }
        int n = 0;
        for (int i = 0; i < BS_CELLS; i++) n += bs_mine[i];
        if (ok && n == BS_SHIP_CELLS) return;
    }
    applog("[bs] could not place the fleet in 40 tries - playing the last one");
}

/* Answer whatever the other side has just fired at me. True if anything
 * changed, which is the caller's cue to send the grid back. */
static bool bs_answer(void)
{
    uint8_t *theirs = bs_shot[1 - bs_me];
    bool changed = false;
    for (int i = 0; i < BS_CELLS; i++) {
        if (theirs[i] != BS_PENDING) continue;
        theirs[i] = bs_mine[i] ? BS_HIT : BS_MISS;
        changed = true;
    }
    return changed;
}

/* Where the machine fires next.
 *
 * 🔑 HUNT AND TARGET, WHICH IS HOW PEOPLE ACTUALLY PLAY. A random shot has a 1
 * in 4.5 chance here; a random shot NEXT TO A HIT is far better than that, and
 * a shot continuing a LINE of two hits is better again. That progression is the
 * whole difficulty ladder:
 *
 *   Easy — random, every time. It will find your fleet eventually and a child
 *          will usually win.
 *   Med  — random until it hits, then it works around that hit. Beatable,
 *          because it does not know which way a ship is lying.
 *   Hard — same, but it EXTENDS a line once it has two in a row, and while it
 *          is still hunting it only fires on one colour of the checkerboard.
 *
 * ⚠️ THE PARITY TRICK HAS A HOLE, AND THE FLEET PUTS IT THERE. Firing only on
 * squares where (row+col) is even cannot miss a ship of length 2 or more — but
 * BS_SHIPS ends in a 1, and a single square can sit on the colour never
 * checked. So parity is a preference, not a rule: when the even squares run
 * out it falls back to whatever is left, which is exactly when that last
 * one-square ship is all that remains. */
static int bs_ai_pick(void)
{
    static const int DR[4] = { -1, 1, 0, 0 }, DC[4] = { 0, 0, -1, 1 };
    const uint8_t *g = bs_shot[1];        /* the machine's shots at me */
    int cand[BS_CELLS], n = 0;

    if (bs_mode >= 1) {
        for (int r = 0; r < BS_N; r++) {
            for (int c = 0; c < BS_N; c++) {
                if (g[r * BS_N + c] != BS_HIT) continue;
                for (int d = 0; d < 4; d++) {
                    int rr = r + DR[d], cc = c + DC[d];
                    if (rr < 0 || rr >= BS_N || cc < 0 || cc >= BS_N) continue;
                    if (g[rr * BS_N + cc] != BS_UNKNOWN) continue;
                    /* Two hits in a line: the ship lies that way, so keep
                     * going. Taken immediately rather than collected, because
                     * nothing else on the board is worth as much. */
                    int br = r - DR[d], bc = c - DC[d];
                    bool in_line = br >= 0 && br < BS_N && bc >= 0 && bc < BS_N &&
                                   g[br * BS_N + bc] == BS_HIT;
                    if (bs_mode == 2 && in_line) return rr * BS_N + cc;
                    cand[n++] = rr * BS_N + cc;
                }
            }
        }
        if (n) return cand[rnd(n)];
    }

    n = 0;
    for (int i = 0; i < BS_CELLS; i++) {
        if (g[i] != BS_UNKNOWN) continue;
        if (bs_mode == 2 && (((i / BS_N) + (i % BS_N)) & 1)) continue;
        cand[n++] = i;
    }
    if (!n)                               /* parity exhausted — see above */
        for (int i = 0; i < BS_CELLS; i++)
            if (g[i] == BS_UNKNOWN) cand[n++] = i;
    return n ? cand[rnd(n)] : -1;
}

static void bs_set_role(void)
{
    /* Locally the player is always side 0 and always fires first. There is no
     * second device to disagree with, and a child going first is the right
     * default anyway. */
    if (bs_mode != BS_VS) { bs_me = 0; bs_starter = espnow_my_id(); return; }
    if (bs_peer < 0) { bs_me = 0; return; }
    bs_starter = starter_fix(bs_starter, espnow_my_id(), espnow_peer_id(bs_peer));
    bs_me = (espnow_my_id() == bs_starter) ? 0 : 1;
}

static void bs_next_starter(int w)
{
    if (bs_mode != BS_VS || bs_peer < 0) return;
    uint16_t me = espnow_my_id(), them = espnow_peer_id(bs_peer);
    bs_starter = (w == bs_me) ? them : me;   /* the loser fires first */
}

static void bs_pick_default_peer(void)
{
    pick_default_peer(&bs_peer);
}

static void bs_broadcast(bool is_new)
{
    if (bs_mode != BS_VS) return;       /* a game against the machine is local */
    if (bs_peer < 0 || !scr_bs) return;
    espnow_game_t g = { 0 };
    g.game    = GAME_BATTLE;
    g.flags   = is_new ? GF_NEW : 0;
    g.cells   = BS_CELLS;
    g.gen     = bs_gen;
    g.starter = bs_starter;
    bs_pack(g.board,           bs_shot[0]);
    bs_pack(g.board + BS_PACK, bs_shot[1]);
    /* The wins ride past the two boards. 36 cells at 2 bits is 9 bytes each,
     * so everything from byte 18 up is free in a 64-byte board. */
    g.board[18] = (uint8_t)bs_wins[0];
    g.board[19] = (uint8_t)(bs_wins[0] >> 8);
    g.board[20] = (uint8_t)bs_wins[1];
    g.board[21] = (uint8_t)(bs_wins[1] >> 8);
    bs_last_tx_us = esp_timer_get_time();
    if (!espnow_game_send(bs_peer, &g) && bs_status)
        lv_label_set_text(bs_status, "send failed");
}

static int bs_my_slot(void)
{
    if (bs_mode != BS_VS || bs_peer < 0) return 0;
    return id_slot(espnow_my_id(), espnow_peer_id(bs_peer));
}

static void bs_finish(int w)
{
    bs_over = true;
    /* Once per game, whichever end saw it happen. bs_apply() re-enters here
     * every time a repeat frame lands on a finished board. */
    if (bs_won_gen != (int)bs_gen) {
        bs_won_gen = (int)bs_gen;
        int mine = bs_my_slot();
        bs_wins[(w == bs_me) ? mine : 1 - mine]++;
    }
    if (!bs_status) return;
    bool won = (w == bs_me);
    lv_label_set_text_fmt(bs_status, "%s - tap to play again",
                          won ? "You sank the fleet!" : "Your fleet is sunk");
    const char *who = won ? "YOU WIN" :
                      (bs_mode == BS_VS && bs_peer >= 0) ? espnow_peer_name(bs_peer)
                                                         : "THE MACHINE";
    if (won) banner_set(&bs_banner, scr_bs, "YOU WIN\nfleet sunk", C_GREEN);
    else {
        char t[48];
        snprintf(t, sizeof(t), "%s WINS\nyour fleet is sunk", who);
        banner_set(&bs_banner, scr_bs, t, C_RED);
    }
    bs_paint();
}

static void bs_reset(void)
{
    memset(bs_shot, 0, sizeof(bs_shot));
    bs_over = false;
    banner_set(&bs_banner, scr_bs, NULL, C_INK);
    if (bs_mode == BS_VS) bs_pick_default_peer();
    bs_gen++;
    bs_set_role();
    bs_place_ships();
    bs_ships_gen = bs_gen;
    if (bs_mode != BS_VS) {
        /* The machine needs a fleet of its own, laid out by the same code that
         * lays out the player's — including its retry loop, because a short
         * fleet on this side is a game that cannot be WON rather than one that
         * cannot be lost. */
        uint8_t keep[BS_CELLS];
        memcpy(keep, bs_mine, sizeof(keep));
        bs_place_ships();
        memcpy(bs_ai_ships, bs_mine, sizeof(bs_ai_ships));
        memcpy(bs_mine, keep, sizeof(bs_mine));
    }
    bs_paint();
    bs_broadcast(true);
    bs_status_set();
}

static void bs_apply(const espnow_game_t *g)
{
    if (g->game != GAME_BATTLE || g->cells != BS_CELLS) return;
    bs_mode = BS_VS;                    /* somebody is shooting at us */
    if (g->gen < bs_gen) return;

    uint8_t in[2][BS_CELLS];
    bs_unpack(in[0], g->board);
    bs_unpack(in[1], g->board + BS_PACK);
    if (g->gen == bs_gen && bs_ladder((const uint8_t (*)[BS_CELLS])in) < bs_ladder((const uint8_t (*)[BS_CELLS])bs_shot))
        return;

    bs_gen     = g->gen;
    bs_starter = (bs_peer >= 0)
                 ? starter_fix(g->starter, espnow_my_id(), espnow_peer_id(bs_peer))
                 : g->starter;
    bs_me      = (espnow_my_id() == bs_starter) ? 0 : 1;
    memcpy(bs_shot, in, sizeof(bs_shot));
    bs_over = false;

    /* 🔑 A NEW GENERATION MEANS A NEW FLEET, AND ONLY ONE HANDSET PRESSED NEW.
     * The other one learns about the new game from this frame, and if it kept
     * the old layout its ships would sit where the opponent has already been
     * shooting. */
    if (bs_ships_gen != bs_gen) { bs_place_ships(); bs_ships_gen = bs_gen; }

    /* Their tally repairs ours before the game is judged, so a win this
     * handset never saw the final frame for still lands on the scoreboard. */
    int theirs[2] = { g->board[18] | (g->board[19] << 8),
                      g->board[20] | (g->board[21] << 8) };
    score_merge(bs_wins, theirs);

    bool answered = bs_answer();
    banner_set(&bs_banner, scr_bs, NULL, C_INK);
    bs_paint();
    int w = bs_winner();
    if (answered) bs_broadcast(false);     /* their shot, answered, straight back */
    if (w >= 0) { bs_next_starter(w); bs_finish(w); }
    else        bs_status_set();
}

/* 🔑 THE MACHINE FIRES ON A TIMER, and here the delay is not only about being
 * seen: a shot that lands the instant the player's does reads as one event, and
 * a child needs to see which of the two squares was theirs. */
static void bs_ai_tick(lv_timer_t *t)
{
    (void)t;
    tmr_stop();
    if (bs_mode == BS_VS || bs_over) return;
    if (!scr_bs || lv_scr_act() != scr_bs) return;
    if (bs_turn() != 1) return;

    int i = bs_ai_pick();
    if (i < 0) return;
    /* Resolved against MY ships, here and nowhere else — the same rule the
     * network game enforces by making the owner answer. */
    bs_shot[1][i] = bs_mine[i] ? BS_HIT : BS_MISS;
    bs_paint();
    int w = bs_winner();
    if (w >= 0) bs_finish(w);
    else        bs_status_set();
}

static void bs_ai_arm(void)
{
    if (bs_mode == BS_VS || bs_over) return;
    if (bs_turn() == 1) tmr_start(bs_ai_tick, 600);
}

static void bs_status_set(void)
{
    if (!bs_status) return;
    if (bs_score_lbl)
        lv_label_set_text_fmt(bs_score_lbl, "%d-%d",
                              bs_wins[bs_my_slot()], bs_wins[1 - bs_my_slot()]);
    if (bs_over) return;
    int t = bs_turn();
    if (bs_mode != BS_VS) {
        lv_label_set_text_fmt(bs_status, "%s  -  hits %d/%d, mine %d/%d",
                              t == 0 ? "Your shot" : "thinking...",
                              bs_hits(bs_shot[0]), BS_SHIP_CELLS,
                              bs_hits(bs_shot[1]), BS_SHIP_CELLS);
        return;
    }
    if (bs_peer < 0) { lv_label_set_text(bs_status, "nobody in range"); return; }
    const char *pn = espnow_peer_name(bs_peer);
    lv_label_set_text_fmt(bs_status, "%s  -  hits %d/%d  vs %s",
                          t < 0        ? "waiting" :
                          t == bs_me   ? "Your shot" : "Their shot",
                          bs_hits(bs_shot[bs_me]), BS_SHIP_CELLS,
                          pn ? pn : "?");
}

static void bs_paint(void)
{
    /* Same trap Connect 4 hit: a frame can arrive before this screen has ever
     * been built. The state above is still updated, so opening the screen later
     * shows the game in progress. */
    if (!scr_bs) return;

    const uint8_t *theirs = bs_shot[bs_me];        /* where I have fired */
    const uint8_t *at_me  = bs_shot[1 - bs_me];    /* where they have fired */

    for (int i = 0; i < BS_CELLS; i++) {
        lv_color_t c = theirs[i] == BS_HIT     ? C_RED :
                       theirs[i] == BS_MISS    ? C_LINE :
                       theirs[i] == BS_PENDING ? C_AMBER : C_PANEL;
        lv_obj_set_style_bg_color(bs_cell[i], c, 0);

        /* My own waters: my ships in green until they are hit. */
        lv_color_t m = at_me[i] == BS_HIT  ? C_RED :
                       at_me[i] == BS_MISS ? C_LINE :
                       bs_mine[i]          ? C_GREEN : C_PANEL;
        lv_obj_set_style_bg_color(bs_mini[i], m, 0);
    }
    for (int m = 0; m < 4; m++)
        if (bs_mbtn[m])
            lv_obj_set_style_bg_color(bs_mbtn[m], m == bs_mode ? C_BLUE : C_PANEL, 0);

    if (bs_peer_btn) {
        lv_obj_t *pl = lv_obj_get_child(bs_peer_btn, 0);
        const char *pn = (bs_peer >= 0) ? espnow_peer_name(bs_peer) : NULL;
        if (pl) lv_label_set_text_fmt(pl, "vs: %s", pn ? pn : "-");
        if (bs_mode == BS_VS) lv_obj_clear_flag(bs_peer_btn, LV_OBJ_FLAG_HIDDEN);
        else                  lv_obj_add_flag(bs_peer_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (bs_score_lbl)
        lv_label_set_text_fmt(bs_score_lbl, "%d-%d",
                              bs_wins[bs_my_slot()], bs_wins[1 - bs_my_slot()]);
}

static void bs_cell_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);

    if (bs_over) { bs_reset(); return; }
    if (bs_mode == BS_VS) {
        bs_pick_default_peer();
        if (bs_peer < 0) { lv_label_set_text(bs_status, "nobody in range"); return; }
        /* The screen can be opened with nobody in range and a peer arrive
         * after. Without this the sides are whatever bs_reset() guessed with no
         * peer to compare ids against, and both handsets can believe they are
         * side 0. */
        bs_set_role();
    }

    int t = bs_turn();
    if (t < 0)      { lv_label_set_text(bs_status, "waiting for their answer"); return; }
    if (t != bs_me) { lv_label_set_text(bs_status, "not your shot"); return; }
    if (bs_shot[bs_me][i] != BS_UNKNOWN) {
        lv_label_set_text(bs_status, "you have fired there already");
        return;
    }

    if (bs_mode != BS_VS) {
        /* 🔑 NO PENDING STATE LOCALLY. Against another handset a shot must go
         * out unresolved because only the owner of that square may answer for
         * it; here this board owns both fleets, so the answer is immediate and
         * a PENDING square would be a promise nobody is left to keep. */
        bs_shot[bs_me][i] = bs_ai_ships[i] ? BS_HIT : BS_MISS;
        bs_paint();
        int w = bs_winner();
        if (w >= 0) { bs_finish(w); return; }
        bs_status_set();
        bs_ai_arm();
        return;
    }

    /* PENDING, not a result: this handset does not own that square and must not
     * decide what is in it. */
    bs_shot[bs_me][i] = BS_PENDING;
    bs_paint();
    bs_broadcast(false);
    bs_status_set();
}

static void bs_mode_clicked(lv_event_t *e)
{
    bs_mode = (int)(intptr_t)lv_event_get_user_data(e);
    bs_wins[0] = bs_wins[1] = 0;        /* a different opponent, a new score */
    bs_won_gen = -1;
    bs_starter = 0;
    tmr_stop();
    bs_reset();
}

static void bs_peer_clicked(lv_event_t *e)
{
    (void)e;
    bs_peer = play_next(bs_peer);
    if (bs_peer < 0) lv_label_set_text(bs_status, "nobody in range");
    bs_set_role();
    bs_paint();
    bs_status_set();
}

static void bs_back(lv_event_t *e)
{
    (void)e;
    tmr_stop();
    if (!scr_menu_ready()) return;
    bs_banner = NULL;               /* freed with the screen; see leave_to */
    leave_to(scr_menu, &scr_bs);
}

static void bs_build(void)
{
    bs_banner = NULL;
    scr_bs = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_bs, C_BG, 0);
    lv_obj_clear_flag(scr_bs, LV_OBJ_FLAG_SCROLLABLE);
    bs_score_lbl = mk_hdr(scr_bs, "Battleship", bs_back);

    static const char *names[4] = { "Easy", "Med", "Hard", "VS" };
    for (int m = 0; m < 4; m++) {
        bs_mbtn[m] = btn(scr_bs, 44, 22, C_PANEL, C_INK, &lv_font_montserrat_12,
                         names[m], bs_mode_clicked, (void *)(intptr_t)m);
        lv_obj_align(bs_mbtn[m], LV_ALIGN_TOP_LEFT, 6 + m * 46, HDR_H + 4);
    }

    bs_peer_btn = btn(scr_bs, 100, 22, C_PANEL, C_BLUE, &lv_font_montserrat_12,
                      "vs: -", bs_peer_clicked, NULL);
    lv_obj_align(bs_peer_btn, LV_ALIGN_TOP_RIGHT, -8, HDR_H + 30);

    /* Narrower than the screen: the vs: button shares this line. */
    bs_status = lbl(scr_bs, &lv_font_montserrat_12, C_MUTED, "");
    lv_label_set_long_mode(bs_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(bs_status, LCD_W - 124);
    lv_obj_align(bs_status, LV_ALIGN_TOP_LEFT, 8, HDR_H + 32);

    /* Their waters — the big grid, because this is the one being tapped. */
    int gw = BS_N * BS_CELL + (BS_N - 1) * BS_GAP;
    int x0 = (LCD_W - gw) / 2, y0 = HDR_H + 56;   /* the mode row took the top */
    for (int r = 0; r < BS_N; r++) {
        for (int c = 0; c < BS_N; c++) {
            int i = r * BS_N + c;
            lv_obj_t *o = rect(scr_bs, BS_CELL, BS_CELL, C_PANEL);
            lv_obj_set_pos(o, x0 + c * (BS_CELL + BS_GAP),
                              y0 + r * (BS_CELL + BS_GAP));
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(o, bs_cell_clicked, LV_EVENT_CLICKED,
                                (void *)(intptr_t)i);
            bs_cell[i] = o;
        }
    }

    /* My waters — small, and not clickable. It is a damage report, not a
     * control: there is nothing a player can do to their own squares. */
    int my = y0 + gw + 14;      /* clear of the grid, and the tag clears both */
    lv_obj_t *tag = lbl(scr_bs, &lv_font_montserrat_10, C_MUTED, "your fleet");
    lv_obj_set_pos(tag, 8, my - 12);
    for (int r = 0; r < BS_N; r++) {
        for (int c = 0; c < BS_N; c++) {
            int i = r * BS_N + c;
            lv_obj_t *o = rect(scr_bs, BS_MINI - 1, BS_MINI - 1, C_PANEL);
            lv_obj_set_pos(o, 8 + c * BS_MINI, my + r * BS_MINI);
            bs_mini[i] = o;
        }
    }
}

static void bs_show(lv_event_t *e)
{
    (void)e;
    if (!scr_bs) bs_build();
    tmr_stop();
    bs_reset();
    lv_scr_load(scr_bs);
    applog("[games] battleship, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static bool bs_net_active(void)
{
    return bs_mode == BS_VS && bs_peer >= 0 && scr_bs && lv_scr_act() == scr_bs;
}

static void bs_heartbeat(void)
{
    if (!bs_net_active() || bs_over) return;
    if (esp_timer_get_time() - bs_last_tx_us < 2000000) return;
    bs_broadcast(bs_ladder((const uint8_t (*)[BS_CELLS])bs_shot) == 0);
}

/* ------------------------------------------------------------------ *
 * Hearts — handset against handset, two players, 26 cards each
 *
 * 🔑 THE DECK IS DERIVED, NOT DEALT. A card game has the same problem
 * Battleship has — hands are secret — but it also has a problem Battleship
 * does not: the two handsets must agree on a SHUFFLE, and a shuffle is 52
 * bytes of state that has to exist before either player can move. Sending it
 * would mean a handshake, an ack, and a state where one side has a deal and
 * the other does not.
 *
 * So neither side sends a deal. One 32-bit SEED travels, both sides run the
 * identical Fisher-Yates over the identical xorshift, and both arrive at the
 * same 52 cards in the same order. That is the builder's own rule: two devices that
 * must agree should COMPUTE the shared state, not negotiate it.
 *
 * What travels after that is the ordered list of cards played — at most 52
 * bytes, and the whole game. Tricks, points, whose turn it is and what is
 * still in each hand are all replayed from it. A repeated frame replays to the
 * same position, so this inherits the idempotence every other game here has.
 *
 * ⚠️ HONESTY NOTE, WRITTEN DOWN SO NOBODY IS SURPRISED LATER. Deriving the
 * deck means each handset can compute the OTHER hand as easily as its own.
 * Nothing displays it and nothing sends it, but a modified firmware could see
 * it. That is a fair trade at this size — the alternative is a commit-reveal
 * protocol on a device a six-year-old is holding — and it is exactly why
 * Battleship, where the secret is the whole game, keeps its ships local and
 * answers shots instead.
 *
 * Rules, and the ones deliberately left out:
 *   - 26 tricks, no passing. Passing three cards needs a whole phase of its own
 *     and two players make it nearly meaningless.
 *   - The 2 of clubs leads the first trick, which also settles who starts
 *     without anybody being told.
 *   - Follow suit if you can. Hearts cannot be LED until one has been played
 *     on another suit, unless hearts is all you hold.
 *   - No hearts and no queen of spades on the very first trick unless there is
 *     nothing else to play.
 *   - Hearts score 1 each, the queen of spades 13. Low total wins.
 *   - Take all 26 and you shoot the moon: the other player takes them instead.
 * ------------------------------------------------------------------ */

#define HR_CARDS  52
#define HR_HAND   26
#define HR_SLOTS  HR_HAND
/* 🃏 THEY HAVE TO LOOK LIKE CARDS. The builder, 2026-08-25: "i was expecting hearts to
 * look like a card game is that not what it is". The first version drew the
 * hand as a grid of dark panels with "QS" written on them — which is a grid of
 * buttons, not a hand of cards, and reads as neither.
 *
 * What makes it read as cards, in order of how much each one buys:
 *   1. **A pale face.** Everything else on this device is dark-on-dark. A card
 *      is the one object that is light, and that alone does most of the work.
 *   2. **Real pips.** ♠ ♥ ♦ ♣ are not in Montserrat, and no arrangement of
 *      letters is a substitute — "QS" is a stock ticker. The two fonts in
 *      lv_font_cards_*.c are DejaVu Sans Bold cut down to the ranks and the
 *      four suits (U+2660/2663/2665/2666), built with lv_font_conv.
 *   3. **Overlapping, and sorted into suits.** A hand of 26 laid out as a grid
 *      is a spreadsheet. Fanned in four rows, each row one suit, it is a hand —
 *      and it is how the cards would be held anyway.
 *
 * ⚠️ THE FAN IS ALSO WHY THE HIT TESTING WORKS. Cards are created left to
 * right, so each one covers the one before and only its left edge stays
 * visible. LVGL hands a press to the topmost object under the point, and that
 * left edge is outside every card that covers it — so tapping the sliver picks
 * the card the sliver belongs to, exactly as it would with real cards. */
#define HR_CW     30                   /* a card FACE; most of it is covered */
#define HR_CH     40
#define HR_STEP   22                   /* how much of each card stays visible */
#define HR_TCW    40                   /* the two cards on the table are bigger */
#define HR_TCH    56
#define HR_ROWS   4                    /* one per suit */

#define HR_FACE   lv_color_hex(0xEFEAD8)   /* old paper, not white */
#define HR_FACE_D lv_color_hex(0x8C8878)   /* a card you may not play */
#define HR_RED    lv_color_hex(0xB4242B)
#define HR_BLACK  lv_color_hex(0x1A1A1A)
#define HR_EDGE   lv_color_hex(0x2A2A2A)

LV_FONT_DECLARE(lv_font_cards_16);
LV_FONT_DECLARE(lv_font_cards_26);
#define HR_QS     (2 * 13 + 10)        /* queen of spades */
#define HR_2C     0                    /* two of clubs — suit 0, rank 0 */

#define HR_SUIT(c) ((c) / 13)
#define HR_RANK(c) ((c) % 13)

static const char *HR_RANKS[13] = { "2","3","4","5","6","7","8","9","10","J","Q","K","A" };
/* UTF-8 for U+2663 U+2666 U+2660 U+2665, in the suit order this file uses
 * (clubs, diamonds, spades, hearts). The names are for the status line, where
 * a pip in a 12px Montserrat label would be a missing-glyph box. */
static const char *HR_SUITS[4]  = { "\xE2\x99\xA3", "\xE2\x99\xA6",
                                    "\xE2\x99\xA0", "\xE2\x99\xA5" };
static const char *HR_SUITNAME[4] = { "clubs", "diamonds", "spades", "hearts" };

static lv_obj_t *scr_hr, *hr_status, *hr_peer_btn, *hr_score_lbl,
                *hr_slot[HR_SLOTS], *hr_trick[2], *hr_trick_note;
/* Four rows of 44 from here ends at 310 on a 320px panel. */
static const int hr_row_y = HDR_H + 112;

/* 0 = an easy machine, 1 = a hard one, 2 = the other handset.
 *
 * 🚨 A GAME THAT NEEDS A SECOND HANDSET IS A GAME NOBODY CAN LOOK AT. Hearts
 * and Battleship both shipped network-only this afternoon, which is the exact
 * complaint the builder had made about Connect 4 an hour earlier — and it is worse
 * here, because with nobody in range Hearts does not even DEAL, so the screen
 * is blank. Default to the machine: alone in a car is the normal case. */
#define HR_VS 2
static int      hr_mode;
static lv_obj_t *hr_mbtn[3];
static uint8_t  hr_playlist[HR_CARDS];
static int      hr_n;                  /* cards played this hand */
static uint32_t hr_seed;
static uint8_t  hr_gen, hr_scored_gen;
static uint16_t hr_starter;            /* device id dealt the first half */
static int      hr_peer = -1;          /* declared above for the dispatcher */
static int      hr_me;                 /* 0 = the starter's half, else 1 */
static bool     hr_over;
/* Indexed by device id (see id_slot), NOT by hr_me. The deal alternates every
 * hand, so hr_me flips every hand, and a total indexed by it is handed to the
 * other player along with the deal. This was the bug on two real handsets. */
static int      hr_total[2];
static lv_obj_t *hr_banner;
static int64_t  hr_last_tx_us;
static int      hr_slot_card[HR_SLOTS];

static void hr_paint(void);
static void hr_status_set(const char *override);
static void hr_new_hand(void);
static void hr_ai_arm(void);

/* The one shuffle both boards must agree on, byte for byte. xorshift32 rather
 * than esp_random(): this has to be REPRODUCIBLE from the seed, on two
 * different boards, forever. */
static uint32_t hr_rng(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

static void hr_deal(uint8_t *deck)
{
    for (int i = 0; i < HR_CARDS; i++) deck[i] = (uint8_t)i;
    uint32_t s = hr_seed ? hr_seed : 1u;
    for (int i = HR_CARDS - 1; i > 0; i--) {
        int j = (int)(hr_rng(&s) % (uint32_t)(i + 1));
        uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
}

static int hr_pts(int card)
{
    if (card == HR_QS) return 13;
    return HR_SUIT(card) == 3 ? 1 : 0;
}

typedef struct {
    uint8_t hand[2][HR_HAND];
    int     nhand[2];
    int     led;                /* the card on the table, or -1 */
    int     lead_side;          /* who leads the current trick */
    int     turn;
    int     pts[2];
    bool    broken;
    int     last[2];            /* the trick just finished, or -1 */
    int     last_win;
} hr_state_t;

/* 🔑 EVERYTHING IS REPLAYED, NOTHING IS REMEMBERED. Hands, tricks, points and
 * the turn are all recomputed from (seed, plays) every time they are needed.
 * There is no incremental state to drift, so a frame that arrives twice, out of
 * order, or after a handset has been out of range for a minute all land in the
 * same place. */
static void hr_replay(hr_state_t *st)
{
    uint8_t deck[HR_CARDS];
    int     owner[HR_CARDS];
    bool    played[HR_CARDS];

    memset(st, 0, sizeof(*st));
    memset(played, 0, sizeof(played));
    st->led = -1;
    st->last[0] = st->last[1] = -1;
    st->last_win = -1;

    hr_deal(deck);
    for (int i = 0; i < HR_CARDS; i++) owner[deck[i]] = (i < HR_HAND) ? 0 : 1;

    /* Whoever was dealt the 2 of clubs opens. Derived, so neither handset has
     * to be told and neither can disagree. */
    st->lead_side = owner[HR_2C];

    int on_table = -1;
    for (int i = 0; i < hr_n && i < HR_CARDS; i++) {
        int c = hr_playlist[i];
        if (c >= HR_CARDS) break;
        played[c] = true;
        if (HR_SUIT(c) == 3) st->broken = true;

        if (on_table < 0) { on_table = c; continue; }

        /* Two cards on the table: the trick is over. */
        int follow_wins = (HR_SUIT(c) == HR_SUIT(on_table) &&
                           HR_RANK(c) > HR_RANK(on_table));
        int w = follow_wins ? 1 - st->lead_side : st->lead_side;
        st->pts[w]  += hr_pts(on_table) + hr_pts(c);
        st->last[0]  = on_table;
        st->last[1]  = c;
        st->last_win = w;
        st->lead_side = w;
        on_table = -1;
    }
    st->led  = on_table;
    st->turn = (on_table < 0) ? st->lead_side : 1 - st->lead_side;

    for (int c = 0; c < HR_CARDS; c++) {
        if (played[c]) continue;
        int o = owner[c];
        if (st->nhand[o] < HR_HAND) st->hand[o][st->nhand[o]++] = (uint8_t)c;
    }

    /* Shooting the moon, applied only once the hand is complete. */
    if (hr_n >= HR_CARDS) {
        for (int s = 0; s < 2; s++) {
            if (st->pts[s] != 26) continue;
            st->pts[s] = 0;
            st->pts[1 - s] = 26;
        }
    }
}

static bool hr_holds(const hr_state_t *st, int who, int suit)
{
    for (int i = 0; i < st->nhand[who]; i++)
        if (HR_SUIT(st->hand[who][i]) == suit) return true;
    return false;
}

/* Returns NULL if the card may be played, or the reason it may not — the
 * reason is what goes on screen, because "not that card" teaches nobody the
 * rules. */
static const char *hr_illegal(const hr_state_t *st, int who, int card)
{
    bool have = false;
    for (int i = 0; i < st->nhand[who]; i++)
        if (st->hand[who][i] == card) have = true;
    if (!have) return "not your card";

    if (st->led < 0) {                             /* leading */
        if (hr_n == 0 && card != HR_2C) return "the 2 of clubs opens";
        if (HR_SUIT(card) == 3 && !st->broken) {
            for (int i = 0; i < st->nhand[who]; i++)
                if (HR_SUIT(st->hand[who][i]) != 3)
                    return "hearts are not broken yet";
        }
        return NULL;
    }

    int led = HR_SUIT(st->led);
    if (hr_holds(st, who, led) && HR_SUIT(card) != led) {
        static char why[24];
        snprintf(why, sizeof(why), "follow %s", HR_SUITNAME[led]);
        return why;
    }
    if (hr_n < 2 && hr_pts(card)) {
        for (int i = 0; i < st->nhand[who]; i++)
            if (!hr_pts(st->hand[who][i])) return "no points on the first trick";
    }
    return NULL;
}

/* Rank above pip, which is what fits a tall narrow card and what stays readable
 * when all but the left edge of the card is covered by the next one. */
static void hr_card_text(int card, char *out, size_t n)
{
    if (card < 0 || card >= HR_CARDS) { snprintf(out, n, " "); return; }
    snprintf(out, n, "%s\n%s", HR_RANKS[HR_RANK(card)], HR_SUITS[HR_SUIT(card)]);
}

/* Red suits red, black suits black — the one convention every card player
 * already knows. The queen of spades is left black rather than highlighted:
 * spotting her is the game. */
static lv_color_t hr_card_colour(int card)
{
    if (card < 0 || card >= HR_CARDS) return HR_BLACK;
    int su = HR_SUIT(card);
    return (su == 1 || su == 3) ? HR_RED : HR_BLACK;
}

/* The machine's card. Two players and an open play list mean it can see the
 * whole position, so the difficulty is in what it CHOOSES to do with it.
 *
 * Hard is four rules a decent human player uses, in order:
 *   - Leading: the lowest card in the shortest suit that is not hearts, which
 *     keeps the lead cheap and runs a suit out.
 *   - Following and able to duck: the HIGHEST card that still loses. Shedding a
 *     big card without taking the points is most of the game.
 *   - Following and forced to win: the lowest card that wins.
 *   - Void: the queen of spades first, then the highest heart, then the biggest
 *     card in hand. A void suit is the only chance to hand the queen over.
 */
static int hr_ai_card(const hr_state_t *st)
{
    int legal[HR_HAND], n = 0;
    for (int i = 0; i < st->nhand[1]; i++) {
        int c = st->hand[1][i];
        if (!hr_illegal(st, 1, c)) legal[n++] = c;
    }
    if (!n) return -1;
    if (hr_mode == 0) return legal[rnd(n)];

    if (st->led < 0) {
        int counts[4] = { 0, 0, 0, 0 };
        for (int i = 0; i < st->nhand[1]; i++) counts[HR_SUIT(st->hand[1][i])]++;
        int best = -1, best_len = 99;
        for (int i = 0; i < n; i++) {
            int c = legal[i], su = HR_SUIT(c);
            int len = counts[su] + (su == 3 ? 20 : 0);   /* hearts last */
            if (len < best_len ||
                (len == best_len && best >= 0 && HR_RANK(c) < HR_RANK(best))) {
                best = c; best_len = len;
            }
        }
        return best >= 0 ? best : legal[0];
    }

    int led = HR_SUIT(st->led);
    int duck = -1, win = -1, dump = -1;
    for (int i = 0; i < n; i++) {
        int c = legal[i];
        if (HR_SUIT(c) == led) {
            if (HR_RANK(c) < HR_RANK(st->led)) {
                if (duck < 0 || HR_RANK(c) > HR_RANK(duck)) duck = c;
            } else if (win < 0 || HR_RANK(c) < HR_RANK(win)) {
                win = c;
            }
        } else {
            if (c == HR_QS) { dump = c; break; }
            if (dump < 0) dump = c;
            else if (hr_pts(c) > hr_pts(dump) ||
                     (hr_pts(c) == hr_pts(dump) && HR_RANK(c) > HR_RANK(dump)))
                dump = c;
        }
    }
    if (duck >= 0) return duck;
    if (dump >= 0 && win < 0) return dump;
    if (win  >= 0) return win;
    return legal[0];
}

static void hr_set_role(void)
{
    if (hr_peer < 0) { hr_me = 0; return; }
    if (!hr_starter) {
        uint16_t a = espnow_my_id(), b = espnow_peer_id(hr_peer);
        hr_starter = a < b ? a : b;
    }
    hr_me = (espnow_my_id() == hr_starter) ? 0 : 1;
}

static void hr_pick_default_peer(void)
{
    pick_default_peer(&hr_peer);
}

static void hr_broadcast(bool is_new)
{
    if (hr_mode != HR_VS) return;       /* a game against the machine is local */
    if (hr_peer < 0 || !scr_hr) return;
    espnow_game_t g = { 0 };
    g.game    = GAME_HEARTS;
    g.flags   = is_new ? GF_NEW : 0;
    g.cells   = (uint8_t)hr_n;
    g.gen     = hr_gen;
    g.starter = hr_starter;
    memcpy(g.board, hr_playlist, hr_n);
    /* The seed rides PAST the plays. A hand is at most 52 cards and the board
     * is 64 bytes, so those four are free and always readable — `cells` says
     * where the plays stop, so nothing has to be escaped or terminated. */
    g.board[52] = (uint8_t)(hr_seed);
    g.board[53] = (uint8_t)(hr_seed >> 8);
    g.board[54] = (uint8_t)(hr_seed >> 16);
    g.board[55] = (uint8_t)(hr_seed >> 24);
    /* And the session totals past those, device-indexed, for the same reason
     * Battleship carries its wins: a hand whose last frame was lost is a hand
     * this end would otherwise never score at all. */
    g.board[56] = (uint8_t)hr_total[0];
    g.board[57] = (uint8_t)(hr_total[0] >> 8);
    g.board[58] = (uint8_t)hr_total[1];
    g.board[59] = (uint8_t)(hr_total[1] >> 8);
    hr_last_tx_us = esp_timer_get_time();
    if (!espnow_game_send(hr_peer, &g) && hr_status)
        lv_label_set_text(hr_status, "send failed");
}

/* Add the finished hand to the running totals exactly once, however many times
 * the final frame arrives. */
/* Which half of hr_total[] is mine. Against the machine I am always 0; in VS
 * it comes from the device ids and never from the seat. */
static int hr_my_slot(void)
{
    if (hr_mode != HR_VS || hr_peer < 0) return 0;
    return id_slot(espnow_my_id(), espnow_peer_id(hr_peer));
}

static void hr_score_hand(const hr_state_t *st)
{
    if (hr_scored_gen == hr_gen) return;
    hr_scored_gen = hr_gen;
    /* st->pts is indexed by SEAT (0 = whoever was dealt the starter's half);
     * hr_total is indexed by device. This line is the whole fix. */
    int mine = hr_my_slot();
    hr_total[mine]     += st->pts[hr_me];
    hr_total[1 - mine] += st->pts[1 - hr_me];
    applog("[hr] hand over: %d - %d, session %d - %d",
           st->pts[hr_me], st->pts[1 - hr_me],
           hr_total[mine], hr_total[1 - mine]);

    /* In hearts the LOW score wins, which is exactly the rule a child gets
     * wrong, so the banner says it rather than leaving two numbers to compare. */
    int me = hr_total[mine], them = hr_total[1 - mine];
    const char *they = (hr_mode == HR_VS && hr_peer >= 0)
                       ? espnow_peer_name(hr_peer) : "the machine";
    char t[64];
    if (me < them)      snprintf(t, sizeof(t), "YOU WIN\n%d - %d, low score wins", me, them);
    else if (them < me) snprintf(t, sizeof(t), "%s WINS\n%d - %d, low score wins", they, them, me);
    else                snprintf(t, sizeof(t), "A DRAW\n%d - %d", me, them);
    banner_set(&hr_banner, scr_hr, t,
               me < them ? C_GREEN : (them < me ? C_RED : C_AMBER));
}

static void hr_apply(const espnow_game_t *g)
{
    if (g->game != GAME_HEARTS || g->cells > HR_CARDS) return;
    hr_mode = HR_VS;                    /* somebody is dealing at us */
    if (g->gen < hr_gen) return;

    uint32_t seed = (uint32_t)g->board[52]        |
                    ((uint32_t)g->board[53] << 8) |
                    ((uint32_t)g->board[54] << 16)|
                    ((uint32_t)g->board[55] << 24);

    if (g->gen == hr_gen) {
        /* 🔑 TWO HANDSETS CAN DEAL AT THE SAME MOMENT. Both press the game
         * button, both bump to the same generation, and each holds a different
         * shuffle. Higher seed wins — both boards compare the same two numbers
         * and reach the same answer with nothing exchanged, which is the same
         * trick the roles use. */
        if (seed != hr_seed) {
            if (seed < hr_seed) return;
        } else if (g->cells < hr_n) {
            return;                       /* a stale replay of our own hand */
        }
    }

    hr_gen     = g->gen;
    hr_starter = g->starter;
    hr_seed    = seed;
    hr_me      = (espnow_my_id() == hr_starter) ? 0 : 1;
    hr_n       = g->cells;
    memcpy(hr_playlist, g->board, hr_n);
    hr_over    = false;

    int theirs[2] = { g->board[56] | (g->board[57] << 8),
                      g->board[58] | (g->board[59] << 8) };
    score_merge(hr_total, theirs);
    banner_set(&hr_banner, scr_hr, NULL, C_INK);

    hr_state_t st;
    hr_replay(&st);
    if (hr_n >= HR_CARDS) { hr_score_hand(&st); hr_over = true; }
    hr_paint();
}

static void hr_new_hand(void)
{
    hr_gen++;
    hr_seed = esp_random() | 1u;          /* xorshift dies on a zero seed */
    hr_n    = 0;
    hr_over = false;
    banner_set(&hr_banner, scr_hr, NULL, C_INK);

    if (hr_mode != HR_VS) {
        /* Against the machine there is nobody to agree with, so the halves are
         * simply mine and its. Who LEADS still comes from the 2 of clubs. */
        hr_me      = 0;
        hr_starter = espnow_my_id();
        hr_paint();
        hr_ai_arm();
        return;
    }

    hr_pick_default_peer();
    if (hr_peer < 0) { hr_status_set("nobody in range"); return; }

    uint16_t me = espnow_my_id(), them = espnow_peer_id(hr_peer);
    /* The deal alternates. Which half a player gets is the only thing `starter`
     * decides here. */
    if (!hr_starter) hr_starter = me < them ? me : them;
    else             hr_starter = (hr_starter == me) ? them : me;
    hr_me = (me == hr_starter) ? 0 : 1;
    hr_paint();
    hr_broadcast(true);
}

/* 🔑 THE MACHINE PLAYS ON A TIMER, LIKE CONNECT 4'S. Not for the thinking time
 * — this AI is a handful of comparisons — but because two of its cards can fall
 * in a row when it wins a trick and leads the next one. Played instantly, the
 * first would never be seen. Half a second is long enough to watch and short
 * enough not to feel stuck. */
static void hr_ai_tick(lv_timer_t *t)
{
    (void)t;
    tmr_stop();
    if (hr_mode == HR_VS || hr_over) return;
    if (!scr_hr || lv_scr_act() != scr_hr) return;

    hr_state_t st;
    hr_replay(&st);
    if (st.turn != 1) return;

    int c = hr_ai_card(&st);
    if (c < 0) return;
    hr_playlist[hr_n++] = (uint8_t)c;
    hr_replay(&st);
    if (hr_n >= HR_CARDS) { hr_score_hand(&st); hr_over = true; }
    hr_paint();
    hr_ai_arm();                       /* it may have won the trick and lead */
}

static void hr_ai_arm(void)
{
    if (hr_mode == HR_VS || hr_over) return;
    hr_state_t st;
    hr_replay(&st);
    if (st.turn == 1) tmr_start(hr_ai_tick, 500);
}

static void hr_status_set(const char *override)
{
    if (!hr_status) return;
    if (hr_score_lbl)
        lv_label_set_text_fmt(hr_score_lbl, "%d-%d",
                              hr_total[hr_my_slot()], hr_total[1 - hr_my_slot()]);
    if (override) { lv_label_set_text(hr_status, override); return; }
    if (hr_mode == HR_VS && hr_peer < 0) {
        lv_label_set_text(hr_status, "nobody in range");
        return;
    }

    hr_state_t st;
    hr_replay(&st);
    if (hr_over) {
        lv_label_set_text_fmt(hr_status, "Hand over  %d-%d  -  tap for a new deal",
                              st.pts[hr_me], st.pts[1 - hr_me]);
        return;
    }
    if (hr_mode != HR_VS) {
        lv_label_set_text_fmt(hr_status, "%s  -  you %d, me %d",
                              st.turn == hr_me ? "Your card" : "thinking...",
                              st.pts[hr_me], st.pts[1 - hr_me]);
        return;
    }
    const char *pn = espnow_peer_name(hr_peer);
    lv_label_set_text_fmt(hr_status, "%s  -  %d pts  vs %s",
                          st.turn == hr_me ? "Your card" : "Their card",
                          st.pts[hr_me], pn ? pn : "?");
}

static void hr_paint(void)
{
    if (!scr_hr) return;                 /* a frame can arrive first — see c4 */

    hr_state_t st;
    hr_replay(&st);

    char txt[8];
    /* The table: what is led, and — once a trick closes — what took it. Two
     * players means a trick is gone in one tap, and a card that vanishes the
     * instant it is beaten is a card nobody saw. */
    int show[2] = { -1, -1 };
    const char *note = "";
    if (st.led >= 0) {
        show[0] = st.led;
        note = (st.lead_side == hr_me) ? "you led" : "they led";
    } else if (st.last_win >= 0) {
        show[0] = st.last[0];
        show[1] = st.last[1];
        note = (st.last_win == hr_me) ? "you took it" : "they took it";
    } else {
        note = "lead the 2 of clubs";
    }
    for (int i = 0; i < 2; i++) {
        lv_obj_t *l = lv_obj_get_child(hr_trick[i], 0);
        hr_card_text(show[i], txt, sizeof(txt));
        lv_label_set_text(l, show[i] >= 0 ? txt : "");
        lv_obj_set_style_text_color(l, hr_card_colour(show[i]), 0);
        /* An empty place on the table is an empty PLACE, not a blank card. */
        lv_obj_set_style_bg_color(hr_trick[i], show[i] >= 0 ? HR_FACE : C_PANEL, 0);
        lv_obj_set_style_border_width(hr_trick[i], show[i] >= 0 ? 1 : 0, 0);
    }
    if (hr_trick_note) lv_label_set_text(hr_trick_note, note);

    /* 🔑 THE HAND IS DEALT INTO FOUR ROWS, ONE PER SUIT, AND FANNED.
     *
     * hr_replay() hands back the cards in index order, which is already suit
     * then rank — so walking it in order fills clubs, diamonds, spades and
     * hearts in turn, and each row comes out sorted with no sorting step.
     *
     * The step shrinks to fit: seven cards of one suit sit comfortably at 22px
     * apart, thirteen do not, and a hand that runs off the right edge is a hand
     * with cards nobody can reach. */
    int rown[HR_ROWS] = { 0, 0, 0, 0 };
    for (int i = 0; i < st.nhand[hr_me]; i++) rown[HR_SUIT(st.hand[hr_me][i])]++;

    int slot = 0;
    for (int su = 0; su < HR_ROWS; su++) {
        int n = rown[su];
        int step = HR_STEP;
        if (n > 1) {
            int fit = (LCD_W - 16 - HR_CW) / (n - 1);
            if (fit < step) step = fit < 8 ? 8 : fit;
        }
        for (int k = 0; k < n; k++, slot++) {
            int card = st.hand[hr_me][slot];
            hr_slot_card[slot] = card;
            lv_obj_t *c = hr_slot[slot];
            lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(c, 8 + k * step, hr_row_y + su * (HR_CH + 4));

            bool ok = (st.turn == hr_me) && !hr_illegal(&st, hr_me, card);
            lv_obj_set_style_bg_color(c, ok ? HR_FACE : HR_FACE_D, 0);

            lv_obj_t *l = lv_obj_get_child(c, 0);
            hr_card_text(card, txt, sizeof(txt));
            lv_label_set_text(l, txt);
            /* A card that cannot be played keeps its colour but loses its
             * paper: dimmed, never hidden. Half of learning Hearts is seeing
             * WHY the card you wanted is not allowed. */
            lv_obj_set_style_text_color(l, hr_card_colour(card), 0);
            lv_obj_set_style_text_opa(l, ok ? LV_OPA_COVER : LV_OPA_60, 0);
        }
    }
    for (int i = slot; i < HR_SLOTS; i++) {
        hr_slot_card[i] = -1;
        lv_obj_add_flag(hr_slot[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int m = 0; m < 3; m++)
        if (hr_mbtn[m])
            lv_obj_set_style_bg_color(hr_mbtn[m], m == hr_mode ? C_BLUE : C_PANEL, 0);

    if (hr_peer_btn) {
        lv_obj_t *pl = lv_obj_get_child(hr_peer_btn, 0);
        const char *pn = (hr_peer >= 0) ? espnow_peer_name(hr_peer) : NULL;
        if (pl) lv_label_set_text_fmt(pl, "vs: %s", pn ? pn : "-");
        if (hr_mode == HR_VS) lv_obj_clear_flag(hr_peer_btn, LV_OBJ_FLAG_HIDDEN);
        else                  lv_obj_add_flag(hr_peer_btn, LV_OBJ_FLAG_HIDDEN);
    }
    hr_status_set(NULL);
}

static void hr_card_clicked(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);

    if (hr_over) { hr_new_hand(); return; }
    if (hr_mode == HR_VS) {
        hr_pick_default_peer();
        if (hr_peer < 0) { hr_status_set("nobody in range"); return; }
        hr_set_role();
    }

    int card = (slot >= 0 && slot < HR_SLOTS) ? hr_slot_card[slot] : -1;
    if (card < 0) return;

    hr_state_t st;
    hr_replay(&st);
    if (st.turn != hr_me)  { hr_status_set("not your turn"); return; }
    const char *why = hr_illegal(&st, hr_me, card);
    if (why) { hr_status_set(why); return; }

    hr_playlist[hr_n++] = (uint8_t)card;
    hr_replay(&st);
    if (hr_n >= HR_CARDS) { hr_score_hand(&st); hr_over = true; }
    hr_paint();
    hr_broadcast(false);
    hr_ai_arm();
}

static void hr_mode_clicked(lv_event_t *e)
{
    hr_mode = (int)(intptr_t)lv_event_get_user_data(e);
    hr_total[0] = hr_total[1] = 0;      /* a different opponent is a new score */
    hr_starter  = 0;
    tmr_stop();
    hr_new_hand();
}

static void hr_peer_clicked(lv_event_t *e)
{
    (void)e;
    hr_peer = play_next(hr_peer);
    if (hr_peer < 0) hr_status_set("nobody in range");
    hr_set_role();
    hr_paint();
}

static void hr_back(lv_event_t *e)
{
    (void)e;
    tmr_stop();
    if (!scr_menu_ready()) return;
    hr_banner = NULL;               /* freed with the screen; see leave_to */
    leave_to(scr_menu, &scr_hr);
}

static void hr_build(void)
{
    hr_banner = NULL;
    scr_hr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_hr, C_BG, 0);
    lv_obj_clear_flag(scr_hr, LV_OBJ_FLAG_SCROLLABLE);
    hr_score_lbl = mk_hdr(scr_hr, "Hearts", hr_back);

    static const char *names[3] = { "Easy", "Hard", "VS" };
    for (int m = 0; m < 3; m++) {
        hr_mbtn[m] = btn(scr_hr, 40, 22, C_PANEL, C_INK, &lv_font_montserrat_12,
                         names[m], hr_mode_clicked, (void *)(intptr_t)m);
        lv_obj_align(hr_mbtn[m], LV_ALIGN_TOP_LEFT, 6 + m * 42, HDR_H + 4);
    }

    hr_peer_btn = btn(scr_hr, 90, 22, C_PANEL, C_BLUE, &lv_font_montserrat_12,
                      "vs: -", hr_peer_clicked, NULL);
    lv_obj_align(hr_peer_btn, LV_ALIGN_TOP_RIGHT, -6, HDR_H + 4);

    hr_status = lbl(scr_hr, &lv_font_montserrat_12, C_MUTED, "");
    lv_label_set_long_mode(hr_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(hr_status, LCD_W - 16);
    lv_obj_align(hr_status, LV_ALIGN_TOP_LEFT, 8, HDR_H + 30);

    /* The table. Two card-shaped places, filled when there is a card in them. */
    for (int i = 0; i < 2; i++) {
        hr_trick[i] = rect(scr_hr, HR_TCW, HR_TCH, C_PANEL);
        lv_obj_set_pos(hr_trick[i], 8 + i * (HR_TCW + 8), HDR_H + 50);
        lv_obj_set_style_radius(hr_trick[i], 4, 0);
        lv_obj_set_style_border_color(hr_trick[i], HR_EDGE, 0);
        lv_obj_t *l = lbl(hr_trick[i], &lv_font_cards_26, HR_BLACK, "");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }
    hr_trick_note = lbl(scr_hr, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_set_pos(hr_trick_note, 8 + 2 * (HR_TCW + 8) + 6, HDR_H + 70);

    /* The hand. Every card exists from the start and is moved and hidden by
     * hr_paint() — building 26 objects on every repaint would show. */
    for (int i = 0; i < HR_SLOTS; i++) {
        lv_obj_t *c = rect(scr_hr, HR_CW, HR_CH, HR_FACE);
        lv_obj_set_style_radius(c, 4, 0);
        lv_obj_set_style_border_width(c, 1, 0);
        lv_obj_set_style_border_color(c, HR_EDGE, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, hr_card_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *l = lbl(c, &lv_font_cards_16, HR_BLACK, "");
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        /* Rank and pip sit against the LEFT edge, because the left edge is the
         * only part of a fanned card that stays visible. */
        lv_obj_align(l, LV_ALIGN_TOP_LEFT, 3, 2);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        hr_slot[i] = c;
        hr_slot_card[i] = -1;
    }
}

static void hr_show(lv_event_t *e)
{
    (void)e;
    if (!scr_hr) hr_build();
    tmr_stop();
    if (hr_mode == HR_VS) { hr_pick_default_peer(); hr_set_role(); }
    /* A hand already in progress is joined, not thrown away: the other handset
     * may have dealt while this one was on the menu. */
    if (hr_n == 0 || hr_over || !hr_seed) hr_new_hand();
    else                                { hr_paint(); hr_ai_arm(); }
    lv_scr_load(scr_hr);
    applog("[games] hearts, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static bool hr_net_active(void)
{
    return hr_mode == HR_VS && hr_peer >= 0 && scr_hr && lv_scr_act() == scr_hr;
}

static void hr_heartbeat(void)
{
    if (!hr_net_active()) return;
    /* 🚨 OPENING HEARTS ALONE USED TO BE A DEAD END. hr_show() deals only if
     * there is somebody to deal against, so a handset that got there first had
     * no hand - and no hand means no card to tap, which means no way to ask for
     * one. The screen sat empty for as long as it was open even after the other
     * handset appeared. Deal on the first tick that has a peer instead. */
    if (!hr_seed) { hr_new_hand(); return; }   /* a peer turned up after we opened */
    if (hr_over) return;
    if (esp_timer_get_time() - hr_last_tx_us < 2000000) return;
    hr_broadcast(hr_n == 0);
}

/* ------------------------------------------------------------------ *
 * the menu
 * ------------------------------------------------------------------ */

/* flamingo_show() takes no event, so the menu table needs a shim. */
static void flamingo_clicked(lv_event_t *e) { (void)e; flamingo_show(); }
/* Same shim, same reason. */
static void tanks_clicked(lv_event_t *e) { (void)e; tanks_show(); }

static void menu_build(void);

/* Games are children of the menu conceptually, but LVGL screens are peers, so
 * the menu has to be alive before a game can be torn down onto it. */
static bool scr_menu_ready(void)
{
    if (!scr_menu) menu_build();
    return scr_menu != NULL;
}

static void menu_back(lv_event_t *e)
{
    tmr_stop();
    ui_show_player();
    if (scr_menu) { lv_obj_del_async(scr_menu); scr_menu = NULL; }
    applog("[games] closed, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* ------------------------------------------------------------------ *
 *  Solitaire (Klondike) — Kid 4 asked for "a good by yourself game"
 *
 *  Draw ONE, not three. Draw three is the version adults know, and it is the
 *  version that deals a hand you cannot win perhaps a third of the time. A
 *  child who loses to the deal rather than to a mistake stops playing, so this
 *  deals one at a time and the stock recycles forever.
 *
 *  🚨 TAP-TAP, NEVER DRAG. Tap the card you want, tap where it goes. Dragging
 *  a 30px card with a finger on a resistive-feeling panel misses constantly,
 *  and this file already learned once that a target a child can miss reads as
 *  a broken device rather than a missed press.
 * ------------------------------------------------------------------ */

#define SOL_STOCK  0
#define SOL_WASTE  1
#define SOL_FND    2                    /* 2..5 */
#define SOL_TAB    6                    /* 6..12 */
#define SOL_PILES  13

#define SOL_CW     30
#define SOL_CH     40
#define SOL_X0     3
#define SOL_DX     34
#define SOL_TOP_Y  (HDR_H + 6)
#define SOL_TAB_Y  (SOL_TOP_Y + SOL_CH + 10)
#define SOL_BOT    296                  /* leave the status line its row */

#define SOL_SUIT(c) ((c) / 13)
#define SOL_RANK(c) ((c) % 13)          /* 0 = A, 12 = K */
#define SOL_RED(c)  (SOL_SUIT(c) == 1 || SOL_SUIT(c) == 2)

static const char *SOL_R[13] = { "A","2","3","4","5","6","7","8","9","10","J","Q","K" };
static const char *SOL_S[4]  = { "\xE2\x99\xA0", "\xE2\x99\xA5",
                                 "\xE2\x99\xA6", "\xE2\x99\xA3" };

static lv_obj_t *scr_sol;
static uint8_t   sol_p[SOL_PILES][52];
static uint8_t   sol_np[SOL_PILES];
static uint8_t   sol_up[SOL_PILES];     /* how many at the TOP are face up */
static int       sol_sel_pile = -1, sol_sel_idx = -1;
static lv_obj_t *sol_card[52];          /* pool; repositioned every paint */
static int       sol_card_pile[52], sol_card_idx[52];
static lv_obj_t *sol_slot[SOL_PILES];
static lv_obj_t *sol_status, *sol_score_lbl, *sol_banner;
static int       sol_moves;
static bool      sol_won;

static void sol_paint(void);

/* ------------------------------------------------------------------ *
 *  Solitaire: undo, and a game that survives leaving the screen
 *
 *  🔑 SNAPSHOT THE WHOLE STATE, DO NOT INVERT THE MOVE. A solitaire move has
 *  side effects that are easy to forget on the way back: moving the last card
 *  off a tabelau pile FLIPS the one underneath face up, and the stock recycle
 *  reverses the whole waste. An inverse-move undo has to remember each of
 *  those; a snapshot cannot get them wrong. The entire game is 706 bytes, so
 *  the cheap correct thing is also the cheap thing.
 *
 *  🔑 AND IT IS THE SAME BYTES BOTH TIMES. Undo and save/resume want exactly
 *  one structure, so there is one definition of "what a game is" rather than
 *  two that drift.
 * ------------------------------------------------------------------ */

#define SOL_SAVE_VER  1
#define SOL_UNDO_MAX  8         /* ~5.7KB, and it lives in PSRAM */

typedef struct __attribute__((packed)) {
    uint8_t  ver;
    uint8_t  won;
    uint16_t moves;
    uint8_t  np[SOL_PILES];
    uint8_t  up[SOL_PILES];
    uint8_t  p[SOL_PILES][52];
} sol_state_t;

static sol_state_t *sol_undo;        /* ring of SOL_UNDO_MAX, or NULL */
static int          sol_undo_n;      /* how many are valid */
static int          sol_undo_head;   /* next slot to write */
static lv_obj_t    *sol_undo_btn;

static void sol_capture(sol_state_t *s)
{
    s->ver   = SOL_SAVE_VER;
    s->won   = sol_won ? 1 : 0;
    s->moves = (uint16_t)sol_moves;
    memcpy(s->np, sol_np, sizeof(s->np));
    memcpy(s->up, sol_up, sizeof(s->up));
    memcpy(s->p,  sol_p,  sizeof(s->p));
}

static bool sol_restore(const sol_state_t *s)
{
    if (s->ver != SOL_SAVE_VER) return false;
    /* Refuse a corrupt save rather than paint an impossible board: a pile
     * count past 52 would index off the end of sol_p on the very next paint. */
    for (int i = 0; i < SOL_PILES; i++)
        if (s->np[i] > 52 || s->up[i] > 52) return false;
    sol_won   = s->won != 0;
    sol_moves = s->moves;
    memcpy(sol_np, s->np, sizeof(sol_np));
    memcpy(sol_up, s->up, sizeof(sol_up));
    memcpy(sol_p,  s->p,  sizeof(sol_p));
    sol_sel_pile = sol_sel_idx = -1;
    return true;
}

static void sol_undo_reset(void) { sol_undo_n = 0; sol_undo_head = 0; }

/* Called BEFORE every state change. Silently does nothing if PSRAM was not
 * available — undo is a convenience and must never be the reason a game will
 * not start. */
static void sol_undo_push(void)
{
    if (!sol_undo) {
        sol_undo = heap_caps_malloc(sizeof(sol_state_t) * SOL_UNDO_MAX,
                                    MALLOC_CAP_SPIRAM);
        if (!sol_undo) return;
    }
    sol_capture(&sol_undo[sol_undo_head]);
    sol_undo_head = (sol_undo_head + 1) % SOL_UNDO_MAX;
    if (sol_undo_n < SOL_UNDO_MAX) sol_undo_n++;
}

static bool sol_undo_pop(void)
{
    if (!sol_undo || sol_undo_n <= 0) return false;
    sol_undo_head = (sol_undo_head + SOL_UNDO_MAX - 1) % SOL_UNDO_MAX;
    sol_undo_n--;
    /* A restore that fails still consumed the slot, which is correct: a
     * snapshot we cannot trust must not be offered again. */
    return sol_restore(&sol_undo[sol_undo_head]);
}

/* ---- the game that survives leaving the screen ---- */

static void sol_save(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) != ESP_OK) return;
    /* A finished game is not worth resuming, and restoring one would show a
     * won banner to somebody who just tapped Solitaire expecting to play. */
    if (sol_won) {
        nvs_erase_key(h, "sol");
    } else {
        sol_state_t s;
        sol_capture(&s);
        nvs_set_blob(h, "sol", &s, sizeof(s));
    }
    nvs_commit(h);
    nvs_close(h);
}

static bool sol_load(void)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READONLY, &h) != ESP_OK) return false;
    sol_state_t s;
    size_t n = sizeof(s);
    bool ok = nvs_get_blob(h, "sol", &s, &n) == ESP_OK && n == sizeof(s);
    nvs_close(h);
    return ok && !s.won && sol_restore(&s);
}

static int sol_top(int p) { return sol_np[p] ? sol_p[p][sol_np[p] - 1] : -1; }

/* Foundations are not pre-assigned to suits: the first ace to arrive claims
 * the first empty pile. Four aces, four piles, so it always works out, and
 * nobody has to remember which corner hearts live in. */
static int sol_fnd_for(int card)
{
    for (int f = SOL_FND; f < SOL_FND + 4; f++)
        if (sol_np[f] && SOL_SUIT(sol_top(f)) == SOL_SUIT(card)) return f;
    if (SOL_RANK(card) != 0) return -1;         /* only an ace opens one */
    for (int f = SOL_FND; f < SOL_FND + 4; f++) if (!sol_np[f]) return f;
    return -1;
}

static bool sol_can_fnd(int card, int f)
{
    if (f < SOL_FND || f >= SOL_FND + 4) return false;
    if (!sol_np[f]) return SOL_RANK(card) == 0;
    int t = sol_top(f);
    return SOL_SUIT(t) == SOL_SUIT(card) && SOL_RANK(card) == SOL_RANK(t) + 1;
}

static bool sol_can_tab(int card, int t)
{
    if (!sol_np[t]) return SOL_RANK(card) == 12;   /* only a king opens a space */
    int d = sol_top(t);
    return SOL_RED(card) != SOL_RED(d) && SOL_RANK(card) + 1 == SOL_RANK(d);
}

/* A pile whose top card is face down has just lost its cover. */
static void sol_flip_exposed(void)
{
    for (int t = SOL_TAB; t < SOL_PILES; t++)
        if (sol_np[t] && sol_up[t] == 0) sol_up[t] = 1;
}

static void sol_check_won(void)
{
    int n = 0;
    for (int f = SOL_FND; f < SOL_FND + 4; f++) n += sol_np[f];
    if (n < 52) return;
    sol_won = true;
    hs_set("sol_w", hs_get("sol_w") + 1);
    banner_set(&sol_banner, scr_sol, "YOU WIN\ntap to deal again", C_GREEN);
    if (sol_status) lv_label_set_text(sol_status, "all four suits home!");
}

static void sol_deal(void)
{
    uint8_t deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (uint8_t)i;
    for (int i = 51; i > 0; i--) {          /* nobody else has to agree on this
                                             * shuffle, so esp_random is fine */
        int j = rnd(i + 1);
        uint8_t t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    memset(sol_np, 0, sizeof(sol_np));
    memset(sol_up, 0, sizeof(sol_up));
    sol_sel_pile = sol_sel_idx = -1;
    sol_moves = 0;
    sol_won = false;
    sol_undo_reset();                   /* you cannot undo past a new deal */
    banner_set(&sol_banner, scr_sol, NULL, C_INK);

    int k = 0;
    for (int c = 0; c < 7; c++)
        for (int r = 0; r <= c; r++)
            sol_p[SOL_TAB + c][sol_np[SOL_TAB + c]++] = deck[k++];
    for (int c = 0; c < 7; c++) sol_up[SOL_TAB + c] = 1;
    while (k < 52) sol_p[SOL_STOCK][sol_np[SOL_STOCK]++] = deck[k++];
    sol_up[SOL_STOCK] = 0;
}

/* Move `cnt` cards from the top of `from` to `to`. */
static void sol_move(int from, int to, int cnt)
{
    sol_undo_push();                    /* before the change, never after */
    int base = sol_np[from] - cnt;
    for (int i = 0; i < cnt; i++) sol_p[to][sol_np[to]++] = sol_p[from][base + i];
    sol_np[from] = (uint8_t)base;
    if (from >= SOL_TAB) {
        sol_up[from] = (uint8_t)(sol_up[from] > cnt ? sol_up[from] - cnt : 0);
    }
    if (to >= SOL_TAB)      sol_up[to] = (uint8_t)(sol_up[to] + cnt);
    else if (to == SOL_WASTE) sol_up[to] = 1;
    sol_moves++;
    /* "Kicked out" is not always a tidy exit — a sleep or a reset never reaches
     * sol_back(). Ten moves is a few seconds of play to lose at worst, and NVS
     * is wear-levelled; a 706-byte blob a few times a minute is nothing. */
    if (sol_moves % 10 == 0) sol_save();
    sol_flip_exposed();
    sol_sel_pile = sol_sel_idx = -1;
    sol_check_won();
}

/* Tapping the stock deals one; tapping it empty turns the waste back over.
 * The recycle is unlimited on purpose — see the file header. */
static void sol_stock_tap(void)
{
    if (sol_np[SOL_STOCK]) { sol_move(SOL_STOCK, SOL_WASTE, 1); return; }
    if (!sol_np[SOL_WASTE]) return;
    /* ⚠️ The other mutation point. The deal-one branch above returns through
     * sol_move(), which pushes its own snapshot — pushing here as well would
     * cost two undos for one tap. */
    sol_undo_push();
    while (sol_np[SOL_WASTE])
        sol_p[SOL_STOCK][sol_np[SOL_STOCK]++] = sol_p[SOL_WASTE][--sol_np[SOL_WASTE]];
    sol_up[SOL_WASTE] = 0;
    sol_sel_pile = sol_sel_idx = -1;
}

/* How many cards are selected, given a tap on card `idx` of `p`. */
static int sol_run_len(int p, int idx) { return sol_np[p] - idx; }

static void sol_try_drop(int dest)
{
    if (sol_sel_pile < 0) return;
    int cnt  = sol_run_len(sol_sel_pile, sol_sel_idx);
    int card = sol_p[sol_sel_pile][sol_sel_idx];

    if (dest >= SOL_FND && dest < SOL_FND + 4) {
        if (cnt != 1 || !sol_can_fnd(card, dest)) {
            if (sol_status) lv_label_set_text(sol_status, "cannot go there");
            return;
        }
        sol_move(sol_sel_pile, dest, 1);
    } else if (dest >= SOL_TAB) {
        if (!sol_can_tab(card, dest)) {
            if (sol_status) lv_label_set_text(sol_status,
                sol_np[dest] ? "other colour, one lower" : "only a king goes there");
            return;
        }
        sol_move(sol_sel_pile, dest, cnt);
    } else return;
    if (sol_status && !sol_won) lv_label_set_text(sol_status, "");
}

static void sol_tap_pile(int p)
{
    if (sol_won) { sol_deal(); sol_paint(); return; }
    if (p == SOL_STOCK) { sol_stock_tap(); sol_paint(); return; }

    /* Tapping the selected card again puts it down. */
    if (sol_sel_pile == p) { sol_sel_pile = sol_sel_idx = -1; sol_paint(); return; }
    if (sol_sel_pile >= 0) { sol_try_drop(p); sol_paint(); return; }

    if (!sol_np[p]) return;                 /* empty, and nothing in hand */
    if (p == SOL_WASTE || (p >= SOL_FND && p < SOL_FND + 4)) {
        sol_sel_pile = p; sol_sel_idx = sol_np[p] - 1;
    } else {
        sol_sel_pile = p; sol_sel_idx = sol_np[p] - 1;   /* top card by default */
    }
    sol_paint();
}

static void sol_tap_card(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    int p = sol_card_pile[slot], idx = sol_card_idx[slot];
    if (p < 0) return;
    if (sol_won) { sol_deal(); sol_paint(); return; }
    if (p == SOL_STOCK) { sol_stock_tap(); sol_paint(); return; }

    /* A tap on a face-down card is a tap on its pile. */
    bool face_up = (p >= SOL_TAB) ? (idx >= sol_np[p] - sol_up[p]) : true;
    if (!face_up) { sol_tap_pile(p); return; }

    if (sol_sel_pile >= 0 && sol_sel_pile != p) { sol_try_drop(p); sol_paint(); return; }
    if (sol_sel_pile == p && sol_sel_idx == idx) {
        /* Second tap on the same card sends it home if it can go. */
        int f = sol_fnd_for(sol_p[p][idx]);
        if (idx == sol_np[p] - 1 && f >= 0 && sol_can_fnd(sol_p[p][idx], f))
            sol_move(p, f, 1);
        else { sol_sel_pile = sol_sel_idx = -1; }
        sol_paint();
        return;
    }
    sol_sel_pile = p; sol_sel_idx = idx;
    sol_paint();
}

static void sol_tap_slot(lv_event_t *e)
{
    sol_tap_pile((int)(intptr_t)lv_event_get_user_data(e));
}

static void sol_face(lv_obj_t *o, int card, bool up, bool sel)
{
    if (!up) {
        lv_label_set_text(o, "");
        lv_obj_set_style_bg_color(o, C_BLUE, 0);
        lv_obj_set_style_border_color(o, C_LINE, 0);
        return;
    }
    char t[12];
    snprintf(t, sizeof(t), "%s%s", SOL_R[SOL_RANK(card)], SOL_S[SOL_SUIT(card)]);
    lv_label_set_text(o, t);
    lv_obj_set_style_text_color(o, SOL_RED(card) ? C_RED : lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_color(o, sel ? C_AMBER : C_INK, 0);
    lv_obj_set_style_border_color(o, sel ? C_AMBER : C_LINE, 0);
}

static void sol_paint(void)
{
    if (!scr_sol) return;
    int n = 0;
    for (int i = 0; i < 52; i++) sol_card_pile[i] = -1;

    /* Stock, waste and foundations show one card each — everything under the
     * top one is invisible, so it costs nothing to draw. */
    struct { int pile; int x; int y; } single[6] = {
        { SOL_STOCK,     SOL_X0,              SOL_TOP_Y },
        { SOL_WASTE,     SOL_X0 + SOL_DX,     SOL_TOP_Y },
        { SOL_FND + 0,   SOL_X0 + 3 * SOL_DX, SOL_TOP_Y },
        { SOL_FND + 1,   SOL_X0 + 4 * SOL_DX, SOL_TOP_Y },
        { SOL_FND + 2,   SOL_X0 + 5 * SOL_DX, SOL_TOP_Y },
        { SOL_FND + 3,   SOL_X0 + 6 * SOL_DX, SOL_TOP_Y },
    };
    for (int s = 0; s < 6; s++) {
        int p = single[s].pile;
        if (!sol_np[p]) continue;
        lv_obj_t *o = sol_card[n];
        sol_card_pile[n] = p; sol_card_idx[n] = sol_np[p] - 1;
        lv_obj_set_pos(o, single[s].x, single[s].y);
        sol_face(o, sol_top(p), p != SOL_STOCK,
                 sol_sel_pile == p && sol_sel_idx == sol_np[p] - 1);
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(o);
        n++;
    }

    for (int c = 0; c < 7 && n < 52; c++) {
        int p = SOL_TAB + c, cnt = sol_np[p];
        if (!cnt) continue;
        int down = cnt - sol_up[p];
        /* 🔑 THE PILE HAS TO FIT THE SCREEN, not the other way round. Six face
         * down and a run of thirteen at a fixed step runs off the bottom, and a
         * card a child cannot see is a card they cannot play. Squeeze the
         * face-up step until the tallest pile fits. */
        int dstep = 8, ustep = 15;
        int need = down * dstep + (sol_up[p] ? (sol_up[p] - 1) * ustep : 0) + SOL_CH;
        int room = SOL_BOT - SOL_TAB_Y;
        if (need > room && sol_up[p] > 1)
            ustep = (room - SOL_CH - down * dstep) / (sol_up[p] - 1);
        if (ustep < 6) { ustep = 6; }
        if (down * dstep + SOL_CH > room && down > 0)
            dstep = (room - SOL_CH) / down;
        if (dstep < 3) dstep = 3;

        int y = SOL_TAB_Y;
        for (int i = 0; i < cnt && n < 52; i++) {
            bool up = i >= down;
            lv_obj_t *o = sol_card[n];
            sol_card_pile[n] = p; sol_card_idx[n] = i;
            lv_obj_set_pos(o, SOL_X0 + c * SOL_DX, y);
            sol_face(o, sol_p[p][i], up,
                     sol_sel_pile == p && i >= sol_sel_idx);
            lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(o);
            y += up ? ustep : dstep;
            n++;
        }
    }
    for (int i = n; i < 52; i++) lv_obj_add_flag(sol_card[i], LV_OBJ_FLAG_HIDDEN);
    if (sol_banner) lv_obj_move_foreground(sol_banner);

    if (sol_score_lbl) {
        int home = 0;
        for (int f = SOL_FND; f < SOL_FND + 4; f++) home += sol_np[f];
        lv_label_set_text_fmt(sol_score_lbl, "%d/52", home);
    }
}

static void sol_back(lv_event_t *e)
{
    (void)e;
    tmr_stop();
    if (!scr_menu_ready()) return;
    sol_save();                         /* the whole point: leaving is not losing */
    sol_banner = NULL;
    leave_to(scr_menu, &scr_sol);
}

static void sol_deal_clicked(lv_event_t *e) { (void)e; sol_deal(); sol_paint(); }

/* Undo exists because the cards are 30px wide on a 240px screen and a thumb is
 * not. The builder, 2026-09-08: "if your tap missis the intended target then you can do
 * over". It steps back through whole board states, so it undoes the flip a move
 * exposed and the stock recycle as readily as an ordinary move. */
static void sol_undo_clicked(lv_event_t *e)
{
    (void)e;
    if (sol_undo_pop()) {
        banner_set(&sol_banner, scr_sol, NULL, C_INK);   /* a win can be undone too */
        if (sol_status) lv_label_set_text(sol_status, "");
    } else if (sol_status) {
        lv_label_set_text(sol_status, "nothing to undo");
    }
    sol_paint();
}

static void sol_build(void)
{
    sol_banner = NULL;
    scr_sol = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_sol, C_BG, 0);
    lv_obj_clear_flag(scr_sol, LV_OBJ_FLAG_SCROLLABLE);
    sol_score_lbl = mk_hdr(scr_sol, "Solitaire", sol_back);

    /* Empty places stay visible as outlines, so a space you can drop a king on
     * looks like somewhere rather than like nothing. */
    for (int p = 0; p < SOL_PILES; p++) {
        int x, y;
        if (p == SOL_STOCK)      { x = SOL_X0;                  y = SOL_TOP_Y; }
        else if (p == SOL_WASTE) { x = SOL_X0 + SOL_DX;         y = SOL_TOP_Y; }
        else if (p < SOL_TAB)    { x = SOL_X0 + (3 + p - SOL_FND) * SOL_DX; y = SOL_TOP_Y; }
        else                     { x = SOL_X0 + (p - SOL_TAB) * SOL_DX;     y = SOL_TAB_Y; }
        lv_obj_t *o = rect(scr_sol, SOL_CW, SOL_CH, C_BG);
        lv_obj_set_style_border_color(o, C_LINE, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_radius(o, 3, 0);
        lv_obj_set_pos(o, x, y);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(o, sol_tap_slot, LV_EVENT_CLICKED, (void *)(intptr_t)p);
        sol_slot[p] = o;
    }

    /* One label per card, not a container plus a label: 52 cards is already the
     * biggest object count in this file and the screen has to be built inside
     * the same internal RAM the player wants a DMA buffer from. */
    for (int i = 0; i < 52; i++) {
        lv_obj_t *o = lv_label_create(scr_sol);
        lv_obj_set_size(o, SOL_CW, SOL_CH);
        lv_obj_set_style_text_font(o, &lv_font_cards_16, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(o, 3, 0);
        lv_obj_set_style_border_width(o, 1, 0);
        lv_obj_set_style_pad_left(o, 3, 0);
        lv_obj_set_style_pad_top(o, 2, 0);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(o, sol_tap_card, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        sol_card[i] = o;
    }

    lv_obj_t *nb = btn(scr_sol, 56, 20, C_PANEL, C_INK, &lv_font_montserrat_12,
                       "Deal", sol_deal_clicked, NULL);
    lv_obj_align(nb, LV_ALIGN_BOTTOM_RIGHT, -4, -2);

    /* Left of Deal, with a real gap between them: these two sit at the bottom
     * edge, so grow_screen_edges() will enlarge both click areas and it clamps
     * growth to half the gap to the nearest sibling. Butted together they would
     * each steal from the other, and an undo that deals a new game is the worst
     * possible mis-tap on this screen. */
    sol_undo_btn = btn(scr_sol, 56, 20, C_PANEL, C_INK, &lv_font_montserrat_12,
                       "Undo", sol_undo_clicked, NULL);
    lv_obj_align(sol_undo_btn, LV_ALIGN_BOTTOM_RIGHT, -66, -2);

    /* ⚠️ BOUNDED, BECAUSE IT SHARES A ROW WITH TWO BUTTONS. An auto-sizing
     * label grew to ~200px on a 240px screen and ran underneath Deal — already
     * true before Undo was added, and worse after. 140px stops clear of both,
     * and every message below is written to fit it. */
    sol_status = lbl(scr_sol, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_set_width(sol_status, 140);
    lv_label_set_long_mode(sol_status, LV_LABEL_LONG_DOT);
    lv_obj_align(sol_status, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

static void sol_show(lv_event_t *e)
{
    (void)e;
    if (!scr_sol) sol_build();
    /* 🔑 RESUME, DO NOT RE-DEAL. This used to call sol_deal() unconditionally,
     * so stepping out to answer a message threw the game away. The builder, 2026-09-08:
     * "if you hop out of get kicked out you can go back in and keep playing".
     * A save that fails to load falls through to a fresh deal, so the worst
     * case is the old behaviour. */
    if (sol_load()) {
        sol_undo_reset();               /* the snapshots did not survive either */
        applog("[games] solitaire resumed at move %d", sol_moves);
    } else {
        sol_deal();
    }
    sol_paint();
    lv_scr_load(scr_sol);
    applog("[games] solitaire, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* ------------------------------------------------------------------ *
 *  Uno — up to four handsets, which is what Kid 4 actually asked for
 *
 *  🔑 THE WHOLE STATE IS WHO OWNS WHICH CARD.
 *
 *  Hearts syncs a list of plays and replays them. Uno cannot: a turn may be a
 *  draw, draws are frequent, and a four-player game's move list runs past the
 *  64 bytes a game frame has. So Uno sends the position instead of the history
 *  — three bits per card saying whose hand it is in — which is 41 bytes and
 *  fixed size no matter how long the game runs.
 *
 *  That keeps the property that matters: the frame is a complete statement of
 *  the game, so one arriving twice, late, or after a handset has been out of
 *  range for a minute all land in the same place. Nothing is incremental and
 *  there is nothing to drift.
 *
 *  🚨 SEATS ARE DERIVED FROM THE ROSTER, NEVER NEGOTIATED. The dealer says WHO
 *  is playing, because that is a fact about the room that no handset can work
 *  out alone. Everything after that is computed: sort the ids ascending and
 *  that is the seating, identically on every handset. See starter_fix() above
 *  for what happens when a seat is remembered instead of derived.
 *
 *  Hidden hands: every handset holds all four hands and shows only its own,
 *  exactly as Hearts already does with its two.
 * ------------------------------------------------------------------ */

#define UNO_CARDS   108
#define UNO_MAXP    4
#define UNO_HAND0   7                   /* cards dealt to each player */
#define UNO_DRAW    4                   /* owner value: the draw pile */
#define UNO_DISC    5                   /* owner value: the discard pile */
#define UNO_WIRE    60                  /* bytes of `board` this game uses */

/* 0..99 are the coloured cards, 25 per colour; 100..103 wild; 104..107 wild
 * draw four. Within a colour: 0 is the single zero, 1..18 are two each of 1-9,
 * then two skips, two reverses, two draw-twos. */
#define UNO_COL(c)   ((c) < 100 ? (c) / 25 : 4)       /* 4 = wild, no colour */
#define UNO_IS_WILD(c) ((c) >= 100)
#define UNO_W4(c)    ((c) >= 104)

enum { UNO_R_NUM = 0, UNO_R_SKIP, UNO_R_REV, UNO_R_D2, UNO_R_WILD, UNO_R_W4 };

static int uno_rank(int c)              /* 0..9 for a number card */
{
    if (c >= 104) return -1;
    if (c >= 100) return -1;
    int w = c % 25;
    if (w == 0) return 0;
    if (w <= 18) return (w - 1) / 2 + 1;
    return -1;
}
static int uno_kind(int c)
{
    if (c >= 104) return UNO_R_W4;
    if (c >= 100) return UNO_R_WILD;
    int w = c % 25;
    if (w <= 18) return UNO_R_NUM;
    if (w <= 20) return UNO_R_SKIP;
    if (w <= 22) return UNO_R_REV;
    return UNO_R_D2;
}

static const char *UNO_CN[4] = { "RED", "YELLOW", "GREEN", "BLUE" };
static lv_color_t uno_colour_of(int k)
{
    switch (k) {
        case 0:  return lv_color_hex(0xE03B3B);
        case 1:  return lv_color_hex(0xF2C21A);
        case 2:  return lv_color_hex(0x3FAE55);
        default: return lv_color_hex(0x3B7BE0);
    }
}
static void uno_card_text(int c, char *out, size_t n)
{
    int k = uno_kind(c);
    switch (k) {
        case UNO_R_SKIP: snprintf(out, n, "X");  return;
        case UNO_R_REV:  snprintf(out, n, "R");  return;
        case UNO_R_D2:   snprintf(out, n, "+2"); return;
        case UNO_R_WILD: snprintf(out, n, "W");  return;
        case UNO_R_W4:   snprintf(out, n, "+4"); return;
        default:         snprintf(out, n, "%d", uno_rank(c)); return;
    }
}

/* 0 easy, 1 med, 2 hard, 3 = other handsets. Same ladder as every other game
 * here and for the reason Hearts and Battleship both needed one: a game that
 * only works with a second handset in range is a game nobody can play in a car.
 *
 * 🔑 AND IT IS THE ONLY WAY TO TEST THE SEATING ON ONE DEVICE. Three machine
 * opponents make a four-handed game, so direction, skip and reverse all run
 * for real without waiting for the fleet to be home. */
#define UNO_VS 3
static int       uno_mode;              /* defaults to easy: alone is normal */
static lv_obj_t *uno_mbtn[4];

static lv_obj_t *scr_uno;
static uint8_t   uno_own[UNO_CARDS];    /* seat 0..3, UNO_DRAW, UNO_DISC */
static uint8_t   uno_deck[UNO_CARDS];   /* the shuffle, from the seed */
static uint32_t  uno_seed;
static uint8_t   uno_top;               /* card showing on the discard */
static uint8_t   uno_colr;              /* the colour in force, 0..3 */
static uint8_t   uno_turn;              /* seat to move */
static uint8_t   uno_dir;               /* 0 = seats ascending */
static uint8_t   uno_np;
static uint16_t  uno_roster[UNO_MAXP];  /* sorted ascending == seat order */
static uint16_t  uno_moves;
static uint8_t   uno_gen;
static int       uno_seat = -1;         /* my seat, or -1 if I am not playing */
static bool      uno_over;
static int       uno_wild_pick = -1;    /* card awaiting a colour choice */
static int64_t   uno_last_tx_us;

static lv_obj_t *uno_status, *uno_score_lbl, *uno_banner, *uno_disc, *uno_disc_lbl;
static lv_obj_t *uno_hand_row, *uno_others[UNO_MAXP], *uno_colbtn[4], *uno_colbox;
static lv_obj_t *uno_hand_btn[UNO_CARDS];
static int       uno_hand_card[UNO_CARDS];

static void uno_paint(void);
static void uno_broadcast(bool is_new);
static void uno_ai_arm(void);

static uint32_t uno_rng(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

/* The one shuffle every handset must agree on, byte for byte — the same
 * xorshift Hearts uses, and for the same reason. */
static void uno_build_deck(void)
{
    for (int i = 0; i < UNO_CARDS; i++) uno_deck[i] = (uint8_t)i;
    uint32_t s = uno_seed ? uno_seed : 1u;
    for (int i = UNO_CARDS - 1; i > 0; i--) {
        int j = (int)(uno_rng(&s) % (uint32_t)(i + 1));
        uint8_t t = uno_deck[i]; uno_deck[i] = uno_deck[j]; uno_deck[j] = t;
    }
}

static int uno_count(int seat)
{
    int n = 0;
    for (int i = 0; i < UNO_CARDS; i++) if (uno_own[i] == seat) n++;
    return n;
}

/* The next card off the draw pile is the first one in shuffle order that is
 * still in it — so the pile has an order without anybody sending one. */
static int uno_draw_next(void)
{
    for (int i = 0; i < UNO_CARDS; i++)
        if (uno_own[uno_deck[i]] == UNO_DRAW) return uno_deck[i];
    /* Pile exhausted: everything on the discard except the top card goes back.
     * Deterministic, so whoever does it first and broadcasts is agreed with. */
    for (int i = 0; i < UNO_CARDS; i++)
        if (uno_own[i] == UNO_DISC && i != uno_top) uno_own[i] = UNO_DRAW;
    for (int i = 0; i < UNO_CARDS; i++)
        if (uno_own[uno_deck[i]] == UNO_DRAW) return uno_deck[i];
    return -1;
}

/* Machine seats have no peer to look up, so the name comes from the mode. */
static const char *uno_name(int seat)
{
    static const char *bots[4] = { "You", "Bot 1", "Bot 2", "Bot 3" };
    if (seat < 0 || seat >= UNO_MAXP) return "?";
    if (uno_mode != UNO_VS) return bots[seat];
    for (int i = 0; i < espnow_peer_count(); i++)
        if (espnow_peer_id(i) == uno_roster[seat]) return espnow_peer_name(i);
    return uno_roster[seat] == espnow_my_id() ? "You" : "?";
}

static int uno_seat_of(uint16_t id)
{
    for (int i = 0; i < uno_np; i++) if (uno_roster[i] == id) return i;
    return -1;
}

static int uno_next_seat(int from, int step)
{
    int n = uno_np ? uno_np : 1;
    int d = uno_dir ? -1 : 1;
    int s = from;
    for (int k = 0; k < step; k++) s = ((s + d) % n + n) % n;
    return s;
}

static bool uno_playable(int c)
{
    if (UNO_IS_WILD(c)) return true;
    if (UNO_COL(c) == uno_colr) return true;
    if (UNO_IS_WILD(uno_top)) return false;      /* colour is all that matters */
    int a = uno_kind(c), b = uno_kind(uno_top);
    if (a != b) return false;
    return a != UNO_R_NUM || uno_rank(c) == uno_rank(uno_top);
}

/* Build the roster from whoever is in range, ourselves included, then sort it.
 * Sorting is what makes the seating derived rather than announced. */
static void uno_build_roster(void)
{
    if (uno_mode != UNO_VS) {
        /* Ids the radio can never produce, so a real handset appearing mid-game
         * can never be mistaken for one of the machines. */
        uno_np   = UNO_MAXP;
        uno_seat = 0;
        uno_roster[0] = espnow_my_id();
        for (int i = 1; i < UNO_MAXP; i++) uno_roster[i] = (uint16_t)(0xFFF0 + i);
        return;
    }
    uint16_t ids[UNO_MAXP];
    int n = 0;
    ids[n++] = espnow_my_id();
    /* 🚨 PLAYABLE PEERS ONLY. Seating every peer in range meant seating the
     * GATEWAYS, and a seat that can never move stops the game dead — Uno waits
     * on `uno_turn` and a gateway will not take its turn. Reported by the builder
     * 2026-09-08 as being unable to play Uno with the kids.
     * ⚠️ This fixes the gateway half ONLY. A real handset that is in range but
     * not on the Uno screen is still seated and still stalls the game; that
     * needs a join step and is written up separately. */
    int np = espnow_play_count();
    for (int k = 0; k < np && n < UNO_MAXP; k++) {
        int i = espnow_play_idx(k);
        if (i < 0) break;
        uint16_t id = espnow_peer_id(i);
        bool seen = false;
        for (int k = 0; k < n; k++) if (ids[k] == id) seen = true;
        if (!seen) ids[n++] = id;
    }
    for (int i = 0; i < n; i++)                  /* ascending == seat order */
        for (int j = i + 1; j < n; j++)
            if (ids[j] < ids[i]) { uint16_t t = ids[i]; ids[i] = ids[j]; ids[j] = t; }
    uno_np = (uint8_t)n;
    for (int i = 0; i < n; i++) uno_roster[i] = ids[i];
    uno_seat = uno_seat_of(espnow_my_id());
}

static void uno_deal(void)
{
    uno_build_roster();
    if (uno_np < 2) {
        if (uno_status) lv_label_set_text(uno_status, "nobody in range - try Easy, or wait for a handset");
        return;
    }
    uno_gen++;
    uno_seed = esp_random() | 1u;
    uno_build_deck();
    uno_moves = 0;
    uno_over = false;
    uno_dir = 0;
    uno_wild_pick = -1;
    banner_set(&uno_banner, scr_uno, NULL, C_INK);

    for (int i = 0; i < UNO_CARDS; i++) uno_own[i] = UNO_DRAW;
    int k = 0;
    for (int r = 0; r < UNO_HAND0; r++)
        for (int p = 0; p < uno_np; p++) uno_own[uno_deck[k++]] = (uint8_t)p;

    /* The opening card must not be a wild — otherwise nobody has named a
     * colour and the first player cannot know what is legal. */
    while (UNO_IS_WILD(uno_deck[k])) k++;
    uno_top = uno_deck[k];
    uno_own[uno_top] = UNO_DISC;
    uno_colr = (uint8_t)UNO_COL(uno_top);
    uno_turn = 0;                        /* seat 0, the lowest id, opens */
    uno_broadcast(true);
    uno_ai_arm();
}

static void uno_check_won(void)
{
    for (int p = 0; p < uno_np; p++) {
        if (uno_count(p) != 0) continue;
        uno_over = true;
        bool mine = (p == uno_seat);
        char t[64];
        if (mine) { hs_set("uno_w", hs_get("uno_w") + 1);
                    snprintf(t, sizeof(t), "YOU WIN\ntap Deal for another"); }
        else snprintf(t, sizeof(t), "%s WINS\ntap Deal for another", uno_name(p));
        banner_set(&uno_banner, scr_uno, t, mine ? C_GREEN : C_RED);
        return;
    }
}

/* Everything a played card does, in one place, so both the local play and a
 * frame arriving from the radio reach the same position. */
static void uno_apply_effect(int card, int by, int colour_choice)
{
    uno_own[card] = UNO_DISC;
    uno_top = (uint8_t)card;
    uno_colr = UNO_IS_WILD(card) ? (uint8_t)colour_choice : (uint8_t)UNO_COL(card);
    uno_moves++;

    int k = uno_kind(card);
    if (k == UNO_R_REV) {
        uno_dir = !uno_dir;
        /* With two players a reverse has nobody to travel back through, so it
         * is a skip — the same rule the printed ones use. */
        uno_turn = (uint8_t)uno_next_seat(by, uno_np == 2 ? 2 : 1);
        return;
    }
    if (k == UNO_R_SKIP) { uno_turn = (uint8_t)uno_next_seat(by, 2); return; }
    if (k == UNO_R_D2 || k == UNO_R_W4) {
        int victim = uno_next_seat(by, 1);
        int n = (k == UNO_R_D2) ? 2 : 4;
        for (int i = 0; i < n; i++) {
            int c = uno_draw_next();
            if (c < 0) break;
            uno_own[c] = (uint8_t)victim;
        }
        uno_turn = (uint8_t)uno_next_seat(by, 2);   /* drawn and skipped */
        return;
    }
    uno_turn = (uint8_t)uno_next_seat(by, 1);
}

static void uno_play(int card, int colour_choice)
{
    if (uno_seat < 0 || uno_turn != uno_seat || uno_over) return;
    uno_apply_effect(card, uno_seat, colour_choice);
    uno_check_won();
    uno_broadcast(false);
    uno_paint();
    uno_ai_arm();
}

/* How many of each colour this seat holds — the wild choice, and at the harder
 * levels the thing worth steering the game towards. */
static int uno_colour_count(int seat, int k)
{
    int n = 0;
    for (int i = 0; i < UNO_CARDS; i++)
        if (uno_own[i] == seat && !UNO_IS_WILD(i) && UNO_COL(i) == k) n++;
    return n;
}

static int uno_best_colour(int seat)
{
    int best = 0, bn = -1;
    for (int k = 0; k < 4; k++) {
        int n = uno_colour_count(seat, k);
        if (n > bn) { bn = n; best = k; }
    }
    return best;
}

/* Returns the card to play, or -1 to draw. `colour` gets the wild choice.
 *
 * Easy plays the first thing that is legal, which is exactly as thoughtless as
 * it sounds and is the point. Medium sheds high cards and saves its wilds.
 * Hard also watches the player it is about to hand the turn to: an opponent
 * down to a card or two is the moment a skip or a draw-two is worth far more
 * than the number printed on it. */
static int uno_ai_pick(int seat, int *colour)
{
    *colour = uno_best_colour(seat);
    int next = uno_next_seat(seat, 1);
    int threat = uno_count(next) <= 2;
    int best = -1, best_score = -1;

    for (int c = 0; c < UNO_CARDS; c++) {
        if (uno_own[c] != seat || !uno_playable(c)) continue;
        if (uno_mode == 0) return c;            /* easy: the first one that fits */

        int k = uno_kind(c), score;
        if (k == UNO_R_W4)        score = 2;
        else if (k == UNO_R_WILD) score = 3;    /* keep wilds for when stuck */
        else if (k == UNO_R_SKIP || k == UNO_R_REV || k == UNO_R_D2) score = 30;
        else score = 10 + uno_rank(c);          /* shed the expensive ones */

        if (uno_mode == 2 && threat) {
            if (k == UNO_R_D2 || k == UNO_R_SKIP) score += 40;
            if (k == UNO_R_W4)                    score += 45;
            if (k == UNO_R_REV && uno_np == 2)    score += 40;
        }
        /* Staying on a colour it actually holds is worth a little, always. */
        if (!UNO_IS_WILD(c) && uno_mode == 2)
            score += uno_colour_count(seat, UNO_COL(c));

        if (score > best_score) { best_score = score; best = c; }
    }
    if (best >= 0 && UNO_IS_WILD(best)) *colour = uno_best_colour(seat);
    return best;
}

/* 🔑 A DRAW ENDS THE TURN. The printed rule lets you play the card you just
 * drew, which needs a "has already drawn" bit that is not in the position and
 * would therefore have to be remembered and synced. One card, turn passes:
 * a house rule, and the one that keeps the frame a complete statement. */
static void uno_draw_one(void)
{
    if (uno_seat < 0 || uno_turn != uno_seat || uno_over) return;
    int c = uno_draw_next();
    if (c >= 0) uno_own[c] = (uint8_t)uno_seat;
    uno_moves++;
    uno_turn = (uint8_t)uno_next_seat(uno_seat, 1);
    uno_broadcast(false);
    uno_paint();
    uno_ai_arm();
}

/* One machine move. The same uno_apply_effect() the player's cards go through,
 * so a bot cannot reach a position a person could not. */
static void uno_ai_move(void)
{
    int seat = uno_turn, colour = 0;
    int c = uno_ai_pick(seat, &colour);
    if (c < 0) {
        int d = uno_draw_next();
        if (d >= 0) uno_own[d] = (uint8_t)seat;
        uno_moves++;
        uno_turn = (uint8_t)uno_next_seat(seat, 1);
    } else {
        uno_apply_effect(c, seat, colour);
    }
    uno_check_won();
    uno_paint();
}

/* 🔑 THE MACHINE PLAYS ON A TIMER, like Hearts' and Connect 4's — three bots in
 * a row would otherwise resolve between two frames and the player would see the
 * discard change three times with nothing to watch. */
static void uno_ai_tick(lv_timer_t *t)
{
    (void)t;
    tmr_stop();
    if (uno_mode == UNO_VS || uno_over) return;
    if (!scr_uno || lv_scr_act() != scr_uno) return;
    if (uno_turn == uno_seat) return;
    uno_ai_move();
    uno_ai_arm();
}

static void uno_ai_arm(void)
{
    if (uno_mode == UNO_VS || uno_over) return;
    if (!scr_uno || uno_turn == uno_seat) return;
    tmr_start(uno_ai_tick, 700);
}

static void uno_pack(espnow_game_t *g)
{
    memset(g->board, 0, GAME_BOARD_MAX);
    for (int i = 0; i < UNO_CARDS; i++) {          /* 3 bits per card */
        int bit = i * 3;
        uint32_t v = uno_own[i] & 7u;
        g->board[bit >> 3]       |= (uint8_t)(v << (bit & 7));
        if ((bit & 7) > 5)
            g->board[(bit >> 3) + 1] |= (uint8_t)(v >> (8 - (bit & 7)));
    }
    g->board[41] = (uint8_t)uno_seed;
    g->board[42] = (uint8_t)(uno_seed >> 8);
    g->board[43] = (uint8_t)(uno_seed >> 16);
    g->board[44] = (uint8_t)(uno_seed >> 24);
    g->board[45] = uno_top;
    g->board[46] = uno_colr;
    g->board[47] = uno_turn;
    g->board[48] = uno_dir;
    g->board[49] = uno_np;
    for (int i = 0; i < UNO_MAXP; i++) {
        g->board[50 + i * 2] = (uint8_t)uno_roster[i];
        g->board[51 + i * 2] = (uint8_t)(uno_roster[i] >> 8);
    }
    g->board[58] = (uint8_t)uno_moves;
    g->board[59] = (uint8_t)(uno_moves >> 8);
}

static void uno_unpack(const espnow_game_t *g)
{
    for (int i = 0; i < UNO_CARDS; i++) {
        int bit = i * 3;
        uint32_t v = (uint32_t)g->board[bit >> 3] >> (bit & 7);
        if ((bit & 7) > 5) v |= (uint32_t)g->board[(bit >> 3) + 1] << (8 - (bit & 7));
        uno_own[i] = (uint8_t)(v & 7u);
    }
    uno_seed = (uint32_t)g->board[41] | ((uint32_t)g->board[42] << 8) |
               ((uint32_t)g->board[43] << 16) | ((uint32_t)g->board[44] << 24);
    uno_top  = g->board[45];
    uno_colr = g->board[46];
    uno_turn = g->board[47];
    uno_dir  = g->board[48];
    uno_np   = g->board[49] > UNO_MAXP ? UNO_MAXP : g->board[49];
    for (int i = 0; i < UNO_MAXP; i++)
        uno_roster[i] = (uint16_t)g->board[50 + i * 2] |
                        ((uint16_t)g->board[51 + i * 2] << 8);
    uno_moves = (uint16_t)g->board[58] | ((uint16_t)g->board[59] << 8);
    uno_build_deck();
    uno_seat = uno_seat_of(espnow_my_id());
}

/* Four players means four ways to send. Unicast to each of them by id — a
 * broadcast gets no 802.11 retry, which is a thing this project has already
 * paid for once. */
static void uno_broadcast(bool is_new)
{
    if (uno_mode != UNO_VS) return;     /* a game against the machine is local */
    if (!scr_uno || uno_np < 2) return;
    espnow_game_t g = { 0 };
    g.game    = GAME_UNO;
    g.flags   = is_new ? GF_NEW : 0;
    g.cells   = UNO_WIRE;
    g.gen     = uno_gen;
    g.starter = uno_roster[0];
    uno_pack(&g);
    uno_last_tx_us = esp_timer_get_time();
    for (int p = 0; p < uno_np; p++) {
        if (uno_roster[p] == espnow_my_id()) continue;
        for (int i = 0; i < espnow_peer_count(); i++)
            if (espnow_peer_id(i) == uno_roster[p]) { espnow_game_send(i, &g); break; }
    }
}

static void uno_apply(const espnow_game_t *g)
{
    if (g->game != GAME_UNO || g->cells != UNO_WIRE) return;
    uno_mode = UNO_VS;                  /* somebody is dealing at us */
    tmr_stop();                         /* and the bots stand down */
    if (g->gen < uno_gen) return;
    /* Within one deal the position that has seen more moves is the later one.
     * Same rule as everywhere else here: newer generation always wins, and
     * inside a generation, more progress wins. */
    uint16_t moves = (uint16_t)g->board[58] | ((uint16_t)g->board[59] << 8);
    if (g->gen == uno_gen && moves <= uno_moves) return;

    uno_gen = g->gen;
    uno_unpack(g);
    uno_over = false;
    uno_wild_pick = -1;
    banner_set(&uno_banner, scr_uno, NULL, C_INK);
    uno_check_won();
    uno_paint();
}

static void uno_card_clicked(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    int c = uno_hand_card[slot];
    if (c < 0 || uno_over) return;
    if (uno_seat < 0)          { lv_label_set_text(uno_status, "you are not in this game"); return; }
    if (uno_turn != uno_seat)  { lv_label_set_text(uno_status, "not your turn"); return; }
    if (!uno_playable(c))      { lv_label_set_text(uno_status, "that one does not match"); return; }
    if (UNO_IS_WILD(c)) {       /* the colour has to be chosen before it counts */
        uno_wild_pick = c;
        uno_paint();
        lv_label_set_text(uno_status, "pick a colour");
        return;
    }
    uno_play(c, 0);
}

static void uno_colour_clicked(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (uno_wild_pick < 0) return;
    int c = uno_wild_pick;
    uno_wild_pick = -1;
    uno_play(c, k);
}

static void uno_draw_clicked(lv_event_t *e)
{
    (void)e;
    if (uno_over) return;
    if (uno_seat < 0)         { lv_label_set_text(uno_status, "you are not in this game"); return; }
    if (uno_turn != uno_seat) { lv_label_set_text(uno_status, "not your turn"); return; }
    uno_draw_one();
}

static void uno_deal_clicked(lv_event_t *e) { (void)e; uno_deal(); uno_paint(); }

static void uno_mode_clicked(lv_event_t *e)
{
    uno_mode = (int)(intptr_t)lv_event_get_user_data(e);
    tmr_stop();
    uno_deal();
    uno_paint();
}

static void uno_paint(void)
{
    if (!scr_uno) return;

    /* The discard, and the colour in force behind it — a wild changes the
     * colour without changing the card, so the swatch is the real answer. */
    if (uno_disc) {
        lv_obj_set_style_bg_color(uno_disc, uno_colour_of(uno_colr), 0);
        char t[8];
        uno_card_text(uno_top, t, sizeof(t));
        if (uno_disc_lbl) lv_label_set_text(uno_disc_lbl, t);
    }

    for (int p = 0; p < UNO_MAXP; p++) {
        if (!uno_others[p]) continue;
        if (p >= uno_np || p == uno_seat) { lv_obj_add_flag(uno_others[p], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(uno_others[p], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(uno_others[p], "%s%s %d",
                              uno_turn == p ? ">" : " ", uno_name(p), uno_count(p));
        lv_obj_set_style_text_color(uno_others[p],
                                    uno_turn == p ? C_AMBER : C_MUTED, 0);
    }

    /* My hand. One button per card, from a pool the size of the deck, because
     * a bad run can leave one player holding a great many of them. */
    int n = 0;
    for (int i = 0; i < UNO_CARDS && uno_seat >= 0; i++) {
        if (uno_own[i] != uno_seat) continue;
        lv_obj_t *b = uno_hand_btn[n];
        uno_hand_card[n] = i;
        lv_obj_set_style_bg_color(b, UNO_IS_WILD(i) ? C_PANEL : uno_colour_of(UNO_COL(i)), 0);
        bool ok = uno_playable(i) && uno_turn == uno_seat && !uno_over;
        lv_obj_set_style_border_color(b, ok ? C_INK : C_LINE, 0);
        lv_obj_set_style_border_width(b, ok ? 2 : 1, 0);
        char t[8];
        uno_card_text(i, t, sizeof(t));
        lv_obj_t *l = lv_obj_get_child(b, 0);
        if (l) lv_label_set_text(l, t);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
        n++;
    }
    for (int i = n; i < UNO_CARDS; i++) {
        uno_hand_card[i] = -1;
        lv_obj_add_flag(uno_hand_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int m = 0; m < 4; m++)
        if (uno_mbtn[m])
            lv_obj_set_style_bg_color(uno_mbtn[m], m == uno_mode ? C_BLUE : C_PANEL, 0);

    if (uno_colbox) {
        if (uno_wild_pick >= 0) lv_obj_clear_flag(uno_colbox, LV_OBJ_FLAG_HIDDEN);
        else                    lv_obj_add_flag(uno_colbox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(uno_colbox);
    }
    if (uno_banner) lv_obj_move_foreground(uno_banner);

    if (uno_score_lbl)
        lv_label_set_text_fmt(uno_score_lbl, "%d", uno_seat >= 0 ? uno_count(uno_seat) : 0);

    if (uno_status && !uno_over && uno_wild_pick < 0) {
        if (uno_np < 2) lv_label_set_text(uno_status, "nobody in range - try Easy");
        else if (uno_seat < 0) lv_label_set_text(uno_status, "watching - you are not in this deal");
        else if (uno_turn == uno_seat)
            lv_label_set_text_fmt(uno_status, "your turn  -  %s", UNO_CN[uno_colr]);
        else lv_label_set_text_fmt(uno_status, "waiting  -  %s", UNO_CN[uno_colr]);
    }
}

static void uno_back(lv_event_t *e)
{
    (void)e;
    tmr_stop();
    if (!scr_menu_ready()) return;
    uno_banner = NULL;
    leave_to(scr_menu, &scr_uno);
}

static void uno_build(void)
{
    uno_banner = NULL;
    scr_uno = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_uno, C_BG, 0);
    lv_obj_clear_flag(scr_uno, LV_OBJ_FLAG_SCROLLABLE);
    uno_score_lbl = mk_hdr(scr_uno, "Uno", uno_back);

    uno_disc = rect(scr_uno, 52, 68, C_PANEL);
    lv_obj_set_style_radius(uno_disc, 6, 0);
    lv_obj_align(uno_disc, LV_ALIGN_TOP_MID, 0, HDR_H + 30);
    uno_disc_lbl = lbl(uno_disc, &lv_font_montserrat_40, C_INK, "");
    lv_obj_center(uno_disc_lbl);

    for (int p = 0; p < UNO_MAXP; p++) {
        uno_others[p] = lbl(scr_uno, &lv_font_montserrat_12, C_MUTED, "");
        lv_obj_align(uno_others[p], LV_ALIGN_TOP_LEFT, 6, HDR_H + 4 + p * 15);
    }

    static const char *mnames[4] = { "Easy", "Med", "Hard", "VS" };
    for (int m = 0; m < 4; m++) {
        uno_mbtn[m] = btn(scr_uno, 44, 20, C_PANEL, C_INK, &lv_font_montserrat_12,
                          mnames[m], uno_mode_clicked, (void *)(intptr_t)m);
        lv_obj_align(uno_mbtn[m], LV_ALIGN_TOP_LEFT, 6 + m * 57, HDR_H + 104);
    }

    lv_obj_t *db = btn(scr_uno, 56, 24, C_PANEL, C_INK, &lv_font_montserrat_12,
                       "Draw", uno_draw_clicked, NULL);
    lv_obj_align(db, LV_ALIGN_TOP_RIGHT, -4, HDR_H + 34);
    lv_obj_t *nb = btn(scr_uno, 56, 24, C_PANEL, C_INK, &lv_font_montserrat_12,
                       "Deal", uno_deal_clicked, NULL);
    lv_obj_align(nb, LV_ALIGN_TOP_RIGHT, -4, HDR_H + 62);

    /* The hand scrolls sideways: eleven cards will not fit across 240px and a
     * card you cannot reach is a card you cannot play. */
    uno_hand_row = lv_obj_create(scr_uno);
    lv_obj_remove_style_all(uno_hand_row);
    lv_obj_set_size(uno_hand_row, 236, 76);
    lv_obj_align(uno_hand_row, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_flex_flow(uno_hand_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(uno_hand_row, 4, 0);
    lv_obj_set_style_pad_all(uno_hand_row, 2, 0);
    lv_obj_set_scroll_dir(uno_hand_row, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(uno_hand_row, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < UNO_CARDS; i++) {
        lv_obj_t *b = btn_ev(uno_hand_row, 40, 62, C_PANEL, C_INK,
                             &lv_font_montserrat_16, "", uno_card_clicked,
                             LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, C_LINE, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        uno_hand_btn[i] = b;
        uno_hand_card[i] = -1;
    }

    uno_colbox = rect(scr_uno, 204, 56, C_PANEL);
    lv_obj_set_style_border_color(uno_colbox, C_INK, 0);
    lv_obj_set_style_border_width(uno_colbox, 2, 0);
    lv_obj_set_style_radius(uno_colbox, 6, 0);
    lv_obj_align(uno_colbox, LV_ALIGN_CENTER, 0, 0);
    for (int k = 0; k < 4; k++) {
        uno_colbtn[k] = btn(uno_colbox, 44, 40, uno_colour_of(k), C_ONAMBER,
                            &lv_font_montserrat_12, "", uno_colour_clicked,
                            (void *)(intptr_t)k);
        lv_obj_align(uno_colbtn[k], LV_ALIGN_LEFT_MID, 4 + k * 48, 0);
    }
    lv_obj_add_flag(uno_colbox, LV_OBJ_FLAG_HIDDEN);

    uno_status = lbl(scr_uno, &lv_font_montserrat_12, C_MUTED, "");
    lv_obj_align(uno_status, LV_ALIGN_BOTTOM_LEFT, 4, -4);
}

static void uno_show(lv_event_t *e)
{
    (void)e;
    if (!scr_uno) uno_build();
    /* Opening the screen does NOT deal. Somebody else may already have one
     * running, and dealing over it would throw away their game. */
    uno_build_roster();
    /* Against the machine the game may as well be dealt: there is nobody whose
     * game we could be throwing away. Against handsets, opening the screen
     * deliberately does NOT deal - somebody else's may already be running. */
    if (uno_mode != UNO_VS && (uno_gen == 0 || uno_over)) uno_deal();
    uno_paint();
    uno_ai_arm();
    lv_scr_load(scr_uno);
    applog("[games] uno, seat %d of %d, internal free %u", uno_seat, uno_np,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static bool uno_net_active(void)
{
    return uno_mode == UNO_VS && scr_uno && lv_scr_act() == scr_uno && uno_np >= 2;
}

/* ------------------------------------------------------------------ *
 * Checkers — handset against handset
 *
 * Every network decision here is the one Connect 4 and Hearts already arrived
 * at, for the same reasons, and each of those replaced a bug found on real
 * hardware: the whole position travels rather than the move, roles come from a
 * carried `starter` rather than a negotiation, a generation separates "next
 * game" from "stale", scores are indexed by DEVICE ID, and an unfinished board
 * is re-announced every couple of seconds so a lost frame heals itself.
 *
 * 🚨 TWO THINGS ARE GENUINELY DIFFERENT HERE, AND BOTH BIT THE OTHER GAMES.
 *
 * 1. THE TURN CANNOT BE DERIVED FROM THE POSITION. Connect 4 counts discs and
 *    Hearts counts plays, because in both, every move adds something. A
 *    checkers move usually changes NO count at all — a man slides one square
 *    and the piece totals are identical. So the turn rides on the wire in
 *    `turn`, which until now was documented as "nothing reads it".
 *
 * 2. PIECE COUNT ORDERS BOARDS BACKWARDS. "More marks wins" is what separates
 *    a newer board from an older one everywhere else. Pieces only ever go
 *    DOWN, so that test would prefer the stale board every time. A monotonic
 *    ply counter rides in board[CK_PLY] instead.
 * ------------------------------------------------------------------ */

#define CK_N     32            /* the dark squares, the only ones ever used  */
#define CK_CELL  26
#define CK_PLY   32            /* board[32..33] ply, [34..37] the two scores */

/* r*4 + c/2 addresses a dark square in either row parity: on even rows the
 * dark columns are 1,3,5,7 and on odd rows 0,2,4,6, and c/2 is 0..3 for both. */
#define CK_IDX(r, c)  ((r) * 4 + ((c) >> 1))
#define CK_ROW(i)     ((i) >> 2)
#define CK_COL(i)     ((((i) & 3) << 1) + ((CK_ROW(i) & 1) ? 0 : 1))

static lv_obj_t *scr_ck, *ck_sq[CK_N], *ck_pc[CK_N], *ck_pl[CK_N];
static lv_obj_t *ck_status, *ck_peer_btn, *ck_banner, *ck_score_lbl;

/* 0 empty, 'r'/'R' red man/king, 'w'/'W' white man/king. Red sits at the
 * bottom and moves UP the screen, which is the way round a player sitting in
 * front of it expects their own pieces to travel. */
static char      ck_b[CK_N];
static char      ck_me, ck_turn;
static bool      ck_over;
static int       ck_sel = -1;      /* square the player has picked up, or -1  */
static int       ck_chain = -1;    /* mid multi-jump: only this piece may move */

#define CK_VS 4
static int       ck_mode;
static lv_obj_t *ck_mbtn[5];
static int       ck_peer = -1;
static uint8_t   ck_gen;
static uint16_t  ck_starter;
static uint16_t  ck_ply;
/* 🚨 PLIES SINCE ANYTHING IRREVERSIBLE HAPPENED — WITHOUT THIS THE GAME NEVER
 * ENDS. Kings move both ways, so two of them shuffle back and forth forever
 * and no winner is ever reached: a self-play game stalled at ply 126 with 80
 * plies of no progress at all. Standard rule, and the reason it exists — 40
 * moves a side with no capture and no man moved is a draw. It rides on the
 * wire because both handsets have to agree WHEN the draw happened. */
#define CK_STILL_MAX 80
static uint8_t   ck_still;
static int64_t   ck_last_tx_us;
static int       ck_wins[2];
static int       ck_won_gen = -1;

static void ck_paint(void);
static void ck_broadcast(bool is_new);
static void ck_status_set(void);

static char ck_side_of(char p)
{
    if (p == 'r' || p == 'R') return 'r';
    if (p == 'w' || p == 'W') return 'w';
    return 0;
}
static bool ck_is_king(char p) { return p == 'R' || p == 'W'; }

/* ---- move generation ---------------------------------------------------- */

typedef struct { int8_t from, to, cap; } ck_move_t;   /* cap = -1 for a slide */

static const int8_t CK_DR[4] = { -1, -1,  1,  1 };
static const int8_t CK_DC[4] = { -1,  1, -1,  1 };

static bool ck_on(int r, int c) { return r >= 0 && r < 8 && c >= 0 && c < 8; }

/* Every legal move for `side`. 🔑 CAPTURE IS COMPULSORY, so if any jump exists
 * the slides are thrown away — that is the rule children argue about most and
 * the one a computer opponent must not quietly ignore. `only` limits the
 * search to one square, which is how a multi-jump is kept on the same piece. */
static int ck_moves(const char *b, char side, ck_move_t *out, int max, int only)
{
    int n = 0;

    /* Pass 0 collects jumps. If there are any, they are the whole legal move
     * list and pass 1 never runs — capture is compulsory, which is the rule
     * children argue about most and the one a computer opponent must not
     * quietly ignore. `only` pins the search to a single square, which is how a
     * multi-jump is kept on the piece that started it. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < CK_N; i++) {
            if (only >= 0 && i != only) continue;
            char p = b[i];
            if (ck_side_of(p) != side) continue;
            int r = CK_ROW(i), c = CK_COL(i);
            for (int d = 0; d < 4; d++) {
                if (!ck_is_king(p)) {           /* a man only walks forwards */
                    int fwd = (side == 'r') ? -1 : 1;
                    if (CK_DR[d] != fwd) continue;
                }
                int r1 = r + CK_DR[d], c1 = c + CK_DC[d];
                if (!ck_on(r1, c1)) continue;
                int i1 = CK_IDX(r1, c1);

                if (pass == 0) {
                    if (!b[i1] || ck_side_of(b[i1]) == side) continue;
                    int r2 = r1 + CK_DR[d], c2 = c1 + CK_DC[d];
                    if (!ck_on(r2, c2)) continue;
                    int i2 = CK_IDX(r2, c2);
                    if (b[i2]) continue;
                    if (n < max) { out[n].from = (int8_t)i; out[n].to = (int8_t)i2;
                                   out[n].cap = (int8_t)i1; n++; }
                } else {
                    if (b[i1]) continue;
                    if (n < max) { out[n].from = (int8_t)i; out[n].to = (int8_t)i1;
                                   out[n].cap = -1; n++; }
                }
            }
        }
        if (pass == 0 && n > 0) return n;       /* captures stand alone */
    }
    return n;
}

/* Applies a move. Returns true if the SAME piece must jump again — the caller
 * keeps the turn when it does. A man crowned by this move stops regardless,
 * which is the standard rule and stops a new king reversing back up the board
 * in the same breath. */
static bool ck_apply_move(char *b, const ck_move_t *m)
{
    char p = b[m->from];
    b[m->from] = 0;
    if (m->cap >= 0) b[m->cap] = 0;
    b[m->to] = p;

    int row = CK_ROW(m->to);
    bool crowned = false;
    if (p == 'r' && row == 0) { b[m->to] = 'R'; crowned = true; }
    if (p == 'w' && row == 7) { b[m->to] = 'W'; crowned = true; }

    if (m->cap < 0 || crowned) return false;
    ck_move_t more[8];
    int n = ck_moves(b, ck_side_of(p), more, 8, m->to);
    return n > 0 && more[0].cap >= 0;
}

/* 'r', 'w' — the side with no move left has lost — or 0 while play continues. */
/* Play a move out in full, including any forced continuation BY THE SAME
 * PIECE. 🚨 The chain must be pinned to the square the piece now sits on: an
 * unpinned search let any other piece of that colour continue the jump, which
 * makes the machine evaluate positions that cannot happen. */
static void ck_play_full(char *b, const ck_move_t *m)
{
    ck_move_t cur = *m;
    for (int guard = 0; guard < 12; guard++) {
        if (!ck_apply_move(b, &cur)) return;
        ck_move_t ch[8];
        int cn = ck_moves(b, ck_side_of(b[cur.to]), ch, 8, cur.to);
        if (cn <= 0 || ch[0].cap < 0) return;
        cur = ch[0];
    }
}

static char ck_winner(const char *b, char to_move)
{
    ck_move_t mv[48];
    if (ck_moves(b, to_move, mv, 48, -1) == 0)
        return to_move == 'r' ? 'w' : 'r';
    return 0;
}

/* ---- the machine -------------------------------------------------------- */

static int ck_eval(const char *b, char side)
{
    int s = 0;
    for (int i = 0; i < CK_N; i++) {
        char p = b[i];
        if (!p) continue;
        /* A king is worth appreciably more than a man, and a man is worth more
         * the closer it gets to being one. */
        int v = ck_is_king(p) ? 160 : 100;
        if (!ck_is_king(p)) {
            int row = CK_ROW(i);
            v += (ck_side_of(p) == 'r') ? (7 - row) * 4 : row * 4;
        }
        s += (ck_side_of(p) == side) ? v : -v;
    }
    return s;
}

static int ck_search(char *b, char side, char me, int depth, int alpha, int beta)
{
    char w = ck_winner(b, side);
    if (w) return (w == me) ? 30000 - depth : -30000 + depth;
    if (depth <= 0) return ck_eval(b, me);

    ck_move_t mv[48];
    int n = ck_moves(b, side, mv, 48, -1);
    if (!n) return ck_eval(b, me);

    char other = (side == 'r') ? 'w' : 'r';
    int best = (side == me) ? -32000 : 32000;
    for (int i = 0; i < n; i++) {
        char nb[CK_N];
        memcpy(nb, b, CK_N);
        /* A chain of jumps is ONE ply: the side keeps moving until it cannot,
         * otherwise the search hands the reply to the wrong player and reads a
         * multi-jump as a blunder. */
        ck_play_full(nb, &mv[i]);
        int v = ck_search(nb, other, me, depth - 1, alpha, beta);
        if (side == me) { if (v > best) best = v; if (best > alpha) alpha = best; }
        else            { if (v < best) best = v; if (best < beta)  beta  = best; }
        if (beta <= alpha) break;
    }
    return best;
}

static int ck_ai_pick(ck_move_t *chosen)
{
    ck_move_t mv[48];
    int n = ck_moves(ck_b, 'w', mv, 48, -1);
    if (n <= 0) return -1;

    /* Easy is deliberately shallow rather than random: a six-year-old should
     * be able to beat it and still feel it was playing. */
    int depth = ck_mode == 1 ? 1 : ck_mode == 2 ? 3 : 5;
    int best = -32000, bi = 0;
    for (int i = 0; i < n; i++) {
        char nb[CK_N];
        memcpy(nb, ck_b, CK_N);
        ck_play_full(nb, &mv[i]);
        int v = ck_search(nb, 'r', 'w', depth, -32000, 32000);
        if (v > best) { best = v; bi = i; }
    }
    *chosen = mv[bi];
    return bi;
}

/* ---- roles, scoring, wire ----------------------------------------------- */

/* `moved` is the piece as it stood BEFORE the move, so a man crowned by this
 * very move still counts as progress — it was a man when it set off. */
static void ck_note_move(char moved, const ck_move_t *m)
{
    if (m->cap >= 0 || !ck_is_king(moved)) ck_still = 0;
    else if (ck_still < 255)               ck_still++;
}

static void ck_set_role(void)
{
    if (ck_peer < 0) { ck_me = 'r'; return; }
    ck_starter = starter_fix(ck_starter, espnow_my_id(), espnow_peer_id(ck_peer));
    ck_me = (espnow_my_id() == ck_starter) ? 'r' : 'w';
}

static void ck_next_starter(char w)
{
    if (ck_peer < 0) return;
    uint16_t me = espnow_my_id(), them = espnow_peer_id(ck_peer);
    if (w == 'D') ck_starter = (ck_starter == me) ? them : me;  /* just alternate */
    else          ck_starter = (w == ck_me) ? them : me;  /* the loser opens next */
}

static void ck_pick_default_peer(void)
{
    pick_default_peer(&ck_peer);
}

static int ck_my_slot(void)
{
    if (ck_mode != CK_VS || ck_peer < 0) return 0;
    return id_slot(espnow_my_id(), espnow_peer_id(ck_peer));
}

static void ck_status_set(void)
{
    if (!ck_status || ck_over) return;
    if (ck_chain >= 0) { lv_label_set_text(ck_status, "jump again!"); return; }
    if (ck_mode != CK_VS) {
        lv_label_set_text(ck_status,
            ck_mode == 0 ? (ck_turn == 'r' ? "Red's turn" : "White's turn")
                         : (ck_turn == 'r' ? "Your turn" : "thinking..."));
        return;
    }
    if (ck_peer < 0) { lv_label_set_text(ck_status, "nobody in range"); return; }
    const char *pn = espnow_peer_name(ck_peer);
    lv_label_set_text_fmt(ck_status, "%s  -  %s vs %s",
                          ck_turn == ck_me ? "Your turn" : "Their turn",
                          ck_me == 'r' ? "red" : "white", pn ? pn : "?");
}

static void ck_broadcast(bool is_new)
{
    if (ck_mode != CK_VS || ck_peer < 0 || !scr_ck) return;
    espnow_game_t g = { 0 };
    g.game    = GAME_CHECKERS;
    g.flags   = is_new ? GF_NEW : 0;
    g.cells   = CK_N;
    g.gen     = ck_gen;
    /* 🔑 THE ONE GAME THAT ACTUALLY READS `turn`. A slide changes no count at
     * all, so unlike every other board here the turn cannot be recomputed from
     * the position and has to travel. */
    g.turn    = (uint8_t)ck_turn;
    g.starter = ck_starter;
    memcpy(g.board, ck_b, CK_N);
    g.board[CK_PLY]     = (uint8_t)ck_ply;
    g.board[CK_PLY + 1] = (uint8_t)(ck_ply >> 8);
    g.board[34] = (uint8_t)ck_wins[0]; g.board[35] = (uint8_t)(ck_wins[0] >> 8);
    g.board[36] = (uint8_t)ck_wins[1]; g.board[37] = (uint8_t)(ck_wins[1] >> 8);
    g.board[38] = ck_still;
    ck_last_tx_us = esp_timer_get_time();
    if (!espnow_game_send(ck_peer, &g))
        lv_label_set_text(ck_status, "send failed");
}

static void ck_finish(char w)
{
    ck_over = true;
    ck_sel = ck_chain = -1;
    if (!ck_status) return;
    if (w == 'D') {
        lv_label_set_text(ck_status, "Draw - tap to play again");
        banner_set(&ck_banner, scr_ck,
                   "A DRAW\nnobody could make progress", C_AMBER);
        ck_paint();
        return;
    }
    {
        bool mine = (ck_mode == CK_VS) ? (w == ck_me) : (w == 'r');
        const char *they = (ck_mode == CK_VS && ck_peer >= 0)
                           ? espnow_peer_name(ck_peer) : "THE MACHINE";
        char t[48];
        if (ck_mode == 0) {
            snprintf(t, sizeof(t), "%s WINS\ntap to play again",
                     w == 'r' ? "RED" : "WHITE");
            banner_set(&ck_banner, scr_ck, t, C_BLUE);
        } else if (mine) {
            banner_set(&ck_banner, scr_ck, "YOU WIN\ntap to play again", C_GREEN);
        } else {
            snprintf(t, sizeof(t), "%s WINS\ntap to play again", they);
            banner_set(&ck_banner, scr_ck, t, C_RED);
        }
    }
    if (ck_mode != CK_VS) {
        if (w == 'r') ck_wins[0]++; else ck_wins[1]++;
    } else if (ck_won_gen != (int)ck_gen) {
        ck_won_gen = (int)ck_gen;       /* once per game, whichever end saw it */
        int mine = ck_my_slot();
        ck_wins[(w == ck_me) ? mine : 1 - mine]++;
    }
    if (ck_mode == 0)
        lv_label_set_text_fmt(ck_status, "%s wins - tap to play again",
                              w == 'r' ? "Red" : "White");
    else if (ck_mode == CK_VS)
        lv_label_set_text_fmt(ck_status, "%s wins - tap to play again",
                              w == ck_me ? "You" : "They");
    else if (w == 'r')
        lv_label_set_text(ck_status, "You win! - tap to play again");
    else
        lv_label_set_text(ck_status, "I win - tap to play again");
    ck_paint();
}

static void ck_reset(void)
{
    memset(ck_b, 0, sizeof(ck_b));
    /* White on rows 0-2, red on rows 5-7, on the dark squares only. */
    for (int i = 0; i < CK_N; i++) {
        int r = CK_ROW(i);
        if (r < 3) ck_b[i] = 'w';
        else if (r > 4) ck_b[i] = 'r';
    }
    ck_over = false;
    ck_sel = ck_chain = -1;
    ck_ply = 0;
    ck_still = 0;
    ck_turn = 'r';                      /* red always opens */
    banner_set(&ck_banner, scr_ck, NULL, C_INK);

    if (ck_mode == CK_VS) {
        ck_pick_default_peer();
        ck_gen++;                       /* a new game outranks the old one */
        ck_set_role();
        ck_paint();
        ck_broadcast(true);
        ck_status_set();
        return;
    }
    ck_me = 'r';
    ck_paint();
    ck_status_set();
}

static void ck_apply(const espnow_game_t *g)
{
    if (g->game != GAME_CHECKERS || g->cells != CK_N) return;
    ck_mode = CK_VS;
    uint16_t ply = (uint16_t)(g->board[CK_PLY] | (g->board[CK_PLY + 1] << 8));
    /* 🚨 PLY, NOT PIECE COUNT. Everywhere else in this file "more marks wins"
     * separates a newer board from an older one. Pieces only ever go DOWN, so
     * that test would prefer the stale board every single time. */
    if (g->gen < ck_gen) return;
    if (g->gen == ck_gen && ply < ck_ply) return;
    ck_gen     = g->gen;
    ck_ply     = ply;
    ck_starter = (ck_peer >= 0)
                 ? starter_fix(g->starter, espnow_my_id(), espnow_peer_id(ck_peer))
                 : g->starter;
    memcpy(ck_b, g->board, CK_N);
    int theirs[2] = { g->board[34] | (g->board[35] << 8),
                      g->board[36] | (g->board[37] << 8) };
    score_merge(ck_wins, theirs);
    ck_still = g->board[38];
    ck_me   = (espnow_my_id() == ck_starter) ? 'r' : 'w';
    ck_turn = (g->turn == 'w') ? 'w' : 'r';
    ck_sel = ck_chain = -1;
    ck_over = false;
    ck_paint();
    char w = ck_winner(ck_b, ck_turn);
    if (!w && ck_still >= CK_STILL_MAX) w = 'D';
    if (w) { ck_next_starter(w); ck_finish(w); }
    else   ck_status_set();
}

/* ---- screen -------------------------------------------------------------- */

#define CK_DARK   lv_color_hex(0x6B4226)
#define CK_LIGHT  lv_color_hex(0xC8A165)
#define CK_TGT    lv_color_hex(0x3E7A3E)

static void ck_paint(void)
{
    if (!scr_ck || !ck_sq[0]) return;

    /* Where the picked-up piece may legally land. Recomputed rather than
     * remembered: the board can change under us when a frame arrives. */
    ck_move_t mv[48];
    int n = (ck_sel >= 0) ? ck_moves(ck_b, ck_turn, mv, 48, ck_sel) : 0;

    for (int i = 0; i < CK_N; i++) {
        bool tgt = false;
        for (int k = 0; k < n; k++) if (mv[k].to == i) { tgt = true; break; }
        lv_obj_set_style_bg_color(ck_sq[i],
            (i == ck_sel) ? C_GREEN : tgt ? CK_TGT : CK_DARK, 0);

        char p = ck_b[i];
        if (!p) { lv_obj_add_flag(ck_pc[i], LV_OBJ_FLAG_HIDDEN); continue; }
        lv_obj_clear_flag(ck_pc[i], LV_OBJ_FLAG_HIDDEN);
        bool red = ck_side_of(p) == 'r';
        lv_obj_set_style_bg_color(ck_pc[i], red ? C_RED : C_INK, 0);
        /* A crown would need a font; a K is legible at 18px and needs none. */
        lv_label_set_text(ck_pl[i], ck_is_king(p) ? "K" : "");
        lv_obj_set_style_text_color(ck_pl[i], red ? C_INK : C_BG, 0);
    }
    if (ck_score_lbl)
        lv_label_set_text_fmt(ck_score_lbl, "%d-%d", ck_wins[0], ck_wins[1]);
}

static void ck_ai_tick(lv_timer_t *t)
{
    (void)t;
    tmr_stop();
    if (ck_mode == 0 || ck_mode == CK_VS || ck_over) return;
    if (!scr_ck || lv_scr_act() != scr_ck) return;
    if (ck_turn != 'w') return;

    ck_move_t m;
    if (ck_ai_pick(&m) >= 0) {
        char moved = ck_b[m.from];
        ck_play_full(ck_b, &m);
        ck_note_move(moved, &m);
    }
    ck_ply++;
    ck_turn = 'r';
    ck_paint();
    char w = ck_winner(ck_b, ck_turn);
    if (!w && ck_still >= CK_STILL_MAX) w = 'D';
    if (w) ck_finish(w);
    else   ck_status_set();
}

static void ck_sq_clicked(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (ck_over) { ck_reset(); return; }

    /* Whose hand is on the board. In two-player-on-one-handset BOTH sides are
     * the player, which is the whole point of that mode. */
    if (ck_mode == CK_VS) {
        ck_pick_default_peer();
        if (ck_peer < 0) { lv_label_set_text(ck_status, "nobody in range"); return; }
        ck_set_role();
    }
    bool mine = (ck_mode == CK_VS) ? (ck_turn == ck_me)
              : (ck_mode == 0)     ? true
                                   : (ck_turn == 'r');
    if (!mine) {
        if (ck_mode == CK_VS) lv_label_set_text(ck_status, "not your turn");
        return;
    }

    /* Mid multi-jump the piece is committed: it must keep jumping and nothing
     * else may be picked up instead. */
    if (ck_chain >= 0 && ck_sel != ck_chain) { ck_sel = ck_chain; ck_paint(); return; }

    if (ck_sel >= 0) {
        ck_move_t mv[48];
        int n = ck_moves(ck_b, ck_turn, mv, 48, ck_sel);
        for (int k = 0; k < n; k++) {
            if (mv[k].to != i) continue;
            char moved = ck_b[mv[k].from];
            bool again = ck_apply_move(ck_b, &mv[k]);
            ck_ply++;
            if (!again) ck_note_move(moved, &mv[k]);
            if (again) {
                ck_sel = ck_chain = mv[k].to;   /* same piece, same turn */
                ck_paint();
                ck_status_set();
                return;
            }
            ck_sel = ck_chain = -1;
            ck_turn = (ck_turn == 'r') ? 'w' : 'r';
            ck_paint();
            char w = ck_winner(ck_b, ck_turn);
            if (!w && ck_still >= CK_STILL_MAX) w = 'D';
            if (w) {
                if (ck_mode == CK_VS) { ck_next_starter(w); ck_broadcast(false); }
                ck_finish(w);
            } else {
                if (ck_mode == CK_VS) ck_broadcast(false);
                ck_status_set();
                /* 🔑 THE MACHINE MOVES ON A TIMER, NOT IN THE TOUCH HANDLER.
                 * Hard is an alpha-beta search; run inline it would hold the
                 * handler until it finished, so the player's own move would
                 * not appear until the reply was already decided and the board
                 * would seem to move twice at once. */
                if (ck_mode != 0 && ck_mode != CK_VS) tmr_start(ck_ai_tick, 120);
            }
            return;
        }
    }

    /* Not a destination, so treat it as picking a piece up — but only one that
     * actually has a move, or the board goes quiet with no explanation. */
    if (ck_side_of(ck_b[i]) == ck_turn) {
        ck_move_t mv[48];
        ck_sel = (ck_moves(ck_b, ck_turn, mv, 48, i) > 0) ? i : -1;
        if (ck_sel < 0 && ck_status)
            lv_label_set_text(ck_status, "that one cannot move");
    } else {
        ck_sel = -1;
    }
    ck_paint();
}

static void ck_mode_clicked(lv_event_t *e)
{
    ck_mode = (int)(intptr_t)lv_event_get_user_data(e);
    ck_wins[0] = ck_wins[1] = 0;
    ck_won_gen = -1;
    ck_reset();
}

static void ck_peer_clicked(lv_event_t *e)
{
    (void)e;
    ck_peer = play_next(ck_peer);
    if (ck_peer < 0) lv_label_set_text(ck_status, "nobody in range");
    ck_set_role();
    ck_paint();
    ck_status_set();
}

static void ck_back(lv_event_t *e)
{
    (void)e;
    tmr_stop();
    if (!scr_menu_ready()) return;
    ck_banner = NULL;
    leave_to(scr_menu, &scr_ck);
}

static void ck_build(void)
{
    ck_banner = NULL;
    scr_ck = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_ck, C_BG, 0);
    lv_obj_clear_flag(scr_ck, LV_OBJ_FLAG_SCROLLABLE);
    ck_score_lbl = mk_hdr(scr_ck, "Checkers", ck_back);

    static const char *names[5] = { "2P", "Easy", "Med", "Hard", "VS" };
    for (int m = 0; m < 5; m++) {
        ck_mbtn[m] = btn(scr_ck, 44, 24, C_PANEL, C_INK, &lv_font_montserrat_12,
                         names[m], ck_mode_clicked, (void *)(intptr_t)m);
        lv_obj_align(ck_mbtn[m], LV_ALIGN_TOP_LEFT, 6 + m * 46, HDR_H + 6);
    }

    ck_peer_btn = btn(scr_ck, 100, 22, C_PANEL, C_BLUE, &lv_font_montserrat_12,
                      "vs: -", ck_peer_clicked, NULL);
    lv_obj_align(ck_peer_btn, LV_ALIGN_TOP_RIGHT, -8, HDR_H + 34);

    ck_status = lbl(scr_ck, &lv_font_montserrat_12, C_MUTED, "");
    lv_label_set_long_mode(ck_status, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ck_status, LCD_W - 124);
    lv_obj_align(ck_status, LV_ALIGN_TOP_LEFT, 8, HDR_H + 38);

    /* One light slab with 32 dark squares laid on it, rather than 64 objects:
     * the light squares are never drawn on and never tapped, so they can just
     * be the board showing through. */
    int side = 8 * CK_CELL;
    int x0 = (LCD_W - side) / 2, y0 = HDR_H + 58;
    lv_obj_t *board = rect(scr_ck, side, side, CK_LIGHT);
    lv_obj_set_pos(board, x0, y0);

    for (int i = 0; i < CK_N; i++) {
        int r = CK_ROW(i), c = CK_COL(i);
        lv_obj_t *sq = rect(board, CK_CELL, CK_CELL, CK_DARK);
        lv_obj_set_pos(sq, c * CK_CELL, r * CK_CELL);
        lv_obj_set_style_radius(sq, 0, 0);
        lv_obj_add_flag(sq, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(sq, ck_sq_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        ck_sq[i] = sq;

        lv_obj_t *pc = rect(sq, CK_CELL - 6, CK_CELL - 6, C_RED);
        lv_obj_set_style_radius(pc, LV_RADIUS_CIRCLE, 0);
        lv_obj_center(pc);
        /* The piece must not eat the square's tap: the target is the square. */
        lv_obj_clear_flag(pc, LV_OBJ_FLAG_CLICKABLE);
        ck_pc[i] = pc;

        ck_pl[i] = lbl(pc, &lv_font_montserrat_12, C_INK, "");
        lv_obj_center(ck_pl[i]);
    }
}

static void ck_show(lv_event_t *e)
{
    (void)e;
    if (!scr_ck) ck_build();
    tmr_stop();
    ck_reset();
    lv_scr_load(scr_ck);
    applog("[games] checkers, internal free %u",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

static bool ck_net_active(void)
{
    return ck_mode == CK_VS && ck_peer >= 0 && scr_ck && lv_scr_act() == scr_ck;
}

static void ck_heartbeat(void)
{
    if (!ck_net_active() || ck_over) return;
    if (esp_timer_get_time() - ck_last_tx_us < 2000000) return;
    ck_broadcast(ck_ply == 0);
}

static void menu_build(void)
{
    if (scr_menu) return;
    scr_menu = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr_menu, C_BG, 0);
    lv_obj_clear_flag(scr_menu, LV_OBJ_FLAG_SCROLLABLE);
    mk_hdr(scr_menu, "Games", menu_back);

    /* 🔑 THE MENU SCROLLS, SO NOTHING HAS TO BE CUT ANY MORE.
     *
     * Eight was a hard limit only because the grid was pinned to the screen:
     * 26 header + 12 + 4*62 = 286 of 320, and a ninth button hung off the
     * bottom with nothing on screen to say so. Three finished, working games
     * were sitting in the binary unreachable because of it — Snake since
     * 2026-08-24, Battleship since 09-17 (displaced by Flamingo), Brick
     * Breaker since 09-19 (displaced by Tanks). Every one of those was "still
     * built, still playable, putting it back is one line", and the line never
     * got written because something else always needed the slot.
     *
     * The builder, 2026-09-20: "lets extend the menu to include the games that got cut
     * just have it scroll down to show more i like brick braker and kid 2
     * asked about battal ship".
     *
     * The buttons stay 100x54 on a 62 pitch. 🚨 They are NOT made smaller to
     * fit more in — "tune for the user not the tester": the target is
     * sized for a six-year-old's finger, and scrolling is what gives way.
     *
     * ⚠️ A SCROLLABLE PARENT CHANGES HOW TAPS BEHAVE. LVGL treats a press that
     * moves more than its scroll limit as a drag rather than a click, which is
     * exactly the "you have to press it several times" fault in
     * "lvgl clicked vs released" — and that note says it is worst on a
     * draggable screen. The buttons keep their generous ext_click_area, the
     * scroll is locked to the vertical so a sideways wobble cannot steal a
     * press, and momentum is off so the list cannot drift under a finger that
     * is trying to choose. */
    /* ⚠️ NOT `static` — C_BLUE and friends are lv_color_hex() CALLS, and a
     * function call cannot initialise an object with static storage duration.
     * The same trap is written up in tanks.c's weapon table. */
    const struct { const char *name; lv_color_t c; lv_event_cb_t cb; } g[] = {
        { "Tic-Tac-Toe",   C_BLUE,  ttt_show },
        { "Connect 4",     C_RED,   c4_show  },
        { "Checkers",      C_AMBER, ck_show  },
        { "Hearts",        C_GREEN, hr_show  },
        { "Battleship",    C_BLUE,  bs_show  },   /* back 2026-09-20, Kid 2 asked */
        { "Flamingo",      C_RED,   flamingo_clicked },
        { "Solitaire",     C_INK,   sol_show },
        { "Uno",           C_AMBER, uno_show },
        { "Tanks",         C_GREEN, tanks_clicked },
        { "Brick Breaker", C_AMBER, brk_show },   /* back 2026-09-20, the builder likes it */
        { "Snake",         C_GREEN, snk_show },   /* back 2026-09-20, cut 08-24 */
    };
    const int NGAMES = (int)(sizeof g / sizeof g[0]);

    /* The viewport: everything under the header, above the hint. */
    lv_obj_t *list = lv_obj_create(scr_menu);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, LCD_W, LCD_H - HDR_H - 18);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, HDR_H);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < NGAMES; i++) {
        lv_obj_t *b = btn(list, 100, 54, g[i].c, C_ONAMBER,
                          &lv_font_montserrat_14, g[i].name, g[i].cb, NULL);
        lv_obj_set_pos(b, (i & 1) ? 124 : 16, 12 + (i / 2) * 62);
    }
    /* A spacer so the last row can scroll clear of the hint. LVGL sizes the
     * scrollable content from its children, and the bottom row would otherwise
     * sit flush against the edge with no room to grab. */
    lv_obj_t *pad = lv_obj_create(list);
    lv_obj_remove_style_all(pad);
    lv_obj_set_size(pad, 4, 8);
    lv_obj_set_pos(pad, 0, 12 + ((NGAMES + 1) / 2) * 62);

    lv_obj_t *hint = lbl(scr_menu, &lv_font_montserrat_10, C_MUTED,
                         "the music keeps playing");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -10);
}

void games_show_menu(void)
{
    if (!scr_menu) menu_build();
    tmr_stop();
    lv_scr_load(scr_menu);
}
