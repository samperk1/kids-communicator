/* ------------------------------------------------------------------ *
 *  Flamingo — tap-to-fly side scroller.
 *
 *  THE JOURNEY. An opening scene, then alternating flights and ponds:
 *
 *     OPENING -> FLY level 1 -> POND -> FLY level 2 -> POND -> ...
 *    (skippable)    30 s       rest       45 s        rest
 *
 *  The builder's design. It replaces an endless runner, which could only ever end by
 *  losing — arriving somewhere is a reward that losing cannot take away, and a
 *  child needs something to reach.
 *
 *  🚨 THE OPENING IS SKIPPABLE ON ANY TAP, AND THAT IS NOT OPTIONAL.
 *  The first time it is a story. By the tenth replay it is a toll, and a child
 *  who dies quickly will replay ten times in five minutes. A cutscene you
 *  cannot skip turns into a punishment for playing.
 *
 *  ---- the control, and why it is not Flappy Bird ----
 *
 *  Flappy SETS velocity to a fixed value on every tap: one tap is always the
 *  same hop, and one mistimed tap is fatal. The builder's rule is that tapping FASTER
 *  climbs HIGHER — each tap ADDS an impulse while gravity pulls the whole time.
 *  Altitude is therefore controlled by tap RATE, and there is no single fatal
 *  tap; you are always a few taps from recovering.
 *
 *  🚨 LV_EVENT_PRESSED, NEVER LV_EVENT_CLICKED.
 *  CLICKED only fires if the finger lifts while still INSIDE the object, and
 *  games.c already paid for that: the Back button "needed several presses"
 *  because a finger rolling two pixels on lift-off left a small target. During
 *  fast tapping that would drop taps constantly and read as a broken game.
 *  PRESSED fires on touch-down, on the whole screen, with nothing to miss.
 *
 *  ---- the constants are tuned for a SIX-YEAR-OLD, not for the builder ----
 *
 *  Measured on the test handset 2026-09-17: The builder peaked at 7 taps/sec (fastest gap 99 ms).
 *  The gate was 5, so the scheme is viable. But the builder is not who plays this, and
 *  a child manages roughly half an adult's rate — so the game asks for 1.5
 *  taps/s to hold altitude and 3.5 to climb at the clamp, and the builder's 7 is
 *  headroom rather than the target. Tuning to the adult who happened to run the
 *  test would have produced a game his kids cannot play.
 *
 *  Equilibrium check: taps/s x TAP_IMPULSE == FPS x GRAVITY
 *                     1.5   x 2.4         == 3.6 == 40 x 0.09  ✓
 *
 *  🔑 MAX_UP is what makes panic-tapping SAFE. Above ~3.5 taps/s the extra
 *  impulses are discarded by the clamp, so hammering the screen cannot launch
 *  the bird into the ceiling — it simply climbs at the maximum. A child who
 *  panics must not end up worse off than one who does nothing.
 * ------------------------------------------------------------------ */

#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "nvs.h"

#include "hw.h"
#include "ui.h"
#include "games.h"
#include "applog.h"
#include "sprites.h"
#include "flamingo.h"

/* Palette #define'd per file, as ui.c:54 and games.c:49 both do. */
#define C_SKY     lv_color_hex(0x87CEEB)
#define C_SKY_HI  lv_color_hex(0xB4E4F5)
#define C_GROUND  lv_color_hex(0x6B8E3A)
#define C_DIRT    lv_color_hex(0x7A5C3E)
#define C_PANEL   lv_color_hex(0x121722)
#define C_INK     lv_color_hex(0xE8EDF5)
#define C_AMBER   lv_color_hex(0xF0A830)
#define C_GREEN   lv_color_hex(0x58B368)
#define C_WATER   lv_color_hex(0x2E86C1)

#define HDR_H      26
#define BAR_H      4                        /* the level marker */
#define FPS_MS     25                       /* 40 fps, as Brick Breaker runs */
#define FPS        (1000 / FPS_MS)

#define TAP_IMPULSE   2.4f
#define GRAVITY       0.09f
#define MAX_UP        3.5f
#define MAX_DOWN      3.2f

#define BIRD_X     40
#define BIRD_W     32
#define BIRD_H     24
#define GROUND_H   30
#define GROUND_Y   (LCD_H - GROUND_H)
#define CEIL_Y     (HDR_H + BAR_H)

#define NCLOUD     3
/* ---- the terrain ----
 * Hills and valleys instead of a flat strip. One rect per column, each with a
 * green top border so a single object gives both grass and soil — two objects
 * per column would be 52 of them, and this runs at 40 fps. */
#define COL_W      10
#define NCOL       (LCD_W / COL_W + 2)
#define TERR_HI    20           /* highest a hill rises above the base line */
#define TERR_LO    12           /* deepest a valley drops below it */
#define NOB        8            /* obstacle pool — fixed, recycled, never freed */
#define LIVES0     3
#define MAXLIVES   5
#define INVULN_F   60           /* 1.5 s of mercy after a hit */
#define HIT_INSET  4            /* bird hitbox is SMALLER than the bird */
#define WARN_F     40           /* 1 s of asteroid warning before it falls */

#define PTS_PER_OB 10
#define MAX_LEVEL  6
#define SCROLL_AT(l)  (2.0f + 0.3f * (l))        /* 2.0 -> 3.8 px/frame */
#define SPAWN_AT(l)   (80 - 6 * (l))             /* 2.0 s -> 1.1 s at 40 fps */

typedef enum {
    OB_NONE = 0, OB_TREE, OB_CACTUS, OB_BIRD, OB_ROCK, OB_PLANE, OB_ASTEROID
} obtype_t;

typedef struct {
    obtype_t  t;
    float     x, y, vy;
    int       w, h;
    /* Which of the three looks this one wears. For a tree or a cactus that is
     * its SIZE; for a mean bird it is its COLOUR. Same field, because in both
     * cases it means "which variant", and a second field would only ever be
     * valid for half the types. */
    uint8_t   var;
    bool      scored;
    lv_obj_t *img;
} ob_t;

/* OPENING: the flock at the pond, the plane, the scatter. Skippable.
 * FLYING:  the game.
 * ARRIVING: level distance is done — stop spawning and let the sky clear, so
 *           nobody arrives at a pond with an asteroid still overhead.
 * REST:    the pond scrolls in, the bird lands, +1 life, "Level N".
 * OVER:    the panel. */
typedef enum { ST_OPENING, ST_FLYING, ST_ARRIVING, ST_REST, ST_OVER } state_t;

static lv_obj_t   *scr, *bird, *lbl_dbg, *lbl_score, *bar_fill;
static lv_obj_t   *terr[NCOL];
static lv_obj_t   *biplane_op, *banner, *puff[4];
static lv_obj_t   *clouds[NCLOUD], *life_img[MAXLIVES];
static lv_obj_t   *over_panel, *lbl_big, *warn_obj;
/* One set of scene objects, built once and repositioned for the opening AND
 * every rest stop — same rule as the obstacle pool. */
static lv_obj_t   *pond, *sc_nest, *sc_baby[2], *sc_duck, *sc_turtle, *sc_reed[2];
static lv_obj_t   *flock[3], *plane_op;
static lv_timer_t *tmr;

static ob_t     obs[NOB];
static state_t  st;
static int      st_t, rest_phase;
static float    scene_x;
static float    bird_y, vy;
static float    cloud_x[NCLOUD];
static int      th[NCOL];               /* ground top, per column */
static float    terr_acc;               /* sub-column scroll carry */
static bool     flatten;                /* level the land for a pond */
static float    bip_x;
static float    flock_y[3];
static float    plane_op_x;
static int      frame, wing;
static int      score, lives, level, invuln, spawn_cd, dist;
static int      warn_t;
static float    warn_x;
static uint32_t tap_ms[16];
static int      tap_head;

static void game_over(void);
static void start_level(int lv);
static void enter_flying(void);
static void bip_show(bool on);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static int taps_per_sec(void)
{
    uint32_t t = now_ms();
    int n = 0;
    for (int i = 0; i < 16; i++)
        if (tap_ms[i] && (t - tap_ms[i]) <= 1000) n++;
    return n;
}

/* Ramping level length: 30 s, 45, 60, 75, then 90 and no longer. Short enough
 * early that a beginner reaches a pond; long enough later to feel like a trip,
 * capped so a good run never becomes a chore. */
static int level_frames(int lv)
{
    int sec = 30 + (lv - 1) * 15;
    if (sec > 90) sec = 90;
    return sec * FPS;
}

/* ---- high score --------------------------------------------------------- */
static int hi_score_get(void)
{
    nvs_handle_t h;
    int32_t v = 0;
    if (nvs_open("games", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "flamingo_hi", &v);
        nvs_close(h);
    }
    return (int)v;
}

static void hi_score_set(int v)
{
    nvs_handle_t h;
    if (nvs_open("games", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "flamingo_hi", (int32_t)v);
    nvs_commit(h);
    nvs_close(h);
}

static void on_press(lv_event_t *e)
{
    (void)e;
    if (st == ST_OPENING) { enter_flying(); return; }   /* skip — see header */
    if (st != ST_FLYING && st != ST_ARRIVING) return;
    vy -= TAP_IMPULSE;
    if (vy < -MAX_UP) vy = -MAX_UP;         /* the clamp — see the header */
    tap_ms[tap_head] = now_ms();
    tap_head = (tap_head + 1) % 16;
}

/* ---- the terrain --------------------------------------------------------
 *
 * A height per column, scrolled by shifting the array rather than moving the
 * objects: the columns stay at fixed x apart from a sub-column offset, so only
 * the heights travel. Everything that stands on the ground — obstacles, the
 * bird's floor — reads its y from here, so there is ONE definition of where
 * the ground is and nothing can disagree with the picture. */
static int terr_next(int prev)
{
    if (flatten) {
        /* Approaching a pond the land levels out. Not decoration: a pond
         * pasted onto a hillside looks wrong, and a flat approach is also a
         * fair one — nobody should arrive at a rest stop over a valley. */
        if (prev < GROUND_Y) return prev + 2 > GROUND_Y ? GROUND_Y : prev + 2;
        if (prev > GROUND_Y) return prev - 2 < GROUND_Y ? GROUND_Y : prev - 2;
        return GROUND_Y;
    }
    int step = (int)(esp_random() % 7) - 3;          /* -3..+3, a gentle walk */
    int v = prev + step;
    if (v < GROUND_Y - TERR_HI) v = GROUND_Y - TERR_HI;
    if (v > GROUND_Y + TERR_LO) v = GROUND_Y + TERR_LO;
    return v;
}

static void terr_apply(void)
{
    int off = (int)terr_acc;
    for (int i = 0; i < NCOL; i++) {
        lv_obj_set_pos(terr[i], i * COL_W - off, th[i]);
        lv_obj_set_height(terr[i], LCD_H - th[i]);
    }
}

static void terr_scroll(float px)
{
    terr_acc += px;
    while (terr_acc >= COL_W) {
        terr_acc -= COL_W;
        for (int i = 0; i < NCOL - 1; i++) th[i] = th[i + 1];
        th[NCOL - 1] = terr_next(th[NCOL - 2]);
    }
    terr_apply();
}

/* The ground's y at a world x. Everything that touches the ground uses this. */
static int terr_y_at(float x)
{
    int i = (int)((x + terr_acc) / COL_W);
    if (i < 0) i = 0;
    if (i >= NCOL) i = NCOL - 1;
    return th[i];
}

/* ---- the scene ---------------------------------------------------------- */
static void scene_place(float x)
{
    lv_obj_set_pos(pond,       (int)x,        GROUND_Y - 10);
    lv_obj_set_pos(sc_reed[0], (int)x + 6,    GROUND_Y - 26);
    lv_obj_set_pos(sc_reed[1], (int)x + 128,  GROUND_Y - 26);
    lv_obj_set_pos(sc_nest,    (int)x + 50,   GROUND_Y - 14);
    lv_obj_set_pos(sc_baby[0], (int)x + 52,   GROUND_Y - 24);
    lv_obj_set_pos(sc_baby[1], (int)x + 62,   GROUND_Y - 24);
    lv_obj_set_pos(sc_duck,    (int)x + 96,   GROUND_Y - 18);
    lv_obj_set_pos(sc_turtle,  (int)x + 24,   GROUND_Y - 16);
    for (int i = 0; i < 3; i++)
        lv_obj_set_pos(flock[i], (int)x + 8 + i * 34, (int)flock_y[i]);
}

static void scene_show(bool opening, bool on)
{
    lv_obj_t *always[] = { pond, sc_reed[0], sc_reed[1] };
    for (unsigned i = 0; i < sizeof(always) / sizeof(*always); i++)
        on ? lv_obj_clear_flag(always[i], LV_OBJ_FLAG_HIDDEN)
           : lv_obj_add_flag(always[i], LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *family[] = { sc_nest, sc_baby[0], sc_baby[1], sc_duck, sc_turtle };
    for (unsigned i = 0; i < sizeof(family) / sizeof(*family); i++)
        (on && !opening) ? lv_obj_clear_flag(family[i], LV_OBJ_FLAG_HIDDEN)
                         : lv_obj_add_flag(family[i], LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++)
        (on && opening) ? lv_obj_clear_flag(flock[i], LV_OBJ_FLAG_HIDDEN)
                        : lv_obj_add_flag(flock[i], LV_OBJ_FLAG_HIDDEN);
}

/* ---- obstacles ---------------------------------------------------------- */
static const lv_img_dsc_t *ob_src(obtype_t t, uint8_t var)
{
    switch (t) {
        case OB_TREE:     return var == 0 ? &tree_s   : var == 2 ? &tree_l   : &tree;
        case OB_CACTUS:   return var == 0 ? &cactus_s : var == 2 ? &cactus_l : &cactus;
        case OB_BIRD:     return var == 0 ? &meanbird : var == 1 ? &meanbird2
                                                                : &meanbird3;
        case OB_ROCK:     return &rock;
        case OB_PLANE:    return &airplane;
        case OB_ASTEROID: return &asteroid;
        default:          return &tree;
    }
}

static void ob_size(obtype_t t, uint8_t var, int *w, int *h)
{
    switch (t) {
        case OB_TREE:   if (var == 0) { *w = 10; *h = 16; }
                        else if (var == 2) { *w = 18; *h = 30; }
                        else { *w = 14; *h = 22; }                break;
        case OB_CACTUS: if (var == 0) { *w = 10; *h = 12; }
                        else if (var == 2) { *w = 14; *h = 22; }
                        else { *w = 12; *h = 16; }                break;
        case OB_BIRD:     *w = 16; *h = 12; break;
        case OB_ROCK:     *w =  8; *h =  8; break;
        case OB_PLANE:    *w = 32; *h = 12; break;
        case OB_ASTEROID: *w = 10; *h = 10; break;
        default:          *w =  8; *h =  8; break;
    }
}

static bool ob_on_ground(obtype_t t) { return t == OB_TREE || t == OB_CACTUS; }

static ob_t *ob_free_slot(void)
{
    for (int i = 0; i < NOB; i++) if (obs[i].t == OB_NONE) return &obs[i];
    return NULL;                            /* pool full: skip, never grow */
}

static void ob_place(ob_t *o, obtype_t t, float x, float y, float vy0, uint8_t var)
{
    o->t = t; o->x = x; o->y = y; o->vy = vy0; o->scored = false; o->var = var;
    ob_size(t, var, &o->w, &o->h);
    lv_img_set_src(o->img, ob_src(t, var));
    lv_obj_set_pos(o->img, (int)x, (int)y);
    lv_obj_clear_flag(o->img, LV_OBJ_FLAG_HIDDEN);
}

static void spawn_ob(void)
{
    ob_t *o = ob_free_slot();
    if (!o) return;

    /* Gated on level, so the first 30 s teaches the controls with nothing but
     * ground obstacles before anything comes at altitude. */
    int roll = esp_random() % 100;
    obtype_t t;
    if      (level >= 3 && roll < 14) t = OB_ASTEROID;
    else if (level >= 2 && roll < 28) t = OB_PLANE;
    else if (level >= 2 && roll < 48) t = OB_BIRD;
    else if (level >= 3 && roll < 60) t = OB_ROCK;
    else                              t = (roll & 1) ? OB_TREE : OB_CACTUS;

    if (t == OB_ASTEROID) {
        /* 🚨 An asteroid is NOT spawned here — it is ANNOUNCED.
         * A hazard entering from ahead can be seen and planned for; one falling
         * from above cannot, so without a warning it is a dice roll rather than
         * a dodge. A child cannot articulate that difference — they just decide
         * the game cheats. The marker flashes at the x it will fall down. */
        if (warn_t > 0) return;             /* one at a time */
        warn_x = 30.0f + (float)(esp_random() % (LCD_W - 60));
        warn_t = WARN_F;
        lv_obj_set_pos(warn_obj, (int)warn_x, CEIL_Y + 2);
        lv_obj_clear_flag(warn_obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    if (t == OB_PLANE) {
        int lo = CEIL_Y + 10, hi = GROUND_Y - 12 - 70;
        ob_place(o, t, LCD_W + 4, lo + (float)(esp_random() % (hi - lo)), 0, 0);
    } else if (t == OB_BIRD) {
        /* Never at the very top or scraping the ground — a mean bird that can
         * only be dodged by flying into the ceiling is not a dodge.
         * `var` here is its COLOUR: three morphs, picked per bird. */
        int lo = CEIL_Y + 24, hi = GROUND_Y - 12 - 50;
        ob_place(o, t, LCD_W + 4, lo + (float)(esp_random() % (hi - lo)), 0,
                 (uint8_t)(esp_random() % 3));
    } else if (t == OB_ROCK) {
        ob_place(o, t, LCD_W + 4, (float)terr_y_at(LCD_W) - 8, -4.6f, 0);
    } else {
        /* Ground obstacles come in three sizes. The big ones are rarer — a
         * screen of large trees is a wall, not a challenge. */
        int r = esp_random() % 100;
        uint8_t var = (r < 35) ? 0 : (r < 80) ? 1 : 2;
        int w, h; ob_size(t, var, &w, &h);
        ob_place(o, t, LCD_W + 4, (float)terr_y_at(LCD_W + 4) - h, 0, var);
    }
}

static void ob_retire(ob_t *o)
{
    o->t = OB_NONE;
    lv_obj_add_flag(o->img, LV_OBJ_FLAG_HIDDEN);
}

static bool any_ob_active(void)
{
    for (int i = 0; i < NOB; i++) if (obs[i].t != OB_NONE) return true;
    return warn_t > 0;
}

/* Axis-aligned overlap, with the BIRD's box pulled in on every side.
 * A hitbox slightly smaller than the sprite always feels fair; one slightly
 * larger always feels cheated, and a six-year-old will not articulate which —
 * they will just say the game is mean. */
static bool collide(const ob_t *o)
{
    int bx = BIRD_X + HIT_INSET,     by = (int)bird_y + HIT_INSET;
    int bw = BIRD_W - 2 * HIT_INSET, bh = BIRD_H - 2 * HIT_INSET;
    return bx < o->x + o->w && bx + bw > o->x &&
           by < o->y + o->h && by + bh > o->y;
}

static void draw_lives(void)
{
    for (int i = 0; i < MAXLIVES; i++) {
        if (i < lives) lv_obj_clear_flag(life_img[i], LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(life_img[i], LV_OBJ_FLAG_HIDDEN);
    }
}

/* 🚨 A HIT MUST GRANT INVULNERABILITY, AND THIS IS NOT A POLISH DETAIL.
 * The bird and the obstacle overlap for many consecutive frames — at 40 fps a
 * single collision is ~10 ticks. Without a mercy window one bump spends every
 * life at once and the game ends instantly with no way to understand why. */
static void take_hit(void)
{
    if (invuln > 0) return;
    lives--;
    invuln = INVULN_F;
    draw_lives();
    applog("[flamingo] hit, %d lives left, score %d, level %d", lives, score, level);
    if (lives <= 0) game_over();
}

/* ---- state transitions -------------------------------------------------- */
static void start_level(int lv)
{
    level = lv;
    dist = 0;
    spawn_cd = SPAWN_AT(level > MAX_LEVEL ? MAX_LEVEL : level);
    warn_t = 0;
    lv_obj_add_flag(warn_obj, LV_OBJ_FLAG_HIDDEN);
    applog("[flamingo] level %d, %d s", level, level_frames(level) / FPS);
}

static void enter_flying(void)
{
    bip_show(false);
    scene_show(true, false);
    scene_show(false, false);
    lv_obj_add_flag(plane_op, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(bird, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_score, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_big, LV_OBJ_FLAG_HIDDEN);
    bird_y = (GROUND_Y - BIRD_H) / 2.0f;
    vy = 0;
    st = ST_FLYING;
    st_t = 0;
    if (level == 0) start_level(1);
}

static void enter_rest(void)
{
    st = ST_REST;
    st_t = 0;
    rest_phase = 0;
    scene_x = LCD_W;
    scene_show(false, true);
    scene_place(scene_x);
}

/* ---- the states --------------------------------------------------------- */
static void bip_show(bool on)
{
    on ? lv_obj_clear_flag(biplane_op, LV_OBJ_FLAG_HIDDEN)
       : lv_obj_add_flag(biplane_op, LV_OBJ_FLAG_HIDDEN);
    on ? lv_obj_clear_flag(banner, LV_OBJ_FLAG_HIDDEN)
       : lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 4; i++)
        on ? lv_obj_clear_flag(puff[i], LV_OBJ_FLAG_HIDDEN)
           : lv_obj_add_flag(puff[i], LV_OBJ_FLAG_HIDDEN);
}

static void tick_opening(void)
{
    /*   0- 95  the biplane tows the FLAMINGO banner across
     * 100-200  the jet comes over the pond
     * 130+     the flock scatters
     * any tap  skips the lot — see the file header. */
    if (st_t < 95) {
        bip_x -= 5.0f;
        lv_obj_set_x(biplane_op, (int)bip_x);
        lv_obj_set_x(banner,     (int)bip_x + 36);
        for (int i = 0; i < 4; i++)
            lv_obj_set_x(puff[i], (int)bip_x + 138 + i * 15);
    } else if (st_t == 95) {
        bip_show(false);
    }

    if (st_t >= 100 && st_t < 200) {
        plane_op_x -= 4.2f;
        lv_obj_set_x(plane_op, (int)plane_op_x);
        lv_obj_clear_flag(plane_op, LV_OBJ_FLAG_HIDDEN);
    }
    if (st_t >= 130) {
        for (int i = 0; i < 3; i++) flock_y[i] -= 2.2f + i * 0.35f;
        scene_place(scene_x);
    }
    if (st_t > 215) enter_flying();
}

static void tick_flying(void)
{
    int lvc = level > MAX_LEVEL ? MAX_LEVEL : level;
    float scroll = SCROLL_AT(lvc);

    if (st == ST_FLYING) {
        dist++;
        if (dist >= level_frames(level)) {
            st = ST_ARRIVING;               /* let the sky clear first */
            flatten = true;                 /* and the land level out */
            applog("[flamingo] level %d cleared, score %d", level, score);
        }
        if (--spawn_cd <= 0) { spawn_ob(); spawn_cd = SPAWN_AT(lvc); }
    }

    /* the asteroid warning, then the asteroid */
    if (warn_t > 0) {
        warn_t--;
        if ((frame / 4) & 1) lv_obj_add_flag(warn_obj, LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_clear_flag(warn_obj, LV_OBJ_FLAG_HIDDEN);
        if (warn_t == 0) {
            lv_obj_add_flag(warn_obj, LV_OBJ_FLAG_HIDDEN);
            ob_t *o = ob_free_slot();
            if (o) ob_place(o, OB_ASTEROID, warn_x, (float)CEIL_Y, 1.4f, 0);
        }
    }

    for (int i = 0; i < NOB; i++) {
        ob_t *o = &obs[i];
        if (o->t == OB_NONE) continue;

        switch (o->t) {
            case OB_PLANE:    o->x -= scroll * 2.2f; break;
            case OB_BIRD:     o->x -= scroll * 1.3f; break;
            case OB_ASTEROID: o->x -= scroll * 0.5f; o->vy += 0.12f;
                              o->y += o->vy;         break;
            case OB_ROCK:     o->x -= scroll;
                              o->vy += GRAVITY * 1.6f; o->y += o->vy; break;
            default:          o->x -= scroll;        break;
        }
        /* A tree stands ON the ground, and the ground now moves. Re-reading its
         * y every frame from terr_y_at() is what keeps a trunk from floating
         * over a valley or sinking into a hill. */
        if (ob_on_ground(o->t)) o->y = (float)terr_y_at(o->x + o->w / 2) - o->h;
        lv_obj_set_pos(o->img, (int)o->x, (int)o->y);

        if (!o->scored && o->x + o->w < BIRD_X) {
            o->scored = true;
            score += PTS_PER_OB;
            char b[16];
            snprintf(b, sizeof(b), "%d", score);
            lv_label_set_text(lbl_score, b);
        }
        /* A rock or an asteroid is retired at the GROUND, not at the bottom of
         * the panel: past GROUND_Y it would be drawn over the dirt strip, a
         * stone falling through solid earth. */
        float gone_y = (o->t == OB_ROCK || o->t == OB_ASTEROID)
                       ? (float)terr_y_at(o->x) : (float)LCD_H;
        if (o->x + o->w < -40 || o->y > gone_y) { ob_retire(o); continue; }
        if (collide(o)) take_hit();
        if (st == ST_OVER) return;
    }

    terr_scroll(scroll);
    if (st == ST_ARRIVING && !any_ob_active()) enter_rest();

    int lf = level_frames(level);
    int w = (dist >= lf) ? LCD_W : (dist * LCD_W) / lf;
    lv_obj_set_width(bar_fill, w < 1 ? 1 : w);

    for (int i = 0; i < NCLOUD; i++) {
        cloud_x[i] -= scroll / 3.0f;
        if (cloud_x[i] < -40) cloud_x[i] = LCD_W + (float)(esp_random() % 60);
        lv_obj_set_x(clouds[i], (int)cloud_x[i]);
    }
}

static void tick_rest(void)
{
    float scroll = SCROLL_AT(level > MAX_LEVEL ? MAX_LEVEL : level);

    if (rest_phase == 0) {                  /* the pond slides in */
        terr_scroll(scroll);
        scene_x -= scroll;
        if (scene_x <= 45) { scene_x = 45; rest_phase = 1; st_t = 0; }
        scene_place(scene_x);
    } else if (rest_phase == 1) {           /* land, reward, hold */
        if (st_t == 1) {
            if (lives < MAXLIVES) lives++;
            draw_lives();
            char b[32];
            snprintf(b, sizeof(b), "Level %d", level + 1);
            lv_label_set_text(lbl_big, b);
            lv_obj_clear_flag(lbl_big, LV_OBJ_FLAG_HIDDEN);
            applog("[flamingo] pond: +1 life (%d), next level %d", lives, level + 1);
        }
        if (st_t > 150) { rest_phase = 2; }
    } else {                                /* and on we go */
        terr_scroll(scroll);
        scene_x -= scroll;
        scene_place(scene_x);
        if (scene_x < -170) {
            scene_show(false, false);
            lv_obj_add_flag(lbl_big, LV_OBJ_FLAG_HIDDEN);
            start_level(level + 1);
            flatten = false;
            st = ST_FLYING;
            st_t = 0;
        }
    }

    /* The bird glides down to the pond and stands there, under no control. */
    float target = (float)terr_y_at(BIRD_X + BIRD_W / 2) - BIRD_H;
    if (bird_y < target) { bird_y += 1.6f; if (bird_y > target) bird_y = target; }
    lv_obj_set_y(bird, (int)bird_y);
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (st == ST_OVER) return;
    frame++;
    st_t++;

    if (st == ST_OPENING) { tick_opening(); return; }

    if (st == ST_FLYING || st == ST_ARRIVING) {
        vy += GRAVITY;
        if (vy >  MAX_DOWN) vy =  MAX_DOWN;
        if (vy < -MAX_UP)   vy = -MAX_UP;
        bird_y += vy;

        if (bird_y < CEIL_Y) { bird_y = CEIL_Y; vy = 0; }
        float floor_y = (float)terr_y_at(BIRD_X + BIRD_W / 2) - BIRD_H;
        if (bird_y > floor_y) {
            bird_y = floor_y;
            vy = 0;
            take_hit();
            if (st == ST_OVER) return;
        }
        lv_obj_set_y(bird, (int)bird_y);

        if (invuln > 0) {
            invuln--;
            if ((frame / 4) & 1) lv_obj_add_flag(bird, LV_OBJ_FLAG_HIDDEN);
            else                 lv_obj_clear_flag(bird, LV_OBJ_FLAG_HIDDEN);
            if (invuln == 0) lv_obj_clear_flag(bird, LV_OBJ_FLAG_HIDDEN);
        }
    }

    int period = (vy < -0.5f) ? 3 : 6;
    if (frame % period == 0) {
        wing ^= 1;
        lv_img_set_src(bird, wing ? &flamingo2 : &flamingo1);
    }

    if (st == ST_FLYING || st == ST_ARRIVING) tick_flying();
    else if (st == ST_REST)                   tick_rest();

    if (frame % 8 == 0) {
        char b[40];
        snprintf(b, sizeof(b), "%d/s  L%d", taps_per_sec(), level);
        lv_label_set_text(lbl_dbg, b);
    }
}

/* ---- game over ---------------------------------------------------------- */
static void start_run(void);

static void again_clicked(lv_event_t *e)
{
    (void)e;
    lv_obj_add_flag(over_panel, LV_OBJ_FLAG_HIDDEN);
    start_run();
}

static void game_over(void)
{
    st = ST_OVER;
    int hi = hi_score_get();
    bool beat = score > hi;
    if (beat) { hi = score; hi_score_set(hi); }

    applog("[flamingo] game over, score %d, level %d, high %d%s",
           score, level, hi, beat ? " (NEW BEST)" : "");

    lv_obj_t *t  = lv_obj_get_child(over_panel, 0);
    lv_obj_t *sc = lv_obj_get_child(over_panel, 1);
    char b[64];
    lv_label_set_text(t, beat ? "NEW BEST!" : "Game Over");
    snprintf(b, sizeof(b), "score %d\nlevel %d\nbest  %d", score, level, hi);
    lv_label_set_text(sc, b);
    lv_obj_clear_flag(over_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(over_panel);
}

static void back(lv_event_t *e)
{
    (void)e;
    if (tmr) { lv_timer_del(tmr); tmr = NULL; }
    games_show_menu();
    if (scr) { lv_obj_del_async(scr); scr = NULL; }
}

static void start_run(void)
{
    memset(tap_ms, 0, sizeof(tap_ms));
    tap_head = frame = wing = 0;
    score = 0; level = 0; invuln = 0; dist = 0; warn_t = 0;
    lives = LIVES0;
    bird_y = (GROUND_Y - BIRD_H) / 2.0f;
    vy = 0;
    for (int i = 0; i < NOB; i++) ob_retire(&obs[i]);
    lv_obj_add_flag(warn_obj, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl_score, "0");
    draw_lives();

    /* Flat land to begin with, so the opening pond sits properly. */
    flatten = false;
    terr_acc = 0;
    for (int i = 0; i < NCOL; i++) th[i] = GROUND_Y;
    terr_apply();

    /* the opening */
    st = ST_OPENING;
    st_t = 0;
    scene_x = 45;
    bip_x = LCD_W + 10;
    bip_show(true);
    lv_obj_set_pos(biplane_op, (int)bip_x, CEIL_Y + 18);
    lv_obj_set_pos(banner, (int)bip_x + 36, CEIL_Y + 16);
    for (int i = 0; i < 4; i++)
        lv_obj_set_pos(puff[i], (int)bip_x + 138 + i * 15, CEIL_Y + 22 + i);
    plane_op_x = LCD_W + 20;
    for (int i = 0; i < 3; i++) flock_y[i] = (float)(GROUND_Y - BIRD_H - 2);
    scene_show(true, true);
    scene_place(scene_x);
    lv_obj_set_pos(plane_op, (int)plane_op_x, CEIL_Y + 30);
    lv_obj_add_flag(plane_op, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bird, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_score, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl_big, "tap to skip");
    lv_obj_clear_flag(lbl_big, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_width(bar_fill, 1);
}

static lv_obj_t *mk_img(lv_obj_t *p, const lv_img_dsc_t *src)
{
    lv_obj_t *o = lv_img_create(p);
    lv_img_set_src(o, src);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static lv_obj_t *mk_rect(lv_obj_t *p, int w, int h, lv_color_t c, int r)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void flamingo_show(void)
{
    scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(scr, C_SKY, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < NCLOUD; i++) {
        clouds[i] = mk_rect(scr, 34 + i * 8, 12, C_SKY_HI, 6);
        cloud_x[i] = 40.0f + i * 90.0f;
        lv_obj_set_pos(clouds[i], (int)cloud_x[i], CEIL_Y + 14 + i * 34);
    }

    /* One rect per column, soil-coloured with a GREEN TOP BORDER. The border
     * is the grass: it gives both surfaces from a single object, where a
     * separate grass strip would double 26 objects to 52 on a 40 fps loop. */
    for (int i = 0; i < NCOL; i++) {
        terr[i] = mk_rect(scr, COL_W, GROUND_H, C_DIRT, 0);
        lv_obj_set_style_border_color(terr[i], C_GROUND, 0);
        lv_obj_set_style_border_width(terr[i], 3, 0);
        lv_obj_set_style_border_side(terr[i], LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_opa(terr[i], LV_OPA_COVER, 0);
        lv_obj_set_pos(terr[i], i * COL_W, GROUND_Y);
    }

    /* the pond scene — built once, used by the opening and every rest stop */
    pond = mk_rect(scr, 150, 20, C_WATER, 9);
    lv_obj_add_flag(pond, LV_OBJ_FLAG_HIDDEN);
    sc_reed[0] = mk_img(scr, &cattail);
    sc_reed[1] = mk_img(scr, &cattail);
    sc_nest    = mk_img(scr, &nest);
    sc_baby[0] = mk_img(scr, &babyflam);
    sc_baby[1] = mk_img(scr, &babyflam);
    sc_duck    = mk_img(scr, &duck);
    sc_turtle  = mk_img(scr, &turtle);
    for (int i = 0; i < 3; i++) flock[i] = mk_img(scr, &flamingo1);
    plane_op = mk_img(scr, &airplane);

    /* The title: a biplane towing a banner, with pink puffs trailing it.
     * The builder offered a banner OR a skywriter in pink smoke. At 240 px wide,
     * skywritten letters would be about 8 px tall and unreadable, so the title
     * rides a towed banner where a real font can carry it — and the smoke idea
     * survives as the pink trail and the pink lettering. */
    biplane_op = mk_img(scr, &biplane);
    banner = mk_rect(scr, 96, 20, lv_color_hex(0xFFFFFF), 3);
    lv_obj_add_flag(banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *bt = lv_label_create(banner);
    lv_obj_set_style_text_font(bt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bt, lv_color_hex(0xF06292), 0);
    lv_label_set_text(bt, "FLAMINGO");
    lv_obj_center(bt);
    for (int i = 0; i < 4; i++) {
        puff[i] = mk_rect(scr, 7 - i, 7 - i, lv_color_hex(0xF49AC1), 4);
        lv_obj_add_flag(puff[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < NOB; i++) {
        obs[i].img = mk_img(scr, &tree);
        obs[i].t = OB_NONE;
    }

    /* the asteroid's telegraph */
    warn_obj = mk_rect(scr, 12, 12, C_AMBER, 2);
    lv_obj_add_flag(warn_obj, LV_OBJ_FLAG_HIDDEN);

    bird = lv_img_create(scr);
    lv_img_set_src(bird, &flamingo1);
    lv_obj_set_pos(bird, BIRD_X, 100);

    lv_obj_t *hdr = mk_rect(scr, LCD_W, HDR_H, C_PANEL, 0);
    lv_obj_set_pos(hdr, 0, 0);

    /* the level marker: how much further to the pond, answered continuously */
    lv_obj_t *track = mk_rect(scr, LCD_W, BAR_H, lv_color_hex(0x2A3342), 0);
    lv_obj_set_pos(track, 0, HDR_H);
    bar_fill = mk_rect(track, 1, BAR_H, C_GREEN, 0);
    lv_obj_set_pos(bar_fill, 0, 0);

    lv_obj_t *bk = lv_btn_create(scr);
    lv_obj_remove_style_all(bk);
    lv_obj_set_size(bk, 56, HDR_H);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_add_event_cb(bk, back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(bk);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(bl, C_AMBER, 0);
    lv_label_set_text(bl, "Back");
    lv_obj_center(bl);

    lbl_score = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_score, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl_score, C_INK, 0);
    lv_label_set_text(lbl_score, "0");
    lv_obj_align(lbl_score, LV_ALIGN_TOP_MID, -14, 4);

    for (int i = 0; i < MAXLIVES; i++) {
        life_img[i] = lv_img_create(scr);
        lv_img_set_src(life_img[i], &pu_life);
        lv_obj_set_pos(life_img[i], LCD_W - 18 - i * 14, 4);
    }

    lbl_big = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_big, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(lbl_big, C_INK, 0);
    lv_obj_set_style_text_align(lbl_big, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(lbl_big, "");
    lv_obj_align(lbl_big, LV_ALIGN_TOP_MID, 0, CEIL_Y + 30);

    lbl_dbg = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_dbg, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(lbl_dbg, C_INK, 0);
    lv_label_set_text(lbl_dbg, "0/s  L0");
    lv_obj_align(lbl_dbg, LV_ALIGN_BOTTOM_LEFT, 4, -4);

    /* Built once and hidden — not created on death, because allocating a
     * screenful of widgets at the moment the game is already doing the most
     * work is how a game stutters when it should feel final. */
    over_panel = mk_rect(scr, 180, 165, C_PANEL, 6);
    lv_obj_center(over_panel);
    lv_obj_add_flag(over_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *ot = lv_label_create(over_panel);          /* child 0 */
    lv_obj_set_style_text_font(ot, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(ot, C_AMBER, 0);
    lv_label_set_text(ot, "Game Over");
    lv_obj_align(ot, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *os = lv_label_create(over_panel);          /* child 1 */
    lv_obj_set_style_text_font(os, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(os, C_INK, 0);
    lv_obj_set_style_text_align(os, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(os, "score 0\nlevel 0\nbest  0");
    lv_obj_align(os, LV_ALIGN_TOP_MID, 0, 38);

    lv_obj_t *ag = lv_btn_create(over_panel);
    lv_obj_remove_style_all(ag);
    lv_obj_set_size(ag, 140, 40);            /* big: pressed more than anything */
    lv_obj_set_style_bg_color(ag, C_AMBER, 0);
    lv_obj_set_style_bg_opa(ag, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ag, 4, 0);
    lv_obj_align(ag, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_add_event_cb(ag, again_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *agl = lv_label_create(ag);
    lv_obj_set_style_text_font(agl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(agl, lv_color_hex(0x16120A), 0);
    lv_label_set_text(agl, "Play Again");
    lv_obj_center(agl);

    lv_obj_add_event_cb(scr, on_press, LV_EVENT_PRESSED, NULL);

    start_run();
    lv_scr_load(scr);
    tmr = lv_timer_create(tick, FPS_MS, NULL);
    applog("[flamingo] start, best %d", hi_score_get());
}
