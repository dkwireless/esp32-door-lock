/*
 * airbnk_parser.h — BLE Advertisement Parser for Airbnk locks
 *
 * Parses BABA manufacturer data from BLE advertisements.
 * Supports Type 1 (barr[6] == 240) and Type 2 (barr[6] != 240).
 * ESP-IDF only — no Arduino dependencies.
 */
#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/// Lock state enumeration matching Home Assistant lock states
typedef enum {
    AIRBNK_STATE_UNLOCKED  = 0,
    AIRBNK_STATE_LOCKED    = 1,
    AIRBNK_STATE_JAMMED    = 2,
    AIRBNK_STATE_OPERATING = 3,
    AIRBNK_STATE_FAILED    = 4,
    AIRBNK_STATE_UNKNOWN   = 0xFF
} airbnk_lock_state_t;

/// Parsed advertisement data structure
typedef struct {
    uint8_t  mac_address[6];    ///< BLE MAC address
    char     serial[16];         ///< Serial number (C-string, null-terminated)
    uint32_t lock_events;        ///< Monotonic event counter (CRITICAL for HMAC signing)
    float    voltage;            ///< Battery voltage (V)
    uint8_t  battery_pct;        ///< Calculated battery percentage
    airbnk_lock_state_t state;   ///< Current lock state
    bool     is_low_battery;     ///< Low battery flag
    bool     is_init;            ///< Lock initialized flag
    bool     is_back_lock;       ///< Back lock flag
    bool     is_enable_auto;     ///< Auto-lock enabled flag
    bool     opens_clockwise;    ///< Opening direction (Type 2 only)
    int8_t   rssi;               ///< Signal strength (dBm)
    bool     is_type1;           ///< true = Type 1, false = Type 2
    uint32_t timestamp_ms;       ///< Reception timestamp (milliseconds)
} airbnk_advert_data_t;

/**
 * @brief Parse a BABA advertisement packet
 *
 * @param data        Raw manufacturer data bytes
 * @param len         Length of manufacturer data
 * @param mac         6-byte BLE MAC address
 * @param rssi        RSSI value in dBm
 * @param out         Output parsed data structure
 * @return true       If BABA header found and parsed successfully
 * @return false      If not a valid Airbnk BABA packet
 */
bool airbnk_parse_advertisement(
    const uint8_t *data, size_t len,
    const uint8_t mac[6], int8_t rssi,
    airbnk_advert_data_t *out
);

/**
 * @brief Map Type 1 raw state byte to lock state enum
 * @param state_raw  (barr[16] >> 4) & 7
 */
airbnk_lock_state_t airbnk_map_state_type1(uint8_t state_raw);

/**
 * @brief Map Type 2 raw state byte to lock state enum
 * @param state_raw  (barr[17] >> 4) & 3
 */
airbnk_lock_state_t airbnk_map_state_type2(uint8_t state_raw);

/**
 * @brief Convert lock state to human-readable string
 */
const char *airbnk_state_to_string(airbnk_lock_state_t state);

#ifdef __cplusplus
}
#endif
