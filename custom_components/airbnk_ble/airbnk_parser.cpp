/*
 * airbnk_parser.cpp — BABA Advertisement Parser implementation
 *
 * Parses manufacturer-specific BLE advertisement data from Airbnk locks.
 * Two advertisement types:
 *   Type 1 (barr[6] == 0xF0): older firmware — 18+ bytes
 *   Type 2 (barr[6] != 0xF0): newer firmware — 18 bytes
 *
 * ESP-IDF only — uses stdint.h, string.h. No Arduino dependencies.
 */
#include "airbnk_parser.h"
#include "esp_log.h"
#include <cstring>
#include <cstdio>

static const char *TAG = "airbnk_parser";

/* ── Type 1 advertisement parsing (barr[6] == 0xF0) ───────────────────── */

static bool parse_type1(const uint8_t *barr, size_t len,
                        const uint8_t mac[6], int8_t rssi,
                        airbnk_advert_data_t *out)
{
    // Type 1 needs at least 18 bytes
    if (len < 18) {
        ESP_LOGW(TAG, "Type 1 advert too short: %zu bytes (need 18)", len);
        return false;
    }

    out->is_type1 = true;

    // MAC address
    memcpy(out->mac_address, mac, 6);

    // Serial number — 9 bytes at offset 7 (not always null-terminated)
    memcpy(out->serial, &barr[7], 9);
    out->serial[9] = '\0';

    // lockEvents — 4 bytes big-endian at offset 10
    out->lock_events = ((uint32_t)barr[10] << 24) |
                       ((uint32_t)barr[11] << 16) |
                       ((uint32_t)barr[12] << 8)  |
                       ((uint32_t)barr[13]);

    // Voltage — 2 bytes at offset 14, scale 0.01
    uint16_t voltage_raw = ((uint16_t)barr[14]) | ((uint16_t)barr[15] << 8);
    out->voltage = (float)voltage_raw * 0.01f;

    // Status flags at barr[16]
    out->is_back_lock      = (barr[16] >> 0) & 1;
    out->is_init           = (barr[16] >> 1) & 1;
    // is_image_a          = (barr[16] >> 2) & 1;
    // is_had_new_record   = (barr[16] >> 3) & 1;
    uint8_t state_raw      = (barr[16] >> 4) & 7;
    out->is_enable_auto    = (barr[16] >> 7) & 1;

    // Status flags at barr[17]
    out->is_low_battery    = (barr[17] >> 4) & 1;

    // Map state
    out->state = airbnk_map_state_type1(state_raw);

    // Type 1: opening direction not available
    out->opens_clockwise = true;

    out->rssi = rssi;

    ESP_LOGD(TAG, "Type1: serial=%s events=%lu volt=%.2fV state=%d low_bat=%d rssi=%d",
             out->serial, (unsigned long)out->lock_events,
             (double)out->voltage, (int)out->state,
             (int)out->is_low_battery, (int)rssi);

    return true;
}

/* ── Type 2 advertisement parsing (barr[6] != 0xF0) ────────────────────── */

static bool parse_type2(const uint8_t *barr, size_t len,
                        const uint8_t mac[6], int8_t rssi,
                        airbnk_advert_data_t *out)
{
    // Type 2 needs at least 18 bytes
    if (len < 18) {
        ESP_LOGW(TAG, "Type 2 advert too short: %zu bytes (need 18)", len);
        return false;
    }

    out->is_type1 = false;

    // MAC address
    memcpy(out->mac_address, mac, 6);

    // Serial number — Type 2 doesn't have serial in advert; copy last 6 bytes of MAC as ID
    snprintf(out->serial, sizeof(out->serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // lockEvents — 4 bytes big-endian at offset 8
    out->lock_events = ((uint32_t)barr[8]  << 24) |
                       ((uint32_t)barr[9]  << 16) |
                       ((uint32_t)barr[10] << 8)  |
                       ((uint32_t)barr[11]);

    // Voltage — 1 byte at offset 16, scale 0.1
    out->voltage = (float)barr[16] * 0.1f;

    // Status flags at barr[17]
    out->is_back_lock      = (barr[17] >> 0) & 1;
    out->is_init           = (barr[17] >> 1) & 1;
    // is_image_a          = (barr[17] >> 2) & 1;
    // is_had_new_record   = (barr[17] >> 3) & 1;
    uint8_t state_raw      = (barr[17] >> 4) & 3;
    out->is_enable_auto    = (barr[17] >> 6) & 1;
    out->opens_clockwise   = !((barr[17] >> 7) & 1);  // negated!

    // Type 2 has no low_battery flag in this byte — use voltage threshold
    out->is_low_battery = (out->voltage < 3.5f);

    // Map state
    out->state = airbnk_map_state_type2(state_raw);

    out->rssi = rssi;

    ESP_LOGD(TAG, "Type2: serial=%s events=%lu volt=%.2fV state=%d low_bat=%d rssi=%d clockwise=%d",
             out->serial, (unsigned long)out->lock_events,
             (double)out->voltage, (int)out->state,
             (int)out->is_low_battery, (int)rssi,
             (int)out->opens_clockwise);

    return true;
}

/* ── Public API ────────────────────────────────────────────────────────── */

bool airbnk_parse_advertisement(
    const uint8_t *data, size_t len,
    const uint8_t mac[6], int8_t rssi,
    airbnk_advert_data_t *out)
{
    if (data == nullptr || out == nullptr || mac == nullptr) {
        return false;
    }

    // Check BABA magic: first two bytes must be 0xBA 0xBA
    if (len < 2 || data[0] != 0xBA || data[1] != 0xBA) {
        return false;
    }

    // Initialize output to zero
    memset(out, 0, sizeof(*out));
    out->state = AIRBNK_STATE_UNKNOWN;

    // Determine advertisement type by barr[6]
    // Type 1: barr[6] == 0xF0 (240 decimal)
    // Type 2: anything else
    if (len > 6 && data[6] == 0xF0) {
        return parse_type1(data, len, mac, rssi, out);
    } else {
        return parse_type2(data, len, mac, rssi, out);
    }
}

/* ── State mapping ─────────────────────────────────────────────────────── */

airbnk_lock_state_t airbnk_map_state_type1(uint8_t state_raw)
{
    // Type 1: (barr[16] >> 4) & 7
    // 0 or 5 → unlocked
    // 1 or 4 → locked
    // other  → jammed
    switch (state_raw) {
        case 0:
        case 5:
            return AIRBNK_STATE_UNLOCKED;
        case 1:
        case 4:
            return AIRBNK_STATE_LOCKED;
        default:
            return AIRBNK_STATE_JAMMED;
    }
}

airbnk_lock_state_t airbnk_map_state_type2(uint8_t state_raw)
{
    // Type 2: (barr[17] >> 4) & 3
    // 0 → unlocked
    // 1 → locked
    // 2 → jammed
    // 3 → operating
    switch (state_raw) {
        case 0: return AIRBNK_STATE_UNLOCKED;
        case 1: return AIRBNK_STATE_LOCKED;
        case 2: return AIRBNK_STATE_JAMMED;
        case 3: return AIRBNK_STATE_OPERATING;
        default: return AIRBNK_STATE_UNKNOWN;
    }
}

const char *airbnk_state_to_string(airbnk_lock_state_t state)
{
    switch (state) {
        case AIRBNK_STATE_UNLOCKED:  return "UNLOCKED";
        case AIRBNK_STATE_LOCKED:    return "LOCKED";
        case AIRBNK_STATE_JAMMED:    return "JAMMED";
        case AIRBNK_STATE_OPERATING: return "OPERATING";
        case AIRBNK_STATE_FAILED:    return "FAILED";
        default:                     return "UNKNOWN";
    }
}
