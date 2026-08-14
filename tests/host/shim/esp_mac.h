#pragma once
/* Shim for host-side tests: esp_mac.h stand-in for esp_read_mac()
 * (michi_discovery uses the STA MAC for the mDNS hostname).
 * Deterministic MAC: 02:00:00:de:ad:01. TEST-ONLY. */

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_MAC_WIFI_STA 1

esp_err_t esp_read_mac(uint8_t *mac, int type);

#ifdef __cplusplus
}
#endif
