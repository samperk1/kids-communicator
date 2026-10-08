#pragma once
#include <stdbool.h>
#include "lvgl.h"

/* Album art, read from the picture EMBEDDED in the MP3.
 *
 * 🔑 THE ART WAS ALWAYS THERE AND NOTHING EVER LOOKED AT IT. library.c builds
 * the whole library from the folder tree — /music/<Artist>/<Album>/<Track>.mp3 —
 * and parses no tags at all, which is why artist and album are always right and
 * why no device has ever shown a cover. Every card the pipeline builds carries a
 * normalised baseline JPEG in each file's APIC frame.
 *
 * Decoded once per ALBUM, not per track: the cover does not change between
 * tracks and a decode is tens of milliseconds of SD reads the player would
 * rather spend on audio. */

/* Decode the picture in `path` into a PSRAM buffer. False if the file has no
 * picture, the tag is unreadable, or memory ran out — every one of which is a
 * normal condition, not an error. The previous image is freed either way. */
bool art_load(const char *path);

/* The decoded cover, or NULL. Owned here; valid until the next art_load(). */
const lv_img_dsc_t *art_img(void);

void art_clear(void);
