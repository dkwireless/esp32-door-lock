#pragma once

#ifndef MBEDTLS_CONFIG_FILE
#define MBEDTLS_CONFIG_FILE "mbedtls/esp_config.h"
#endif

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/mqtt/mqtt_client.h"

#include <cstdint>
#include <string>
#include <vector>

#include "mbedtls/aes.h"
#include "mbedtls/md.h"

namespace esphome::airbnk_gateway {

namespace espbt = esphome::esp32_ble_tracker;

static constexpr uint16_t AIRBNK_SERVICE_UUID       = 0xFFF0;
static constexpr uint16_t AIRBNK_WRITE_CHAR_UUID    = 0xFFF2;
static constexpr uint16_t AIRBNK_READ_CHAR_UUID     = 0xFFF3;

static constexpr size_t   AIRBNK_CMD_PACKET_SIZE    = 36;
static constexpr uint8_t  AIRBNK_HEADER[3]          = {0xAA, 0x10, 0x1A};
static constexpr uint8_t  AIRBNK_LOCK_OP_LOCK       = 0x01;
static constexpr uint8_t  AIRBNK_LOCK_OP_UNLOCK     = 0x02;

/**
 * AirbnkGateway - ESPHome Component bridging Airbnk smart locks
 * between BLE (via esp32_ble_client) and MQTT.
 *
 * Designed for ESP-IDF framework: no Arduino.h, no String.
 * Uses mbedtls for crypto (AES-ECB + HMAC-SHA1).
 */
class AirbnkGateway : public Component, public ble_client::BLEClientNode {
 public:
  /* ---- Configuration setters (called from Python codegen) ---- */
  void set_mac_address(const std::string &mac);
  void set_topic_prefix(const std::string &prefix);
  void set_manufacturer_key(const std::string &key);
  void set_binding_key(const std::string &key);

  /* ---- ESPHome Component lifecycle ---- */
  void setup() override;
  void loop() override;
  float get_setup_priority() const override;

  /* ---- BLEClientNode event handlers ---- */
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event,
                         esp_ble_gap_cb_param_t *param) override;

 protected:
  /* ---- Internal state machine ---- */
  enum class GatewayState : uint8_t {
    IDLE,
    WAITING_FOR_LOCK,
    CONNECTING,
    DISCOVERING_SERVICE,
    DISCOVERING_CHARS,
    WRITING,
    READING,
  };

  /* ---- MQTT ---- */
  void subscribe_mqtt();
  void publish_advertisement(const std::string &mac, int rssi, const std::string &data);
  void publish_command_result(bool success, const std::string &status);
  bool on_mqtt_command(const std::string &topic, const std::string &payload);

  /* ---- Airbnk command generation ---- */
  bool has_keys() const;
  void generate_command_packet(uint8_t lock_op,
                               std::vector<uint8_t> &cmd1,
                               std::vector<uint8_t> &cmd2);
  void send_lock_command();
  void send_unlock_command();

  /* ---- BLE operations (via esp_gattc) ---- */
  void discover_services();
  void discover_characteristics();
  void write_to_lock(const std::vector<uint8_t> &data);
  void read_from_lock();

  /* ---- Helpers ---- */
  static std::vector<uint8_t> hex_to_bytes(const std::string &hex);
  static std::string bytes_to_hex(const uint8_t *data, size_t len);
  static std::string bytes_to_hex(const std::vector<uint8_t> &data);
  static std::string mac_to_string(const uint8_t *addr);
  static bool aes_ecb_encrypt(const std::vector<uint8_t> &key,
                              const std::vector<uint8_t> &plaintext,
                              std::vector<uint8_t> &ciphertext);
  static bool hmac_sha1(const std::vector<uint8_t> &key,
                         const uint8_t *data, size_t data_len,
                         std::vector<uint8_t> &output);
  static uint8_t calculate_checksum(const uint8_t *data, size_t len);

  /* ---- Configuration ---- */
  std::string mac_address_;
  std::string topic_prefix_;
  std::string manufacturer_key_;
  std::string binding_key_;

  /* ---- MQTT Topics ---- */
  std::string advert_topic_;
  std::string command_topic_;
  std::string command_result_topic_;

  /* ---- BLE state ---- */
  GatewayState gw_state_{GatewayState::IDLE};
  uint16_t svc_start_handle_{0};
  uint16_t svc_end_handle_{0};
  uint16_t write_char_handle_{0};
  uint16_t read_char_handle_{0};
  bool lock_found_{false};
  bool mqtt_subscribed_{false};

  /* ---- Pending command ---- */
  std::vector<uint8_t> pending_write_data_;

  static constexpr const char *TAG = "airbnk_gw";
};

}  // namespace esphome::airbnk_gateway
