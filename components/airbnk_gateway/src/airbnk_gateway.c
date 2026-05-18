#include "airbnk_gateway.h"
#include "airbnk_ble.h"
#include "airbnk_mqtt.h"
#include "airbnk_utils.h"

#include "cJSON.h"
#include "esp_log.h"

#include <string.h>
#include <stdio.h>

#define TAG "airbnk_gateway"

static airbnk_gateway_config_t g_config;
static bool g_running = false;

// ---------------------------------------------------------------------------
// BLE -> MQTT: forward advertisement data
// ---------------------------------------------------------------------------
static void on_ble_adv(const char *mac, int rssi,
                       const uint8_t *manuf_data, int data_len,
                       void *user_ctx) {
    (void)user_ctx;

    // Convert manufacturer data to uppercase hex string
    char data_hex[64];
    airbnk_to_hex(data_hex, manuf_data,
                  data_len > (int)(sizeof(data_hex) - 1) / 2
                      ? (int)(sizeof(data_hex) - 1) / 2
                      : data_len);

    // Build JSON: {"mac":"...","rssi":-NN,"data":"HEX..."}
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "mac", mac);
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddStringToObject(root, "data", data_hex);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        airbnk_mqtt_publish_adv(json_str);
        free(json_str);
    }
    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// BLE -> MQTT: forward command execution result
// ---------------------------------------------------------------------------
static void on_ble_cmd_result(bool success, const char *status_hex,
                               void *user_ctx) {
    (void)user_ctx;

    // Build JSON: {"success":true/false, "error":"...", "sign":N,
    //              "mac":"...", "lockStatus":"..."}
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", success);
    cJSON_AddStringToObject(root, "error",
                            success ? "" : "BLE_COMMAND_FAILED");
    cJSON_AddNumberToObject(root, "sign", 0);  // sign passed through from command
    cJSON_AddStringToObject(root, "mac", g_config.lock_mac);
    cJSON_AddStringToObject(root, "lockStatus",
                            status_hex ? status_hex : "");

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        airbnk_mqtt_publish_result(json_str);
        free(json_str);
    }
    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// MQTT -> BLE: incoming command from HA integration
// ---------------------------------------------------------------------------
static void on_mqtt_cmd(const char *topic, const char *data,
                         int data_len, void *user_ctx) {
    (void)topic;
    (void)user_ctx;

    // Parse JSON command: {"sign":N, "command1":"HEX...", "command2":"HEX..."}
    // Make null-terminated copy of data
    char *buf = malloc(data_len + 1);
    if (!buf) return;
    memcpy(buf, data, data_len);
    buf[data_len] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGW(TAG, "Failed to parse command JSON");
        return;
    }

    cJSON *sign_json      = cJSON_GetObjectItem(root, "sign");
    cJSON *command1_json  = cJSON_GetObjectItem(root, "command1");
    cJSON *command2_json  = cJSON_GetObjectItem(root, "command2");

    if (!cJSON_IsNumber(sign_json) ||
        !cJSON_IsString(command1_json) ||
        !cJSON_IsString(command2_json)) {
        ESP_LOGW(TAG, "Invalid command JSON fields");
        cJSON_Delete(root);
        return;
    }

    int sign = sign_json->valueint;
    const char *cmd1 = command1_json->valuestring;
    const char *cmd2 = command2_json->valuestring;

    ESP_LOGI(TAG, "Received command: sign=%d, cmd1=%s, cmd2=%s",
             sign, cmd1, cmd2);

    // Forward to BLE
    airbnk_ble_send_command(cmd1, cmd2, sign);

    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int airbnk_gateway_start(const airbnk_gateway_config_t *config) {
    if (config == NULL || config->lock_mac == NULL ||
        config->mqtt_broker_uri == NULL || config->mqtt_topic_prefix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (g_running) return ESP_ERR_INVALID_STATE;

    memcpy(&g_config, config, sizeof(airbnk_gateway_config_t));

    // 1. Initialize BLE subsystem
    airbnk_ble_config_t ble_cfg = {
        .target_mac    = config->lock_mac,
        .scan_interval = config->scan_interval ? config->scan_interval : 0x80,
        .scan_window   = config->scan_window ? config->scan_window : 0x40,
        .active_scan   = false,
        .adv_callback      = on_ble_adv,
        .cmd_result_callback = on_ble_cmd_result,
        .user_ctx      = NULL,
    };

    int ret = airbnk_ble_init(&ble_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLE init failed: %d", ret);
        return ret;
    }

    // 2. Initialize MQTT subsystem
    airbnk_mqtt_config_t mqtt_cfg = {
        .broker_uri   = config->mqtt_broker_uri,
        .username     = config->mqtt_username,
        .password     = config->mqtt_password,
        .topic_prefix = config->mqtt_topic_prefix,
        .lwt_topic    = NULL,  // auto-generated as prefix/LWT
        .lwt_online   = "Online",
        .lwt_offline  = "Offline",
        .cmd_callback = on_mqtt_cmd,
        .user_ctx     = NULL,
    };

    ret = airbnk_mqtt_init(&mqtt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "MQTT init failed: %d", ret);
        airbnk_ble_deinit();
        return ret;
    }

    g_running = true;
    ESP_LOGI(TAG, "Airbnk Gateway started: lock=%s, mqtt=%s, prefix=%s",
             config->lock_mac, config->mqtt_broker_uri,
             config->mqtt_topic_prefix);

    return ESP_OK;
}

int airbnk_gateway_stop(void) {
    if (!g_running) return ESP_OK;

    airbnk_mqtt_deinit();
    airbnk_ble_deinit();

    g_running = false;
    ESP_LOGI(TAG, "Airbnk Gateway stopped");

    return ESP_OK;
}
