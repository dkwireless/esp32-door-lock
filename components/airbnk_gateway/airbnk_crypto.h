#ifndef AIRBNK_CRYPTO_H
#define AIRBNK_CRYPTO_H

#ifndef MBEDTLS_CONFIG_FILE
#define MBEDTLS_CONFIG_FILE "mbedtls/esp_config.h"
#endif

#include <cstring>
#include <cstdint>
#include <array>
#include <string>
#include "mbedtls/aes.h"
#include "mbedtls/md.h"
#include "esp_log.h"

static const char *AIRBNK_CRYPTO_TAG = "airbnk_crypto";

class AirbnkCrypto {
public:
    static void XOR64Buffer(uint8_t *arr, uint8_t value) {
        for (int i = 0; i < 64; i++) {
            arr[i] ^= value;
        }
    }

    static std::array<uint8_t, 20> generateWorkingKey(const uint8_t *key, int keyLen, uint32_t counter) {
        // arr2 = bytearray(72): key[0:keyLen] + 0x36 XOR'd + counter at end
        uint8_t arr2[72];
        memset(arr2, 0, 72);
        memcpy(arr2, key, (keyLen < 64) ? keyLen : 64);
        XOR64Buffer(arr2, 0x36);
        arr2[71] = counter & 0xFF;
        arr2[70] = (counter >> 8) & 0xFF;
        arr2[69] = (counter >> 16) & 0xFF;
        arr2[68] = (counter >> 24) & 0xFF;

        uint8_t sha1_out[20];
        mbedtls_md_context_t md_ctx;
        mbedtls_md_init(&md_ctx);
        mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0);
        mbedtls_md_starts(&md_ctx);
        mbedtls_md_update(&md_ctx, arr2, 72);
        mbedtls_md_finish(&md_ctx, sha1_out);
        mbedtls_md_free(&md_ctx);

        // arr3 = bytearray(84): key[0:64] XOR 0x5C + sha1_out[0:20]
        uint8_t arr3[84];
        memset(arr3, 0, 84);
        memcpy(arr3, key, (keyLen < 64) ? keyLen : 64);
        XOR64Buffer(arr3, 0x5C);
        memcpy(arr3 + 64, sha1_out, 20);

        std::array<uint8_t, 20> result;
        mbedtls_md_init(&md_ctx);
        mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0);
        mbedtls_md_starts(&md_ctx);
        mbedtls_md_update(&md_ctx, arr3, 84);
        mbedtls_md_finish(&md_ctx, result.data());
        mbedtls_md_free(&md_ctx);

        return result;
    }

    static std::array<uint8_t, 8> generatePswV2(const uint8_t *sha1_out) {
        // Take bytes [16..19] of SHA1, use each as index into the first 16 bytes of SHA1
        std::array<uint8_t, 8> result;
        result.fill(0);
        for (int i = 0; i < 4; i++) {
            uint8_t b = sha1_out[i + 16];
            int i2 = i * 2;
            result[i2] = sha1_out[(b >> 4) & 0x0F];
            result[i2 + 1] = sha1_out[b & 0x0F];
        }
        return result;
    }

    static std::array<uint8_t, 8> generateSignatureV2(const uint8_t *workingKey, uint32_t lockEvents,
                                                        const uint8_t *data, int dataLen) {
        int lenArr = dataLen;
        uint8_t *arr2 = new uint8_t[lenArr + 68];
        memset(arr2, 0, lenArr + 68);
        memcpy(arr2, workingKey, 20);
        XOR64Buffer(arr2, 0x36);
        memcpy(arr2 + 64, data, lenArr);
        arr2[lenArr + 67] = lockEvents & 0xFF;
        arr2[lenArr + 66] = (lockEvents >> 8) & 0xFF;
        arr2[lenArr + 65] = (lockEvents >> 16) & 0xFF;
        arr2[lenArr + 64] = (lockEvents >> 24) & 0xFF;

        uint8_t sha1_out[20];
        mbedtls_md_context_t md_ctx;
        mbedtls_md_init(&md_ctx);
        mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0);
        mbedtls_md_starts(&md_ctx);
        mbedtls_md_update(&md_ctx, arr2, lenArr + 68);
        mbedtls_md_finish(&md_ctx, sha1_out);
        mbedtls_md_free(&md_ctx);
        delete[] arr2;

        uint8_t arr3[84];
        memset(arr3, 0, 84);
        memcpy(arr3, workingKey, 20);
        XOR64Buffer(arr3, 0x5C);
        memcpy(arr3 + 64, sha1_out, 20);

        uint8_t sha1_out2[20];
        mbedtls_md_init(&md_ctx);
        mbedtls_md_setup(&md_ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), 0);
        mbedtls_md_starts(&md_ctx);
        mbedtls_md_update(&md_ctx, arr3, 84);
        mbedtls_md_finish(&md_ctx, sha1_out2);
        mbedtls_md_free(&md_ctx);

        return generatePswV2(sha1_out2);
    }

    static uint8_t getCheckSum(const uint8_t *data, int start, int end) {
        uint8_t c = 0;
        for (int i = start; i < end; i++) {
            c += data[i];
        }
        return c;
    }

    // AES-ECB encrypt 14 bytes (padded to 16 with PKCS7)
    static bool aesEncryptECB(const uint8_t *key, const uint8_t *input, int inputLen,
                               uint8_t *output, int *outputLen) {
        if (inputLen > 16) return false;

        uint8_t padded[16];
        memset(padded, 0, 16);
        memcpy(padded, input, inputLen);
        int padLen = 16 - inputLen;
        for (int i = inputLen; i < 16; i++) {
            padded[i] = padLen;
        }

        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        int ret = mbedtls_aes_setkey_enc(&aes, key, 128);
        if (ret != 0) {
            mbedtls_aes_free(&aes);
            ESP_LOGE(AIRBNK_CRYPTO_TAG, "AES setkey failed: %d", ret);
            return false;
        }

        ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, padded, output);
        mbedtls_aes_free(&aes);
        if (ret != 0) {
            ESP_LOGE(AIRBNK_CRYPTO_TAG, "AES ECB encrypt failed: %d", ret);
            return false;
        }

        *outputLen = 16;
        return true;
    }

    // AES-ECB decrypt
    static bool aesDecryptECB(const uint8_t *key, const uint8_t *input, int inputLen,
                               uint8_t *output, int *outputLen) {
        if (inputLen != 16) return false;

        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);
        int ret = mbedtls_aes_setkey_dec(&aes, key, 128);
        if (ret != 0) {
            mbedtls_aes_free(&aes);
            return false;
        }

        ret = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, input, output);
        mbedtls_aes_free(&aes);
        if (ret != 0) return false;

        // Remove PKCS7 padding
        uint8_t pad = output[15];
        if (pad > 0 && pad <= 16) {
            *outputLen = 16 - pad;
        } else {
            *outputLen = 16;
        }
        return true;
    }

    // Convert hex string to bytes
    static int fromHex(uint8_t *dest, const char *src, int maxLen) {
        int len = strlen(src);
        if (len > maxLen * 2) len = maxLen * 2;
        int count = 0;
        for (int i = 0; i + 1 < len; i += 2) {
            uint8_t high = 0, low = 0;
            char c = src[i];
            if (c >= '0' && c <= '9') high = c - '0';
            else if (c >= 'A' && c <= 'F') high = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') high = c - 'a' + 10;
            else high = 0;
            c = src[i + 1];
            if (c >= '0' && c <= '9') low = c - '0';
            else if (c >= 'A' && c <= 'F') low = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') low = c - 'a' + 10;
            else low = 0;
            dest[count++] = (high << 4) | low;
        }
        return count;
    }

    // Convert bytes to hex string
    static std::string toHex(const uint8_t *data, int len) {
        static const char hexChars[] = "0123456789ABCDEF";
        std::string result;
        result.reserve(len * 2);
        for (int i = 0; i < len; i++) {
            result += hexChars[(data[i] >> 4) & 0x0F];
            result += hexChars[data[i] & 0x0F];
        }
        return result;
    }
};

#endif // AIRBNK_CRYPTO_H
