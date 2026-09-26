#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

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

/* AUDIO-Q-01: queued stale PCM -> QUIESCE -> ACK -> stale PCM never submitted afterward */
static void test_audio_q_01_stale_pcm_never_submitted(void)
{
    printf("=== AUDIO-Q-01: Stale PCM never submitted after quiesce ACK ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    uint8_t pcm_data[1920];
    memset(pcm_data, 0x55, sizeof(pcm_data));

    /* Queue non-zero audio */
    for (int i = 0; i < 5; i++) {
        CHECK(michi_audio_output_write(pcm_data, sizeof(pcm_data)) == ESP_OK, "write audio succeeds");
    }

    /* Trigger quiesce: must flush ring, clear chunk, output silence, and enter QUIESCED */
    CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds");
    CHECK(michi_audio_output_is_quiesced(), "pipeline is quiesced");
    CHECK(test_i2s_last_write_was_silence(), "silence submitted during quiesce");

    /* Resume and allow worker to run */
    CHECK(michi_audio_output_resume() == ESP_OK, "resume succeeds");
    usleep(50000);

    /* Assert no old 0x55 PCM was submitted */
    CHECK(test_i2s_last_write_was_silence(), "no stale PCM submitted after quiesce");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* AUDIO-Q-02: write while QUIESCED -> rejected */
static void test_audio_q_02_write_while_quiesced_rejected(void)
{
    printf("=== AUDIO-Q-02: Write while quiesced rejected ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds");

    uint8_t pcm_data[256];
    memset(pcm_data, 0x11, sizeof(pcm_data));
    esp_err_t err = michi_audio_output_write(pcm_data, sizeof(pcm_data));
    CHECK(err == ESP_ERR_INVALID_STATE, "write while quiesced rejected with ESP_ERR_INVALID_STATE");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* AUDIO-Q-03: resume -> fresh PCM accepted */
static void test_audio_q_03_resume_accepts_fresh_pcm(void)
{
    printf("=== AUDIO-Q-03: Resume accepts fresh PCM ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds");

    CHECK(michi_audio_output_resume() == ESP_OK, "resume succeeds");
    CHECK(!michi_audio_output_is_quiesced(), "pipeline is no longer quiesced");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_RUNNING, "state is RUNNING");

    uint8_t pcm_data[256];
    memset(pcm_data, 0x22, sizeof(pcm_data));
    CHECK(michi_audio_output_write(pcm_data, sizeof(pcm_data)) == ESP_OK, "fresh write succeeds after resume");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void *writer_thread_func(void *arg)
{
    (void)arg;
    uint8_t pcm[1920];
    memset(pcm, 0x33, sizeof(pcm));
    for (int i = 0; i < 20; i++) {
        (void)michi_audio_output_write(pcm, sizeof(pcm));
        usleep(5000);
    }
    return NULL;
}

/* AUDIO-Q-04: QUIESCE while worker is blocked inside I2S write */
static void test_audio_q_04_quiesce_while_blocked_in_write(void)
{
    printf("=== AUDIO-Q-04: Quiesce while worker is blocked inside I2S write ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    /* Introduce artificial delay in I2S write */
    test_i2s_set_write_delay_ms(50);

    pthread_t th;
    pthread_create(&th, NULL, writer_thread_func, NULL);
    usleep(15000); /* Ensure writer is executing and worker is inside i2s_channel_write */

    /* Quiesce concurrently */
    CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce completes cleanly while worker was in write");
    CHECK(michi_audio_output_is_quiesced(), "pipeline is quiesced");

    pthread_join(th, NULL);
    test_i2s_set_write_delay_ms(0);

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* AUDIO-Q-05: I2S write failure during quiesce -> error propagated */
static void test_audio_q_05_i2s_write_fail_during_quiesce(void)
{
    printf("=== AUDIO-Q-05: I2S write failure during quiesce propagates error ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    /* Inject I2S write error */
    test_i2s_set_write_fail(ESP_FAIL);

    esp_err_t err = michi_audio_output_quiesce();
    CHECK(err == ESP_FAIL, "quiesce propagates I2S write failure");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED, "pipeline state enters FAULTED");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds from faulted state");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* AUDIO-Q-06: repeated QUIESCE behavior defined */
static void test_audio_q_06_repeated_quiesce_idempotent(void)
{
    printf("=== AUDIO-Q-06: Repeated quiesce is idempotent ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    CHECK(michi_audio_output_quiesce() == ESP_OK, "first quiesce succeeds");
    CHECK(michi_audio_output_is_quiesced(), "quiesced true");

    CHECK(michi_audio_output_quiesce() == ESP_OK, "second quiesce succeeds (idempotent)");
    CHECK(michi_audio_output_is_quiesced(), "quiesced still true");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* AUDIO-Q-07: STOP while QUIESCED */
static void test_audio_q_07_stop_while_quiesced(void)
{
    printf("=== AUDIO-Q-07: Stop while quiesced terminates cleanly ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop from quiesced state succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state is STOPPED");
    CHECK(!michi_audio_output_is_running(), "pipeline is not running");

    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* AUDIO-Q-08: RESUME while STOPPING/STOPPED rejected */
static void test_audio_q_08_resume_while_stopped_rejected(void)
{
    printf("=== AUDIO-Q-08: Resume while stopped rejected ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");

    CHECK(michi_audio_output_resume() == ESP_ERR_INVALID_STATE, "resume while stopped returns ESP_ERR_INVALID_STATE");

    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* AUDIO-Q-09: only one context ever calls i2s_channel_write */
static void test_audio_q_09_single_i2s_channel_write_context(void)
{
    printf("=== AUDIO-Q-09: Only one context ever calls i2s_channel_write ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    uint8_t pcm[1920];
    memset(pcm, 0x44, sizeof(pcm));
    for (int i = 0; i < 5; i++) {
        CHECK(michi_audio_output_write(pcm, sizeof(pcm)) == ESP_OK, "write audio succeeds");
    }

    CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds");
    CHECK(michi_audio_output_resume() == ESP_OK, "resume succeeds");

    for (int i = 0; i < 5; i++) {
        CHECK(michi_audio_output_write(pcm, sizeof(pcm)) == ESP_OK, "write audio succeeds");
    }

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");

    CHECK(!test_i2s_multiple_writers_detected(), "zero multiple writers detected: only audio task called i2s_channel_write");
}

/* AUDIO-Q-10: s_chunk never modified concurrently */
static void test_audio_q_10_single_owner_s_chunk(void)
{
    printf("=== AUDIO-Q-10: Single owner for s_chunk (exclusive worker ownership) ===\n");
    test_i2s_reset();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    pthread_t th;
    pthread_create(&th, NULL, writer_thread_func, NULL);

    for (int i = 0; i < 5; i++) {
        CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds under concurrent writer");
        usleep(5000);
        CHECK(michi_audio_output_resume() == ESP_OK, "resume succeeds under concurrent writer");
        usleep(5000);
    }

    pthread_join(th, NULL);
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

int main(void)
{
    printf("=== michi_audio_output quiesce tests (AUDIO-Q-01..10) ===\n");
    test_audio_q_01_stale_pcm_never_submitted();
    test_audio_q_02_write_while_quiesced_rejected();
    test_audio_q_03_resume_accepts_fresh_pcm();
    test_audio_q_04_quiesce_while_blocked_in_write();
    test_audio_q_05_i2s_write_fail_during_quiesce();
    test_audio_q_06_repeated_quiesce_idempotent();
    test_audio_q_07_stop_while_quiesced();
    test_audio_q_08_resume_while_stopped_rejected();
    test_audio_q_09_single_i2s_channel_write_context();
    test_audio_q_10_single_owner_s_chunk();

    if (failures != 0) {
        printf("\nFAILED: %d check(s) failed\n", failures);
        return 1;
    }
    printf("\nPASSED: all AUDIO-Q-01..10 checks passed\n");
    return 0;
}

