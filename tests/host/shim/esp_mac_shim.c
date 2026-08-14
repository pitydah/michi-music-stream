/* Shim for host-side tests: esp_mac stand-in (see esp_mac.h).
 * TEST-ONLY. */

#include "esp_mac.h"

esp_err_t esp_read_mac(uint8_t *mac, int type)
{
    (void)type;
    static const uint8_t fake[6] = { 0x02, 0x00, 0x00, 0xde, 0xad, 0x01 };
    for (int i = 0; i < 6; i++) {
        mac[i] = fake[i];
    }
    return ESP_OK;
}
