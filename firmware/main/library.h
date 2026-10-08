#pragma once
#include <stddef.h>
#include "esp_err.h"
#include <stdbool.h>

/* `name` is deliberately the FIRST member of all three structs: the sort
 * comparator casts the struct pointer straight to char* and compares names. */
typedef struct {
    char name[128];
    int  first_album;
    int  n_albums;
    int  n_tracks;
} lib_artist_t;

typedef struct {
    char name[128];
    int  artist;        /* rebuilt after the artist sort */
    int  first_track;
    int  n_tracks;
} lib_album_t;

typedef struct {
    char name[128];
    int  album;         /* ⚠️ stale after the album sort — nothing reads it.
                         * Track ranges come from album->first_track instead. */
} lib_track_t;

typedef void (*library_progress_cb)(int artists, int albums, int tracks);

esp_err_t library_mount(void);
/* Give the card up so USB mass storage can have it. Everything that reads the
 * card must have stopped first — see usbmsc.c. */
esp_err_t library_unmount(void);
/* Walks the card, or loads the cached result when the card has not changed.
 * See library.c: validity is decided from the card's own serial, capacity and
 * free space, so an edit made in a PC card reader invalidates it too. */
esp_err_t library_scan(library_progress_cb cb);

/* Delete the cache so the next boot does a full walk. For a "Rescan library"
 * action, or after anything that rewrites the card without changing its free
 * space. */
void library_invalidate_cache(void);

int  library_artist_count(void);
const lib_artist_t *library_artist(int i);

/* ---- the A-Z index -------------------------------------------------------
 * 🔑 THE SORT KEY SKIPS A LEADING "The ". Measured 2026-09-21 on the 519-artist
 * card: 55 artists begin with "The", which piles 76 of them under T - the worst
 * bucket by far - and files "The White Stripes" under T, where nobody looks for
 * it. Indexing on the key instead puts it under W and drops the worst bucket to
 * 41. Artists are SORTED by this key, so every letter is one contiguous run. */
const char *library_sortkey(const char *name);
/* 'A'..'Z' for an artist, '#' for anything else (digits, symbols). */
char library_artist_letter(int i);
/* First index and count for one letter. false (and count 0) if none. */
bool library_letter_range(char letter, int *first, int *count);
/* The first letter that has any artists — where the index opens. */
char library_first_letter(void);

/* ---- browsing by ALBUM ---------------------------------------------------
 * The builder, 2026-09-21: "this was an album and the songs are ment to be togather ...
 * if i wont to lisin to the black panther sound track i go to the black panther
 * sound track". Artist-first browsing cannot express that: a compilation has no
 * one artist, so it can only be reached through whoever happens to sing track
 * one. These give a flat, name-sorted list of every album on the card, indexed
 * by the same A-Z key as the artists. */
int  library_albums_total(void);
/* The i-th album in NAME order; returns the GLOBAL album index the rest of the
 * album API already takes. */
int  library_album_sorted(int i);
char library_album_letter(int i);          /* i is the SORTED position */
bool library_album_letter_range(char letter, int *first, int *count);
char library_album_first_letter(void);

int  library_album_count(int artist);
const lib_album_t  *library_album(int artist, int n);
const lib_album_t  *library_album_by_index(int idx);   /* global index */

/* ---- shuffle -------------------------------------------------------------
 * Pick by a flat index over a scope and get back (album, track). The scope's
 * size comes from the matching total. See library.c on why these walk album
 * counts rather than assuming an artist's tracks are contiguous. */
int  library_total_tracks(void);
int  library_artist_track_total(int artist);
bool library_pick_global(int g, int *album_out, int *track_out);
bool library_pick_in_artist(int artist, int g, int *album_out, int *track_out);

/* Albums are addressed by their GLOBAL index for tracks, which is what
 * library_album() returns a pointer into — use artist->first_album + n. */
int  library_track_count(int album_idx);
const lib_track_t  *library_track(int album_idx, int n);
void library_track_path(int album_idx, int n, char *out, size_t len);
