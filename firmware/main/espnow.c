/* ESP-NOW transport — see espnow.h. Ported from the CYD's msg_core.cpp.
 *
 * Milestone 1: presence. Beacons out, peers in, RSSI logged. Sending messages
 * comes next and reuses this frame, this peer table and this callback.
 *
 * ⚠️ NOTHING HEAVY RUNS IN THE RECEIVE CALLBACK. It runs in the WiFi task and
 * blocking it stalls the radio. Frames are copied into a queue and handled in a
 * task of our own — the same rule the CYD firmware follows.
 */

#include <string.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_idf_version.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "nvs.h"
#include "applog.h"
#include "net.h"
#include "espnow.h"
#include "esp_random.h"
#include "fleet.h"
#include "voice_wire.h"
#include "gw_wire.h"
#include "settings.h"
#include "player.h"
#include "esp_rom_sys.h"

/* "MSG2" — deliberately not the link tester's "LNK1", so the two firmwares are
 * mutually invisible and neither can pollute the other's test. */
#define MSG_MAGIC     0x4D534732
#define ID_BROADCAST  0xFFFF
#define PAYLOAD_MAX   1400          /* ESP-NOW v2 allows 1470 total */

enum {
    T_BEACON = 0, T_MSG = 1, T_ACK = 2, T_CFG = 3, T_PING = 4, T_RATE = 5,
    T_GAME = 6, T_PAIR = 7, T_PAIR_ACK = 8,
    /* Gateway request/response — see gw_wire.h. Addressed by to_id, never
     * broadcast, so a second gateway is a configuration change not a rewrite. */
    T_GW_REQ = 9, T_GW_RSP = 10,
};

/* The pairing payload: a random link key, wrapped. 44 bytes, well inside the
 * frame. The key itself is NEVER on the wire in the clear. */
typedef struct __attribute__((packed)) {
    uint8_t nonce[12];
    uint8_t ct[FLEET_KEY_LEN];
    uint8_t tag[16];
} pair_payload_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  type;
    uint8_t  hop;
    uint8_t  ttl;
    uint8_t  flags;
    uint32_t msg_id;
    uint16_t from_id;
    uint16_t to_id;
    uint16_t payload_len;
    char     name[10];
    uint8_t  payload[PAYLOAD_MAX];
} msg_frame_t;

#define MSG_HDR  (offsetof(msg_frame_t, payload))     /* 28 */

#define BEACON_MS       1000
#define PEER_STALE_MS   8000
/* 🔑 NO LONGER A CONSTANT, AND NO LONGER THE AP'S CHANNEL BY DEFINITION.
 *
 * It was 6 because that is the house AP's channel, so associating never dragged
 * the radio off ESP-NOW's. A survey on 2026-08-26 showed what that costs: SIX
 * access points on channel 6, all of them the builder's own — three mesh nodes each
 * beaconing the house SSID and the IoT one, the strongest at signal 87 — and every one of
 * them competing with the kids' voice. Channels 1 and 11 were completely empty,
 * and with the country code fixed at `01` (ch 1-11) that makes 11 the only
 * channel with nothing inside the 22MHz it overlaps.
 *
 * ⚠️ THE TRADE, SAID OUT LOUD: off the AP's channel, an ASSOCIATED handset is
 * deaf to ESP-NOW until it comes back. That was unacceptable while the walkie
 * needed the AP; it is affordable now that telemetry stands down for a
 * conversation, text retries and store-and-forwards, and a game re-announces
 * every 2s. */
#define ESPNOW_CHANNEL  (settings_espnow_channel())

static const uint8_t BCAST[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };

typedef struct {
    bool     used;
    uint8_t  mac[6];
    uint16_t id;
    char     name[10];
    int8_t   rssi;
    int64_t  last_heard;
    bool     ha_ok;      /* gateway only: it says HA is answering it */
    /* Set from the beacon's F_IS_GATEWAY flag. A handset finds gateways this
     * way rather than by a compiled-in address, so replacing the board does not
     * mean reflashing five handsets. */
    bool     is_gateway;
} peer_t;

/* 🔑 A RUNTIME FLAG, NOT A COMPILE SWITCH. The gateway compiles THIS FILE —
 * see comm-gateway/main/CMakeLists.txt — so the fleet has one implementation of
 * the wire format and the crypto. Which role a board plays is therefore a call
 * at startup, not a #ifdef, and there is no second code path to drift. */
static bool        s_is_gateway;
static void (*s_gw_req_cb)(uint16_t from_id, const uint8_t *p, int len);
static void (*s_gw_rsp_cb)(uint16_t from_id, const uint8_t *p, int len);

static peer_t      s_peers[8];
static uint8_t     s_mac[6];
static uint16_t    s_id;
static char        s_name[10];
static bool        s_running;
static uint32_t    s_beacon_seq;

/* 🚨 RADIO QUIET, FOR GPS. Measured 2026-09-12 on the test handset: this beacon goes out
 * every BEACON_MS = 1000ms, relentlessly, in every session. A GPS navigation
 * SUBFRAME IS 6 SECONDS LONG and a receiver needs three consecutive clean ones
 * — 18 seconds — to get the ephemeris without which it can never fix. At one
 * transmission per second EVERY subframe spans about six transmit bursts, so
 * not one of them can ever complete.
 *
 * 🔑 And the symptom is a liar: C/N0 stays high (a 1ms burst barely moves a 1Hz
 * average) and satellites keep being TRACKED, so every screen and every log
 * says the signal is fine. The handset tracked 25 satellites at 38 dB for 218
 * UNBROKEN SECONDS and still never marked a single one as USED. Three separate
 * theories — "needs more time", "the antenna", "the ground plane" — were all
 * argued from C/N0, which is exactly the metric blind to this.
 *
 * So the GPS screen stands the beacon down while it acquires. This does NOT
 * stop receiving, and it does not tear the radio down: peers keep hearing us
 * go quiet, which they already handle as ordinary absence. */
static volatile bool s_beacon_quiet;
static QueueHandle_t s_rx_q;

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    /* 🔑 How the frame was delivered: true = addressed to OUR unicast MAC,
     * false = broadcast. ESP-NOW encrypts only unicast to a locked peer, so
     * this is half of the "is it authentic?" question the T7 fix asks — a
     * broadcast claiming to be a fleet member cannot have been encrypted. */
    bool     unicast;
    uint16_t len;
    /* 🚨 MUST HOLD A WHOLE TEXT MESSAGE, not just a beacon. This was
     * MSG_HDR + 64 "presence frames only, for now", and the "for now" outlived
     * the comment: a 120-character message would have been silently clipped. */
    uint8_t  buf[MSG_HDR + MSG_TEXT_MAX];   /* also holds a game board */
} rx_evt_t;

/* ---------------------------------------------------------------- peers */

static peer_t *peer_by_mac(const uint8_t *mac)
{
    for (int i = 0; i < 8; i++)
        if (s_peers[i].used && !memcmp(s_peers[i].mac, mac, 6)) return &s_peers[i];
    return NULL;
}

/* 🔑 1 Mbps, BECAUSE RANGE IS WORTH MORE HERE THAN SPEED.
 *
 * ESP-NOW otherwise sends at whatever rate the station is using, which indoors
 * is a fast, fragile one. The builder, 2026-08-26, with the kids actually using them:
 * "we are getting more cut out then i was expecting ... the esp now is a bit
 * week but we are in doors".
 *
 * Everything this project puts on the air is tiny — voice is the biggest at
 * 67 kbps — so the speed is pure headroom and dropping it buys roughly 10-13 dB
 * of link budget, which is walls.
 *
 * ⚠️ AIRTIME IS THE THING TO WATCH, NOT BANDWIDTH. At 1 Mbps with a long
 * preamble one 172-byte voice frame is ~1.6ms on the air (192us preamble alone),
 * plus the ack. Unicasting to three listeners every 20ms is ~27% airtime — fine,
 * but not free, and it is shared with the house AP on the same channel.
 *
 * ⚠️ DELIBERATELY NOT APPLIED TO THE BT DONGLE. That peer carries MP3 at
 * ~200 kbps and is the one path here where throughput actually matters; leave
 * it on the fast rate it was measured at. */
static void peer_set_rate(const uint8_t *mac)
{
    const bool lr = settings_wt_range() != 0;

    /* 🛰 LR IS 500K, NOT 250K, AND AIRTIME IS WHY. LR trades bytes-per-second
     * for link budget, so a frame sits on the air far longer: 172 bytes at
     * 250 kbps is ~5.5ms, and unicasting that to three listeners every 20ms
     * would be 82% airtime — not viable. At 500 kbps it is ~2.75ms and three
     * listeners is 41%, which fits. 250K stays available if range still is not
     * enough and the fleet drops back to broadcast to pay for it. */
    esp_now_rate_config_t rc = {
        .phymode = lr ? WIFI_PHY_MODE_LR      : WIFI_PHY_MODE_11B,
        .rate    = lr ? WIFI_PHY_RATE_LORA_500K : WIFI_PHY_RATE_1M_L,
        .ersu    = false,
        .dcm     = false,
    };
    esp_err_t e = esp_now_set_peer_rate_config(mac, &rc);
    if (e != ESP_OK)
        applog("[now] rate config failed: %s", esp_err_to_name(e));
}

/* Re-apply the current rate to broadcast and every known peer. Called when the
 * range setting changes, so a menu tap takes effect without a reboot — peers
 * were rate-configured when they were LEARNED, which may have been minutes ago
 * under the old setting. */
static peer_t *peer_at(int idx);   /* defined below; needed by the gateway API */

/* ---- gateway role and addressing (see gw_wire.h) ---------------------- */

void espnow_set_gateway(bool on) { s_is_gateway = on; }

/* Gateway build only: whether HA answered us last time we looked. */
static bool s_gw_ha_ok;
void espnow_set_gateway_ha_ok(bool ok) { s_gw_ha_ok = ok; }

void espnow_gw_on_request(void (*cb)(uint16_t, const uint8_t *, int))
{ s_gw_req_cb = cb; }
void espnow_gw_on_response(void (*cb)(uint16_t, const uint8_t *, int))
{ s_gw_rsp_cb = cb; }

/* The id of the BEST gateway currently in range, or 0.
 *
 * 🔑 STRONGEST SIGNAL WINS, NOT FIRST IN THE TABLE. There are two gateways as
 * of 2026-09-07 — one by the bench, one toward Kid 4's end of the house, where
 * the handsets measured -83..-89 dBm. Taking whichever happened to be found
 * first would send a handset standing next to one gateway across the house to
 * the other, over the exact weak link the second board exists to avoid.
 *
 * ⚠️ RETURNS ONE, AND THE CALLER MUST NOT CACHE IT. Which gateway is nearest
 * changes as a child walks around; the peer table is the authority, not a
 * remembered number. Cheap enough to ask every time — it is eight comparisons.
 *
 * ⚠️ rssi is 0 until the first frame after discovery fills it in, and 0 would
 * otherwise look like the strongest possible signal. An unmeasured peer is
 * therefore treated as the weakest, not the best. */
/* 🔑 READ THE SETTING, DO NOT MIRROR IT. A local copy would need somebody to
 * remember to push every change into it — see "derive dont negotiate",
 * and the stale-derived-identity bugs this project has already paid for.
 * settings_ask_gw_id() is the single source; 0 means choose automatically. */

/* 🔑 REACHABILITY BEATS SIGNAL. With two gateways, the nearer one is not
 * necessarily the working one — and a gateway with a dead wifi link looks
 * identical from here except for F_GW_HA_OK. Prefer a gateway that says it can
 * reach HA; only fall back to raw RSSI when none of them do (which is also the
 * honest behaviour against an older gateway that never sets the bit).
 *
 * A PIN wins over both, because the person holding it may know something the
 * radio does not. It is ignored when that gateway is not in range, so pinning
 * one and walking away from it does not disable the feature. */
uint16_t espnow_gateway_id(void) { return espnow_gateway_id_except(0); }

uint16_t espnow_gateway_id_except(uint16_t skip)
{
    uint16_t pin = settings_ask_gw_id();
    if (pin && pin != skip) {
        for (int i = 0; i < 8; i++)
            if (s_peers[i].used && s_peers[i].is_gateway && s_peers[i].id == pin)
                return pin;
    }

    uint16_t best_id = 0;
    int      best_rssi = -128;
    bool     best_ha = false;

    for (int i = 0; i < 8; i++) {
        if (!s_peers[i].used || !s_peers[i].is_gateway) continue;
        if (skip && s_peers[i].id == skip) continue;
        int  r  = s_peers[i].rssi ? s_peers[i].rssi : -127;
        bool ha = s_peers[i].ha_ok;
        if (!best_id || (ha && !best_ha) || (ha == best_ha && r > best_rssi)) {
            best_id = s_peers[i].id; best_rssi = r; best_ha = ha;
        }
    }
    return best_id;
}

bool espnow_gateway_ha_ok(uint16_t id)
{
    for (int i = 0; i < 8; i++)
        if (s_peers[i].used && s_peers[i].is_gateway && s_peers[i].id == id)
            return s_peers[i].ha_ok;
    return false;
}

bool espnow_peer_is_gateway(int i)
{
    peer_t *p = peer_at(i);
    return p && p->is_gateway;
}

/* Playable peers: everything in range that is not a gateway. See espnow.h for
 * why this exists and why it hands back TRUE indices instead of renumbering. */
int espnow_play_count(void)
{
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (s_peers[i].used && !s_peers[i].is_gateway) n++;
    return n;
}

int espnow_play_idx(int n)
{
    if (n < 0) return -1;
    /* Walks the same peer_at() ordering the rest of the API exposes, so the
     * index handed back is the one espnow_peer_id()/name() will agree with. */
    int cnt = espnow_peer_count();
    for (int i = 0; i < cnt; i++)
        if (!espnow_peer_is_gateway(i) && n-- == 0) return i;
    return -1;
}

/* Send a request to a NAMED gateway, or a response back to a named handset.
 * Unicast only — there is no broadcast form on purpose. */
static bool gw_send(uint16_t to_id, uint8_t type, const void *payload, int len)
{
    if (!s_running || len < 0 || len > 200) return false;
    for (int i = 0; i < 8; i++) {
        if (!s_peers[i].used || s_peers[i].id != to_id) continue;
        msg_frame_t f = { 0 };
        f.magic = MSG_MAGIC; f.type = type; f.ttl = 1;
        f.from_id = s_id; f.to_id = to_id;
        f.payload_len = (uint16_t)len;
        strlcpy(f.name, s_name, sizeof(f.name));
        if (len) memcpy(f.payload, payload, len);
        return esp_now_send(s_peers[i].mac, (uint8_t *)&f, MSG_HDR + len) == ESP_OK;
    }
    return false;   /* not in range: the caller decides whether to care */
}

bool espnow_gw_request(uint16_t to_id, const void *payload, int len)
{ return gw_send(to_id, T_GW_REQ, payload, len); }

bool espnow_gw_respond(uint16_t to_id, const void *payload, int len)
{ return gw_send(to_id, T_GW_RSP, payload, len); }

void espnow_apply_rate(void)
{
    if (!s_running) return;
    peer_set_rate(BCAST);
    for (int i = 0; i < 8; i++)
        if (s_peers[i].used) peer_set_rate(s_peers[i].mac);
    applog("[now] radio rate is now %s",
           settings_wt_range() ? "LONG RANGE (LR 500K)" : "1 Mbps");
}

/* A key upgrade waiting for its reply to actually leave the radio. Declared up
 * here rather than with the other pairing state below because peer_ensure()
 * has to consult it — see the note at the encrypt decision in peer_add(). */
static uint8_t s_upg_mac[6], s_upg_lmk[FLEET_KEY_LEN];
static int64_t s_upg_us;
#define UPG_DELAY_US 150000

static peer_t *peer_add(const uint8_t *mac, uint16_t id, const char *name)
{
    for (int i = 0; i < 8; i++) {
        if (s_peers[i].used) continue;
        peer_t *p = &s_peers[i];
        memset(p, 0, sizeof(*p));
        p->used = true;
        memcpy(p->mac, mac, 6);
        p->id = id;
        strlcpy(p->name, name, sizeof(p->name));
        /* ⚠️ ESP-NOW will not send to an address that is not a peer, even one
         * we have just heard from. Adding it here is what makes a reply
         * possible at all. channel 0 = "whatever channel we are on". */
        /* 🔑 THE ROSTER DECIDES THE KEY. It does NOT decide visibility.
         *
         * 🚨 An earlier version refused to add an unpaired peer while migration
         * was closed, and that made pairing IMPOSSIBLE: a handset you cannot
         * see is a handset you cannot tap to pair with, so a closed fleet could
         * never gain a member. Found on Parent, which showed nothing while
         * Kid 1 showed Parent.
         *
         * Discovery is free anyway — the beacon is broadcast and unencrypted,
         * so we hear it whatever we do; declining to record it only blinds us.
         * What migration actually gates is whether we ACT on what an unpaired
         * handset sends, and that belongs in handle(). */
        const fleet_peer_t *fp = fleet_find(mac);
        esp_now_peer_info_t pi = { 0 };
        memcpy(pi.peer_addr, mac, 6);
        pi.channel = 0;
        pi.encrypt = false;

        /* 🚨 NOT WHILE A PAIR REPLY IS STILL IN FLIGHT — THE SECOND HALF OF THE
         * 2026-09-06 SEND RACE.
         *
         * The T_PAIR handler already knows not to swap the key underneath its
         * own queued acknowledgement; that is what s_upg_* and UPG_DELAY_US are
         * for. But it calls fleet_mark_encrypted() IMMEDIATELY, and this
         * function reads the roster. So any re-add during those 150ms installs
         * the key here instead — and the deferred guard never gets a say.
         *
         * A re-add is not hypothetical: the 8s stale reap calls
         * esp_now_del_peer(), and the very next beacon lands back in here. The builder,
         * 2026-09-07, pairing Parent with Kid 1 — Kid 1's log flapping
         * "- Kid 1 (silent 8s)" / "+ Kid 1" throughout, Kid 1 reporting the
         * link paired and Parent reporting nothing. The plaintext ACK went out
         * encrypted with a key Parent could not have yet (Parent only installs it ON
         * that ACK), so it was lost and Parent never committed.
         *
         * ⚠️ The symptom is a ONE-WAY link that both ends describe differently,
         * which reads as a radio or range fault and is not one.
         */
        bool upg_pending = s_upg_us && !memcmp(s_upg_mac, mac, 6);

        if (fp && !upg_pending) {
            pi.encrypt = true;
            memcpy(pi.lmk, fp->lmk, FLEET_KEY_LEN);
        } else if (upg_pending) {
            applog("[now] %04X re-added in the clear - pair reply still in flight", id);
        }
        esp_err_t ae = esp_now_add_peer(&pi);
        if (ae != ESP_OK)
            applog("[now] add_peer %04X FAILED: %s", id, esp_err_to_name(ae));
        peer_set_rate(mac);
        applog("[now] + %s (%04X) rssi %d", p->name, p->id, p->rssi);
        return p;
    }
    return NULL;
}

/* ------------------------------------------------------------- callbacks */


/* 🎙 The voice hand-off: filled by the WiFi task in on_recv(), drained by the
 * walkie task. Single producer, single consumer, so the two indices need no
 * lock — aligned byte writes are atomic on this core and each index has exactly
 * one writer. 8 slots is 160ms of jitter buffer for ~1.4KB. */
#define VOX_RING 8
typedef struct { uint8_t buf[VOX_MAX]; uint16_t len; } vox_slot_t;
static vox_slot_t      s_vox[VOX_RING];
static volatile uint8_t s_vox_head, s_vox_tail;
static uint32_t        s_vox_over;
static volatile int64_t s_vox_last_us;   /* when a voice frame last arrived */

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    uint32_t magic;
    if (len < 4) return;
    memcpy(&magic, data, 4);


    /* 🎙 VOICE. Copied into a ring and handed to the walkie task, which owns
     * the codec and the audio device. Nothing is decoded here: this runs on the
     * WiFi task and must return immediately. */
    if (magic == VOX_MAGIC) {
        if (len < (int)VOX_HDR || len > VOX_MAX) return;
        vox_hdr_t h;
        memcpy(&h, data, VOX_HDR);
        /* Our own broadcast comes back to us. Playing it would be an echo into
         * a live mic — the one thing push-to-talk exists to prevent. */
        if (h.from_id == s_id) return;

        uint8_t next = (uint8_t)((s_vox_head + 1) % VOX_RING);
        if (next == s_vox_tail) {
            /* ⚠️ DROP THE NEWEST AND COUNT IT. The ring is 8 frames = 160ms; if
             * it fills, the walkie task is not draining at 50/s and the problem
             * is upstream of here. Dropping the OLDEST would be better for
             * latency but the tail belongs to the consumer and racing it to
             * save 20ms is not worth the bug. A non-zero counter is the signal
             * to come back to this. */
            s_vox_over++;
            return;
        }
        memcpy(s_vox[s_vox_head].buf, data, len);
        s_vox[s_vox_head].len = (uint16_t)len;
        s_vox_head = next;
        /* Stamped even when the ring is full above, would be better — but a
         * full ring means we ARE hearing a conversation, and the caller only
         * asks "recently?", so the distinction never changes an answer. */
        s_vox_last_us = esp_timer_get_time();
        return;
    }

    if (len < (int)MSG_HDR) return;
    if (magic != MSG_MAGIC) return;              /* not ours; ignore quietly */

    rx_evt_t e = { 0 };
    memcpy(e.mac, info->src_addr, 6);
    e.unicast = info->des_addr && memcmp(info->des_addr, BCAST, 6) != 0;
    e.rssi = info->rx_ctrl ? info->rx_ctrl->rssi : 0;
    e.len  = (uint16_t)(len > (int)sizeof(e.buf) ? (int)sizeof(e.buf) : len);
    memcpy(e.buf, data, e.len);
    /* From the WiFi task: never block here. A dropped frame is a dropped
     * beacon, which the next one replaces a second later. */
    xQueueSend(s_rx_q, &e, 0);
}

/* ------------------------------------------------------------------ *
 * TEXT OVER ESP-NOW — the thing the handsets are actually FOR
 *
 * 🚨 THIS WAS THE HOLE. The ESP-NOW port brought the transport and stopped:
 * T_MSG arrived and hit `default: not handled yet`, and espnow.h had no send
 * function at all. So `peers 2` meant "these boards can see each other's
 * beacons" and nothing more. Meanwhile the Talkie is UDP over wifi and the T9
 * screen posted to Home Assistant — BOTH need the house AP. The builder took all three
 * handsets to a store twice and nothing worked, which was correct behaviour for
 * what was built and not at all what anyone intended.
 *
 * The protocol is not invented here. It is the CYD's, in
 * espnow-link/src/msg_core.cpp, which has been proven on hardware: same MSG2
 * magic, same 28-byte header, same three rules —
 *
 *   1. A message WANTS AN ACK and is retried until it gets one.
 *   2. DEDUPE BY (originator, msg_id). Retries deliver the same message twice
 *      and a child must not see it twice.
 *   3. The id must be unique per originator, so it is persisted — a reboot
 *      that restarted at 1 would collide with everything already seen.
 * ------------------------------------------------------------------ */

#define F_WANT_ACK  0x01
/* 🔑 ADVERTISED, NOT CONFIGURED. Only the gateway build sets this, and it sets
 * it on every beacon, so a handset learns where Home Assistant lives simply by
 * hearing it. Nothing in the handset firmware names a gateway. */
#define F_IS_GATEWAY 0x02
/* 🔑 "AND I CAN ACTUALLY REACH HOME ASSISTANT."
 *
 * Being a gateway and being USEFUL are different things: a gateway whose wifi
 * has dropped still beacons, still answers pairing, and — until this bit — was
 * still chosen by espnow_gateway_id() if it happened to be the nearer of the
 * two. That is the same mistake as preferring the better radio over the
 * reliable one, which split the fleet once already.
 * Additive bit: an older build simply never sets it and is treated as unknown,
 * so this needs no version bump. */
#define F_GW_HA_OK   0x04

/* Defined below; the espnow task and the send path both reach back. */
static void outbox_pump(void);
static void peer_encrypt_now(const uint8_t *mac, const uint8_t *lmk);

/* One outstanding offer at a time — pairing is a deliberate parent action, not
 * something that happens in bulk. */
static uint8_t s_pend_mac[6], s_pend_lmk[FLEET_KEY_LEN], s_pend_tag[16];
static uint16_t s_pend_id;
static char    s_pend_name[10];
static int64_t s_pend_us;
#define PEND_WINDOW_US 5000000

static peer_t *peer_at(int idx);

static void (*s_status_cb)(const char *);

/* One slot: a newer board supersedes an older one, so queueing them would
 * only let the screen fall behind the game. */
static espnow_game_t s_game;
static char          s_game_from[10];
static uint16_t      s_game_from_id;
static volatile bool s_game_have;
void espnow_msg_on_status(void (*cb)(const char *)) { s_status_cb = cb; }

/* 🚨 PERSISTED. (from_id, msg_id) is the dedupe key, so a reboot that restarted
 * the counter at 1 would produce ids the far end has ALREADY SEEN and silently
 * drops — a handset that looks like it is sending and never arrives. Bumped in
 * blocks so a message does not cost an NVS write each time. */
#define ID_BLOCK 32
static uint32_t s_next_msg_id, s_id_ceiling;

static uint32_t next_msg_id(void)
{
    if (s_next_msg_id >= s_id_ceiling) {
        nvs_handle_t h;
        if (nvs_open("kcespnow", NVS_READWRITE, &h) == ESP_OK) {
            uint32_t base = 0;
            nvs_get_u32(h, "msgid", &base);
            if (base < s_next_msg_id) base = s_next_msg_id;
            s_next_msg_id = base;
            s_id_ceiling  = base + ID_BLOCK;
            nvs_set_u32(h, "msgid", s_id_ceiling);
            nvs_commit(h);
            nvs_close(h);
        } else {
            s_id_ceiling = s_next_msg_id + ID_BLOCK;   /* no NVS: still monotonic */
        }
    }
    return ++s_next_msg_id;
}

typedef struct {
    bool     used;
    uint16_t to_id;          /* ID_BROADCAST for "anyone" */
    uint32_t msg_id;
    uint8_t  tries;
    int64_t  next_us;
    uint16_t len;
    char     text[MSG_TEXT_MAX];
} out_entry_t;

#define OUTBOX_N   4
#define OUT_TRIES  6
#define OUT_GAP_MS 700
static out_entry_t s_outbox[OUTBOX_N];

typedef struct { uint16_t from_id; uint32_t msg_id; } seen_t;
static seen_t s_seen[24];
static int    s_seen_head;

/* Dedupe by (originator, msg_id). Retries and any future relay both deliver
 * the same message more than once; the child must see it once. */
static bool seen_before(uint16_t from_id, uint32_t msg_id)
{
    for (int i = 0; i < (int)(sizeof(s_seen) / sizeof(s_seen[0])); i++)
        if (s_seen[i].from_id == from_id && s_seen[i].msg_id == msg_id) return true;
    s_seen[s_seen_head].from_id = from_id;
    s_seen[s_seen_head].msg_id  = msg_id;
    s_seen_head = (s_seen_head + 1) % (int)(sizeof(s_seen) / sizeof(s_seen[0]));
    return false;
}

/* The inbox. Newest first, so the screen shows what just arrived without
 * scrolling. Kept in RAM: a message is a "come home" or a "where are you",
 * and one that outlives the trip has no value. */
static espnow_msg_t s_inbox[ESPNOW_INBOX_N];
static int          s_inbox_n;
static bool         s_inbox_dirty;

static void inbox_add(const char *from, uint16_t from_id, const char *text,
                      int8_t rssi, bool priv)
{
    if (s_inbox_n < ESPNOW_INBOX_N) s_inbox_n++;
    for (int i = s_inbox_n - 1; i > 0; i--) s_inbox[i] = s_inbox[i - 1];
    memset(&s_inbox[0], 0, sizeof(s_inbox[0]));
    strlcpy(s_inbox[0].from, from, sizeof(s_inbox[0].from));
    strlcpy(s_inbox[0].text, text, sizeof(s_inbox[0].text));
    s_inbox[0].rssi = rssi;
    s_inbox[0].when_us = esp_timer_get_time();
    s_inbox[0].from_id = from_id;
    s_inbox[0].priv    = priv;
    s_inbox_dirty = true;
    applog("[now] %s from %s (%04X): %s", priv ? "MSG (private)" : "MSG",
           from, (unsigned)from_id, text);
}

static void send_ack(const uint8_t *mac, uint16_t to_id, uint32_t msg_id)
{
    msg_frame_t f = { 0 };
    f.magic   = MSG_MAGIC;
    f.type    = T_ACK;
    f.from_id = s_id;
    f.to_id   = to_id;
    f.msg_id  = msg_id;
    strlcpy(f.name, s_name, sizeof(f.name));
    esp_now_send(mac, (uint8_t *)&f, MSG_HDR);
}

/* 🔑 T7 FIX, 2026-10-07 — content that CLAIMS a fleet member must have arrived
 * the way a real one must: as an ENCRYPTED UNICAST.
 *
 * T7 (the spoofing test) forged a T_MSG wearing a paired sibling's MAC and
 * found two ways past the roster gate above, which only checks fleet_find(mac):
 *   - BROADCAST: ESP-NOW has no key for the broadcast address, so a cleartext
 *     broadcast naming a sibling was shown as a private message from them.
 *   - A NOT-YET-LOCKED peer (lost pairing, drifted channel, fresh flash): added
 *     cleartext, so a forged cleartext unicast of its MAC was accepted too.
 * The stack already stops a forged unicast to a LOCKED peer (it decrypts with
 * the link key and a cleartext frame never reaches us), which is why those two
 * were all that got through. This closes them in the one place the app can:
 *
 *   accept only if the claimed sender is locked AND the frame came unicast.
 *
 * Only fleet MEMBERS are held to this. A non-member is the migration gate's
 * business — dropped when closed (the light/public build), and the full build's
 * migration path (stripped before public) is the only thing that still trusts
 * their cleartext. Real paired handsets always unicast to a known peer, so this
 * costs legitimate traffic nothing. */
static bool content_authentic(const rx_evt_t *e, bool peer_existed)
{
    fleet_peer_t *fp = fleet_find(e->mac);
    if (!fp) return true;              /* not a claimed member — not our call here */
    if (!fp->seen_encrypted) return false;  /* link not locked: cleartext is forgeable */
    if (!e->unicast) return false;     /* broadcast cannot be encrypted */
    /* 🚨 AND THE FRAME MUST NOT HAVE CREATED ITS OWN PEER.
     *
     * peer_add() installs encrypt=true for a roster member — but it runs WHILE
     * handling the frame that triggered it, so THAT frame was received before
     * the key existed and is therefore cleartext. Only frames that find the
     * peer already present were delivered through the encrypted L2 peer, which
     * drops any cleartext forgery before it reaches us.
     *
     * T7 proved why this matters: the attacker never beacons, so the stale
     * reap (8s of silence) deletes its peer every cycle and the next forged
     * frame re-creates it in the clear — accepted forever. A genuine sibling
     * beacons every second, so its encrypted peer is already present when its
     * text arrives; a dropped first text is retried (F_WANT_ACK) and lands once
     * the beacon has established the peer. */
    return peer_existed;
}

static void handle(const rx_evt_t *e)
{
    /* 🚨 COPY THE PAYLOAD, NOT ONLY THE HEADER.
     *
     * This was `e->len < MSG_HDR ? e->len : MSG_HDR` — correct while beacons
     * were the only frame type, and silently wrong the moment text arrived:
     * every message was delivered, acked, stored and shown with an EMPTY body.
     * The first real test read "MSG from Kid 2:" and nothing else, which looks
     * like a send bug and is not. */
    msg_frame_t f = { 0 };
    size_t cap = sizeof(f) < sizeof(e->buf) ? sizeof(f) : sizeof(e->buf);
    memcpy(&f, e->buf, e->len < cap ? e->len : cap);
    if (f.from_id == s_id) return;               /* our own broadcast */

    char name[10] = { 0 };
    memcpy(name, f.name, sizeof(f.name) - 1);

    peer_t *p = peer_by_mac(e->mac);
    /* 🔑 Did this peer already exist, or is this frame creating it? A content
     * frame that creates its own peer was received before encryption was
     * installed (see content_authentic) — so the answer gates trust below. */
    bool peer_existed = (p != NULL);
    if (!p) p = peer_add(e->mac, f.from_id, name);
    if (!p) return;
    p->rssi = e->rssi;
    p->last_heard = esp_timer_get_time();
    if (name[0]) strlcpy(p->name, name, sizeof(p->name));

    /* 🚨 WHERE MIGRATION ACTUALLY BITES, AND IT IS NOT DISCOVERY.
     *
     * A handset we hold no key for is still heard and still listed — that is
     * what makes it tappable on the Fleet screen and therefore pairable. What
     * closing migration stops is ACTING on anything it says. Beacons and
     * pairing offers are exempt: presence is not content, and refusing a pair
     * offer while closed would mean a closed fleet could never gain a member. */
    if (f.type != T_BEACON && f.type != T_PAIR && f.type != T_PAIR_ACK &&
        !fleet_find(e->mac) && !fleet_clear_allowed(e->mac)) {
        applog("[now] ignored %s (%04X) type %u - not paired, migration closed",
               p->name, f.from_id, f.type);
        return;
    }

    switch (f.type) {
    case T_BEACON:
        /* Presence only — plus the one bit that says this peer speaks for Home
         * Assistant. Recorded on every beacon rather than only on discovery, so
         * a gateway that reboots into a different role is believed promptly. */
        p->is_gateway = (f.flags & F_IS_GATEWAY) != 0;
        p->ha_ok      = (f.flags & F_GW_HA_OK)   != 0;
        break;

    /* 🚨 ADDRESSED, NEVER BROADCAST. A request carries the gateway's id and a
     * gateway answers only what names it. Two gateways in earshot must not both
     * answer the same question — that is the whole reason gw_wire.h insists on
     * explicit addressing rather than "whoever hears it". */
    case T_GW_REQ:
        if (f.to_id != s_id) break;
        if (!s_is_gateway) break;          /* a handset is not anyone's gateway */
        if (s_gw_req_cb) s_gw_req_cb(f.from_id, f.payload, f.payload_len);
        break;

    case T_GW_RSP:
        if (f.to_id != s_id) break;
        if (s_gw_rsp_cb) s_gw_rsp_cb(f.from_id, f.payload, f.payload_len);
        break;

    case T_MSG: {
        /* Addressed to us, or broadcast to everyone. A message for a sibling
         * is not ours to show. */
        if (f.to_id != s_id && f.to_id != ID_BROADCAST) break;

        /* 🔑 T7: refuse a forgeable message before it is acked, shown or
         * deduped. A dropped forgery must leave NO trace — not an ack the
         * attacker could use to confirm the MAC, and not a dedupe entry that
         * would then suppress a later genuine message with the same id. */
        if (!content_authentic(e, peer_existed)) {
            applog("[now] dropped text claiming %s (%04X): %s", name, f.from_id,
                   !e->unicast           ? "broadcast, cannot be encrypted" :
                   !peer_existed         ? "peer not yet encrypted (cleartext)"
                                         : "link not locked");
            break;
        }

        /* ⚠️ ACK BEFORE THE DEDUPE CHECK, NOT AFTER. A duplicate means the
         * sender never heard our first ack — so it is precisely the case that
         * needs another one. Acking only new messages makes the sender retry
         * until it gives up on a message we already have. */
        if (f.flags & F_WANT_ACK) send_ack(e->mac, f.from_id, f.msg_id);
        if (seen_before(f.from_id, f.msg_id)) break;

        char text[MSG_TEXT_MAX];
        uint16_t n = f.payload_len;
        if (n >= sizeof(text)) n = sizeof(text) - 1;
        memcpy(text, f.payload, n);
        text[n] = 0;
        /* f.to_id survived the filter above, so anything not BROADCAST was
         * addressed to this handset specifically. */
        inbox_add(p->name, f.from_id, text, e->rssi, f.to_id != ID_BROADCAST);
        break;
    }

    case T_GAME: {
        if (f.to_id != s_id && f.to_id != ID_BROADCAST) {
            applog("[now] game frame for %04X, not us (%04X)", f.to_id, s_id);
            break;
        }
        /* 🔑 T7: a game board is content too — a forged one wearing a sibling's
         * MAC could hand an opponent a winning move or corrupt the match. Same
         * rule as a text: locked peer, unicast, or it never happened. */
        if (!content_authentic(e, peer_existed)) {
            applog("[now] dropped game board claiming %s (%04X)", name, f.from_id);
            break;
        }
        if (f.payload_len > sizeof(espnow_game_t)) break;
        /* 🔑 NO DEDUPE AND NO ACK, DELIBERATELY. The board is idempotent: the
         * same frame applied twice is the same board. An ack would only add
         * traffic to a link a child is waiting on, and the NEXT board is a
         * better repair than a retry of the last one. */
        memcpy(&s_game, f.payload, f.payload_len);
        strlcpy(s_game_from, p->name, sizeof(s_game_from));
        s_game_from_id = f.from_id;
        s_game_have = true;
        break;
    }

    case T_ACK: {
        for (int i = 0; i < OUTBOX_N; i++) {
            if (!s_outbox[i].used || s_outbox[i].msg_id != f.msg_id) continue;
            s_outbox[i].used = false;
            applog("[now] delivered to %s", p->name);
            if (s_status_cb) s_status_cb("delivered");
        }
        break;
    }

    /* 🚨 THE ORDER HERE IS THE WHOLE DESIGN.
     *
     * B stores the key and upgrades its peer to encrypted BEFORE replying, so
     * the ACK goes out encrypted — which makes the ACK itself the proof that
     * the encrypted link works, not just that the frame was understood.
     *
     * A has already upgraded (see espnow_pair_with), so it can read that ACK.
     * If the ACK is lost, both ends still hold the same key and stay
     * consistent. It is only a LOST T_PAIR that leaves them mismatched, and the
     * cure for that is Remove on the roster screen and pair again. */
    case T_PAIR: {
        if (f.payload_len != sizeof(pair_payload_t)) break;
        if (!fleet_has_pmk()) { applog("[now] pair offer, but no fleet code here"); break; }

        pair_payload_t pp;
        memcpy(&pp, f.payload, sizeof(pp));

        uint8_t pk[FLEET_KEY_LEN], lmk[FLEET_KEY_LEN];
        fleet_derive_pair_key(s_mac, e->mac, pk);
        /* 🚨 THIS USED TO break WITH NO LOG AT ALL, and that is why a pair that
         * would not pair looked like nothing happening. A failed unwrap is the
         * single most informative event in this whole protocol — it means the
         * offer arrived intact but this handset could not open it, which is a
         * DIFFERENT FLEET CODE or a corrupted payload, not a radio problem.
         * Silence here sent the search to the antenna twice. */
        if (!fleet_unwrap_lmk(pk, pp.nonce, pp.ct, pp.tag, lmk)) {
            applog("[fleet] pair offer from %s could NOT be opened - "
                   "different fleet code, or a damaged frame", f.name);
            break;
        }

        /* 🔑 ANSWER FIRST, THEN UPGRADE. The acknowledgement has to leave while
         * this peer is still unencrypted, or the other end — which has not
         * upgraded either — could not read it. It carries the offer's own tag
         * back as proof that we really did decrypt it, so the initiator is
         * answering to something only a holder of the fleet code could produce.
         * The tag is not a secret: it is public in the offer we just received. */
        msg_frame_t r = { 0 };
        r.magic = MSG_MAGIC; r.type = T_PAIR_ACK; r.ttl = 1;
        r.from_id = s_id; r.to_id = f.from_id;
        r.payload_len = 16;
        strlcpy(r.name, s_name, sizeof(r.name));
        memcpy(r.payload, pp.tag, 16);
        esp_now_send(e->mac, (uint8_t *)&r, MSG_HDR + 16);

        fleet_peer_t *np = fleet_add(e->mac, f.from_id, f.name, lmk);
        if (!np) break;
        fleet_mark_encrypted(e->mac);

        /* 🚨 DO NOT SWAP THE KEY WHILE THAT REPLY IS STILL IN FLIGHT.
         *
         * esp_now_send() only QUEUES the frame. Deleting and re-adding the peer
         * with a new key immediately afterwards changes the key underneath a
         * frame that has not gone out yet, so the reply leaves encrypted with a
         * key the other end does not have — and is lost.
         *
         * This is what broke the fleet on 2026-09-06. The builder paired every handset
         * from every handset, so each link was paired TWICE; the second pairing
         * raced its own offer every time and left the two ends holding
         * different keys. Both showed "locked", neither could talk. Only the
         * links that happened to be paired once survived.
         *
         * The upgrade is therefore deferred to the espnow task, which applies it
         * once the frame has had time to leave. */
        memcpy(s_upg_mac, e->mac, 6);
        memcpy(s_upg_lmk, lmk, FLEET_KEY_LEN);
        s_upg_us = esp_timer_get_time();
        applog("[fleet] paired with %s", f.name);
        break;
    }

    case T_PAIR_ACK: {
        /* Only NOW does the initiator commit. No answer, no change. */
        /* 🚨 EVERY REJECTION HERE USED TO BE SILENT EXCEPT THE TAG.
         *
         * This is the initiator's ONLY commit point, so every one of these
         * breaks produces the same outcome: the responder is paired, this end
         * is not, and neither screen explains it. The builder, 2026-09-07, chasing
         * exactly that — Parent with an EMPTY roster while the gateway
         * listed him as paired and encrypted. Four different causes, one
         * symptom, no log. Name them. */
        if (f.payload_len != 16) {
            applog("[fleet] pair reply from %s malformed (%u bytes, want 16)",
                   f.name, f.payload_len);
            break;
        }
        if (!s_pend_us) {
            applog("[fleet] pair reply from %s but we are not pairing with anyone",
                   f.name);
            break;
        }
        if (memcmp(s_pend_mac, e->mac, 6)) {
            applog("[fleet] pair reply from %s (%04X) - we offered to someone else",
                   f.name, f.from_id);
            break;
        }
        if (esp_timer_get_time() - s_pend_us > PEND_WINDOW_US) {
            applog("[fleet] pair reply from %s arrived %llds late - window is %ds",
                   f.name,
                   (long long)((esp_timer_get_time() - s_pend_us) / 1000000),
                   (int)(PEND_WINDOW_US / 1000000));
            break;
        }
        if (memcmp(f.payload, s_pend_tag, 16)) {
            applog("[fleet] pair reply did not match - ignored");
            break;
        }
        fleet_peer_t *np = fleet_add(s_pend_mac, s_pend_id, s_pend_name, s_pend_lmk);
        if (!np) break;
        fleet_mark_encrypted(s_pend_mac);
        peer_encrypt_now(s_pend_mac, s_pend_lmk);
        memset(s_pend_mac, 0, sizeof(s_pend_mac));
        applog("[fleet] paired with %s - link encrypted", f.name);
        if (s_status_cb) s_status_cb("paired");
        break;
    }

    default:
        applog("[now] type %u from %s (%04X), %u bytes — not handled yet",
               f.type, p->name, f.from_id, f.payload_len);
        break;
    }
}

/* 🔑 UNPAIR PROPERLY — the roster entry AND the driver's peer table.
 *
 * fleet_remove() on its own wipes the stored key but leaves esp_now holding the
 * peer with encrypt=true. Enforcement lives BELOW the application (see the
 * ratchet note in fleet.h), so that stale RAM entry goes on DROPPING the other
 * end's plaintext pair offer before on_recv ever sees it. The link simply looks
 * dead, with nothing in either log to say why.
 *
 * ⚠️ This is almost certainly the "stale encrypted RAM peer" that made the
 * 2026-09-07 pairing diagnosis ambiguous — a reboot cured it, which is exactly
 * what you would expect, and is why the real cause could not be pinned down.
 *
 * Deleting the driver peer is safe: the very next beacon re-adds it unencrypted.
 * Returns false if that id is not in the roster. */
bool espnow_unpair(uint16_t id)
{
    for (int i = 0; i < FLEET_MAX; i++) {
        fleet_peer_t *p = fleet_at(i);
        if (!p || p->id != id) continue;

        uint8_t mac[6];
        char    name[FLEET_NAME_LEN];
        memcpy(mac, p->mac, sizeof(mac));
        strlcpy(name, p->name, sizeof(name));

        fleet_remove(mac);          /* the stored key */
        esp_now_del_peer(mac);      /* the RAM peer that does the dropping */

        /* 🚨 AND PUT THE PLAIN PEER STRAIGHT BACK, OR PAIRING AGAIN NEEDS A REBOOT.
         * The driver only gains a peer in peer_add(), when a NEW s_peers entry
         * is made. Deleting the encrypted peer left the s_peers entry in place
         * (it is still beaconing) with NO driver peer behind it, so the next
         * pair offer failed in esp_now_send() — "not found" — until the 8s
         * stale reap made a fresh entry. A gateway beacons constantly and is
         * never reaped. The builder, 2026-10-11: forgot Kay on the gateway,
         * removed it on Kay, re-pair "didn't take" until Kay was rebooted. */
        for (int j = 0; j < 8; j++) {
            if (!s_peers[j].used || memcmp(s_peers[j].mac, mac, 6)) continue;
            esp_now_peer_info_t pi = { 0 };
            memcpy(pi.peer_addr, mac, 6);
            pi.channel = 0;
            pi.encrypt = false;
            esp_err_t ae = esp_now_add_peer(&pi);
            if (ae != ESP_OK)
                applog("[now] re-add %04X in the clear FAILED: %s",
                       (unsigned)id, esp_err_to_name(ae));
            peer_set_rate(mac);
            break;
        }
        applog("[now] unpaired %s (%04X) - roster entry and encrypted peer both gone",
               name, (unsigned)id);
        return true;
    }
    return false;
}

/* Re-add a peer with encryption on. esp_now_add_peer() refuses a duplicate, so
 * the existing entry has to go first — the peer table is keyed by MAC. */
static void peer_encrypt_now(const uint8_t *mac, const uint8_t *lmk)
{
    esp_now_del_peer(mac);
    esp_now_peer_info_t pi = { 0 };
    memcpy(pi.peer_addr, mac, 6);
    pi.channel = 0;
    pi.encrypt = true;
    memcpy(pi.lmk, lmk, FLEET_KEY_LEN);
    esp_err_t e = esp_now_add_peer(&pi);
    if (e != ESP_OK) applog("[fleet] encrypt peer failed: %s", esp_err_to_name(e));
    peer_set_rate(mac);
}

bool espnow_pair_with(int peer_idx)
{
    if (!s_running || !fleet_has_pmk()) return false;
    peer_t *p = peer_at(peer_idx);
    if (!p) return false;

    /* 🔑 RANDOM, NOT DERIVED. A key computed from the fleet code and the two
     * MACs could be recomputed by any ex-member — the MACs are broadcast in the
     * beacon — so revocation would look like it worked and would not. */
    uint8_t lmk[FLEET_KEY_LEN];
    esp_fill_random(lmk, sizeof(lmk));

    uint8_t pk[FLEET_KEY_LEN];
    fleet_derive_pair_key(s_mac, p->mac, pk);

    pair_payload_t pp;
    if (!fleet_wrap_lmk(pk, lmk, pp.nonce, pp.ct, pp.tag)) return false;

    msg_frame_t f = { 0 };
    f.magic = MSG_MAGIC; f.type = T_PAIR; f.ttl = 1;
    f.from_id = s_id; f.to_id = p->id;
    f.payload_len = sizeof(pp);
    strlcpy(f.name, s_name, sizeof(f.name));
    memcpy(f.payload, &pp, sizeof(pp));

    /* Sent in the clear — it has to be, the peer has no key yet. The payload is
     * what is protected, not the frame. */
    esp_err_t e = esp_now_send(p->mac, (uint8_t *)&f, MSG_HDR + sizeof(pp));
    if (e != ESP_OK) { applog("[fleet] pair send failed: %s", esp_err_to_name(e)); return false; }

    /* 🚨 DO NOT STORE OR ENCRYPT YET — WAIT TO BE ANSWERED.
     *
     * The first version stored the key and switched to an encrypted peer the
     * moment the offer was SENT. When the far end rejected the offer (a
     * different fleet code, so the tag check failed) the initiator was left
     * encrypting to a handset that could not decrypt, while refusing that
     * handset's cleartext in return — deaf and mute, silently, to a peer that
     * still appeared present because beacons are broadcast. It cost Parent
     * its whole fleet: `4 paired` against four handsets that had paired with
     * nobody. A pairing that FAILS must change nothing. */
    memcpy(s_pend_mac, p->mac, 6);
    memcpy(s_pend_lmk, lmk, FLEET_KEY_LEN);
    memcpy(s_pend_tag, pp.tag, 16);
    s_pend_id  = p->id;
    strlcpy(s_pend_name, p->name, sizeof(s_pend_name));
    s_pend_us  = esp_timer_get_time();
    applog("[fleet] pair offer sent to %s - waiting", p->name);
    return true;
}

/* ------------------------------------------------------------------ task */

/* 🔍 DID THE FRAME ACTUALLY LEAVE? Two different answers, and only both
 * together are useful: esp_now_send() returning ESP_OK means the driver ACCEPTED
 * it (a code fault shows up here), while the send callback reports what the MAC
 * layer saw. ⚠️ For a BROADCAST the callback always says success - there is
 * nobody to acknowledge it - so a healthy count here does NOT mean anyone
 * heard it. It only rules out our own software. */
static uint32_t s_tx_ok, s_tx_err, s_cb_ok, s_cb_fail;

/* ⚠️ THE SEND-CALLBACK SIGNATURE DIFFERS BY IDF VERSION, AND THIS FILE NOW
 * BUILDS UNDER BOTH. ESP-NOW v2 (IDF 5.4+) passes a `const esp_now_send_info_t
 * *`; IDF 5.3 and earlier pass the peer MAC. Same protocol ON THE AIR — only
 * the C prototype moved.
 *
 * 🔑 WHY THE GUARD EXISTS AT ALL: the handsets build on IDF 5.3, and the
 * ESPHome gateway builds on whatever ESPHome ships (5.5.5 as of 2026-09-11).
 * The same espnow.c has to compile for both or the gateway would need its own
 * copy of the radio — which is exactly the drift this file is written to
 * prevent. Without the guard the 5.5 build fails with "passing argument 1 of
 * esp_now_register_send_cb from incompatible pointer type", which is at least
 * loud; a cast to silence it would have been the dangerous fix. */
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
static void on_sent(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    (void)info;
#else
static void on_sent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
#endif
    if (status == ESP_NOW_SEND_SUCCESS) s_cb_ok++; else s_cb_fail++;
}

static void send_beacon(void)
{
    msg_frame_t f = { 0 };
    f.magic   = MSG_MAGIC;
    f.type    = T_BEACON;
    f.flags   = s_is_gateway ? (F_IS_GATEWAY | (s_gw_ha_ok ? F_GW_HA_OK : 0)) : 0;
    f.from_id = s_id;
    f.to_id   = ID_BROADCAST;
    f.msg_id  = ++s_beacon_seq;
    strlcpy(f.name, s_name, sizeof(f.name));
    /* 🔑 Carry OUR interval: without it the far end cannot know how many
     * beacons it should have had, and so cannot tell silence from "nothing was
     * sent". The CYD's walk test is built on this. */
    uint16_t bm = BEACON_MS;
    memcpy(f.payload, &bm, 2);
    f.payload_len = 2;
    esp_err_t e = esp_now_send(BCAST, (uint8_t *)&f, MSG_HDR + 2);
    if (e == ESP_OK) s_tx_ok++;
    else {
        s_tx_err++;
        if (s_tx_err <= 3) applog("[now] beacon send failed: %s", esp_err_to_name(e));
    }
    /* Once every 15 beacons, so a board left on a bench says what it is doing
     * without filling the log. */
    if ((s_tx_ok + s_tx_err) % 15 == 0) {
        /* `peers` is everything on the radio; `playable` is what the games,
         * the Talkie and T9 will actually offer. They differ by the number of
         * gateways in earshot, and printing both is what makes the 2026-09-08
         * filter verifiable from a log instead of taken on trust. */
        applog("[now] tx ok %u err %u | cb ok %u fail %u | peers %d (%d playable)",
               (unsigned)s_tx_ok, (unsigned)s_tx_err, (unsigned)s_cb_ok,
               (unsigned)s_cb_fail, espnow_peer_count(), espnow_play_count());

        /* 🔑 PER-PEER RSSI, so coverage can be answered with MEASUREMENT rather
         * than by building three repeaters on a guess.
         *
         * The plan was one repeater per floor. But each repeater has to be an
         * encrypted peer of both ends to relay at all — ESP-NOW has no repeat
         * function and unicast is MAC-filtered — so every one of them is a
         * board on a shelf holding fleet keys, which is the exposure this whole
         * project exists to remove. Worth knowing the real numbers first.
         *
         * ⚠️ The `+ peer` discovery line prints rssi 0 because peer_add()
         * memsets the struct before the first frame fills it in. That is NOT a
         * dead reading and cost an hour of suspicion on 2026-09-07 — this line,
         * printed later, carries the real value.
         *
         * Rides the existing 15-beacon cadence rather than adding a timer: the
         * kids carry these round the house all day, so normal use writes the
         * coverage map by itself. */
        for (int i = 0; i < 8; i++)
            if (s_peers[i].used)
                applog("[rssi] %s %04X %d dBm", s_peers[i].name,
                       s_peers[i].id, s_peers[i].rssi);
    }
}


/* ---- the channel we are ACTUALLY on, and how we get back to company ------
 *
 * 🚨 HANDSETS ON DIFFERENT CHANNELS ARE STONE DEAF TO EACH OTHER, and moving
 * the fleet from 6 to 11 means a window where some are on each. This is the
 * safety net for that window: a handset that finds itself completely alone goes
 * looking, and joins whichever channel its siblings are actually on.
 *
 * 🔑 IT IS A SAFETY NET, NOT A MIGRATION MECHANISM, and the difference matters.
 * It cannot converge a fleet on its own: a newly-flashed handset is always in
 * the minority, so it follows the others back to the OLD channel every time,
 * and the group never moves. Migrating is still a deliberate act — set all four
 * on the menu, within a few minutes of each other. What this buys is that no
 * handset is ever LOST while you do it, and a wrong setting is recoverable
 * without a cable. That is the lesson the CYD's Setup screen already taught.
 *
 * ⚠️ s_op_channel, not the setting, is what espnow_radio_back() re-pins to —
 * otherwise coming back from a wifi association would jump a handset home and
 * abandon the siblings it just found. */
static uint8_t s_op_channel;
static int64_t s_alone_since, s_last_sweep;

/* Dwell here, draining the rx queue so peers are learned normally, until
 * somebody shows up or the time runs out. Beacons arrive every second, so 2.5s
 * gives two chances rather than one. */
static bool listen_for_peers(int ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < end) {
        rx_evt_t e;
        if (xQueueReceive(s_rx_q, &e, pdMS_TO_TICKS(50)) == pdTRUE) handle(&e);
        if (espnow_peer_count() > 0) return true;
    }
    return espnow_peer_count() > 0;
}

static void channel_sweep(void)
{
    static const uint8_t CAND[] = { 1, 6, 11 };
    const uint8_t home = (uint8_t)settings_espnow_channel();

    applog("[now] alone on ch %d - looking for siblings", s_op_channel);
    for (unsigned i = 0; i < sizeof(CAND) / sizeof(CAND[0]); i++) {
        if (CAND[i] == s_op_channel) continue;
        esp_wifi_set_channel(CAND[i], WIFI_SECOND_CHAN_NONE);
        s_op_channel = CAND[i];
        /* Announce ourselves while we listen — the sibling may be as alone as
         * we are, and two silent boards sweeping past each other would find
         * nothing. */
        send_beacon();
        if (listen_for_peers(2500)) {
            applog("[now] found siblings on ch %d (home is %d) - staying",
                   CAND[i], home);
            return;
        }
    }
    esp_wifi_set_channel(home, WIFI_SECOND_CHAN_NONE);
    s_op_channel = home;
    applog("[now] nobody on any channel - back to ch %d", home);
}

static void sweep_tick(int64_t now)
{
    if (espnow_peer_count() > 0) { s_alone_since = 0; return; }
    if (!s_alone_since) { s_alone_since = now; return; }
    /* 30s alone before looking, and never more often than every 30s. Sweeping
     * costs ~5s off-channel, so a handset that is legitimately the only one
     * switched on must not spend its life doing it. */
    if (now - s_alone_since < 30000000) return;
    if (s_last_sweep && now - s_last_sweep < 30000000) return;
    s_last_sweep = now;
    channel_sweep();
    s_alone_since = esp_timer_get_time();
}

static void espnow_task(void *arg)
{
    (void)arg;
    int64_t last_beacon = 0;
    for (;;) {
        rx_evt_t e;
        if (xQueueReceive(s_rx_q, &e, pdMS_TO_TICKS(100)) == pdTRUE) handle(&e);

        int64_t now = esp_timer_get_time();
        if (now - last_beacon > (int64_t)BEACON_MS * 1000) {
            last_beacon = now;
            /* ⚠️ outbox_pump() still runs: a queued text is something the user
             * asked for and must not be silently held. Only the once-a-second
             * PRESENCE chatter stands down. */
            if (!s_beacon_quiet) send_beacon();
            outbox_pump();          /* retries ride the same 1s tick */
        }
        /* Apply a deferred key upgrade once its reply has had time to go. */
        if (s_upg_us && now - s_upg_us > UPG_DELAY_US) {
            peer_encrypt_now(s_upg_mac, s_upg_lmk);
            s_upg_us = 0;
        }

        /* Age peers out, so "in range" means heard recently and not merely
         * heard once. */
        for (int i = 0; i < 8; i++) {
            if (!s_peers[i].used) continue;
            if (now - s_peers[i].last_heard > (int64_t)PEER_STALE_MS * 1000) {
                applog("[now] - %s (silent %ds)", s_peers[i].name,
                       (int)((now - s_peers[i].last_heard) / 1000000));
                esp_now_del_peer(s_peers[i].mac);
                s_peers[i].used = false;
            }
        }

        sweep_tick(now);
    }
}

/* ---------------------------------------------------------------- public */

void espnow_radio_back(void)
{
    if (!s_running) return;
    /* ⚠️ Associating drags the radio onto the AP's channel and disconnecting
     * does NOT put it back. Every peer would still be on 6 while we sat on
     * whatever the AP used — deaf, with a healthy-looking driver. */
    esp_wifi_set_channel(s_op_channel ? s_op_channel : ESPNOW_CHANNEL,
                         WIFI_SECOND_CHAN_NONE);
}

/* Move to a new channel now, from the menu. Resets the sweep state so a change
 * gets a fresh 30s before the handset decides it is lonely and goes looking. */
void espnow_set_channel(int ch)
{
    if (ch != 1 && ch != 6 && ch != 11) return;
    settings_set_espnow_channel(ch);
    if (!s_running) return;
    s_op_channel = (uint8_t)ch;
    esp_wifi_set_channel(s_op_channel, WIFI_SECOND_CHAN_NONE);
    s_alone_since = s_last_sweep = 0;
    applog("[now] channel is now %d", ch);
}

static void out_tx(out_entry_t *o)
{
    msg_frame_t f = { 0 };
    f.magic       = MSG_MAGIC;
    f.type        = T_MSG;
    f.flags       = F_WANT_ACK;
    f.ttl         = 1;
    f.from_id     = s_id;
    f.to_id       = o->to_id;
    f.msg_id      = o->msg_id;
    f.payload_len = o->len;
    strlcpy(f.name, s_name, sizeof(f.name));
    memcpy(f.payload, o->text, o->len);

    /* Unicast to a known peer, broadcast otherwise. Broadcast is never acked
     * by ESP-NOW itself, which is why the APPLICATION ack above exists. */
    const uint8_t *mac = BCAST;
    for (int i = 0; i < 8; i++)
        if (s_peers[i].used && s_peers[i].id == o->to_id) { mac = s_peers[i].mac; break; }

    esp_now_send(mac, (uint8_t *)&f, MSG_HDR + o->len);
    o->tries++;
    o->next_us = esp_timer_get_time() + (int64_t)OUT_GAP_MS * 1000;
}

/* Called from the espnow task. Retries anything unacked until OUT_TRIES, then
 * gives up OUT LOUD — a message that silently vanishes is worse than one that
 * says it failed, because the child walks away believing it was sent. */
static void outbox_pump(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < OUTBOX_N; i++) {
        out_entry_t *o = &s_outbox[i];
        if (!o->used || now < o->next_us) continue;
        if (o->tries >= OUT_TRIES) {
            o->used = false;
            applog("[now] gave up on msg %u after %d tries",
                   (unsigned)o->msg_id, o->tries);
            if (s_status_cb) s_status_cb("not delivered");
            continue;
        }
        out_tx(o);
    }
}

bool espnow_msg_send(int peer_idx, const char *text)
{
    if (!s_running || !text || !*text) return false;

    uint16_t to = ID_BROADCAST;
    if (peer_idx >= 0) {
        peer_t *p = peer_at(peer_idx);
        if (!p) return false;
        to = p->id;
    }

    for (int i = 0; i < OUTBOX_N; i++) {
        if (s_outbox[i].used) continue;
        out_entry_t *o = &s_outbox[i];
        memset(o, 0, sizeof(*o));
        o->used   = true;
        o->to_id  = to;
        o->msg_id = next_msg_id();
        strlcpy(o->text, text, sizeof(o->text));
        o->len    = (uint16_t)strlen(o->text);
        out_tx(o);
        if (s_status_cb) s_status_cb("sending...");
        return true;
    }
    applog("[now] outbox full, message dropped");
    if (s_status_cb) s_status_cb("busy - try again");
    return false;
}

bool espnow_game_send(int peer_idx, const espnow_game_t *g)
{
    if (!s_running || !g) return false;
    if (g->cells > GAME_BOARD_MAX) return false;

    uint16_t to = ID_BROADCAST;
    const uint8_t *mac = BCAST;
    if (peer_idx >= 0) {
        peer_t *p = peer_at(peer_idx);
        if (!p) return false;
        to  = p->id;
        mac = p->mac;
    }

    msg_frame_t f = { 0 };
    f.magic       = MSG_MAGIC;
    f.type        = T_GAME;
    f.from_id     = s_id;
    f.to_id       = to;
    f.payload_len = sizeof(espnow_game_t);
    strlcpy(f.name, s_name, sizeof(f.name));
    memcpy(f.payload, g, sizeof(*g));
    esp_err_t e = esp_now_send(mac, (uint8_t *)&f, MSG_HDR + sizeof(espnow_game_t));
    if (e != ESP_OK)
        applog("[now] game send to %04X failed: %s", to, esp_err_to_name(e));
    return e == ESP_OK;
}

bool espnow_game_take(espnow_game_t *g, char *from, size_t from_n, int *peer_idx)
{
    if (!s_game_have) return false;
    s_game_have = false;
    if (g)    memcpy(g, &s_game, sizeof(*g));
    if (from) strlcpy(from, s_game_from, from_n);
    if (peer_idx) {
        *peer_idx = -1;
        int n = 0;
        for (int i = 0; i < 8; i++) {
            if (!s_peers[i].used) continue;
            if (s_peers[i].id == s_game_from_id) { *peer_idx = n; break; }
            n++;
        }
    }
    return true;
}

int espnow_inbox_count(void) { return s_inbox_n; }

const espnow_msg_t *espnow_inbox(int i)
{
    return (i >= 0 && i < s_inbox_n) ? &s_inbox[i] : NULL;
}

void espnow_inbox_clear(void) { s_inbox_n = 0; s_inbox_dirty = false; }

bool espnow_inbox_take_dirty(void)
{
    bool d = s_inbox_dirty;
    s_inbox_dirty = false;
    return d;
}

bool espnow_begin(void)
{
    esp_read_mac(s_mac, ESP_MAC_WIFI_STA);
    s_id = ((uint16_t)s_mac[4] << 8) | s_mac[5];

    /* The handset's own name, so an unknown peer is still readable on a screen.
     * settings_name() is "Boy"/"Girl"/"Guest"; fall back to the id. */
    extern const char *msg_dev_name(void);
    strlcpy(s_name, msg_dev_name(), sizeof(s_name));

    s_rx_q = xQueueCreate(6, sizeof(rx_evt_t));
    if (!s_rx_q) { applog("[now] no memory for the rx queue"); return false; }

    /* 🚨 THE DRIVER MUST STAY STARTED OR ESP-NOW HEARS NOTHING. net_down()
     * respects the hold and stops deinitialising. */
    net_radio_hold();
    if (!net_start_radio()) {
        applog("[now] the wifi driver would not start - no ESP-NOW");
        net_radio_release();
        return false;
    }
    s_op_channel = (uint8_t)ESPNOW_CHANNEL;
    esp_wifi_set_channel(s_op_channel, WIFI_SECOND_CHAN_NONE);

    /* 🛰 ADVERTISE LR AS A CAPABILITY, ALWAYS — it costs nothing and it is what
     * makes the range setting a runtime toggle instead of a reflash.
     *
     * 🔑 ENABLING LR IS NOT THE SAME AS TRANSMITTING LR. This bitmap says what
     * this radio can RECEIVE; what it SENDS is chosen per peer by
     * esp_now_set_peer_rate_config(). So a handset can carry LR support while
     * still talking to everyone at 1 Mbps, which is exactly what allows a
     * staged rollout: flash every board first (a behavioural no-op), flip the
     * rate afterwards.
     *
     * ⚠️ No access point speaks LR, so 11b/g/n stay in the list and the STA
     * associates on those as before. Logged rather than assumed — if telemetry
     * ever stops associating after this, THIS is the line to suspect. */
    esp_err_t pe = esp_wifi_set_protocol(WIFI_IF_STA,
                                         WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                                         WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
    applog("[now] protocol 11b/g/n+LR: %s", esp_err_to_name(pe));

    esp_err_t e = esp_now_init();
    if (e != ESP_OK) {
        applog("[now] esp_now_init failed: %s", esp_err_to_name(e));
        net_radio_release();
        return false;
    }
    esp_now_register_recv_cb(on_recv);
    esp_now_register_send_cb(on_sent);

    esp_now_peer_info_t pi = { 0 };
    memcpy(pi.peer_addr, BCAST, 6);
    pi.channel = 0;
    pi.encrypt = false;
    esp_now_add_peer(&pi);
    peer_set_rate(BCAST);   /* beacons and the voice fallback ride this one */

    s_running = true;
    xTaskCreatePinnedToCore(espnow_task, "espnow", 8192, NULL, 4, NULL, 0);
    applog("[now] up as %s (%04X) on channel %d, %02X:%02X:%02X:%02X:%02X:%02X",
           s_name, s_id, ESPNOW_CHANNEL,
           s_mac[0], s_mac[1], s_mac[2], s_mac[3], s_mac[4], s_mac[5]);
    return true;
}

bool espnow_running(void) { return s_running; }

/* The beacon, the ack and every message frame all read s_name when they are
 * BUILT, not when the radio came up, so overwriting it here is enough — the
 * next beacon (1s away at most) carries the new name and the far end's peer
 * entry is refreshed by note_peer() the moment it arrives.
 *
 * ⚠️ 9 CHARACTERS PLUS A NUL, AND THAT WIDTH IS ON THE WIRE. msg_frame_t.name
 * is char[10] and is byte-compatible with the CYD's msg_core.cpp. strlcpy
 * truncates rather than overruns, but a name longer than 9 will arrive cut —
 * keep the picker's list inside it. */
void espnow_set_name(const char *n)
{
    if (!n || !n[0]) return;
    if (!strcmp(s_name, n)) return;          /* no log spam on a no-op tap */
    strlcpy(s_name, n, sizeof(s_name));
    applog("[now] this handset is now %s (%04X)", s_name, s_id);
}

static peer_t *peer_at(int idx)
{
    int n = 0;
    for (int i = 0; i < 8; i++)
        if (s_peers[i].used) { if (n == idx) return &s_peers[i]; n++; }
    return NULL;
}

int espnow_peer_count(void)
{
    int n = 0;
    for (int i = 0; i < 8; i++) if (s_peers[i].used) n++;
    return n;
}
const char *espnow_peer_name(int i) { peer_t *p = peer_at(i); return p ? p->name : ""; }

uint16_t espnow_peer_id(int i)
{
    peer_t *p = peer_at(i);
    return p ? p->id : 0;
}

uint16_t espnow_my_id(void) { return s_id; }

const char *espnow_nobody_text(void)
{
    static char t[24];
    snprintf(t, sizeof t, "nobody on ch %d", ESPNOW_CHANNEL);
    return t;
}
int8_t      espnow_peer_rssi(int i) { peer_t *p = peer_at(i); return p ? p->rssi : 0; }

/* ------------------------------------------------------------------ voice */

bool espnow_voice_send(const void *frame, int len, uint16_t to_id)
{
    if (!s_running || len <= 0 || len > VOX_MAX) return false;

    /* 🚨 ONE LISTENER: SEND TO THEM AND STOP. Not a filter at the far end — the
     * frame never leaves for anybody else, so a private conversation cannot be
     * overheard by a handset that simply ignores the to_id field. It also cuts
     * the airtime by the number of siblings NOT being talked to: three
     * listeners at 8.4 KB/s is ~200 kbps of a 1 Mbps link, and this is a third
     * of it.
     *
     * A target that has gone silent returns false rather than falling through
     * to everyone — the failure a child needs told about is "they cannot hear
     * you", and quietly broadcasting instead is the one outcome that breaks the
     * promise the To: row makes. */
    if (to_id) {
        for (int i = 0; i < 8; i++)
            if (s_peers[i].used && s_peers[i].id == to_id)
                return esp_now_send(s_peers[i].mac,
                                    (const uint8_t *)frame, len) == ESP_OK;
        return false;
    }

    /* 🚨 UNICAST PER PEER, AND THIS REPLACED A BROADCAST THAT WAS COSTING AUDIO.
     *
     * The first version broadcast, on the reasoning that a voice frame is worth
     * having once or not at all so retries are pointless. That was wrong about
     * the mechanism: ESP-NOW broadcast gets NO 802.11 retry at ALL, while a
     * unicast gets the full retry machinery — and a retry at 1 Mbps completes in
     * well under a millisecond, so it lands INSIDE the 20ms frame budget and is
     * heard, not discarded. Free frames, not late ones.
     *
     * 🔑 AND IT MAKES LOSS VISIBLE. Broadcast is never acked, so on_sent() always
     * reported success and `cb fail 0` meant nothing whatsoever. Unicast is
     * acked, so that counter finally says something true about the air.
     *
     * The arithmetic that made broadcast look necessary was for the ~25 KB/s
     * music stream; voice is 8.4 KB/s, so three listeners is 200 kbps and fits
     * inside 1 Mbps with room over. */
    int n_peers = 0;
    for (int i = 0; i < 8; i++) if (s_peers[i].used) n_peers++;
    if (n_peers == 0) return false;          /* nobody to hear it */

    /* ⚠️ A GUARD, NOT A FEATURE — the fleet is four handsets, so this cannot
     * trigger today. Past four listeners the per-peer airtime stops being free
     * (see peer_set_rate) and one broadcast for everybody is the better trade,
     * retries or no retries. */
    if (n_peers > 4)
        return esp_now_send(BCAST, (const uint8_t *)frame, len) == ESP_OK;

    int sent = 0;
    for (int i = 0; i < 8; i++) {
        if (!s_peers[i].used) continue;
        if (esp_now_send(s_peers[i].mac, (const uint8_t *)frame, len) == ESP_OK)
            sent++;
    }
    return sent > 0;
}

int espnow_voice_take(void *out, int max)
{
    if (s_vox_tail == s_vox_head) return 0;          /* empty */
    vox_slot_t *s = &s_vox[s_vox_tail];
    int n = s->len;
    if (n > max) n = max;
    memcpy(out, s->buf, n);
    s_vox_tail = (uint8_t)((s_vox_tail + 1) % VOX_RING);
    return n;
}

uint32_t espnow_voice_overruns(void) { return s_vox_over; }
uint16_t espnow_self_id(void)        { return s_id; }

/* 🚨 IS SOMEBODY TALKING? — ASKED BY THE TELEMETRY LOOP, NOT BY THE UI.
 *
 * A handset knows its OWN walkie screen is open, but a conversation makes every
 * handset in earshot a participant: the listeners must stay off the AP too, and
 * they have no other way to find out. A voice frame arriving IS the signal, so
 * this needs no beacon flag and no protocol change — exactly the shape of
 * espnow_dongle_busy(), which was added for the same reason when an idle
 * sibling's association bonged someone else's music.
 *
 * 8 seconds: long enough to cover the gaps between one child's presses, short
 * enough that telemetry resumes soon after the conversation really ends. */
bool espnow_voice_recent(void)
{
    if (!s_vox_last_us) return false;
    return (esp_timer_get_time() - s_vox_last_us) < 8000000;
}


void espnow_beacon_quiet(bool on)
{
    if (s_beacon_quiet == on) return;
    s_beacon_quiet = on;
    applog("[now] presence beacon %s", on ? "PAUSED (gps acquiring)" : "resumed");
}

bool espnow_beacon_quiet_get(void) { return s_beacon_quiet; }
