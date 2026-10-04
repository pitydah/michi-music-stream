#include "validators.h"

#include <string.h>

#define MICHI_UUID_LEN 36u

static bool uuid_dash_at(size_t i)
{
    return i == 8 || i == 13 || i == 18 || i == 23;
}

bool michi_uuid_valid(const char *id)
{
    if (id == NULL) {
        return false;
    }
    if (strlen(id) != MICHI_UUID_LEN) {
        return false;
    }
    for (size_t i = 0; i < MICHI_UUID_LEN; i++) {
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

bool michi_token_matches(const uint8_t *a, const uint8_t *b, size_t n)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    volatile uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc = (uint8_t)(acc | (uint8_t)(a[i] ^ b[i]));
    }
    return acc == 0;
}
