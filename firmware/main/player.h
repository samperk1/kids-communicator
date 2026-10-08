#pragma once
#include <stdbool.h>

/* ⚠️ THE STORED VALUE 2 WAS SINK_BT AND STILL EXISTS IN NVS.
 * Bluetooth was removed 2026-09-06; handsets last used with the dongle have a
 * 2 saved under "sink". settings.c clamps it on load — do not renumber these. */
typedef enum { SINK_SPEAKER = 0, SINK_LINEOUT = 1 } player_sink_t;

typedef struct {
    bool          playing;
    int           album;        /* global album index, -1 = nothing loaded */
    int           track;        /* index within the album */
    char          title[128];
    int           progress;     /* 0-100, from bytes consumed */
    int           elapsed_ms;
    player_sink_t sink;
    int           volume;       /* 0-100 */
} player_state_t;

void player_init(void);
/* Reload the last-played track without starting it — see the note in player.c
 * about why this deliberately does not auto-play. */
void player_restore(void);
const player_state_t *player_state(void);

void player_play(int album_idx, int track_n);
void player_toggle(void);
void player_next(void);

/* ---- shuffle -------------------------------------------------------------
 * 🔑 THE SCOPE IS THE POINT. "Random" on a 3,987-track card is a different
 * feature from "random" inside the album a child just chose: the first is
 * discovery, the second is listening to one record without hearing it in the
 * same order every time. Both are wanted, so the mode carries its scope.
 *
 * ⚠️ ALBUM and ARTIST follow whatever is PLAYING, re-read at each track change,
 * so picking a new album while shuffling stays inside the new one. */
typedef enum {
    SHUFFLE_OFF = 0,
    SHUFFLE_ALBUM,      /* within the album now playing */
    SHUFFLE_ARTIST,     /* anything by the artist now playing */
    SHUFFLE_CARD,       /* anything on the card */
    SHUFFLE_FAVES,      /* only the hearted songs — see faves.h */
} player_shuffle_t;

void             player_set_shuffle(player_shuffle_t m);
player_shuffle_t player_shuffle(void);
/* "Off" / "Album" / "Artist" / "Card" - for the button label. */
const char      *player_shuffle_name(player_shuffle_t m);
void player_prev(void);
void player_set_sink(player_sink_t sink);

void player_set_volume(int pct);

/* Silence the output without touching - or saving - the volume setting. Moving
 * the slider clears it. */
/* Where audio that is NOT the music player should be written: the user's sink
 * choice. Radio and the walkie-talkie use it so the SINK button governs them
 * too. */
player_sink_t player_out_sink(void);

void player_set_mute(bool on);
bool player_muted(void);

/* Stop for a voice interaction; pass the return value back afterwards. */
bool player_suspend(void);
void player_unsuspend(bool was_playing);
