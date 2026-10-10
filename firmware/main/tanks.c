/*
 * Tanks — turn-based artillery, step 1: hot-seat on one handset.
 *
 * The radio is NOT here yet, and
 * deliberately so: the plan's gate is that two players who both keep missing
 * must still reach an end, and that is provable without a second board.
 *
 * 🔑 INTEGER PHYSICS, NO FLOAT. Two handsets will later run this same
 * simulation from a shared seed and must agree byte for byte. Anything
 * "realistic but slightly different at each end" is worse than useless — one
 * child would see a kill and the other a miss, with nothing in the protocol
 * able to tell. Writing it in fixed point now costs nothing and means the
 * multiplayer step is a transport job rather than a rewrite.
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "hw.h"
#include "applog.h"
#include "games.h"
#include "tanks.h"

static const char *TAG = "tanks";

/* ---- colours, matching games.c ------------------------------------------ */
#define C_BG      lv_color_hex(0x080A0E)
#define C_PANEL   lv_color_hex(0x121722)
#define C_LINE    lv_color_hex(0x1E2636)
#define C_INK     lv_color_hex(0xE8EDF5)
#define C_MUTED   lv_color_hex(0x7C8798)
#define C_AMBER   lv_color_hex(0xF0A830)
#define C_BLUE    lv_color_hex(0x6E9BD1)
#define C_GREEN   lv_color_hex(0x58B368)
#define C_RED     lv_color_hex(0xD9534F)
#define HDR_H 26

/* ---- geometry ------------------------------------------------------------ */
#define PLAY_W   LCD_W                 /* 240 */
/* 🚨 172, NOT 196. At 196 the bottom of this screen ran off the bottom of the
 * panel: FIRE was clipped by 10px, NEW by 5, and the AMMO button sat at y=328
 * on a 320px display — COMPLETELY INVISIBLE. The Heavy shell and the Digger
 * are sold in the shop and there has never been a way to select them. Nobody
 * noticed because the game is playable with the free shell alone.
 * ⚠️ This is a SHARED SIMULATION CONSTANT — terrain heights and the off-map
 * test both use it — so the two handsets must be flashed together. */
#define PLAY_H   172
#define PLAY_Y   (HDR_H + 2)

/* ---- fixed point --------------------------------------------------------- */
/* 1/16 px. Small enough that a shell never tunnels through a 1px ridge at the
 * speeds this game uses, large enough that an int32 cannot overflow across a
 * whole flight. */
#define FP_SHIFT 4
#define FP_ONE   (1 << FP_SHIFT)
#define TO_PX(v) ((v) >> FP_SHIFT)
#define TO_FP(v) ((v) << FP_SHIFT)

/* Per tick, in 1/16 px per tick. Tuned so a mid-power shot crosses roughly half
 * the screen — the numbers are arbitrary but they must be the SAME arbitrary
 * numbers at both ends, which is why they are constants and not settings. */
#define GRAVITY_FP 3
#define TICK_MS    25

/* sin(0..90 degrees) * 1024. Table, not libm: identical on every build. */
static const int16_t SIN_T[91] = {
       0,   18,   36,   54,   71,   89,  107,  125,  143,  160,
     178,  195,  213,  230,  248,  265,  282,  299,  316,  333,
     350,  367,  384,  400,  416,  433,  449,  465,  481,  496,
     512,  527,  543,  558,  573,  587,  602,  616,  630,  644,
     658,  672,  685,  698,  711,  724,  737,  749,  761,  773,
     784,  796,  807,  818,  828,  839,  849,  859,  868,  878,
     887,  896,  904,  912,  920,  928,  935,  943,  949,  956,
     962,  968,  974,  979,  984,  989,  994,  998, 1002, 1005,
    1008, 1011, 1014, 1016, 1018, 1020, 1022, 1023, 1023, 1024,
    1024,
};
static int isin(int deg) { if (deg < 0) deg = 0; if (deg > 90) deg = 90; return SIN_T[deg]; }
static int icos(int deg) { return isin(90 - deg); }

/* Signed sine over a full turn, -1024..1024. The quadrant fold was written
 * inline three times in terrain_make() and once in the shield ring, which is
 * three chances to get a sign wrong in code whose whole job is to be identical
 * on two devices. */
static int isin360(int deg)
{
    deg %= 360; if (deg < 0) deg += 360;
    if (deg <= 90)  return  isin(deg);
    if (deg <= 180) return  isin(180 - deg);
    if (deg <= 270) return -isin(deg - 180);
    return                 -isin(360 - deg);
}
static int icos360(int deg) { return isin360(deg + 90); }

/* ---- wind ---------------------------------------------------------------- *
 *
 * The builder: "is it posable to update the game so that wind ... can affect the game".
 *
 * 🚨 DERIVED FROM THE SEED AND THE SHOT NUMBER, NEVER FROM esp_random(). This
 * is the rule that has kept the cluster weapon unbuilt: anything random that
 * feeds the simulation must be computable identically at both ends, or one
 * child sees a kill and the other a miss with nothing in the protocol able to
 * tell. Wind costs NOTHING on the wire — both handsets already share the seed
 * and the shot count, so both already know every wind there will ever be.
 *
 * It changes every shot, which is what makes it interesting: you cannot simply
 * find the range once and repeat it. */
#define WIND_MAX 6              /* 1/256 px per step^2; ~40px of drift at full */

static int wind_for(uint32_t sd, int shot)
{
    /* An integer avalanche hash. The LCG in nr() is reserved for the terrain
     * and must not be disturbed, and a table would be one more thing for the
     * two builds to disagree about. */
    uint32_t h = sd ^ (0x9E3779B9u * (uint32_t)(shot + 1));
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    h ^= h >> 16;
    return (int)(h % (uint32_t)(WIND_MAX * 2 + 1)) - WIND_MAX;
}

/* 🔑 ONE COPY of the integration, shared by the animation, the silent replay
 * and the machine's aiming probe. Three hand-written copies of an accumulator
 * is three chances for the two handsets to disagree about where a shell went.
 *
 * ⚠️ `/ 16`, NOT `>> 4`. A right shift of a negative value is arithmetic on
 * both our toolchains but the C standard does not say so, and division is
 * defined to truncate toward zero everywhere. One handset builds on GCC 15 and another
 * on GCC 13; this is not a place to rely on them agreeing by luck. */
static inline void wind_step(int w, int32_t *vx, int32_t *acc)
{
    *acc += w;
    int32_t d = *acc / 16;
    *vx  += d;
    *acc -= d * 16;
}

/* ---- game state ---------------------------------------------------------- */
#define N_TANKS 2
#define HP_MAX     100
#define DIRECT_DMG 100      /* 🔑 a direct hit on a bare tank is exactly fatal */
#define BLAST_R    18       /* px, standard shell */
#define CRATER_R   14

/* The plan's end rule. Two children who both keep missing is the checkers
 * failure — a game that passes every rule test and never finishes. After this
 * many shots the ground itself starts taking them apart, so the game always
 * ends. See "a game needs a rule that ends it". */
/* 🚨 THE FIRST VERSION OF THIS RULE DID NOT END ANYTHING, and it is the second
 * reason the builder and Kid 4 never finished a game.
 *
 * It was a flat 6 damage a shot from shot 20, against a history that is hard
 * capped at MAX_SHOTS = 24 by the 64-byte board. That is **30 damage total**
 * against a 100 hp Scout — let alone a 220 hp Super behind a shield. Two
 * children who kept missing reached shot 24 with both tanks healthy, do_fire()
 * returned early on `shots >= MAX_SHOTS`, and the board simply froze: FIRE
 * dead, no winner, nothing on screen to say why. The plan asserted "MAX_SHOTS
 * > SUDDEN_DEATH_SHOT, so a game always fits" and never did the arithmetic.
 *
 * This is "a game needs a rule that ends it" exactly — checkers passed
 * all 18 of its rule tests and still never finished.
 *
 * So the ground now takes them apart FASTER each shot, and end_check() carries
 * an unconditional backstop at MAX_SHOTS. Two Scouts: 8+16+24+32 = 80 by shot
 * 19, dead on shot 20. Nothing can outlast it, and if anything somehow does,
 * the last shot decides on points. */
/* 🚨 MEASURED 2026-09-20 14:08: in a 20-shot game the ground dealt 120 of the
 * ~230 points that killed P2, against 100 from the builder's only hit. The rule meant
 * to break a stalemate had become the main source of damage, and the game was
 * being decided by the clock rather than by shooting. The builder: "it said i won and
 * there was no hit" — his last shell hit nothing and the game announced a win.
 * Starting at 19 with a steeper ramp keeps it to the last six shots: 12, 24,
 * 36, 48, 60, 72 — 252 cumulative by MAX_SHOTS, which still finishes anything,
 * with the on-points backstop for a tank richer than that. */
#define SUDDEN_DEATH_SHOT 19
#define SUDDEN_DEATH_DMG  12

/* ---- weapons -------------------------------------------------------------
 *
 * 🔑 NONE OF THESE HAS ANY RANDOMNESS, which is why they are the ones built
 * first. A shell, a heavy shell and a digger are all "apply a fixed shape to
 * the terrain at the impact point" — replayable from a shot list at both ends
 * with no shared-seed machinery at all. The cluster weapon the builder remembers, which
 * bursts at the top of the arc and rains sub-munitions, is the ONLY one that
 * needs its spread derived from the shared seed, and it is deliberately last.
 * See TANKS-PLAN.md 6c. */
typedef enum { WEP_SHELL = 0, WEP_HEAVY, WEP_DIGGER, WEP_N } wep_t;

typedef struct {
    const char *name;
    int   blast_r;      /* damage radius */
    int   crater_r;     /* terrain removed */
    int   dig;          /* extra depth straight down - the undermining weapon */
    int   dmg;          /* at the centre */
    int   price;        /* per shot */
    uint32_t rgb;       /* ⚠️ raw hex, NOT lv_color_t: lv_color_hex() is a
                         * function call and cannot initialise a static const
                         * table. Converted at the point of use. */
} wepdef_t;

static const wepdef_t WEP[WEP_N] = {
    /* The plain shell is FREE AND UNLIMITED. A child who spends all their cash
     * must still be able to play; running out of ammo entirely is a way to lose
     * without being beaten, which is not a game. */
    { "Shell",  18, 14,  0, 100,   0, 0xF0A830 },
    { "Heavy",  28, 24,  0, 140,  60, 0xD9534F },
    { "Digger", 10,  8, 46,  60,  45, 0x6E9BD1 },
};

/* ---- the shop ------------------------------------------------------------ */
#define TANK_TIERS 3
static const struct { const char *name; int hp; int price; } TANK[TANK_TIERS] = {
    { "Scout",  100,   0 },
    { "Heavy",  150, 120 },
    { "Super",  220, 300 },
};
#define SHIELD_PRICE 80     /* per 50 points of shield */
#define SHIELD_STEP  50
#define PARA_PRICE   70

typedef struct {
    int cash;
    int tier;               /* index into TANK */
    int shield;             /* carried into the round */
    int parachutes;
    int ammo[WEP_N];        /* WEP_SHELL is unlimited and not counted */
} loadout_t;

typedef struct {
    int  x;             /* px, column */
    int  y;             /* px, top of the tank body; derived from terrain */
    int  hp;
    int  shield;        /* absorbs before hp. 0 in step 1; the model is here so
                         * the shop does not mean redoing every damage path. */
    int  angle;         /* 0..90 from horizontal */
    int  power;         /* 10..100 */
    bool parachute;     /* consumed by a fall, not permanent */
    wep_t wep;          /* selected weapon */
    int  dealt;         /* damage dealt this round - what earnings are paid on */
    lv_color_t col;
} tank_t;

static lv_obj_t  *scr, *canvas, *lbl_turn, *lbl_stats, *lbl_hint;
static lv_obj_t  *lbl_ang, *lbl_pow;   /* the live numbers, beside their buttons */
static lv_color_t *cbuf;
static lv_timer_t *tmr;

static uint8_t  terr[PLAY_W];        /* height above the canvas floor, 0..PLAY_H-1 */
static tank_t   tk[N_TANKS];
static int      turn;                /* whose turn: 0 or 1 */
static int      shots;               /* 🔑 only ever goes up — the turn derivation */
static bool     flying, game_over;
static int32_t  px_fp, py_fp, vx_fp, vy_fp;
static int32_t  wind_acc;        /* sub-step remainder of the wind integration */
static int      cur_wind;        /* the wind the shell in the air is flying in */
static uint32_t seed;
static loadout_t lo[N_TANKS];
/* 🚨 ROUND-LOCAL, AND THAT IS THE WHOLE POINT. settle_all() used to decrement
 * lo[i].parachutes directly, but rebuild_to() replays the entire history from
 * shot 0 every time a board arrives — so a tank saved by a parachute at shot 3
 * was NOT saved on the next rebuild, and the two handsets ended up disagreeing
 * about who was alive. A replay has to be idempotent or the full-history design
 * is worth nothing. The spend is committed once, at the end of the round. */
static int      para_left[N_TANKS];
/* 🔑 WHICH ROUND, NOT JUST WHICH BOARD. espnow.h spells out why every other
 * game carries this: "a new game has FEWER marks than the one it replaces, and
 * without a generation the two rules fight — a fresh empty board looks stale
 * next to the finished one". Tanks hardcoded `gen = 1`, so a NEW game whose
 * seed happened to be lower than the finished one's was rejected outright and
 * the other handset could never be dragged into the next round. */
static uint8_t  tnk_gen;
/* Paid out once per round, for the same reason: end_check() runs inside every
 * replay. Same trick as ck_won_gen in games.c. */
static uint32_t paid_seed;
static bool     paid;
/* Did THIS shot's ground damage make the kill? Read by end_check(), which runs
 * immediately after sudden_death() on every path. */
static bool     sd_kill;
static lv_obj_t *scr_shop;
static int       shop_who;          /* which player is buying */
static lv_obj_t *lbl_wep;
static bool      started;           /* has the first shop been through? */

/* ---- the shot history IS the game --------------------------------------- *
 *
 * 🔑 EVERY BOARD SENT CARRIES THE WHOLE GAME, not the latest move. A shot packs
 * into two bytes, so seed + both loadouts + twenty shots fits inside the
 * 64-byte espnow_game_t with room to spare.
 *
 * That is what makes a lossy link survivable. games.h is explicit that a board
 * is never retried — "a lost board is not retried, by design" — and with only
 * the last move on the wire, one dropped frame leaves the two handsets
 * permanently disagreeing with nothing able to notice. With the full history
 * the next board repairs everything: the receiver rebuilds from the seed and
 * lands in the right state no matter what it missed.
 *
 * It is also the same rule the plan gives for craters: reconstructible, never
 * accumulated. */
#define MAX_SHOTS 24                 /* > SUDDEN_DEATH_SHOT, so a game always fits */

typedef struct { uint8_t angle, power, wep; } shot_t;
static shot_t sh[MAX_SHOTS];
/* Who takes shot 0. Carried on the wire rather than worked out separately at
 * each end — the same reasoning as espnow_game_t.starter: if the two sides ever
 * disagreed about who started they would disagree about whose turn it is for
 * the whole game. */
static int   starter;



/* Defined down with the controls, used up in refresh_labels(). */
static void wep_label(void);
static void tanks_net_send(void);
static void tnk_pick_peer(void);
static bool my_turn(void);
static void do_fire(void);
static void banner_clear(void);

/* ---- who we are playing, and as which tank ------------------------------ *
 *
 * 🚨 THE MODE IS CHOSEN, NEVER GUESSED. The first cut called tnk_pick_peer()
 * from new_game() and switched itself into net play whenever ANYTHING playable
 * was in range — so a handset left on a shelf became your opponent and then
 * never took its turn. That is the identical fault the gateway comment in
 * games.c (line ~232) and the dispatcher comment (~630) were both written for,
 * re-introduced here a week later. The builder: "it needs to let you pick a aponit like
 * it does with the other games or the walkie talkie".
 *
 * So tanks now carries the same five modes as checkers and Connect 4, with the
 * same names, and the same espnow_play_idx() picker. */
#define TNK_2P   0
#define TNK_EASY 1
#define TNK_MED  2
#define TNK_HARD 3
#define TNK_VS   4

static int  tnk_mode = TNK_2P;
static int  tnk_peer = -1;
/* 🔑 CHOSEN BY A FINGER, or merely defaulted to. The distinction matters: an
 * explicit choice must outrank a stray frame, but a DEFAULT must not, or two
 * children who each defaulted to a different third handset would sit ignoring
 * each other's boards with the link working perfectly. So: picked by hand ⇒
 * only that handset; not picked ⇒ the first board to arrive settles it, which
 * is what the other games do and why they work. */
static bool tnk_peer_chosen;
static bool tnk_net;            /* mode is VS *and* a playable peer is picked */
static int  tnk_me;             /* 0 or 1 - which tank this handset drives */

/* The machine drives P1 in the three difficulty modes. */
static bool tnk_ai(void)    { return tnk_mode >= TNK_EASY && tnk_mode <= TNK_HARD; }
/* Which tank this handset's buttons and shop belong to. Hot seat drives both,
 * so it answers 0 and my_turn() lets everything through. */
static int  tnk_local(void) { return (tnk_mode == TNK_VS) ? tnk_me : 0; }

/* ---- deterministic RNG --------------------------------------------------- */
/* 🚨 NOT esp_random(). Terrain must be reproducible from the seed alone, or the
 * two handsets will not share a battlefield. esp_random() is used ONCE, to pick
 * the seed, and never again inside the simulation. */
static uint32_t rng;
static uint32_t nr(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

/* ---- terrain ------------------------------------------------------------- */
static void terrain_make(uint32_t s)
{
    rng = s ? s : 1;
    int base = PLAY_H / 3;
    int a1 = 14 + (int)(nr() % 16), p1 = (int)(nr() % 360);
    int a2 =  8 + (int)(nr() % 10), p2 = (int)(nr() % 360);
    int a3 =  4 + (int)(nr() %  6), p3 = (int)(nr() % 360);
    for (int x = 0; x < PLAY_W; x++) {
        /* Three summed sines: smooth, always solvable, and no valley so deep a
         * tank cannot see out of it. isin() takes 0..90, so the argument is
         * folded rather than wrapped. */
        int s1 = isin360((x * 3)  / 2 + p1);
        int s2 = isin360((x * 7)  / 2 + p2);
        int s3 = isin360((x * 11) / 2 + p3);
        int h = base + (a1 * s1 + a2 * s2 + a3 * s3) / 1024;
        if (h < 8) h = 8;
        if (h > PLAY_H - 40) h = PLAY_H - 40;
        terr[x] = (uint8_t)h;
    }
}

/* Canvas y for a terrain height. The canvas is drawn with y=0 at the top, the
 * heightfield measured from the bottom — this is the one place they meet. */
static int terr_y(int x) { return PLAY_H - 1 - terr[x]; }

static void tank_settle(tank_t *t)
{
    int x = t->x < 2 ? 2 : (t->x > PLAY_W - 3 ? PLAY_W - 3 : t->x);
    t->y = terr_y(x) - 6;
}

/* ---- drawing ------------------------------------------------------------- */
/* 🔑 PIXELS ARE WRITTEN STRAIGHT INTO THE CANVAS BUFFER, not drawn with
 * lv_canvas_draw_*. A heightfield is 240 vertical runs per frame and the map
 * screen has already shown what thousands of LVGL draw calls cost (121 ms
 * median). A direct fill is a memory write.
 *
 * ⚠️ lv_color_make() is used rather than hand-packed RGB565 — this build has
 * LV_COLOR_16_SWAP=y and hand-packed pixels come out green and magenta with
 * perfect shapes. See "lvgl rgb565 byte swap". */
static inline void px(int x, int y, lv_color_t c)
{
    if (x < 0 || y < 0 || x >= PLAY_W || y >= PLAY_H) return;
    cbuf[y * PLAY_W + x] = c;
}
static void fill_rect(int x0, int y0, int w, int h, lv_color_t c)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++) px(x, y, c);
}
static void disc(int cx, int cy, int r, lv_color_t c)
{
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) px(cx + x, cy + y, c);
}

static void render(void)
{
    const lv_color_t sky  = lv_color_make(0x0D, 0x12, 0x1A);
    const lv_color_t dirt = lv_color_make(0x5A, 0x43, 0x2A);
    const lv_color_t top  = lv_color_make(0x6E, 0x8B, 0x3D);

    /* Sky, then each terrain column in one run. */
    for (int y = 0; y < PLAY_H; y++)
        for (int x = 0; x < PLAY_W; x++) cbuf[y * PLAY_W + x] = sky;
    for (int x = 0; x < PLAY_W; x++) {
        int ty = terr_y(x);
        for (int y = ty; y < PLAY_H; y++) cbuf[y * PLAY_W + x] = dirt;
        px(x, ty, top);
        if (ty + 1 < PLAY_H) px(x, ty + 1, top);
    }

    for (int i = 0; i < N_TANKS; i++) {
        if (tk[i].hp <= 0) continue;
        fill_rect(tk[i].x - 5, tk[i].y + 2, 11, 5, tk[i].col);
        fill_rect(tk[i].x - 3, tk[i].y - 1, 7, 3, tk[i].col);
        /* Barrel, showing the angle it will actually fire at. Left tank points
         * right, right tank points left. */
        int dir = (i == 0) ? 1 : -1;
        for (int s = 2; s <= 10; s++) {
            int bx = tk[i].x + dir * (s * icos(tk[i].angle)) / 1024;
            int by = tk[i].y - (s * isin(tk[i].angle)) / 1024;
            px(bx, by, C_INK);
        }
        /* Shield, when there is one: a ring, so "the shield is up" is visible
         * without reading a number. */
        if (tk[i].shield > 0)
            for (int a = 0; a < 360; a += 12)
                px(tk[i].x     + (14 * isin360(a)) / 1024,
                   tk[i].y + 2 + (14 * icos360(a)) / 1024, C_BLUE);
    }

    /* 🔑 THE WIND, DRAWN NOT WRITTEN. An arrow whose length is its strength is
     * readable by a six-year-old at arm's length and needs no words; a number
     * in a corner would not be. Shown for the shot ABOUT to be taken, or for
     * the one in the air while it flies. */
    {
        int w  = flying ? cur_wind : wind_for(seed, shots);
        int cx = PLAY_W / 2, cy = 8;
        lv_color_t wc = lv_color_make(0x6E, 0x9B, 0xD1);
        for (int i = -2; i <= 2; i++) px(cx, cy + i, lv_color_make(0x3A, 0x44, 0x55));
        if (w != 0) {
            int dir = w > 0 ? 1 : -1, len = (w > 0 ? w : -w) * 5;
            for (int i = 0; i <= len; i++) px(cx + dir * i, cy, wc);
            for (int i = 1; i <= 4; i++) {        /* arrowhead */
                px(cx + dir * (len - i), cy - i, wc);
                px(cx + dir * (len - i), cy + i, wc);
            }
        }
    }

    if (flying) disc(TO_PX(px_fp), TO_PX(py_fp), 2, C_AMBER);

    lv_obj_invalidate(canvas);
}

/* ---- damage, craters, falling -------------------------------------------- */
static void hurt(tank_t *t, int dmg)
{
    /* 🔑 THE SHIELD IS A POOL IN FRONT OF THE HEALTH, not a reduction. That is
     * what lets a direct hit be fatal to a bare tank and survivable behind a
     * shield — the builder's spec — and it keeps the arithmetic integer and identical
     * at both ends. */
    if (t->shield > 0) {
        int a = dmg < t->shield ? dmg : t->shield;
        t->shield -= a; dmg -= a;
    }
    if (dmg > 0) t->hp -= dmg;
    if (t->hp < 0) t->hp = 0;
}

/* 🚨 THE DIGGER IS WHY THE PARACHUTE EXISTS. `dig` removes a narrow shaft
 * straight down rather than a wide bowl — it undermines rather than flattens,
 * the tank above it loses its footing, and the fall is what actually hurts.
 * The builder asked for the digger and the parachute separately; they are the same
 * mechanic seen from the two ends. */
static void crater(int cx, int cy, int r, int dig)
{
    /* Terrain is a heightfield, so a crater lowers columns rather than carving
     * a true circle. Good enough, and — more to the point — cheap to replay
     * from a shot list at both ends, which a mutated bitmap would not be. */
    for (int x = cx - r; x <= cx + r; x++) {
        if (x < 0 || x >= PLAY_W) continue;
        int dx = x - cx;
        int d = r * r - dx * dx;
        if (d <= 0) continue;
        /* integer sqrt */
        int s = 0; while ((s + 1) * (s + 1) <= d) s++;
        int ytop = cy - s, ybot = cy + s;
        int surf = terr_y(x);
        if (ybot < surf) continue;                 /* crater hangs in the air */
        int newsurf = (ytop > surf) ? ytop : surf; /* dig from the surface down */
        int removed = ybot - newsurf;
        if (removed <= 0) continue;
        int h = terr[x] - removed;
        terr[x] = (uint8_t)(h < 0 ? 0 : h);
    }
    /* The shaft. Narrow and deep, centred on the impact. */
    if (dig > 0) {
        for (int x = cx - 3; x <= cx + 3; x++) {
            if (x < 0 || x >= PLAY_W) continue;
            int h = (int)terr[x] - dig;
            terr[x] = (uint8_t)(h < 0 ? 0 : h);
        }
    }
}

/* 🚨 THE FALL IS WHY THE PARACHUTE EXISTS. A digger removes the ground beneath
 * a tank; the tank drops; the drop hurts. All three or none of them mean
 * anything. See TANKS-PLAN.md 6b. */
static void settle_all(void)
{
    for (int i = 0; i < N_TANKS; i++) {
        if (tk[i].hp <= 0) continue;
        int was = tk[i].y;
        tank_settle(&tk[i]);
        int drop = tk[i].y - was;
        if (drop > 8) {
            if (tk[i].parachute) {
                /* Consumed, not permanent: a parachute that saved you forever
                 * would make the digger pointless after one purchase. Spent out
                 * of the ROUND-LOCAL count so a replay lands in the same place
                 * every time; lo[] is debited once, in end_check(). */
                if (para_left[i] > 0) para_left[i]--;
                tk[i].parachute = para_left[i] > 0;
                applog("[tanks] P%d floated down %dpx (parachute used)", i + 1, drop);
            } else {
                int dmg = drop / 3;
                hurt(&tk[i], dmg);
                applog("[tanks] P%d fell %dpx, %d damage", i + 1, drop, dmg);
            }
        }
    }
}

/* ---- the result banner --------------------------------------------------- *
 *
 * 🚨 THE RESULT USED TO BE TWO LABELS THAT OVERLAPPED. lbl_turn sits top-left
 * and lbl_hint top-right on the SAME line of a 240px screen, and "YOU angle 45
 * power 55" next to "Game over - NEW for another" is 290px of text in 240px of
 * room. The builder: "it shows i won i think the text is mixed with other text and hard
 * to read". Worse, end_check() wrote the winner into lbl_turn and then
 * refresh_labels() immediately overwrote it with the turn line.
 *
 * games.c already solved this for every other game — "one result banner for
 * every game... a child holding the handset at arm's length is looking at the
 * board, and who won has to be readable from there without hunting". Same
 * shape here, and deliberately NOT clickable: NEW starts the next round, and a
 * banner that swallowed that tap would read as a frozen device. */
static lv_obj_t *banner;

static void banner_clear(void)
{
    if (banner) { lv_obj_del(banner); banner = NULL; }
}

static void banner_show(const char *txt, lv_color_t c)
{
    if (!scr) return;
    banner_clear();
    banner = lv_obj_create(scr);
    lv_obj_remove_style_all(banner);
    lv_obj_set_size(banner, 204, 54);
    lv_obj_set_style_bg_color(banner, C_PANEL, 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(banner, c, 0);
    lv_obj_set_style_border_width(banner, 2, 0);
    lv_obj_set_style_radius(banner, 6, 0);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(banner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(banner, LV_ALIGN_TOP_MID, 0, PLAY_Y + PLAY_H / 2 - 27);
    lv_obj_t *l = lv_label_create(banner);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, C_INK, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, 188);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_move_foreground(banner);
}

/* ---- UI text ------------------------------------------------------------- */
static void refresh_labels(void)
{
    if (lbl_turn) {
        /* 🔑 SAY WHOSE TURN IT IS BY NAME. Every one of the faults this file
         * was carrying presented to the builder as "it stopped working", with nothing
         * on the screen to say whether it was his turn, Kid 4's, or nobody's. */
        if (game_over) {
            lv_label_set_text(lbl_turn, "");     /* the banner says it */
        } else if (!my_turn()) {
            const char *they = tnk_ai() ? "The machine"
                             : (tnk_peer >= 0 ? espnow_peer_name(tnk_peer) : NULL);
            lv_label_set_text_fmt(lbl_turn, "%s is aiming...", they ? they : "They");
        } else {
            /* ⚠️ THE NUMBERS ARE NOT IN HERE ANY MORE. This label is 146px wide
             * and "YOU   angle 45   power 55" wrapped onto a second line that
             * landed on top of the angle buttons — the builder: "you moved some of the
             * text so its under the angle buttons so i cant see power leavel".
             * angle and power now sit directly beside the buttons that change
             * them, which is where they should always have been. */
            lv_label_set_text(lbl_turn,
                              (tnk_mode == TNK_2P) ? (turn == 0 ? "P1's turn" : "P2's turn")
                                                   : "YOUR TURN");
        }
    }
    if (lbl_stats)
        lv_label_set_text_fmt(lbl_stats, "P1 %d%s    shots %d    P2 %d%s",
                              tk[0].hp, tk[0].shield ? "+" : "",
                              shots,
                              tk[1].hp, tk[1].shield ? "+" : "");
    if (lbl_ang) lv_label_set_text_fmt(lbl_ang, "angle %d", tk[turn].angle);
    if (lbl_pow) lv_label_set_text_fmt(lbl_pow, "power %d", tk[turn].power);
    wep_label();
    if (lbl_hint) {
        /* ⚠️ KEEP THIS SHORT. It shares a 240px line with lbl_turn, and the old
         * "Game over - NEW for another" was 150px of it. */
        if (game_over) lv_label_set_text(lbl_hint, "");
        /* 🔑 THE ENDGAME MUST BE VISIBLE BEFORE IT LANDS. "SUDDEN DEATH" alone
         * said nothing about how hard or how soon, so the finish arrived as a
         * surprise — the builder: "i was still playing it". Now it counts down, then
         * names the damage each shot costs both tanks. */
        else if (shots >= SUDDEN_DEATH_SHOT)
            lv_label_set_text_fmt(lbl_hint, "SUDDEN -%d",
                                  (shots - SUDDEN_DEATH_SHOT + 1) * SUDDEN_DEATH_DMG);
        else if (shots >= SUDDEN_DEATH_SHOT - 5)
            lv_label_set_text_fmt(lbl_hint, "death in %d", SUDDEN_DEATH_SHOT - shots);
        else if (tnk_mode == TNK_VS && tnk_peer < 0) lv_label_set_text(lbl_hint, espnow_nobody_text());
        else lv_label_set_text(lbl_hint, "");
    }
}

static void end_check(void)
{
    if (game_over) return;

    bool dead = (tk[0].hp <= 0 || tk[1].hp <= 0);
    /* 🚨 THE BACKSTOP. The shot history is hard capped at MAX_SHOTS by the
     * 64-byte board, so that cap is an end of the game whether anybody died or
     * not — otherwise do_fire() just stops working and the board freezes with
     * no winner and no explanation, which is what it used to do. On points:
     * health plus shield, the same two numbers at both ends. */
    bool out = (shots >= MAX_SHOTS);

    if (dead || out) {
        game_over = true;
        int pool0 = tk[0].hp + tk[0].shield, pool1 = tk[1].hp + tk[1].shield;
        /* -1 is a draw. Computed once and used for the banner AND the win
         * bonus, so the two can never disagree about who won. */
        int win = dead ? ((tk[0].hp <= 0 && tk[1].hp <= 0) ? -1 : (tk[1].hp <= 0 ? 0 : 1))
                       : ((pool0 > pool1) ? 0 : (pool1 > pool0) ? 1 : -1);
        const char *who = (win < 0) ? "Draw" : (win == 0) ? "P1 wins" : "P2 wins";
        /* 🔑 PAID ON DAMAGE DEALT, PLUS A WIN BONUS. Paying only the winner
         * means the child who loses every round can never afford anything and
         * keeps losing - the shop would widen the gap it exists to close. */
        /* 🚨 ONCE PER ROUND, NOT ONCE PER REPLAY. end_check() is reached from
         * fly_silent(), and rebuild_to() calls that for every shot in the
         * history every time a board arrives — so the payout used to land again
         * on each frame that turned up after the last shot, and the parachutes
         * were spent again with it. Keyed on the seed, which is what makes a
         * round a round. */
        if (!(paid && paid_seed == seed)) {
            paid = true; paid_seed = seed;
            for (int i = 0; i < N_TANKS; i++) {
                int earn = tk[i].dealt * 2 + 40;
                if (win == i) earn += 150;
                lo[i].cash += earn;
                lo[i].parachutes = para_left[i];   /* the round's spend, committed */
                applog("[tanks] P%d earned %d (dealt %d), cash %d",
                       i + 1, earn, tk[i].dealt, lo[i].cash);
            }
        }
        applog("[tanks] %s after %d shots%s", who, shots,
               out && !dead ? " (out of shots, decided on points)" : "");

        /* One unmissable banner rather than two labels racing for the same
         * line. In hot seat it names the player; anywhere else it is you or
         * them, because "P2 wins" means nothing to a child holding P1. */
        /* 🔑 SAY WHY IT ENDED. A win with no explanation reads as a bug — the builder
         * fired a shell that hit nothing and the screen said he had won, which
         * was true (the ground had finished the other tank) and completely
         * unreadable. */
        char t[72];
        const char *tail = (out && !dead) ? "\nout of shots - on points"
                         : sd_kill        ? "\nthe ground got them"
                                          : "\nNEW for the shop";
        lv_color_t col = C_AMBER;
        if (win < 0) {
            snprintf(t, sizeof t, "A DRAW%s", tail);
        } else if (tnk_mode == TNK_2P) {
            snprintf(t, sizeof t, "P%d WINS%s", win + 1, tail);
            col = C_BLUE;
        } else if (win == tnk_local()) {
            snprintf(t, sizeof t, "YOU WIN%s", tail);
            col = C_GREEN;
        } else {
            const char *they = tnk_ai() ? "THE MACHINE"
                             : (tnk_peer >= 0 ? espnow_peer_name(tnk_peer) : NULL);
            snprintf(t, sizeof t, "%s WINS%s", they ? they : "THEY", tail);
            col = C_RED;
        }
        banner_show(t, col);
    }
}

/* The ground closing in. 🔑 ONE COPY, called from all three places a shot can
 * conclude — the animated impact, the animated fly-off and the silent replay.
 * Three hand-written copies of an escalating formula is three chances for the
 * two handsets to disagree about who is alive. */
static void sudden_death(void)
{
    sd_kill = false;
    if (shots < SUDDEN_DEATH_SHOT) return;
    int dmg = (shots - SUDDEN_DEATH_SHOT + 1) * SUDDEN_DEATH_DMG;

    int pool[N_TANKS];
    for (int i = 0; i < N_TANKS; i++) pool[i] = tk[i].hp + tk[i].shield;

    for (int i = 0; i < N_TANKS; i++)
        if (tk[i].hp > 0) { hurt(&tk[i], dmg); if (tk[i].hp <= 0) sd_kill = true; }

    /* 🚨 A RULE THAT ENDS THE GAME MUST PRODUCE A WINNER.
     *
     * MEASURED 2026-09-20 13:51, both handsets: "Draw after 19 shots", P1 had
     * dealt 112 and P2 125. The builder: "how did i end up with a drew thats not right
     * i was still playing it". Sudden death hits BOTH tanks with the same
     * escalating chunk, so any two tanks within one step of each other — 32
     * points by shot 19, which is most close games — cross zero on the same
     * shot and nobody wins. The first version of this rule never killed
     * anybody; the fix for that turned a game with no end into a game with no
     * winner, which is the same bug wearing a hat.
     * See "a game needs a rule that ends it".
     *
     * So the ground taking them both apart cannot be what decides it: whoever
     * was healthier going into the shot lives on 1hp. Both ends compute this
     * from the same two numbers, so it cannot desync. An EXACT tie is still a
     * draw, and that one is honest. */
    if (pool[0] > 0 && pool[1] > 0 &&
        tk[0].hp <= 0 && tk[1].hp <= 0 && pool[0] != pool[1]) {
        int keep = (pool[0] > pool[1]) ? 0 : 1;
        tk[keep].hp = 1;
        applog("[tanks] sudden death took both; P%d was ahead %d-%d and lives",
               keep + 1, pool[keep], pool[1 - keep]);
    }
}

/* ---- the flight ---------------------------------------------------------- */
/* Applies one shot's effects. Called BOTH from the animated flight and from a
 * silent replay, so there is exactly one damage path and a replayed game cannot
 * drift from a played one. `who` fires it, `wi` is the weapon — taken as
 * arguments rather than read from `turn`, because a replay walks the history
 * and `turn` is derived, not remembered. */
/* `struck` is the tank the shell actually ran into, or -1 if it hit the ground.
 *
 * 🚨 IT USED TO BE DERIVED FROM DISTANCE: anything landing within 6px of the
 * tank's centre counted as a direct hit and took the full 100 — so a shell
 * burying itself in the dirt BESIDE a tank killed it outright, which on a
 * 240px screen looks exactly like a miss. The builder reported the symptom as "there
 * was no hit". The flight loops already know the difference, because they stop
 * on the tank's collision box; there is no reason to re-derive it worse.
 * A near miss now does blast damage by distance, which is the honest model and
 * still leaves the builder's spec intact: a real direct hit is exactly fatal to a bare
 * tank, and survivable behind a shield. */
static void impact_at(int ix, int iy, int who, wep_t wi, int struck)
{
    flying = false;
    const wepdef_t *w = &WEP[wi];
    crater(ix, iy, w->crater_r, w->dig);

    for (int i = 0; i < N_TANKS; i++) {
        if (tk[i].hp <= 0) continue;
        int before = tk[i].hp + tk[i].shield;
        int dx = tk[i].x - ix, dy = (tk[i].y + 3) - iy;
        int d2 = dx * dx + dy * dy;
        if (i == struck) {                   /* the shell ran into this tank */
            hurt(&tk[i], w->dmg);
            applog("[tanks] DIRECT HIT on P%d with %s", i + 1, w->name);
        } else if (d2 <= w->blast_r * w->blast_r) {
            int d = 0; while ((d + 1) * (d + 1) <= d2) d++;
            int dmg = w->dmg * (w->blast_r - d) / (w->blast_r * 2);
            if (dmg > 0) hurt(&tk[i], dmg);
        }
        /* Earnings are paid on damage DEALT, so a near miss that chips a shield
         * is still worth something - a child who keeps nearly hitting is not
         * punished with an empty wallet as well as a loss. */
        if (i != who) tk[who].dealt += before - (tk[i].hp + tk[i].shield);
    }

    settle_all();

    /* The end rule. Past the threshold every shot costs both tanks, so two
     * players who cannot hit each other still finish.
     * ⚠️ `shots` is NOT incremented here any more — the history owns the count,
     * and it was already appended when the shot was fired. Incrementing in both
     * places is how a replayed game would end up one turn out of step. */
    sudden_death();

    end_check();
    /* 🔑 Turn derives from a count that only goes up, never from a flag that
     * both ends have to agree to flip. See
     * "derivation needs a changing signal". */
    if (!game_over) turn = (starter + shots) & 1;
}

static void impact(int ix, int iy, int struck)
{
    impact_at(ix, iy, turn, tk[turn].wep, struck);
    refresh_labels();
    render();
}

/* Fly one shot to its conclusion with no animation and no drawing. Identical
 * arithmetic to tick(), which is the whole point: a replayed shot must land
 * exactly where the animated one did, on the other handset, every time. */
static void fly_silent(int who, const shot_t *s, int shot_idx)
{
    int dir = (who == 0) ? 1 : -1;
    int32_t x = TO_FP(tk[who].x + dir * 10), y = TO_FP(tk[who].y - 2);
    int sp = s->power * 5 / 2;
    int32_t vx = dir * (sp * icos(s->angle)) / 1024;
    int32_t vy =      -(sp * isin(s->angle)) / 1024;
    int     w   = wind_for(seed, shot_idx);
    int32_t acc = 0;

    for (int guard = 0; guard < 20000; guard++) {
        wind_step(w, &vx, &acc);
        x += vx; y += vy; vy += GRAVITY_FP;
        int ix = TO_PX(x), iy = TO_PX(y);
        if (ix < -20 || ix > PLAY_W + 20 || iy > PLAY_H + 40) {
            /* Off the map: no crater, but the turn still passes and sudden
             * death still bites. Same as the animated path. */
            sudden_death();
            end_check();
            if (!game_over) turn = (starter + shots) & 1;
            return;
        }
        if (iy >= 0 && ix >= 0 && ix < PLAY_W && iy >= terr_y(ix)) {
            impact_at(ix, iy, who, (wep_t)s->wep, -1); return;
        }
        for (int i = 0; i < N_TANKS; i++) {
            if (tk[i].hp <= 0) continue;
            if (ix >= tk[i].x - 6 && ix <= tk[i].x + 6 &&
                iy >= tk[i].y - 2 && iy <= tk[i].y + 8) {
                impact_at(ix, iy, who, (wep_t)s->wep, i); return;
            }
        }
    }
}

/* Rebuild the entire game state from the seed and the first `n` shots. This is
 * what makes a dropped board harmless. */
static void rebuild_to(int n)
{
    terrain_make(seed);
    for (int i = 0; i < N_TANKS; i++) {
        tk[i].hp        = TANK[lo[i].tier].hp;
        tk[i].shield    = lo[i].shield;
        para_left[i]    = lo[i].parachutes;   /* reset, so the replay is idempotent */
        tk[i].parachute = para_left[i] > 0;
        tk[i].dealt     = 0;
        tank_settle(&tk[i]);
    }
    game_over = false;
    banner_clear();
    for (shots = 0; shots < n && !game_over; ) {
        int who = (starter + shots) & 1;
        shots++;                       /* count the shot BEFORE it lands, as
                                        * firing does — see impact_at() */
        fly_silent(who, &sh[shots - 1], shots - 1);
    }
    /* 🚨 `shots` IS EXACTLY WHAT WAS REPLAYED. It used to be
     *     shots = save < n ? n : save;
     * which KEPT THE OLD COUNT whenever the new history was shorter — and the
     * new history is shortest of all at the start of a fresh game.
     *
     * MEASURED 2026-09-20 14:00:19: a handset joined a brand-new game (n = 0)
     * one second after it was created, kept `shots = 2` from the round it had
     * just abandoned, and then transmitted those two stale shots back as if
     * they belonged to the new game. `sh[]` still held them because unpacking
     * zero shots overwrites nothing. The far end replayed them faithfully.
     * The builder: "now it said i won but i had not hit kid 3" — one of the phantom
     * shots was a direct hit, and the log duly paid him for 100 damage
     * (`P1 earned 390 (dealt 100)`) on a shell he never fired.
     *
     * A rebuild reconstructs the whole round from the seed and `n` shots. Any
     * count other than `n` is a claim about history that was not replayed.
     * The caller decides what `n` should be. */
    if (game_over && shots < n) shots = n;   /* the round ended early; the rest
                                              * of the history is still ours */
    if (!game_over) turn = (starter + shots) & 1;
}


/* ---- the computer opponent ---------------------------------------------- *
 *
 * The builder: "it should still let you play against the computer". Tanks was the only
 * game in the menu with no machine to play — every other one has Easy/Med/Hard
 * — and that made it the only game a child could not open on their own.
 *
 * 🔑 IT AIMS BY ASKING THE PHYSICS, not by a formula. The simulation is already
 * integer, seeded and side-effect free, so the machine can simply fly a few
 * hundred candidate shots and keep the best. That is worth more than a ballistic
 * solution: it is automatically right about the terrain in the way, about the
 * crater the last shot left, and about a tank that has fallen down a shaft.
 *
 * 🚨 IT MAY NEVER RUN IN VS. probe() is deterministic, but the aim error below
 * comes from esp_random(), and one call to that inside a networked game is the
 * silent desync the cluster weapon is still unbuilt to avoid. tnk_ai() is false
 * in TNK_VS and every entry point checks it.
 */

/* Where would this shot land? The identical arithmetic to fly_silent(), but it
 * touches NOTHING — no craters, no damage, no turn. Returns false if the shell
 * leaves the map. */
static bool probe(int who, int angle, int power, int *ox, int *oy)
{
    int dir = (who == 0) ? 1 : -1;
    int32_t x = TO_FP(tk[who].x + dir * 10), y = TO_FP(tk[who].y - 2);
    int sp = power * 5 / 2;
    int32_t vx = dir * (sp * icos(angle)) / 1024;
    int32_t vy =      -(sp * isin(angle)) / 1024;
    /* The machine aims into the wind it is ABOUT to fire in, so it adapts on
     * its own with no extra cleverness. */
    int     w   = wind_for(seed, shots);
    int32_t acc = 0;

    for (int guard = 0; guard < 4000; guard++) {
        wind_step(w, &vx, &acc);
        x += vx; y += vy; vy += GRAVITY_FP;
        int ix = TO_PX(x), iy = TO_PX(y);
        if (ix < -20 || ix > PLAY_W + 20 || iy > PLAY_H + 40) return false;
        if (iy >= 0 && ix >= 0 && ix < PLAY_W && iy >= terr_y(ix)) {
            *ox = ix; *oy = iy; return true;
        }
        for (int i = 0; i < N_TANKS; i++) {
            if (tk[i].hp <= 0) continue;
            if (ix >= tk[i].x - 6 && ix <= tk[i].x + 6 &&
                iy >= tk[i].y - 2 && iy <= tk[i].y + 8) { *ox = ix; *oy = iy; return true; }
        }
    }
    return false;
}

/* How far off the machine deliberately aims, in pixels. 🔑 THE NUMBER IS SET
 * FOR THE CHILD, NOT THE TESTER — "tune for the user not the tester".
 * Hard keeps a real error so a determined six-year-old can still beat it; a
 * machine that solves the exact shot every time is a machine nobody plays
 * twice. Easy misses by about a tank's width and a half. */
static int ai_slop(void)
{
    switch (tnk_mode) {
        case TNK_EASY: return 26;
        case TNK_MED:  return 11;
        default:       return 4;
    }
}

/* Coarse sweep then a refine, minimising horizontal distance to the aim point —
 * which is the metric an artillery player actually uses. ~390 flights, all
 * integer, a couple of milliseconds. */
static void ai_pick(int who, int *out_a, int *out_p)
{
    int slop  = ai_slop();
    int aim_x = tk[1 - who].x;
    if (slop > 0) aim_x += (int)(esp_random() % (uint32_t)(slop * 2 + 1)) - slop;
    if (aim_x < 2) aim_x = 2;
    if (aim_x > PLAY_W - 3) aim_x = PLAY_W - 3;

    int ba = 45, bp = 55, best = 1 << 24;
    for (int a = 20; a <= 80; a += 4)
        for (int pw = 20; pw <= 100; pw += 4) {
            int ix, iy;
            if (!probe(who, a, pw, &ix, &iy)) continue;
            int d = ix > aim_x ? ix - aim_x : aim_x - ix;
            if (d < best) { best = d; ba = a; bp = pw; }
        }
    for (int a = ba - 3; a <= ba + 3; a++) {
        if (a < 5 || a > 88) continue;
        for (int pw = bp - 3; pw <= bp + 3; pw++) {
            if (pw < 10 || pw > 100) continue;
            int ix, iy;
            if (!probe(who, a, pw, &ix, &iy)) continue;
            int d = ix > aim_x ? ix - aim_x : aim_x - ix;
            if (d < best) { best = d; ba = a; bp = pw; }
        }
    }
    *out_a = ba; *out_p = bp;
}

/* Ticks of thinking before it shoots. Instant is unreadable — the shell appears
 * from nowhere and a child cannot tell whose turn it was. */
#define AI_THINK_TICKS (700 / TICK_MS)
static int ai_wait;

static void ai_take_turn(void)
{
    int a = 45, pw = 55;
    ai_pick(1, &a, &pw);
    tk[1].angle = a; tk[1].power = pw;
    /* Spend a heavy shell when it owns one; the digger is saved for a tank
     * sitting on a ridge, where undermining is worth more than a blast. */
    tk[1].wep = WEP_SHELL;
    if (tnk_mode == TNK_HARD && lo[1].ammo[WEP_HEAVY] > 0) tk[1].wep = WEP_HEAVY;
    else if (tnk_mode != TNK_EASY && lo[1].ammo[WEP_DIGGER] > 0 && terr[tk[0].x] > PLAY_H / 3)
        tk[1].wep = WEP_DIGGER;
    refresh_labels();
    do_fire();
}

/* The machine's shopping. It buys against the same price list the child does
 * and out of the same purse, so a long session does not quietly leave it with a
 * Super tank and the child with a Scout. */
static void ai_shop(void)
{
    loadout_t *L = &lo[1];
    if (tnk_mode == TNK_EASY) return;              /* a bare Scout, always */
    if (tnk_mode == TNK_HARD && L->cash >= TANK[L->tier + 1 < TANK_TIERS ? L->tier + 1 : L->tier].price
        && L->tier + 1 < TANK_TIERS) {
        L->cash -= TANK[L->tier + 1].price; L->tier++;
    }
    if (L->cash >= SHIELD_PRICE && L->shield < 100) { L->cash -= SHIELD_PRICE; L->shield += SHIELD_STEP; }
    if (tnk_mode == TNK_HARD && L->cash >= WEP[WEP_HEAVY].price) {
        L->cash -= WEP[WEP_HEAVY].price; L->ammo[WEP_HEAVY]++;
    }
    if (L->cash >= PARA_PRICE && L->parachutes == 0) { L->cash -= PARA_PRICE; L->parachutes++; }
}

static void tick(lv_timer_t *t)
{
    (void)t;
    if (!flying) {
        /* The machine's turn is driven from here rather than from a timer of
         * its own: the turn changes in three different places and a separate
         * arming call would eventually be forgotten in one of them. */
        if (tnk_ai() && !game_over && turn == 1) {
            if (ai_wait <= 0) ai_wait = AI_THINK_TICKS;
            if (--ai_wait == 0) ai_take_turn();
        } else {
            ai_wait = 0;
        }
        return;
    }

    for (int step = 0; step < 2; step++) {      /* 2 substeps: no tunnelling */
        wind_step(cur_wind, &vx_fp, &wind_acc);
        px_fp += vx_fp;
        py_fp += vy_fp;
        vy_fp += GRAVITY_FP;

        int ix = TO_PX(px_fp), iy = TO_PX(py_fp);
        if (ix < -20 || ix > PLAY_W + 20 || iy > PLAY_H + 40) {
            /* 🚨 NO `shots++` HERE. fire_clicked() has already counted this
             * shot and written it into the history — impact_at() says it in as
             * many words: "`shots` is NOT incremented here any more, the
             * history owns the count". This branch never got that edit, so
             * every shell that flew off the side of the screen was counted
             * TWICE: `shots` ran ahead of `sh[]`, leaving a hole that the other
             * handset faithfully replayed as a phantom shot at angle 0 power 0,
             * out of the wrong tank. Overshooting off the screen is the most
             * common thing that happens in this game. It is why the builder and Kid 4
             * could never finish one.
             *
             * 🚨 AND THE TURN NEEDS `starter`. `shots & 1` is right only when
             * P0 started. Whenever Kid 4's handset opened the game, this line
             * handed the turn to the wrong tank on one screen only — and
             * fire_clicked()'s "not your tank" guard then made FIRE do nothing
             * at both ends. It reads exactly like a dropped connection.
             * Matches fly_silent(), impact_at() and rebuild_to(). */
            flying = false;
            sudden_death();
            end_check();
            if (!game_over) turn = (starter + shots) & 1;
            refresh_labels(); render(); return; }
        if (iy >= 0 && ix >= 0 && ix < PLAY_W && iy >= terr_y(ix)) { impact(ix, iy, -1); return; }
        for (int i = 0; i < N_TANKS; i++) {
            if (tk[i].hp <= 0) continue;
            if (ix >= tk[i].x - 6 && ix <= tk[i].x + 6 &&
                iy >= tk[i].y - 2 && iy <= tk[i].y + 8) { impact(ix, iy, i); return; }
        }
    }
    render();
}

/* ---- controls ------------------------------------------------------------ */
/* 🚨 IN NET PLAY YOU CAN ONLY FIRE ON YOUR OWN TURN, and against the machine
 * you can only drive your own tank. Without this both handsets can fire into
 * the same shot slot and the two histories diverge permanently — the exact
 * failure the whole full-history design exists to prevent, reintroduced through
 * the UI. Hot seat is unaffected: it drives both tanks from this screen. */
static bool my_turn(void)
{
    if (tnk_mode == TNK_2P) return true;
    return turn == tnk_local();
}

/* The firing path itself, with no guard and no event. The machine walks it too,
 * so there is exactly ONE place a shot enters the history — a second copy for
 * the AI is how the two ends would come to disagree about what was fired. */
static void do_fire(void)
{
    tank_t *t = &tk[turn];
    /* Ammo is spent at the moment of firing. WEP_SHELL is free and unlimited,
     * so running dry silently drops back to it rather than refusing to fire. */
    if (t->wep != WEP_SHELL) {
        if (lo[turn].ammo[t->wep] > 0) lo[turn].ammo[t->wep]--;
        else t->wep = WEP_SHELL;
    }
    /* Append to the history BEFORE flying it. The history is the authority: the
     * animation is just this same shot being watched. */
    if (shots >= MAX_SHOTS) return;
    sh[shots].angle = (uint8_t)t->angle;
    sh[shots].power = (uint8_t)t->power;
    sh[shots].wep   = (uint8_t)t->wep;
    shots++;
    tanks_net_send();

    int dir = (turn == 0) ? 1 : -1;
    px_fp = TO_FP(t->x + dir * 10);
    py_fp = TO_FP(t->y - 2);
    /* power 10..100 -> a usable muzzle speed in 1/16 px per tick */
    int sp = t->power * 5 / 2;
    vx_fp = dir * (sp * icos(t->angle)) / 1024;
    vy_fp =      -(sp * isin(t->angle)) / 1024;
    /* `shots` was incremented above, so this shell is shot number shots-1 —
     * the same index the replay will use at the other end. */
    cur_wind = wind_for(seed, shots - 1);
    wind_acc = 0;
    flying   = true;
    applog("[tanks] P%d fires a%d p%d (wind %d)", turn + 1, t->angle, t->power, cur_wind);
}

static void fire_clicked(lv_event_t *e)
{
    (void)e;
    if (flying || game_over) return;
    if (!my_turn()) return;
    do_fire();
}

static void wep_label(void)
{
    if (!lbl_wep) return;
    wep_t w = tk[turn].wep;
    if (w == WEP_SHELL) lv_label_set_text(lbl_wep, "Shell");
    else lv_label_set_text_fmt(lbl_wep, "%s x%d", WEP[w].name, lo[turn].ammo[w]);
    lv_obj_set_style_text_color(lbl_wep, lv_color_hex(WEP[w].rgb), 0);
}

/* Cycles only through what this player actually owns, so the button never
 * offers something that cannot be fired. */
static void wep_clicked(lv_event_t *e)
{
    (void)e;
    if (flying || game_over) return;
    if (!my_turn()) return;
    for (int n = 1; n <= WEP_N; n++) {
        wep_t c = (wep_t)((tk[turn].wep + n) % WEP_N);
        if (c == WEP_SHELL || lo[turn].ammo[c] > 0) { tk[turn].wep = c; break; }
    }
    wep_label();
}

static void adj_clicked(lv_event_t *e)
{
    if (flying || game_over) return;
    if (!my_turn()) return;                  /* not your tank to aim */
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    tank_t *t = &tk[turn];
    if (d == 1 || d == -1) { t->angle += d; if (t->angle < 0) t->angle = 0; if (t->angle > 90) t->angle = 90; }
    else                   { t->power += d / 10; if (t->power < 10) t->power = 10; if (t->power > 100) t->power = 100; }
    refresh_labels();
    render();
}

static void new_game(void)
{
    seed = esp_random();          /* the ONLY non-deterministic call */
    terrain_make(seed);
    memset(tk, 0, sizeof tk);
    tk[0].x = 26;  tk[0].col = C_GREEN;
    tk[1].x = PLAY_W - 26; tk[1].col = C_RED;
    for (int i = 0; i < N_TANKS; i++) {
        /* 🚨 THE SHOP'S PURCHASES ENTER THE SIMULATION HERE, and these are the
         * ones that will have to cross the air in multiplayer: max HP and
         * shield both feed damage, so both ends must agree on them or the two
         * screens disagree about who died. Cash and ammo counts never do.
         * See TANKS-PLAN.md 6b. */
        tk[i].hp     = TANK[lo[i].tier].hp;
        tk[i].shield = lo[i].shield;
        para_left[i] = lo[i].parachutes;
        tk[i].parachute = para_left[i] > 0;
        tk[i].wep    = WEP_SHELL;
        tk[i].dealt  = 0;
        tk[i].angle  = 45;  tk[i].power = 55;
        tank_settle(&tk[i]);
    }
    memset(sh, 0, sizeof sh);
    shots = 0; flying = false; game_over = false;
    paid = false; ai_wait = 0;
    tnk_gen++;
    banner_clear();
    tnk_pick_peer();
    /* The handset that pressed NEW starts. In hot seat and against the machine
     * that is P0 either way. */
    starter = tnk_net ? tnk_me : 0;
    turn = (starter + shots) & 1;
    applog("[tanks] new game, seed %08x, mode %d%s", (unsigned)seed, tnk_mode,
           tnk_net ? " (vs a handset)" : (tnk_ai() ? " (vs the machine)" : " (hot seat)"));
    /* Announce it: the other end sees a seed it does not have and joins. */
    tanks_net_send();
    refresh_labels();
    render();
}

/* ---- ESP-NOW ------------------------------------------------------------- *
 *
 * 🔑 THE WHOLE GAME IN 64 BYTES. Seed, both loadouts and every shot fired, so a
 * receiver rebuilds from scratch and a dropped board costs nothing. games.h is
 * explicit that "a lost board is not retried, by design" — carrying the full
 * history is what turns that from fatal into irrelevant. With only the latest
 * move on the wire, one lost frame leaves the two handsets permanently
 * disagreeing and nothing able to notice.
 *
 * A shot is 16 bits exactly: angle 0..90 (7) | power 0..100 (7) | weapon (2).
 * Twenty-four of them is 48 bytes, leaving 11 for the header.
 *
 * ⚠️ ONLY WHAT FEEDS THE SIMULATION TRAVELS. Max HP and shield do, because both
 * ends compute damage and must agree on it. Cash and ammo counts never do —
 * they are private and cannot change what the other handset sees.
 * See TANKS-PLAN.md 6b.
 */
#define TNK_OFF_SHOTS   0
#define TNK_OFF_HP0     1
#define TNK_OFF_HP1     2
#define TNK_OFF_SH0     3
#define TNK_OFF_SH1     4
#define TNK_OFF_PARA0   5
#define TNK_OFF_PARA1   6
#define TNK_OFF_SEED    7      /* 4 bytes */
#define TNK_OFF_SHOTS0 11      /* 2 bytes each */

/* 🔑 WHO AM I PLAYING, AND WHICH TANK AM I? Both DERIVED, never negotiated.
 * The peer is whoever is actually exchanging boards; the side is decided by
 * comparing the two device ids, so both ends reach the same answer from the
 * same two numbers with nothing to agree on. See "derive dont negotiate".
 *
 * ⚠️ espnow_play_idx() skips gateways — a gateway is a peer and is not a player,
 * and auto-picking "peer 0" is the bug the other games already paid for. */
static void tnk_pick_peer(void)
{
    /* Keep the handset already chosen for as long as it is still a playable
     * peer; only fall back to the first one when it is not. ⚠️ THESE ARE TRUE
     * PEER INDICES — espnow_play_idx() maps a playable position to the real
     * index, and a dense 0..n-1 index would address whoever took the slot the
     * moment somebody walks out of range. Same rule as pick_default_peer() in
     * games.c, and a gateway is never offered: it beacons like a handset, it
     * will never take its turn, and the builder has already lost an evening of Uno to
     * exactly that. */
    if (tnk_peer < 0 || tnk_peer >= espnow_peer_count() ||
        espnow_peer_is_gateway(tnk_peer))
        tnk_peer = espnow_play_idx(0);

    tnk_net = (tnk_mode == TNK_VS && tnk_peer >= 0);
    if (tnk_net)
        /* 🔑 WHICH TANK AM I? Derived, never negotiated: the lower device id is
         * always P0, so both ends reach the same answer from the same two
         * numbers with nothing to exchange and nothing to get wrong.
         * See "derive dont negotiate". */
        tnk_me = (espnow_my_id() < espnow_peer_id(tnk_peer)) ? 0 : 1;
    else
        tnk_me = 0;
}

/* The next playable handset after the current one, wrapping. A peer that aged
 * out starts the cycle from the beginning rather than failing — the button must
 * always do something. Mirrors play_next() in games.c. */
static void tnk_next_peer(void)
{
    int n = espnow_play_count();
    if (n <= 0) { tnk_peer = -1; tnk_net = false; tnk_me = 0; return; }
    int pos = -1;
    for (int k = 0; k < n; k++) if (espnow_play_idx(k) == tnk_peer) { pos = k; break; }
    tnk_peer = espnow_play_idx((pos + 1) % n);
    tnk_net  = (tnk_mode == TNK_VS && tnk_peer >= 0);
    if (tnk_net) tnk_me = (espnow_my_id() < espnow_peer_id(tnk_peer)) ? 0 : 1;
}

/* ---- the heartbeat ------------------------------------------------------ *
 *
 * 🚨 TANKS WAS THE ONE GAME IN THE MENU WITHOUT THIS, and it is why the builder and Kid 4
 * never finished a game. games_poll_net() heartbeats tic-tac-toe, Connect 4,
 * checkers, battleship and hearts; the comment sitting directly above that list
 * describes what happens to the one that does not:
 *
 *   "The design says a dropped board costs nothing because the next one repairs
 *    it — true only while somebody is still moving. When the lost frame is the
 *    one that would have told the other side it is THEIR turn, both sides sit
 *    waiting and no next board is ever sent."
 *
 * Tanks transmitted only on NEW and on FIRE, and games.h is explicit that a
 * board is never retried. Lose that single frame and neither handset can move
 * again — the full history cannot repair anything, because the repair only ever
 * rode on the next move. Re-announcing every two seconds turns every fault in
 * this protocol (a lost shot, a lost NEW, a handset that walked out of range and
 * came back) into something that heals itself before anybody reaches for the
 * button. */
static int64_t tnk_last_tx_us;

bool tanks_net_active(void)
{
    return tnk_net && scr && lv_scr_act() == scr;
}

void tanks_heartbeat(void)
{
    /* 🚨 THIS KEEPS BEATING AFTER THE GAME IS OVER, unlike checkers and the
     * rest, and deliberately. games.c: "a lost board costs nothing because the
     * next one replaces it, but a lost WIN was gone forever... The builder lost about
     * four of them to this". The other games repair that with a win counter
     * that rides in every frame and merges by max. Tanks has no separate
     * score — the shot history IS the result — so the ONLY repair path for a
     * lost final board is to keep sending it. The builder, 2026-09-20: "on my screen it
     * shows i won ... but on kid 3 it still shows that i am aiming". */
    if (!tanks_net_active()) return;
    if (esp_timer_get_time() - tnk_last_tx_us < 2000000) return;
    tanks_net_send();
}

static void tanks_net_send(void)
{
    if (!tnk_net || tnk_peer < 0) return;

    espnow_game_t g;
    memset(&g, 0, sizeof g);
    g.game    = GAME_TANKS;
    g.gen     = tnk_gen;
    g.cells   = (uint8_t)shots;
    g.starter = (starter == tnk_me) ? espnow_my_id() : espnow_peer_id(tnk_peer);

    g.board[TNK_OFF_SHOTS] = (uint8_t)shots;
    g.board[TNK_OFF_HP0]   = (uint8_t)(TANK[lo[0].tier].hp / 10);
    g.board[TNK_OFF_HP1]   = (uint8_t)(TANK[lo[1].tier].hp / 10);
    g.board[TNK_OFF_SH0]   = (uint8_t)(lo[0].shield / 10);
    g.board[TNK_OFF_SH1]   = (uint8_t)(lo[1].shield / 10);
    g.board[TNK_OFF_PARA0] = (uint8_t)lo[0].parachutes;
    g.board[TNK_OFF_PARA1] = (uint8_t)lo[1].parachutes;
    for (int i = 0; i < 4; i++)
        g.board[TNK_OFF_SEED + i] = (uint8_t)(seed >> (8 * i));

    int n = shots > MAX_SHOTS ? MAX_SHOTS : shots;
    for (int i = 0; i < n; i++) {
        int o = TNK_OFF_SHOTS0 + i * 2;
        if (o + 1 >= GAME_BOARD_MAX) break;
        uint16_t v = (uint16_t)((sh[i].angle & 0x7F) |
                                ((sh[i].power & 0x7F) << 7) |
                                ((sh[i].wep   & 0x03) << 14));
        g.board[o]     = (uint8_t)v;
        g.board[o + 1] = (uint8_t)(v >> 8);
    }
    tnk_last_tx_us = esp_timer_get_time();
    espnow_game_send(tnk_peer, &g);
}

static void tanks_apply(const espnow_game_t *g)
{
    if (!scr) return;                      /* not on this screen — ignore */
    if (tnk_mode != TNK_VS) return;        /* hot seat and the machine are local */

    uint32_t s2 = 0;
    for (int i = 0; i < 4; i++)
        s2 |= (uint32_t)g->board[TNK_OFF_SEED + i] << (8 * i);
    int n = g->board[TNK_OFF_SHOTS];
    if (n > MAX_SHOTS) n = MAX_SHOTS;

    /* 🚨 THEIR LOADOUT COMES OUT OF THE FRAME *BEFORE* ANY STALENESS TEST, and
     * this is the bug that made the builder win a game Kid 3 did not lose.
     *
     * Measured 2026-09-20, 13:21:27-29: one shot, "direct hit on P2" logged on
     * BOTH handsets, P2 dead on Parent's screen and alive on Kid 3's. Kid 3 had
     * JOINED Parent's game rather than starting it, and joining transmits nothing
     * — so his tank's real health had never crossed the air. It should have
     * ridden the next heartbeat, except a heartbeat carries the same seed and
     * the same shot count, so the staleness guard below threw the frame away
     * before anybody read the loadout out of it. Parent's handset killed a tank
     * it believed was a bare Scout.
     *
     * Their numbers are theirs, they are idempotent, and taking them can never
     * wind our game backwards — so they are never stale. ⚠️ Only what feeds the
     * damage arithmetic travels; cash and ammo stay private. */
    int them = 1 - tnk_me;
    int hp10 = g->board[them == 0 ? TNK_OFF_HP0 : TNK_OFF_HP1];
    int tier = 0;
    for (int t = TANK_TIERS - 1; t >= 0; t--)
        if (TANK[t].hp == hp10 * 10) { tier = t; break; }
    int shld = g->board[them == 0 ? TNK_OFF_SH0 : TNK_OFF_SH1] * 10;
    int para = g->board[them == 0 ? TNK_OFF_PARA0 : TNK_OFF_PARA1];
    bool loadout_new = (lo[them].tier != tier || lo[them].shield != shld ||
                        lo[them].parachutes != para);
    lo[them].tier = tier; lo[them].shield = shld; lo[them].parachutes = para;

    /* 🚨 ORDER BY GENERATION FIRST, THEN BY SHOT COUNT. espnow.h: "a new game
     * has FEWER marks than the one it replaces, and without a generation the
     * two rules fight". Tanks sent a constant gen of 1, so a fresh round whose
     * seed happened to be lower than the finished one's was refused forever. */
    if (g->gen < tnk_gen) return;
    if (g->gen == tnk_gen) {
        if (s2 != seed) {
            /* 🔑 TWO CHILDREN CAN PRESS NEW AT THE SAME MOMENT. Higher seed
             * wins — both ends compare the same two numbers and reach the same
             * answer with nothing exchanged, exactly as hearts does at
             * games.c ~3042. Without it A took B's battlefield while B took
             * A's and nothing ever converged. */
            if (s2 < seed) return;
        } else if (n <= shots && !loadout_new) {
            return;                        /* nothing in it we do not have */
        }
    }

    bool joined = (s2 != seed);
    if (joined) {                          /* a different game from the far end */
        /* 🚨 A DIFFERENT GAME KEEPS NONE OF THE OLD ONE'S SHOTS. Unpacking `n`
         * shots below only overwrites the first `n` slots, so without this the
         * tail of the abandoned round survives in `sh[]` — and a fresh game
         * has n = 0, which overwrites nothing at all. */
        memset(sh, 0, sizeof sh);
        seed    = s2;
        paid    = false;                   /* a new round: the payout is owed again */
        starter = (g->starter == espnow_my_id()) ? tnk_me : (1 - tnk_me);
        applog("[tanks] joined a game, seed %08x gen %u, I am P%d",
               (unsigned)s2, (unsigned)g->gen, tnk_me + 1);
    }
    tnk_gen = g->gen;

    for (int i = 0; i < n; i++) {
        int o = TNK_OFF_SHOTS0 + i * 2;
        if (o + 1 >= GAME_BOARD_MAX) break;
        uint16_t v = (uint16_t)(g->board[o] | (g->board[o + 1] << 8));
        sh[i].angle = (uint8_t)(v & 0x7F);
        sh[i].power = (uint8_t)((v >> 7) & 0x7F);
        sh[i].wep   = (uint8_t)((v >> 14) & 0x03);
    }

    flying = false;                        /* their board is the truth now */
    /* 🔑 WHAT TO REBUILD TO. Their board when it is a different game or a
     * longer history; OUR count when the only new thing in the frame was their
     * loadout, because truncating to `n` there would throw away shots we have
     * legitimately fired and they have not seen yet. */
    rebuild_to(joined ? n : (n > shots ? n : shots));
    refresh_labels();
    render();

    /* 🔑 ANSWER A GAME WE HAVE JUST JOINED, ONCE. Joining used to be silent, so
     * the far end did not learn our tank until we took our first shot. One
     * reply closes that window immediately; it cannot ping-pong, because the
     * board it provokes carries the same gen, the same seed and the same shot
     * count and is dropped by the test above. */
    if (joined) tanks_net_send();
}

void tanks_net_take(const espnow_game_t *g, int peer_idx)
{
    /* 🚨 ONLY THE HANDSET YOU PICKED. This used to latch tnk_peer to whoever
     * sent the frame AND force tnk_net = true — so a third handset sitting on
     * the tanks screen dragged you into its game mid-round, and a hot-seat game
     * could be switched into net play by a frame arriving from across the room.
     *
     * The other games deliberately "play whoever is actually sending boards",
     * because they have no picker. Tanks has one now, because the builder asked for
     * one, and an explicit choice must outrank a stray frame. */
    if (tnk_mode != TNK_VS) return;
    if (peer_idx < 0) return;
    if (tnk_peer_chosen && tnk_peer >= 0 && peer_idx != tnk_peer) return;

    tnk_peer = peer_idx;
    /* 🔑 WHICH TANK AM I? Derived, not negotiated: the lower device id is
     * always P0. Both ends compute the same answer from the same two numbers
     * with no exchange to get wrong. See "derive dont negotiate". */
    tnk_me  = (espnow_my_id() < espnow_peer_id(tnk_peer)) ? 0 : 1;
    tnk_net = true;
    tanks_apply(g);
}

/* ---- the shop -------------------------------------------------------------
 *
 * The builder: *"you could start in a menu that would let you start with some money and
 * you would buy your tank and some ammo... and befor each game you would get the
 * shop to get upgrades with you cash"*. So it runs BEFORE the first battle as
 * well as between rounds — the first purchase is part of the game, not a reward
 * for surviving one.
 *
 * Each player buys in turn on the same handset. In multiplayer each end will
 * shop privately and only the HP and shield totals need to cross.
 */
static void shop_open(int who);
static lv_obj_t *mkbtn(lv_obj_t *p, int w, int h, lv_color_t bg, const char *txt,
                       lv_event_cb_t cb, void *ud, lv_align_t al, int ox, int oy);

static lv_obj_t *shop_cash, *shop_rows[6];
/* The chooser strip across the top of the shop. 🔑 IT LIVES HERE AND NOT ON THE
 * BATTLE SCREEN because the battle screen has no room — the canvas runs to
 * y=224 and three rows of buttons fill what is left — and because the shop is
 * already the thing you see before every round. No extra tap, nothing new to
 * find. */
static lv_obj_t *shop_title, *shop_go, *shop_mbtn[5], *shop_vs;
static void shop_refresh(void);
static const char *MODE_NAME[5] = { "2P", "Easy", "Med", "Hard", "VS" };

/* A 12pt button. mkbtn() hardcodes montserrat_14, which will not fit five
 * labels across 240px. */
static lv_obj_t *smallbtn(lv_obj_t *p, int w, int h, lv_color_t bg, const char *txt,
                          lv_event_cb_t cb, void *ud, lv_align_t al, int ox, int oy)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    /* 🔑 EXTRA CLICK AREA, ALWAYS. A release landing a pixel outside a small
     * target reads as "you have to press it several times" — the fault
     * "lvgl clicked vs released" was written for, and worst on exactly
     * this size of button. */
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(l, C_INK, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_align(b, al, ox, oy);
    return b;
}

static void btn_text(lv_obj_t *b, const char *txt)
{
    if (!b) return;
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l) lv_label_set_text(l, txt);
}

/* Everything on the strip that can change when the mode or the opponent does. */
static void shop_chrome(void)
{
    for (int m = 0; m < 5; m++)
        if (shop_mbtn[m])
            lv_obj_set_style_bg_color(shop_mbtn[m],
                                      m == tnk_mode ? C_BLUE : C_PANEL, 0);
    if (shop_vs) {
        const char *pn = (tnk_peer >= 0) ? espnow_peer_name(tnk_peer) : NULL;
        char t[40];
        if (tnk_mode == TNK_VS)
            snprintf(t, sizeof t, "vs: %s", pn ? pn : espnow_nobody_text());
        else if (tnk_ai())
            snprintf(t, sizeof t, "vs: the machine");
        else
            snprintf(t, sizeof t, "vs: P2 on this handset");
        btn_text(shop_vs, t);
    }
    if (shop_title) {
        lv_obj_set_style_text_color(shop_title, shop_who == 0 ? C_GREEN : C_RED, 0);
        lv_obj_align(shop_title, LV_ALIGN_TOP_MID, 0, 4);
    }
    /* Hot seat is the only mode with a second shop to hand over to. */
    btn_text(shop_go, (tnk_mode == TNK_2P && shop_who == 0) ? "P2's turn" : "FIGHT");
}

static void mode_clicked(lv_event_t *e)
{
    int m = (int)(intptr_t)lv_event_get_user_data(e);
    tnk_mode = m;
    tnk_pick_peer();
    /* In VS the shop belongs to whichever tank THIS handset drives, and that is
     * derived from the two device ids — it is not always P1. */
    shop_who = tnk_local();
    shop_refresh();
    shop_chrome();
}

static void peer_clicked(lv_event_t *e)
{
    (void)e;
    /* Tapping the opponent means "play a handset", so it selects VS as well as
     * cycling. Asking the child to press two buttons in the right order is how
     * you get a game against nobody. */
    if (tnk_mode != TNK_VS) { tnk_mode = TNK_VS; tnk_pick_peer(); }
    else                     tnk_next_peer();
    tnk_peer_chosen = (tnk_peer >= 0);
    shop_who = tnk_local();
    shop_refresh();
    shop_chrome();
}

static void shop_refresh(void)
{
    loadout_t *L = &lo[shop_who];
    if (shop_cash)
        lv_label_set_text_fmt(shop_cash, "P%d    $%d", shop_who + 1, L->cash);
    if (shop_rows[0])
        lv_label_set_text_fmt(shop_rows[0], "Tank: %s (%d hp)%s",
                              TANK[L->tier].name, TANK[L->tier].hp,
                              L->tier + 1 < TANK_TIERS ? "" : "  MAX");
    if (shop_rows[1])
        lv_label_set_text_fmt(shop_rows[1], "Shield: %d", L->shield);
    if (shop_rows[2])
        lv_label_set_text_fmt(shop_rows[2], "Parachutes: %d", L->parachutes);
    if (shop_rows[3])
        lv_label_set_text_fmt(shop_rows[3], "Heavy: %d", L->ammo[WEP_HEAVY]);
    if (shop_rows[4])
        lv_label_set_text_fmt(shop_rows[4], "Digger: %d", L->ammo[WEP_DIGGER]);
}

static void buy_clicked(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    loadout_t *L = &lo[shop_who];
    switch (what) {
        case 0:
            if (L->tier + 1 < TANK_TIERS && L->cash >= TANK[L->tier + 1].price) {
                L->cash -= TANK[L->tier + 1].price; L->tier++;
            }
            break;
        case 1:
            if (L->cash >= SHIELD_PRICE) { L->cash -= SHIELD_PRICE; L->shield += SHIELD_STEP; }
            break;
        case 2:
            if (L->cash >= PARA_PRICE) { L->cash -= PARA_PRICE; L->parachutes++; }
            break;
        case 3:
            if (L->cash >= WEP[WEP_HEAVY].price) { L->cash -= WEP[WEP_HEAVY].price; L->ammo[WEP_HEAVY]++; }
            break;
        case 4:
            if (L->cash >= WEP[WEP_DIGGER].price) { L->cash -= WEP[WEP_DIGGER].price; L->ammo[WEP_DIGGER]++; }
            break;
    }
    shop_refresh();
}

static void shop_done(lv_event_t *e)
{
    (void)e;
    /* 🚨 lv_obj_del_async, NOT lv_obj_del — AND ONLY AFTER THE NEXT SCREEN IS
     * LOADED. This handler belongs to a button that lives ON scr_shop, so a
     * synchronous delete frees the object mid-dispatch and the crash lands back
     * in LVGL with nothing pointing at this file. The builder found it immediately:
     * "it crashes when i click the button for 2nd player turn".
     *
     * ⚠️ The pattern is spelled out in games.c's leave_to() — load the
     * destination first, then delete asynchronously — and I wrote this while
     * working two files away from that comment. */
    lv_obj_t *old = scr_shop;
    scr_shop   = NULL;
    shop_cash  = NULL;
    shop_title = shop_go = shop_vs = NULL;
    memset(shop_mbtn, 0, sizeof shop_mbtn);
    memset(shop_rows, 0, sizeof shop_rows);

    /* 🔑 ONLY HOT SEAT HAS A SECOND SHOP. Against the machine or another
     * handset this board kits out one tank; the machine buys for itself, and
     * the far handset has its own shop on its own screen. */
    if (tnk_mode == TNK_2P && shop_who == 0) {
        shop_open(1);               /* builds and loads P2's shop */
    } else {
        if (tnk_ai()) ai_shop();
        started = true;
        new_game();
        lv_scr_load(scr);
    }
    if (old) lv_obj_del_async(old); /* safe now: it is no longer the active screen */
}

static void shop_open(int who)
{
    shop_who = who;
    scr_shop = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr_shop);
    lv_obj_set_size(scr_shop, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(scr_shop, C_BG, 0);
    lv_obj_set_style_bg_opa(scr_shop, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr_shop, LV_OBJ_FLAG_SCROLLABLE);

    shop_title = lv_label_create(scr_shop);
    lv_obj_set_style_text_font(shop_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(shop_title, who == 0 ? C_GREEN : C_RED, 0);
    lv_label_set_text(shop_title, "SHOP");
    lv_obj_align(shop_title, LV_ALIGN_TOP_MID, 0, 4);

    /* The chooser. Same five names, same order and the same 44x46 spacing as
     * ck_build() in games.c, because a child who has learned it on checkers
     * should not have to learn it again here. */
    for (int m = 0; m < 5; m++)
        shop_mbtn[m] = smallbtn(scr_shop, 44, 22, C_PANEL, MODE_NAME[m],
                                mode_clicked, (void *)(intptr_t)m,
                                LV_ALIGN_TOP_LEFT, 6 + m * 46, 24);

    shop_vs = smallbtn(scr_shop, 228, 20, C_LINE, "vs: -", peer_clicked, NULL,
                       LV_ALIGN_TOP_LEFT, 6, 50);

    shop_cash = lv_label_create(scr_shop);
    lv_obj_set_style_text_font(shop_cash, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(shop_cash, C_AMBER, 0);
    lv_obj_align(shop_cash, LV_ALIGN_TOP_MID, 0, 74);

    static const struct { const char *buy; int price; } ROW[5] = {
        { "UPGRADE", -1 },          /* price varies by tier */
        { "BUY", SHIELD_PRICE },
        { "BUY", PARA_PRICE },
        { "BUY", 0 },               /* filled below */
        { "BUY", 0 },
    };
    int prices[5] = { 0, SHIELD_PRICE, PARA_PRICE,
                      WEP[WEP_HEAVY].price, WEP[WEP_DIGGER].price };

    for (int i = 0; i < 5; i++) {
        /* 36px rows rather than 40, to buy back the height the chooser takes.
         * The last row ends at y=251 and FIGHT starts at 274. */
        int y = 96 + i * 36;
        shop_rows[i] = lv_label_create(scr_shop);
        lv_obj_set_style_text_font(shop_rows[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(shop_rows[i], C_INK, 0);
        lv_obj_align(shop_rows[i], LV_ALIGN_TOP_LEFT, 10, y + 4);

        lv_obj_t *pl = lv_label_create(scr_shop);
        lv_obj_set_style_text_font(pl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(pl, C_MUTED, 0);
        if (i == 0) lv_label_set_text_fmt(pl, "$%d", TANK[1].price);
        else        lv_label_set_text_fmt(pl, "$%d", prices[i]);
        lv_obj_align(pl, LV_ALIGN_TOP_LEFT, 10, y + 17);

        mkbtn(scr_shop, 74, 28, C_LINE, ROW[i].buy, buy_clicked,
              (void *)(intptr_t)i, LV_ALIGN_TOP_RIGHT, -8, y + 1);
    }

    shop_go = mkbtn(scr_shop, 110, 36, C_AMBER, "FIGHT", shop_done, NULL,
                    LV_ALIGN_BOTTOM_MID, 0, -8);

    shop_refresh();
    shop_chrome();
    lv_scr_load(scr_shop);
}

static void new_clicked(lv_event_t *e)
{
    (void)e;
    tnk_pick_peer();          /* the opponent may have walked off since the last round */
    shop_open(tnk_local());
}

static void back_clicked(lv_event_t *e)
{
    (void)e;
    if (tmr) { lv_timer_del(tmr); tmr = NULL; }
    flying = false;
    games_show_menu();
    /* Same rule as shop_done: this button lives on the screen being removed. */
    if (scr) { lv_obj_del_async(scr); scr = NULL; }
    canvas = lbl_turn = lbl_stats = lbl_hint = lbl_wep = NULL;
    lbl_ang = lbl_pow = NULL;
    banner = NULL;   /* deleted with the screen above; never free it twice */
    /* 🚨 cbuf IS DELIBERATELY NOT FREED HERE. The delete above is ASYNC, so the
     * canvas object still exists — and still points at this buffer — until
     * LVGL gets round to it. Freeing now is a use-after-free with a one-frame
     * fuse.
     * It is 94KB of PSRAM, not internal RAM, and PSRAM is the one thing this
     * board has spare (6.4MB free). games.c's "rebuild, don't hold" rule is
     * about INTERNAL RAM and does not apply. Holding it also makes re-entry
     * instant. */
}

/* ---- little widget helpers ---------------------------------------------- */
/* Positions the button itself. The first cut aligned by fishing the last child
 * back out of the parent with lv_obj_get_child_cnt() - correct only for as long
 * as nothing else is ever added between the calls, which is the kind of thing
 * that breaks silently a month later when a label moves. */
static lv_obj_t *mkbtn(lv_obj_t *p, int w, int h, lv_color_t bg, const char *txt,
                       lv_event_cb_t cb, void *ud,
                       lv_align_t al, int ox, int oy)
{
    lv_obj_t *b = lv_btn_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 4, 0);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, C_INK, 0);
    lv_label_set_text(l, txt);
    lv_obj_center(l);
    lv_obj_align(b, al, ox, oy);
    return b;
}

/* 🔑 STARTING CASH, ONCE PER POWER-ON. The builder: "start with some money and you
 * would buy your tank and some ammo" — so the first shop is a real choice, not
 * a formality with nothing affordable in it. $200 buys a Heavy tank, or a
 * shield and two diggers, or a spread — enough for the first decision to
 * matter. */
#define START_CASH 200

void tanks_show(void)
{
    if (!started) {
        for (int i = 0; i < N_TANKS; i++) {
            memset(&lo[i], 0, sizeof lo[i]);
            lo[i].cash = START_CASH;
        }
    }
    if (!cbuf) {
        cbuf = heap_caps_malloc((size_t)PLAY_W * PLAY_H * sizeof(lv_color_t),
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!cbuf) { ESP_LOGE(TAG, "no PSRAM for the canvas"); return; }
    }

    scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, LCD_W, LCD_H);
    lv_obj_set_style_bg_color(scr, C_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Header. RELEASED for Back, and 14px of invisible click area: the same
     * fix the other games needed — see mk_hdr() in games.c. */
    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, LCD_W, HDR_H);
    lv_obj_set_style_bg_color(bar, C_PANEL, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t *bk = lv_btn_create(scr);
    lv_obj_remove_style_all(bk);
    lv_obj_set_size(bk, 54, 22);
    lv_obj_set_style_bg_color(bk, C_LINE, 0);
    lv_obj_set_style_bg_opa(bk, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bk, 4, 0);
    lv_obj_align(bk, LV_ALIGN_TOP_LEFT, 3, 2);
    lv_obj_set_ext_click_area(bk, 14);
    lv_obj_add_event_cb(bk, back_clicked, LV_EVENT_RELEASED, NULL);
    lv_obj_t *bl = lv_label_create(bk);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(bl, C_INK, 0);
    lv_label_set_text(bl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(bl);

    lbl_stats = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_stats, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_stats, C_MUTED, 0);
    lv_obj_align(lbl_stats, LV_ALIGN_TOP_RIGHT, -4, 7);

    canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(canvas, cbuf, PLAY_W, PLAY_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_align(canvas, LV_ALIGN_TOP_MID, 0, PLAY_Y);

    lbl_turn = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_turn, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_turn, C_AMBER, 0);
    /* 🚨 A HARD WIDTH, NOT A HOPE. These two labels sit at opposite ends of one
     * 240px line and both grow with their text — "Kid 3 is aiming..." beside
     * "SUDDEN DEATH" is wider than the screen. Clipping with an ellipsis is
     * ugly; two labels drawn on top of each other is unreadable, which is what
     * the builder got. 146 + 82 + margins fits. */
    lv_label_set_long_mode(lbl_turn, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_turn, 128);
    lv_obj_align(lbl_turn, LV_ALIGN_TOP_LEFT, 8, PLAY_Y + PLAY_H + 4);

    lbl_hint = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_hint, C_RED, 0);
    lv_label_set_long_mode(lbl_hint, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl_hint, 94);   /* 8+128 = 136, 240-8-94 = 138: no overlap */
    lv_obj_set_style_text_align(lbl_hint, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(lbl_hint, LV_ALIGN_TOP_RIGHT, -8, PLAY_Y + PLAY_H + 6);

    /* Angle on the left, power on the right, FIRE big in the middle. 40x34 with
     * 10px of invisible margin - sized for a six-year-old's finger, not the builder's:
     * "tune for the user not the tester".
     *
     * 🚨 EVERY OFFSET BELOW IS CHECKED AGAINST LCD_H (320). The previous layout
     * put AMMO at y=328 and the weapon label at y=332 — off the bottom of the
     * panel and therefore unusable, so the Heavy shell and the Digger could be
     * bought and never fired. FIRE was clipped by 10px and NEW by 5. Anything
     * added here must still end above 320. */
    int row = PLAY_Y + PLAY_H + 2;                 /* 202 */
    mkbtn(scr, 40, 34, C_LINE,  "-",  adj_clicked, (void *)(intptr_t)-1,  LV_ALIGN_TOP_LEFT,   6, row + 24);
    mkbtn(scr, 40, 34, C_LINE,  "+",  adj_clicked, (void *)(intptr_t)1,   LV_ALIGN_TOP_LEFT,  50, row + 24);
    mkbtn(scr, 40, 34, C_BLUE,  "<",  adj_clicked, (void *)(intptr_t)-10, LV_ALIGN_TOP_LEFT, 152, row + 24);
    mkbtn(scr, 40, 34, C_BLUE,  ">",  adj_clicked, (void *)(intptr_t)10,  LV_ALIGN_TOP_LEFT, 196, row + 24);

    /* The live numbers, directly under the buttons that change them. */
    lbl_ang = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_ang, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_ang, C_MUTED, 0);
    lv_obj_align(lbl_ang, LV_ALIGN_TOP_LEFT, 12, row + 60);
    lbl_pow = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_pow, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_pow, C_MUTED, 0);
    lv_obj_align(lbl_pow, LV_ALIGN_TOP_RIGHT, -12, row + 60);

    /* Weapon name between them, on the same line. */
    lbl_wep = lv_label_create(scr);
    lv_obj_set_style_text_font(lbl_wep, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_wep, C_AMBER, 0);
    lv_obj_align(lbl_wep, LV_ALIGN_TOP_MID, 0, row + 60);

    /* Bottom row: AMMO left, FIRE centre, NEW right. Ends at y=316. */
    mkbtn(scr, 64, 30, C_LINE,  "AMMO", wep_clicked,  NULL, LV_ALIGN_TOP_LEFT,   6, row + 82);
    mkbtn(scr, 74, 38, C_AMBER, "FIRE", fire_clicked, NULL, LV_ALIGN_TOP_MID,    0, row + 78);
    mkbtn(scr, 56, 30, C_LINE,  "NEW",  new_clicked,  NULL, LV_ALIGN_TOP_RIGHT, -8, row + 82);

    new_game();
    tmr = lv_timer_create(tick, TICK_MS, NULL);

    /* 🚨 THE SHOP COMES FIRST, BEFORE THE FIRST BATTLE. On a fresh power-on
     * both players kit out; afterwards NEW returns here between rounds. The
     * battle screen is built either way because the shop hands straight to it. */
    if (!started) { tnk_pick_peer(); shop_open(tnk_local()); }
    else          lv_scr_load(scr);
}
