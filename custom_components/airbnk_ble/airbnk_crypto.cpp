/*
 * airbnk_crypto.cpp — AES-128-ECB + HMAC-SHA1 implementation via mbedtls
 *
 * All crypto uses the ESP-IDF built-in mbedtls library with
 * hardware acceleration enabled on ESP32-C3.
 *
 * ESP-IDF only — no Arduino dependencies.
 */
#include "airbnk_crypto.h"
#include "esp_log.h"
#include "aes/esp_aes.h"
#include "mbedtls/aes.h"       /* MBEDTLS_AES_ENCRYPT / MBEDTLS_AES_DECRYPT */
#include "mbedtls/md.h"
#include <cstring>

static const char *TAG = "airbnk_crypto";

/* ── AES-128-ECB ───────────────────────────────────────────────────────── */

int airbnk_aes_128_ecb_encrypt(
    const uint8_t key[16],
    const uint8_t *input, size_t input_len,
    uint8_t output[16])
{
    if (key == nullptr || input == nullptr || output == nullptr || input_len > 16) {
        return -1;
    }

    esp_aes_context aes;
    esp_aes_init(&aes);

    int ret = esp_aes_setkey(&aes, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "AES setkey failed: %d", ret);
        esp_aes_free(&aes);
        return ret;
    }

    uint8_t block[16];
    memcpy(block, input, input_len);

    // PKCS#7 padding
    if (input_len < 16) {
        uint8_t pad_val = (uint8_t)(16 - input_len);
        memset(block + input_len, pad_val, pad_val);
    }

    ret = esp_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, block, output);
    esp_aes_free(&aes);

    if (ret != 0) {
        ESP_LOGE(TAG, "AES encrypt failed: %d", ret);
    }
    return ret;
}

int airbnk_aes_128_ecb_decrypt(
    const uint8_t key[16],
    const uint8_t input[16],
    uint8_t output[16])
{
    if (key == nullptr || input == nullptr || output == nullptr) {
        return -1;
    }

    esp_aes_context aes;
    esp_aes_init(&aes);

    int ret = esp_aes_setkey(&aes, key, 128);
    if (ret != 0) {
        ESP_LOGE(TAG, "AES setkey failed: %d", ret);
        esp_aes_free(&aes);
        return ret;
    }

    ret = esp_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, input, output);
    esp_aes_free(&aes);

    if (ret != 0) {
        ESP_LOGE(TAG, "AES decrypt failed: %d", ret);
    }
    return ret;
}

/* ── HMAC-SHA1 / SHA1 ──────────────────────────────────────────────────── */

int airbnk_hmac_sha1(
    const uint8_t *key, size_t key_len,
    const uint8_t *data, size_t data_len,
    uint8_t output[20])
{
    if (key == nullptr || data == nullptr || output == nullptr) {
        return -1;
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (md_info == nullptr) {
        ESP_LOGE(TAG, "SHA1 not available in mbedtls");
        return -1;
    }

    int ret = mbedtls_md_hmac(md_info, key, key_len, data, data_len, output);
    if (ret != 0) {
        ESP_LOGE(TAG, "HMAC-SHA1 failed: %d", ret);
    }
    return ret;
}

int airbnk_sha1_hash(
    const uint8_t *data, size_t data_len,
    uint8_t output[20])
{
    if (data == nullptr || output == nullptr) {
        return -1;
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (md_info == nullptr) {
        ESP_LOGE(TAG, "SHA1 not available in mbedtls");
        return -1;
    }

    int ret = mbedtls_md(md_info, data, data_len, output);
    if (ret != 0) {
        ESP_LOGE(TAG, "SHA1 failed: %d", ret);
    }
    return ret;
}

/* ── Airbnk-specific crypto ────────────────────────────────────────────── */

void airbnk_generate_working_key(
    const uint8_t binding_key[20],
    uint32_t counter,
    uint8_t output[20])
{
    // Manual HMAC implementation for working key derivation
    // ipad = binding_key XOR 0x36
    // opad = binding_key XOR 0x5C

    uint8_t ipad_key[20];
    uint8_t opad_key[20];
    for (int i = 0; i < 20; i++) {
        ipad_key[i] = binding_key[i] ^ 0x36;
        opad_key[i] = binding_key[i] ^ 0x5C;
    }

    // Convert counter to little-endian bytes
    uint8_t counter_le[4];
    counter_le[0] = (uint8_t)(counter & 0xFF);
    counter_le[1] = (uint8_t)((counter >> 8) & 0xFF);
    counter_le[2] = (uint8_t)((counter >> 16) & 0xFF);
    counter_le[3] = (uint8_t)((counter >> 24) & 0xFF);

    // Step 1: SHA1(ipad_key || counter_le[4])
    uint8_t inner_data[24];  // 20 + 4
    memcpy(inner_data, ipad_key, 20);
    memcpy(inner_data + 20, counter_le, 4);

    uint8_t inner_hash[20];
    int ret = airbnk_sha1_hash(inner_data, 24, inner_hash);
    if (ret != 0) {
        memset(output, 0, 20);
        return;
    }

    // Step 2: SHA1(opad_key || inner_hash)
    uint8_t outer_data[40];  // 20 + 20
    memcpy(outer_data, opad_key, 20);
    memcpy(outer_data + 20, inner_hash, 20);

    airbnk_sha1_hash(outer_data, 40, output);
}

void airbnk_generate_signature_v2(
    const uint8_t working_key[20],
    uint32_t lock_events,
    const uint8_t data[17],
    uint8_t output[8])
{
    // Prepare HMAC input: lock_events (4 bytes LE) || data (17 bytes)
    uint8_t hmac_input[21];  // 4 + 17
    hmac_input[0] = (uint8_t)(lock_events & 0xFF);
    hmac_input[1] = (uint8_t)((lock_events >> 8) & 0xFF);
    hmac_input[2] = (uint8_t)((lock_events >> 16) & 0xFF);
    hmac_input[3] = (uint8_t)((lock_events >> 24) & 0xFF);
    memcpy(hmac_input + 4, data, 17);

    uint8_t hmac_result[20];
    int ret = airbnk_hmac_sha1(working_key, 20, hmac_input, 21, hmac_result);
    if (ret != 0) {
        memset(output, 0, 8);
        return;
    }

    // Take bytes 2-9 of HMAC result as signature
    airbnk_generate_psw_v2(hmac_result, output);
}

void airbnk_generate_psw_v2(
    const uint8_t hash[20],
    uint8_t output[8])
{
    // XOR-fold 20-byte hash into 8 bytes
    // output[0:4] = hash[0:4] ^ hash[4:8] ^ hash[8:12] ^ hash[12:16] ^ hash[16:20]
    // output[4:8] = hash[2:6] ^ hash[6:10] ^ hash[10:14] ^ hash[14:18]

    for (int i = 0; i < 4; i++) {
        output[i] = hash[i] ^ hash[i + 4] ^ hash[i + 8] ^ hash[i + 12] ^ hash[i + 16];
    }
    for (int i = 0; i < 4; i++) {
        output[i + 4] = hash[i + 2] ^ hash[i + 6] ^ hash[i + 10] ^ hash[i + 14];
    }
}

void airbnk_make_package_v3(
    uint8_t lock_dir,
    uint32_t timestamp,
    uint32_t lock_events,
    const uint8_t manufacturer_key[16],
    const uint8_t binding_key[20],
    uint8_t output[36])
{
    memset(output, 0, 36);

    // Byte 0: start marker
    output[0] = 0xAA;

    // Byte 1: command
    output[1] = 0x10;

    // Byte 2: sub-command
    output[2] = 0x1A;

    // Byte 3: version high
    output[3] = 3;

    // ── Build plaintext payload (14 bytes) for AES-ECB ──
    uint8_t plain[14];
    memset(plain, 0, 14);
    plain[0] = 3;                          // version_l
    plain[1] = 16 + lock_dir;              // 17 = unlock, 18 = lock
    // plain[2:4] = padding (0x00)
    plain[4] = 1;                          // op_mode = 1
    // Timestamp (big-endian, 4 bytes at offset 5)
    plain[5] = (uint8_t)((timestamp >> 24) & 0xFF);
    plain[6] = (uint8_t)((timestamp >> 16) & 0xFF);
    plain[7] = (uint8_t)((timestamp >> 8) & 0xFF);
    plain[8] = (uint8_t)(timestamp & 0xFF);
    // plain[9:13] = padding (0x00)
    // Byte 13: PKCS#7 padding value (2 for 14→16)
    plain[13] = 2;

    // AES-ECB encrypt: plain[14] + PKCS#7 pad → 16 bytes → output[4:20]
    airbnk_aes_128_ecb_encrypt(manufacturer_key, plain, 14, &output[4]);

    // ── Generate working key from binding_key + lock_events ──
    uint8_t working_key[20];
    airbnk_generate_working_key(binding_key, lock_events, working_key);

    // ── Generate signature (8 bytes) ──
    // Sign code[3:20] = output[3:20] = 17 bytes
    uint8_t signature[8];
    airbnk_generate_signature_v2(working_key, lock_events, &output[3], signature);
    memcpy(&output[20], signature, 8);

    // ── Checksum (byte 28): XOR of code[3:28] ──
    uint8_t checksum = 0;
    for (int i = 3; i < 28; i++) {
        checksum ^= output[i];
    }
    output[28] = checksum;

    // Bytes 29-35: padding (already zeroed)

    ESP_LOGD(TAG, "Package V3: dir=%d ts=%lu events=%lu",
             (int)lock_dir, (unsigned long)timestamp, (unsigned long)lock_events);
}
