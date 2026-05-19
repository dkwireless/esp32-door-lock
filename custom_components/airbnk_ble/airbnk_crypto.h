/*
 * airbnk_crypto.h — AES-128-ECB + HMAC-SHA1 for Airbnk lock protocol
 *
 * Uses ESP-IDF built-in mbedtls (HW-accelerated on ESP32-C3).
 * Implements the Airbnk V3 command package generation.
 *
 * Key functions:
 *   aes_128_ecb_encrypt/decrypt  — single-block AES-ECB via mbedtls
 *   hmac_sha1 / sha1_hash        — HMAC and plain SHA1 via mbedtls
 *   generate_working_key         — bindingKey + counter → working key
 *   generate_signature_v2        — HMAC-based command signature
 *   make_package_v3              — complete 36-byte unlock/lock opcode
 *
 * ESP-IDF only — no Arduino dependencies.
 */
#pragma once

#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Low-level crypto ──────────────────────────────────────────────────── */

/**
 * @brief AES-128-ECB encrypt a single 16-byte block
 *
 * PKCS#7 padding is applied if input_len < 16.
 *
 * @param key         16-byte AES key
 * @param input       Plaintext (up to 16 bytes)
 * @param input_len   Plaintext length
 * @param output      16-byte ciphertext buffer
 * @return 0 on success, mbedtls error code otherwise
 */
int airbnk_aes_128_ecb_encrypt(
    const uint8_t key[16],
    const uint8_t *input, size_t input_len,
    uint8_t output[16]
);

/**
 * @brief AES-128-ECB decrypt a single 16-byte block
 *
 * @param key     16-byte AES key
 * @param input   16-byte ciphertext
 * @param output  16-byte plaintext buffer
 * @return 0 on success, mbedtls error code otherwise
 */
int airbnk_aes_128_ecb_decrypt(
    const uint8_t key[16],
    const uint8_t input[16],
    uint8_t output[16]
);

/**
 * @brief HMAC-SHA1 using mbedtls
 *
 * @param key       HMAC key
 * @param key_len   Key length in bytes
 * @param data      Data to sign
 * @param data_len  Data length in bytes
 * @param output    20-byte HMAC output
 * @return 0 on success
 */
int airbnk_hmac_sha1(
    const uint8_t *key, size_t key_len,
    const uint8_t *data, size_t data_len,
    uint8_t output[20]
);

/**
 * @brief Plain SHA1 hash using mbedtls
 *
 * @param data      Input data
 * @param data_len  Input length
 * @param output    20-byte SHA1 output
 * @return 0 on success
 */
int airbnk_sha1_hash(
    const uint8_t *data, size_t data_len,
    uint8_t output[20]
);

/* ── Airbnk protocol crypto ────────────────────────────────────────────── */

/**
 * @brief Generate working key from binding key and counter
 *
 * Airbnk uses a custom HMAC-SHA1-based key derivation:
 *   ipad_key = binding_key XOR 0x36 for each byte
 *   opad_key = binding_key XOR 0x5C for each byte
 *   hash = SHA1(ipad_key || counter_le[4])
 *   output = SHA1(opad_key || hash)
 *
 * @param binding_key  20-byte binding key
 * @param counter      32-bit counter (lock_events, little-endian)
 * @param output       20-byte derived working key
 */
void airbnk_generate_working_key(
    const uint8_t binding_key[20],
    uint32_t counter,
    uint8_t output[20]
);

/**
 * @brief Generate V2 signature for command code
 *
 * Uses HMAC-SHA1 with the working key over the command data.
 * The signature is derived from bytes 2-18 of the HMAC result.
 *
 * @param working_key   20-byte working key
 * @param lock_events   32-bit lock event counter (for HMAC context)
 * @param data          17-byte payload (code[3:20])
 * @param output        8-byte signature
 */
void airbnk_generate_signature_v2(
    const uint8_t working_key[20],
    uint32_t lock_events,
    const uint8_t data[17],
    uint8_t output[8]
);

/**
 * @brief Generate password V2 — final 8 signature bytes from SHA1 hash
 *
 * XOR folds a 20-byte SHA1 hash into 8 bytes.
 *
 * @param hash    20-byte SHA1 hash
 * @param output  8-byte derived password/signature
 */
void airbnk_generate_psw_v2(
    const uint8_t hash[20],
    uint8_t output[8]
);

/**
 * @brief Build complete 36-byte V3 command package
 *
 * The output is a 36-byte opcode ready for hex encoding and
 * splitting into FF00/FF01 frames.
 *
 * @param lock_dir          1 = unlock, 2 = lock
 * @param timestamp         Current Unix timestamp (big-endian)
 * @param lock_events       Current lock_events from advertisement
 * @param manufacturer_key  16-byte manufacturer AES key
 * @param binding_key       20-byte binding key for HMAC
 * @param output            36-byte command code buffer
 */
void airbnk_make_package_v3(
    uint8_t lock_dir,
    uint32_t timestamp,
    uint32_t lock_events,
    const uint8_t manufacturer_key[16],
    const uint8_t binding_key[20],
    uint8_t output[36]
);

#ifdef __cplusplus
}
#endif
