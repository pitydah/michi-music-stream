/* H19: verify the same explicit credential wiping helper used by firmware. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "michi_wifi_secret.h"

int main(void)
{
    uint8_t buffer[65];
    memset(buffer, 0xA5, sizeof(buffer));
    michi_wifi_wipe_stack_secret(buffer, sizeof(buffer));
    for (size_t i = 0; i < sizeof(buffer); ++i) {
        if (buffer[i] != 0) {
            fprintf(stderr, "credential wipe failed at byte %zu\n", i);
            return 1;
        }
    }
    /* The empty range must not disturb surrounding memory. */
    buffer[0] = 0x5A;
    michi_wifi_wipe_stack_secret(buffer, 0);
    if (buffer[0] != 0x5A) return 1;
    puts("H19 transient Wi-Fi secret wipe: PASS");
    return 0;
}
