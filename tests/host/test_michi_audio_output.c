#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "michi_audio_output.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <time.h>

static int64_t get_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000);
}

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

/* H12: driver may return ESP_OK yet write fewer silence bytes. This is
 * a failure of the quiesce safety barrier, not success. */
static void test_h12_quiesce_short_silence_dma_fails_closed(void)
{
    printf("=== H12: incomplete silence DMA never publishes QUIESCED ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H12 short DMA init");
    CHECK(michi_audio_output_start() == ESP_OK, "H12 short DMA start");
    test_i2s_set_next_write_short(1024);
    CHECK(michi_audio_output_quiesce() != ESP_OK, "H12 short silence DMA propagates failure");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED,
          "H12 short silence DMA latches FAULTED");
    CHECK(!michi_audio_output_is_quiesced(), "H12 short DMA never claims quiesced");
    CHECK(michi_audio_output_stop() == ESP_OK, "H12 short DMA stop");
    CHECK(michi_audio_output_deinit() == ESP_OK, "H12 short DMA deinit");
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

/* ==================================================================
 * AUDIO-INIT-FAIL-01..05: Initialization failure handling & cleanup
 * ================================================================== */

static void test_audio_init_fail_01_binary_sem_alloc(void)
{
    printf("=== AUDIO-INIT-FAIL-01: Binary semaphore allocation failure ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    test_semphr_set_fail_create_binary(true);
    esp_err_t err = michi_audio_output_init(&cfg);
    test_semphr_set_fail_create_binary(false);

    CHECK(err == ESP_ERR_NO_MEM, "binary semaphore alloc failure returns ESP_ERR_NO_MEM");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED, "state remains UNINITIALIZED");

    /* Retry init without injected failure must succeed */
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "retry init succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_INITIALIZED, "state is INITIALIZED");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_init_fail_02_mutex_alloc(void)
{
    printf("=== AUDIO-INIT-FAIL-02: Mutex allocation failure ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    test_semphr_set_fail_create_mutex(true);
    esp_err_t err = michi_audio_output_init(&cfg);
    test_semphr_set_fail_create_mutex(false);

    CHECK(err == ESP_ERR_NO_MEM, "mutex alloc failure returns ESP_ERR_NO_MEM");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED, "state remains UNINITIALIZED");

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "retry init succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_init_fail_03_i2s_new_channel(void)
{
    printf("=== AUDIO-INIT-FAIL-03: i2s_new_channel failure ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    test_i2s_set_new_channel_fail(ESP_ERR_NO_MEM);
    esp_err_t err = michi_audio_output_init(&cfg);

    CHECK(err == ESP_ERR_NO_MEM, "i2s_new_channel failure returns ESP_ERR_NO_MEM");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED, "state remains UNINITIALIZED");

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "retry init succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_init_fail_04_i2s_init_std_mode(void)
{
    printf("=== AUDIO-INIT-FAIL-04: i2s_channel_init_std_mode failure ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    test_i2s_set_init_std_mode_fail(ESP_FAIL);
    esp_err_t err = michi_audio_output_init(&cfg);

    CHECK(err == ESP_FAIL, "i2s_channel_init_std_mode failure returns ESP_FAIL");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED, "state remains UNINITIALIZED");

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "retry init succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_init_fail_05_task_create_fail(void)
{
    printf("=== AUDIO-INIT-FAIL-05: Task creation failure during start ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    test_task_set_create_fail(true);
    esp_err_t err = michi_audio_output_start();
    test_task_set_create_fail(false);

    CHECK(err == ESP_ERR_NO_MEM, "task create failure returns ESP_ERR_NO_MEM");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_INITIALIZED, "state reverts to INITIALIZED");

    /* Retry start without failure */
    CHECK(michi_audio_output_start() == ESP_OK, "retry start succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_RUNNING, "state is RUNNING");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* ==================================================================
 * AUDIO-STATE-01..08: Mathematically explicit transition matrix tests
 * ================================================================== */

static void test_audio_state_01_init_transitions(void)
{
    printf("=== AUDIO-STATE-01: Init transitions and illegal repeated init ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED, "initial state is UNINITIALIZED");
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_INITIALIZED, "state transitions to INITIALIZED");

    /* Repeated init must be rejected */
    CHECK(michi_audio_output_init(&cfg) == ESP_ERR_INVALID_STATE, "repeated init rejected");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED, "state is UNINITIALIZED");
}

static void test_audio_state_02_quiesce_while_initialized_rejected(void)
{
    printf("=== AUDIO-STATE-02: Quiesce while INITIALIZED rejected (no worker) ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_quiesce() == ESP_ERR_INVALID_STATE, "quiesce while INITIALIZED rejected");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_INITIALIZED, "state remains INITIALIZED");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_state_03_resume_while_initialized_rejected(void)
{
    printf("=== AUDIO-STATE-03: Resume while INITIALIZED rejected ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_resume() == ESP_ERR_INVALID_STATE, "resume while INITIALIZED rejected");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_INITIALIZED, "state remains INITIALIZED");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_state_04_start_and_repeated_start_rejected(void)
{
    printf("=== AUDIO-STATE-04: Start and illegal repeated start ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_RUNNING, "state is RUNNING");

    /* Repeated start while RUNNING rejected */
    CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE, "repeated start while RUNNING rejected");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_state_05_start_while_quiesced_rejected(void)
{
    printf("=== AUDIO-STATE-05: Start while QUIESCED rejected ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_QUIESCED, "state is QUIESCED");

    CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE, "start while QUIESCED rejected");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_state_06_stop_from_initialized_and_stopped(void)
{
    printf("=== AUDIO-STATE-06: Stop from INITIALIZED and STOPPED is idempotent no-op ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop while INITIALIZED is no-op ESP_OK");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_INITIALIZED, "state remains INITIALIZED");

    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state is STOPPED");

    CHECK(michi_audio_output_stop() == ESP_OK, "second stop while STOPPED is no-op ESP_OK");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state remains STOPPED");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_state_07_quiesce_while_stopped_rejected(void)
{
    printf("=== AUDIO-STATE-07: Quiesce while STOPPED rejected ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state is STOPPED");

    CHECK(michi_audio_output_quiesce() == ESP_ERR_INVALID_STATE, "quiesce while STOPPED rejected");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state remains STOPPED");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

static void test_audio_state_08_illegal_write_and_flush(void)
{
    printf("=== AUDIO-STATE-08: Illegal write and flush rejected ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    uint8_t pcm[64] = {0};

    /* From UNINITIALIZED */
    CHECK(michi_audio_output_write(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE, "write while UNINITIALIZED rejected");
    CHECK(michi_audio_output_flush() == ESP_ERR_INVALID_STATE, "flush while UNINITIALIZED rejected");

    /* From INITIALIZED */
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_write(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE, "write while INITIALIZED rejected");
    CHECK(michi_audio_output_flush() == ESP_ERR_INVALID_STATE, "flush while INITIALIZED rejected");

    /* From STOPPED */
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_write(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE, "write while STOPPED rejected");
    CHECK(michi_audio_output_flush() == ESP_ERR_INVALID_STATE, "flush while STOPPED rejected");

    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
}

/* ==================================================================
 * AUDIO-CMD-01..06: Command protocol, generation ACK, timeout, faults
 * ================================================================== */

/* AUDIO-CMD-01: Late ACK from command A cannot satisfy command B or overwrite state */
static void test_audio_cmd_01_late_ack_cannot_satisfy_future_cmd(void)
{
    printf("=== AUDIO-CMD-01: Late ACK cannot satisfy future command or overwrite state ===\n");
    test_i2s_reset();
    test_task_reset_invalid_notify_count();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    /* 1. Command A is genuinely in-flight: induce real latency in worker silence write */
    test_i2s_set_write_delay_ms(60);
    test_michi_audio_output_set_cmd_timeout_ms(20);

    esp_err_t err = michi_audio_output_quiesce();
    CHECK(err == ESP_ERR_TIMEOUT, "in-flight command A times out returning ESP_ERR_TIMEOUT");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED, "pipeline enters FAULTED on timeout");

    /* 2. Worker finishes its in-flight execution late */
    usleep(70000);
    test_i2s_set_write_delay_ms(0);
    test_michi_audio_output_set_cmd_timeout_ms(0);

    /* 3. Late execution of in-flight command A MUST NOT overwrite FAULTED state */
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED,
          "late execution of in-flight command A preserves FAULTED state without overwrite");

    /* 4. Recover via stop and restart */
    CHECK(michi_audio_output_stop() == ESP_OK, "stop recovers from FAULTED");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state is STOPPED");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_RUNNING, "state is RUNNING");

    /* 5. Command B dispatches with fresh generation and succeeds */
    CHECK(michi_audio_output_quiesce() == ESP_OK, "command B with fresh generation succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_QUIESCED, "state is QUIESCED");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
    CHECK(test_task_invalid_notify_count() == 0, "no invalid task notifications in CMD-01");
}

/* AUDIO-CMD-02: Worker missing or dead -> immediate failure, no full timeout wait */
static void test_audio_cmd_02_dead_worker_immediate_reject(void)
{
    printf("=== AUDIO-CMD-02: Missing or dead worker rejects immediately ===\n");
    test_i2s_reset();
    test_task_reset_invalid_notify_count();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    /* No worker running: quiesce must reject immediately without waiting */
    int64_t t0 = get_now_us();
    CHECK(michi_audio_output_quiesce() == ESP_ERR_INVALID_STATE, "quiesce with NULL worker returns ESP_ERR_INVALID_STATE");
    int64_t elapsed_us = get_now_us() - t0;
    CHECK(elapsed_us < 100000, "rejected immediately without timeout wait");

    /* Start and stop: worker is now dead / stopped */
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");
    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state is STOPPED");

    t0 = get_now_us();
    CHECK(michi_audio_output_quiesce() == ESP_ERR_INVALID_STATE, "quiesce on dead worker rejected immediately");
    CHECK(michi_audio_output_resume() == ESP_ERR_INVALID_STATE, "resume on dead worker rejected immediately");
    elapsed_us = get_now_us() - t0;
    CHECK(elapsed_us < 100000, "dead worker rejected immediately without timeout wait");

    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
    CHECK(test_task_invalid_notify_count() == 0, "no invalid task notifications in CMD-02");
}

/* AUDIO-CMD-03: Command timeout -> defined fault/recovery state */
static void test_audio_cmd_03_timeout_fault_semantics(void)
{
    printf("=== AUDIO-CMD-03: Timeout transitions to FAULTED and clean recovery ===\n");
    test_i2s_reset();
    test_task_reset_invalid_notify_count();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    /* Configure 20ms timeout override and induce 60ms write latency in worker */
    test_i2s_set_write_delay_ms(60);
    test_michi_audio_output_set_cmd_timeout_ms(20);

    esp_err_t err = michi_audio_output_quiesce();
    CHECK(err == ESP_ERR_TIMEOUT, "command times out returning ESP_ERR_TIMEOUT");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED, "pipeline enters FAULTED on timeout");

    usleep(70000);
    test_i2s_set_write_delay_ms(0);
    test_michi_audio_output_set_cmd_timeout_ms(0);

    /* Illegal transitions while FAULTED */
    uint8_t pcm[64] = {0};
    CHECK(michi_audio_output_write(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE, "write while FAULTED rejected");
    CHECK(michi_audio_output_flush() == ESP_ERR_INVALID_STATE, "flush while FAULTED rejected");
    CHECK(michi_audio_output_quiesce() == ESP_ERR_INVALID_STATE, "quiesce while FAULTED rejected");
    CHECK(michi_audio_output_resume() == ESP_ERR_INVALID_STATE, "resume while FAULTED rejected");
    CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE, "start while FAULTED rejected");
    CHECK(michi_audio_output_deinit() == ESP_ERR_INVALID_STATE, "deinit while FAULTED rejected");

    /* Recovery: stop must cleanly recover from FAULTED */
    CHECK(michi_audio_output_stop() == ESP_OK, "stop recovers from FAULTED");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state is STOPPED");

    /* Restart runs cleanly */
    CHECK(michi_audio_output_start() == ESP_OK, "re-start after recovery succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_RUNNING, "state is RUNNING");

    uint8_t pcm2[256] = {0xAA};
    CHECK(michi_audio_output_write(pcm2, sizeof(pcm2)) == ESP_OK, "write succeeds on recovered pipeline");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
    CHECK(test_task_invalid_notify_count() == 0, "no invalid task notifications in CMD-03");
}

/* AUDIO-CMD-04: QUIESCE and RESUME concurrent callers serialize deterministically */
typedef struct {
    int iterations;
    int success_count;
    int error_count;
} concur_worker_arg_t;

static void *concur_quiescer(void *arg)
{
    concur_worker_arg_t *c = (concur_worker_arg_t *)arg;
    for (int i = 0; i < c->iterations; i++) {
        esp_err_t err = michi_audio_output_quiesce();
        if (err == ESP_OK) {
            c->success_count++;
        } else {
            c->error_count++;
        }
        usleep(500);
    }
    return NULL;
}

static void *concur_resumer(void *arg)
{
    concur_worker_arg_t *c = (concur_worker_arg_t *)arg;
    for (int i = 0; i < c->iterations; i++) {
        esp_err_t err = michi_audio_output_resume();
        if (err == ESP_OK) {
            c->success_count++;
        } else {
            c->error_count++;
        }
        usleep(500);
    }
    return NULL;
}

static void test_audio_cmd_04_concurrent_callers_serialized(void)
{
    printf("=== AUDIO-CMD-04: Concurrent QUIESCE and RESUME callers serialized ===\n");
    test_i2s_reset();
    test_task_reset_invalid_notify_count();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    concur_worker_arg_t arg_q = {.iterations = 20, .success_count = 0, .error_count = 0};
    concur_worker_arg_t arg_r = {.iterations = 20, .success_count = 0, .error_count = 0};

    pthread_t th1, th2;
    pthread_create(&th1, NULL, concur_quiescer, &arg_q);
    pthread_create(&th2, NULL, concur_resumer, &arg_r);

    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    michi_audio_output_state_t st = michi_audio_output_get_state();
    CHECK(st == MICHI_AUDIO_STATE_RUNNING || st == MICHI_AUDIO_STATE_QUIESCED,
          "pipeline state is valid (RUNNING or QUIESCED) after concurrent commands");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
    CHECK(test_task_invalid_notify_count() == 0, "no invalid task notifications in CMD-04");
}

/* AUDIO-CMD-05: STOP racing command cannot produce illegal state */
static void *slow_command_thread(void *arg)
{
    esp_err_t *res = (esp_err_t *)arg;
    *res = michi_audio_output_quiesce();
    return NULL;
}

static void test_audio_cmd_05_stop_racing_command(void)
{
    printf("=== AUDIO-CMD-05: STOP racing command cannot produce illegal state ===\n");
    test_i2s_reset();
    test_task_reset_invalid_notify_count();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    /* Induce real delay in worker silence write (100ms) */
    test_i2s_set_write_delay_ms(100);
    test_michi_audio_output_set_cmd_timeout_ms(500);

    esp_err_t cmd_res = ESP_OK;
    pthread_t th;
    pthread_create(&th, NULL, slow_command_thread, &cmd_res);

    /* Allow slow command to enter send_cmd_and_wait_ack and be dequeued */
    usleep(15000);

    /* Main thread calls stop() while command is in-flight */
    CHECK(michi_audio_output_stop() == ESP_OK, "stop racing in-flight command succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state is STOPPED");

    pthread_join(th, NULL);

    test_i2s_set_write_delay_ms(0);
    test_michi_audio_output_set_cmd_timeout_ms(0);

    /* deinit immediately after must succeed cleanly (no UAF or mutex lockup) */
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit immediately after stopped race succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED, "state is UNINITIALIZED");
    CHECK(test_task_invalid_notify_count() == 0, "no invalid task notifications in CMD-05");
}

/* H13: a command has dropped the command mutex but not its ownership
 * reference. STOP is free to complete; DEINIT must refuse to free semaphores
 * until the last admitted QUIESCE caller has left the public API. */
static void test_h13_control_reference_blocks_deinit(void)
{
    printf("=== H13: control command reference survives STOP ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H13 init");
    CHECK(michi_audio_output_start() == ESP_OK, "H13 start");

    test_michi_audio_output_hold_control_exit(true);
    pthread_t th;
    esp_err_t cmd_res = ESP_FAIL;
    pthread_create(&th, NULL, slow_command_thread, &cmd_res);
    bool observed = false;
    for (int i = 0; i < 200; i++) {
        if (test_michi_audio_output_control_exit_is_held()) {
            observed = true;
            break;
        }
        usleep(5000);
    }
    CHECK(observed, "H13 command reached control reference window");
    if (observed) {
        CHECK(michi_audio_output_stop() == ESP_OK, "H13 STOP joins worker");
        CHECK(michi_audio_output_deinit() == ESP_ERR_INVALID_STATE,
              "H13 DEINIT refuses live control reference");
    }
    test_michi_audio_output_hold_control_exit(false);
    pthread_join(th, NULL);
    if (!observed) (void)michi_audio_output_stop();
    CHECK(michi_audio_output_deinit() == ESP_OK,
          "H13 DEINIT succeeds after control reference returns");
}

/* AUDIO-CMD-06: Zero notifications to dead/invalid task handle across lifecycle */
static void test_audio_cmd_06_no_stale_taskhandle_notification(void)
{
    printf("=== AUDIO-CMD-06: Zero notifications to dead/invalid task handle across lifecycle ===\n");
    test_i2s_reset();
    test_task_reset_invalid_notify_count();
    michi_audio_output_config_t cfg = default_cfg();

    /* Cycle start/stop 10 times with writes and commands */
    for (int i = 0; i < 10; i++) {
        CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
        CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

        uint8_t pcm[256] = {0};
        (void)michi_audio_output_write(pcm, sizeof(pcm));

        CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds");
        CHECK(michi_audio_output_resume() == ESP_OK, "resume succeeds");

        CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
        CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
    }

    CHECK(test_task_invalid_notify_count() == 0,
          "FINAL INVARIANT: test_task_invalid_notify_count == 0 across all lifecycles");
}

/* AUDIO-CMD-07: Concurrent writer during quiesce rejects writes and post-resume content verified */
typedef struct {
    _Atomic bool stop;
    uint32_t rejected_writes;
    uint32_t accepted_writes;
} forbidden_writer_arg_t;

static void *forbidden_writer_thread(void *arg)
{
    forbidden_writer_arg_t *w = (forbidden_writer_arg_t *)arg;
    uint8_t forbidden_pcm[256];
    memset(forbidden_pcm, 0xDE, sizeof(forbidden_pcm));

    while (!w->stop) {
        esp_err_t err = michi_audio_output_write(forbidden_pcm, sizeof(forbidden_pcm));
        if (err == ESP_OK) {
            w->accepted_writes++;
        } else {
            w->rejected_writes++;
        }
        usleep(500);
    }
    return NULL;
}

static void test_audio_cmd_07_concurrent_writer_during_quiesce_content_verification(void)
{
    printf("=== AUDIO-CMD-07: Concurrent writer during QUIESCE and content verification post-resume ===\n");
    test_i2s_reset();
    test_i2s_clear_captured_data();
    test_task_reset_invalid_notify_count();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    forbidden_writer_arg_t warg = {.stop = false, .rejected_writes = 0, .accepted_writes = 0};
    pthread_t th;
    pthread_create(&th, NULL, forbidden_writer_thread, &warg);

    /* Let writer write initial valid data */
    usleep(5000);

    /* Trigger QUIESCE while forbidden writer is actively calling michi_audio_output_write */
    CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce succeeds under concurrent writer");
    CHECK(michi_audio_output_is_quiesced(), "pipeline is quiesced");

    /* Allow writer to continue hammering while quiesced */
    usleep(15000);
    warg.stop = true;
    pthread_join(th, NULL);

    CHECK(warg.rejected_writes > 0, "forbidden writer had writes rejected during quiesce");

    /* Direct write while quiesced must also be rejected by barrier */
    uint8_t probe[64];
    memset(probe, 0xDE, sizeof(probe));
    CHECK(michi_audio_output_write(probe, sizeof(probe)) == ESP_ERR_INVALID_STATE,
          "write while quiesced rejected with ESP_ERR_INVALID_STATE");

    /* Clear capture buffer right before resume */
    test_i2s_clear_captured_data();

    /* RESUME pipeline */
    CHECK(michi_audio_output_resume() == ESP_OK, "resume succeeds");
    CHECK(!michi_audio_output_is_quiesced(), "pipeline is not quiesced");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_RUNNING, "state is RUNNING");

    /* Write 4096 bytes of valid post-resume audio (0x42) */
    uint8_t valid_pcm[4096];
    memset(valid_pcm, 0x42, sizeof(valid_pcm));
    CHECK(michi_audio_output_write(valid_pcm, sizeof(valid_pcm)) == ESP_OK, "post-resume write succeeds");

    /* Wait for worker to consume and output to I2S */
    usleep(60000);

    /* CONTENT VERIFICATION:
     * 1. Zero 0xDE bytes may appear in the captured post-quiesce I2S stream.
     * 2. Valid 0x42 bytes must be present in captured output. */
    CHECK(!test_i2s_contains_byte(0xDE),
          "CONTENT INTEGRITY: zero bytes of forbidden pattern (0xDE) admitted during quiesce");
    CHECK(test_i2s_contains_byte(0x42),
          "CONTENT INTEGRITY: valid post-resume pattern (0x42) output cleanly to I2S");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
    CHECK(test_task_invalid_notify_count() == 0, "no invalid task notifications in CMD-07");
}

/* AUDIO-CMD-08: Stop timeout -> late worker exit -> retry stop -> deinit reconciles done flag */
static void test_audio_cmd_08_stop_timeout_late_worker_retry_reconciles_done(void)
{
    printf("=== AUDIO-CMD-08: Stop timeout -> late worker exit -> retry stop -> deinit ===\n");
    test_i2s_reset();
    test_task_reset_invalid_notify_count();
    michi_audio_output_config_t cfg = default_cfg();

    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    /* Hold worker at shutdown to guarantee deterministic join timeout without timing races */
    test_michi_audio_output_hold_worker(true);
    test_michi_audio_output_set_join_timeout_ms(20);

    /* Stop times out waiting for held worker */
    esp_err_t err = michi_audio_output_stop();
    CHECK(err == ESP_ERR_TIMEOUT, "stop returns ESP_ERR_TIMEOUT on slow worker");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPING, "state is STOPPING");

    /* Release worker hold and reset join timeout */
    test_michi_audio_output_hold_worker(false);
    test_michi_audio_output_set_join_timeout_ms(0);
    usleep(50000); /* 50ms: allow worker to finish self-exit and mark STOPPED / s_task_done */

    /* Worker has exited late and set s_task_done = true, s_state = STOPPED.
     * Retry stop must reconcile s_task_done and return ESP_OK cleanly. */
    CHECK(michi_audio_output_stop() == ESP_OK, "retry stop reconciles s_task_done and succeeds");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED, "state is STOPPED");

    /* deinit must now succeed without reporting task alive */
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds after stop retry");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED, "state is UNINITIALIZED");
    CHECK(test_task_invalid_notify_count() == 0, "no invalid task notifications in CMD-08");
}

/* AUDIO-CMD-09: Deterministic barrier ordering - worker NEVER observes QUIESCE before write barrier is closed */
typedef struct {
    _Atomic bool stop;
    _Atomic uint32_t writes_attempted;
} barrier_stress_writer_t;

static void *barrier_stress_writer_thread(void *arg)
{
    barrier_stress_writer_t *w = (barrier_stress_writer_t *)arg;
    uint8_t pcm[128];
    memset(pcm, 0x5A, sizeof(pcm));

    while (!w->stop) {
        (void)michi_audio_output_write(pcm, sizeof(pcm));
        w->writes_attempted++;
        usleep(100);
    }
    return NULL;
}

static void test_audio_cmd_09_worker_never_observes_quiesce_before_write_barrier_closed(void)
{
    printf("=== AUDIO-CMD-09: Worker never observes QUIESCE before write barrier is closed ===\n");
    test_i2s_reset();
    test_task_reset_invalid_notify_count();
    test_michi_audio_output_reset_quiesce_barrier_race_count();

    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "init succeeds");
    CHECK(michi_audio_output_start() == ESP_OK, "start succeeds");

    /* Spawn 4 concurrent writer threads hammering write admission */
    const int NUM_WRITERS = 4;
    barrier_stress_writer_t wargs[4];
    pthread_t th[4];

    for (int i = 0; i < NUM_WRITERS; i++) {
        wargs[i].stop = false;
        wargs[i].writes_attempted = 0;
        pthread_create(&th[i], NULL, barrier_stress_writer_thread, &wargs[i]);
    }

    /* Execute 50 rapid QUIESCE / RESUME cycles under heavy write contention */
    for (int cycle = 0; cycle < 50; cycle++) {
        CHECK(michi_audio_output_quiesce() == ESP_OK, "quiesce under writer contention succeeds");
        CHECK(michi_audio_output_is_quiesced(), "pipeline is quiesced");

        /* Quiesce is idempotent: repeated quiesce returns ESP_OK and keeps pipeline quiesced */
        CHECK(michi_audio_output_quiesce() == ESP_OK, "idempotent repeated quiesce returns ESP_OK");
        CHECK(michi_audio_output_is_quiesced(), "pipeline remains quiesced");

        /* Direct write must be rejected by barrier while quiesced */
        uint8_t sample[64] = {0};
        CHECK(michi_audio_output_write(sample, sizeof(sample)) == ESP_ERR_INVALID_STATE,
              "write while quiesced rejected by barrier");

        CHECK(michi_audio_output_resume() == ESP_OK, "resume succeeds");
        CHECK(!michi_audio_output_is_quiesced(), "pipeline resumed");

        /* Resume is idempotent: repeated resume returns ESP_OK and keeps pipeline running */
        CHECK(michi_audio_output_resume() == ESP_OK, "idempotent repeated resume returns ESP_OK");
        usleep(500);
    }

    /* Stop writer threads */
    for (int i = 0; i < NUM_WRITERS; i++) {
        wargs[i].stop = true;
        pthread_join(th[i], NULL);
    }

    /* VERIFY CRITICAL INVARIANT:
     * Across all 50 QUIESCE cycles under 4 concurrent writer threads,
     * the worker never once observed QUIESCE while the write barrier was open. */
    CHECK(test_michi_audio_output_get_quiesce_barrier_race_count() == 0,
          "DETERMINISTIC ORDERING: worker NEVER observed QUIESCE before write barrier was closed (race_count == 0)");

    CHECK(michi_audio_output_stop() == ESP_OK, "stop succeeds");
    CHECK(michi_audio_output_deinit() == ESP_OK, "deinit succeeds");
    CHECK(test_task_invalid_notify_count() == 0, "no invalid task notifications in CMD-09");
}

/* H8: only complete interleaved PCM frames may enter the SPSC ring. */
static void test_h8_complete_frame_boundary(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H8 stereo init");
    CHECK(michi_audio_output_start() == ESP_OK, "H8 stereo start");
    uint8_t sample[1920] = {0};
    CHECK(michi_audio_output_write(sample, 3) == ESP_ERR_INVALID_ARG,
          "H8 rejects partial 16-bit stereo PCM frame");
    CHECK(michi_audio_output_write(sample, sizeof(sample)) == ESP_OK,
          "H8 accepts full 16-bit stereo frame batch");
    CHECK(michi_audio_output_stop() == ESP_OK, "H8 stereo stop");
    CHECK(michi_audio_output_deinit() == ESP_OK, "H8 stereo deinit");

    test_i2s_reset();
    cfg = default_cfg();
    cfg.bit_depth = 24;
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H8 24bit init");
    CHECK(michi_audio_output_start() == ESP_OK, "H8 24bit start");
    CHECK(michi_audio_output_write(sample, 5) == ESP_ERR_INVALID_ARG,
          "H8 rejects incomplete 24-bit stereo frame");
    CHECK(michi_audio_output_write(sample, sizeof(sample)) == ESP_OK,
          "H8 accepts 24-bit stereo frame batch");
    CHECK(michi_audio_output_stop() == ESP_OK, "H8 24bit stop");
    CHECK(michi_audio_output_deinit() == ESP_OK, "H8 24bit deinit");
}

/* H9: consecutive failed DMA writes must surface a fault, never an
 * endless lie that the audio output remains RUNNING. */
static void test_h9_dma_fault_latches_and_can_stop(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    michi_audio_output_quality_t before = {0};
    CHECK(michi_audio_output_get_quality(&before) == ESP_OK, "H9 quality getter");
    CHECK(michi_audio_output_get_quality(NULL) == ESP_ERR_INVALID_ARG,
          "H9 quality rejects NULL");
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H9 fault test init");
    CHECK(michi_audio_output_start() == ESP_OK, "H9 fault test start");
    test_i2s_set_write_fail(ESP_FAIL);
    uint8_t pcm[1920] = {0};
    /* 80ms prefill: 8 x 10ms PCM frames at the v1-lite output rate. */
    for (int i = 0; i < 9; i++) {
        CHECK(michi_audio_output_write(pcm, sizeof(pcm)) == ESP_OK,
              "H9 queue valid stereo PCM");
    }
    bool faulted = false;
    for (int i = 0; i < 100; i++) {
        if (michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED) {
            faulted = true;
            break;
        }
        usleep(10000);
    }
    CHECK(faulted, "H9 repeated DMA write errors latch FAULTED");
    michi_audio_output_quality_t after = {0};
    CHECK(michi_audio_output_get_quality(&after) == ESP_OK,
          "H9 read quality after DMA failure");
    CHECK(after.fatal_dma_streaks > before.fatal_dma_streaks,
          "H9 fatal DMA count increments");
    CHECK(after.i2s_errors >= before.i2s_errors + 3,
          "H9 I2S diagnostics count the failed writes");
    CHECK(michi_audio_output_write(pcm, sizeof(pcm)) == ESP_ERR_INVALID_STATE,
          "H9 faulted pipeline rejects new PCM");
    CHECK(michi_audio_output_stop() == ESP_OK,
          "H9 stop disables the channel after worker fault");
    CHECK(michi_audio_output_deinit() == ESP_OK,
          "H9 fault teardown releases resources");
    test_i2s_set_write_fail(ESP_OK);
}

/* H10: the channel must not be considered stopped after a failed disable.
 * Retry may recover ownership; never re-enable or deinit while ambiguous. */
static void test_h10_stop_disable_failure_retries_safely(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H10 stop-failure init");
    CHECK(michi_audio_output_start() == ESP_OK, "H10 stop-failure start");
    test_i2s_set_disable_fail(ESP_FAIL);
    CHECK(michi_audio_output_stop() == ESP_FAIL,
          "H10 stop propagates I2S disable failure");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED,
          "H10 failed disable latches FAULTED");
    CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE,
          "H10 failed disable blocks start");
    CHECK(michi_audio_output_deinit() == ESP_ERR_INVALID_STATE,
          "H10 failed disable blocks deinit");
    CHECK(michi_audio_output_stop() == ESP_OK,
          "H10 retry stop succeeds after disable recovers");
    CHECK(!test_i2s_is_enabled(), "H10 channel disabled after retry");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED,
          "H10 stop state cleared only on success");
    CHECK(michi_audio_output_deinit() == ESP_OK, "H10 deinit after stop retry");
}

/* H10: if task creation fails after enabling the channel and rollback
 * disable also fails, the module must refuse a second start until STOP. */
static void test_h10_start_rollback_disable_failure(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H10 start-rollback init");
    test_task_set_create_fail(true);
    test_i2s_set_disable_fail(ESP_FAIL);
    CHECK(michi_audio_output_start() == ESP_FAIL,
          "H10 task creation failure propagates failed disable rollback");
    test_task_set_create_fail(false);
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED,
          "H10 rollback failure remains FAULTED");
    CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE,
          "H10 rollback failure disallows second start");
    CHECK(michi_audio_output_stop() == ESP_OK,
          "H10 STOP retries channel disable without a worker");
    CHECK(michi_audio_output_deinit() == ESP_OK,
          "H10 init failure recovery frees the channel");
}

/* H10: deletion failure leaves the initialized channel and buffers owned
 * by this module so a later deinit can retry; start must fail closed. */
static void test_h10_deinit_delete_failure_retains_resources(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H10 delete-failure init");
    test_i2s_set_delete_fail(ESP_FAIL);
    CHECK(michi_audio_output_deinit() == ESP_FAIL,
          "H10 deinit propagates channel deletion failure");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_INITIALIZED,
          "H10 deletion failure retains initialized ownership");
    CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE,
          "H10 deletion failure blocks start");
    CHECK(michi_audio_output_deinit() == ESP_OK,
          "H10 retry deinit succeeds when driver recovers");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED,
          "H10 state resets only on complete deinit");
    CHECK(michi_audio_output_init(&cfg) == ESP_OK,
          "H10 fresh init possible after successful retry");
    CHECK(michi_audio_output_deinit() == ESP_OK,
          "H10 fresh channel deletes after retry");
}

/* H11: a partially-initialized channel that refuses deletion stays owned,
 * cannot start, and can be recovered with a later DEINIT. */
static void test_h11_init_cleanup_delete_failure(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    test_i2s_set_init_std_mode_fail(ESP_FAIL);
    test_i2s_set_delete_fail(ESP_FAIL);
    CHECK(michi_audio_output_init(&cfg) == ESP_FAIL,
          "H11 init exposes deletion failure");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_FAULTED,
          "H11 partially initialized channel is quarantined");
    CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE,
          "H11 quarantined channel cannot start");
    CHECK(michi_audio_output_init(&cfg) == ESP_ERR_INVALID_STATE,
          "H11 quarantined channel cannot be overwritten");
    CHECK(michi_audio_output_deinit() == ESP_OK,
          "H11 cleanup retries channel destruction");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_UNINITIALIZED,
          "H11 recovery clears quarantined state");
    CHECK(michi_audio_output_init(&cfg) == ESP_OK,
          "H11 clean init works after recovery");
    CHECK(michi_audio_output_deinit() == ESP_OK,
          "H11 fully recovered channel can deinit");
}

static void *h11_start_thread(void *arg)
{
    esp_err_t *res = (esp_err_t *)arg;
    *res = michi_audio_output_start();
    return NULL;
}

static void *h11_deinit_thread(void *arg)
{
    esp_err_t *res = (esp_err_t *)arg;
    *res = michi_audio_output_deinit();
    return NULL;
}

/* H11: the state check and the I2S driver calls form ONE lifecycle lease. */
static void test_h11_start_vs_deinit_race(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H11 concurrency init");
    test_i2s_set_enable_delay_ms(100);
    esp_err_t start_result = ESP_FAIL;
    pthread_t thread;
    pthread_create(&thread, NULL, h11_start_thread, &start_result);
    bool entered = false;
    for (int i = 0; i < 200; i++) {
        if (test_i2s_enable_entered()) { entered = true; break; }
        usleep(1000);
    }
    CHECK(entered, "H11 START has entered I2S enable");
    if (entered) {
        CHECK(michi_audio_output_deinit() == ESP_ERR_INVALID_STATE,
              "H11 DEINIT must not destroy a channel being enabled");
        CHECK(michi_audio_output_stop() == ESP_ERR_INVALID_STATE,
              "H11 STOP must not race a channel being enabled");
        CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE,
              "H11 START must not race another START");
    }
    pthread_join(thread, NULL);
    test_i2s_set_enable_delay_ms(0);
    CHECK(start_result == ESP_OK, "H11 first START completed");
    CHECK(michi_audio_output_stop() == ESP_OK, "H11 STOP after START");
    CHECK(michi_audio_output_deinit() == ESP_OK, "H11 DEINIT after STOP");
}

static void test_h11_deinit_vs_init_race(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H11 deletion race init");
    test_i2s_set_delete_delay_ms(100);
    esp_err_t deinit_result = ESP_FAIL;
    pthread_t thread;
    pthread_create(&thread, NULL, h11_deinit_thread, &deinit_result);
    bool entered = false;
    for (int i = 0; i < 200; i++) {
        if (test_i2s_delete_entered()) { entered = true; break; }
        usleep(1000);
    }
    CHECK(entered, "H11 DEINIT entered deletion");
    if (entered) {
        CHECK(michi_audio_output_init(&cfg) == ESP_ERR_INVALID_STATE,
              "H11 INIT must not race channel deletion");
        CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE,
              "H11 START must not reenable a channel being deleted");
    }
    pthread_join(thread, NULL);
    test_i2s_set_delete_delay_ms(0);
    CHECK(deinit_result == ESP_OK, "H11 completed channel deletion");
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H11 fresh init after teardown");
    CHECK(michi_audio_output_deinit() == ESP_OK, "H11 cleanup after fresh init");
}

/* H11 regression: a failed disable and a late worker exit must not
 * accidentally turn STOPPING into STOPPED without retrying disable. */
static void test_h11_disable_failure_with_join_timeout(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H11 dual failure init");
    CHECK(michi_audio_output_start() == ESP_OK, "H11 dual failure start");
    test_michi_audio_output_hold_worker(true);
    test_michi_audio_output_set_join_timeout_ms(20);
    test_i2s_set_disable_fail(ESP_FAIL);
    CHECK(michi_audio_output_stop() == ESP_ERR_TIMEOUT,
          "H11 STOP join timeout never claims success");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPING,
          "H11 pending worker remains STOPPING");
    test_michi_audio_output_hold_worker(false);
    test_michi_audio_output_set_join_timeout_ms(0);
    usleep(80000);
    CHECK(michi_audio_output_get_state() != MICHI_AUDIO_STATE_STOPPED,
          "H11 late worker cannot publish STOPPED before disable retry");
    CHECK(michi_audio_output_stop() == ESP_OK,
          "H11 retry completes the missing I2S disable");
    CHECK(!test_i2s_is_enabled(), "H11 hardware channel disabled after retry");
    CHECK(michi_audio_output_get_state() == MICHI_AUDIO_STATE_STOPPED,
          "H11 owner alone publishes STOPPED");
    CHECK(michi_audio_output_deinit() == ESP_OK,
          "H11 deinit succeeds after disable retry");
}

static void *h11_write_thread(void *arg)
{
    esp_err_t *res = (esp_err_t *)arg;
    uint8_t pcm[1920] = {0};
    *res = michi_audio_output_write(pcm, sizeof(pcm));
    return NULL;
}

static void test_h11_producer_cannot_outlive_deinit(void)
{
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H11 write-lease init");
    CHECK(michi_audio_output_start() == ESP_OK, "H11 write-lease start");
    test_michi_audio_output_hold_producer(true);
    esp_err_t writer_result = ESP_OK;
    pthread_t thread;
    pthread_create(&thread, NULL, h11_write_thread, &writer_result);
    bool held = false;
    for (int i = 0; i < 200; i++) {
        if (test_michi_audio_output_producer_is_held()) { held = true; break; }
        usleep(1000);
    }
    CHECK(held, "H11 producer has acquired its memory lease");
    if (held) {
        CHECK(michi_audio_output_stop() == ESP_OK,
              "H11 STOP remains cooperative with an active producer");
        CHECK(michi_audio_output_deinit() == ESP_ERR_INVALID_STATE,
              "H11 DEINIT refuses to free ring under active producer");
        CHECK(michi_audio_output_start() == ESP_ERR_INVALID_STATE,
              "H11 next START refuses ring reused before producer release");
    }
    test_michi_audio_output_hold_producer(false);
    pthread_join(thread, NULL);
    if (held) {
        CHECK(writer_result == ESP_ERR_INVALID_STATE,
              "H11 stale producer cannot write to the stopped session");
        CHECK(michi_audio_output_deinit() == ESP_OK,
              "H11 DEINIT succeeds once producer releases its lease");
    } else {
        (void)michi_audio_output_stop();
        (void)michi_audio_output_deinit();
    }
}

/* H14: a producer call must be bounded by total elapsed time even when
 * the consumer intermittently drains the ring and the writer progresses.
 * This uses the real host monotonic-clock FreeRTOS shim. */
static void test_h14_total_pcm_write_deadline(void)
{
    printf("=== H14: total PCM enqueue deadline includes partial progress ===\n");
    test_i2s_reset();
    michi_audio_output_config_t cfg = default_cfg();
    CHECK(michi_audio_output_init(&cfg) == ESP_OK, "H14 bounded-write init");
    CHECK(michi_audio_output_start() == ESP_OK, "H14 bounded-write start");
    enum { PCM_BYTES = 256 * 1024 };
    uint8_t *pcm = malloc(PCM_BYTES);
    CHECK(pcm != NULL, "H14 allocate long PCM input");
    if (pcm != NULL) {
        memset(pcm, 0x33, PCM_BYTES);
        test_i2s_set_write_delay_ms(120);
        const int64_t started_us = get_now_us();
        const esp_err_t err = michi_audio_output_write(pcm, PCM_BYTES);
        const int64_t elapsed_us = get_now_us() - started_us;
        CHECK(err == ESP_ERR_TIMEOUT,
              "H14 slow partial progress must respect global deadline");
        CHECK(elapsed_us <= 3000000,
              "H14 producer finishes within generous wall-clock budget");
        free(pcm);
        test_i2s_set_write_delay_ms(0);
    }
    CHECK(michi_audio_output_stop() == ESP_OK, "H14 bounded-write stop");
    CHECK(michi_audio_output_deinit() == ESP_OK, "H14 bounded-write deinit");
}

int main(void)
{
    test_h14_total_pcm_write_deadline();
    test_task_reset_invalid_notify_count();
    test_h11_producer_cannot_outlive_deinit();
    test_h11_init_cleanup_delete_failure();
    test_h11_start_vs_deinit_race();
    test_h11_deinit_vs_init_race();
    test_h11_disable_failure_with_join_timeout();
    test_h10_stop_disable_failure_retries_safely();
    test_h10_start_rollback_disable_failure();
    test_h10_deinit_delete_failure_retains_resources();
    test_h8_complete_frame_boundary();
    test_h9_dma_fault_latches_and_can_stop();

    printf("=== michi_audio_output quiesce tests (AUDIO-Q-01..10) ===\n");
    test_audio_q_01_stale_pcm_never_submitted();
    test_audio_q_02_write_while_quiesced_rejected();
    test_audio_q_03_resume_accepts_fresh_pcm();
    test_audio_q_04_quiesce_while_blocked_in_write();
    test_audio_q_05_i2s_write_fail_during_quiesce();
    test_h12_quiesce_short_silence_dma_fails_closed();
    test_audio_q_06_repeated_quiesce_idempotent();
    test_audio_q_07_stop_while_quiesced();
    test_audio_q_08_resume_while_stopped_rejected();
    test_audio_q_09_single_i2s_channel_write_context();
    test_audio_q_10_single_owner_s_chunk();

    printf("\n=== michi_audio_output init fail tests (AUDIO-INIT-FAIL-01..05) ===\n");
    test_audio_init_fail_01_binary_sem_alloc();
    test_audio_init_fail_02_mutex_alloc();
    test_audio_init_fail_03_i2s_new_channel();
    test_audio_init_fail_04_i2s_init_std_mode();
    test_audio_init_fail_05_task_create_fail();

    printf("\n=== michi_audio_output state machine tests (AUDIO-STATE-01..08) ===\n");
    test_audio_state_01_init_transitions();
    test_audio_state_02_quiesce_while_initialized_rejected();
    test_audio_state_03_resume_while_initialized_rejected();
    test_audio_state_04_start_and_repeated_start_rejected();
    test_audio_state_05_start_while_quiesced_rejected();
    test_audio_state_06_stop_from_initialized_and_stopped();
    test_audio_state_07_quiesce_while_stopped_rejected();
    test_audio_state_08_illegal_write_and_flush();

    printf("\n=== michi_audio_output command protocol tests (AUDIO-CMD-01..09) ===\n");
    test_audio_cmd_01_late_ack_cannot_satisfy_future_cmd();
    test_audio_cmd_02_dead_worker_immediate_reject();
    test_audio_cmd_03_timeout_fault_semantics();
    test_audio_cmd_04_concurrent_callers_serialized();
    test_audio_cmd_05_stop_racing_command();
    test_h13_control_reference_blocks_deinit();
    test_audio_cmd_06_no_stale_taskhandle_notification();
    test_audio_cmd_07_concurrent_writer_during_quiesce_content_verification();
    test_audio_cmd_08_stop_timeout_late_worker_retry_reconciles_done();
    test_audio_cmd_09_worker_never_observes_quiesce_before_write_barrier_closed();

    CHECK(test_task_invalid_notify_count() == 0,
          "FINAL SUITE INVARIANT: test_task_invalid_notify_count == 0 across full test suite");

    if (failures != 0) {
        printf("\nFAILED: %d check(s) failed\n", failures);
        return 1;
    }
    printf("\nPASSED: all AUDIO-Q, AUDIO-INIT-FAIL, AUDIO-STATE, AUDIO-CMD checks passed\n");
    return 0;
}

