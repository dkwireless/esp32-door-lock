#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Convert hex string to byte array.
 * E.g. "AABBCC" -> {0xAA, 0xBB, 0xCC}
 *
 * @param dest   Output buffer (caller-allocated, at least maxlen bytes)
 * @param src    Null-terminated hex string
 * @param maxlen Maximum number of bytes to write
 * @return       Number of bytes decoded, or 0 on error
 */
int airbnk_from_hex(uint8_t *dest, const char *src, int maxlen);

/**
 * Convert byte array to hex string.
 * E.g. {0xAA, 0xBB, 0xCC} with len=3 -> "AABBCC"
 *
 * @param dest   Output buffer (caller-allocated, at least len*2 + 1 bytes)
 * @param src    Input byte array
 * @param len    Number of input bytes
 */
void airbnk_to_hex(char *dest, const uint8_t *src, int len);

/**
 * Convert a MAC address string to uppercase.
 * Modifies in-place.
 *
 * @param mac Null-terminated MAC string (e.g. "aa:bb:cc:dd:ee:ff")
 */
void airbnk_mac_uppercase(char *mac);

/**
 * Compare two MAC address strings case-insensitively.
 *
 * @return true if equal
 */
bool airbnk_mac_equals(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
