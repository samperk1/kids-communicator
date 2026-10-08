#pragma once

/* Flamingo — tap-to-fly side scroller.
 *
 * Solo game: no ESP-NOW, no GAME_* id, no board[64], and like everything in
 * games.h it MUST NOT open WiFi — internal RAM sits on the ~44KB threshold
 * whenever the audio path is open, and a game over music is exactly that case.
 *
 * Right now this is STEP 1 of the plan: the tap-rate spike, not the game. The
 * whole control scheme rests on the panel being able to report ~5 taps/second,
 * and that is measured, not assumed, before any of the rest is built. */
void flamingo_show(void);
