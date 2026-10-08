/*
 * Text messages, handset side.
 *
 * WHY THIS SHAPE: The builder already had an `input_text.kids_message` helper sitting in
 * HA doing nothing, and was rightly unimpressed — "i made that other helper kids
 * messaage and it does nothing". A helper IS inert. It is a box that holds a
 * string; it does something only when a device reads it. This is that device.
 *
 * 🔑 last_changed IS THE SEQUENCE NUMBER, SO NO SECOND HELPER IS NEEDED.
 * The obvious design has an input_number alongside the text that the sender must
 * remember to bump — one more helper to create, and one more thing to get out of
 * step. HA already stamps every state with `last_changed`, and it changes
 * exactly when the text does. Comparing that string against the last one seen is
 * the whole of new-message detection.
 *
 * ⚠️ It changes when the state CHANGES, not when it is written. Setting the
 * helper to the same text twice does not move it — so sending the identical
 * message twice in a row is invisible. Correct for "is there something new",
 * surprising if you are testing by hand with the same string. Use different
 * text, or watch for the beep rather than the log.
 */

#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "applog.h"
#include "audio.h"
#include "ha.h"
#include "player.h"
#include "esp_mac.h"
#include "net.h"
#include <stdio.h>
#include "msg.h"
#include "settings.h"

/* HA's timestamps are ISO-8601 with microseconds and a zone:
 * "2026-08-17T20:13:01.540715+00:00" — 32 chars plus the terminator. */
#define STAMP_MAX   40
#define TEXT_MAX    128         /* the helper is capped at 100 by HA */

#define NVS_NS      "msg"
#define NVS_STAMP   "last"

static char s_stamp[STAMP_MAX];     /* last_changed we have already shown */
static char s_last_text[TEXT_MAX];
static bool s_have_new;
static int  s_missed;      /* how many arrived since the last look */

/* Defined with the rest of the inbox below; used by the arrival paths and by
 * msg_init(), both of which come first in this file. */
static void inbox_add(const char *text);
static void inbox_load(void);

static void stamp_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof(s_stamp);
    nvs_get_str(h, NVS_STAMP, s_stamp, &len);
    nvs_close(h);
}

/* 🔑 PERSISTED, NOT JUST HELD IN RAM. Kept only in memory, every reboot would
 * re-announce the last message still sitting in the helper — and a handset that
 * beeps an old message at you every time it starts is worse than one that misses
 * a new one. */
static void stamp_save(const char *stamp)
{
    strlcpy(s_stamp, stamp, sizeof(s_stamp));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_STAMP, s_stamp);
    nvs_commit(h);
    nvs_close(h);
}

/* comm_XXXX, from the MAC — the same id telemetry and wt.c use, so one handset
 * is one identity everywhere and two boards can never collide. */
static const char *dev_id(void)
{
    static char id[16];
    if (!id[0]) {
        uint8_t mac[6] = { 0 };
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(id, sizeof(id), "comm_%02x%02x", mac[4], mac[5]);
    }
    return id;
}

/* 🔑 WHO THIS HANDSET IS, IN WORDS.
 *
 * The name comes from Settings → Name, which is persisted, so the person
 * holding the handset can name it without a toolchain. An unnamed board falls
 * back to its MAC-derived comm_XXXX id — better than blank, unique, and it makes
 * a new handset obvious the moment it appears.
 *
 * (The family build this came from also had a hardcoded MAC -> name table for
 * boards that predated the setting. It is not needed here and is gone.) */
const char *msg_name_for(const char *id)
{
    return id;
}

const char *msg_dev_name(void)
{
    const char *n = settings_name();
    if (n && *n) return n;
    return dev_id();
}

/* 🔑 EACH HANDSET WRITES ITS OWN ENTITY, NOT A SHARED ONE.
 *
 * A single shared thread entity would mean two handsets racing to overwrite
 * each other, and neither would notice it had lost. One sensor per board, keyed
 * on the MAC, makes every write conflict-free — and the Lovelace card just
 * renders every sensor.comm_*_message it can find, so a third handset appears
 * with no configuration anywhere.
 *
 * 🔑 GOES TO AN HA WEBHOOK, CARRYING NO CREDENTIAL. It used to POST
 * /api/states, which genuinely requires admin — an admin token is full control
 * of the house, and it has no business on a child's handset. The webhook needs
 * no rights at all, and `local_only: true` keeps it to the LAN.
 *
 * ⚠️ Blocks for the length of an HTTP round trip. NEVER call from the UI thread. */
bool msg_send(const char *text)
{
    char esc[TEXT_MAX * 2];
    ha_json_escape(text, esc, sizeof(esc));

    /* Flat, not the /api/states envelope — see the note in telemetry.c. */
    char body[420];
    snprintf(body, sizeof(body),
             "{\"text\":\"%s\",\"from\":\"%s\"}",
             esc, msg_dev_name());

    /* ⚠️ ONE WEBHOOK PER HANDSET, exactly as the entity was per handset: two
     * boards sharing one would race to overwrite each other and neither would
     * notice it had lost. */
    char hook[80];
    snprintf(hook, sizeof(hook), "%s_message", dev_id());

    bool sent = ha_post_webhook(hook, body);
    int  st   = ha_status();
    if (sent && (st == 200 || st == 201)) {
        applog("[msg] sent \"%s\"", text);
        return true;
    }
    applog("[msg] send FAILED http %d", st);
    return false;
}

/* Reading the sibling handsets' messages out of Home Assistant is a family-
 * build feature: it needs WiFi and an HA token, and this build has neither.
 * Handset-to-handset text goes over ESP-NOW (espnow.c) instead. Kept as a
 * function so callers need no #if. */
bool msg_poll_peers(void)
{
    return false;
}

void msg_init(void)
{
    stamp_load();
    inbox_load();
    applog("[msg] inbox %s, last seen %s", MSG_INBOX_ENTITY,
           s_stamp[0] ? s_stamp : "(nothing yet)");
}

/* --- the inbox -------------------------------------------------------------
 *
 * One NVS blob, newest first. Small enough that rewriting the whole thing on
 * every arrival is simpler and safer than a ring with a head index - messages
 * arrive minutes apart, not milliseconds.
 *
 * ⚠️ VERSIONED. Old bytes read back as a new struct are garbage, and garbage in
 * an inbox looks exactly like a radio fault - the CYD learned that when its
 * entry gained a field. A format change wipes rather than misreads. */
#define INBOX_FMT   1
#define INBOX_TEXT  104            /* "Sender: " + HA's 100-char cap */

static char    s_inbox[MSG_INBOX_MAX][INBOX_TEXT];
static uint8_t s_inbox_n;

static void inbox_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "ibfmt", INBOX_FMT);
    nvs_set_u8(h, "ibn", s_inbox_n);
    nvs_set_blob(h, "ib", s_inbox, sizeof(s_inbox));
    nvs_commit(h);
    nvs_close(h);
}

static void inbox_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t fmt = 0;
    if (nvs_get_u8(h, "ibfmt", &fmt) == ESP_OK && fmt == INBOX_FMT) {
        size_t len = sizeof(s_inbox);
        if (nvs_get_blob(h, "ib", s_inbox, &len) != ESP_OK) len = 0;
        if (nvs_get_u8(h, "ibn", &s_inbox_n) != ESP_OK) s_inbox_n = 0;
        if (s_inbox_n > MSG_INBOX_MAX) s_inbox_n = 0;
    }
    nvs_close(h);
}

/* Newest first: the message you want is almost always the last one. */
static void inbox_add(const char *text)
{
    if (!text || !*text) return;
    int keep = s_inbox_n < MSG_INBOX_MAX ? s_inbox_n : MSG_INBOX_MAX - 1;
    for (int i = keep; i > 0; i--)
        memcpy(s_inbox[i], s_inbox[i - 1], INBOX_TEXT);
    strlcpy(s_inbox[0], text, INBOX_TEXT);
    if (s_inbox_n < MSG_INBOX_MAX) s_inbox_n++;
    inbox_save();
}

uint8_t     msg_inbox_count(void)        { return s_inbox_n; }
const char *msg_inbox_text(uint8_t i)    { return i < s_inbox_n ? s_inbox[i] : ""; }
void        msg_inbox_clear(void)        { s_inbox_n = 0; inbox_save(); }

bool msg_have_new(void)      { return s_have_new; }
const char *msg_text(void)   { return s_last_text; }
void msg_mark_read(void)     { s_have_new = false; s_missed = 0; }
int  msg_missed(void)        { return s_missed; }

/* Called with the radio ALREADY up — from the telemetry cycle, which brings
 * WiFi up every 5 minutes anyway. That is the whole reason the poll is free:
 * it adds an HTTP round trip to a link that was coming up regardless, not a
 * radio wake of its own. */
/* Pull one "key":"value" out of JSON starting at p, and return a pointer just
 * past it. ha_json_str() cannot be reused here: it always finds the FIRST match
 * in the body, and this walks a LIST of messages. */
static const char *next_str(const char *p, const char *key, char *out, size_t len)
{
    char pat[24];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *s = strstr(p, pat);
    if (!s) return NULL;
    s += strlen(pat);
    size_t i = 0;
    while (*s && *s != '"') {
        if (*s == '\\' && s[1]) s++;          /* \" and \\ land as themselves */
        if (i + 1 < len) out[i++] = *s;
        s++;
    }
    out[i] = 0;
    return *s ? s + 1 : s;
}

/* 🔑 ASK WHAT CHANGED SINCE LAST TIME, NOT WHAT IS IN THE BOX NOW.
 *
 * The first version read the helper's CURRENT state, so two messages sent
 * between two polls meant the first was never seen by anyone — it had already
 * been overwritten. The builder hit this immediately: his "this is a new test" was
 * replaced before the handset's next look and simply vanished.
 *
 * There was never any need to build a mailbox. HA's recorder has been storing
 * every state change all along, on MariaDB, and the history API hands back
 * exactly the messages missed since a given instant. So:
 *
 *   - nothing is lost to overwriting — every change is in the database
 *   - it survives an HA restart, unlike anything POSTed to /api/states
 *   - no new helper, no queue entity, no broker
 *
 * ⚠️ The reply is capped by ha.c's 2048-byte body buffer. A handset that has
 * been off for a day asks for a very long window and the tail is silently
 * dropped — which is SAFE here: the stamp only advances over messages actually
 * parsed, so the next poll resumes from where this one stopped rather than
 * skipping the remainder.
 *
 * ⚠️ '+' MUST BE ESCAPED. The stamp ends "+00:00", and an unescaped '+' is read
 * as a space by HA's URL handling, which yields a 400 on a request that looks
 * perfectly correct in the log. */
bool msg_poll(void)
{
    /* Nothing seen yet: adopt the current state quietly rather than replaying
     * whatever history happens to exist. */
    if (!s_stamp[0]) {
        if (!ha_get_json("/api/states/" MSG_INBOX_ENTITY, true) ||
            ha_status() != 200) {
            applog("[msg] first poll failed, http %d", ha_status());
            return false;
        }
        char stamp[STAMP_MAX] = { 0 }, text[TEXT_MAX] = { 0 };
        if (ha_json_str(ha_body(), "last_changed", stamp, sizeof(stamp))) {
            ha_json_str(ha_body(), "state", text, sizeof(text));
            stamp_save(stamp);
            strlcpy(s_last_text, text, sizeof(s_last_text));
            applog("[msg] starting from \"%s\" without announcing", text);
        }
        return false;
    }

    char since[STAMP_MAX * 2] = { 0 };
    for (const char *a = s_stamp; *a; a++) {
        if (*a == '+') strlcat(since, "%2B", sizeof(since));
        else { size_t n = strlen(since); since[n] = *a; since[n + 1] = 0; }
    }

    char path[200];
    snprintf(path, sizeof(path),
             "/api/history/period/%s?filter_entity_id=" MSG_INBOX_ENTITY
             "&minimal_response&no_attributes", since);

    /* 🔎 ALWAYS SAY WHAT WAS ASKED. A poll that finds nothing new is silent by
     * design, and that made "the handset is not receiving" indistinguishable
     * from "the handset never asked" — which cost an hour on 2026-08-17, with
     * one handset receiving and its twin silently not. One line a minute. */
    applog("[msg] poll since %s", s_stamp);

    if (!ha_get_json(path, true)) {
        applog("[msg] poll failed (transport)");
        return false;
    }
    if (ha_status() != 200) {
        /* 🚨 ALWAYS LOG THE STATUS. ha.c's finish() returns true whenever the
         * transport succeeded regardless of the code, so a 401 or a 400 for a
         * malformed timestamp looks exactly like success to the caller. */
        applog("[msg] poll http %d (since %s)", ha_status(), s_stamp);
        return false;
    }

    /* Walk every state change in the window. The first entry is the state as it
     * was AT the requested instant — i.e. the one already seen — and is skipped
     * by the timestamp test rather than by counting. */
    const char *p = ha_body();
    char text[TEXT_MAX], stamp[STAMP_MAX];
    int  count = 0, seen = 0;

    for (;;) {
        const char *q = next_str(p, "state", text, sizeof(text));
        if (!q) break;
        const char *r = next_str(q, "last_changed", stamp, sizeof(stamp));
        if (!r) break;                    /* truncated tail: stop cleanly */
        p = r;

        /* ISO-8601 sorts lexicographically, so a string compare is a time
         * compare as long as every stamp comes from the same source. */
        seen++;
        if (strcmp(stamp, s_stamp) <= 0) continue;

        stamp_save(stamp);
        if (!text[0]) continue;           /* someone cleared the box */

        strlcpy(s_last_text, text, sizeof(s_last_text));
        inbox_add(s_last_text);
        count++;
        applog("[msg] NEW: \"%s\"  (%s)", text, stamp);
    }

    /* 🚨 SELF-HEAL A STAMP THAT HAS RUN AHEAD OF REALITY.
     *
     * The stored stamp is the only thing deciding what counts as new, and it
     * lives in NVS across reboots and firmware changes. If it ever ends up
     * LATER than anything Home Assistant holds — a bad parse, a clock skew, an
     * entity that got rebuilt — then every future message compares as old and
     * the handset goes deaf permanently, while still reporting battery happily
     * so nothing looks wrong. Suspected on the Kid 1 handset 2026-08-17: HA to
     * Kid 2 worked, HA to Kid 1 silently did not, and both were polling fine.
     *
     * If a window that returned entries yielded nothing newer, the newest of
     * them IS the truth and our stamp is wrong. Adopt it and carry on. Costs
     * one silently-skipped message and restores a handset that would otherwise
     * need reflashing. */
    if (!count && seen > 0 && strcmp(stamp, s_stamp) < 0) {
        applog("[msg] stamp %s was ahead of HA's newest %s — resetting",
               s_stamp, stamp);
        stamp_save(stamp);
        return false;
    }

    if (!count) {
        applog("[msg] poll: %d entries, none newer", seen);
        return false;
    }

    s_have_new = true;
    s_missed   = count;
    if (count > 1)
        applog("[msg] %d messages arrived while the handset was not looking",
               count);

    applog("[msg] cue: sink=%s vol=%d dac_gain=%ddB",
           player_state()->sink == SINK_SPEAKER ? "speaker" : "jack",
           player_state()->volume, audio_dac_gain_db(audio_dac_gain_raw()));
    audio_earcon(CUE_MESSAGE);
    return true;
}
