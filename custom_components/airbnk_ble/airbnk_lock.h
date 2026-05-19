/*
 * airbnk_lock.h — NimBLE BLE stack for Airbnk lock communication
 *
 * ESP-IDF NimBLE only. Manages: init, passive scan, GATT client.
 * NO Arduino BLE, NO Bluedroid, NO esp32_ble_tracker.
 */
#pragma once

#include "airbnk_parser.h"
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*airbnk_advert_cb_t)(const airbnk_advert_data_t *adv);
typedef void (*airbnk_cmd_done_t)(bool success);

/// Command to send to the lock
typedef struct {
    uint8_t  mac_address[6];
    uint8_t  opcode[36];
    airbnk_cmd_done_t on_done;
} airbnk_cmd_t;

/* ── NimBLE lifecycle ──────────────────────────────────────────────────── */

int  airbnk_nimble_init(void);
void airbnk_nimble_deinit(void);
int  airbnk_nimble_start_scan(airbnk_advert_cb_t cb);
void airbnk_nimble_stop_scan(void);

/* ── Command interface ─────────────────────────────────────────────────── */

int  airbnk_lock_send_command(const airbnk_cmd_t *cmd);
bool airbnk_lock_is_busy(void);
void airbnk_lock_cancel(void);

#ifdef __cplusplus
}
#endif
