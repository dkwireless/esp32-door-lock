#include "airbnk_mqtt.h"

#include "mqtt_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "airbnk_mqtt"

// Event group bits
#define MQTT_CONNECTED_BIT  (1 << 0)

static airbnk_mqtt_config_t g_mqtt_config;
static esp_mqtt_client_handle_t g_mqtt_client = NULL;
static EventGroupHandle_t g_mqtt_evt_group = NULL;
static bool g_initialized = false;

// Derived topic strings
static char g_topic_adv[128];
static char g_topic_cmd[128];
static char g_topic_result[128];
static char g_topic_lwt[128];

// ---------------------------------------------------------------------------
// MQTT event handler
// ---------------------------------------------------------------------------
static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected");
        xEventGroupSetBits(g_mqtt_evt_group, MQTT_CONNECTED_BIT);

        // Subscribe to command topic
        esp_mqtt_client_subscribe_single(g_mqtt_client, g_topic_cmd, 0);
        ESP_LOGI(TAG, "Subscribed to: %s", g_topic_cmd);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT disconnected");
        xEventGroupClearBits(g_mqtt_evt_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_DATA:
        // Incoming message
        if (g_mqtt_config.cmd_callback) {
            char topic_buf[128];
            int topic_len = event->topic_len < sizeof(topic_buf) - 1
                            ? event->topic_len : sizeof(topic_buf) - 1;
            memcpy(topic_buf, event->topic, topic_len);
            topic_buf[topic_len] = '\0';

            g_mqtt_config.cmd_callback(topic_buf, event->data, event->data_len,
                                        g_mqtt_config.user_ctx);
        }
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error");
        break;

    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Publish helper
// ---------------------------------------------------------------------------
static int mqtt_publish(const char *topic, const char *payload) {
    if (g_mqtt_client == NULL) return ESP_ERR_INVALID_STATE;

    int msg_id = esp_mqtt_client_publish(g_mqtt_client, topic, payload, 0, 0, 1);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Failed to publish to %s", topic);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "Published to %s: %s", topic, payload);
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int airbnk_mqtt_init(const airbnk_mqtt_config_t *config) {
    if (config == NULL || config->broker_uri == NULL || config->topic_prefix == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&g_mqtt_config, config, sizeof(airbnk_mqtt_config_t));

    g_mqtt_evt_group = xEventGroupCreate();
    if (g_mqtt_evt_group == NULL) return ESP_ERR_NO_MEM;

    // Build topic strings
    snprintf(g_topic_adv,    sizeof(g_topic_adv),    "%s/adv",            config->topic_prefix);
    snprintf(g_topic_cmd,    sizeof(g_topic_cmd),    "%s/command",        config->topic_prefix);
    snprintf(g_topic_result, sizeof(g_topic_result), "%s/command_result", config->topic_prefix);
    snprintf(g_topic_lwt,    sizeof(g_topic_lwt),    "%s/LWT",            config->topic_prefix);

    // MQTT client config
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker = {
            .address.uri = config->broker_uri,
        },
        .credentials = {
            .username = config->username ? config->username : "",
            .authentication = {
                .password = config->password ? config->password : "",
            },
        },
        .session = {
            .last_will = {
                .topic = g_topic_lwt,
                .msg = config->lwt_offline ? config->lwt_offline : "Offline",
                .qos = 1,
                .retain = 1,
            },
        },
    };

    g_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (g_mqtt_client == NULL) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        vEventGroupDelete(g_mqtt_evt_group);
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(g_mqtt_client, ESP_EVENT_ANY_ID,
                                    mqtt_event_handler, NULL);

    esp_err_t ret = esp_mqtt_client_start(g_mqtt_client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %d", ret);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
        vEventGroupDelete(g_mqtt_evt_group);
        return ret;
    }

    // Wait for connection
    xEventGroupWaitBits(g_mqtt_evt_group, MQTT_CONNECTED_BIT,
                         pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    // Send birth message
    if (config->lwt_online) {
        esp_mqtt_client_publish(g_mqtt_client, g_topic_lwt,
                                 config->lwt_online, 0, 1, 1);
    }

    g_initialized = true;
    ESP_LOGI(TAG, "MQTT initialized, broker: %s, prefix: %s",
             config->broker_uri, config->topic_prefix);

    return ESP_OK;
}

int airbnk_mqtt_deinit(void) {
    if (!g_initialized) return ESP_OK;
    g_initialized = false;

    if (g_mqtt_client) {
        esp_mqtt_client_stop(g_mqtt_client);
        esp_mqtt_client_destroy(g_mqtt_client);
        g_mqtt_client = NULL;
    }
    if (g_mqtt_evt_group) {
        vEventGroupDelete(g_mqtt_evt_group);
        g_mqtt_evt_group = NULL;
    }
    return ESP_OK;
}

int airbnk_mqtt_publish_adv(const char *json_str) {
    return mqtt_publish(g_topic_adv, json_str);
}

int airbnk_mqtt_publish_result(const char *json_str) {
    return mqtt_publish(g_topic_result, json_str);
}

bool airbnk_mqtt_is_connected(void) {
    if (g_mqtt_evt_group == NULL) return false;
    return (xEventGroupGetBits(g_mqtt_evt_group) & MQTT_CONNECTED_BIT) != 0;
}
