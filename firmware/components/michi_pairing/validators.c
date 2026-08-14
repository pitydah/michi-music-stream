/* Pure pairing validators shared with the host-side tests (F15).
 * No ESP-IDF dependencies on purpose: tests/host/test_pairing_validators.c
 * compiles this SAME source. */

#include "validators.h"

#include <string.h>

#define MICHI_PAIRING_PIN_LEN 6u
#define MICHI_PAIRING_UUID_LEN 36u

bool michi_pairing_pin_valid(const char *pin)
{
    if (pin == NULL) {
        return false;
    }
    if (strlen(pin) != MICHI_PAIRING_PIN_LEN) {
        return false;
    }
    for (size_t i = 0; i < MICHI_PAIRING_PIN_LEN; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return false;
        }
    }
    return true;
}

/* Position of each '-' in the canonical 8-4-4-4-12 grouping. */
static bool uuid_dash_at(size_t i)
{
    return i == 8 || i == 13 || i == 18 || i == 23;
}

bool michi_pairing_uuid_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }
    if (strlen(id) != MICHI_PAIRING_UUID_LEN) {
        return false;
    }
    for (size_t i = 0; i < MICHI_PAIRING_UUID_LEN; i++) {
        const char c = id[i];
        if (uuid_dash_at(i)) {
            if (c != '-') {
                return false;
            }
            continue;
        }
        const bool hex_low = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex_low) {
            return false;
        }
    }
    return true;
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
    if (a == NULL || b == NULL) {
        return false;
    }
    volatile uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (uint8_t)(acc | (uint8_t)(a[i] ^ b[i]));
    }
    return acc == 0;
}
