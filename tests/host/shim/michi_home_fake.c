#include "michi_home_fake.h"
#include "michi_home.h"
#include <stdio.h>
#include <string.h>

static bool s_provisioned = false;
static char s_home_id[MICHI_HOME_ID_LEN] = "FU1FL-wFLfsfew3qpbR7XjDkmStWZY4g84MyW-zXPOs";

void test_michi_home_fake_set_provisioned(bool provisioned, const char *home_id)
{
    s_provisioned = provisioned;
    if (home_id != NULL) {
        snprintf(s_home_id, sizeof(s_home_id), "%s", home_id);
    }
}

__attribute__((weak)) bool michi_home_is_provisioned(void)
{
    return s_provisioned;
}

__attribute__((weak)) esp_err_t michi_home_get_id(char *out, size_t out_len)
{
    if (!s_provisioned) {
        return ESP_ERR_NOT_FOUND;
    }
    if (out == NULL || out_len < strlen(s_home_id) + 1) {
        return ESP_ERR_INVALID_SIZE;
    }
    snprintf(out, out_len, "%s", s_home_id);
    return ESP_OK;
}
