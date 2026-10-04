#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void test_michi_home_fake_set_provisioned(bool provisioned, const char *home_id);

#ifdef __cplusplus
}
#endif
