#pragma once
#include <stdbool.h>

/* ---- favourites ----------------------------------------------------------
 *
 * 🚨 A FAVOURITE IS A PATH, NEVER AN INDEX. album/track indexes are produced by
 * the scan and change whenever the card is rewritten - which is exactly what a
 * library rebuild does. Storing indexes would silently point every heart at a
 * different song the first time the card is refreshed, and it would look like
 * the favourites had been shuffled rather than lost. The path is what the user
 * actually chose, so the path is what is stored, and the indexes are re-derived
 * after every scan.
 *
 * 🔑 UNRESOLVED FAVOURITES SURVIVE. A path that is not on the card right now is
 * kept in the list and written back out untouched. Cards move between handsets
 * in this house; putting a different card in must not quietly delete the
 * favourites belonging to the other one.
 *
 * The file is `/sdcard/favourites.m3u` - a real playlist format, so it can be
 * read, edited and backed up on a PC without this firmware being involved. */

/* Read the file and resolve what is on the card. Call AFTER library_scan(). */
void faves_load(void);

/* Write the file if anything changed. Cheap and safe to call often. */
void faves_flush(void);

bool faves_contains(int album_idx, int track);
/* Returns the new state (true = now a favourite). */
bool faves_toggle(int album_idx, int track);

/* How many favourites are actually present on this card - the shuffle scope. */
int  faves_count(void);
/* The g-th resolved favourite, 0 <= g < faves_count(). */
bool faves_pick(int g, int *album_out, int *track_out);
