#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "lvgl.h"

/* The vector road map — .vmp tiles off the SD card, drawn as lines.
 *
 * 🔑 VECTORS, NOT PICTURES. The builder, 2026-09-12: *"in 1999 I had a phone that would
 * do GPS for driving, the SD card was 100 MB at most — there must be a good way
 * to do it."* There is, and it is what those units did. ONE dataset renders at
 * EVERY zoom: all of Michigan is 37 MB of roads, against 163 MB of raster tiles
 * that would only cover z6-z12 and go blurry past the zoom they were cut for.
 *
 * Tiles are `/sdcard/vmap/12/<x>/<y>.vmp`, built by tools/osm2vmp.py. The
 * format is described there; the short version is a 16-byte header and then
 * flat records of {class, minzoom, npoints, npoints x u16 pairs} until EOF.
 *
 * ⚠️ Data is © OpenStreetMap contributors (ODbL). The attribution on screen is
 * not decoration — it is the licence condition, and it is why this can use a
 * Geofabrik extract at all. */

/* z8 is the whole of south-east Michigan on one screen. The overview level
 * makes it cheap: below z12 the device reads 27 statewide tiles, not 3,496. */
#define MAP_Z_MIN   8
#define MAP_Z_MAX   17
#define MAP_TILE_Z  12
#define MAP_OVER_Z  8                  /* the overview level, used below z12 */
#define MAP_SUB     65536              /* sub-tile units per tile edge */

/* 🚨 TWO LEVELS, AND THE SECOND ONE IS NOT AN OPTIMISATION — IT IS THE FIX.
 *
 * Measured on hardware 2026-09-13: at display zoom 10 the screen spans 20-30
 * z12 tiles. Near a big city that is ~3 MB read off the card PER REDRAW against a
 * 9-tile cache, every feature in each walked to draw a handful of motorways.
 * The handset looked locked up. 🔑 A tile costs what it CONTAINS, not what gets
 * drawn from it, so zoom alone can never make a dense tile cheap.
 *
 * Below z12 the device reads z8 tiles instead: 16x the ground each, carrying
 * only roads with minzoom <= 11. One or two small files instead of thirty big
 * ones. */

/* True when /sdcard/vmap/index.txt is readable, i.e. the card has a map. */
bool map_available(void);

/* One line for the screen: the tile count, or why there is nothing to draw. */
const char *map_status(void);

int  map_zoom(void);
void map_zoom_set(int z);

/* Where the screen is looking. Following mode keeps this on the fix. */
void map_center_set(double lat, double lon);
void map_pan_px(int dx, int dy);

/* The walk. The trail is worth more than the pin: it shows WHERE the receiver
 * dropped out, which is the actual case-placement evidence. */
void map_trail_add(double lat, double lon);
void map_trail_clear(void);
int  map_trail_count(void);

void map_pin_set(double lat, double lon);
bool map_pin_get(double *lat, double *lon);

/* Draw everything into a canvas sized w x h. `have_fix` decides whether the
 * position marker is drawn solid or hollow. */
void map_draw(lv_obj_t *canvas, int w, int h, bool have_fix);

/* Bumps whenever a redraw would look different — centre, zoom, trail or pin.
 * ui.c redraws only when this has moved, because map_draw() is the longest
 * thing that runs on the LVGL task and it was repeating itself twice a second
 * while the handset stood still. */
uint32_t map_generation(void);

/* Where the LAST redraw spent its time: total, the part inside tile_get()
 * (card I/O, and only on a cache miss), and how many tiles missed. Everything
 * not in `io_ms` is the feature walk. See map.c — the two halves want opposite
 * fixes, so this is measured before either is built. */
void map_draw_stats(uint32_t *total_ms, uint32_t *io_ms, uint16_t *misses);

/* ---- the card as an RF aggressor -----------------------------------------
 *
 * 🔑 THE MAP IS THE HEAVIEST CARD USER IN THE FIRMWARE and the GPS field test
 * needs to reproduce that on demand. This reads real .vmp tiles off the card,
 * BYPASSING the tile cache — a cached read touches no SDMMC clock and would
 * make the "loud" half of the card A/B silently identical to the quiet one.
 *
 * Returns bytes read, 0 if there is nothing to read. Call it in a loop from a
 * low-priority task; it blocks for as long as one tile takes. */
size_t map_card_probe(void);

/* Forget the discovered tile list. Call after the card has been unmounted and
 * remounted, in case the card itself changed. */
void map_card_probe_reset(void);
