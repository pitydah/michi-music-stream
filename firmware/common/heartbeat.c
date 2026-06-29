#include <esp_timer.h>
#include "esp_log.h"
#include "heartbeat.h"

static int64_t s_last = 0, s_start = 0;
static bool s_active = false;
static heartbeat_cb_t s_cb = NULL;
static void *s_ctx = NULL;

void heartbeat_init(void) { s_active = false; }
void heartbeat_set_callback(heartbeat_cb_t cb, void *ctx) { s_cb = cb; s_ctx = ctx; }

void heartbeat_reset(void)
{
    s_last = esp_timer_get_time() / 1000;
    if (!s_active) { s_start = s_last; s_active = true; }
}

void heartbeat_stop(void) { s_active = false; s_last = s_start = 0; }

bool heartbeat_is_active(void)
{
    if (!s_active) return false;
    if ((esp_timer_get_time() / 1000 - s_last) > HEARTBEAT_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Heartbeat timeout");
        s_active = false;
        if (s_cb) s_cb(s_ctx);
        return false;
    }
    return true;
}

uint64_t heartbeat_get_uptime_ms(void)
{
    if (!s_active || s_start == 0) return 0;
    return (uint64_t)((esp_timer_get_time() / 1000) - s_start);
}
