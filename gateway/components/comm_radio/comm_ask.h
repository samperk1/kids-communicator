#pragma once

#include "espnow_microphone.h"
#include "esphome/components/voice_assistant/voice_assistant.h"

#include <cstdint>
#include <string>

namespace esphome {
namespace comm_radio {

/* The gateway half of GW_OP_ASK — a handset's recording, arriving over the
 * radio and going into ESPHome's audio pipeline.
 *
 * 🔑 THE TRANSPORT IS A PORT OF THE EARLIER C GATEWAY'S gw_ask.c (not published), NOT A REWRITE.
 * The window rules, the cumulative ack and the "contiguous prefix only" rule
 * are already worked out and already agreed with the handset end; reinventing
 * them would be inventing a second opinion about a protocol that has exactly
 * two participants. Only the DESTINATION changed: where the C gateway wrote
 * each chunk into an open HTTP request to Home Assistant, this pushes it into
 * an EspNowMicrophone and lets ESPHome's voice_assistant carry it over the
 * native API — no HTTP, no token.
 *
 * 🚨 THE "CANNOT REWIND" RULE SURVIVES THE CHANGE, AND THAT IS WHY THE WINDOW
 * IS STILL HERE. Audio pushed into the pipeline is as committed as a byte
 * written to a socket: it is gone downstream and a gap cannot be repaired
 * afterwards. So this still writes only the contiguous prefix it holds, and
 * still acks the lowest index it needs.
 *
 * ⚠️ NOTHING IS BUFFERED WHOLE. A gateway may be a plain ESP32 with no PSRAM and one
 * binary serves both gateways, so a 384 KB recording has nowhere to live. The
 * window is 32 x 194 = 6,208 bytes and that is the entire memory cost.
 */
class AskRelay {
 public:
  /* Starts the worker task and registers with the radio. Call once, after
   * espnow_begin() has succeeded. */
  void setup(EspNowMicrophone *mic);

  /* Called from the ESP-NOW RX task. NEVER blocks — see the note in the .cpp
   * about why the work happens on the worker instead. */
  void on_frame(uint16_t from_id, const uint8_t *p, int len);

  void set_voice_assistant(voice_assistant::VoiceAssistant *va) { this->va_ = va; }

  /* 🚨 SERVICED ON ESPHome's LOOP TASK, AND THAT IS THE WHOLE REASON IT EXISTS.
   * VoiceAssistant is built to be driven from the loop; calling request_start()
   * straight from the radio worker would be a cross-task call into a component
   * that never expected one. The worker therefore only raises a flag and this
   * turns it into the actual call. */
  void loop();

  /* The pipeline's answers, coming back from the YAML triggers — which run on
   * the loop task. Named for what the HANDSET will do with them, not for the
   * ESPHome event that produced them. */
  void heard(const std::string &text);   /* on_stt_end  -> ASK_TEXT  */
  void replied(const std::string &text); /* on_tts_start-> ASK_REPLY */
  void finished();                       /* on_end      -> ASK_DONE ok */
  void failed(const std::string &code, const std::string &message);

 protected:
  void ack_();
  void done_(uint8_t err, uint16_t status);
  void send_text_(uint8_t sub, const char *text);
  static void worker_(void *arg);
  void run_transfer_();

  EspNowMicrophone *mic_{nullptr};
  voice_assistant::VoiceAssistant *va_{nullptr};
  void *worker_handle_{nullptr};

  volatile bool want_start_{false};
  volatile bool want_stop_{false};
  /* Set once the answer has been sent, so a late on_end cannot close an ask
   * that already finished — or a failed one twice. */
  volatile bool replied_{false};
};

/* One relay, because there is one radio. The ESP-NOW callback is a plain
 * function pointer with no user-data argument, so the instance has to be
 * reachable without one. */
extern AskRelay global_ask_relay;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace comm_radio
}  // namespace esphome
