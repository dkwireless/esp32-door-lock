/*
 * airbnk_ble.h — ESPHome component: Airbnk BLE-MQTT bridge
 *
 * Pure ESP-IDF NimBLE + MQTT. No entity headers needed on ESP-IDF.
 * All state published as MQTT JSON; commands received via MQTT subscribe.
 */
#pragma once

#include "esphome/core/component.h"
#include "airbnk_parser.h"
#include <string>
#include <cstdint>

namespace esphome {
namespace airbnk_ble {

class AirbnkBleComponent : public esphome::Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override {
    return esphome::setup_priority::AFTER_WIFI;
  }

  /* ── Public command interface ────────────────────────────────────────── */

  void trigger_unlock();
  void trigger_lock();

  /* ── Advert callback (from NimBLE) ───────────────────────────────────── */

  void on_advertisement(const airbnk_advert_data_t &adv);

  /* ── YAML setters ───────────────────────────────────────────────────── */

  void set_mac_address(const std::string &s);
  void set_manufacturer_key(const std::string &b64);
  void set_binding_key(const std::string &b64);
  void set_device_id(const std::string &s);
  void set_battery_v1(float v) { v1_ = v; }
  void set_battery_v2(float v) { v2_ = v; }
  void set_battery_v3(float v) { v3_ = v; }

  /* ── Accessors ──────────────────────────────────────────────────────── */

  const char *get_device_id() const { return devid_; }

 protected:
  static bool   b64decode_(const std::string &in, uint8_t *out, size_t n);
  static uint8_t batpct_(float v, float a, float b, float c);
  void pub_state_(const airbnk_advert_data_t &a);
  bool send_cmd_(uint8_t dir, const airbnk_advert_data_t &a);
  void cmd_done_(bool ok);

  uint8_t  mac_[6] = {0};
  uint8_t  mfkey_[16] = {0};
  uint8_t  bdkey_[20] = {0};
  char     devid_[16] = {0};
  bool     keys_ok_ = false;

  float v1_{3.5f}, v2_{3.8f}, v3_{4.1f};

  airbnk_advert_data_t last_;
  uint32_t last_ms_{0};
  bool     busy_{false};
  uint32_t cmd_events_{0};
};

}  // namespace airbnk_ble
}  // namespace esphome
