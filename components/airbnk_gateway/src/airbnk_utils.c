#include "airbnk_utils.h"
#include <string.h>
#include <ctype.h>

static const char hex_chars[] = "0123456789ABCDEF";

int airbnk_from_hex(uint8_t *dest, const char *src, int maxlen) {
    int srclen = (int)strlen(src);
    if (srclen % 2 != 0) return 0;  // Must be even length

    int outlen = srclen / 2;
    if (outlen > maxlen) return 0;

    memset(dest, 0, maxlen);

    for (int i = 0; i < outlen; i++) {
        char high = src[i * 2];
        char low  = src[i * 2 + 1];

        // Handle '/' as '0' (legacy compatibility from airbnk_mqtt)
        if (high == '/') high = '0';
        if (low == '/')  low  = '0';

        if (!isxdigit((unsigned char)high) || !isxdigit((unsigned char)low)) {
            return 0;
        }

        unsigned int byte;
        char tmp[3] = {high, low, '\0'};
        if (sscanf(tmp, "%2x", &byte) != 1) return 0;
        dest[i] = (uint8_t)byte;
    }
    return outlen;
}

void airbnk_to_hex(char *dest, const uint8_t *src, int len) {
    for (int i = 0; i < len; i++) {
        dest[i * 2]     = hex_chars[(src[i] >> 4) & 0x0F];
        dest[i * 2 + 1] = hex_chars[src[i] & 0x0F];
    }
    dest[len * 2] = '\0';
}

void airbnk_mac_uppercase(char *mac) {
    for (int i = 0; mac[i]; i++) {
        mac[i] = (char)toupper((unsigned char)mac[i]);
    }
}

bool airbnk_mac_equals(const char *a, const char *b) {
    if (!a || !b) return false;
    // Case-insensitive compare
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}
