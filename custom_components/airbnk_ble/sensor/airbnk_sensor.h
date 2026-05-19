/*
 * sensor/airbnk_sensor.h — Airbnk sensor entity definitions
 *
 * Defines individual sensor entity classes for use within the
 * AirbnkBleComponent. These are optional — the main component
 * can also manage entity pointers directly.
 *
 * ESP-IDF only — no Arduino dependencies.
 */
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/lock/lock.h"

#ifdef __cplusplus
extern "C" {
#endif

// Individual sensor components are optional — the main AirbnkBleComponent
// can handle all entity management directly through entity pointers.
//
// If more complex sensor logic is needed (filters, averaging),
// individual classes can be added here and registered in __init__.py.

/// Helper: Lock state enum to string conversion (for text_sensor)
inline const char *airbnk_entity_state_to_string(uint8_t state)
{
    switch (state) {
        case 0: return "UNLOCKED";
        case 1: return "LOCKED";
        case 2: return "JAMMED";
        case 3: return "OPERATING";
        case 4: return "FAILED";
        default: return "UNKNOWN";
    }
}

/// Helper: Calculate battery percentage from voltage
/// @param voltage  Measured voltage in volts
/// @param v1       Low threshold (usually 3.5V)
/// @param v2       Mid threshold (usually 3.8V)
/// @param v3       High threshold (usually 4.1V)
/// @return         Battery percentage 0-100
inline uint8_t airbnk_entity_calc_battery_pct(float voltage,
                                               float v1, float v2, float v3)
{
    if (voltage <= v1) return 0;
    if (voltage >= v3) return 100;
    if (voltage >= v2) {
        return (uint8_t)(66.6f + 33.3f * (voltage - v2) / (v3 - v2));
    }
    return (uint8_t)(33.3f + 33.3f * (voltage - v1) / (v2 - v1));
}

#ifdef __cplusplus
}
#endif
