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
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#include "esp_bt.h"
#include "esp_nimble_hci.h"

namespace esphome {
namespace airbnk_gateway {

/* BLE Service and Characteristic UUIDs */
static constexpr uint16_t AIRBNK_SERVICE_UUID       = 0xFFF0;
static constexpr uint16_t AIRBNK_WRITE_CHAR_UUID    = 0xFFF2;
static constexpr uint16_t AIRBNK_READ_CHAR_UUID     = 0xFFF3;

/* BLE scan parameters */
static constexpr int32_t  AIRBNK_SCAN_DURATION_MS   = 0;        /* 0 = continuous */
static constexpr int32_t  AIRBNK_CONNECT_TIMEOUT_MS = 10000;
static constexpr uint16_t AIRBNK_CONN_ITVL          = 0x0018;
static constexpr uint16_t AIRBNK_CONN_LATENCY       = 0;
static constexpr uint16_t AIRBNK_CONN_SUPERVISION   = 0x0200;

/* Max retries for command sending */
static constexpr uint8_t  CMD_MAX_RETRIES           = 4;

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
 * AirbnkGateway - Dumb passthrough ESPHome component.
 *
 * NO manufacturer_key, NO binding_key, NO crypto.
 * Just forwards pre-computed command1/command2 from MQTT to BLE.
 */
class AirbnkGateway : public Component {
public:
    /* Configuration setters */
    void set_mac_address(const std::string &mac);
    void set_topic_prefix(const std::string &prefix);

    /* ESPHome Component lifecycle */
    void setup() override;
    void loop() override;
    float get_setup_priority() const override;

protected:
    /* MQTT */
    void subscribe_mqtt();
    void publish_advertisement(const std::string &mac, int rssi,
                               const std::string &data);
    void publish_command_result(bool success, const std::string &error,
                                const std::string &lock_status);
    void on_mqtt_command(const std::string &topic, const std::string &payload);

    /* BLE - NimBLE host API */
    void init_nimble();
    void start_scanning();
    void stop_scanning();

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

    /* Connection operations */
    void connect_to_lock(const ble_addr_t &addr);
    void discover_services(uint16_t conn_handle);
    void discover_characteristics(uint16_t conn_handle);
    void write_to_lock(const std::vector<uint8_t> &data);
    void read_from_lock();

    /* Send pre-computed command (the core of the pass-through) */
    void send_custom_command(const std::vector<uint8_t> &cmd1,
                             const std::vector<uint8_t> &cmd2);

    /* Helpers */
    static std::vector<uint8_t> hex_to_bytes(const std::string &hex);
    static std::string bytes_to_hex(const uint8_t *data, size_t len);
    static std::string bytes_to_hex(const std::vector<uint8_t> &data);
    static std::string mac_to_string(const uint8_t *addr);

    /* Configuration */
    std::string mac_address_;
    std::string topic_prefix_;

    /* MQTT Topics */
    std::string advert_topic_;
    std::string command_topic_;
    std::string command_result_topic_;

    /* BLE state */
    BleState state_{BleState::IDLE};
    ble_addr_t lock_addr_{};
    uint16_t conn_handle_{0};
    uint16_t svc_start_handle_{0};
    uint16_t svc_end_handle_{0};
    uint16_t write_chr_handle_{0};
    uint16_t read_chr_handle_{0};
    bool lock_found_{false};
    bool is_sending_{false};
    uint8_t send_retries_{0};
    bool ble_init_done_{false};

    /* Synchronisation semaphore for BLE operations */
    SemaphoreHandle_t sync_sem_{nullptr};

    /* Pending command data */
    std::vector<uint8_t> pending_cmd1_;
    std::vector<uint8_t> pending_cmd2_;

    static constexpr const char *TAG = "airbnk_gw";
};

}  // namespace airbnk_gateway
}  // namespace esphome
