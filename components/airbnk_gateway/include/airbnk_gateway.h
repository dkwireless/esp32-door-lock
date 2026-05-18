#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Airbnk Gateway configuration.
 * Provides everything needed to bridge BLE lock <-> MQTT.
 */
typedef struct {
    // Lock identification
    const char *lock_mac;          // Airbnk lock MAC address (e.g. "E4:E1:12:C7:5C:52")

    // MQTT settings
    const char *mqtt_broker_uri;   // e.g. "mqtt://192.168.6.11"
    const char *mqtt_username;     // NULL if none
    const char *mqtt_password;     // NULL if none
    const char *mqtt_topic_prefix; // e.g. "tele/maindoor"

    // BLE scan settings (optional, defaults used if 0)
    uint16_t scan_interval;        // Default: 0x80
    uint16_t scan_window;          // Default: 0x40
} airbnk_gateway_config_t;

/**
 * Initialize and start the Airbnk BLE-to-MQTT gateway.
 *
 * This initializes BLE, connects to MQTT, starts scanning for the lock,
 * and begins bridging advertisements and commands.
 *
 * @param config Gateway configuration
 * @return ESP_OK on success
 */
int airbnk_gateway_start(const airbnk_gateway_config_t *config);

/**
 * Stop the gateway and release resources.
 */
int airbnk_gateway_stop(void);

#ifdef __cplusplus
}
#endif
