#include "comm_ask.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <cstring>

extern "C" {
#include "espnow.h"
#include "gw_wire.h"
}

namespace esphome {
namespace comm_radio {

static const char *const TAG = "comm_radio.ask";

/* mic.c:21 on the handset — the RIFF header it writes over the front of every
 * recording. Not ours to change; see the note where it is skipped. */
static const int WAV_HEADER_BYTES = 44;

/* 🚨 A CEILING, NOT A DURATION — AND THAT DISTINCTION IS THE WHOLE FIX.
 * Feeding a FIXED 700ms of silence and then stopping did not work: it just
 * moved my own forced ending 700ms later. Home Assistant's VAD never fired its
 * end event, because the stream vanished while it was still deciding.
 * A real microphone does not stop; it keeps producing silence until the
 * pipeline says it is finished. So this is only the point at which we give up
 * waiting for HA and end it ourselves. */
static const uint32_t MAX_TAIL_SILENCE_MS = 4000;

/* 🚨 HOME ASSISTANT CANNOT HEAR SPEECH IN THE FIRST SECOND OF A STREAM.
 * Its pipeline debug, across every device, never marked speech as starting
 * earlier than ~1050ms in. A kid's quick "what time is it" — press, speak,
 * let go, 1.5s in all — was reported as no speech at all (no stt-vad-start,
 * no transcript) and the handset showed "lost audio". Tested 2026-10-08: holding
 * for a beat before speaking made it work every time. A real satellite has
 * been listening for a while before anyone speaks; this one is opened cold by
 * every question, so it gets that second of quiet here instead. */
static const uint32_t LEAD_SILENCE_MS = 1000;

AskRelay global_ask_relay;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

/* The window, and the only large buffer in this file: 32 x 194 = 6,208 bytes,
 * which a no-PSRAM gateway can spare from internal RAM. */
static uint8_t s_slot[GW_ASK_WIN][GW_ASK_DATA_MAX];
static volatile uint16_t s_slot_len[GW_ASK_WIN];
static volatile bool s_slot_full[GW_ASK_WIN];

static struct {
  volatile bool active;
  volatile bool aborted;
  uint16_t peer;
  uint8_t seq;
  uint32_t bytes;
  uint16_t chunks;
  uint16_t rate;
  volatile uint16_t expect;   /* the next index we will push to the pipeline */
  volatile int64_t last_rx_us;
} s_ask;

/* Send an ack naming the lowest index we still need.
 *
 * 🔑 CUMULATIVE, SO A LOST ACK COSTS NOTHING. The next one supersedes it, which
 * is why there is no ack retransmit anywhere in this file. */
void AskRelay::ack_() {
  gw_ask_ack_t a = {};
  a.ver = GW_VER;
  a.op = GW_OP_ASK;
  a.seq = s_ask.seq;
  a.sub = ASK_ACK;
  a.next = s_ask.expect;
  espnow_gw_respond(s_ask.peer, &a, sizeof(a));
}

void AskRelay::done_(uint8_t err, uint16_t status) {
  gw_ask_done_t d = {};
  d.ver = GW_VER;
  d.op = GW_OP_ASK;
  d.seq = s_ask.seq;
  d.sub = ASK_DONE;
  d.err = err;
  d.status = status;
  espnow_gw_respond(s_ask.peer, &d, sizeof(d));
  ESP_LOGI(TAG, "%04X finished: err %u status %u", s_ask.peer, err, status);
  s_ask.active = false;
}

void AskRelay::send_text_(uint8_t sub, const char *text) {
  const int room = (int) sizeof(((gw_ask_text_t *) nullptr)->text);
  const int n = (int) strlen(text);
  if (n <= 0)
    return;
  for (int off = 0, part = 0; off < n; off += room, part++) {
    gw_ask_text_t t = {};
    t.ver = GW_VER;
    t.op = GW_OP_ASK;
    t.seq = s_ask.seq;
    t.sub = sub;
    t.part = (uint8_t) part;
    const int take = (n - off > room) ? room : (n - off);
    memcpy(t.text, text + off, take);
    t.last = (off + take >= n) ? 1 : 0;
    espnow_gw_respond(s_ask.peer, &t, (int) (6 + take));
    /* One frame per tick: these follow a burst that has just saturated the
     * link, and stacking them behind it only invites the driver to refuse. */
    vTaskDelay(1);
  }
}

/* ------------------------------------------------------------------ *
 * The worker. Everything slow happens here.
 *
 * 🚨 NOT ON ESPHome's LOOP TASK, AND THIS IS NOT A PREFERENCE. Deriving the
 * fleet PMK on the loop task reboot-looped this very board earlier tonight,
 * and ESPHome warned at 130ms that the loop was already being held too long.
 * A 12-second recording moving at 58 KB/s would hold it for seconds.
 * ------------------------------------------------------------------ */
void AskRelay::run_transfer_() {
  if (this->mic_ == nullptr) {
    ESP_LOGE(TAG, "no microphone configured - nothing can receive this audio");
    this->done_(ASK_ERR_NET, 0);
    return;
  }

  ESP_LOGI(TAG, "streaming %u bytes (%u chunks) into the pipeline",
           (unsigned) s_ask.bytes, s_ask.chunks);
  const int64_t t0 = esp_timer_get_time();
  bool bad = false;

  this->replied_ = false;

  /* Ask the loop task to open a pipeline, then wait for the microphone to
   * actually be capturing.
   *
   * 🚨 WAITING IS NOT OPTIONAL. push() drops audio while the microphone is
   * stopped — correctly, so a late frame from an abandoned ask cannot bleed
   * into the next question — which means pushing before the pipeline is up
   * would silently eat the START of the recording. The symptom would be a
   * transcript missing its first word or two, which reads as poor speech
   * recognition rather than as a race. */
  this->want_start_ = true;
  const int64_t wait0 = esp_timer_get_time();
  while (!this->mic_->is_running()) {
    if (esp_timer_get_time() - wait0 > 5LL * 1000 * 1000) {
      ESP_LOGE(TAG, "the voice pipeline never started - is Home Assistant connected?");
      this->want_start_ = false;
      this->done_(ASK_ERR_NET, 0);
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  ESP_LOGD(TAG, "pipeline open after %dms",
           (int) ((esp_timer_get_time() - wait0) / 1000));

  /* 🚨 THE AUDIO MUST GO IN AT ROUGHLY THE SPEED IT WAS RECORDED, AND THIS IS
   * NOT A POLISH ITEM — IT IS THE BUG THAT BROKE GATE 3.3 FIRST TIME.
   *
   * The radio delivers faster than realtime: 52 KB/s measured against the
   * 32 KB/s that 16 kHz 16-bit mono consumes. Pushing at that rate fills the
   * native API's TCP buffer with audio, and the NEXT control message loses.
   * What lost was the one that matters — voice_assistant's own stop:
   *
   *     [W][voice_assistant]: Stop request dropped, TCP buffer full
   *
   * Home Assistant then never learned the utterance had ended, never finalised
   * the transcript, and the handset sat until its own 30-second timeout. The
   * symptom is "no answer", which looks like the agent failing rather than the
   * transport drowning a 2-byte message in audio.
   *
   * 🔑 A SMALL LEAD IS KEPT ON PURPOSE. Feeding exactly at realtime would leave
   * the pipeline starved by any jitter in the radio; LEAD_US of audio in hand
   * absorbs that without ever building the backlog that caused the drop. */
  const auto info = this->mic_->get_audio_stream_info();
  const uint32_t bytes_per_sec = (uint32_t) info.get_sample_rate() *
                                 info.get_channels() *
                                 (info.get_bits_per_sample() / 8);
  const int64_t LEAD_US = 200 * 1000;
  const int64_t stream_t0 = esp_timer_get_time();

  /* 🚨 THE HANDSET SENDS A WAV FILE; THE PIPELINE WANTS RAW PCM.
   * mic.c:21 defines WAV_HDR as 44 and mic.c:136 writes a RIFF header over the
   * front of the recording, because the C gateway POSTed the whole thing to
   * Home Assistant's STT endpoint, which expects a file. voice_assistant is
   * fed samples, not a file — so those 44 bytes would be played into the
   * pipeline as ~22 samples of whatever "RIFFxxxxWAVEfmt " sounds like.
   * ⚠️ Skipped here rather than on the handset: the handset's WAV is correct
   * for every other consumer, and changing the wire to suit this one gateway
   * is exactly the drift this project keeps paying for. */
  int header_left = WAV_HEADER_BYTES;

  /* The lead-in, paced like everything else: pushed at once it would put a
   * whole second in the TCP buffer — the very backlog described above. The
   * handset's chunks keep arriving into the window meanwhile; one second is
   * inside its 1.5s stall timer, so it simply waits for the first ack. */
  if (bytes_per_sec > 0) {
    static const uint8_t SILENCE[256] = {0};
    const uint32_t lead_bytes = bytes_per_sec * LEAD_SILENCE_MS / 1000;
    uint32_t sent = 0;
    while (sent < lead_bytes && !s_ask.aborted) {
      const uint32_t n = (lead_bytes - sent < sizeof(SILENCE)) ? lead_bytes - sent
                                                               : sizeof(SILENCE);
      this->mic_->push(SILENCE, n);
      sent += n;
      const int64_t audio_us =
          (int64_t) this->mic_->pushed_bytes() * 1000000 / bytes_per_sec;
      const int64_t real_us = esp_timer_get_time() - stream_t0;
      if (audio_us - real_us > LEAD_US)
        vTaskDelay(pdMS_TO_TICKS((audio_us - real_us - LEAD_US) / 1000));
      else
        vTaskDelay(1);
    }
  }
  const uint32_t lead_bytes_pushed = this->mic_->pushed_bytes();

  while (s_ask.expect < s_ask.chunks && !s_ask.aborted) {
    const int slot = s_ask.expect % GW_ASK_WIN;
    if (!s_slot_full[slot]) {
      /* Nothing to push yet. ⚠️ Ack anyway: the handset winds back to the last
       * acked index when it stalls, so an ack here is how a lost frame gets
       * resent rather than the upload dying. */
      if (esp_timer_get_time() - s_ask.last_rx_us > 500 * 1000) {
        this->ack_();
        s_ask.last_rx_us = esp_timer_get_time();
      }
      if (esp_timer_get_time() - t0 > 40LL * 1000 * 1000) {
        ESP_LOGW(TAG, "gave up waiting at %u/%u", s_ask.expect, s_ask.chunks);
        bad = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }

    {
      const uint8_t *chunk = s_slot[slot];
      int chunk_len = s_slot_len[slot];
      if (header_left > 0) {
        const int skip = (header_left < chunk_len) ? header_left : chunk_len;
        chunk += skip;
        chunk_len -= skip;
        header_left -= skip;
      }
      if (chunk_len > 0)
        this->mic_->push(chunk, chunk_len);
    }
    s_slot_full[slot] = false;
    s_ask.expect++;

    /* Hold the feed to realtime plus LEAD_US. Delaying here also throttles the
     * RADIO, because the handset stalls waiting for the ack it is owed — one
     * brake, applied in one place, for both halves of the path. */
    if (bytes_per_sec > 0) {
      const int64_t audio_us =
          (int64_t) this->mic_->pushed_bytes() * 1000000 / bytes_per_sec;
      const int64_t real_us = esp_timer_get_time() - stream_t0;
      const int64_t lead_us = audio_us - real_us;
      if (lead_us > LEAD_US) {
        const uint32_t nap_ms = (uint32_t) ((lead_us - LEAD_US) / 1000);
        if (nap_ms > 0)
          vTaskDelay(pdMS_TO_TICKS(nap_ms));
      }
    }

    /* Ack often enough that the handset's window never empties: every eight
     * chunks is ~1.5KB of credit returned well before 32 are spent. */
    if ((s_ask.expect % 8) == 0 || s_ask.expect == s_ask.chunks)
      this->ack_();
  }

  /* 🔑 KEEP BEING A MICROPHONE UNTIL THE PIPELINE SAYS STOP.
   * Home Assistant decides an utterance is over when its VAD stops hearing
   * speech — it does NOT finalise because the stream ended. The handset stops
   * recording the instant the speaker stops talking, so without this the audio
   * ends mid-word from the VAD's point of view.
   *
   * 🔑 THE EXIT CONDITION IS voice_assistant STOPPING OUR MICROPHONE. When HA
   * calls the end of speech, VoiceAssistant moves to STOP_MICROPHONE and calls
   * stop() on us, so is_running() going false IS the pipeline saying "I have
   * what I need". Watching for that beats any fixed duration, because the
   * answer depends on HA's own finished-speaking setting, which lives over
   * there and can be changed without telling us. */
  uint32_t silence_bytes = 0;
  if (!bad && !s_ask.aborted && bytes_per_sec > 0) {
    static const uint8_t SILENCE[256] = {0};
    const int64_t silence_t0 = esp_timer_get_time();
    const int64_t cap_us = (int64_t) MAX_TAIL_SILENCE_MS * 1000;
    while (this->mic_->is_running() &&
           (esp_timer_get_time() - silence_t0) < cap_us) {
      this->mic_->push(SILENCE, sizeof(SILENCE));
      silence_bytes += sizeof(SILENCE);
      const int64_t audio_us =
          (int64_t) this->mic_->pushed_bytes() * 1000000 / bytes_per_sec;
      const int64_t real_us = esp_timer_get_time() - stream_t0;
      if (audio_us - real_us > LEAD_US)
        vTaskDelay(pdMS_TO_TICKS((audio_us - real_us - LEAD_US) / 1000));
      else
        vTaskDelay(1);   /* never spin: this loop can run for seconds */
    }
    ESP_LOGD(TAG, "fed %ums of trailing silence, pipeline %s",
             (unsigned) (silence_bytes * 1000 / bytes_per_sec),
             this->mic_->is_running() ? "still listening" : "ended it");
  }

  const uint32_t got = this->mic_->pushed_bytes();
  const int ms = (int) ((esp_timer_get_time() - t0) / 1000);
  ESP_LOGI(TAG, "%u lead-in + %u audio + %u silence bytes in %dms (%d B/s)",
           (unsigned) lead_bytes_pushed,
           (unsigned) (got - silence_bytes - lead_bytes_pushed),
           (unsigned) silence_bytes, ms,
           ms > 0 ? (int) ((int64_t) got * 1000 / ms) : 0);

  /* ⚠️ ONLY IF HOME ASSISTANT NEVER ENDED IT ITSELF. Stopping a pipeline that
   * is still deciding is precisely what produced two empty transcripts, so
   * this is now a backstop for a pipeline that has gone quiet, not the normal
   * path. When VAD worked, the microphone is already stopped by here. */
  if (this->mic_->is_running()) {
    ESP_LOGW(TAG, "HA never called the end of speech after %ums - stopping it",
             MAX_TAIL_SILENCE_MS);
    this->want_stop_ = true;
  }

  if (bad || s_ask.aborted) {
    this->done_(ASK_ERR_GAP, 0);
    return;
  }

  /* The answer arrives later, on the loop task, through heard()/replied()/
   * finished(). This task's job is over — it must NOT sit here holding the
   * window buffers while Home Assistant thinks. */
}

void AskRelay::worker_(void *arg) {
  auto *self = static_cast<AskRelay *>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (!s_ask.active)
      continue;
    self->run_transfer_();
  }
}

/* ------------------------------------------------------------------ *
 * called from the ESP-NOW RX task. NEVER blocks here.
 * ------------------------------------------------------------------ */
void AskRelay::on_frame(uint16_t from_id, const uint8_t *p, int len) {
  if (len < 4)
    return;

  if (p[3] == ASK_BEGIN) {
    if (len < (int) sizeof(gw_ask_begin_t))
      return;
    gw_ask_begin_t b;
    memcpy(&b, p, sizeof(b));

    /* One at a time. Two handsets streaming into one pipeline would interleave
     * into noise, and saying so is far better than producing a transcript of
     * two children at once. */
    if (s_ask.active && from_id != s_ask.peer) {
      gw_ask_done_t d = {};
      d.ver = GW_VER;
      d.op = GW_OP_ASK;
      d.seq = b.seq;
      d.sub = ASK_DONE;
      d.err = ASK_ERR_BUSY;
      espnow_gw_respond(from_id, &d, sizeof(d));
      return;
    }
    if (!b.chunks || !b.bytes)
      return;

    memset((void *) s_slot_full, 0, sizeof(s_slot_full));
    s_ask.peer = from_id;
    s_ask.seq = b.seq;
    s_ask.bytes = b.bytes;
    s_ask.chunks = b.chunks;
    s_ask.rate = b.rate ? b.rate : 16000;
    s_ask.expect = 0;
    s_ask.aborted = false;
    s_ask.last_rx_us = esp_timer_get_time();
    s_ask.active = true;
    ESP_LOGI(TAG, "%04X asking: %u bytes, %u chunks at %u Hz", from_id,
             (unsigned) b.bytes, b.chunks, s_ask.rate);
    this->ack_();  /* "send from 0" */
    if (this->worker_handle_ != nullptr)
      xTaskNotifyGive(static_cast<TaskHandle_t>(this->worker_handle_));
    return;
  }

  if (p[3] == ASK_DATA) {
    if (!s_ask.active || from_id != s_ask.peer || p[2] != s_ask.seq)
      return;
    if (len < 6)
      return;
    gw_ask_data_t d;
    const int n = len > (int) sizeof(d) ? (int) sizeof(d) : len;
    memcpy(&d, p, n);

    const uint16_t want = s_ask.expect;
    /* Below the window: already pushed and committed. Re-ack so a handset that
     * missed the ack stops resending, then drop it. */
    if (d.index < want) {
      this->ack_();
      return;
    }
    if (d.index >= (uint16_t) (want + GW_ASK_WIN))
      return; /* too far ahead */

    const int slot = d.index % GW_ASK_WIN;
    if (s_slot_full[slot])
      return; /* already have it */
    const int payload = n - 6;
    if (payload <= 0 || payload > GW_ASK_DATA_MAX)
      return;
    memcpy(s_slot[slot], d.data, payload);
    s_slot_len[slot] = (uint16_t) payload;
    s_slot_full[slot] = true;
    s_ask.last_rx_us = esp_timer_get_time();
  }
}

void AskRelay::loop() {
  if (this->va_ == nullptr)
    return;
  if (this->want_start_) {
    this->want_start_ = false;
    /* continuous = false: one question, not an open microphone.
     *
     * 🚨 silence_detection = TRUE, AND THE FIRST ATTEMPT HAD IT FALSE.
     * The reasoning for false was sound and wrong: we know exactly when the
     * recording ends, so telling the pipeline outright should beat waiting for
     * a VAD. But Home Assistant does not finalise a transcript because the
     * STREAM stopped — it finalises when its VAD hears speech end. With
     * detection off, the run ended with no transcript at all. Measured twice,
     * 2026-09-11. The tail of silence above is what lets the VAD do its job. */
    this->va_->request_start(false, true);
  }
  if (this->want_stop_) {
    this->want_stop_ = false;
    this->va_->request_stop();
  }
}

void AskRelay::heard(const std::string &text) {
  if (!s_ask.active)
    return;
  ESP_LOGI(TAG, "heard: %s", text.c_str());
  this->send_text_(ASK_TEXT, text.c_str());
}

void AskRelay::replied(const std::string &text) {
  if (!s_ask.active)
    return;
  ESP_LOGI(TAG, "answer: %s", text.c_str());
  this->send_text_(ASK_REPLY, text.c_str());
  this->replied_ = true;
}

void AskRelay::finished() {
  if (!s_ask.active)
    return;
  /* ⚠️ AN on_end WITH NO ANSWER IS A FAILURE, NOT A SUCCESS. The pipeline ends
   * for good reasons and bad ones alike, and a handset told ASK_OK with no text
   * would show an empty bubble and no way to know why. ASK_ERR_EMPTY is exactly
   * this case and the handset already has words for it. */
  if (!this->replied_) {
    ESP_LOGW(TAG, "pipeline ended without an answer");
    this->done_(ASK_ERR_EMPTY, 0);
    return;
  }
  this->done_(ASK_OK, 0);
}

void AskRelay::failed(const std::string &code, const std::string &message) {
  ESP_LOGE(TAG, "pipeline error: %s / %s", code.c_str(), message.c_str());
  if (!s_ask.active)
    return;
  /* The handset distinguishes "HA said no" from "HA was never reached" because
   * those are two different jobs for whoever is holding it. Anything the
   * pipeline itself reports means HA was reached. */
  this->done_(ASK_ERR_HA, 0);
}

/* The radio's callback is a plain function pointer with no user-data argument,
 * so it lands here and is forwarded to the one relay. */
static void ask_frame_trampoline(uint16_t from_id, const uint8_t *p, int len) {
  global_ask_relay.on_frame(from_id, p, len);
}

void AskRelay::setup(EspNowMicrophone *mic) {
  this->mic_ = mic;
  TaskHandle_t h = nullptr;
  /* 6KB was not enough in the C gateway and the failure mode was a reboot
   * mid-conversation rather than a clean error, so 8192 with the big buffers
   * in BSS. The headroom is logged after each ask rather than assumed. */
  xTaskCreate(&AskRelay::worker_, "comm_ask", 8192, this, 5, &h);
  this->worker_handle_ = h;
  espnow_gw_on_request(&ask_frame_trampoline);
  ESP_LOGI(TAG, "ask relay ready, window %u x %u bytes", GW_ASK_WIN,
           GW_ASK_DATA_MAX);
}

}  // namespace comm_radio
}  // namespace esphome

#endif  // USE_ESP32
