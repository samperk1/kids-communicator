#pragma once
#include "lvgl.h"

/* GENERATED sprites — see art/sprites.py (ASCII grids) and tools/png2lvgl.py.
 * All `const`, so they live in flash and cost no RAM. Chroma-keyed on #00FF00,
 * packed by LV_COLOR_MAKE at compile time so LV_COLOR_16_SWAP is handled.
 *
 * ⚠️ The palette in sprites.py is GLOBAL across every grid in the file. Reusing
 * a key already spent on another sprite is how the nest came out BLUE — 'B' was
 * shield blue long before the nest wanted a light brown. */
extern const lv_img_dsc_t flamingo1, flamingo2;
extern const lv_img_dsc_t tree, cactus, meanbird, rock;
extern const lv_img_dsc_t tree_s, tree_l, cactus_s, cactus_l;
extern const lv_img_dsc_t meanbird2, meanbird3;   /* same grid, palette swapped */
extern const lv_img_dsc_t biplane;                /* the title banner, not the villain */
extern const lv_img_dsc_t airplane, asteroid;
extern const lv_img_dsc_t nest, babyflam, duck, turtle, cattail;
extern const lv_img_dsc_t pu_life, pu_shield, pu_lighter, pu_slow;
