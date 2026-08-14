/* Fake esp_timer for host-side tests (see esp_timer.h). TEST-ONLY. */

#include "esp_timer.h"

#include <string.h>

struct test_esp_timer {
    test_esp_timer_cb_t callback;
    void *arg;
    int64_t deadline_us;
    bool armed;
};

static int64_t s_now_us;
static struct test_esp_timer *s_timer; /* single-timer model */

int64_t test_esp_timer_now(void)
{
    return s_now_us;
}

void test_esp_timer_reset(void)
{
    s_now_us = 0;
    if (s_timer != NULL) {
        s_timer->armed = false;
        s_timer->deadline_us = 0;
    }
}

void test_esp_timer_set_time(int64_t us)
{
    s_now_us = us;
}

void test_esp_timer_advance(int64_t us)
{
    s_now_us += us;
    if (s_timer != NULL && s_timer->armed &&
        s_now_us >= s_timer->deadline_us) {
        s_timer->armed = false;
        s_timer->callback(s_timer->arg);
    }
}

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out)
{
    if (args == NULL || out == NULL || args->callback == NULL ||
        s_timer != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_timer = (struct test_esp_timer *)calloc(1, sizeof(*s_timer));
    if (s_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_timer->callback = args->callback;
    s_timer->arg = args->arg;
    *out = s_timer;
    return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    timer->deadline_us = s_now_us + (int64_t)timeout_us;
    timer->armed = true;
    return ESP_OK;
}

esp_err_t esp_timer_restart(esp_timer_handle_t timer, uint64_t timeout_us)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    timer->deadline_us = s_now_us + (int64_t)timeout_us;
    timer->armed = true;
    return ESP_OK;
}

bool esp_timer_is_active(esp_timer_handle_t timer)
{
    return timer != NULL && timer->armed;
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    timer->armed = false;
    return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer)
{
    if (timer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (timer == s_timer) {
        s_timer = NULL;
    }
    free(timer);
    return ESP_OK;
}
