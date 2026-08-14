#pragma once
/* Test hooks for shim/michi_audio_fake.c. TEST-ONLY. */

#include <stdint.h>

#include "esp_err.h"
#include "michi_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The fake engine's observable state (includes the settable metrics). */
typedef struct {
    esp_err_t start_err;        /* injected start failure (0 = success) */
    int start_calls;
    int stop_calls;
    bool active;
    bool paused;
    uint16_t port_requested;
    uint32_t ssrc_requested;
    char source_ip_requested[16];
    uint16_t bound_port;
    uint16_t next_auto_port;
    michi_audio_metrics_t metrics;
} michi_audio_fake_state_t;

void test_michi_audio_reset(void);
void test_michi_audio_set_start_err(esp_err_t err);
michi_audio_fake_state_t *test_michi_audio_state(void);
michi_audio_metrics_t *test_michi_audio_metrics(void);

#ifdef __cplusplus
}
#endif
