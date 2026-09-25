#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "michi_audio_output.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } else { \
        printf("PASS: %s\n", (msg)); \
    } \
} while (0)

static michi_audio_output_config_t default_cfg(void)
{
    michi_audio_output_config_t cfg = {
        .sample_rate = 48000,
        .bit_depth = 16,
        .channels = 2,
        .buffer_ms = 80,
        .ring_buffer_kb = 64,
        .bclk = 3,
        .lrck = 18,
        .din = 5,
        .mclk = -1,
    };
    return cfg;
}

static void test_quiesce_clean_and_rejection_gate(void)
{
    printf("=== QUIESCE-01..04: sample-clean pause/stop quiesce and write rejection gate ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    esp_err_t err = michi_audio_output_init(&cfg);
    CHECK(err == ESP_OK, "init output pipeline succeeds");

    err = michi_audio_output_start();
    CHECK(err == ESP_OK, "start output pipeline succeeds");
    CHECK(michi_audio_output_is_running(), "pipeline is running");
    CHECK(!michi_audio_output_is_quiesced(), "pipeline is not quiesced initially");

    /* Generate PCM data */
    uint8_t pcm_data[1920];
    for (size_t i = 0; i < sizeof(pcm_data); i++) {
        pcm_data[i] = (uint8_t)(i + 1); /* Non-zero audio */
    }

    /* Write non-zero audio to fill prefill and start playback */
    for (int i = 0; i < 10; i++) {
        err = michi_audio_output_write(pcm_data, sizeof(pcm_data));
        CHECK(err == ESP_OK, "write audio data before quiesce succeeds");
    }

    /* Trigger pause quiesce */
    err = michi_audio_output_quiesce();
    CHECK(err == ESP_OK, "quiesce succeeds");
    CHECK(michi_audio_output_is_quiesced(), "michi_audio_output_is_quiesced() returns true");

    /* QUIESCE-02 / GATE: After pause acknowledgement, no additional PCM payload from old session may be submitted */
    err = michi_audio_output_write(pcm_data, sizeof(pcm_data));
    CHECK(err == ESP_ERR_INVALID_STATE, "QUIESCE GATE: write during quiesce rejected with ESP_ERR_INVALID_STATE");

    /* QUIESCE-03: Silence flushed through DMA to clear in-flight hardware FIFO */
    CHECK(test_i2s_last_write_was_silence(), "DMA received explicit digital silence chunk");

    /* QUIESCE-04: Resume re-enables pipeline acceptance */
    err = michi_audio_output_resume();
    CHECK(err == ESP_OK, "resume succeeds");
    CHECK(!michi_audio_output_is_quiesced(), "michi_audio_output_is_quiesced() returns false after resume");

    err = michi_audio_output_write(pcm_data, sizeof(pcm_data));
    CHECK(err == ESP_OK, "write after resume succeeds");

    /* Cleanup */
    err = michi_audio_output_stop();
    CHECK(err == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_is_quiesced(), "stop leaves pipeline in quiesced state");

    err = michi_audio_output_deinit();
    CHECK(err == ESP_OK, "deinit succeeds");
}

int main(void)
{
    printf("=== michi_audio_output quiesce tests (Phase R2-F) ===\n");
    test_quiesce_clean_and_rejection_gate();

    if (failures != 0) {
        printf("\nFAILED: %d check(s) failed\n", failures);
        return 1;
    }
    printf("\nPASSED: all QUIESCE checks passed\n");
    return 0;
}
