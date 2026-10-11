#include "comm_radio.h"
#include "comm_ask.h"

#ifdef USE_ESP32

#include "esphome/components/wifi/wifi_component.h"
#include "esp_wifi.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

extern "C" {
#include "applog.h"
#include "espnow.h"
#include "fleet.h"
void gw_shim_set_channel(int ch);
void gw_shim_set_name(const char *n);
}

namespace esphome {
namespace comm_radio {

static const char *const TAG = "comm_radio";

void CommRadio::setup() {
  gw_shim_set_channel(this->channel_);
  gw_shim_set_name(this->name_);
  applog_init();
  fleet_init();

  /* 🔑 DERIVE ONCE, EVER. The PMK is stored in NVS by fleet_set_code(), so a
   * board that has one must NOT pay the derivation again — and re-deriving on
   * every boot is what turned a 20-second first boot into a permanent reboot
   * loop. Same code always gives the same key, so skipping is safe. */
  if (fleet_has_pmk()) {
    ESP_LOGI(TAG, "fleet PMK already stored, not re-deriving");
    this->pmk_ready_ = true;
  } else if (this->fleet_code_[0]) {
    ESP_LOGI(TAG, "no stored PMK - deriving one from the fleet code on its own "
                  "task (100k PBKDF2 iterations, tens of seconds on this chip)");
    xTaskCreatePinnedToCore(&CommRadio::derive_task_, "fleet_kdf", 8192, this, 5,
                            nullptr, 0);
  } else {
    ESP_LOGW(TAG,
             "NO FLEET CODE AND NO STORED PMK. Beacons will still be heard, but "
             "this gateway cannot pair with anything. Set fleet_code: in the YAML.");
    this->pmk_ready_ = true;   /* beacon-only is a valid, if useless, state */
  }

  ESP_LOGCONFIG(TAG, "waiting for the station to associate before starting ESP-NOW");
}

/* Runs on its own task, pinned to the core ESPHome's loop is NOT on, so the
 * loop task keeps feeding its watchdog throughout. The remaining exposure is
 * the IDLE task on this core starving, which the watchdog also watches — hence
 * the timeout is relaxed for the duration and put back afterwards rather than
 * being left loose, which would weaken it for everything else forever. */
void CommRadio::derive_task_(void *arg) {
  auto *self = static_cast<CommRadio *>(arg);

  const uint32_t idle_mask =
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
      (1 << 0) |
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
      (1 << 1) |
#endif
      0;
  esp_task_wdt_config_t relaxed = {
      .timeout_ms = 120000, .idle_core_mask = idle_mask, .trigger_panic = true};
  esp_task_wdt_reconfigure(&relaxed);

  const int64_t t0 = esp_timer_get_time();
  fleet_set_code(self->fleet_code_);
  const int64_t ms = (esp_timer_get_time() - t0) / 1000;

  esp_task_wdt_config_t normal = {.timeout_ms = CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000,
                                  .idle_core_mask = idle_mask,
                                  .trigger_panic = true};
  esp_task_wdt_reconfigure(&normal);

  if (fleet_has_pmk())
    ESP_LOGI(TAG, "fleet PMK derived and stored in %lldms", (long long) ms);
  else
    ESP_LOGE(TAG, "fleet code REJECTED after %lldms - this gateway cannot pair",
             (long long) ms);

  self->pmk_ready_ = true;
  vTaskDelete(nullptr);
}

bool CommRadio::start_radio_() {
  /* 🚨 WHAT CHANNEL DID WE ACTUALLY LAND ON? Ask the radio, never assume. The
   * fleet is deaf to a gateway on the wrong channel and nothing anywhere
   * reports an error — the same class of silent fault as a mismatched PMK. */
  uint8_t actual = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&actual, &second);

  if (actual != this->channel_) {
    ESP_LOGE(TAG,
             "THE AP PUT US ON CHANNEL %u, THE FLEET IS ON %u. Every handset is "
             "deaf to this gateway. Pin the AP to channel %u (20 MHz, not auto).",
             actual, this->channel_, this->channel_);
    this->status_set_error(LOG_STR("ESP-NOW is on the wrong channel"));
  } else {
    ESP_LOGI(TAG, "associated on channel %u, which is the fleet's channel", actual);
    this->status_clear_error();
  }

  /* Tell espnow.c the truth about where the radio is, so it does not try to
   * move a station that cannot be moved. */
  gw_shim_set_channel(actual);

  if (!espnow_begin()) {
    ESP_LOGE(TAG, "espnow_begin() FAILED - nothing can reach this gateway");
    this->mark_failed();
    return false;
  }

  char gwname[16];
  snprintf(gwname, sizeof(gwname), "GW-%04X", (unsigned) espnow_self_id());
  espnow_set_name(gwname);

  /* The role is a runtime call, not an #ifdef — the same espnow.c compiles for
   * handsets and for this. */
  espnow_set_gateway(true);

  /* Only after espnow_begin() has succeeded: espnow_gw_on_request() on a radio
   * that never started would arm a callback nothing can ever call, and the
   * silence would look identical to a handset out of range. */
  global_ask_relay.setup(this->mic_);

  ESP_LOGI(TAG, "up as %s (%04X), %d peer(s) known", gwname,
           (unsigned) espnow_self_id(), espnow_peer_count());
  applog("[gw] up as %04X under ESPHome", (unsigned) espnow_self_id());
  return true;
}

void CommRadio::loop() {
  if (!this->started_) {
    if (wifi::global_wifi_component == nullptr || !wifi::global_wifi_component->is_connected())
      return;
    /* Don't bring the radio up mid-derivation: a pairing attempt that landed in
     * the gap would fail for a reason no log would explain. */
    if (!this->pmk_ready_)
      return;
    this->started_ = true;  /* set first: a failed start must not retry forever */
    this->start_radio_();
    return;
  }

  /* ---- Gate 3.1 self-test ------------------------------------------------
   * Runs once, after the radio is up, and only when asked for in the YAML.
   * 🔑 IT PUSHES SILENCE ON PURPOSE. A recognisable tone would tempt someone
   * to judge this by listening; the only thing being proved here is that bytes
   * reach a registered callback with the right length. Audio quality is not a
   * question Gate 3.1 can answer. */
  if (this->self_test_ && !this->self_tested_) {
    this->self_tested_ = true;
    if (this->mic_ == nullptr) {
      ESP_LOGE(TAG, "self test: no microphone configured");
    } else {
      static const uint8_t SILENCE[320] = {0};   /* 10ms at 16kHz 16-bit mono */
      this->mic_->start();
      const bool ok = this->mic_->push(SILENCE, sizeof(SILENCE));
      const uint32_t got = this->mic_->pushed_bytes();
      this->mic_->stop();
      if (ok && got == sizeof(SILENCE))
        ESP_LOGI(TAG, "self test PASSED: %u bytes reached the microphone",
                 (unsigned) got);
      else
        ESP_LOGE(TAG, "self test FAILED: push returned %s, %u bytes accounted for",
                 ok ? "true" : "false", (unsigned) got);
    }
  }

  /* The relay raises flags from its worker task; turning them into calls has
   * to happen here, on the loop. */
  global_ask_relay.loop();

  const uint32_t now = millis();
  if (now - this->last_report_ < 10000) return;
  this->last_report_ = now;

  uint8_t actual = 0;
  wifi_second_chan_t second;
  esp_wifi_get_channel(&actual, &second);
  ESP_LOGD(TAG, "channel %u, %d peer(s), self %04X", actual, espnow_peer_count(),
           (unsigned) espnow_self_id());
}

void CommRadio::ask_heard(const std::string &text) { global_ask_relay.heard(text); }
void CommRadio::ask_replied(const std::string &text) { global_ask_relay.replied(text); }
void CommRadio::ask_finished() { global_ask_relay.finished(); }
void CommRadio::ask_failed(const std::string &code, const std::string &message) {
  global_ask_relay.failed(code, message);
}

void CommRadio::dump_config() {
  ESP_LOGCONFIG(TAG, "Communicator radio:");
  ESP_LOGCONFIG(TAG, "  Fleet channel: %u", this->channel_);
  ESP_LOGCONFIG(TAG, "  Name: %s", this->name_);
  ESP_LOGCONFIG(TAG, "  Running: %s", espnow_running() ? "yes" : "no");
}

}  // namespace comm_radio
}  // namespace esphome

#endif  // USE_ESP32
