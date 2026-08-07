/* Pure pairing validators shared with the host-side tests (F15).
 * No ESP-IDF dependencies on purpose: tests/host/test_pairing_validators.c
 * compiles this SAME source. */

#include "validators.h"

#include <string.h>

bool michi_pairing_id_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }
    const size_t len = strlen(id);
    if (len == 0 || len > MICHI_PAIRING_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        const char c = id[i];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9');
        if (!alnum && c != '-') {
            return false;
        }
    }
    return true;
}

uint8_t michi_pairing_hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint8_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint8_t)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (uint8_t)(c - 'A' + 10);
    }
    return 0xff;
}

bool michi_pairing_hex_decode(const char *src, size_t src_len, uint8_t *dst,
                              size_t dst_len)
{
    if (src == NULL || dst == NULL || src_len != 2 * dst_len) {
        return false;
    }
    for (size_t i = 0; i < dst_len; i++) {
        const uint8_t hi = michi_pairing_hex_val(src[2 * i]);
        const uint8_t lo = michi_pairing_hex_val(src[2 * i + 1]);
        if (hi > 15 || lo > 15) {
            return false;
        }
        dst[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

void michi_pairing_hex_encode(const uint8_t *src, size_t len, char *dst)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        dst[2 * i] = digits[src[i] >> 4];
        dst[2 * i + 1] = digits[src[i] & 0x0f];
    }
    dst[2 * len] = '\0';
}

bool michi_pairing_token_matches(const uint8_t *a, const uint8_t *b, size_t n)
{
    /* Constant-time: every byte of both buffers is read on every call and
     * the XORs are accumulated (never branched on) into a volatile
     * accumulator - the volatile prevents the compiler from skipping or
     * reordering reads. Timing depends on n only, never on the data.
     *
     * F15 tradeoff: this volatile accumulator is the host-testable
     * replacement for mbedtls_ct_memcmp (used in production OTA/HTTP
     * paths, but it pulls mbedTLS into the host build). Production COULD
     * revert to mbedtls_ct_memcmp for stronger CT guarantees; the host
     * test cannot prove constant-time behavior anyway - it only checks
     * correctness of the comparison. Documented so the replacement is
     * not mistaken for a hardening regression. */
    volatile uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (uint8_t)(acc | (uint8_t)(a[i] ^ b[i]));
    }
    return acc == 0;
}
