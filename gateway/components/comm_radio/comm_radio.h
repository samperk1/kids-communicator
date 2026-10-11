#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include "espnow_microphone.h"

#include <string>

namespace esphome {
namespace comm_radio {

/* The fleet's ESP-NOW radio, running inside ESPHome.
 *
 * 🔑 THE RADIO CODE IS NOT REIMPLEMENTED HERE. espnow.c and fleet.c are
 * SYMLINKS to the handset tree — the same source that builds for the S3s and
 * for the C gateway. A gateway that reimplemented the wire format, the per-link
 * LMKs, the AES-GCM or the pairing ratchet would drift from the handsets
 * silently, and the failure would look like a radio fault rather than a
 * protocol mismatch.
 *
 * 🚨 START IS DEFERRED UNTIL THE STATION IS ASSOCIATED. ESPHome is always-
 * associated by design, and a station's channel is the AP's — esp_wifi_set_
 * channel() cannot move it while connected. Starting ESP-NOW before the
 * association would pin channel 6, then let the association silently move the
 * radio somewhere else, which is the exact fault mode that reads as dead
 * hardware. So we wait, then check what channel we actually landed on.
 */
class CommRadio : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;

  /* AFTER_WIFI so the WiFi component is constructed first; the actual radio
   * start waits for an association in loop(). */
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_channel(uint8_t channel) { this->channel_ = channel; }
  void set_gw_name(const char *name) { this->name_ = name; }

  /* 🚨 THE FLEET CODE IS THIS BOARD'S IDENTITY, AND A REFLASH DESTROYS IT.
   * The PMK is PBKDF2-SHA256(code, "kids-comm/pmk/v1"), so the SAME code always
   * derives the SAME key — which is the only reason a gateway can be reflashed
   * at all. ESPHome's partition table puts NVS at 0x390000 where the C
   * firmware had it at 0x9000, so a first flash wipes the old roster and PMK
   * and they cannot be read back. Supplying the code here re-derives the PMK on
   * every boot; the per-peer LMKs still have to be re-paired. */
  void set_fleet_code(const char *code) { this->fleet_code_ = code; }

  /* Where audio arriving over the radio is handed to ESPHome. Set by the
   * microphone platform's codegen, so a config without one fails at BUILD time
   * rather than with a null pointer on the first question anyone asks. */
  void set_microphone(EspNowMicrophone *mic) { this->mic_ = mic; }
  EspNowMicrophone *get_microphone() const { return this->mic_; }

  /* Gate 3.1 only: push one synthetic frame so the path from this component to
   * an `on_data:` automation can be proved without a handset, a recording or a
   * radio in the way. Remove once real audio flows. */
  void set_self_test(bool on) { this->self_test_ = on; }

  /* Forwarded to the ask relay. These are what the YAML's voice_assistant
   * triggers call, so the wiring between ESPHome's pipeline and the fleet's
   * wire protocol is visible in the config rather than buried in C++. */
  void ask_heard(const std::string &text);
  void ask_replied(const std::string &text);
  void ask_finished();
  void ask_failed(const std::string &code, const std::string &message);

  /* The paired handsets, one roster slot at a time (0..7), for Home Assistant.
   * gateway.yaml shows each slot as a text sensor with a Forget button beside
   * it, so ONE handset with a broken pairing can be fixed without wiping the
   * others — before this, the only cure was erasing the gateway's NVS from a
   * PC, which forgot every handset at once. */
  std::string slot_text(int slot);
  void forget_slot(int slot);

 protected:
  bool start_radio_();

  /* 🚨 DERIVING THE PMK ON ESPHome's LOOP TASK REBOOT-LOOPS THE BOARD.
   * fleet_set_code() is 100,000 PBKDF2-HMAC-SHA256 iterations. That is ~1s on
   * an S3's hardware SHA and well over 10s on a plain ESP32's software port —
   * long enough that ESPHome's loop task misses the task watchdog and the chip
   * panics, every boot, forever. Measured 2026-09-11.
   *
   * ⚠️ THE ITERATION COUNT IS NOT THE KNOB. It is a fleet-wide security
   * parameter AND an input to the derivation: lowering it here would produce a
   * DIFFERENT key from the handsets', and the gateway would fail to pair with
   * no error that says why. It has to move off the loop task instead. */
  static void derive_task_(void *arg);

  uint8_t channel_{6};
  const char *name_{"Gateway"};
  const char *fleet_code_{""};
  EspNowMicrophone *mic_{nullptr};
  bool self_test_{false};
  bool self_tested_{false};
  bool started_{false};
  volatile bool pmk_ready_{false};
  uint32_t last_report_{0};
};

}  // namespace comm_radio
}  // namespace esphome
