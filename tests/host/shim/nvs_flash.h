#pragma once
/* Shim for host-side tests: nvs_flash stand-in (whole-partition erase).
 * Erases the shared fake NVS store (test_nvs_reset) and counts calls so
 * the button factory-reset wiring is assertable. TEST-ONLY: never
 * compiled into firmware. */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- test hooks --- */

int test_nvs_flash_erase_count(void);
void test_nvs_flash_erase_count_reset(void);

/* --- fake nvs_flash API (used by the firmware sources under test) --- */

esp_err_t nvs_flash_erase(void);

#ifdef __cplusplus
}
#endif
