/*
 * The talk loop.
 *
 * mic -> /api/stt/... -> /api/conversation/process -> /api/tts_get_url -> speaker
 *
 * Runs on its own task rather than in the LVGL loop: the network half takes
 * 8-15 seconds and the interface has to stay alive and repainting the whole
 * time, or the device looks crashed at exactly the moment a child is waiting
 * for an answer.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "hw.h"
#include "audio.h"
#include "player.h"
#include "applog.h"
#include "mic.h"
#include "net.h"
#include "ha.h"
#include "voice.h"
#include "espnow.h"
#include "ask.h"
#include "settings.h"

#define MINIMP3_ONLY_MP3
#include "minimp3.h"

static const char *TAG = "voice";

/* Held presses shorter than this are stray taps, not speech. */
#define MIN_HOLD_MS 400

static volatile bool s_held;
static volatile bool s_send;
static volatile bool s_want_start;   /* set by the LVGL thread, acted on by the task */
static volatile bool s_want_stop;
static voice_stage_t s_stage;
static char s_heard[256];
static char s_reply[512];

/* 🔤 THE FONT IS ASCII (plus a few symbols), AND HA ANSWERS IN TYPOGRAPHY.
 * "loud and clear — the living room LED is on" drew the dash as an empty box
 * (filmed 2026-10-09). Fold the usual suspects to ASCII in place, and drop any
 * other non-ASCII character (an emoji) rather than draw a box for it. Every
 * replacement is no longer than what it replaces, so in place is safe. */
static void ascii_fold(char *s)
{
    static const struct { const char *u, *a; } MAP[] = {
        { "\xE2\x80\x94", "-" },  { "\xE2\x80\x93", "-" },   /* em, en dash */
        { "\xE2\x80\x98", "'" },  { "\xE2\x80\x99", "'" },   /* curly single */
        { "\xE2\x80\x9C", "\"" }, { "\xE2\x80\x9D", "\"" }, /* curly double */
        { "\xE2\x80\xA6", "..." },{ "\xC2\xA0", " " },         /* ellipsis, nbsp */
        { "\xC2\xB0", "\xC2\xB0" },                              /* degree: in the font */
    };
    char *w = s;
    for (const char *r = s; *r; ) {
        if ((unsigned char)*r < 0x80) { *w++ = *r++; continue; }
        bool done = false;
        for (size_t i = 0; i < sizeof MAP / sizeof MAP[0] && !done; i++) {
            size_t n = strlen(MAP[i].u);
            if (!strncmp(r, MAP[i].u, n)) {
                for (const char *a = MAP[i].a; *a; ) *w++ = *a++;
                r += n; done = true;
            }
        }
        if (done) continue;
        r++;                                        /* skip the lead byte... */
        while (((unsigned char)*r & 0xC0) == 0x80) r++;   /* ...and its tail */
    }
    *w = 0;
}
static char s_note[96];   /* actionable hint shown on the talk screen */
static bool s_was_playing;

/* Waiting in line for a busy gateway: ask again every WAIT_RETRY_MS, give up
 * after WAIT_IN_LINE_MS. One answer takes 5-15s, so 45s covers a couple of
 * siblings ahead of you. */
#define WAIT_RETRY_MS     2000
#define WAIT_IN_LINE_MS   45000
static volatile bool s_cancel_wait;   /* set from the LVGL thread (Back) */
static int64_t s_press_ms;

voice_stage_t voice_stage(void) { return s_stage; }
const char   *voice_heard(void) { return s_heard; }
const char   *voice_reply(void) { return s_reply; }
const char   *voice_note(void)  { return s_note; }
void          voice_cancel_wait(void) { s_cancel_wait = true; }

/* ------------------------------------------------------------------ *
 * speaking the reply
 * ------------------------------------------------------------------ */

/* Decode an MP3 sitting in PSRAM straight to the active sink.
 *
 * 🚨 This is MP3 and not WAV even though the engine is Piper — HA's tts_proxy
 * serves .mp3 regardless. Confirmed live against the builder's HA before this was
 * written. */
static void speak_mp3(const uint8_t *buf, size_t len)
{
    mp3dec_t *dec = malloc(sizeof(mp3dec_t));
    int16_t  *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!dec || !pcm) { free(dec); free(pcm); applog("[tts] no memory"); return; }
    mp3dec_init(dec);

    /* 🔑 THE ASSISTANT ALWAYS ANSWERS ON THE INTERNAL SPEAKER. The builder's call,
     * 2026-08-14, and it is a product decision rather than a technical one:
     *
     *   "the ai replies play through the speaker not the pcm — the pcm is for
     *    headphones and loud speaker with friends"
     *
     * So the two outputs have different JOBS. The line-out is a deliberate,
     * social choice the user makes: plug in headphones, or hook up a bigger
     * speaker to share music. The assistant is the device itself talking back,
     * and it should always be able to. Not a fallback, not a default — the
     * intended behaviour. Do not "fix" this by routing it through
     * player_state()->sink.
     *
     * (It also happens to avoid a real failure mode: there is no jack-detect
     * wired on this board, so firmware cannot tell headphones-in from
     * nothing-plugged-in, and a kid left on JACK would ask a question and get
     * silence with no way to work out why.) */
    const player_sink_t sink = SINK_SPEAKER;
    /* Which way the reply is being sent, and how much of it there is. Silence
     * with a healthy-looking transcript is almost always this: the sink chip is
     * on JACK, so the reply goes to the PCM5102A line-out while the internal
     * amp stays muted. The earcons ignore the setting and always use the
     * speaker, which is what makes "I heard beeps but no answer" diagnostic. */
    /* Replies go out at FULL volume regardless of where the music slider sits.
     * A spoken answer has to be heard across a room by a child holding the
     * device at arm's length; music volume is a comfort setting for a track you
     * are already listening to. Restored below. */
    int prev_vol = audio_push_volume(100);
    applog("[tts] %uKB -> %s @vol100 (music was %d)", (unsigned)(len / 1024),
           sink == SINK_SPEAKER ? "SPEAKER" : "JACK", prev_vol);
    if (sink == SINK_SPEAKER) audio_amp(true);

    size_t off = 0;
    int frames = 0, rate = 0;
    while (off < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, buf + off, len - off, pcm, &info);
        if (info.frame_bytes == 0) break;
        off += info.frame_bytes;
        if (!samples) continue;

        /* The reply's sample rate comes from its first frame, not from the
         * music rate the codec happens to be at. Playing a 22.05kHz voice at
         * 44.1k is an instantly obvious chipmunk. */
        if (info.hz && info.hz != rate) {
            rate = info.hz;
            audio_set_rate(rate);
            if (sink == SINK_SPEAKER) audio_amp(true);   /* survives the reopen */
        }
        if (info.channels == 1)
            for (int i = samples - 1; i >= 0; i--) {
                pcm[i * 2] = pcm[i];
                pcm[i * 2 + 1] = pcm[i];
            }
        audio_write(sink, pcm, samples * 2 * sizeof(int16_t));
        frames++;
    }

    /* ⚠️ i2s_channel_write() returns when the data is QUEUED to DMA, not when
     * it has been heard. Cutting the amp here truncates the tail of the reply —
     * on unit #1 a 150ms cue never made it out at all. */
    vTaskDelay(pdMS_TO_TICKS(300));
    audio_amp(false);
    audio_set_volume(prev_vol);          /* give the user their setting back */
    free(dec); free(pcm);
    applog("[tts] spoke %d frames @%dHz on %s", frames, rate,
           sink == SINK_SPEAKER ? "SPEAKER" : "JACK");
    if (frames == 0) applog("[tts] DECODED NOTHING - not an MP3?");
}

/* ------------------------------------------------------------------ *
 * the network half
 * ------------------------------------------------------------------ */

static void do_talk(void)
{
    /* ⚠️ Declared before the first `goto done`: a jump that skips an
     * initialiser leaves the flag indeterminate, and this one guards a
     * refcount. */
    bool held = false;

    s_stage = VOICE_THINKING;
    s_heard[0] = 0;
    s_reply[0] = 0;


    /* 🚨 HOLD THE RADIO FOR THE WHOLE EXCHANGE, OR SOMETHING ELSE TAKES IT.
     *
     * Seen on Parent 2026-09-08, and it fails on one attempt and works on
     * the next: STT returned 200 with a clean transcript, then
     * `[wifi] disconnected, reason 8` landed BETWEEN step 1 and step 2 and the
     * agent call died as ESP_ERR_HTTP_FETCH_HEADER. Reason 8 is ASSOC_LEAVE —
     * the handset took its OWN radio down mid-conversation. telemetry.c wakes
     * on its own timer, polls, and calls net_down() on the way out; it stands
     * down for the walkie, for games and for another handset's voice frames,
     * but nothing there knows about the assistant.
     *
     * 🔑 This is the 2026-08-26 walkie cut-out again in a different place —
     * "the cut-out was TELEMETRY associating mid-conversation, not the antenna".
     * Fixing it with a hold rather than another check in telemetry.c because
     * net_down() ALREADY returns early while a hold is taken, and its own
     * comment says this is the case it exists for. A hold covers every caller,
     * including the next one somebody adds. */
    net_hold();
    held = true;

    /* --- 1. speech to text --- */

    /* 🔑 PHASE A OF THE VOICE RELAY. With the setting on and a gateway in
     * range, the recording goes over ESP-NOW and the GATEWAY talks to Home
     * Assistant — the whole point being that this handset then needs no HA
     * token and no wifi password. Off by default; see VOICE-RELAY-PLAN.md.
     *
     * ⚠️ NO SILENT FALLBACK TO THE DIRECT PATH. A relay that quietly works by
     * doing the thing it was supposed to replace would test as a pass and ship
     * as a failure. If the gateway path is selected and fails, it says so. */
    bool used_gw = false;
    if (settings_ask_via_gw() && espnow_gateway_id()) {
        /* 🔑 WAIT IN LINE RATHER THAN FAIL. The gateway takes one question at
         * a time and cannot hold a second one: it has no PSRAM and streams each
         * recording straight into HA through a 6KB window. This handset already
         * holds its recording, and a BUSY reply comes back to the BEGIN frame
         * before any audio is sent, so asking again every couple of seconds
         * costs almost nothing. Not first-come-first-served — whoever asks first
         * after the gateway frees up goes next — which is fine for a handful
         * of kids. Back cancels the wait. */
        int r;
        int64_t wait_from = esp_timer_get_time();
        s_cancel_wait = false;
        while (1) {
            r = ask_run(mic_wav(), mic_wav_len(), MIC_RATE,
                        s_heard, sizeof(s_heard), s_reply, sizeof(s_reply));
            if (r != ASK_ERR_BUSY || s_cancel_wait ||
                esp_timer_get_time() - wait_from > WAIT_IN_LINE_MS * 1000LL)
                break;
            if (!s_note[0]) applog("[talk] gateway busy - waiting in line");
            strlcpy(s_note, "Waiting for helper...", sizeof(s_note));
            for (int i = 0; i < WAIT_RETRY_MS / 100 && !s_cancel_wait; i++)
                vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_note[0] = 0;
        used_gw = true;
        if (s_cancel_wait) {
            applog("[talk] stopped waiting for the gateway");
            s_stage = VOICE_IDLE;
            goto done;
        }
        if (r != ASK_OK || !s_heard[0]) {
            applog("[talk] gateway ask failed: err %d", r);
            /* 🔑 WORDS FOR A KID, NOT FOR THE BUILDER. The exact cause is in the
             * log line above; the screen only needs to say what to do next.
             * "Unavailable" was too hard a word, so a gateway that is down, out
             * of reach of HA, or still busy after the wait all read as busy. */
            strlcpy(s_note, r == ASK_ERR_HA    ? "Helper didn't understand" :
                            r == ASK_ERR_EMPTY ? "Didn't hear you - try again" :
                                                 "Helper is busy - try later",
                    sizeof(s_note));
            s_stage = VOICE_ERROR;
            goto done;
        }
    }

    /* 🔑 THE GATEWAY IS THE ONLY ROUTE IN THIS BUILD, AND THAT IS THE POINT.
     * There is no token and no wifi password here to fall back to, so say which
     * thing is missing rather than failing as "the AI is broken". */
    if (!used_gw) {
        strlcpy(s_note, espnow_gateway_id() ? "Turn on 'Ask via GATEWAY'"
                                            : "No helper nearby", sizeof(s_note));
        s_stage = VOICE_ERROR;
        goto done;
    }
    ascii_fold(s_heard);
    applog("[talk] heard: %s", s_heard);
    audio_earcon(CUE_OK);          /* it understood you; the LLM takes a moment */

    /* The capture buffer is up to 384KB of PSRAM and its job is finished. */
    mic_free();

    /* --- 2. ask the agent ---
     * 🔑 PHASE B: when the gateway was used it has ALREADY made this call with
     * its own token, and s_reply is filled. That is the whole reason the
     * gateway exists — a handset that carries no credential still gets an
     * ANSWER, not merely a transcript of itself. */
    if (!s_reply[0]) {
        applog("[talk] no answer came back");
        strlcpy(s_note, "Heard you - no answer came back", sizeof(s_note));
        s_stage = VOICE_ERROR;
        goto done;
    }
    ascii_fold(s_reply);
    applog("[talk] reply: %s", s_reply);

    /* --- 3. the answer is shown, not spoken ---
     * The gateway relays the answer as TEXT only, so the handset answers ON
     * SCREEN. There is deliberately no direct call to Home Assistant here:
     * the handset holds no credentials. */
    uint8_t *audio = NULL;
    size_t   audio_len = 0;
    if (held) { net_release(); held = false; }

    if (audio && audio_len) {
        s_stage = VOICE_SPEAKING;
        speak_mp3(audio, audio_len);
        heap_caps_free(audio);
        s_stage = VOICE_IDLE;
    } else {
        /* Text-only by design: the gateway relays the answer, not its audio.
         * An answer on screen is a success, not an error. */
        s_stage = VOICE_IDLE;
    }

done:
    /* Every `goto done` above lands here, including the ones before the hold
     * was taken — hence the flag rather than a bare release. */
    if (held) { net_release(); held = false; }
    if (net_is_up()) net_down();
    if (s_stage == VOICE_ERROR) audio_earcon(CUE_FAIL);
    mic_free();
    /* Back to music rate whatever happened, or the next track plays at 16kHz. */
    audio_set_rate(44100);
    player_unsuspend(s_was_playing);
    s_was_playing = false;
}

/* ------------------------------------------------------------------ */

static void stop_listening_and_reset(const char *why, int held_ms)
{
    applog("[talk] %s (%dms)", why, held_ms);
    audio_earcon(CUE_NOHEAR);
    mic_free();
    audio_set_rate(44100);
    player_unsuspend(s_was_playing);
    s_was_playing = false;
    s_stage = VOICE_IDLE;
}

static void voice_task(void *arg)
{
    while (1) {
        /* --- start, on THIS task, never on the LVGL thread --- */
        if (s_want_start) {
            s_want_start = false;
            s_was_playing = player_suspend();
            /* 🚨 EARCON BEFORE mic_begin(), NEVER AFTER.
             *
             * mic_begin() flushes the RX DMA, and the DMA keeps filling
             * regardless of who is listening. Playing the cue after the flush
             * puts the cue INSIDE the recording — unit #1 hit exactly this and
             * the giveaway was a suspiciously identical peak on every single
             * attempt. Beep first, then flush, then record. */
            audio_earcon(CUE_START);
            if (!mic_begin()) {
                s_stage = VOICE_ERROR;
                player_unsuspend(s_was_playing);
                s_was_playing = false;
            } else {
                s_note[0] = 0;
                s_stage = VOICE_LISTENING;
                applog("[talk] listening");
            }
            continue;
        }

        if (s_stage == VOICE_LISTENING) {
            mic_pump();
            /* Once a second, prove the capture is actually accumulating. A
             * silent failure here is invisible until the very end otherwise. */
            static int64_t last_beat;
            int64_t now = esp_timer_get_time();
            if (now - last_beat > 1000000) {
                last_beat = now;
                static size_t last_len;
                size_t len = mic_bytes();
                int pk = mic_peak() * 100 / 32767;
                if (len == last_len)
                    applog("[mic] STALLED at %ds (no new audio)", mic_seconds());
                else
                    applog("[mic] %ds peak %d%%%s", mic_seconds(), pk,
                           pk >= 99 ? " CLIPPING" : "");
                last_len = len;
            }

            /* Hard cap. Without it a held button (or a release event that never
             * arrives) records until the buffer is full and then spins. */
            /* 🚨 CAP ON WALL-CLOCK TIME, NOT ON BUFFER FILL.
             *
             * Ending the recording only on release-or-buffer-full has two ways
             * to hang forever, and BOTH were seen on hardware: the touch
             * controller can hold a press so LVGL never fires RELEASED, and the
             * capture itself can stall so the buffer never fills. Observed as
             * "[mic] 3s peak 100%" repeating for 40+ seconds with no way out.
             * Real elapsed time is the one condition that cannot get stuck. */
            int listened = (int)(esp_timer_get_time() / 1000 - s_press_ms);
            if (listened > MIC_MAX_SECS * 1000) {
                applog("[talk] hit the %ds limit", MIC_MAX_SECS);
                s_held = false; s_want_stop = false;
                if (mic_end() > 0) { s_send = true; s_stage = VOICE_THINKING; }
                else stop_listening_and_reset("NO AUDIO CAPTURED", listened);
                continue;
            }

            if (mic_full()) {
                s_held = false;
                s_want_stop = false;
                if (mic_end() > 0) { s_send = true; }
                else stop_listening_and_reset("nothing recorded", 0);
                if (s_send) s_stage = VOICE_THINKING;
                continue;
            }

            if (s_want_stop) {
                s_want_stop = false;
                int held = (int)(esp_timer_get_time() / 1000 - s_press_ms);
                size_t n = mic_end();
                /* A stray tap is not an utterance. Without this the device
                 * fires a whole network round trip at HA every time a pocket
                 * brushes the screen. */
                /* 🚨 ADVANCE THE STAGE. Without this the LISTENING branch
                 * `continue`s forever: s_send is set but the `if (s_send)`
                 * check below is never reached, mic_pump() returns early
                 * because capture is closed, and the log fills with
                 * "[mic] STALLED" while nothing is ever sent. */
                if (held < MIN_HOLD_MS || n == 0) {
                    /* Two very different faults, so say which. A long hold that
                     * captures nothing is a MIC problem; a short hold is a
                     * user-gesture problem. Reporting both as "too short" sent
                     * the first debug session in the wrong direction. */
                    const char *why = (held < MIN_HOLD_MS) ? "too short"
                                                           : "NO AUDIO CAPTURED";
                    /* Say WHY. A silent return to idle teaches a child nothing
                     * except that the button is broken. */
                    strlcpy(s_note, (held < MIN_HOLD_MS)
                            ? "Keep holding while you talk"
                            : "Mic heard nothing - check the log", sizeof(s_note));
                    stop_listening_and_reset(why, held);
                    s_stage = VOICE_ERROR;
                } else {
                    s_send  = true;
                    s_stage = VOICE_THINKING;
                }
                continue;
            }

            /* 🚨 vTaskDelay(1), NOT pdMS_TO_TICKS(5).
             *
             * CONFIG_FREERTOS_HZ is 100, so a tick is 10ms and pdMS_TO_TICKS(5)
             * rounds to ZERO — and vTaskDelay(0) does not yield at all. This
             * task then spun flat out at priority 4 and starved the LVGL loop
             * it shares a core with, so the RELEASE event could never be
             * delivered and the recording never ended. The device locked up
             * hard and the task watchdog named `voice` on CPU 0 every 5s.
             * Any sub-tick delay is a busy-wait. */
            vTaskDelay(1);
            continue;
        }

        if (s_send) {
            s_send = false;
            do_talk();
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));      /* 2 ticks — this one is real */
    }
}

/* Both of these run in LVGL event callbacks, so they do NOTHING but set a
 * flag. Everything they used to do — stopping the player, reallocating the
 * capture buffer, reconfiguring the codec's sample rate — blocks, and blocking
 * here freezes the interface. */
void voice_press(void)
{
    if (s_stage != VOICE_IDLE && s_stage != VOICE_ERROR) return;
    s_press_ms   = esp_timer_get_time() / 1000;
    s_held       = true;
    s_want_stop  = false;
    s_want_start = true;
}

void voice_release(void)
{
    if (!s_held) return;
    s_held      = false;
    s_want_stop = true;
}

void voice_init(void)
{
    /* 32KB: minimp3's synthesis runs on the caller's stack for the TTS reply
     * exactly as it does for music, and starving it corrupts the stack rather
     * than failing cleanly. Same trap, second task.
     *
     * 🚨 CORE 1, NOT CORE 0. app_main runs the LVGL loop on core 0, and this
     * task is a higher priority than it. Sharing a core means any misbehaviour
     * here takes the whole interface with it — which is exactly what happened.
     * Core 1 already hosts the player task, and neither is latency-critical
     * against the other. */
    xTaskCreatePinnedToCore(voice_task, "voice", 32768, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "voice ready - GATEWAY ONLY, no HA credentials in this build");
}
