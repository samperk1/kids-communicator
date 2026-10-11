#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The ASK_* result codes are part of this API, not an implementation detail. */
#include "gw_wire.h"

/* ------------------------------------------------------------------ *
 * Asking the GATEWAY instead of asking Home Assistant.
 *
 * 🚨 THIS IS WHAT LETS A HANDSET LEAVE THE HOUSE. Today voice.c holds the wifi
 * password and a Home Assistant token that cannot be scoped to entities — full
 * control of the house, in a child's pocket, at school. This path sends the
 * microphone audio to the gateway over ESP-NOW and the gateway does all four HA
 * calls, so the handset needs neither credential. Possession of the gateway is
 * the permission.
 *
 * 🔑 THE GATEWAY NEVER BUFFERS THE RECORDING. `GW-AAAA` is a plain ESP32 with
 * no PSRAM and one binary serves both gateways, so a 384KB WAV has nowhere to
 * live. It streams into the HA request as chunks arrive — which is exactly what
 * ha_post_audio() already does on this side, and which was measured on
 * 2026-09-08 to work at 50 KB/s for the full 12-second worst case.
 *
 * ⚠️ AND THAT IS WHY THIS SENDS A WINDOW RATHER THAN A BLAST. A streamed POST
 * cannot rewind: a byte written to HA is committed, so a gap cannot be repaired
 * after the fact. The gateway acks the lowest index it still needs and we never
 * run more than GW_ASK_WIN ahead of it. The SPEED test blasts and counts the
 * losses afterwards; this cannot, and the difference is not a detail.
 * ------------------------------------------------------------------ */

/* Send `wav` to the best gateway in range and wait for the transcript.
 *
 * Blocks for as long as the whole round trip takes, so it MUST run on the voice
 * task and never on the LVGL thread. Returns ASK_OK (gw_wire.h) with `heard`
 * filled, or one of the ASK_ERR_* codes — which are distinguished on purpose:
 * "no answer" covers a dead gateway, a dead HA and a dead token, and those are
 * three different jobs for whoever is holding the handset. */
int ask_run(const uint8_t *wav, size_t len, uint16_t rate,
            char *heard, size_t heard_n, char *reply, size_t reply_n);

/* The same, to a NAMED gateway — for falling back to a second one. */
int ask_run_to(uint16_t gw, const uint8_t *wav, size_t len, uint16_t rate,
               char *heard, size_t heard_n, char *reply, size_t reply_n);

/* True once the last ask's recording had fully reached its gateway. After that
 * point HA may already have ACTED on the question ("toggle the lights"), so a
 * failure is not safe to resend to another gateway — it could run twice. */
bool ask_last_uploaded(void);

/* Fed from the ESP-NOW gateway-response callback. Runs on the RX task, so it
 * only ever records state — never blocks, never sends. */
void ask_on_response(uint16_t from_id, const uint8_t *p, int len);

/* True while an ask is in flight, so telemetry and anything else that fancies
 * taking the radio down can stand off. */
bool ask_busy(void);
