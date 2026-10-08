#pragma once
#include <stdbool.h>

/* Games. Reached ONLY from the Now Playing screen's middle button, and every
 * Back lands there again — the point of them is something to do while a track
 * plays on a car ride, so nothing here ever touches the player or the radio.
 *
 * 🚨 NOTHING IN HERE MAY OPEN WIFI. Internal RAM sits on the ~44KB WiFi
 * threshold whenever the audio path is open, and a game running over music is
 * exactly that case. Games are offline by construction. */
void games_show_menu(void);

/* Called from the UI tick. Picks up a board sent by another handset — even
 * while the player is on a different screen, so opening the game shows the
 * game rather than an empty grid. */
void games_poll_net(void);

/* True while a networked game is live. Anything that would take the radio away
 * from ESP-NOW must check this: a lost board is not retried, by design. */
bool games_net_active(void);
