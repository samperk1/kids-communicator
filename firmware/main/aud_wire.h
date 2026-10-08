/* ------------------------------------------------------------------ *
 * The gate-3 audio wire format, shared by BOTH ends.
 *
 * The sender is the S3 handset (ESP-IDF, communicator/main/espnow_audio.c),
 * the receiver is the WROVER dongle (Arduino, src/bt_gate3.cpp). Two toolchains,
 * one header, copied byte for byte — the same discipline msg_core.cpp and
 * espnow.c already follow for the messaging frame.
 *
 * 🔑 COMPRESSED ON THE WIRE, NOT PCM. 44.1kHz stereo PCM is 176 KB/s and ESP-NOW
 * cannot carry it. The MP3 is ~16 KB/s and the handset already has those exact
 * bytes on its SD card, so forwarding them is cheaper for the sender than
 * decoding would be. The dongle does the work because the dongle has PSRAM and
 * nothing else to do.
 *
 * ⚠️ 250-BYTE ESP-NOW PAYLOAD. The CYD's Arduino core is on IDF 5.4 and can do
 * ESP-NOW v2 (1428 bytes); this project's S3 is on IDF 5.3 and cannot. 250 is
 * what BOTH ends can always do, so the format is sized for it.
 * ------------------------------------------------------------------ */
#ifndef AUD_WIRE_H
#define AUD_WIRE_H

#include <stdint.h>

#define AUD_MAGIC   0x31445541u        /* "AUD1" */
#define AUD_HDR     8
#define AUD_PAYLOAD 242                /* 250 - AUD_HDR */

enum {
    AUD_START = 0,     /* new track; receiver resets its buffers and decoder */
    AUD_DATA  = 1,     /* raw MP3 bytes, in file order */
    AUD_END   = 2,     /* end of track; receiver DRAINS what it holds, then stops */
    AUD_STOP  = 3,     /* pause/stop: receiver DISCARDS what it holds, at once.
                        * ⚠️ Distinct from AUD_END on purpose. The dongle banks
                        * ~5 seconds, so on a pause an "end" that drains would
                        * keep playing for five seconds after the button — which
                        * is indistinguishable from pause not working. */
    AUD_CTL   = 4,     /* dongle -> handset: a button or tap on the headphones.
                        * payload[0] is one of the CTL_* codes below.
                        *
                        * 🔑 THE CONTROL PATH IS THE AUDIO PATH, BACKWARDS. The
                        * A2DP library already registers an AVRCP *target*, so
                        * PLAY/PAUSE/FORWARD/BACKWARD arrive at the dongle from
                        * the headset. The dongle has no idea what a playlist is
                        * — it just relays the press to whichever handset is
                        * feeding it, which already knows how to act on it. */
    AUD_STAT  = 6,     /* dongle -> handset: "I am still here, and this is my
                        * state". Sent about once a second.
                        *
                        * 🚨 WITHOUT THIS THE HANDSET STREAMS INTO A VOID. The
                        * only frame that ever came BACK was AUD_CTL, and that
                        * only when a finger touched the headset. So when the
                        * dongle lost its Bluetooth link — or was unplugged —
                        * the handset carried on decoding and transmitting at
                        * ~25 KB/s to nobody, and the child saw a progress bar
                        * moving with no sound. The builder hit exactly that.
                        *
                        * Two things make it useful: `bt` says whether the
                        * dongle actually has headphones on the other end, and
                        * the ARRIVAL of the frame at all says the dongle is
                        * alive. The handset needs both — a dongle that has
                        * crashed sends nothing, so silence must count as
                        * "gone" just as much as bt == 0 does. */
    AUD_CMD   = 7,     /* handset -> dongle: change the Bluetooth headphones.
                        *
                        * 🔑 THE DONGLE HAS NO SCREEN, SO THE HANDSET IS ITS UI.
                        * Until now the headsets it would accept were three
                        * hardcoded name fragments compiled into the dongle, so
                        * pairing a new pair of headphones meant editing C and
                        * flashing over a cable. Payload is aud_cmd_t. */
    AUD_DEV   = 8,     /* dongle -> handset: ONE device seen while scanning.
                        *
                        * Sent one per frame rather than as a list: a frame is
                        * 250 bytes, a device is 28, and the handset wants to
                        * show them as they appear rather than after the scan
                        * finishes. Payload is aud_dev_t. */
    AUD_VOL   = 5,     /* handset -> dongle: playback volume, payload[0] = 0..100.
                        *
                        * 🔑 NEEDED BECAUSE THE STREAM IS UNTOUCHED MP3. The
                        * handset forwards the encoded bytes exactly as they sit
                        * on the SD card — which is what makes the sender cheap —
                        * so its volume control cannot affect them. The only
                        * place volume can be applied is where the audio becomes
                        * sound, at the dongle. */
};

/* Codes carried in an AUD_CTL payload. Deliberately fewer than AVRCP defines:
 * a headset sends PLAY when it thinks we are paused and PAUSE when it thinks we
 * are playing, and a single toggle is correct in both cases without the two
 * ends having to agree on state. */
enum {
    CTL_PLAYPAUSE = 0,
    CTL_NEXT      = 1,
    CTL_PREV      = 2,
    CTL_STOP      = 3,
};

/* What an AUD_CMD asks the dongle to do. */
enum {
    BTC_SCAN   = 0,    /* forget the target and report everything you see, so
                        * the handset can offer a list. The dongle keeps
                        * scanning until something is picked. */
    BTC_PICK   = 1,    /* connect to mac[] and REMEMBER it across reboots */
    BTC_FORGET = 2,    /* drop the remembered headset and go back to scanning */
    BTC_RETRY  = 3,    /* try the link again NOW, keeping the remembered headset.
                        * For a headset that powered itself off mid-track, and
                        * for the dongle's own discovery loop giving up - see
                        * the note on BTC_SCAN in bt_gate3.cpp. */
};

typedef struct __attribute__((packed)) {
    uint8_t cmd;       /* one of BTC_* */
    uint8_t mac[6];    /* BTC_PICK: which device. Ignored otherwise. */
} aud_cmd_t;

/* One device the dongle saw while scanning.
 *
 * ⚠️ The MAC is the identity, not the name. Two pairs of the same cheap
 * earbuds carry the same name, and a name can be empty on the first inquiry
 * response — so the handset shows the name and picks by MAC. */
typedef struct __attribute__((packed)) {
    uint8_t mac[6];
    int8_t  rssi;
    uint8_t current;   /* 1 = this is the remembered/connected one */
    char    name[20];  /* may be empty; the handset shows the MAC then */
} aud_dev_t;

/* The payload of an AUD_STAT frame. Kept small and fixed: it rides the same
 * 250-byte ESP-NOW limit as everything else, and it is sent once a second for
 * as long as a handset is streaming. */
typedef struct __attribute__((packed)) {
    uint8_t  bt;         /* 1 = A2DP link to the headphones is up, 0 = not */
    uint8_t  vol;        /* what the dongle currently has set, 0..100 */
    uint16_t underruns;  /* buffer underruns since boot — audible as dropouts */
    uint16_t ring_pct;   /* how full the dongle's MP3 ring is, 0..100 */
    char     sink[20];   /* headset name, NUL-terminated and truncated */
    uint8_t  scanning;   /* 1 = looking for headphones, so the handset can say so
                          * instead of showing a silent empty list */
    uint8_t  have_target;/* 1 = a headset is remembered in NVS */
    uint8_t  busy;       /* 1 = audio is arriving from SOME handset right now.
                          *
                          * 🚨 FOR THE HANDSETS THAT ARE **NOT** PLAYING. The
                          * telemetry loop already skips its 33-second wifi
                          * association while THIS handset is streaming - but
                          * the airtime is shared, not local. The house AP and
                          * ESP-NOW are both on channel 6, so an IDLE sibling
                          * associating punches a hole in someone else's music.
                          * The builder heard it as a periodic "bong" while Kid 1 played
                          * and Guest sat doing nothing. A handset cannot see a
                          * sibling's state, but it can see the dongle's. */
} aud_stat_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;    /* AUD_MAGIC */
    uint8_t  type;     /* AUD_START / AUD_DATA / AUD_END */
    uint8_t  seq;      /* rolling; the receiver only uses it to COUNT loss */
    uint16_t len;      /* payload bytes that follow */
    /* uint8_t payload[len] */
} aud_hdr_t;

#endif
