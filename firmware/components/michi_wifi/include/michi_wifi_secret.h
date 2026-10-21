#pragma once
/* H19: overwrite transient copies of Wi-Fi credentials in host/firmware.
 * Volatile byte stores avoid dead-store elimination of a final memset.
 * This is not a claim of whole-device RAM sanitization: the Wi-Fi driver
 * retains its authorized configuration separately. */
#include <stddef.h>
#include <stdint.h>

static inline void michi_wifi_wipe_stack_secret(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len-- > 0) {
        *p++ = 0;
    }
}
