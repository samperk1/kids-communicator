#pragma once

#include "esphome/components/microphone/microphone.h"
#include "esphome/core/component.h"

#include <cstdint>
#include <vector>

namespace esphome {
namespace comm_radio {

/* A microphone whose audio arrives over ESP-NOW instead of from an I2S pin.
 *
 * 🔑 WHY THIS EXISTS AT ALL. The handsets already record at 16 kHz, 16-bit,
 * mono (firmware/main/mic.h:15) and ship the WAV to a gateway in 194-byte
 * chunks. Home Assistant's assist pipeline wants exactly that format from a
 * microphone. So rather than translating the fleet's ask protocol into HTTP
 * calls, the gateway presents the incoming chunks AS a microphone and lets
 * ESPHome's own voice_assistant carry them over the native API — no HTTP, no
 * token, no second protocol to keep in step.
 *
 * ⚠️ THIS CLASS DELIBERATELY DOES NO BUFFERING. A gateway may be a plain ESP32 with
 * no PSRAM and one binary serves both gateways, so a 384 KB recording can never
 * be held anywhere. Chunks are pushed straight through to whoever is listening.
 * The window and the acks that make that safe live in the ask handler, not
 * here — see gw_wire.h.
 */
class EspNowMicrophone : public microphone::Microphone, public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

  void start() override;
  void stop() override;

  /* Hand one slice of audio to the pipeline. Safe to call when stopped — it is
   * dropped, which is the correct answer for a frame from an ask that has
   * already finished or been abandoned. Returns whether it was delivered, so a
   * caller can count drops rather than discover them later. */
  bool push(const uint8_t *data, size_t len);

  uint32_t pushed_bytes() const { return this->pushed_bytes_; }

 protected:
  uint32_t pushed_bytes_{0};
  uint32_t dropped_bytes_{0};
};

}  // namespace comm_radio
}  // namespace esphome
