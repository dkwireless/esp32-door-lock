#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback type for BLE advertisement reception.
 *
 * @param mac        Uppercase MAC address string (e.g. "AA:BB:CC:DD:EE:FF")
 * @param rssi       Signal strength in dBm
 * @param manuf_data Raw manufacturer data bytes
 * @param data_len   Length of manufacturer data
 * @param user_ctx   User context pointer passed to airbnk_ble_init()
 */
typedef void (*airbnk_ble_adv_cb_t)(const char *mac, int rssi,
                                    const uint8_t *manuf_data, int data_len,
                                    void *user_ctx);

/**
 * Callback type for command execution result.
 *
 * @param success   Whether the command was sent and acknowledged
 * @param status    Status bytes read from FFF3 characteristic (as hex string, or NULL)
 * @param user_ctx  User context pointer
 */
typedef void (*airbnk_ble_cmd_result_cb_t)(bool success, const char *status_hex,
                                           void *user_ctx);

/**
 * BLE gateway configuration.
 */
typedef struct {
    const char *target_mac;          // Target lock MAC address (uppercase)
    uint16_t    scan_interval;       // BLE scan interval (default: 0x80 = 128 * 0.625ms = 80ms)
    uint16_t    scan_window;         // BLE scan window   (default: 0x40 = 64 * 0.625ms = 40ms)
    bool        active_scan;         // Use active scanning (default: false, passive)
    airbnk_ble_adv_cb_t       adv_callback;       // Called on matching advertisement
    airbnk_ble_cmd_result_cb_t cmd_result_callback; // Called after command execution
    void       *user_ctx;            // Passed through to callbacks
} airbnk_ble_config_t;

/**
 * Initialize the BLE subsystem (NimBLE host stack).
 * Must be called before any other BLE functions.
 *
 * @param config BLE configuration
 * @return ESP_OK on success
 */
int airbnk_ble_init(const airbnk_ble_config_t *config);

/**
 * Deinitialize BLE subsystem.
 */
int airbnk_ble_deinit(void);

/**
 * Start BLE scanning for lock advertisements.
 * Called automatically by airbnk_ble_init().
 */
int airbnk_ble_start_scan(void);

/**
 * Stop BLE scanning.
 */
int airbnk_ble_stop_scan(void);

/**
 * Send a command (two hex frames) to the lock via BLE GATT.
 * Blocks until the operation completes (with retries).
 *
 * @param command1_hex First command frame as hex string (up to 40 chars = 20 bytes)
 * @param command2_hex Second command frame as hex string
 * @param sign         Sign value reported back in the result callback
 * @return ESP_OK on success
 */
int airbnk_ble_send_command(const char *command1_hex, const char *command2_hex, int sign);

/**
 * Check if a command is currently in progress.
 */
bool airbnk_ble_is_busy(void);

#ifdef __cplusplus
}
#endif
