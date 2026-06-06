#include "airbnk_gateway.h"
#include "esphome/core/log.h"
#include "esphome/components/network/util.h"

#include <ArduinoJson.h>

#include <cstring>
#include <algorithm>
#include <cstdio>

/* NimBLE host headers (ESP-IDF native) */
extern "C" {
#include "esp_bt.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
}

namespace esphome {
namespace airbnk_gateway {

static const char *const TAG = "airbnk_gw";

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
    command_result_topic_ = prefix + "/command_result";
}

/* ================================================================== */
/*  ESPHome Component Lifecycle                                        */
/* ================================================================== */

void AirbnkGateway::setup() {
    ESP_LOGI(TAG, "AirbnkGateway (dumb passthrough) starting up");
    ESP_LOGI(TAG, "  Target MAC: %s", mac_address_.c_str());
    ESP_LOGI(TAG, "  Topic prefix: %s", topic_prefix_.c_str());

    /* Create synchronisation semaphore (binary, initially taken) */
    sync_sem_ = xSemaphoreCreateBinary();
    if (!sync_sem_) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        this->mark_failed();
        return;
    }

    /* Subscribe to MQTT command topic */
    subscribe_mqtt();

    /* Defer NimBLE + BLE scan to loop() — WiFi must be up first */
    ble_init_scheduled_ = true;

    ESP_LOGI(TAG, "AirbnkGateway setup complete (BLE deferred)");
}

void AirbnkGateway::loop() {
    /* Deferred BLE init: wait for WiFi before touching BT controller */
    if (ble_init_scheduled_ && !ble_init_done_) {
        if (network::is_connected()) {
            ESP_LOGI(TAG, "WiFi connected, initializing NimBLE now");
            init_nimble();
            if (ble_init_done_) {
                start_scanning();
            }
        } else {
            static uint32_t last_warn = 0;
            if (millis() - last_warn > 10000) {
                ESP_LOGD(TAG, "Waiting for WiFi before BLE init...");
                last_warn = millis();
            }
        }
    }
}

float AirbnkGateway::get_setup_priority() const {
    return setup_priority::AFTER_WIFI;
}

/* ================================================================== */
/*  MQTT                                                               */
/* ================================================================== */

void AirbnkGateway::subscribe_mqtt() {
    auto *mqtt = mqtt::global_mqtt_client;
    if (!mqtt) {
        ESP_LOGE(TAG, "MQTT client not available!");
        return;
    }

    ESP_LOGI(TAG, "Subscribing to MQTT: %s", command_topic_.c_str());
    mqtt->subscribe(
        command_topic_,
        [this](const std::string &topic, const std::string &payload) {
            this->on_mqtt_command(topic, payload);
        },
        1);
}

void AirbnkGateway::publish_advertisement(const std::string &mac,
                                           int rssi,
                                           const std::string &data) {
    auto *mqtt = mqtt::global_mqtt_client;
    if (!mqtt) return;

    JsonDocument doc;
    doc["mac"]  = mac;
    doc["rssi"] = rssi;
    doc["data"] = data;
    std::string json_str;
    serializeJson(doc, json_str);
    mqtt->publish(advert_topic_, json_str, 1, false);
}

void AirbnkGateway::publish_command_result(bool success,
                                           const std::string &error,
                                           int sign,
                                           const std::string &lock_status) {
    auto *mqtt = mqtt::global_mqtt_client;
    if (!mqtt) return;

    JsonDocument doc;
    doc["success"]    = success;
    doc["error"]      = error;
    doc["sign"]       = sign;
    doc["mac"]        = mac_address_;
    doc["lockStatus"] = lock_status;
    std::string json_str;
    serializeJson(doc, json_str);
    mqtt->publish(command_result_topic_, json_str, 1, false);
}

void AirbnkGateway::on_mqtt_command(const std::string &topic,
                                     const std::string &payload) {
    if (is_sending_) {
        ESP_LOGW(TAG, "Already sending a command, ignoring");
        return;
    }

    /* Parse JSON: {"sign": N, "command1": "HEX...", "command2": "HEX..."} */
    /* Use ArduinoJson via ESPHome's helpers */
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        ESP_LOGW(TAG, "Failed to parse command JSON: %s", err.c_str());
        return;
    }

    const char *cmd1_str = doc["command1"];
    const char *cmd2_str = doc["command2"];

    if (!cmd1_str || !cmd2_str) {
        ESP_LOGW(TAG, "Command JSON missing command1/command2 fields");
        publish_command_result(false, "MISSING_COMMAND_FIELDS", 0, "");
        return;
    }

    int sign = doc["sign"] | 0;
    ESP_LOGI(TAG, "Received command: sign=%d, cmd1=%s, cmd2=%s", sign, cmd1_str, cmd2_str);

    std::vector<uint8_t> cmd1 = hex_to_bytes(cmd1_str);
    std::vector<uint8_t> cmd2 = hex_to_bytes(cmd2_str);

    if (cmd1.empty() || cmd2.empty()) {
        ESP_LOGW(TAG, "Empty command data after hex parsing");
        publish_command_result(false, "EMPTY_COMMAND_DATA", sign, "");
        return;
    }

    send_custom_command(cmd1, cmd2, sign);
}

/* ================================================================== */
/*  BLE - NimBLE initialisation                                        */
/* ================================================================== */

static void nimble_host_task(void *param) {
    ESP_LOGD(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void AirbnkGateway::init_nimble() {
    if (ble_init_done_) return;

    ESP_LOGI(TAG, "Initialising NimBLE host");

    /* nimble_port_init() handles BT controller + HCI + host internally */
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        this->mark_failed();
        return;
    }

    ble_svc_gap_init();

    rc = ble_svc_gap_device_name_set("AirbnkGateway");
    if (rc != 0) {
        ESP_LOGW(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
    }

    ble_hs_cfg.sync_cb = [](void) {
        ESP_LOGD("airbnk", "NimBLE synced with controller");
    };
    ble_hs_cfg.reset_cb = [](int reason) {
        ESP_LOGW("airbnk", "NimBLE host reset, reason: %d", reason);
    };

    nimble_port_freertos_init(nimble_host_task);

    ble_init_done_ = true;
    ESP_LOGI(TAG, "NimBLE host initialised");
}

/* ================================================================== */
/*  BLE - Scanning                                                     */
/* ================================================================== */

void AirbnkGateway::start_scanning() {
    if (state_ == BleState::SCANNING) return;

    ESP_LOGI(TAG, "Starting BLE scan for %s", mac_address_.c_str());

    /* Parse target MAC into ble_addr_t */
    std::string mac = mac_address_;
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());

    if (mac.length() != 12) {
        ESP_LOGE(TAG, "Invalid MAC address: %s", mac_address_.c_str());
        return;
    }

    for (size_t i = 0; i < 6; i++) {
        std::string byte_str = mac.substr(i * 2, 2);
        lock_addr_[5 - i] = strtol(byte_str.c_str(), nullptr, 16);
    }
    lock_addr_type_ = BLE_OWN_ADDR_PUBLIC;

    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.itvl              = 0x30;
    disc_params.window            = 0x30;
    disc_params.filter_policy     = 0;
    disc_params.limited           = 0;
    disc_params.passive           = 1;
    disc_params.filter_duplicates = 1;

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                          AIRBNK_SCAN_DURATION_MS,
                          &disc_params,
                          gap_event_cb,
                          this);
    if (rc == 0) {
        state_ = BleState::SCANNING;
        ESP_LOGI(TAG, "BLE scan started");
    } else {
        ESP_LOGW(TAG, "ble_gap_disc failed: %d (will retry)", rc);
        state_ = BleState::IDLE;
    }
}

void AirbnkGateway::stop_scanning() {
    if (state_ != BleState::SCANNING) return;

    int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel: %d", rc);
    }
    state_ = BleState::IDLE;
}

/* ================================================================== */
/*  BLE - GAP event handler                                            */
/* ================================================================== */

int AirbnkGateway::gap_event_cb(struct ble_gap_event *event, void *arg) {
    auto *self = static_cast<AirbnkGateway *>(arg);
    if (!self) return BLE_HS_EINVAL;
    return self->handle_gap_event(event);
}

int AirbnkGateway::handle_gap_event(struct ble_gap_event *event) {
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        const auto &disc = event->disc;

        /* Check if this is our lock */
        if (memcmp(disc.addr.val, lock_addr_, 6) != 0) {
            return 0;
        }

        ESP_LOGI(TAG, "Lock found! RSSI: %d", disc.rssi);

        /* Extract manufacturer data */
        std::string man_data;

        /* Debug: dump raw advertisement */
        std::string raw_hex = bytes_to_hex(disc.data, std::min<uint8_t>(disc.length_data, 64));
        ESP_LOGI(TAG, "Raw adv: %d bytes = %s", disc.length_data, raw_hex.c_str());

        /* Parse manufacturer data (AD type 0xFF) from raw advertisement */
        if (disc.length_data > 0 && disc.data != nullptr) {
            const uint8_t *p = disc.data;
            uint8_t remain = disc.length_data;
            while (remain >= 2) {
                uint8_t field_len = p[0];
                uint8_t field_type = p[1];
                if (field_len == 0) break;

                if (field_type == 0xFF && remain >= field_len + 1) {
                    if (field_len >= 4) {
                        man_data = bytes_to_hex(p + 4, field_len - 3);
                    }
                    break;
                }
                if (field_len + 1 > remain) break;
                remain -= (field_len + 1);
                p += (field_len + 1);
            }
        }

        /* Publish to MQTT */
        std::string mac_str = mac_to_string(disc.addr.val);
        ESP_LOGI(TAG, "Adv data: %s", man_data.c_str());
        publish_advertisement(mac_str, disc.rssi, man_data);

        /* If not currently sending, just report */
        if (!is_sending_) {
            lock_found_ = true;
            return 0;
        }

        /* We need to connect - stop scanning first */
        stop_scanning();
        memcpy(lock_addr_, disc.addr.val, 6);
        lock_addr_type_ = disc.addr.type;
        connect_to_lock(lock_addr_, lock_addr_type_);
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE: {
        ESP_LOGD(TAG, "Scan complete (reason: %d)", event->disc_complete.reason);
        if (state_ == BleState::SCANNING) {
            /* Restart scan for continuous scanning */
            struct ble_gap_disc_params disc_params;
            memset(&disc_params, 0, sizeof(disc_params));
            disc_params.itvl              = 0x30;
            disc_params.window            = 0x30;
            disc_params.filter_policy     = 0;
            disc_params.limited           = 0;
            disc_params.passive           = 1;
            disc_params.filter_duplicates = 1;

            int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                                  AIRBNK_SCAN_DURATION_MS,
                                  &disc_params,
                                  gap_event_cb,
                                  this);
            if (rc != 0) {
                ESP_LOGW(TAG, "Scan restart failed: %d", rc);
            }
        }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status == 0) {
            conn_handle_ = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected to lock, handle=%d", conn_handle_);
            state_ = BleState::CONNECTED_SERVICE_DISC;

            /* Discover the Airbnk service */
            discover_services(conn_handle_);

            if (sync_sem_) xSemaphoreGive(sync_sem_);
        } else {
            ESP_LOGE(TAG, "Connection failed: %d", event->connect.status);
            state_ = BleState::ERROR;
            if (sync_sem_) xSemaphoreGive(sync_sem_);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "Disconnected from lock, reason=%d",
                 event->disconnect.reason);
        conn_handle_ = 0;
        svc_start_handle_ = 0;
        svc_end_handle_ = 0;
        write_chr_handle_ = 0;
        read_chr_handle_ = 0;
        state_ = BleState::IDLE;

        if (is_sending_) {
            if (sync_sem_) xSemaphoreGive(sync_sem_);
        }

        /* Restart scanning */
        start_scanning();
        return 0;
    }

    default:
        return 0;
    }
}

/* ================================================================== */
/*  BLE - Connection                                                   */
/* ================================================================== */

void AirbnkGateway::connect_to_lock(const uint8_t *addr, uint8_t addr_type) {
    ESP_LOGI(TAG, "Connecting to lock...");
    state_ = BleState::CONNECTING;

    ble_addr_t ble_addr;
    memcpy(ble_addr.val, addr, 6);
    ble_addr.type = addr_type;

    struct ble_gap_conn_params conn_params;
    memset(&conn_params, 0, sizeof(conn_params));
    conn_params.scan_itvl            = 0x0030;
    conn_params.scan_window          = 0x0030;
    conn_params.itvl_min             = AIRBNK_CONN_ITVL;
    conn_params.itvl_max             = AIRBNK_CONN_ITVL;
    conn_params.latency              = AIRBNK_CONN_LATENCY;
    conn_params.supervision_timeout  = AIRBNK_CONN_SUPERVISION;
    conn_params.min_ce_len           = 16;
    conn_params.max_ce_len           = 32;

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &ble_addr,
                             AIRBNK_CONNECT_TIMEOUT_MS,
                             &conn_params,
                             gap_event_cb,
                             this);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
        state_ = BleState::ERROR;
        if (sync_sem_) xSemaphoreGive(sync_sem_);
    }
}

/* ================================================================== */
/*  BLE - Service discovery                                            */
/* ================================================================== */

void AirbnkGateway::discover_services(uint16_t conn_handle) {
    ble_uuid16_t svc_uuid;
    svc_uuid.u.type = BLE_UUID_TYPE_16;
    svc_uuid.value  = AIRBNK_SERVICE_UUID;

    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &svc_uuid.u,
                                         gatt_svc_disc_cb, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_svc_by_uuid failed: %d", rc);
        state_ = BleState::ERROR;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int AirbnkGateway::gatt_svc_disc_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_svc *service,
                                     void *arg) {
    auto *self = static_cast<AirbnkGateway *>(arg);
    if (!self) return BLE_HS_EINVAL;

    if (error->status == 0 && service != nullptr) {
        self->svc_start_handle_ = service->start_handle;
        self->svc_end_handle_ = service->end_handle;
        ESP_LOGI(TAG, "Service found: start=%d end=%d",
                 service->start_handle, service->end_handle);
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (self->svc_start_handle_ == 0) {
            ESP_LOGE(TAG, "Airbnk service 0xFFF0 not found!");
            self->state_ = BleState::ERROR;
            if (self->is_sending_ && self->sync_sem_)
                xSemaphoreGive(self->sync_sem_);
            return 0;
        }
        /* Service found, now discover characteristics */
        self->discover_characteristics(conn_handle);
        return 0;
    }

    ESP_LOGE(TAG, "Service discovery error: %d", error->status);
    return 0;
}

/* ================================================================== */
/*  BLE - Characteristic discovery                                     */
/* ================================================================== */

void AirbnkGateway::discover_characteristics(uint16_t conn_handle) {
    int rc = ble_gattc_disc_all_chrs(conn_handle,
                                      svc_start_handle_,
                                      svc_end_handle_,
                                      gatt_chr_disc_cb,
                                      this);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_all_chrs failed: %d", rc);
        state_ = BleState::ERROR;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int AirbnkGateway::gatt_chr_disc_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *chr,
                                     void *arg) {
    auto *self = static_cast<AirbnkGateway *>(arg);
    if (!self) return BLE_HS_EINVAL;

    if (error->status == 0 && chr != nullptr) {
        if (chr->uuid.u.type == BLE_UUID_TYPE_16) {
            uint16_t uuid = chr->uuid.u16.value;
            if (uuid == AIRBNK_WRITE_CHAR_UUID) {
                self->write_chr_handle_ = chr->val_handle;
                ESP_LOGI(TAG, "Found write char 0xFFF2: handle=0x%04x",
                         chr->val_handle);
            } else if (uuid == AIRBNK_READ_CHAR_UUID) {
                self->read_chr_handle_ = chr->val_handle;
                ESP_LOGI(TAG, "Found read char 0xFFF3: handle=0x%04x",
                         chr->val_handle);
            }
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (self->write_chr_handle_ == 0) {
            ESP_LOGE(TAG, "Write characteristic 0xFFF2 not found!");
            self->state_ = BleState::ERROR;
            if (self->is_sending_ && self->sync_sem_)
                xSemaphoreGive(self->sync_sem_);
            return 0;
        }
        ESP_LOGI(TAG, "Characteristic discovery complete");

        /* If we are in the middle of sending, proceed with writes */
        if (self->is_sending_) {
            self->state_ = BleState::CONNECTED_WRITING;
            self->write_to_lock(self->pending_cmd1_);
        }
        return 0;
    }

    ESP_LOGE(TAG, "Characteristic discovery error: %d", error->status);
    return 0;
}

/* ================================================================== */
/*  BLE - Write and Read                                               */
/* ================================================================== */

void AirbnkGateway::write_to_lock(const std::vector<uint8_t> &data) {
    if (write_chr_handle_ == 0) return;

    int rc = ble_gattc_write_flat(conn_handle_,
                                   write_chr_handle_,
                                   data.data(), data.size(),
                                   gatt_write_cb,
                                   this);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_flat failed: %d", rc);
        state_ = BleState::ERROR;
        publish_command_result(false, "WRITE_FAILED", 0, "");
        if (sync_sem_) xSemaphoreGive(sync_sem_);
    }
}

int AirbnkGateway::gatt_write_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr,
                                  void *arg) {
    auto *self = static_cast<AirbnkGateway *>(arg);
    if (!self) return BLE_HS_EINVAL;

    if (error->status == 0) {
        ESP_LOGI(TAG, "Write completed successfully");

        /* If we wrote cmd1, now write cmd2 */
        if (!self->pending_cmd2_.empty()) {
            std::vector<uint8_t> cmd2 = std::move(self->pending_cmd2_);
            self->pending_cmd2_.clear();
            self->write_to_lock(cmd2);
            return 0;
        }

        /* Both commands written, now read status */
        self->state_ = BleState::CONNECTED_READING;
        self->read_from_lock();
        return 0;
    }

    ESP_LOGE(TAG, "Write error: %d", error->status);
    self->state_ = BleState::ERROR;
    self->publish_command_result(false, "WRITE_GATT_ERROR", self->pending_sign_, "");
    if (self->sync_sem_) xSemaphoreGive(self->sync_sem_);
    return 0;
}

void AirbnkGateway::read_from_lock() {
    if (read_chr_handle_ == 0) return;

    int rc = ble_gattc_read(conn_handle_, read_chr_handle_,
                             gatt_read_cb, this);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_read failed: %d", rc);
        /* Read failure is not fatal - still report success for the command */
        publish_command_result(true, "OK_NO_STATUS", 0, "");
        is_sending_ = false;
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
        if (sync_sem_) xSemaphoreGive(sync_sem_);
    }
}

int AirbnkGateway::gatt_read_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr,
                                 void *arg) {
    auto *self = static_cast<AirbnkGateway *>(arg);
    if (!self) return BLE_HS_EINVAL;

    if (error->status == 0 && attr != nullptr) {
        std::string status_hex = bytes_to_hex(attr->om->om_data,
                                               attr->om->om_len);
        ESP_LOGI(TAG, "Read status: %s (%d bytes)",
                 status_hex.c_str(), attr->om->om_len);
        self->publish_command_result(true, "", self->pending_sign_, status_hex);
    } else {
        ESP_LOGW(TAG, "Read error: %d - reporting OK anyway", error->status);
        self->publish_command_result(true, "OK_NO_STATUS", self->pending_sign_, "");
    }

    self->is_sending_ = false;
    self->state_ = BleState::CONNECTED_DISCONNECTING;

    /* Disconnect and restart scanning */
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);

    if (self->sync_sem_) xSemaphoreGive(self->sync_sem_);
    return 0;
}

/* ================================================================== */
/*  Send custom command (dumb pass-through)                             */
/* ================================================================== */

void AirbnkGateway::send_custom_command(const std::vector<uint8_t> &cmd1,
                                         const std::vector<uint8_t> &cmd2,
                                         int sign) {
    if (is_sending_) {
        ESP_LOGW(TAG, "Already sending, ignoring");
        publish_command_result(false, "BUSY", 0, "");
        return;
    }

    is_sending_ = true;
    send_retries_ = 0;
    pending_sign_ = sign;
    pending_cmd1_ = cmd1;
    pending_cmd2_ = cmd2;

    if (lock_found_) {
        /* Lock address already known, connect directly */
        stop_scanning();
        connect_to_lock(lock_addr_, lock_addr_type_);
    } else {
        /* Scan will find the lock and connect via BLE_GAP_EVENT_DISC */
        ESP_LOGI(TAG, "Waiting for lock advertisement to connect...");
    }
}

/* ================================================================== */
/*  Helpers                                                            */
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

}  // namespace airbnk_gateway
}  // namespace esphome
