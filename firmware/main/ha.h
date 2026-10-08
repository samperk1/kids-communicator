#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Home Assistant client.
 *
 * 🚨 VOICE GOES THROUGH HOME ASSISTANT, NOT THE RELAY. VOICE-PLAN.md is written
 * around the relay's /transcribe and /ask and is out of date on exactly this
 * point. HA already has speech-to-text, an agent and text-to-speech; the
 * "missing TTS endpoint" was never missing. */

bool ha_post_json(const char *path, const char *body);

/* Telemetry and messaging, to an HA webhook that needs NO credential.
 *
 * 🚨 THIS REPLACED ha_post_json_admin(). POST /api/states genuinely requires
 * admin — HA's APIEntityStateView.post checks is_admin, measured 401 on
 * 2026-09-10 — so the old path needed rights no handset should ever hold. The
 * webhook needs none. `hook_id` is per device (comm_<id>_battery, and so on);
 * see the note over the implementation for why it must not be shared. */
bool ha_post_webhook(const char *hook_id, const char *body);
bool ha_post_audio(const char *path, const void *wav, size_t len,
                   const char *speech_content);
/* Fetches into PSRAM. Caller owns the buffer via ha_take_binary(). */
bool ha_get_binary(const char *path);

int         ha_status(void);
const char *ha_body(void);

uint8_t *ha_take_binary(size_t *len);   /* hands over ownership, clears ours */
void     ha_free_binary(void);

/* Pulls one string value out of a flat-ish JSON body. Deliberately not a real
 * parser: the four fields this device reads ("text", "speech", "path", "url")
 * are all plain strings and a full parser would cost more RAM than the replies
 * themselves. */
bool ha_json_str(const char *json, const char *key, char *out, size_t len);
/* Escapes a string so it can be dropped into a JSON body. A transcript with a
 * quote or a backslash in it would otherwise produce a malformed request that
 * HA rejects with a confusing 400. */
void ha_json_escape(const char *in, char *out, size_t len);

/* GET a JSON body into ha_body(), for reading HA state.
 *
 * 🔑 THE HANDSET ONLY EVER PULLS WHAT IS NEW, NEVER THE WHOLE THREAD.
 * ha.c's body buffer is 2048 bytes of INTERNAL RAM, and a 30-message thread
 * would not fit — nor should it, because the handset already holds the full
 * history on its SD card. HA keeps the thread for Lovelace's benefit; the
 * device pulls one message at a time and merges locally. Keep every path that
 * uses this small.
 *
 * `admin` picks the identity, exactly as the POST pair does. */
bool ha_get_json(const char *path, bool admin);
