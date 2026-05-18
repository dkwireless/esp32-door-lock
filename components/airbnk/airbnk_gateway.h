#pragma once

#include "esphome/core/component.h"
#include "esphome/components/mqtt/mqtt_client.h"

#include <cstdint>
#include <string>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* NimBLE host headers (ESP-IDF native, NOT NimBLEDevice Arduino wrapper) */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

/* mbedtls for AES-ECB and HMAC-SHA1 */
#include "mbedtls/aes.h"
#include "mbedtls/md.h"

namespace airbnk {

/* BLE Service and Characteristic UUIDs */
static constexpr uint16_t AIRBNK_SERVICE_UUID       = 0xFFF0;
static constexpr uint16_t AIRBNK_WRITE_CHAR_UUID    = 0xFFF2;
static constexpr uint16_t AIRBNK_READ_CHAR_UUID     = 0xFFF3;

/* Command packet constants */
static constexpr size_t   AIRBNK_CMD_PACKET_SIZE    = 36;
static constexpr uint8_t  AIRBNK_HEADER[3]          = {0xAA, 0x10, 0x1A};
static constexpr uint8_t  AIRBNK_AES_BLOCK_SIZE     = 16;
static constexpr uint8_t  AIRBNK_HMAC_KEY_SIZE      = 16;
static constexpr uint8_t  AIRBNK_LOCK_OP_LOCK       = 0x01;
static constexpr uint8_t  AIRBNK_LOCK_OP_UNLOCK     = 0x02;

/* BLE scan parameters */
static constexpr int32_t  AIRBNK_SCAN_DURATION_MS   = 0;       /* 0 = continuous */
static constexpr int32_t  AIRBNK_CONNECT_TIMEOUT_MS = 10000;
static constexpr uint16_t AIRBNK_CONN_ITVL          = 0x0018;  /* 30ms */
static constexpr uint16_t AIRBNK_CONN_LATENCY       = 0;
static constexpr uint16_t AIRBNK_CONN_SUPERVISION   = 0x0200;  /* 2s */

/* BLE operation states */
enum class BleState : uint8_t {
    IDLE,
    SCANNING,
    DISCOVERED,
    CONNECTING,
    CONNECTED_SERVICE_DISC,
    CONNECTED_CHAR_DISC,
    CONNECTED_WRITING,
    CONNECTED_READING,
    CONNECTED_DISCONNECTING,
    ERROR
};

/**
 * AirbnkGateway - ESPHome component that bridges Airbnk smart locks
 * between BLE and MQTT.
 *
 * Designed for ESP-IDF framework: no Arduino.h, no String, no delay().
 * Uses native ESP-IDF NimBLE host API and mbedtls for crypto.
 */
class AirbnkGateway : public Component {
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

    /* ---- Public API ---- */
    void send_lock_command();
    void send_unlock_command();
    void send_custom_command(const std::vector<uint8_t> &cmd1,
                             const std::vector<uint8_t> &cmd2);

protected:
    /* ---- MQTT ---- */
    void subscribe_mqtt();
    void publish_advertisement(const std::string &mac, int rssi,
                               const std::string &data);
    void publish_command_result(bool success, const std::string &error,
                                const std::string &lock_status);
    void on_mqtt_command(const std::string &topic, JsonObject root);

    /* BLE — NimBLE host API */
    void init_nimble();
    void start_scanning();
    void stop_scanning();
    void try_start_scan();
    bool is_scanning() const { return state_ == BleState::SCANNING; }
    bool is_ble_ready() const;

    /* NimBLE GAP event callback (static, dispatches to instance) */
    static int gap_event_cb(struct ble_gap_event *event, void *arg);
    int handle_gap_event(struct ble_gap_event *event);

    /* NimBLE GATT callbacks (static) */
    static int gatt_svc_disc_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_svc *service,
                                void *arg);
    static int gatt_chr_disc_cb(uint16_t conn_handle,
                                const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr,
                                void *arg);
    static int gatt_write_cb(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg);
    static int gatt_read_cb(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            struct ble_gatt_attr *attr,
                            void *arg);

    /* Connection / characteristic handles */
    void connect_to_lock(const ble_addr_t &addr);
    void discover_services(uint16_t conn_handle);
    void discover_characteristics(uint16_t conn_handle);
    void write_to_lock(const std::vector<uint8_t> &data);
    void read_from_lock();

    /* ---- Airbnk command generation ---- */
    bool has_keys() const;
    void generate_command_packet(uint8_t lock_op,
                                 std::vector<uint8_t> &cmd1,
                                 std::vector<uint8_t> &cmd2);

    static std::vector<uint8_t> hex_to_bytes(const std::string &hex);
    static std::string bytes_to_hex(const uint8_t *data, size_t len);
    static std::string bytes_to_hex(const std::vector<uint8_t> &data);
    static std::string mac_to_string(const uint8_t *addr);

    /* Crypto helpers (mbedtls) */
    static bool aes_ecb_encrypt(const std::vector<uint8_t> &key,
                                const std::vector<uint8_t> &plaintext,
                                std::vector<uint8_t> &ciphertext);
    static bool hmac_sha1(const std::vector<uint8_t> &key,
                          const uint8_t *data, size_t data_len,
                          std::vector<uint8_t> &output);
    static uint8_t calculate_checksum(const uint8_t *data, size_t len);

    /* ---- Configuration ---- */
    std::string mac_address_;          /* Target lock MAC (colon-separated) */
    std::string topic_prefix_;         /* MQTT topic prefix */
    std::string manufacturer_key_;     /* AES key (hex string, 32 chars) */
    std::string binding_key_;          /* HMAC key (hex string, 32 chars) */

    /* MQTT Topics */
    std::string advert_topic_;
    std::string command_topic_;
    std::string command_result_topic_;

    /* BLE state */
    BleState state_{BleState::IDLE};
    ble_addr_t lock_addr_{};
    uint16_t conn_handle_{0};
    uint16_t svc_handle_{0};
    uint16_t write_chr_handle_{0};
    uint16_t read_chr_handle_{0};
    bool lock_found_{false};
    bool is_sending_{false};
    uint8_t send_retries_{0};
    bool ble_init_done_{false};

    /* Synchronisation semaphore for BLE operations */
    SemaphoreHandle_t sync_sem_{nullptr};

    /* Event data for passing context to NimBLE callbacks */
    struct EventContext {
        AirbnkGateway *self;
        std::vector<uint8_t> write_data;
        std::vector<uint8_t> read_result;
    };
    EventContext *event_ctx_{nullptr};

    /* Last advertisement data for re-publishing */
    std::string last_adv_data_;
    int last_adv_rssi_{0};

    /* MQTT subscription handle */
    mqtt::MQTTClient *mqtt_client_{nullptr};

    static constexpr const char *TAG = "airbnk";
};

}  /* namespace airbnk */
