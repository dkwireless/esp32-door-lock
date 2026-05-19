#include "airbnk_gateway.h"
#include "esphome/core/log.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include <cstring>
#include <algorithm>
#include <cstdio>

#include "esp_bt_defs.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"

namespace esphome::airbnk_gateway {

static const char *const TAG = "airbnk_gw";

namespace espbt = esphome::esp32_ble_tracker;

/* ================================================================== */
/*  Configuration Setters                                              */
/* ================================================================== */

void AirbnkGateway::set_mac_address(const std::string &mac) {
  mac_address_ = mac;
  std::transform(mac_address_.begin(), mac_address_.end(),
                 mac_address_.begin(), ::toupper);
}

void AirbnkGateway::set_topic_prefix(const std::string &prefix) {
  topic_prefix_ = prefix;
  advert_topic_         = prefix + "/adv";
  command_topic_        = prefix + "/command";
  command_result_topic_  = prefix + "/command_result";
}

void AirbnkGateway::set_manufacturer_key(const std::string &key) {
  manufacturer_key_ = key;
}

void AirbnkGateway::set_binding_key(const std::string &key) {
  binding_key_ = key;
}

/* ================================================================== */
/*  ESPHome Component Lifecycle                                        */
/* ================================================================== */

void AirbnkGateway::setup() {
  ESP_LOGI(TAG, "Setting up Airbnk Gateway for MAC: %s", mac_address_.c_str());
  subscribe_mqtt();

  if (this->parent() != nullptr) {
    this->parent()->register_ble_node(this);
    ESP_LOGI(TAG, "Registered BLE node with client");
  } else {
    ESP_LOGE(TAG, "No parent ble_client set!");
  }
}

void AirbnkGateway::loop() {
  if (!mqtt_subscribed_) {
    subscribe_mqtt();
  }
}

float AirbnkGateway::get_setup_priority() const {
  return setup_priority::AFTER_CONNECTION;
}

/* ================================================================== */
/*  MQTT                                                                */
/* ================================================================== */

void AirbnkGateway::subscribe_mqtt() {
  if (mqtt_subscribed_) return;
  auto *mqtt = mqtt::global_mqtt_client;
  if (mqtt == nullptr) return;

  mqtt->subscribe(
      command_topic_,
      [this](const std::string &topic, const std::string &payload) {
        this->on_mqtt_command(topic, payload);
      },
      1);

  mqtt_subscribed_ = true;
  ESP_LOGI(TAG, "Subscribed to MQTT topic: %s", command_topic_.c_str());
  mqtt->publish(topic_prefix_ + "/LWT", "Online", 1, true);
}

void AirbnkGateway::publish_advertisement(const std::string &mac, int rssi,
                                           const std::string &data) {
  auto *mqtt = mqtt::global_mqtt_client;
  if (mqtt == nullptr) return;
  char buf[512];
  int n = snprintf(buf, sizeof(buf),
      R"({"mac":"%s","rssi":%d,"data":"%s"})",
      mac.c_str(), rssi, data.c_str());
  if (n > 0) mqtt->publish(advert_topic_, std::string(buf, n), 1, false);
}

void AirbnkGateway::publish_command_result(bool success,
                                            const std::string &status) {
  auto *mqtt = mqtt::global_mqtt_client;
  if (mqtt == nullptr) return;
  char buf[256];
  int n = snprintf(buf, sizeof(buf),
      R"({"success":%s,"lockStatus":"%s"})",
      success ? "true" : "false", status.c_str());
  if (n > 0) mqtt->publish(command_result_topic_, std::string(buf, n), 1, false);
}

bool AirbnkGateway::on_mqtt_command(const std::string &topic,
                                     const std::string &payload) {
  ESP_LOGI(TAG, "MQTT command received: %s", payload.c_str());

  // Simple keyword matching for lock/unlock commands
  if (payload.find("unlock") != std::string::npos ||
      payload.find("open") != std::string::npos) {
    send_unlock_command();
    return true;
  }
  if (payload.find("lock") != std::string::npos ||
      payload.find("close") != std::string::npos) {
    send_lock_command();
    return true;
  }

  // Try JSON format: {"sign": N, "command1": "HEX", "command2": "HEX"}
  // Format from rospogrigio/airbnk_mqtt
  ESP_LOGW(TAG, "Unknown command payload: %s", payload.c_str());
  return false;
}

/* ================================================================== */
/*  BLEClientNode Event Handlers                                       */
/* ================================================================== */

void AirbnkGateway::gap_event_handler(esp_gap_ble_cb_event_t event,
                                       esp_ble_gap_cb_param_t *param) {
  // Not used - scanning is handled by the parent ble_client
}

void AirbnkGateway::gattc_event_handler(esp_gattc_cb_event_t event,
                                         esp_gatt_if_t gattc_if,
                                         esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status != ESP_GATT_OK) {
        ESP_LOGE(TAG, "BLE connect failed: status %d", param->open.status);
        gw_state_ = GatewayState::IDLE;
        publish_command_result(false, "connect_failed");
        return;
      }
      ESP_LOGI(TAG, "BLE connected to lock");
      gw_state_ = GatewayState::DISCOVERING_SERVICE;
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      // Service discovery complete - find characteristics
      auto svc_uuid = espbt::ESPBTUUID::from_uint16(AIRBNK_SERVICE_UUID);
      auto write_uuid = espbt::ESPBTUUID::from_uint16(AIRBNK_WRITE_CHAR_UUID);
      auto read_uuid = espbt::ESPBTUUID::from_uint16(AIRBNK_READ_CHAR_UUID);

      auto *chr_write = this->parent()->get_characteristic(svc_uuid, write_uuid);
      auto *chr_read = this->parent()->get_characteristic(svc_uuid, read_uuid);

      if (chr_write == nullptr || chr_read == nullptr) {
        ESP_LOGE(TAG, "Airbnk characteristics not found on device");
        gw_state_ = GatewayState::IDLE;
        publish_command_result(false, "chars_not_found");
        return;
      }

      write_char_handle_ = chr_write->handle;
      read_char_handle_ = chr_read->handle;

      ESP_LOGI(TAG, "Found Airbnk chars - write: 0x%04X, read: 0x%04X",
               write_char_handle_, read_char_handle_);

      gw_state_ = GatewayState::DISCOVERING_CHARS;
      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "Write to lock successful");
        gw_state_ = GatewayState::READING;
        // Read the status back
        read_from_lock();
      } else {
        ESP_LOGE(TAG, "Write failed: status %d", param->write.status);
        gw_state_ = GatewayState::IDLE;
        publish_command_result(false, "write_failed");
      }
      break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.handle == read_char_handle_) {
        if (param->read.status == ESP_GATT_OK) {
          std::string value = bytes_to_hex(param->read.value, param->read.value_len);
          ESP_LOGI(TAG, "Read from lock: %s (%d bytes)", value.c_str(), param->read.value_len);
          gw_state_ = GatewayState::IDLE;
          publish_command_result(true, "ok");
        } else {
          ESP_LOGW(TAG, "Read failed: status %d", param->read.status);
          gw_state_ = GatewayState::IDLE;
          publish_command_result(true, "ok_no_status");
        }
      }
      break;
    }

    case ESP_GATTC_CLOSE_EVT:
    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGI(TAG, "BLE disconnected from lock");
      gw_state_ = GatewayState::IDLE;
      publish_command_result(false, "disconnected");
      break;
    }

    default:
      break;
  }
}

/* ================================================================== */
/*  BLE Operations                                                     */
/* ================================================================== */

void AirbnkGateway::write_to_lock(const std::vector<uint8_t> &data) {
  if (this->parent() == nullptr || write_char_handle_ == 0) return;

  auto status = esp_ble_gattc_write_char(
      this->parent()->get_gattc_if(),
      this->parent()->get_conn_id(),
      write_char_handle_,
      data.size(),
      (uint8_t *)data.data(),
      ESP_GATT_WRITE_TYPE_RSP,
      ESP_GATT_AUTH_REQ_NONE);

  if (status == ESP_GATT_OK) {
    ESP_LOGI(TAG, "Wrote %zu bytes to lock char 0x%04X", data.size(), write_char_handle_);
  } else {
    ESP_LOGE(TAG, "Write to lock failed: status %d", status);
  }
}

void AirbnkGateway::read_from_lock() {
  if (this->parent() == nullptr || read_char_handle_ == 0) return;

  auto status = esp_ble_gattc_read_char(
      this->parent()->get_gattc_if(),
      this->parent()->get_conn_id(),
      read_char_handle_,
      ESP_GATT_AUTH_REQ_NONE);

  if (status == ESP_GATT_OK) {
    ESP_LOGI(TAG, "Reading from lock char 0x%04X", read_char_handle_);
  } else {
    ESP_LOGE(TAG, "Read from lock failed: status %d", status);
  }
}

/* ================================================================== */
/*  Airbnk Command Generation                                          */
/* ================================================================== */

bool AirbnkGateway::has_keys() const {
  return !manufacturer_key_.empty() && !binding_key_.empty();
}

void AirbnkGateway::generate_command_packet(uint8_t lock_op,
                                             std::vector<uint8_t> &cmd1,
                                             std::vector<uint8_t> &cmd2) {
  cmd1.clear();
  cmd2.clear();

  if (manufacturer_key_.empty() || binding_key_.empty()) {
    // Without keys, send simple command (may not work on encrypted locks)
    cmd1 = {0xAA, 0x10, 0x1A, lock_op, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, lock_op};
    ESP_LOGW(TAG, "No crypto keys configured - sending unencrypted command");
    return;
  }

  // Parse keys from hex
  auto manuf_key = hex_to_bytes(manufacturer_key_);
  auto bind_key = hex_to_bytes(binding_key_);

  if (manuf_key.size() != 16 || bind_key.size() != 16) {
    ESP_LOGE(TAG, "Invalid key sizes: manuf=%zu, bind=%zu (expected 16 each)",
             manuf_key.size(), bind_key.size());
    return;
  }

  // Build 36-byte command packet
  std::vector<uint8_t> packet(AIRBNK_CMD_PACKET_SIZE, 0);
  std::memcpy(packet.data(), AIRBNK_HEADER, 3);
  packet[3] = lock_op;

  // Fill padding bytes 4-15 with deterministic values
  for (int i = 4; i < 16; i++) packet[i] = (uint8_t)(i + lock_op);

  // Encrypt first 16-byte block (AES-ECB with manufacturer key)
  std::vector<uint8_t> block1(packet.begin(), packet.begin() + 16);
  std::vector<uint8_t> encrypted1;
  if (!aes_ecb_encrypt(manuf_key, block1, encrypted1)) {
    ESP_LOGE(TAG, "AES-ECB encryption failed for block 1");
    return;
  }

  // Second block: bytes 16-27 plaintext, 28-35 for HMAC
  std::vector<uint8_t> block2(16, 0);
  std::memcpy(block2.data(), packet.data() + 16, 12);

  std::vector<uint8_t> encrypted2;
  if (!aes_ecb_encrypt(manuf_key, block2, encrypted2)) {
    ESP_LOGE(TAG, "AES-ECB encryption failed for block 2");
    return;
  }

  // HMAC-SHA1 over both encrypted blocks
  std::vector<uint8_t> hmac_input;
  hmac_input.insert(hmac_input.end(), encrypted1.begin(), encrypted1.end());
  hmac_input.insert(hmac_input.end(), encrypted2.begin(), encrypted2.end());

  std::vector<uint8_t> hmac_result;
  if (!hmac_sha1(bind_key, hmac_input.data(), hmac_input.size(), hmac_result)) {
    ESP_LOGE(TAG, "HMAC-SHA1 failed");
    return;
  }

  // Build cmd1: header + encrypted block 1
  cmd1.assign(AIRBNK_HEADER, AIRBNK_HEADER + 3);
  cmd1.insert(cmd1.end(), encrypted1.begin(), encrypted1.end());

  // Build cmd2: header + encrypted block 2 + HMAC (8 bytes) + checksum
  cmd2.push_back(AIRBNK_HEADER[0]);  // 0xAA
  cmd2.push_back(AIRBNK_HEADER[1]);  // 0x10
  cmd2.push_back(AIRBNK_HEADER[2]);  // 0x1A
  cmd2.insert(cmd2.end(), encrypted2.begin(), encrypted2.end());
  cmd2.insert(cmd2.end(), hmac_result.begin(),
              hmac_result.begin() + std::min(hmac_result.size(), (size_t)8));
  cmd2.push_back(calculate_checksum(encrypted2.data(), encrypted2.size()));
}

void AirbnkGateway::send_lock_command() {
  ESP_LOGI(TAG, "Sending lock command");
  std::vector<uint8_t> cmd1, cmd2;
  generate_command_packet(AIRBNK_LOCK_OP_LOCK, cmd1, cmd2);

  if (!cmd1.empty() && write_char_handle_ != 0) {
    write_to_lock(cmd1);
  } else if (write_char_handle_ != 0) {
    std::vector<uint8_t> simple = {0xAA, 0x10, 0x1A, AIRBNK_LOCK_OP_LOCK};
    write_to_lock(simple);
  } else {
    ESP_LOGW(TAG, "Cannot lock: not connected");
    publish_command_result(false, "not_connected");
  }
}

void AirbnkGateway::send_unlock_command() {
  ESP_LOGI(TAG, "Sending unlock command");
  std::vector<uint8_t> cmd1, cmd2;
  generate_command_packet(AIRBNK_LOCK_OP_UNLOCK, cmd1, cmd2);

  if (!cmd1.empty() && write_char_handle_ != 0) {
    write_to_lock(cmd1);
  } else if (write_char_handle_ != 0) {
    std::vector<uint8_t> simple = {0xAA, 0x10, 0x1A, AIRBNK_LOCK_OP_UNLOCK};
    write_to_lock(simple);
  } else {
    ESP_LOGW(TAG, "Cannot unlock: not connected");
    publish_command_result(false, "not_connected");
  }
}

/* ================================================================== */
/*  Helpers & Crypto (mbedtls)                                         */
/* ================================================================== */

std::vector<uint8_t> AirbnkGateway::hex_to_bytes(const std::string &hex) {
  std::vector<uint8_t> bytes;
  std::string h;
  for (char c : hex) {
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
      h += static_cast<char>(::toupper(c));
  }
  for (size_t i = 0; i + 1 < h.size(); i += 2) {
    unsigned int byte;
    std::sscanf(h.c_str() + i, "%2x", &byte);
    bytes.push_back((uint8_t)byte);
  }
  return bytes;
}

std::string AirbnkGateway::bytes_to_hex(const uint8_t *data, size_t len) {
  std::string hex;
  hex.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02X", data[i]);
    hex += buf;
  }
  return hex;
}

std::string AirbnkGateway::bytes_to_hex(const std::vector<uint8_t> &data) {
  return bytes_to_hex(data.data(), data.size());
}

std::string AirbnkGateway::mac_to_string(const uint8_t *addr) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
  return std::string(buf);
}

bool AirbnkGateway::aes_ecb_encrypt(const std::vector<uint8_t> &key,
                                     const std::vector<uint8_t> &plaintext,
                                     std::vector<uint8_t> &ciphertext) {
  if (key.size() != 16 || plaintext.size() != 16) return false;

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);

  int ret = mbedtls_aes_setkey_enc(&aes, key.data(), 128);
  if (ret != 0) { mbedtls_aes_free(&aes); return false; }

  ciphertext.resize(16);
  ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                               plaintext.data(), ciphertext.data());
  mbedtls_aes_free(&aes);
  return ret == 0;
}

bool AirbnkGateway::hmac_sha1(const std::vector<uint8_t> &key,
                               const uint8_t *data, size_t data_len,
                               std::vector<uint8_t> &output) {
  output.resize(20);
  const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
  if (md_info == nullptr) return false;

  return mbedtls_md_hmac(md_info, key.data(), key.size(),
                          data, data_len, output.data()) == 0;
}

uint8_t AirbnkGateway::calculate_checksum(const uint8_t *data, size_t len) {
  uint8_t sum = 0;
  for (size_t i = 0; i < len; i++) sum ^= data[i];
  return sum;
}

}  // namespace esphome::airbnk_gateway
