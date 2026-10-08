/*
 * Handset side of GW_OP_ASK — see ask.h for why this exists at all.
 *
 * The shape: BEGIN, then DATA frames kept inside a window the gateway advances
 * with cumulative acks, then the gateway's TEXT and DONE come back.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "applog.h"
#include "espnow.h"
#include "gw_wire.h"
#include "ask.h"

/* How long to wait with no ack movement before assuming frames were lost and
 * winding back to what the gateway last confirmed. Two beacon-ish periods: long
 * enough that a busy gateway is not mistaken for a lost frame, short enough
 * that a real loss does not stall the upload for a noticeable time. */
#define ASK_STALL_MS    1500
/* The whole upload. 1,980 chunks at the measured 58 KB/s is ~6.6s, so this is
 * roughly 4x the worst case and only trips when something is actually wrong. */
#define ASK_UPLOAD_MS   30000
/* After the last byte is in, HA still has to transcribe. Generous on purpose:
 * a slow answer is not a failure and retrying would send the whole recording
 * again. */
#define ASK_REPLY_MS    30000
/* Enough retries that a single dropped BEGIN does not cost the whole attempt,
 * few enough that a gateway which has genuinely gone is reported quickly. */
#define ASK_BEGIN_TRIES 8

static volatile bool     s_busy;
static volatile uint8_t  s_seq;          /* rises per ask; stale replies dropped */
static volatile uint16_t s_acked;        /* the gateway wants this index next */
static volatile bool     s_done;
static volatile uint8_t  s_err;
static volatile uint16_t s_status;
static char              s_heard[256];
static volatile bool     s_heard_last;
/* PHASE B. Kept separate from s_heard rather than reusing it: a handset that
 * gets a transcript and then loses the agent must still be able to show what it
 * heard, and one buffer would have overwritten that with nothing. */
/* 512 to match voice.c. At 256 an ordinary LLM answer was being clipped
 * mid-sentence, and a clipped answer reads as a bad answer. */
static char              s_reply[512];
static volatile bool     s_reply_last;

bool ask_busy(void) { return s_busy; }

/* ------------------------------------------------------------------ *
 * the receive half — runs on the ESP-NOW RX task
 *
 * ⚠️ RECORDS STATE ONLY. Sending from here would be sending from the WiFi
 * task's callback context, which the ESP-NOW docs warn against and which this
 * project has already been bitten by once.
 * ------------------------------------------------------------------ */
void ask_on_response(uint16_t from_id, const uint8_t *p, int len)
{
    (void)from_id;
    if (len < 4 || p[0] != GW_VER || p[1] != GW_OP_ASK) return;

    /* 🔑 A REPLY FROM A PREVIOUS ASK IS NOT AN ANSWER TO THIS ONE. Without the
     * seq check a late ack from an abandoned attempt would advance this
     * attempt's window past frames the gateway never received. */
    if (p[2] != s_seq) return;

    switch (p[3]) {
    case ASK_ACK: {
        if (len < (int)sizeof(gw_ask_ack_t)) return;
        gw_ask_ack_t a;
        memcpy(&a, p, sizeof(a));
        /* Never let the window go BACKWARDS: acks can be reordered, and a stale
         * one arriving late would re-send frames that are already committed. */
        if (a.next > s_acked) s_acked = a.next;
        break;
    }
    case ASK_TEXT:
    case ASK_REPLY: {
        if (len < 6) return;
        gw_ask_text_t t;
        int n = len > (int)sizeof(t) ? (int)sizeof(t) : len;
        memset(&t, 0, sizeof(t));
        memcpy(&t, p, n);
        /* Same wire shape, two destinations — the sub says which. */
        bool  is_reply = (t.sub == ASK_REPLY);
        char *dst      = is_reply ? s_reply : s_heard;
        int   cap      = (int)(is_reply ? sizeof(s_reply) : sizeof(s_heard));
        int room = cap - 1;
        int have = (int)strlen(dst);
        int add  = n - 6;
        if (add > 0 && have < room) {
            if (add > room - have) add = room - have;
            memcpy(dst + have, t.text, add);
            dst[have + add] = 0;
        }
        if (t.last) { if (is_reply) s_reply_last = true; else s_heard_last = true; }
        break;
    }
    case ASK_DONE: {
        if (len < (int)sizeof(gw_ask_done_t)) return;
        gw_ask_done_t d;
        memcpy(&d, p, sizeof(d));
        s_err    = d.err;
        s_status = d.status;
        s_done   = true;
        break;
    }
    default: break;
    }
}

/* ------------------------------------------------------------------ *
 * the send half — runs on the voice task and blocks
 * ------------------------------------------------------------------ */
int ask_run(const uint8_t *wav, size_t len, uint16_t rate,
            char *heard, size_t heard_n, char *reply, size_t reply_n)
{
    if (heard && heard_n) heard[0] = 0;
    if (!wav || !len) return ASK_ERR_GAP;

    uint16_t gw = espnow_gateway_id();
    if (!gw) {
        applog("[ask] no gateway in range");
        return ASK_ERR_NET;
    }

    const uint16_t chunks = (uint16_t)((len + GW_ASK_DATA_MAX - 1) / GW_ASK_DATA_MAX);

    s_busy       = true;
    s_seq        = (uint8_t)(s_seq + 1);
    if (!s_seq) s_seq = 1;              /* 0 is never a live ask */
    s_acked      = 0;
    s_done       = false;
    s_err        = ASK_OK;
    s_status     = 0;
    s_heard[0]   = 0;
    s_heard_last = false;
    s_reply[0]   = 0;
    s_reply_last = false;

    const uint8_t seq = s_seq;

    /* ---- BEGIN, retried: one lost frame must not cost the recording ---- */
    gw_ask_begin_t b = {
        .ver = GW_VER, .op = GW_OP_ASK, .seq = seq, .sub = ASK_BEGIN,
        .bytes = (uint32_t)len, .rate = rate, .chunks = chunks,
    };
    bool sent = false;
    for (int i = 0; i < ASK_BEGIN_TRIES && !sent; i++) {
        sent = espnow_gw_request(gw, &b, sizeof(b));
        if (!sent) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!sent) {
        applog("[ask] gateway %04X would not take the request", gw);
        s_busy = false;
        return ASK_ERR_NET;
    }
    applog("[ask] %04X: %u bytes in %u chunks", gw, (unsigned)len, chunks);

    /* ---- DATA, held inside the window the gateway advances ---- */
    const int64_t t0 = esp_timer_get_time();
    int64_t   last_move = t0;
    uint16_t  next_to_send = 0;
    uint16_t  last_acked = 0;
    int       rewinds = 0;

    while (s_acked < chunks) {
        if (s_done) break;              /* the gateway gave up or failed early */

        int64_t now = esp_timer_get_time();
        if (now - t0 > (int64_t)ASK_UPLOAD_MS * 1000) {
            applog("[ask] upload timed out at %u/%u", s_acked, chunks);
            s_busy = false;
            return ASK_ERR_GAP;
        }

        if (s_acked != last_acked) { last_acked = s_acked; last_move = now; }

        /* 🚨 WIND BACK TO WHAT THE GATEWAY CONFIRMED, NOT TO WHERE WE STOPPED.
         * The gateway commits a contiguous prefix to an HTTP body it cannot
         * rewind, so the only safe place to resume is the first index it has
         * not acked. Resuming from `next_to_send` would leave a hole in the
         * WAV that nothing downstream could detect. */
        if (now - last_move > (int64_t)ASK_STALL_MS * 1000) {
            next_to_send = s_acked;
            last_move    = now;
            if (++rewinds <= 3 || rewinds % 10 == 0)
                applog("[ask] stalled at %u/%u, resending (rewind %d)",
                       s_acked, chunks, rewinds);
        }

        /* Keep at most GW_ASK_WIN frames ahead of the gateway. */
        uint16_t limit = (uint16_t)(s_acked + GW_ASK_WIN);
        if (limit > chunks) limit = chunks;

        if (next_to_send >= limit) { vTaskDelay(pdMS_TO_TICKS(2)); continue; }

        gw_ask_data_t d = { .ver = GW_VER, .op = GW_OP_ASK, .seq = seq,
                            .sub = ASK_DATA, .index = next_to_send };
        size_t off = (size_t)next_to_send * GW_ASK_DATA_MAX;
        size_t n   = len - off;
        if (n > GW_ASK_DATA_MAX) n = GW_ASK_DATA_MAX;
        memcpy(d.data, wav + off, n);

        /* Same backpressure the SPEED test proved at 58-64 KB/s: the driver
         * refusing is the queue being full, which is a reason to wait rather
         * than a reason to fail. */
        if (espnow_gw_request(gw, &d, (int)(6 + n))) next_to_send++;
        else vTaskDelay(1);
    }

    /* ---- the answer ---- */
    if (!s_done) applog("[ask] uploaded, waiting for HA");
    const int64_t tw = esp_timer_get_time();
    while (!s_done) {
        if (esp_timer_get_time() - tw > (int64_t)ASK_REPLY_MS * 1000) {
            applog("[ask] no answer from %04X after %ums", gw, ASK_REPLY_MS);
            s_busy = false;
            return ASK_ERR_GAP;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (heard && heard_n) {
        strlcpy(heard, s_heard, heard_n);
        /* Say so rather than silently handing back half a sentence: a truncated
         * transcript looks like a mis-hearing and would be blamed on the mic. */
        if (!s_heard_last && s_heard[0])
            applog("[ask] transcript may be incomplete");
    }
    if (reply && reply_n) {
        strlcpy(reply, s_reply, reply_n);
        if (!s_reply_last && s_reply[0])
            applog("[ask] answer may be incomplete");
    }
    {
        int ms = (int)((esp_timer_get_time() - t0) / 1000);
        applog("[ask] done err %u status %u in %dms (%d B/s)",
               s_err, s_status, ms, ms > 0 ? (int)((int64_t)len * 1000 / ms) : 0);
    }
    s_busy = false;
    return s_err;
}
