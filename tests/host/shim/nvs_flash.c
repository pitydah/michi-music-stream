/* Fake nvs_flash_erase for host-side tests (see nvs_flash.h).
 * TEST-ONLY. */

#include "nvs_flash.h"

#include "nvs.h" /* fake NVS shim: the shared in-RAM store */

static int s_erase_count;

int test_nvs_flash_erase_count(void)
{
    return s_erase_count;
}

void test_nvs_flash_erase_count_reset(void)
{
    s_erase_count = 0;
}

esp_err_t nvs_flash_erase(void)
{
    test_nvs_reset(); /* the whole fake partition */
    s_erase_count++;
    return ESP_OK;
}
