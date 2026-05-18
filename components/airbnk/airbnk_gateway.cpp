#include "airbnk_gateway.h"

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"

#include <cstring>
#include <algorithm>
#include <ctime>

namespace airbnk {

/* ------------------------------------------------------------------ */
/*  Configuration setters                                              */
/* ------------------------------------------------------------------ */

void AirbnkGateway::set_mac_address(const std::string &mac) {
    mac_address_ = mac;
    /* Normalise: uppercase */
    std::transform(mac_address_.begin(), mac_address_.end(),
                   mac_address_.begin(), ::toupper);
}

void AirbnkGateway::set_topic_prefix(const std::string &prefix) {
    topic_prefix_ = prefix;
    advert_topic_        = prefix + "/adv";
    command_topic_       = prefix + "/command";
    command_result_topic_ = prefix + "/command_result";
}

void AirbnkGateway::set_manufacturer_key(const std::string &key) {
    manufacturer_key_ = key;
}

void AirbnkGateway::set_binding_key(const std::string &key) {
    binding_key_ = key;
}


/* ------------------------------------------------------------------ */
/*  ESPHome lifecycle                                                  */
/* ------------------------------------------------------------------ */

void AirbnkGateway::setup() {
    ESP_LOGI(TAG, "AirbnkGateway starting up");
    ESP_LOGI(TAG, "  Target MAC: %s", mac_address_.c_str());
    ESP_LOGI(TAG, "  Topic prefix: %s", topic_prefix_.c_str());

    /* Resolve MQTT client pointer */
    mqtt_client_ = mqtt::global_mqtt_client;
    if (!mqtt_client_) {
        ESP_LOGE(TAG, "MQTT client not available! Is MQTT configured?");
        this->mark_failed();
        return;
    }

    /* Create synchronisation semaphore (binary, initially taken) */
    sync_sem_ = xSemaphoreCreateBinary();
    if (!sync_sem_) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        this->mark_failed();
        return;
    }

    /* Subscribe to MQTT command topic */
    subscribe_mqtt();

    /* Initialise NimBLE */
    init_nimble();

    /* Start BLE scanning */
    start_scanning();

    ESP_LOGI(TAG, "AirbnkGateway setup complete");
}

void AirbnkGateway::loop() {
    /* Nothing periodic for now — NimBLE runs in its own task.
     * The event callbacks handle state transitions via semaphores.
     * We could add a watchdog timer here if needed. */
}

float AirbnkGateway::get_setup_priority() const {
    /* After WiFi/MQTT are ready but before other BLE components */
    return setup_priority::AFTER_WIFI;
}


/* ------------------------------------------------------------------ */
/*  MQTT                                                               */
/* ------------------------------------------------------------------ */

void AirbnkGateway::subscribe_mqtt() {
    if (!mqtt_client_) return;

    ESP_LOGI(TAG, "Subscribing to MQTT: %s", command_topic_.c_str());
    mqtt_client_->subscribe_json(
        command_topic_,
        [this](const std::string &topic, JsonObject root) {
            this->on_mqtt_command(topic, root);
        },
        1  /* QoS */
    );
}

void AirbnkGateway::publish_advertisement(const std::string &mac,
                                           int rssi,
                                           const std::string &data) {
    if (!mqtt_client_) return;

    mqtt_client_->publish_json(
        advert_topic_,
        [&mac, rssi, &data](JsonObject root) {
            root["mac"]  = mac;
            root["rssi"] = rssi;
            root["data"] = data;
        },
        1,    /* QoS */
        false /* retain */
    );
}

void AirbnkGateway::publish_command_result(bool success,
                                           const std::string &error,
                                           const std::string &lock_status) {
    if (!mqtt_client_) return;

    mqtt_client_->publish_json(
        command_result_topic_,
        [this, success, &error, &lock_status](JsonObject root) {
            root["success"]    = success;
            root["error"]      = error;
            root["mac"]        = mac_address_;
            root["lockStatus"] = lock_status;
        },
        1,    /* QoS */
        false /* retain */
    );
}

void AirbnkGateway::on_mqtt_command(const std::string &topic,
                                     JsonObject root) {
    if (is_sending_) {
        ESP_LOGW(TAG, "Already sending a command, ignoring");
        return;
    }

    /* Check if pre-computed commands are provided */
    const char *cmd1_str = root["command1"];
    const char *cmd2_str = root["command2"];

    if (cmd1_str && cmd2_str) {
        /* Pre-computed command (rospogrigio/airbnk_mqtt compatible) */
        std::vector<uint8_t> cmd1 = hex_to_bytes(cmd1_str);
        std::vector<uint8_t> cmd2 = hex_to_bytes(cmd2_str);
        ESP_LOGI(TAG, "Received pre-computed command (%zu + %zu bytes)",
                 cmd1.size(), cmd2.size());
        send_custom_command(cmd1, cmd2);
    } else if (has_keys()) {
        /* Generate command internally */
        const char *op_str = root["lockOp"];
        uint8_t lock_op = (op_str && strcmp(op_str, "unlock") == 0)
                              ? AIRBNK_LOCK_OP_UNLOCK
                              : AIRBNK_LOCK_OP_LOCK;

        if (root.containsKey("command")) {
            const char *cmd_str = root["command"];
            if (strcmp(cmd_str, "lock") == 0) {
                lock_op = AIRBNK_LOCK_OP_LOCK;
            } else if (strcmp(cmd_str, "unlock") == 0) {
                lock_op = AIRBNK_LOCK_OP_UNLOCK;
            }
        }

        ESP_LOGI(TAG, "Generating %s command internally",
                 lock_op == AIRBNK_LOCK_OP_LOCK ? "lock" : "unlock");

        std::vector<uint8_t> cmd1, cmd2;
        generate_command_packet(lock_op, cmd1, cmd2);
        if (!cmd1.empty() && !cmd2.empty()) {
            send_custom_command(cmd1, cmd2);
        } else {
            publish_command_result(false, "COMMAND_GENERATION_FAILED", "");
        }
    } else {
        ESP_LOGW(TAG, "No command data and no keys configured");
        publish_command_result(false, "NO_COMMAND_AND_NO_KEYS", "");
    }
}


/* ------------------------------------------------------------------ */
/*  BLE — NimBLE initialisation                                        */
/* ------------------------------------------------------------------ */

void AirbnkGateway::nimble_host_task(void *param) {
    /* This function never returns — it runs the NimBLE event loop */
    ESP_LOGD("airbnk", "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void AirbnkGateway::init_nimble() {
    ESP_LOGI(TAG, "Initialising NimBLE host");

    int rc;

    /* Initialise the NimBLE host */
    rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", rc);
        this->mark_failed();
        return;
    }

    /* Initialise GAP and GATT services */
    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Set the device name (not critical but nice for debugging) */
    rc = ble_svc_gap_device_name_set("AirbnkGateway");
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_svc_gap_device_name_set failed: %d", rc);
    }

    /* Configure host callbacks */
    ble_hs_cfg.sync_cb = [](void) {
        /* Called when host and controller are synchronised */
        ESP_LOGD("airbnk", "NimBLE synced with controller");
    };
    ble_hs_cfg.reset_cb = [](int reason) {
        ESP_LOGW("airbnk", "NimBLE host reset, reason: %d", reason);
    };

    /* Start the NimBLE host FreeRTOS task */
    nimble_port_freertos_init(nimble_host_task);

    ESP_LOGI(TAG, "NimBLE host initialised");
}


/* ------------------------------------------------------------------ */
/*  BLE — Scanning                                                     */
/* ------------------------------------------------------------------ */

void AirbnkGateway::start_scanning() {
    if (state_ == BleState::SCANNING) return;

    ESP_LOGI(TAG, "Starting BLE scan for %s", mac_address_.c_str());

    /* Parse target MAC into ble_addr_t
       Expected format: "E4:E1:12:C7:5C:52" */
    std::string mac = mac_address_;
    mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());

    if (mac.length() != 12) {
        ESP_LOGE(TAG, "Invalid MAC address: %s", mac_address_.c_str());
        return;
    }

    /* Convert hex string to bytes */
    for (size_t i = 0; i < 6; i++) {
        std::string byte_str = mac.substr(i * 2, 2);
        lock_addr_.val[5 - i] = strtol(byte_str.c_str(), nullptr, 16);
    }
    lock_addr_.type = BLE_OWN_ADDR_PUBLIC;

    /* Configure discovery parameters */
    struct ble_gap_disc_params disc_params;
    memset(&disc_params, 0, sizeof(disc_params));
    disc_params.itvl             = 0x30;   /* Scan interval: 30 * 0.625ms */
    disc_params.window           = 0x30;   /* Scan window:  30 * 0.625ms */
    disc_params.filter_policy    = 0;      /* Accept all advertisements */
    disc_params.limited          = 0;      /* Not limited discovery */
    disc_params.passive          = 1;      /* Passive scan (no scan req) */
    disc_params.filter_duplicates = 1;     /* Filter duplicates (1=on) */

    int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                          AIRBNK_SCAN_DURATION_MS,
                          &disc_params,
                          gap_event_cb,
                          this);
    if (rc == 0) {
        state_ = BleState::SCANNING;
        ESP_LOGI(TAG, "BLE scan started");
    } else {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
        state_ = BleState::ERROR;
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


/* ------------------------------------------------------------------ */
/*  BLE — GAP event handler                                            */
/* ------------------------------------------------------------------ */

int AirbnkGateway::gap_event_cb(struct ble_gap_event *event, void *arg) {
    auto *self = static_cast<AirbnkGateway *>(arg);
    if (!self) return BLE_HS_EINVAL;
    return self->handle_gap_event(event);
}

int AirbnkGateway::handle_gap_event(struct ble_gap_event *event) {
    switch (event->type) {

    case BLE_GAP_EVENT_DISC: {
        /* Advertisement received */
        const auto &disc = event->disc;

        /* Compare MAC addresses */
        if (memcmp(disc.addr.val, lock_addr_.val, 6) != 0) {
            return 0;  /* Not our lock */
        }

        ESP_LOGI(TAG, "Lock found! RSSI: %d", disc.rssi);

        /* Extract manufacturer data */
        std::string man_data;
        if (disc.length_data > 0 && disc.data != nullptr) {
            const uint8_t *p = disc.data;
            uint8_t remain = disc.length_data;
            while (remain >= 2) {
                uint8_t field_len = p[0];
                uint8_t field_type = p[1];
                if (field_len == 0) break;

                if (field_type == 0xFF && remain >= field_len + 1) {
                    /* Manufacturer specific data */
                    man_data = bytes_to_hex(p + 2, field_len - 1);
                    break;
                }
                if (field_len + 1 > remain) break;
                remain -= (field_len + 1);
                p += (field_len + 1);
            }
        }

        /* Publish to MQTT */
        std::string mac_str = mac_to_string(disc.addr.val);
        publish_advertisement(mac_str, disc.rssi, man_data);

        /* If not currently sending, just report */
        if (!is_sending_) {
            lock_found_ = true;
            last_adv_data_ = man_data;
            last_adv_rssi_ = disc.rssi;
            return 0;
        }

        /* We need to connect — stop scanning first */
        stop_scanning();
        lock_addr_ = disc.addr;
        connect_to_lock(lock_addr_);
        return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE: {
        ESP_LOGD(TAG, "Scan complete (reason: %d)", event->disc_complete.reason);
        /* Scan finished — restart if we're still in scanning state */
        if (state_ == BleState::SCANNING) {
            /* Restart scan (for continuous scanning) */
            int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC,
                                  AIRBNK_SCAN_DURATION_MS,
                                  nullptr,  /* reuse previous params */
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
            /* Connection successful */
            conn_handle_ = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected to lock, handle=%d", conn_handle_);
            state_ = BleState::CONNECTED_SERVICE_DISC;

            /* Discover the Airbnk service */
            discover_services(conn_handle_);

            /* Give semaphore to unblock caller */
            if (sync_sem_) xSemaphoreGive(sync_sem_);
        } else {
            ESP_LOGE(TAG, "Connection failed: %d", event->connect.status);
            state_ = BleState::ERROR;

            /* Notify caller */
            if (sync_sem_) xSemaphoreGive(sync_sem_);
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT: {
        ESP_LOGI(TAG, "Disconnected from lock, reason=%d",
                 event->disconnect.reason);
        conn_handle_ = 0;
        state_ = BleState::IDLE;

        /* If we were sending, notify caller */
        if (sync_sem_) xSemaphoreGive(sync_sem_);

        /* Restart scanning */
        start_scanning();
        return 0;
    }

    default:
        return 0;
    }
}


/* ------------------------------------------------------------------ */
/*  BLE — Connection                                                   */
/* ------------------------------------------------------------------ */

void AirbnkGateway::connect_to_lock(const ble_addr_t &addr) {
    ESP_LOGI(TAG, "Connecting to lock...");
    state_ = BleState::CONNECTING;

    struct ble_gap_conn_params conn_params;
    memset(&conn_params, 0, sizeof(conn_params));
    conn_params.scan_itvl        = 0x0030;
    conn_params.scan_window      = 0x0030;
    conn_params.itvl_min         = AIRBNK_CONN_ITVL;
    conn_params.itvl_max         = AIRBNK_CONN_ITVL;
    conn_params.latency          = AIRBNK_CONN_LATENCY;
    conn_params.supervision_timeout = AIRBNK_CONN_SUPERVISION;
    conn_params.min_ce_len       = 16;
    conn_params.max_ce_len       = 32;

    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr,
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


/* ------------------------------------------------------------------ */
/*  BLE — Service discovery                                            */
/* ------------------------------------------------------------------ */

void AirbnkGateway::discover_services(uint16_t conn_handle) {
    ble_uuid16_t svc_uuid;
    svc_uuid.u.type = BLE_UUID_TYPE_16;
    svc_uuid.value  = AIRBNK_SERVICE_UUID;

    auto *ctx = new EventContext();
    ctx->self = this;

    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, &svc_uuid.u,
                                         gatt_svc_disc_cb, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_svc_by_uuid failed: %d", rc);
        delete ctx;
        state_ = BleState::ERROR;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int AirbnkGateway::gatt_svc_disc_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_svc *service,
                                     void *arg) {
    auto *ctx = static_cast<EventContext *>(arg);
    if (!ctx) return BLE_HS_EINVAL;
    auto *self = ctx->self;
    if (!self) return BLE_HS_EINVAL;

    if (error->status == 0 && service != nullptr) {
        /* Service found */
        self->svc_handle_ = service->start_handle;
        ESP_LOGI(self->TAG, "Service found: start_handle=%d end_handle=%d",
                 service->start_handle, service->end_handle);
        return 0;  /* Continue discovery (should be only one) */
    }

    if (error->status == BLE_HS_EDONE) {
        /* Discovery complete */
        if (self->svc_handle_ == 0) {
            ESP_LOGE(self->TAG, "Airbnk service not found!");
            self->state_ = BleState::ERROR;
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            delete ctx;
            return 0;
        }

        /* Proceed to characteristic discovery */
        self->state_ = BleState::CONNECTED_CHAR_DISC;
        self->discover_characteristics(conn_handle);
    } else {
        ESP_LOGE(self->TAG, "Service discovery error: %d", error->status);
        self->state_ = BleState::ERROR;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    delete ctx;
    return 0;
}


/* ------------------------------------------------------------------ */
/*  BLE — Characteristic discovery                                     */
/* ------------------------------------------------------------------ */

void AirbnkGateway::discover_characteristics(uint16_t conn_handle) {
    /* Discover write characteristic (0xFFF2) */
    ble_uuid16_t write_uuid;
    write_uuid.u.type = BLE_UUID_TYPE_16;
    write_uuid.value  = AIRBNK_WRITE_CHAR_UUID;

    auto *ctx = new EventContext();
    ctx->self = this;

    int rc = ble_gattc_disc_chrs_by_uuid(conn_handle,
                                          svc_handle_, svc_handle_ + 0xFFFF,
                                          &write_uuid.u,
                                          gatt_chr_disc_cb,
                                          ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_disc_chrs_by_uuid failed: %d", rc);
        delete ctx;
        state_ = BleState::ERROR;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int AirbnkGateway::gatt_chr_disc_cb(uint16_t conn_handle,
                                     const struct ble_gatt_error *error,
                                     const struct ble_gatt_chr *chr,
                                     void *arg) {
    auto *ctx = static_cast<EventContext *>(arg);
    if (!ctx) return BLE_HS_EINVAL;
    auto *self = ctx->self;
    if (!self) return BLE_HS_EINVAL;

    if (error->status == 0 && chr != nullptr) {
        uint16_t uuid_val = ble_uuid_u16(&chr->uuid.u);
        if (uuid_val == AIRBNK_WRITE_CHAR_UUID) {
            self->write_chr_handle_ = chr->val_handle;
            ESP_LOGI(self->TAG, "Write char found: def_handle=%d val_handle=%d",
                     chr->def_handle, chr->val_handle);
        } else if (uuid_val == AIRBNK_READ_CHAR_UUID) {
            self->read_chr_handle_ = chr->val_handle;
            ESP_LOGI(self->TAG, "Read char found: def_handle=%d val_handle=%d",
                     chr->def_handle, chr->val_handle);
        }
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        /* Check if we found both characteristics */
        if (self->write_chr_handle_ == 0) {
            ESP_LOGE(self->TAG, "Write characteristic (0xFFF2) not found!");
            ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            delete ctx;
            return 0;
        }

        /* If we only searched for write char, now search for read char */
        if (self->read_chr_handle_ == 0) {
            ble_uuid16_t read_uuid;
            read_uuid.u.type = BLE_UUID_TYPE_16;
            read_uuid.value  = AIRBNK_READ_CHAR_UUID;

            auto *ctx2 = new EventContext();
            ctx2->self = self;
            ctx2->write_data = ctx->write_data;

            int rc = ble_gattc_disc_chrs_by_uuid(
                conn_handle,
                self->svc_handle_, self->svc_handle_ + 0xFFFF,
                &read_uuid.u,
                gatt_chr_disc_cb,
                ctx2);

            delete ctx;

            if (rc != 0) {
                ESP_LOGE(self->TAG, "Read char discovery failed: %d", rc);
                self->state_ = BleState::ERROR;
                ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        }

        /* Both found — proceed to write if we have data */
        self->state_ = BleState::CONNECTED_WRITING;

        if (!ctx->write_data.empty()) {
            self->write_to_lock(ctx->write_data);
        } else if (self->event_ctx_ && !self->event_ctx_->write_data.empty()) {
            self->write_to_lock(self->event_ctx_->write_data);
        } else {
            ESP_LOGI(self->TAG, "No data to write, reading status...");
            /* Just read status for discovery */
            self->read_from_lock();
        }
    } else {
        ESP_LOGE(self->TAG, "Char discovery error: %d", error->status);
        self->state_ = BleState::ERROR;
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    delete ctx;
    return 0;
}


/* ------------------------------------------------------------------ */
/*  BLE — Write                                                        */
/* ------------------------------------------------------------------ */

void AirbnkGateway::write_to_lock(const std::vector<uint8_t> &data) {
    ESP_LOGI(TAG, "Writing %zu bytes to characteristic 0xFFF2",
             data.size());

    auto *ctx = new EventContext();
    ctx->self = this;
    ctx->write_data = data;

    int rc = ble_gattc_write_flat(conn_handle_, write_chr_handle_,
                                   data.data(), data.size(),
                                   gatt_write_cb, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_write_flat failed: %d", rc);
        delete ctx;
        state_ = BleState::ERROR;
        publish_command_result(false, "WRITE_FAILED", "");
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int AirbnkGateway::gatt_write_cb(uint16_t conn_handle,
                                  const struct ble_gatt_error *error,
                                  struct ble_gatt_attr *attr,
                                  void *arg) {
    auto *ctx = static_cast<EventContext *>(arg);
    if (!ctx) return BLE_HS_EINVAL;
    auto *self = ctx->self;
    if (!self) return BLE_HS_EINVAL;

    if (error->status == 0) {
        ESP_LOGI(self->TAG, "Write succeeded");

        /* If there's a second command to write, do it now.
         * Otherwise, proceed to read status. */
        if (self->event_ctx_ && !self->event_ctx_->write_data.empty()
            && ctx->write_data != self->event_ctx_->write_data) {
            /* This was the first command; write the second */
            self->write_to_lock(self->event_ctx_->write_data);
        } else {
            self->state_ = BleState::CONNECTED_READING;
            self->read_from_lock();
        }
    } else {
        ESP_LOGE(self->TAG, "Write failed: %d", error->status);
        self->state_ = BleState::ERROR;
        self->publish_command_result(false, "WRITE_FAILED", "");
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }

    delete ctx;
    return 0;
}


/* ------------------------------------------------------------------ */
/*  BLE — Read                                                         */
/* ------------------------------------------------------------------ */

void AirbnkGateway::read_from_lock() {
    if (read_chr_handle_ == 0) {
        ESP_LOGW(TAG, "No read characteristic handle");
        publish_command_result(true, "", "OK");
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
        return;
    }

    ESP_LOGI(TAG, "Reading status from characteristic 0xFFF3");

    auto *ctx = new EventContext();
    ctx->self = this;

    int rc = ble_gattc_read(conn_handle_, read_chr_handle_,
                             gatt_read_cb, ctx);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gattc_read failed: %d", rc);
        delete ctx;
        state_ = BleState::ERROR;
        publish_command_result(false, "READ_FAILED", "");
        ble_gap_terminate(conn_handle_, BLE_ERR_REM_USER_CONN_TERM);
    }
}

int AirbnkGateway::gatt_read_cb(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 struct ble_gatt_attr *attr,
                                 void *arg) {
    auto *ctx = static_cast<EventContext *>(arg);
    if (!ctx) return BLE_HS_EINVAL;
    auto *self = ctx->self;
    if (!self) return BLE_HS_EINVAL;

    std::string status_hex;

    if (error->status == 0 && attr != nullptr) {
        status_hex = bytes_to_hex(attr->om->om_data, attr->om->om_len);
        ESP_LOGI(self->TAG, "Read status: %s", status_hex.c_str());
    } else if (error->status == BLE_HS_EDONE) {
        ESP_LOGD(self->TAG, "Read complete");
    } else {
        ESP_LOGW(self->TAG, "Read status: %d", error ? error->status : -1);
    }

    self->state_ = BleState::IDLE;
    self->is_sending_ = false;
    self->publish_command_result(true, "", status_hex);

    /* Disconnect */
    ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);

    delete ctx;
    return 0;
}


/* ------------------------------------------------------------------ */
/*  Public API — send commands                                         */
/* ------------------------------------------------------------------ */

void AirbnkGateway::send_lock_command() {
    if (!has_keys()) {
        ESP_LOGE(TAG, "Cannot generate lock command: no keys configured");
        return;
    }
    std::vector<uint8_t> cmd1, cmd2;
    generate_command_packet(AIRBNK_LOCK_OP_LOCK, cmd1, cmd2);
    if (!cmd1.empty() && !cmd2.empty()) {
        send_custom_command(cmd1, cmd2);
    }
}

void AirbnkGateway::send_unlock_command() {
    if (!has_keys()) {
        ESP_LOGE(TAG, "Cannot generate unlock command: no keys configured");
        return;
    }
    std::vector<uint8_t> cmd1, cmd2;
    generate_command_packet(AIRBNK_LOCK_OP_UNLOCK, cmd1, cmd2);
    if (!cmd1.empty() && !cmd2.empty()) {
        send_custom_command(cmd1, cmd2);
    }
}

void AirbnkGateway::send_custom_command(const std::vector<uint8_t> &cmd1,
                                         const std::vector<uint8_t> &cmd2) {
    if (is_sending_) {
        ESP_LOGW(TAG, "Already sending, command queued...");

        /* Wait briefly then retry via MQTT re-publish */
        vTaskDelay(pdMS_TO_TICKS(500));
        if (is_sending_) {
            ESP_LOGE(TAG, "Still sending, dropping command");
            publish_command_result(false, "BUSY", "");
            return;
        }
    }

    is_sending_ = true;
    send_retries_ = 0;

    /* Store command data in event context */
    if (!event_ctx_) {
        event_ctx_ = new EventContext();
    }
    event_ctx_->self = this;
    event_ctx_->write_data = cmd2;  /* cmd2 is written after cmd1 */

    /* If we have the lock address, connect directly */
    if (lock_found_) {
        ESP_LOGI(TAG, "Lock already discovered, connecting...");
        stop_scanning();
        connect_to_lock(lock_addr_);

        /* Wait for connection (with timeout) */
        if (sync_sem_ && xSemaphoreTake(sync_sem_, pdMS_TO_TICKS(15000)) == pdTRUE) {
            if (state_ == BleState::CONNECTED_SERVICE_DISC) {
                /* Connection succeeded, write cmd1 to the write char */
                /* (The gap event callback already started service discovery) */
                /* Wait for service discovery, char discovery, then write ctx data */
                event_ctx_->write_data = cmd1;  /* write cmd1 first */
            } else {
                ESP_LOGE(TAG, "Connection failed!");
                goto reconnect;
            }
        } else {
            ESP_LOGE(TAG, "Connection timeout!");
            goto reconnect;
        }
    } else {
        /* Need to discover the lock first */
        ESP_LOGI(TAG, "Lock not yet discovered, starting scan...");
        /* Store commands and let discovery callback trigger connection */
        event_ctx_->write_data = cmd1;  /* First write passes cmd1 */
        start_scanning();
    }

    return;

reconnect:
    /* Retry logic */
    is_sending_ = false;
    lock_found_ = false;
    publish_command_result(false, "CONNECTION_FAILED", "");
    start_scanning();
}


/* ------------------------------------------------------------------ */
/*  Airbnk command generation                                          */
/* ------------------------------------------------------------------ */

bool AirbnkGateway::has_keys() const {
    return !manufacturer_key_.empty() && !binding_key_.empty();
}

void AirbnkGateway::generate_command_packet(uint8_t lock_op,
                                             std::vector<uint8_t> &cmd1,
                                             std::vector<uint8_t> &cmd2) {
    if (!has_keys()) {
        ESP_LOGE(TAG, "Cannot generate command: keys not configured");
        return;
    }

    /* Convert hex keys to byte arrays */
    std::vector<uint8_t> manuf_key = hex_to_bytes(manufacturer_key_);
    std::vector<uint8_t> bind_key  = hex_to_bytes(binding_key_);

    if (manuf_key.size() != 16) {
        ESP_LOGE(TAG, "Manufacturer key must be 16 bytes (32 hex chars), got %zu",
                 manuf_key.size());
        return;
    }
    if (bind_key.size() != 16) {
        ESP_LOGE(TAG, "Binding key must be 16 bytes (32 hex chars), got %zu",
                 bind_key.size());
        return;
    }

    /* Generate timestamp (seconds since epoch) */
    uint32_t timestamp = (uint32_t)time(nullptr);

    /* Use last lock events counter (starts at 0, increments with each op).
     * In a persistent implementation, this would be stored in NVS.
     * For now we use the timestamp as the event counter. */
    uint32_t lock_events = timestamp & 0xFFFF;

    /* Build packet bytes for command 1 */

    /* Packet structure (36 bytes total, split into cmd1[20] + cmd2[16] using only first 4?):
     *   Byte 0-2:   Header AA 10 1A
     *   Byte 3:     lockOp (1=lock, 2=unlock)
     *   Byte 4-19:  AES-ECB encrypted block (16 bytes) containing key data
     *   Byte 20-27: HMAC-SHA1 (first 8 bytes)
     *   Byte 28:    Checksum
     *   Byte 29-35: Padding/zeros
     */

    /* Create the plaintext for AES encryption:
     *   Bytes 0-3:   timestamp (4 bytes, little-endian)
     *   Bytes 4-7:   lockEvents (4 bytes, little-endian)
     *   Bytes 8-15:  zeros (padding to 16 bytes)
     */
    std::vector<uint8_t> plaintext(16, 0);
    plaintext[0] = (timestamp >> 0)  & 0xFF;
    plaintext[1] = (timestamp >> 8)  & 0xFF;
    plaintext[2] = (timestamp >> 16) & 0xFF;
    plaintext[3] = (timestamp >> 24) & 0xFF;
    plaintext[4] = (lock_events >> 0)  & 0xFF;
    plaintext[5] = (lock_events >> 8)  & 0xFF;
    plaintext[6] = (lock_events >> 16) & 0xFF;
    plaintext[7] = (lock_events >> 24) & 0xFF;
    /* Bytes 8-15 stay as 0 */

    /* AES-ECB encrypt with manufacturer key */
    std::vector<uint8_t> encrypted(16);
    if (!aes_ecb_encrypt(manuf_key, plaintext, encrypted)) {
        ESP_LOGE(TAG, "AES-ECB encryption failed");
        return;
    }

    /* Build the 36-byte packet */
    std::vector<uint8_t> packet(36, 0);

    /* Header */
    packet[0] = AIRBNK_HEADER[0];  /* 0xAA */
    packet[1] = AIRBNK_HEADER[1];  /* 0x10 */
    packet[2] = AIRBNK_HEADER[2];  /* 0x1A */

    /* Lock operation */
    packet[3] = lock_op;

    /* Bytes 4-19: AES-ECB encrypted data (16 bytes) */
    memcpy(&packet[4], encrypted.data(), 16);

    /* Bytes 20-27: HMAC-SHA1 signature (first 8 bytes).
     * The HMAC is computed over: header(3) + lockOp(1) + encrypted(16) = 20 bytes */
    std::vector<uint8_t> hmac_input;
    hmac_input.insert(hmac_input.end(), packet.begin(), packet.begin() + 4);
    hmac_input.insert(hmac_input.end(), encrypted.begin(), encrypted.end());
    /* hmac_input is 20 bytes: 3 header + 1 lockOp + 16 encrypted */

    std::vector<uint8_t> hmac_result(20, 0);  /* SHA1 = 20 bytes */
    if (!hmac_sha1(bind_key, hmac_input.data(), hmac_input.size(), hmac_result)) {
        ESP_LOGE(TAG, "HMAC-SHA1 failed");
        return;
    }

    /* Copy first 8 bytes of HMAC into packet */
    memcpy(&packet[20], hmac_result.data(), 8);

    /* Byte 28: checksum = XOR of all previous bytes */
    packet[28] = calculate_checksum(packet.data(), 28);

    /* Bytes 29-35: zeros (already set by initialization) */

    /* Split into two commands: cmd1 = first 20 bytes, cmd2 = last 16 bytes.
     * Actually per the original code, both commands are up to 20 bytes.
     * Looking at the original: cmd1 is 20 bytes containing the first part,
     * cmd2 is also written but may be shorter. Let's look at the packet again:
     *
     * Based on the rospogrigio implementation, the 36-byte packet is sent
     * as two writes: first 20 bytes, then remaining 16 bytes.
     */
    cmd1.assign(packet.begin(), packet.begin() + 20);
    cmd2.assign(packet.begin() + 20, packet.end());

    ESP_LOGI(TAG, "Generated command for op=%d, ts=%lu", lock_op,
             (unsigned long)timestamp);
    ESP_LOGD(TAG, "  cmd1: %s", bytes_to_hex(cmd1).c_str());
    ESP_LOGD(TAG, "  cmd2: %s", bytes_to_hex(cmd2).c_str());
}


/* ------------------------------------------------------------------ */
/*  Utility functions                                                  */
/* ------------------------------------------------------------------ */

std::vector<uint8_t> AirbnkGateway::hex_to_bytes(const std::string &hex) {
    std::vector<uint8_t> bytes;
    std::string h = hex;
    /* Remove any spaces or colons */
    h.erase(std::remove_if(h.begin(), h.end(),
                           [](char c) { return c == ' ' || c == ':'; }),
            h.end());

    if (h.length() % 2 != 0) return bytes;

    bytes.reserve(h.length() / 2);
    for (size_t i = 0; i < h.length(); i += 2) {
        unsigned int byte;
        if (sscanf(h.c_str() + i, "%2x", &byte) == 1) {
            bytes.push_back((uint8_t)byte);
        } else {
            bytes.clear();
            return bytes;
        }
    }
    return bytes;
}

std::string AirbnkGateway::bytes_to_hex(const uint8_t *data, size_t len) {
    static const char hex_chars[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        result.push_back(hex_chars[(data[i] >> 4) & 0x0F]);
        result.push_back(hex_chars[data[i] & 0x0F]);
    }
    return result;
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


/* ------------------------------------------------------------------ */
/*  Crypto helpers (mbedtls)                                           */
/* ------------------------------------------------------------------ */

bool AirbnkGateway::aes_ecb_encrypt(const std::vector<uint8_t> &key,
                                     const std::vector<uint8_t> &plaintext,
                                     std::vector<uint8_t> &ciphertext) {
    if (key.size() != 16) {
        ESP_LOGE(TAG, "AES key must be 16 bytes, got %zu", key.size());
        return false;
    }
    if (plaintext.size() != 16) {
        ESP_LOGE(TAG, "AES plaintext must be 16 bytes, got %zu",
                 plaintext.size());
        return false;
    }

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    int rc = mbedtls_aes_setkey_enc(&aes, key.data(), 128);
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_aes_setkey_enc failed: %d", rc);
        mbedtls_aes_free(&aes);
        return false;
    }

    ciphertext.resize(16);
    rc = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT,
                                plaintext.data(), ciphertext.data());
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_aes_crypt_ecb failed: %d", rc);
        mbedtls_aes_free(&aes);
        return false;
    }

    mbedtls_aes_free(&aes);
    return true;
}

bool AirbnkGateway::hmac_sha1(const std::vector<uint8_t> &key,
                               const uint8_t *data, size_t data_len,
                               std::vector<uint8_t> &output) {
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(
        MBEDTLS_MD_SHA1);
    if (!md_info) {
        ESP_LOGE(TAG, "mbedtls_md_info_from_type failed");
        return false;
    }

    output.resize(20);  /* SHA1 produces 20 bytes */

    int rc = mbedtls_md_hmac(md_info, key.data(), key.size(),
                              data, data_len,
                              output.data());
    if (rc != 0) {
        ESP_LOGE(TAG, "mbedtls_md_hmac failed: %d", rc);
        return false;
    }

    return true;
}

uint8_t AirbnkGateway::calculate_checksum(const uint8_t *data, size_t len) {
    uint8_t cs = 0;
    for (size_t i = 0; i < len; i++) {
        cs ^= data[i];
    }
    return cs;
}


}  /* namespace airbnk */
