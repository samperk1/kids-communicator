/* ESP-NOW messaging for the S3 handsets — the transport the CYD already runs.
 *
 * 🔑 WIRE-COMPATIBLE WITH espnow-link/src/msg_core.cpp, BYTE FOR BYTE: magic
 * "MSG2", a 28-byte header, ids that are the last two MAC bytes. That firmware
 * is the reference implementation and it is already proven on hardware —
 * app-level acks, retry, park/store-and-forward and dedupe all measured. This
 * is a port of the transport, not a second design.
 *
 * WHY, in one line: the S3s text each other through a shared
 * `input_text.kids_message` helper in Home Assistant, so text needs the house
 * wifi, needs HA up, has no addressing (one string for everyone) and cannot
 * reach the CYD at all. ESP-NOW is peer-to-peer, addressed, acked, and works
 * with no AP anywhere in sight.
 *
 * ⚠️ It does NOT replace the HA path. That is what lets the builder text the kids from
 * his phone, and it keeps working alongside this.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Starts the radio (taking a net_radio_hold), pins the channel and registers
 * with ESP-NOW. False if the driver would not start — the handset still works,
 * it just has no peer-to-peer messaging. */
bool espnow_begin(void);
bool espnow_running(void);

/* Rename this handset ON THE AIR, without a reboot.
 *
 * 🚨 THE BEACON CARRIED A NAME THAT WAS READ ONCE, AT BOOT. espnow_begin()
 * copied settings_name() into a static and nothing ever refreshed it, so a
 * child who renamed the handset in the menu kept announcing the OLD name to
 * every sibling until the board was power-cycled. The menu row even admitted
 * it — "reboot to rename on the walkie" — which is a bug wearing a label.
 *
 * 🔑 IT MATTERS MORE NOW THAT THE NAMES ARE PEOPLE. The handsets move between
 * the kids, so renaming is a THING THAT HAPPENS DAILY, not a one-off at build
 * time. A rename that needs a reboot is a rename that will not happen, and the
 * in-range list is only worth having if the names in it are true.
 *
 * Safe to call before espnow_begin(); the name is picked up either way. */
void espnow_set_name(const char *n);

/* Called by net.c after an association drops, to put the ESP-NOW channel back:
 * associating forces the radio onto the AP's channel and leaving it does not
 * restore ours. */
void espnow_radio_back(void);

/* Stand the once-a-second presence beacon down. Receiving continues and queued
 * messages still go; only the periodic broadcast stops.
 *
 * 🚨 This exists for GPS. See the long note in espnow.c: a 1 Hz transmission
 * lands inside every 6-second GPS navigation subframe, so the receiver can
 * never assemble the 18 seconds of clean data an ephemeris needs — while C/N0
 * and the tracked-satellite count both stay healthy and hide it completely. */
void espnow_beacon_quiet(bool on);
bool espnow_beacon_quiet_get(void);

/* Who is in range. Peers are learned from beacons; nothing is configured. */
/* ---- text between handsets, with NO router and NO Home Assistant ----------
 *
 * 🚨 THIS IS WHAT THE HANDSETS ARE FOR, and until now it did not exist on the
 * S3. The Talkie is UDP over wifi and the T9 screen posted to Home Assistant —
 * both need the house AP — while ESP-NOW carried presence beacons and nothing
 * else. Away from home the boards could see each other and had no way to say
 * anything. The builder found that out in a shop, twice.
 *
 * The protocol is the CYD's, byte for byte (espnow-link/src/msg_core.cpp):
 * acked, retried, and deduped by (originator, msg_id). */

#define MSG_TEXT_MAX     120
#define ESPNOW_INBOX_N   12

typedef struct {
    char    from[10];
    char    text[MSG_TEXT_MAX];
    int8_t  rssi;
    int64_t when_us;
    /* 🔑 THE SENDER'S ID, SO A REPLY CAN GO BACK TO THEM ALONE. The name is for
     * the screen and is not unique enough to address with — ids are. Without
     * this, "Coming" tapped on a popup could only be broadcast, which is how
     * a private message ends up answered in public. */
    uint16_t from_id;
    /* Addressed to this handset only, rather than to everyone in range. */
    bool     priv;
} espnow_msg_t;

/* peer_idx from the peer list, or -1 to send to everyone in range.
 *
 * 🚨 A PRIVATE MESSAGE MUST NEVER FALL BACK TO A SHARED CHANNEL. The caller,
 * not this function, owns that rule — see reply_task() in ui.c. */
bool espnow_msg_send(int peer_idx, const char *text);

/* Newest first. */
int                 espnow_inbox_count(void);
const espnow_msg_t *espnow_inbox(int i);
void                espnow_inbox_clear(void);

/* True once if something arrived since the last call — so a screen can repaint
 * without polling the whole list every tick. */
bool espnow_inbox_take_dirty(void);

/* Delivery state in words a child can read: "sending...", "delivered",
 * "not delivered". */
void espnow_msg_on_status(void (*cb)(const char *));

/* ---- voice, handset to handset, with NO router ----------------------------
 *
 * 🚨 THE FEATURE THE OFF-GRID TEST WENT LOOKING FOR AND DID NOT FIND. The
 * Talkie is UDP over the house AP, so five feet from a sibling in a car park it
 * said "no wifi" — while text and games over ESP-NOW worked fine between the
 * very same handsets. This is voice on the same peer-to-peer radio.
 *
 * Frames are voice_wire.h's vox_hdr_t + ADPCM payload, 172 bytes at most, one
 * 20ms frame per packet with no fragmentation. wt.c owns the codec and the
 * audio; this is only the pipe. */

/* 🔑 UNICAST TO EACH PEER, FOR THE 802.11 RETRIES.
 *
 * This began as a broadcast, reasoning that a late voice frame is useless so
 * retries buy nothing. Wrong about the mechanism: ESP-NOW broadcast gets no
 * retry at all, and a unicast retry at 1 Mbps finishes inside the same 20ms
 * frame — so it arrives in time to be played. The builder heard the difference as
 * cut-out indoors on 2026-08-26.
 *
 * Kept for the record, because the reasoning still applies at scale:
 *
 * espnow_msg_send() unicasts and waits for an application ack, because a text
 * that arrives late is still worth having and one that vanishes is not. Voice
 * is the other way round: a 20ms frame that arrives late is USELESS (it would
 * have to be played out of order or thrown away), and one that vanishes costs
 * 20ms of audio nobody will notice.
 *
 * ⚠️ wt.h argues the reverse for UDP — "unicast to discovered peers, NOT
 * multicast" — and that reasoning does NOT carry over. It is about an ACCESS
 * POINT relaying multicast at the lowest basic rate, unacknowledged, because
 * there is no single station to ack. ESP-NOW broadcast has no AP in the path at
 * all. What it does share is the lack of retries, which is exactly what voice
 * wants.
 *
 * And it is what makes the arithmetic work: one broadcast is 8.4 KB/s no matter
 * how many handsets are listening. Unicasting to three peers would be 25 KB/s,
 * which IS the measured ceiling of this link. */
/* to_id 0 = everyone in range, exactly as before. Otherwise the ONE handset
 * this frame is for.
 *
 * 🔑 NOTHING ON THE WIRE CHANGED TO MAKE THIS WORK. The send was already a
 * unicast per peer (see the note below), so talking to one person is simply not
 * looping — no to_id field, no VOX_MAGIC bump, and tools/wt.py and the CYD's
 * copy of voice_wire.h stay valid byte for byte.
 *
 * Returns false if to_id is not in range, so the caller can say so rather than
 * transmitting into nothing. */
bool espnow_voice_send(const void *frame, int len, uint16_t to_id);

/* This handset's own device id — the last two bytes of its MAC, the same id the
 * game protocol addresses by. Senders stamp it into vox_hdr_t.from_id so a
 * receiver can drop its own broadcast coming back to it. */
uint16_t espnow_self_id(void);

/* Re-apply the PHY rate (see settings_wt_range) to broadcast and every known
 * peer, so a change takes effect without a reboot. */
void espnow_apply_rate(void);

/* Move ESP-NOW to channel 1, 6 or 11 and remember it. Takes effect at once.
 * ⚠️ Handsets on different channels cannot hear each other at all — change the
 * whole fleet, not one handset. The recovery sweep will stop a straggler being
 * lost, but it will not migrate the fleet for you. */
void espnow_set_channel(int ch);

/* Drain one received voice frame into `out`. Returns its length, or 0 if none
 * is waiting. Called from the walkie task; frames are filled by the WiFi task,
 * so this is the hand-off between them. */
int  espnow_voice_take(void *out, int max);

/* Overruns since boot — frames dropped because the walkie task was not
 * draining fast enough. Should be 0; anything else means the audio path is
 * being starved and is worth a log line rather than a silent dropout. */
uint32_t espnow_voice_overruns(void);

/* True while a voice conversation is live in earshot — a VOX frame arrived in
 * the last few seconds. The telemetry loop uses it to stay off the AP; see the
 * note over the implementation. */
bool espnow_voice_recent(void);

/* ---- games, handset against handset --------------------------------------
 *
 * 🔑 THE WHOLE BOARD TRAVELS, NEVER THE MOVE. A board is tens of bytes and a
 * move is two, but a lost move desynchronises the two handsets permanently and
 * a lost board costs nothing — the next one repairs it. Idempotent beats
 * clever at this size, and out of range of any router there is nobody to ask
 * for a resend.
 *
 * `seq` rises with every move so a frame that arrives late cannot undo a newer
 * one. `turn` carries whose move it is next, so neither side has to infer it. */

#define GAME_BOARD_MAX 64

enum { GAME_TTT = 0, GAME_C4 = 1, GAME_DOTS = 2, GAME_BATTLE = 3,
       GAME_HEARTS = 4, GAME_UNO = 5, GAME_CHECKERS = 6, GAME_TANKS = 7 };
enum { GF_NEW = 0x01 };          /* start over; clears the far end's board */

typedef struct __attribute__((packed)) {
    uint8_t  game;
    uint8_t  seq;
    uint8_t  turn;               /* on the wire for readability; nothing reads it */
    uint8_t  flags;
    uint8_t  cells;
    /* 🔑 WHICH GAME, NOT JUST WHICH BOARD. Marks only ever go up WITHIN a game,
     * so mark count orders boards — but a new game has FEWER marks than the one
     * it replaces, and without a generation the two rules fight: a fresh empty
     * board looks stale next to the finished one, and the finished one looks
     * newer than the fresh one. Higher gen always wins; within a gen, more
     * marks wins. */
    uint8_t  gen;
    /* Who moves first this game, by device id. Carried rather than worked out
     * separately at each end: the loser starts the next game, and if the two
     * sides ever disagreed about who lost they would disagree about who is X. */
    uint16_t starter;
    uint8_t  board[GAME_BOARD_MAX];
} espnow_game_t;

bool espnow_game_send(int peer_idx, const espnow_game_t *g);

/* Poll from the UI timer. True once per received frame; `from` gets the
 * sender's name and `peer_idx` its position in the peer list, or -1 if it has
 * since aged out.
 *
 * 🔑 THE INDEX IS THE POINT. A game must be played against whoever is actually
 * sending boards, not against whatever happens to sit first in the peer list —
 * an idle handset on a shelf is still a peer. */
bool espnow_game_take(espnow_game_t *g, char *from, size_t from_n, int *peer_idx);

/* Remove a peer's key AND its encrypted entry in the esp_now driver. Use this
 * rather than fleet_remove() alone — see the note in espnow.c. Needed to repair
 * a HALF pairing, where this end kept a key the other end has forgotten. */
bool        espnow_unpair(uint16_t id);

int         espnow_peer_count(void);
const char *espnow_peer_name(int i);
/* The two ids are what decide who plays first in a game: lower id is X. Both
 * handsets compute the same answer from the same two numbers, so there is no
 * negotiation to race. */
uint16_t    espnow_peer_id(int i);
uint16_t    espnow_my_id(void);
int8_t      espnow_peer_rssi(int i);

/* ---------------- pairing ------------------------------------------------
 *
 * Hands the peer a RANDOM link key, wrapped with a key derived from this
 * handset's PMK — so only a handset holding the same fleet code can unwrap it.
 * Both ends switch to an encrypted peer; the reply arrives encrypted, which is
 * what proves the link rather than merely that the offer was received.
 *
 * ⚠️ If the offer is lost the two ends can disagree: this handset holds a key,
 * the other does not, and neither can reach the other. Remove on the Fleet
 * screen and pair again is the cure. */
bool espnow_pair_with(int peer_idx);

/* ---- gateway (see gw_wire.h) ----
 *
 * The gateway holds the WiFi credentials and the Home Assistant token; the
 * handsets hold neither and ASK it. Possession of the board is the permission.
 *
 * espnow_set_gateway() is called by the gateway build only — the same espnow.c
 * compiles for both, so the role is a runtime call and not an #ifdef. */
void     espnow_set_gateway(bool on);

/* Gateway build only: publish whether HA is answering, so handsets can prefer a
 * gateway that WORKS over one that is merely closer. */
void     espnow_set_gateway_ha_ok(bool ok);

/* Which gateway a handset would use right now. Honours settings_ask_gw_id()
 * when that gateway is in range, otherwise chooses: one that says it can reach
 * HA beats one that is merely closer. */
bool     espnow_gateway_ha_ok(uint16_t id);

/* 0 if none is in range. Ask each time rather than caching: which gateway is
 * reachable can change, and there may be more than one later. */
uint16_t espnow_gateway_id(void);
bool     espnow_peer_is_gateway(int i);

/* ------------------------------------------------------------------ *
 * PLAYABLE peers — the ones a person is sitting behind.
 *
 * 🚨 A GATEWAY IS A PEER BUT IT IS NOT A PLAYER. It beacons like everything
 * else, so from 2026-09-07 it started appearing in every game picker and, worse,
 * in Uno's roster — which seats whoever is in range and then waits for that seat
 * to move. The builder could not play Uno with the kids because the game was waiting for
 * `GW-BBBB` to take its turn, and it never will.
 *
 * So anything that means "another person" must count playable peers, and
 * anything that means "who is on the radio" (the Fleet screen, where you PAIR a
 * gateway) must keep using espnow_peer_count().
 *
 * ⚠️ THESE RETURN TRUE PEER INDICES, THEY DO NOT RENUMBER. ui.c already carries
 * a warning about a remembered index addressing whoever took its slot; handing
 * out a dense 0..n-1 index that means something different from the real one
 * would manufacture exactly that bug. Walk with espnow_play_idx(). */
int      espnow_play_count(void);
int      espnow_play_idx(int n);   /* true peer index of the n-th playable, or -1 */

/* Unicast to a NAMED peer. There is deliberately no broadcast form — two
 * gateways in earshot must never both answer the same question. Returns false
 * if that id is not currently in range. */
bool espnow_gw_request(uint16_t to_id, const void *payload, int len);
bool espnow_gw_respond(uint16_t to_id, const void *payload, int len);

void espnow_gw_on_request(void (*cb)(uint16_t from_id, const uint8_t *p, int len));
void espnow_gw_on_response(void (*cb)(uint16_t from_id, const uint8_t *p, int len));
