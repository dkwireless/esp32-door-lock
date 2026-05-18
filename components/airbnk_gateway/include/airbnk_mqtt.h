#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback for incoming MQTT command messages.
 *
 * @param topic   MQTT topic the message arrived on
 * @param data    Raw payload
 * @param data_len Payload length
 * @param user_ctx User context pointer
 */
typedef void (*airbnk_mqtt_cmd_cb_t)(const char *topic, const char *data,
                                      int data_len, void *user_ctx);

/**
 * MQTT configuration.
 */
typedef struct {
    const char *broker_uri;        // MQTT broker URI (e.g. "mqtt://192.168.6.11")
    const char *username;          // MQTT username (NULL if none)
    const char *password;          // MQTT password (NULL if none)
    const char *topic_prefix;      // Topic prefix (e.g. "tele/maindoor")
    const char *lwt_topic;         // Last Will topic
    const char *lwt_online;        // Birth message payload
    const char *lwt_offline;       // Will message payload
    airbnk_mqtt_cmd_cb_t cmd_callback;
    void       *user_ctx;
} airbnk_mqtt_config_t;

/**
 * Initialize and start MQTT client.
 *
 * @return ESP_OK on success
 */
int airbnk_mqtt_init(const airbnk_mqtt_config_t *config);

/**
 * Stop and deinitialize MQTT client.
 */
int airbnk_mqtt_deinit(void);

/**
 * Publish a JSON message to the advertisement topic.
 *
 * @param json_str JSON payload string
 * @return ESP_OK on success
 */
int airbnk_mqtt_publish_adv(const char *json_str);

/**
 * Publish a JSON message to the command_result topic.
 *
 * @param json_str JSON payload string
 * @return ESP_OK on success
 */
int airbnk_mqtt_publish_result(const char *json_str);

/**
 * Check if MQTT is connected.
 */
bool airbnk_mqtt_is_connected(void);

#ifdef __cplusplus
}
#endif
