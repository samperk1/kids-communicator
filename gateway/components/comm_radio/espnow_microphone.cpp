#include "espnow_microphone.h"

#include "esphome/core/log.h"

namespace esphome {
namespace comm_radio {

static const char *const TAG = "comm_radio.microphone";

void EspNowMicrophone::setup() {
  /* 16-bit, 1 channel, 16 kHz — and every one of those matches the handset's
   * recorder rather than being a preference. MIC_RATE is 16000 in
   * firmware/main/mic.h, which is also HA's pipeline rate, so nothing on
   * this path resamples. If the handset's rate ever changes, THIS is the line
   * that has to change with it, and a mismatch will sound like fast or slow
   * speech rather than reporting itself. */
  this->audio_stream_info_ = audio::AudioStreamInfo(16, 1, 16000);
  this->state_ = microphone::STATE_STOPPED;
}

void EspNowMicrophone::start() {
  if (this->state_ == microphone::STATE_RUNNING)
    return;
  this->pushed_bytes_ = 0;
  this->dropped_bytes_ = 0;
  this->state_ = microphone::STATE_RUNNING;
  ESP_LOGD(TAG, "capture started");
}

void EspNowMicrophone::stop() {
  if (this->state_ == microphone::STATE_STOPPED)
    return;
  this->state_ = microphone::STATE_STOPPED;
  ESP_LOGD(TAG, "capture stopped after %" PRIu32 " bytes (%" PRIu32 " dropped)",
           this->pushed_bytes_, this->dropped_bytes_);
}

bool EspNowMicrophone::push(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0)
    return false;

  if (this->state_ != microphone::STATE_RUNNING) {
    /* Not an error. A late frame from an abandoned ask lands here, and
     * dropping it is what keeps it out of the NEXT question's audio. */
    this->dropped_bytes_ += len;
    return false;
  }

  /* ⚠️ The callback signature is a std::vector, so this copies. That is
   * ESPHome's interface, not a choice — and at 194 bytes a chunk it is cheap
   * next to the radio time that delivered it. */
  std::vector<uint8_t> buf(data, data + len);
  this->data_callbacks_.call(buf);
  this->pushed_bytes_ += len;
  return true;
}

void EspNowMicrophone::dump_config() {
  ESP_LOGCONFIG(TAG, "ESP-NOW microphone:");
  ESP_LOGCONFIG(TAG, "  Format: %" PRIu32 " Hz, %u-bit, %u channel(s)",
                this->audio_stream_info_.get_sample_rate(),
                this->audio_stream_info_.get_bits_per_sample(),
                this->audio_stream_info_.get_channels());
}

}  // namespace comm_radio
}  // namespace esphome
