/*
 * Walkie-talkie. Mic -> UDP -> other handsets, and back.
 *
 * The whole feature is one task, one socket and two 640-byte buffers. There is
 * no codec: 16kHz 16-bit mono is what the mic already produces and what the
 * speaker already takes, so nothing is converted anywhere. 32 KB/s against the
 * 100 KB/s this board was measured doing to Home Assistant.
 *
 * Half-duplex by design. Push-to-talk means the mic is live only while the
 * button is held, so there is never an echo path and never double-talk — which
 * deletes acoustic echo cancellation, the single hardest part of doing voice on
 * an ESP32. The button is not a compromise, it is the reason this is tractable.
 *
 * ⚠️ Wire-compatible with tools/wt.py. Change one, change the other.
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "hw.h"
#include "audio.h"
#include "settings.h"
#include "player.h"
#include "applog.h"
#include "adpcm.h"
#include "espnow.h"
#include "settings.h"
#include "voice_wire.h"
#include "net.h"
#include "wt.h"

static const char *TAG = "wt";

/* header: magic(3) type(1) seq(4, big-endian) id(16, nul-padded) = 24 bytes.
 * Assembled by hand rather than as a struct — a compiler is free to pad a
 * struct and this has to match Python's `!3sBI16s` byte for byte. */
#define HDR_LEN 24
static const char MAGIC[3] = { 'W', 'T', '1' };

enum { T_BEACON = 1, T_START = 2, T_AUDIO = 3, T_END = 4 };

#define BEACON_US   (2 * 1000 * 1000)
#define PEER_TTL_US (8 * 1000 * 1000)

typedef struct {
    uint32_t ip;
    char     name[17];
    int64_t  seen;
} peer_t;

static int        s_sock = -1;
static wt_mode_t  s_mode;
static volatile bool s_ptt;          /* button state, set from the UI thread */
static bool       s_tx;              /* we are actually transmitting          */
static int        s_txpeak;          /* loudest sample of the current press    */
/* 🔍 Per-second frame counters. The builder, 2026-08-18: both handsets "stop
 * brodcasting after a few seconds" while the press log says the button was
 * still down 8-9s and wifi never dropped. A press-level log cannot tell a
 * sender that ran dry from a receiver that stopped playing; these can. */
static int        s_stat_tx, s_stat_rx;
/* Per-second PEAK, not just frame count. The counters proved frames keep
 * flowing for the whole press while the builder hears the audio cut at 5-7s, and a
 * press-level peak cannot show that: it reports the loud first seconds no
 * matter what follows. Silent frames and real frames look identical until the
 * level is measured per second, at BOTH ends. */
static int        s_stat_txpk, s_stat_rxpk;
/* The samples are provably good all the way to audio_write() and the sound
 * still stops, so the next suspects are the three things between that call and
 * the speaker: the I2S write status, the amp enable, and the codec's own DAC
 * volume register (which set_out_vol has been caught rewriting before). */
static esp_err_t  s_stat_werr;
static int64_t    s_stat_at;
static uint32_t   s_seq;
static peer_t     s_peers[WT_MAX_PEERS];
static char       s_hearing[17];
static int64_t    s_hearing_at;
/* 🚨 WHOSE VOICE WE ARE PLAYING, AS AN ID — THE TALKER LATCH.
 *
 * Without this, two siblings keying up at the same handset interleave into one
 * decode ring and both come out as noise. That was already true when everyone
 * heard everyone, but going one-to-one INVITES it: a private conversation
 * sounds like it ought to be protected, so the moment it is not, it reads as
 * broken hardware. First talker wins until they stop or time out. */
static uint16_t   s_hearing_id;
/* The chosen listener, 0 = everyone. */
static uint16_t   s_target;
/* Set while transmitting to a target that is no longer in range, so the screen
 * can say "they cannot hear you" instead of looking like it worked. */
static bool       s_tx_unreachable;
static char       s_id[17];
static char       s_status[64] = "off";

/* 🎙 WHICH RADIO THIS SESSION IS ON. Decided once at open and fixed for the
 * session — switching transports mid-press would drop the tail of whatever was
 * being said, and the answer cannot change while the screen is up anyway. */
static bool           s_via_espnow;
static adpcm_state_t  s_enc;              /* our encoder, runs across frames */
static uint8_t        s_vox_seq;
static uint32_t       s_vox_rx, s_vox_tx;
static bool       s_was_playing;
static volatile bool s_want_open;
static void wt_do_open(void);

wt_mode_t   wt_mode(void)       { return s_mode; }
bool        wt_talking(void)    { return s_tx; }
const char *wt_hearing(void)    { return s_hearing; }
uint16_t    wt_target(void)     { return s_target; }
void        wt_set_target(uint16_t id) { s_target = id; }
const char *wt_status(void)     { return s_status; }

int wt_peer_count(void)
{
    /* 🔑 THE PEER LIST BELONGS TO WHICHEVER RADIO IS CARRYING THE CHANNEL.
     * The UDP list below is built from WT1 beacons and is empty in an ESP-NOW
     * session — reporting it would say "0 peers" to a child standing next to
     * their sibling, which is the exact confusion this whole change set exists
     * to remove. */
    /* ⚠️ PLAYABLE peers: a gateway beacons like a handset but there is
     * nobody behind it to hear you. Counting it would tell a child alone
     * in a room that somebody is listening. */
    if (s_via_espnow) return espnow_play_count();

    int64_t now = esp_timer_get_time();
    int n = 0;
    for (int i = 0; i < WT_MAX_PEERS; i++)
        if (s_peers[i].ip && now - s_peers[i].seen < PEER_TTL_US) n++;
    return n;
}

void wt_ptt(bool down)
{
    /* Clear the reachability flag on the way DOWN, so the warning describes
     * this press and not the last one. */
    if (down) s_tx_unreachable = false;
    s_ptt = down;                            /* nothing else: UI must not block */
}

/* ------------------------------------------------------------------ */

static int build(uint8_t *out, int kind, const void *payload, int len)
{
    memcpy(out, MAGIC, 3);
    out[3] = (uint8_t)kind;
    uint32_t seq = ++s_seq;
    out[4] = seq >> 24; out[5] = seq >> 16; out[6] = seq >> 8; out[7] = seq;
    memset(out + 8, 0, 16);
    memcpy(out + 8, s_id, strnlen(s_id, 16));
    if (len) memcpy(out + HDR_LEN, payload, len);
    return HDR_LEN + len;
}

static void note_peer(uint32_t ip, const char *name)
{
    int64_t now = esp_timer_get_time();
    int free_slot = -1;
    for (int i = 0; i < WT_MAX_PEERS; i++) {
        if (s_peers[i].ip == ip) { s_peers[i].seen = now; return; }
        if (!s_peers[i].ip && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) return;
    s_peers[free_slot].ip = ip;
    s_peers[free_slot].seen = now;
    strlcpy(s_peers[free_slot].name, name, sizeof(s_peers[free_slot].name));
    applog("[wt] + %s", name);
}

/* One frame to every live peer. */
static void send_all(const uint8_t *pkt, int len)
{
    int64_t now = esp_timer_get_time();
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(WT_PORT) };
    for (int i = 0; i < WT_MAX_PEERS; i++) {
        if (!s_peers[i].ip || now - s_peers[i].seen > PEER_TTL_US) continue;
        to.sin_addr.s_addr = s_peers[i].ip;
        sendto(s_sock, pkt, len, 0, (struct sockaddr *)&to, sizeof(to));
    }
}

static void send_beacon(void)
{
    uint8_t pkt[HDR_LEN];
    int n = build(pkt, T_BEACON, NULL, 0);
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(WT_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    sendto(s_sock, pkt, n, 0, (struct sockaddr *)&to, sizeof(to));
}

/* ------------------------------------------------------------------ */

/* Build and broadcast one VOX frame. `st` is the encoder state as it was
 * BEFORE this frame's samples were encoded — the snapshot is the whole point,
 * see the note in voice_wire.h. NULL for START/END, which carry no audio. */
static void vox_tx(uint8_t type, const adpcm_state_t *st,
                   const void *payload, int len)
{
    uint8_t pkt[VOX_MAX];
    vox_hdr_t h = {
        .magic     = VOX_MAGIC,
        .type      = type,
        .seq       = s_vox_seq++,
        .from_id   = espnow_self_id(),
        .predictor = st ? st->predictor : 0,
        .index     = st ? st->index : 0,
        .pad       = 0,
    };
    memcpy(pkt, &h, VOX_HDR);
    if (len > 0) memcpy(pkt + VOX_HDR, payload, len);
    if (!espnow_voice_send(pkt, VOX_HDR + (len > 0 ? len : 0), s_target)
        && s_target)
        s_tx_unreachable = true;      /* chosen listener has gone silent */
}

/* Who is 0x74E8? The peer list already knows, because beacons carry names —
 * so a VOX_AUDIO frame does not have to. Only VOX_START carries the name, and
 * this covers the case where that one frame was the one that got lost. */
static const char *vox_name_for(uint16_t id)
{
    int n = espnow_peer_count();
    for (int i = 0; i < n; i++)
        if (espnow_peer_id(i) == id) return espnow_peer_name(i);
    return "someone";
}

static void wt_task(void *arg)
{
    static uint8_t  pkt[HDR_LEN + WT_FRAME_BYTES];
    static int16_t  stereo[WT_FRAME_SAMP * 2];
    static int16_t  mono[WT_FRAME_SAMP];
    static uint8_t  rx[HDR_LEN + WT_FRAME_BYTES + 64];
    /* Its own buffer rather than reusing mono[], which the TX path uses as its
     * frame accumulator. They never overlap in time today — half-duplex sees to
     * that — but 640 bytes is cheap next to depending on it. */
    static int16_t  vox_pcm[WT_FRAME_SAMP];
    int64_t last_beacon = 0;
    size_t  fill = 0;              /* samples accumulated toward one frame */

    while (1) {
        if (s_want_open) {
            s_want_open = false;
            wt_do_open();
            continue;
        }
        if (s_mode == WT_OFF || (!s_via_espnow && s_sock < 0)) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        int64_t now = esp_timer_get_time();
        /* One line a second, and only while something is actually flowing, so
         * a quiet channel does not fill the on-screen log. 50 frames/s is the
         * 20ms frame rate: anything less is the fault, and WHICH counter sags
         * says which end it is on. */
        if (now - s_stat_at > 1000000) {
            if (s_stat_tx || s_stat_rx) {
                char dac[48] = {0};
                if (s_stat_rx) audio_dac_state(dac, sizeof(dac));
                /* 🔑 PEER COUNT IS IN HERE ON PURPOSE. "tx 50/s to 0 peers" is
                 * the signature of the 2026-08-18 stale-peer bug and it was
                 * invisible for three test rounds without it. */
                applog("[wt] tx %d@%d%% rx %d@%d%% p%d amp%d w%d %dHz %s",
                       s_stat_tx, s_stat_txpk * 100 / 32767, s_stat_rx,
                       s_stat_rxpk * 100 / 32767, wt_peer_count(),
                       audio_amp_on() ? 1 : 0, (int)s_stat_werr, audio_rate(),
                       dac);
                /* 🔑 OVERRUNS ARE THE ONE NUMBER THAT SAYS "THE FAULT IS HERE".
                 * A dropout with 0 overruns was lost on the air; a dropout with
                 * overruns climbing means this task is not draining the ring at
                 * 50/s and the radio is innocent. Two very different fixes. */
                if (s_via_espnow)
                    applog("[wt] vox tx %lu rx %lu overrun %lu",
                           (unsigned long)s_vox_tx, (unsigned long)s_vox_rx,
                           (unsigned long)espnow_voice_overruns());
            }
            s_stat_tx = s_stat_rx = s_stat_txpk = s_stat_rxpk = 0;
            s_stat_at = now;
        }
        /* ⚠️ UDP ONLY. espnow.c beacons once a second on its own and keeps the
         * peer list this session reads; a second discovery mechanism on top of
         * it would be two sources of truth for one question. */
        if (!s_via_espnow && now - last_beacon > BEACON_US) {
            last_beacon = now;
            send_beacon();
        }

        /* ---- transmit ---- */
        if (s_ptt) {
            if (!s_tx) {
                s_tx = true;
                fill = 0;                      /* no fragments across presses */
                s_txpeak = 0;
                audio_agc_reset();
                audio_amp(false);              /* half-duplex: speaker off    */
                audio_flush_rx();              /* drop whatever DMA was holding */
                if (s_via_espnow) {
                    /* Fresh encoder per press. The far end resets too, so the
                     * first frame of a new press never depends on the last
                     * frame of the previous one — which may have been minutes
                     * ago, or may never have arrived. */
                    s_enc.predictor = 0;
                    s_enc.index     = 0;
                    char nm[VOX_NAME] = { 0 };
                    strlcpy(nm, s_id, sizeof(nm));
                    vox_tx(VOX_START, NULL, nm, VOX_NAME);
                } else {
                    uint8_t hdr[HDR_LEN];
                    send_all(hdr, build(hdr, T_START, NULL, 0));
                }
                applog("[wt] talking to %d peer(s)", wt_peer_count());
            }
            /* 🚨 ONLY EVER SEND A FULL 20ms FRAME.
             *
             * audio_read() returns whatever the DMA happens to hold, which is
             * usually a partial buffer. Sending those straight out produced
             * variable-length packets, and the far end — which reasonably
             * assumes one packet is one 20ms frame — then miscounted the
             * stream: 128 packets reported as 2.6s of audio that was really
             * 1.92s. A jitter buffer built on that would drift permanently.
             * Accumulate, then send exactly WT_FRAME_SAMP samples. */
            size_t got = 0;
            audio_read(stereo, sizeof(stereo), &got, 60);
            size_t frames = got / (2 * sizeof(int16_t));
            for (size_t i = 0; i < frames; i++) {
                /* LEFT slot only — the mono mic is on the left and the right
                 * reads exactly 0 on this board. */
                int16_t v = stereo[i * 2];
                /* 🔑 PEAK ON THE WALKIE PATH TOO. mic.c logs one for the Talk
                 * capture and this path had none, so an ALC A/B run over the
                 * walkie — which is how the builder actually tests it — produced no
                 * number at all, only an impression. */
                int a = v < 0 ? -v : v;
                if (a > s_txpeak) s_txpeak = a;
                mono[fill++] = v;
                if (fill == WT_FRAME_SAMP) {
                    for (int k = 0; k < WT_FRAME_SAMP; k++) {
                        int m = mono[k] < 0 ? -mono[k] : mono[k];
                        if (m > s_stat_txpk) s_stat_txpk = m;
                    }
                    if (s_via_espnow) {
                        /* 🔑 SNAPSHOT BEFORE ENCODING. The header must carry the
                         * state the decoder needs to START this frame, not the
                         * state encoding it left behind. Getting this backwards
                         * would decode every frame from the wrong predictor and
                         * sound like gravel. */
                        adpcm_state_t snap = s_enc;
                        uint8_t comp[VOX_BYTES];
                        adpcm_encode(&s_enc, mono, comp, WT_FRAME_SAMP);
                        vox_tx(VOX_AUDIO, &snap, comp, VOX_BYTES);
                        s_vox_tx++;
                    } else {
                        send_all(pkt, build(pkt, T_AUDIO, mono, WT_FRAME_BYTES));
                    }
                    s_stat_tx++;
                    fill = 0;
                }
            }
            /* 🚨 DO NOT `continue` HERE, AND THIS IS THE WHOLE BUG.
             *
             * This used to skip straight back to the top while the button was
             * held, so a TRANSMITTING board never read its socket — and
             * note_peer() only runs from there. send_all() drops any peer whose
             * last-seen is older than PEER_TTL_US (8s), so 6-8s into a press
             * (8s minus however old the peer's last 2s beacon already was) the
             * peer went stale and every frame was sent to NOBODY.
             *
             * The builder, 2026-08-18: "i counted to 6 and it cut out", on both
             * handsets, in both directions. The frame counters said tx 50/s the
             * whole time because s_stat_tx++ runs after send_all() whether or
             * not anything was in date to send to — a sender talking to an
             * empty peer list looks IDENTICAL to a healthy one. */
        } else if (s_tx) {
            s_tx = false;
            if (s_via_espnow) {
                /* 🚨 SENT TWICE, AND NOT BECAUSE ONE MIGHT FAIL TO SEND.
                 * It is an unacknowledged broadcast and it is the frame that
                 * closes the far end's amp. Losing it leaves a sibling's
                 * speaker on until the 1.5s silence timeout notices — audible
                 * as a hiss after every single press. Two costs 24 bytes. */
                vox_tx(VOX_END, NULL, NULL, 0);
                vox_tx(VOX_END, NULL, NULL, 0);
            } else {
                uint8_t hdr[HDR_LEN];
                send_all(hdr, build(hdr, T_END, NULL, 0));
            }
            applog("[wt] done talking, peak %d (%d%% FS) agc x%.1f", s_txpeak,
                   s_txpeak * 100 / 32767, audio_agc_gain());
            /* Does the servo actually MOVE a gain register? 0x16 is the one to
             * watch: if ALC drives it, this dump differs from the boot one. */
            if (audio_alc()) audio_dump_codec_regs("post-tx ");
        }

        /* ---- receive ---- */
        if (s_via_espnow) {
            uint8_t rxv[VOX_MAX];
            int n = espnow_voice_take(rxv, sizeof(rxv));
            if (n >= (int)VOX_HDR) {
                vox_hdr_t h;
                memcpy(&h, rxv, VOX_HDR);
                if (s_tx) {
                    /* Half-duplex, exactly as the UDP path does it: while WE
                     * are talking an incoming frame is never played. espnow.c
                     * already keeps the peer list alive, so there is nothing
                     * else to do with it either. */
                } else if (h.type == VOX_START) {
                    /* Somebody else already has the channel: let them finish.
                     * The stale timeout below is what frees it if their END
                     * never arrives, so this can never wedge. */
                    if (s_hearing_id && s_hearing_id != h.from_id) {
                        applog("[wt] %s tried to talk over %s - ignored",
                               vox_name_for(h.from_id), s_hearing);
                        goto after_rx;
                    }
                    char nm[VOX_NAME] = { 0 };
                    if (n >= (int)VOX_HDR + VOX_NAME)
                        memcpy(nm, rxv + VOX_HDR, VOX_NAME - 1);
                    if (!nm[0]) strlcpy(nm, vox_name_for(h.from_id), sizeof(nm));
                    strlcpy(s_hearing, nm, sizeof(s_hearing));
                    s_hearing_id = h.from_id;
                    s_hearing_at = now;
                    audio_amp(player_out_sink() == SINK_SPEAKER);
                    applog("[wt] %s is talking (esp-now)", nm);
                } else if (h.type == VOX_AUDIO &&
                           n >= (int)VOX_HDR + VOX_BYTES) {
                    /* Same rule for audio, because a lost START must not let a
                     * second voice in through the recovery path below. */
                    if (s_hearing_id && s_hearing_id != h.from_id) goto after_rx;
                    /* 🔑 NO NAME IN THE AUDIO FRAME, SO RECOVER ONE. If START
                     * was the frame that got lost we would otherwise play a
                     * sibling's voice with nothing on screen to say whose. */
                    if (!s_hearing[0]) {
                        strlcpy(s_hearing, vox_name_for(h.from_id),
                                sizeof(s_hearing));
                        s_hearing_id = h.from_id;
                    }
                    s_hearing_at = now;

                    /* State straight off the wire — adpcm_decode() clamps the
                     * index, which is the only field a corrupt frame could use
                     * to read outside the step table. */
                    adpcm_state_t dec = { .predictor = h.predictor,
                                          .index     = h.index };
                    adpcm_decode(&dec, rxv + VOX_HDR, vox_pcm, WT_FRAME_SAMP);

                    for (int i = 0; i < WT_FRAME_SAMP; i++) {
                        int16_t v = vox_pcm[i];
                        stereo[i * 2]     = v;
                        stereo[i * 2 + 1] = v;    /* codec is opened stereo */
                        int m = v < 0 ? -v : v;
                        if (m > s_stat_rxpk) s_stat_rxpk = m;
                    }
                    player_sink_t sk = player_out_sink();
                    audio_amp(sk == SINK_SPEAKER);
                    s_stat_werr = audio_write(sk, stereo,
                                              WT_FRAME_SAMP * 2 * sizeof(int16_t));
                    s_stat_rx++;
                    s_vox_rx++;
                } else if (h.type == VOX_END) {
                    /* Only the handset that HAS the channel can end it —
                     * otherwise a sibling's stray END cuts off the person
                     * actually speaking. */
                    if (s_hearing_id && s_hearing_id != h.from_id) goto after_rx;
                    applog("[wt] %s done", s_hearing[0] ? s_hearing
                                                        : vox_name_for(h.from_id));
                    s_hearing[0]  = 0;
                    s_hearing_id  = 0;
                    audio_amp(false);
                }
            } else if (!s_tx) {
                vTaskDelay(1);           /* nothing waiting: yield a tick */
            }
            goto after_rx;
        }

        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s_sock, rx, sizeof(rx), MSG_DONTWAIT,
                         (struct sockaddr *)&from, &fl);
        if (n >= HDR_LEN && !memcmp(rx, MAGIC, 3)) {
            char name[17] = {0};
            memcpy(name, rx + 8, 16);
            if (strcmp(name, s_id) != 0) {     /* ignore our own broadcast */
                note_peer(from.sin_addr.s_addr, name);   /* the point of the drain */
                int kind = rx[3];
                if (s_tx) {
                    /* Half-duplex: while WE are talking an incoming packet is
                     * good for exactly one thing, keeping the peer alive, and
                     * note_peer() just did it. Never play it, never touch the
                     * amp. */
                } else if (kind == T_START) {
                    strlcpy(s_hearing, name, sizeof(s_hearing));
                    s_hearing_at = esp_timer_get_time();
                    audio_amp(player_out_sink() == SINK_SPEAKER);
                    applog("[wt] %s is talking", name);
                } else if (kind == T_AUDIO && !s_tx) {
                    /* Not while transmitting: half-duplex, and playing our own
                     * echo back into a live mic is the one thing PTT exists to
                     * avoid. */
                    strlcpy(s_hearing, name, sizeof(s_hearing));
                    s_hearing_at = esp_timer_get_time();
                    int payload = n - HDR_LEN;
                    int samples = payload / 2;
                    const int16_t *in = (const int16_t *)(rx + HDR_LEN);
                    for (int i = 0; i < samples && i < WT_FRAME_SAMP; i++) {
                        stereo[i * 2] = in[i];
                        stereo[i * 2 + 1] = in[i];   /* codec is opened stereo */
                    }
                    for (int k = 0; k < samples && k < WT_FRAME_SAMP; k++) {
                        int m = in[k] < 0 ? -in[k] : in[k];
                        if (m > s_stat_rxpk) s_stat_rxpk = m;
                    }
                    player_sink_t sk = player_out_sink();
                    audio_amp(sk == SINK_SPEAKER);
                    s_stat_werr = audio_write(sk, stereo,
                                              samples * 2 * sizeof(int16_t));
                    s_stat_rx++;
                } else if (kind == T_END) {
                    applog("[wt] %s done", name);
                    s_hearing[0] = 0;
                    s_hearing_id = 0;
                    audio_amp(false);
                }
            }
        } else if (!s_tx) {
            vTaskDelay(1);                     /* nothing waiting: yield a tick */
        }
        /* ⚠️ No yield while transmitting: audio_read() below already blocks on
         * the mic, and a tick of sleep per iteration there would starve the
         * 20ms frame cadence. */

after_rx:
        /* Stale talker (their END was lost) — do not sit "hearing" forever.
         * ⚠️ THIS IS LOAD-BEARING FOR ESP-NOW, not just a tidy-up. VOX_END is an
         * unacknowledged broadcast; if both copies are lost this timeout is the
         * ONLY thing that closes the amp. */
        if (s_hearing[0] && esp_timer_get_time() - s_hearing_at > 1500000) {
            s_hearing[0]  = 0;
            s_hearing_id  = 0;      /* releases the channel to the next talker */
            audio_amp(false);
        }

        /* ⚠️ ASCII ONLY. The separator here was "·" (U+00B7) and LVGL's
         * built-in Montserrat subset does not contain it, so the screen showed
         * a hollow box. Anything drawn with the stock fonts must stay inside
         * plain ASCII unless a glyph is deliberately added to the font. */
        /* The radio is named only when it is the new one, so the wifi session
         * reads exactly as it always has — and so the builder can tell at a glance
         * which path he is actually testing. */
        /* 🚨 SAY IT WHEN THE FLEET IS SPLIT. On the UDP channel with nobody on
         * it, while ESP-NOW can plainly see siblings RIGHT THERE, the handsets
         * are on two different radios and no amount of pressing Talk will
         * bridge them. That exact state cost an afternoon on 2026-08-26 while
         * both screens showed nothing but a peer count. Only reachable now by
         * forcing the modes apart, since auto always picks ESP-NOW — but that
         * is precisely when somebody needs telling. */
        if (!s_via_espnow && wt_peer_count() == 0 && espnow_play_count() > 0)
            snprintf(s_status, sizeof(s_status),
                     "on wifi - others are on esp-now");
        /* 🚨 SAY IT WHEN THE CHOSEN LISTENER CANNOT HEAR. Talking to one person
         * who has gone out of range looks EXACTLY like talking to someone who
         * is there — the button goes red, the mic level moves, and nothing
         * says the frames went nowhere. This is the one-to-one version of the
         * split-fleet trap above, and it costs the same afternoon. */
        else if (s_tx_unreachable)
            snprintf(s_status, sizeof(s_status), "%s cannot hear you",
                     vox_name_for(s_target));
        else
            snprintf(s_status, sizeof(s_status), "%s - %d peer%s%s",
                     s_tx ? "TALKING" : (s_hearing[0] ? s_hearing : "listening"),
                     wt_peer_count(), wt_peer_count() == 1 ? "" : "s",
                     s_via_espnow ? " esp-now" : "");
    }
}

/* ------------------------------------------------------------------ */

/* 🚨 CALLED FROM AN LVGL EVENT — MUST NOT BLOCK.
 *
 * This used to call net_up() inline, which takes 2-24 SECONDS. The interface
 * froze on entering the Radio screen ("it took a really long time to get
 * there") and then reported "no wifi" once both 12s attempts had expired. The
 * same mistake voice_press() made. Set a flag; let the task do the work. */
void wt_open(void) { s_want_open = true; }

/* Bring the channel up over ESP-NOW: no AP, no socket, no association wait.
 * Everything it needs is already running — espnow_begin() holds the WiFi driver
 * up for good and the peer list is already populated from beacons. */
static bool open_espnow(void)
{
    if (!espnow_running()) {
        strlcpy(s_status, "no esp-now", sizeof(s_status));
        applog("[wt] esp-now is not running");
        return false;
    }
    s_via_espnow = true;
    s_enc.predictor = 0;
    s_enc.index     = 0;
    s_vox_seq = 0;
    applog("[wt] channel open as %s over ESP-NOW (%d nearby)",
           s_id, espnow_play_count());
    return true;
}

static void wt_do_open(void)
{
    if (s_mode == WT_OPEN) return;

    strlcpy(s_status, "connecting...", sizeof(s_status));
    s_was_playing = player_suspend();
    s_via_espnow = false;

    /* 🎙 WHICH RADIO. See the note on wt_radio_t in settings.h for why auto
     * prefers wifi rather than the peer-to-peer link it is named for. */
    const int pref = settings_wt_radio();

    if (pref == WT_RADIO_ESPNOW) {
        if (!open_espnow()) { player_unsuspend(s_was_playing); return; }
        goto opened;
    }

    /* 🚨🚨 AUTO PREFERS ESP-NOW, AND THE FIRST VERSION OF THIS PREFERRED WIFI.
     * THAT SPLIT THE FLEET DOWN THE MIDDLE.
     *
     * The rule was "wifi if it is there, ESP-NOW if it is not" — chosen to keep
     * the CYD, which speaks only the UDP format, able to hear everyone at home.
     * It is wrong, and the builder found it within minutes on 2026-08-26:
     *
     *     Kid 1    listening - 2 peers esp-now      <- net_up() failed, fell back
     *     Kid 2  listening - 0 peers              <- net_up() worked, stayed UDP
     *
     * 🔑 THE DECISION IS PER-HANDSET BUT THE CHANNEL IS SHARED. Two boards on
     * the same shelf disagreed about which radio the channel was on, because
     * wifi association is FLAKY and each one asked independently. Kid 2's "0
     * peers" was not a discovery bug — it was correct, and Kid 1 genuinely was
     * not there to be found. Neither screen said anything was wrong.
     *
     * 🔑 ESP-NOW IS THE ONLY TRANSPORT EVERY HANDSET ALWAYS HAS. espnow_begin()
     * holds the driver up from boot and never lets go, so it cannot fail
     * intermittently the way an association can. Preferring the RELIABLE radio
     * is what makes every handset reach the same answer without having to
     * negotiate — the derive-don't-negotiate rule this project already runs on.
     *
     * It is also simply better: the channel opens instantly instead of costing
     * a 2-24s net_up(), and nobody associates, which stops an idle sibling
     * punching holes in channel 6 (the "bong" in aud_wire.h's note on busy).
     *
     * ⚠️ THE ONE ARGUMENT FOR THE OLD RULE IS GONE. Preferring wifi existed only
     * to keep the CYD — which speaks the UDP format and not VOX — in the
     * conversation at home. The builder retired the CYD on 2026-08-26 and the fleet is
     * S3-only, so there is now nothing on the UDP channel to protect.
     *
     * ⏭ WHICH MAKES THE UDP PATH DEAD WEIGHT. Everything in this file guarded
     * by `!s_via_espnow` — the socket, send_all(), note_peer(), the WT1 beacon,
     * s_peers[] — now exists only for a fallback nobody reaches. Deleting it
     * would take a few hundred lines and a screen row out of the project. NOT
     * done here: the ESP-NOW path is one evening old and the fallback is the
     * only way back if it disappoints. Revisit once it has a week on it. */
    if (pref == WT_RADIO_AUTO && espnow_running()) {
        if (open_espnow()) goto opened;
    }

    /* The user pressed Talkie. Whatever the away backoff thinks, try for real. */
    net_retry_reset();
    if (!net_up()) {
        strlcpy(s_status, "no wifi", sizeof(s_status));
        applog("[wt] no wifi");
        player_unsuspend(s_was_playing);
        return;
    }
    /* Ours until wt_close(). Without this a Talk round trip's net_down() takes
     * the channel down mid-session and the Radio screen goes to "no wifi". */
    net_hold();

    s_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_sock < 0) {
        strlcpy(s_status, "no socket", sizeof(s_status));
        return;
    }
    int on = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(s_sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in me = {
        .sin_family = AF_INET,
        .sin_port = htons(WT_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_sock, (struct sockaddr *)&me, sizeof(me)) < 0) {
        applog("[wt] bind failed");
        close(s_sock);
        s_sock = -1;
        strlcpy(s_status, "bind failed", sizeof(s_status));
        return;
    }

    applog("[wt] channel open as %s on udp/%d", s_id, WT_PORT);

opened:
    /* 16kHz for the channel. Music runs at 44100 and gets it back on close. */
    audio_set_rate(WT_RATE);
    memset(s_peers, 0, sizeof(s_peers));
    s_hearing[0] = 0;
    s_vox_rx = s_vox_tx = 0;
    s_mode = WT_OPEN;
    strlcpy(s_status, "listening", sizeof(s_status));
}

void wt_close(void)
{
    if (s_mode == WT_OFF) return;
    s_mode = WT_OFF;
    s_ptt = false;
    s_tx = false;
    if (s_sock >= 0) { close(s_sock); s_sock = -1; }
    audio_amp(false);
    /* ⚠️ ONLY IF WE TOOK ONE. An ESP-NOW session never called net_up() and never
     * held the link, so releasing here would decrement somebody else's hold and
     * pull the radio out from under whatever is using it. */
    if (!s_via_espnow) {
        net_release();          /* pairs with the hold in wt_do_open() */
        net_down();
    }
    s_via_espnow = false;
    audio_set_rate(44100);
    player_unsuspend(s_was_playing);
    s_was_playing = false;
    strlcpy(s_status, "off", sizeof(s_status));
    applog("[wt] channel closed");
}

void wt_init(void)
{
    /* Name the handset after its MAC tail so two boards are never both
     * "communicator" on the channel. */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    /* The name is what the OTHER handset displays, so it is worth more than the
     * MAC here: "Boy is talking" beats "comm-XXXX is talking" for a child. Falls
     * back to the MAC when unset, which keeps every board unique out of the box
     * and keeps two unnamed boards from colliding. */
    const char *nm = settings_name();
    if (nm && nm[0]) snprintf(s_id, sizeof(s_id), "%s", nm);
    else             snprintf(s_id, sizeof(s_id), "comm-%02X%02X", mac[4], mac[5]);

    /* Core 1, away from the LVGL loop on core 0 — the same lesson the voice
     * task taught: a task that shares a core with the interface can take the
     * interface down with it. */
    xTaskCreatePinnedToCore(wt_task, "wt", 6144, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "walkie-talkie ready as %s", s_id);
}

/* ⚠️ s_id is read by build() on the TX path, which runs on the wt task while
 * this runs on the LVGL task. A torn read is possible in theory and harmless
 * in practice: the worst case is one beacon carrying a half-written name, and
 * the next one 2s later corrects it. A lock here would be worth more than the
 * bug it prevents. Named rather than left as a silent assumption. */
void wt_set_name(const char *n)
{
    if (!n || !n[0]) return;
    if (!strcmp(s_id, n)) return;
    snprintf(s_id, sizeof(s_id), "%s", n);
    applog("[wt] channel id is now %s", s_id);
}
