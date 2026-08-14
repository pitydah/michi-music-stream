/* Fake michi_audio engine for host-side tests: a controllable test
 * double implementing the REAL michi_audio.h (the public contract the
 * session layer compiles against - no struct drift). TEST-ONLY: never
 * compiled into firmware.
 *
 * The fake models the engine's observable contract:
 *  - all-or-nothing session_start: an injected start_err simulates
 *    bind/buffer/pipeline failures (ESP_FAIL, ESP_ERR_NO_MEM,
 *    ESP_ERR_INVALID_STATE); on success the session is "active";
 *  - port 0 = the receiver picks the next port from 49152 (wrap at
 *    65535) - the range contract, deterministic for tests;
 *  - pause is a flag (the real engine keeps the task alive);
 *  - metrics are a settable struct (the session layer's info mapping
 *    is tested by injecting values);
 *  - get_ssrc/get_peer return the negotiated values while active. */

#include "michi_audio.h"
#include "michi_audio_fake.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef michi_audio_fake_state_t fake_state_t;

static fake_state_t s_fake;

void test_michi_audio_reset(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    s_fake.next_auto_port = 49152;
    s_fake.metrics.received = 0;
}

fake_state_t *test_michi_audio_state(void)
{
    return &s_fake;
}

michi_audio_metrics_t *test_michi_audio_metrics(void)
{
    return &s_fake.metrics;
}

void test_michi_audio_set_start_err(esp_err_t err)
{
    s_fake.start_err = err;
}

esp_err_t michi_audio_session_start(uint16_t port, uint32_t ssrc,
                                    const char *source_ip)
{
    s_fake.start_calls++;
    if (s_fake.start_err != ESP_OK) {
        return s_fake.start_err; /* bind/buffer/pipeline failure */
    }
    if (ssrc == 0 || source_ip == NULL || source_ip[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    s_fake.port_requested = port;
    s_fake.ssrc_requested = ssrc;
    strncpy(s_fake.source_ip_requested, source_ip,
            sizeof(s_fake.source_ip_requested) - 1);
    if (port == 0) {
        s_fake.bound_port = s_fake.next_auto_port;
        s_fake.next_auto_port =
            s_fake.next_auto_port == 65535 ? 49152
                                           : (uint16_t)(s_fake.next_auto_port + 1);
    } else {
        s_fake.bound_port = port;
    }
    memset(&s_fake.metrics, 0, sizeof(s_fake.metrics));
    s_fake.paused = false;
    s_fake.active = true;
    return ESP_OK;
}

esp_err_t michi_audio_session_stop(void)
{
    s_fake.stop_calls++;
    s_fake.active = false;
    s_fake.paused = false;
    return ESP_OK;
}

bool michi_audio_session_active(void)
{
    return s_fake.active;
}

void michi_audio_session_set_paused(bool paused)
{
    s_fake.paused = paused;
}

esp_err_t michi_audio_session_get_port(uint16_t *out_port)
{
    if (out_port == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_fake.active) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_port = s_fake.bound_port;
    return ESP_OK;
}

esp_err_t michi_audio_session_get_ssrc(uint32_t *out_ssrc)
{
    if (out_ssrc == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_fake.active) {
        return ESP_ERR_NOT_FOUND;
    }
    *out_ssrc = s_fake.ssrc_requested; /* the negotiated value */
    return ESP_OK;
}

esp_err_t michi_audio_session_get_peer(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_fake.active) {
        return ESP_ERR_NOT_FOUND;
    }
    const size_t len = strlen(s_fake.source_ip_requested);
    if (len >= out_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(out, s_fake.source_ip_requested, len + 1);
    return ESP_OK;
}

esp_err_t michi_audio_get_metrics(michi_audio_metrics_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = s_fake.metrics;
    return ESP_OK;
}
