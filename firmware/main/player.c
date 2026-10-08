/*
 * Audio: I2S to both sinks, ES8311 for the speaker, minimp3 for decoding, and
 * a task that owns playback so the UI never blocks on it.
 *
 * Lifted from an SD-card test sketch, which proved real-time decode on this
 * board (957 frames in 25s) and 168KB of heap left over. What is new here is
 * the task, the transport controls and the sink switch.
 *
 * ⚠️ MCLK is 256x. The ES8311's coefficient table has NO entry for 384x at
 * 44.1kHz and the symptom is quiet static, not an error.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "hw.h"
#include "display.h"
#include "audio.h"
#include "library.h"
#include "faves.h"
#include "settings.h"
#include "applog.h"
#include "player.h"
#include "espnow.h"

#define MINIMP3_ONLY_MP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

static const char *TAG = "play";

static TaskHandle_t s_task;
static player_state_t s_state = { .sink = SINK_SPEAKER, .volume = 70 };

/* Commands are a single word so the UI can post without a queue or a lock. */
typedef enum { CMD_NONE = 0, CMD_PLAY, CMD_STOP, CMD_NEXT, CMD_PREV, CMD_PAUSE } cmd_t;
static volatile cmd_t s_cmd;
static volatile int   s_want_album = -1;
static volatile int   s_want_track = -1;

#define amp(on) audio_amp(on)

const player_state_t *player_state(void) { return &s_state; }

void player_play(int album_idx, int track_n)
{
    s_want_album = album_idx;
    s_want_track = track_n;
    s_cmd = CMD_PLAY;
}
void player_toggle(void)
{
    if (s_state.playing) s_cmd = CMD_PAUSE;
    else if (s_state.album >= 0) player_play(s_state.album, s_state.track);
}
void player_next(void) { s_cmd = CMD_NEXT; }
void player_prev(void) { s_cmd = CMD_PREV; }

void player_set_sink(player_sink_t sink)
{
    s_state.sink = sink;
    /* The amp is only ever on while the speaker is the active sink AND audio is
     * playing — a live amp with no signal is just hiss next to a mic. */
    if (sink != SINK_SPEAKER) amp(false);
    else if (s_state.playing) amp(true);
    settings_set_sink(sink);
    ESP_LOGI(TAG, "sink -> %s", sink == SINK_SPEAKER ? "speaker" : "line-out");
}

/* 🔑 MUTE IS NOT A VOLUME OF ZERO, AND MUST NOT BE SAVED AS ONE.
 *
 * The builder, 2026-08-25: "the speaker icon maybe be a mute button". The obvious
 * implementation - set the volume to 0 - writes 0 to NVS, so a handset muted at
 * bedtime comes back silent, and a silent handset with a slider that is already
 * at zero looks broken rather than muted. The user's setting stays exactly
 * where it was in s_state and in NVS; only what the hardware is doing changes.
 *
 * The Bluetooth dongle is told too: the stream it plays is untouched MP3, so
 * muting here without telling it would leave the headphones playing on. */
static bool s_muted;

/* 🔑 WHERE EVERYTHING THAT IS NOT MUSIC GOES.
 *
 * The builder, 2026-08-25: "can we make it so audio in radio mode and in talkie mode go
 * through the jack headphones". Both hardcoded SINK_SPEAKER, so the SINK button
 * governed the music and nothing else — with headphones plugged in, internet
 * radio still came out of the little speaker.
 *
 * There is no headphone-detect pin on this board, so "are headphones in?" is
 * not a question the firmware can answer. The user's existing choice is, and it
 * is already persisted, so radio and the walkie-talkie simply follow it.
 *
 * ⚠️ BT FOLDS BACK TO THE SPEAKER. SINK_BT is not a third output, it is "send
 * MP3 frames to the dongle" — the dongle decodes them. Radio and the talkie
 * hand over PCM that has already been decoded, so there is nothing to send and
 * the dongle would have nothing to do with it. Rather than silence, they use
 * the speaker. */
player_sink_t player_out_sink(void)
{
    return s_state.sink == SINK_LINEOUT ? SINK_LINEOUT : SINK_SPEAKER;
}

void player_set_mute(bool on)
{
    s_muted = on;
    audio_set_volume(on ? 0 : s_state.volume);
    ESP_LOGI(TAG, "%s (setting stays %d)", on ? "muted" : "unmuted",
             s_state.volume);
}

bool player_muted(void) { return s_muted; }

void player_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_muted = false;              /* touching the slider is a way out of mute */
    /* A BT session is played by the dongle, not by this board's codec, so the
     * volume has to travel with the audio. Harmless when not streaming. */
    s_state.volume = pct;
    audio_set_volume(pct);
    settings_set_volume(pct);
}

/* Stop for a voice interaction and report whether anything was playing.
 *
 * Music over the top of a child talking is the fastest way to a useless
 * transcript, so this is a quality decision rather than the memory workaround
 * unit #1 needed — that board could not hold WiFi and a decoder at once, this
 * one has ~168KB spare with both up. Position within the track is deliberately
 * not preserved: restarting the current track is easier for a kid to
 * understand than resuming mid-word. */
bool player_suspend(void)
{
    bool was = s_state.playing;
    if (was) {
        s_cmd = CMD_STOP;
        for (int i = 0; i < 100 && s_state.playing; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
    }
    amp(false);
    return was;
}

void player_unsuspend(bool was_playing)
{
    if (was_playing && s_state.album >= 0)
        player_play(s_state.album, s_state.track);
}

/* ------------------------------------------------------------------ */

/* Decodes one file, returning why it stopped. Checks s_cmd between frames so a
 * button press is acted on within a frame (~26ms), not at end of track. */

static cmd_t play_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { ESP_LOGE(TAG, "cannot open %s", path); return CMD_NEXT; }

    mp3dec_t *dec = malloc(sizeof(mp3dec_t));
    const int IN = 16 * 1024;
    uint8_t *in = malloc(IN);
    int16_t *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    if (!dec || !in || !pcm) {
        free(dec); free(in); free(pcm); fclose(f);
        ESP_LOGE(TAG, "out of memory starting playback");
        return CMD_STOP;
    }
    mp3dec_init(dec);

    s_state.playing = true;
    s_state.elapsed_ms = 0;
    if (s_state.sink == SINK_SPEAKER) amp(true);
    /* 🔑 ONE CONTINUOUS BYTE STREAM ACROSS TRACKS. The dongle does not need to
     * know where one file ends: it feeds bytes to minimp3, which finds its own
     * frames. Restarting the stream at every track boundary is what made
     * changing songs "go nuts" — AUD_START flushed the dongle mid-note while it
     * still held five seconds of the previous song. */

    int filled = 0, frames = 0;
    /* Bytes we are willing to skip looking for the next frame before accepting
     * that the file really is finished. 64KB comfortably covers embedded cover
     * art, which is the usual culprit. */
    const int MAX_RESYNC = 64 * 1024;
    int resync = 0;
    long total_bytes = 0, file_bytes = 0;
    fseek(f, 0, SEEK_END); file_bytes = ftell(f); fseek(f, 0, SEEK_SET);

    cmd_t why = CMD_NEXT;
    while (1) {
        /* ⏸️ A REAL PAUSE, WHICH MEANS NOT LEAVING THIS FUNCTION.
         *
         * Pause used to be CMD_STOP, so play_file() returned and the next press
         * called player_play() — which opens the file again from byte zero.
         * The builder, 2026-08-23: "it restarted the song on restart or unpuse". The
         * position only exists as `f` and the decoder state, so the only way to
         * keep it is to stay here and stop consuming.
         *
         * The dongle is told to drop what it holds (~5s of banked audio), and
         * given a fresh AUD_START on resume so it re-primes cleanly. */
        /* 🚨 THE DONGLE WENT AWAY — STOP PLAYING INTO NOTHING.
         *
         * The builder, 2026-08-24: "it should pause the music if the connection gets
         * lost, the music keeps playing so it won't just keep playing with
         * nothing happening." Before AUD_STAT the handset had no way to know:
         * the only frame that ever came back was a button press, so a dongle
         * that lost its headphones looked exactly like one playing happily,
         * and the child watched the progress bar move in silence.
         *
         * 🔑 PAUSE ONLY ON A LINK THAT WAS GOOD AND THEN DROPPED. Gating on
         * "not connected" alone would make starting impossible: the dongle
         * takes up to a MINUTE to find its headphones — measured 2026-08-24,
         * four failed auto-reconnects at 10s each before it falls back to
         * scanning — so a fresh session is legitimately silent for a while.
         * Waiting for the first good link sidesteps the whole startup race and
         * matches the symptom exactly.
         *
         * Raising CMD_PAUSE rather than pausing here reuses the existing
         * machinery below: it tells the dongle to drop its ~5s of banked audio
         * and re-primes with AUD_START on resume, and it keeps the position,
         * so pressing play carries on where the sound stopped. */

        if (s_cmd == CMD_PAUSE) {
            s_cmd = CMD_NONE;
            s_state.playing = false;
            amp(false);

            /* Wait for a button. Nothing else can end a pause now that the
             * dongle's auto-resume is gone. */
            while (s_cmd == CMD_NONE) vTaskDelay(pdMS_TO_TICKS(50));
            /* Resume only if it is THIS track being asked for. Choosing a
             * different one while paused must still start that one. */
            if (s_cmd == CMD_PLAY &&
                s_want_album == s_state.album && s_want_track == s_state.track) {
                s_cmd = CMD_NONE;
                s_state.playing = true;
                if (s_state.sink == SINK_SPEAKER) amp(true);
            }
            /* anything else falls through and is handled as a command below */
        }

        if (s_cmd != CMD_NONE) { why = s_cmd; s_cmd = CMD_NONE; break; }

        if (filled < IN) {
            size_t got = fread(in + filled, 1, IN - filled, f);
            filled += got;
            if (got == 0 && filled == 0) break;
        }
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, in, filled, pcm, &info);

        /* 🚨 A FRAME WE CANNOT PARSE IS NOT THE END OF THE TRACK.
         *
         * This used to `break` here, which play_file() reports as CMD_NEXT -
         * so ANY mid-file hiccup silently skipped to the next song. The builder,
         * 2026-08-16, on a Gorillaz album: "it's not letting the songs
         * complete before it plays the next song... it's doing it on its own".
         *
         * MP3s in the wild carry things minimp3 will not sync on: ID3v2 blocks,
         * APE tags, cover art, junk between frames. The right response is the
         * same one radio.c already uses for joining a live stream mid-frame -
         * drop a byte and rescan - not to declare the track over.
         *
         * Only give up when the file is exhausted AND a full buffer still has
         * no frame in it, with a bounded resync so corrupt data cannot spin
         * here forever. */
        if (info.frame_bytes == 0) {
            if (filled > 4 && resync < MAX_RESYNC) {
                memmove(in, in + 1, --filled);
                resync++;
                continue;
            }
            break;                       /* genuinely out of decodable audio */
        }
        resync = 0;                      /* a good frame clears the budget */
        /* 🔑 FORWARD THE FRAME BEFORE IT IS CONSUMED. These are whole, frame-
         * aligned MP3 bytes straight off the SD card, so the dongle's decoder
         * syncs on the very first one instead of hunting for a header. */
        memmove(in, in + info.frame_bytes, filled - info.frame_bytes);
        filled -= info.frame_bytes;
        total_bytes += info.frame_bytes;

        if (samples) {
            /* 🔍 SAY WHAT IS ACTUALLY BEING DECODED, ONCE PER TRACK.
             *
             * The builder, 2026-08-21: "on the head set jack am i only getting mono i
             * thought i could have stero". The whole firmware path IS stereo -
             * I2S1 is opened SLOT_MODE_STEREO for the PCM5102A and never
             * reconfigured, and the duplication below only runs for a MONO
             * FILE - so "mono at the jack" has exactly two remaining causes:
             * the track is mono, or only one of LOUT/ROUT is wired to it.
             * This line separates them without opening the case. */
            static int last_ch, last_hz;
            if (info.channels != last_ch || info.hz != last_hz) {
                last_ch = info.channels;
                last_hz = info.hz;
                applog("[play] decoding %d Hz %s", info.hz,
                       info.channels == 1 ? "MONO (file is mono)" : "STEREO");
            }
            if (info.channels == 1) {
                for (int i = samples - 1; i >= 0; i--) {
                    pcm[i * 2] = pcm[i];
                    pcm[i * 2 + 1] = pcm[i];
                }
            }
            audio_write(s_state.sink,
                        pcm, samples * 2 * sizeof(int16_t));
            if (info.hz) s_state.elapsed_ms += (samples * 1000) / info.hz;
            /* Heartbeat, so a silent failure is diagnosable from the log
             * instead of from the builder's ears. Cheap: once every ~5 seconds. */
            if (++frames % 200 == 0) {
                ESP_LOGI(TAG, "  [%ds] frames=%d free=%u stack_hw=%u",
                         s_state.elapsed_ms / 1000, frames,
                         (unsigned)esp_get_free_heap_size(),
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
            /* Progress from bytes consumed: exact enough for a bar, and it
             * costs nothing next to parsing a VBR header. */
            if (file_bytes > 0)
                s_state.progress = (int)((total_bytes * 100) / file_bytes);
        }
    }

    /* 🚨 A PERSON CHANGING SONGS WANTS THE OLD ONE GONE, NOT DRAINED.
     * Any command the child pressed — pause, a new selection, next, previous —
     * dumps what is buffered. A track that simply ran out says nothing at all,
     * and the next file's bytes continue the same stream. */
    amp(false);
    fclose(f);
    free(dec); free(in); free(pcm);
    s_state.playing = false;
    return why;
}

/* ---- shuffle --------------------------------------------------------------
 *
 * 🚨 PURE RANDOM FEELS BROKEN. Picking uniformly every time will replay the
 * track that just finished about once in N, and a child hearing the same song
 * twice in a row does not think "coincidence", they think it is stuck. So the
 * last few picks are remembered and rejected.
 *
 * 🔑 THE HISTORY MUST SCALE WITH THE SCOPE, or it deadlocks. Sixteen entries is
 * fine across 3,987 tracks and impossible inside a 3-track album — every
 * candidate would be in the history and there would be nothing legal to pick.
 * The window is therefore capped at n-1, which always leaves at least one.
 *
 * ⚠️ esp_random() is used, not rand(): rand() without srand() gives the SAME
 * sequence every boot, so every power-on would shuffle identically. */
#define SHUF_HIST_MAX 16
static player_shuffle_t s_shuffle;
static int  s_hist[SHUF_HIST_MAX];
static int  s_hist_n, s_hist_at;

static void shuf_reset(void) { s_hist_n = 0; s_hist_at = 0; }

static bool shuf_seen(int g)
{
    for (int i = 0; i < s_hist_n; i++) if (s_hist[i] == g) return true;
    return false;
}

static void shuf_remember(int g, int window)
{
    if (window <= 0) { shuf_reset(); return; }
    if (window > SHUF_HIST_MAX) window = SHUF_HIST_MAX;
    if (s_hist_n < window) s_hist_n++;
    s_hist[s_hist_at] = g;
    s_hist_at = (s_hist_at + 1) % window;
}

void player_set_shuffle(player_shuffle_t m)
{
    s_shuffle = m;
    shuf_reset();
    applog("[play] shuffle %s", player_shuffle_name(m));
}
player_shuffle_t player_shuffle(void) { return s_shuffle; }

const char *player_shuffle_name(player_shuffle_t m)
{
    switch (m) {
        case SHUFFLE_ALBUM:  return "Album";
        case SHUFFLE_ARTIST: return "Artist";
        case SHUFFLE_CARD:   return "Card";
        case SHUFFLE_FAVES:  return "Faves";
        default:             return "Off";
    }
}

/* Choose the next (album, track) for the current mode. Returns false when
 * shuffle is off or the scope is too small to be worth shuffling, in which case
 * the caller keeps its normal sequential behaviour. */
static bool shuf_next(int cur_album, int *album_out, int *track_out)
{
    if (s_shuffle == SHUFFLE_OFF) return false;

    int total = 0, artist = -1;
    switch (s_shuffle) {
        case SHUFFLE_CARD:
            total = library_total_tracks();
            break;
        case SHUFFLE_FAVES:
            /* The RESOLVED count, not the stored one: a favourite whose file is
             * not on this card cannot be played, and including it in the scope
             * would make the draw fail and fall back to sequential. */
            total = faves_count();
            break;
        case SHUFFLE_ARTIST: {
            const lib_album_t *al = library_album_by_index(cur_album);
            if (!al) return false;
            artist = al->artist;
            total  = library_artist_track_total(artist);
            break;
        }
        case SHUFFLE_ALBUM:
            total = library_track_count(cur_album);
            break;
        default: return false;
    }
    if (total <= 1) return false;          /* nothing to shuffle */

    int window = (total - 1 < SHUF_HIST_MAX) ? total - 1 : SHUF_HIST_MAX;

    /* Bounded retries, then accept a repeat rather than spin. With the window
     * capped at total-1 a free slot always exists, but the draw is random and
     * this must not become an unbounded loop in the audio task. */
    for (int attempt = 0; attempt < 40; attempt++) {
        int g = (int)(esp_random() % (uint32_t)total);
        if (shuf_seen(g) && attempt < 39) continue;
        bool ok = (s_shuffle == SHUFFLE_ARTIST)
                    ? library_pick_in_artist(artist, g, album_out, track_out)
                    : (s_shuffle == SHUFFLE_CARD)
                        ? library_pick_global(g, album_out, track_out)
                        : (s_shuffle == SHUFFLE_FAVES)
                            ? faves_pick(g, album_out, track_out)
                            : (*album_out = cur_album, *track_out = g, true);
        if (!ok) continue;
        shuf_remember(g, window);
        return true;
    }
    return false;
}

static void player_task(void *arg)
{
    char path[600];
    while (1) {
        if (s_cmd != CMD_PLAY) { vTaskDelay(pdMS_TO_TICKS(30)); continue; }
        s_cmd = CMD_NONE;

        int album = s_want_album;
        int track = s_want_track;

        while (album >= 0) {
            int n = library_track_count(album);
            if (n <= 0) break;
            if (track < 0) track = 0;
            if (track >= n) {
                /* End of album. Sequentially that is the end of playback, and
                 * deliberately so. But a CARD or ARTIST shuffle running out of
                 * album is not the end of anything — it should roll on, which
                 * is the whole point of choosing that scope. */
                int na, nt;
                if (s_shuffle == SHUFFLE_CARD || s_shuffle == SHUFFLE_ARTIST) {
                    if (shuf_next(album, &na, &nt)) { album = na; track = nt; continue; }
                }
                break;                             /* end of album: stop */
            }

            s_state.album = album;
            s_state.track = track;
            /* Remembered so the device comes back where it left off. Written
             * per track, not per second: NVS is flash, and settings.c holds it
             * in RAM until it has been still for a few seconds anyway. */
            settings_set_last(album, track);
            const lib_track_t *t = library_track(album, track);
            strlcpy(s_state.title, t ? t->name : "", sizeof(s_state.title));
            library_track_path(album, track, path, sizeof(path));
            ESP_LOGI(TAG, "play %s", path);
            /* The builder reports "sounds like half volume". Log what the codec is
             * actually being asked for, so a low SETTING and a low CODEC
             * RESPONSE can be told apart. */
            applog("[play] vol %d, sink %s", s_state.volume,
                   s_state.sink == SINK_SPEAKER ? "spk" :
                   s_state.sink == SINK_LINEOUT ? "jack" : "bt");

            cmd_t why = play_file(path);

            /* A new selection arrives as CMD_PLAY, and play_file() has already
             * consumed it — so it MUST be handled here. Falling through to
             * track++ instead makes a tap on a different artist silently
             * advance the album that is already playing, which is exactly the
             * "tapped Al Green, got the next Adele track" bug. */
            if (why == CMD_PLAY) {
                album = s_want_album;
                track = s_want_track;
                continue;
            }
            if (why == CMD_STOP) break;
            if (why == CMD_PREV) { track = (track > 0) ? track - 1 : 0; continue; }

            /* 🔑 SHUFFLE DECIDES THE NEXT TRACK, for both a natural end and a
             * NEXT press — a shuffle that only applies when a song runs out
             * would ignore the button a child presses most.
             *
             * The scope is re-read from the album PLAYING NOW, so choosing a
             * different album while shuffling moves the scope with it. */
            {
                int na, nt;
                if (shuf_next(album, &na, &nt)) { album = na; track = nt; continue; }
            }
            track++;                               /* NEXT and natural end */
        }
        /* Nothing more is coming, so let the dongle play out what it holds and
         * then fall silent. This is the one place a DRAIN is the right answer. */
        s_state.playing = false;
        s_state.progress = 0;
    }
}

void player_init(void)
{
    s_state.album  = -1;
    s_state.track  = 0;
    s_state.volume = settings_volume();
    s_state.sink   = settings_sink();
    audio_init(44100);

    /* 🚨 APPLY THE SAVED VOLUME AT BOOT — audio_init() DOES NOT.
     *
     * codec_open() calls esp_codec_dev_set_out_vol() and stops there, and that
     * call leaves the ES8311's DAC register at about -3dB (measured 2026-08-16
     * — it is why this firmware was quieter than ESPHome on the same board).
     * The per-board gain tuned into NVS is applied by audio_set_volume() and by
     * NOTHING ELSE, so until the user moved the slider or started a track, the
     * codec sat de-tuned and every earcon played into it.
     *
     * The builder, 2026-08-17, on the new-message beep: "i still didnt here any thing".
     * The message path was fine; the codec was simply never turned up. Sounds
     * that arrive UNPROMPTED are the ones that expose this — the cues inside a
     * voice interaction do not, because that path sets the volume itself. */
    /* ⚠️ Respect a mute here too. Opening a track re-applies the volume because
     * codec_open() de-tunes the register, and a muted handset that starts
     * singing on the next track is worse than one that never muted at all. */
    audio_set_volume(s_muted ? 0 : s_state.volume);

    if (s_state.sink != SINK_SPEAKER) amp(false);
    /* 🚨 32KB, NOT the 8KB that looks generous. minimp3's synthesis runs on the
     * caller's stack, and starving it does not fail cleanly — it corrupts the
     * stack and dies as a LoadProhibited on a garbage address inside some
     * unrelated trivial function, seconds after playback starts. Setting
     * CONFIG_ESP_MAIN_TASK_STACK_SIZE does NOT help here: that sizes app_main's
     * task, and decoding happens on this one. */
    /* 🔑 THE STACK LIVES IN PSRAM — IT IS THE SINGLE BIGGEST INTERNAL-RAM ITEM.
     *
     * 32KB of internal RAM for one task stack is about what the whole BLE
     * controller costs, and it is the reason BLE and WiFi cannot both be
     * resident: WiFi needs ~44KB of DMA-capable internal RAM and BLE ~31KB, on
     * a board with ~65KB free (measured 2026-08-17). Recovering this closes
     * that gap.
     *
     * ⚠️ THE PCM BUFFER WAS NEVER THE PROBLEM. minimp3's output buffer is a
     * ~4.6KB malloc; the 32KB is STACK, because minimp3 runs its synthesis on
     * the caller's stack. So there is nothing to point at a different
     * allocator - the stack itself has to move, which is what
     * xTaskCreatePinnedToCoreWithCaps() is for.
     *
     * ⚠️ SAFE ONLY BECAUSE FLASH OPERATIONS STALL BOTH CORES. Code cannot touch
     * PSRAM while the cache is disabled, and NVS writes disable it - msg.c
     * commits one on every incoming message. IDF's flash driver halts the other
     * core for the duration, so no task is running from a PSRAM stack in that
     * window. Remove that guarantee and this becomes a random crash.
     *
     * ⚠️ WATCH FOR STUTTER. PSRAM is several times slower than internal SRAM on
     * a cache miss, and this stack is hot. MP3 decode was measured at real time
     * with an internal stack; if playback now breaks up, that is this line and
     * nothing else. */
    xTaskCreatePinnedToCoreWithCaps(player_task, "player", 32768, NULL, 5,
                                    &s_task, 1, MALLOC_CAP_SPIRAM);
}

/* Load the last track WITHOUT starting it.
 *
 * Deliberately not auto-play. A device that begins blasting music the instant
 * it is switched on is hostile in a bedroom at night, and this one lives in a
 * kid's pocket where the power switch gets knocked. Resume means "Play is ready
 * to carry on where you were", not "music now". */
void player_restore(void)
{
    int album = settings_last_album();
    int track = settings_last_track();
    /* The card may have changed since the last boot, so the saved index is not
     * trusted — library_album_by_index() is the bounds check. */
    if (album < 0 || !library_album_by_index(album)) return;
    if (track < 0 || track >= library_track_count(album)) track = 0;

    s_state.album = album;
    s_state.track = track;
    const lib_track_t *t = library_track(album, track);
    strlcpy(s_state.title, t ? t->name : "", sizeof(s_state.title));
    ESP_LOGI(TAG, "resume ready: album %d track %d (%s)", album, track,
             s_state.title);
}
