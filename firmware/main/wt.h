#pragma once
#include <stdbool.h>

/* Walkie-talkie: a shared channel, not a call.
 *
 * Press to talk, everyone on the channel hears it. No dialling, no answering,
 * no busy — the model a child already understands from the toy.
 *
 * ⚠️ The wire protocol MUST stay in step with tools/wt.py, which is the second
 * handset until a second handset exists. Any change here is a change there.
 *
 * 🚨 UNICAST TO DISCOVERED PEERS, NOT MULTICAST. Multicast looks like the
 * obvious fit for a shared channel and is a trap on WiFi: APs send multicast
 * and broadcast at the lowest basic rate, unacknowledged and unretried, because
 * there is no single station to ACK them — which is exactly the dropout
 * behaviour a walkie-talkie cannot tolerate. With a handful of handsets,
 * sending each 640-byte frame as unicast is cheap and gets the full 802.11
 * retry machinery. Only the 2s discovery beacon is broadcast, where a loss
 * costs nothing.
 */

#define WT_PORT        5005
#define WT_RATE        16000          /* same as the mic path — no conversion  */
#define WT_FRAME_MS    20
#define WT_FRAME_SAMP  (WT_RATE * WT_FRAME_MS / 1000)   /* 320 */
#define WT_FRAME_BYTES (WT_FRAME_SAMP * 2)              /* 640, under any MTU */
#define WT_MAX_PEERS   6

typedef enum {
    WT_OFF = 0,     /* radio down. best battery, hears nothing               */
    WT_OPEN,        /* radio up continuously. instant, worst battery         */
} wt_mode_t;

void wt_init(void);

/* Open/close the channel. wt_open() brings WiFi up and moves the codec to
 * 16kHz; wt_close() puts both back. */
void wt_open(void);
void wt_close(void);

void wt_ptt(bool down);          /* from the LVGL button. Non-blocking.      */

/* Rename the id this handset announces on the channel, without a reboot.
 * wt_init() read settings_name() ONCE at start-up, so a rename in the menu did
 * not reach the far end's "X is talking" until the board was power-cycled.
 * Pairs with espnow_set_name() — the two transports must never disagree about
 * who this handset is. */
void wt_set_name(const char *n);

/* 🔑 WHO THIS HANDSET IS TALKING TO. 0 = everyone on the channel, which is the
 * default and the behaviour the toy has always had. Otherwise one peer id.
 *
 * The kids asked for this: they wanted to talk one to one without everybody
 * else having to listen. An ID, not an index — see s_reply_to in ui.c for why
 * an index remembered across a change in the peer list addresses the wrong
 * person.
 *
 * ⚠️ ESP-NOW ONLY. The UDP path is a shared channel by construction; auto
 * always picks ESP-NOW, so this is the path that matters. */
void        wt_set_target(uint16_t id);
uint16_t    wt_target(void);

wt_mode_t wt_mode(void);
bool        wt_talking(void);    /* we are transmitting                      */
const char *wt_hearing(void);    /* who is transmitting to us, or ""         */
int         wt_peer_count(void);
const char *wt_status(void);     /* one short line for the screen            */
