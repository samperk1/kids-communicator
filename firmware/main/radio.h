#pragma once
#include <stdbool.h>

/* Internet radio. "Radio" means INTERNET radio throughout this firmware; the
 * walkie-talkie is "Talkie" and the AI assistant is "Comms".
 *
 * Stations come from /sdcard/radio.txt as "name|url" lines, so they can be
 * changed without a reflash. Every URL must be plain HTTP and MP3 - see the
 * notes at the top of radio.c for why both matter. */

#define RADIO_MAX 16

void        radio_init(void);
void        radio_load_stations(void);   /* re-read the card */
int         radio_count(void);
const char *radio_name(int i);
int         radio_current(void);         /* -1 when nothing selected */
bool        radio_playing(void);
/* Short human-readable state for the UI: connecting / buffering / playing /
 * an error. Never NULL. */
const char *radio_status(void);

bool        radio_play(int idx);
void        radio_stop(void);
