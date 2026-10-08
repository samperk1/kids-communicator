#pragma once
#include "espnow.h"

/* Tanks — turn-based artillery.
 *
 * STEP 1 of that plan: hot-seat on ONE handset. No ESP-NOW, no GAME_* id, no
 * board[64] — the radio comes later and proves nothing until the physics, the
 * craters, the fall and the end rule are right.
 *
 * 🔑 EVERYTHING HERE IS INTEGER, DELIBERATELY. Two handsets will eventually
 * simulate the same shot from the same seed and must agree bit for bit, so the
 * single-player build is written to that rule from the start rather than
 * retrofitted. Fixed point is 1/16 px (FP_ONE below); the trig is a table.
 *
 * ⚠️ Like everything in games.h this MUST NOT open WiFi — internal RAM sits near
 * the ~44KB threshold whenever the audio path is open, and a game over music is
 * exactly that case.
 */
void tanks_show(void);

/* Handed a board addressed to GAME_TANKS by games_poll_net(). `peer_idx` is who
 * sent it, or -1 if they have since aged out of the peer list.
 *
 * ⚠️ Unlike the other games this IGNORES a frame from anybody but the handset
 * the player picked in the shop. The others have no picker and so must play
 * whoever is sending; tanks has one, and an explicit choice outranks a stray
 * frame from a board left on a shelf. */
void tanks_net_take(const espnow_game_t *g, int peer_idx);

/* 🚨 Re-announce the board every couple of seconds. Tanks was the ONE game
 * without this and it is why a game between two handsets could not be
 * finished: a board is never retried, so losing the single frame that says
 * "your turn" left both ends waiting forever. Called from games_poll_net()
 * beside ck_heartbeat() and the rest. */
void tanks_heartbeat(void);

/* True while a tanks game is live on the radio — games_net_active() folds this
 * in so telemetry does not take the radio away mid-game. */
bool tanks_net_active(void);
