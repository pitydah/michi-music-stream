#pragma once
/* Shim for host-side tests: esp_log.h stand-in. Logs go to stderr.
 * TEST-ONLY: never compiled into firmware. */

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_LOGE(tag, fmt, ...) \
    fprintf(stderr, "E (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) \
    fprintf(stderr, "W (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) \
    fprintf(stderr, "I (%s) " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) \
    fprintf(stderr, "D (%s) " fmt "\n", tag, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif
