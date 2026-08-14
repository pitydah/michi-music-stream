#pragma once
/* Shim for host-side tests: esp_timer stand-in with a FAKE monotonic
 * clock and a single one-shot timer (the pairing component owns exactly
 * one). Tests drive time with test_esp_timer_advance(), which fires the
 * timer callback synchronously when the deadline passes - exactly like
 * the esp_timer task would. TEST-ONLY: never compiled into firmware. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*test_esp_timer_cb_t)(void *arg);

typedef struct {
    test_esp_timer_cb_t callback;
    void *arg;
    const char *name;
} esp_timer_create_args_t;

typedef struct test_esp_timer *esp_timer_handle_t;

/* --- test hooks --- */

int64_t test_esp_timer_now(void);
void test_esp_timer_reset(void);
void test_esp_timer_set_time(int64_t us);

/* Advance the fake clock; the registered one-shot timer's callback fires
 * synchronously once its deadline passes. */
void test_esp_timer_advance(int64_t us);

/* --- fake esp_timer API (used by the firmware sources under test) --- */

int64_t esp_timer_get_time(void);

esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                           esp_timer_handle_t *out);

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);

esp_err_t esp_timer_stop(esp_timer_handle_t timer);

esp_err_t esp_timer_delete(esp_timer_handle_t timer);

#ifdef __cplusplus
}
#endif
