#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "michi_audio.h"
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

static void test_buf_01_50ms(void)
{
    printf("BUF-01: 50 ms -> 5 packets\n");
    uint32_t pkts = michi_audio_calculate_prefill_target(50);
    CHECK(pkts == 5, "50 ms produces exactly 5 packets");
}

static void test_buf_02_300ms(void)
{
    printf("BUF-02: 300 ms -> 30 packets\n");
    uint32_t pkts = michi_audio_calculate_prefill_target(300);
    CHECK(pkts == 30, "300 ms produces exactly 30 packets");
}

static void test_buf_03_500ms(void)
{
    printf("BUF-03: 500 ms -> 50 packets\n");
    uint32_t pkts = michi_audio_calculate_prefill_target(500);
    CHECK(pkts == 50, "500 ms produces exactly 50 packets");
}

static void test_buf_04_recovery_respects_target(void)
{
    printf("BUF-04: recovery respects target\n");
    /* Recovery deadline must give enough time to receive target buffer without premature timeout */
    uint32_t d50 = michi_audio_recovery_deadline_ms(50);
    CHECK(d50 >= 50, "recovery deadline for 50 ms is at least 50 ms");
    CHECK(d50 <= 1000, "recovery deadline for 50 ms is bounded");

    uint32_t d300 = michi_audio_recovery_deadline_ms(300);
    CHECK(d300 >= 300, "recovery deadline for 300 ms is at least 300 ms");

    uint32_t d500 = michi_audio_recovery_deadline_ms(500);
    CHECK(d500 >= 500, "recovery deadline for 500 ms is at least 500 ms");
    CHECK(d500 <= 1000, "recovery deadline for 500 ms is bounded");

    /* Recovery prefill targets match negotiated buffer_ms */
    CHECK(michi_audio_calculate_prefill_target(50) == 5, "recovery target for 50 ms is 5 packets");
    CHECK(michi_audio_calculate_prefill_target(300) == 30, "recovery target for 300 ms is 30 packets");
    CHECK(michi_audio_calculate_prefill_target(500) == 50, "recovery target for 500 ms is 50 packets");
}

static void test_buf_05_invalid_49_rejected(void)
{
    printf("BUF-05: invalid 49 rejected by audio engine\n");
    CHECK(michi_audio_validate_buffer_ms(49) == ESP_ERR_INVALID_ARG,
          "michi_audio_validate_buffer_ms(49) returns ESP_ERR_INVALID_ARG");
    CHECK(michi_audio_validate_buffer_ms(0) == ESP_ERR_INVALID_ARG,
          "michi_audio_validate_buffer_ms(0) returns ESP_ERR_INVALID_ARG");
}

static void test_buf_06_invalid_501_rejected(void)
{
    printf("BUF-06: invalid 501 rejected\n");
    CHECK(michi_audio_validate_buffer_ms(501) == ESP_ERR_INVALID_ARG,
          "michi_audio_validate_buffer_ms(501) returns ESP_ERR_INVALID_ARG");
    CHECK(michi_audio_validate_buffer_ms(1000) == ESP_ERR_INVALID_ARG,
          "michi_audio_validate_buffer_ms(1000) returns ESP_ERR_INVALID_ARG");
}

static void test_buf_07_engine_capacity_invariant(void)
{
    printf("BUF-07: engine capacity cannot silently clamp advertised contract\n");
    /* If build jitter buffer capacity is less than advertised max, init fails closed */
    CHECK(michi_audio_check_capacity_invariant(499, 500) == ESP_ERR_INVALID_STATE,
          "capacity 499 < advertised 500 returns ESP_ERR_INVALID_STATE");
    CHECK(michi_audio_check_capacity_invariant(100, 500) == ESP_ERR_INVALID_STATE,
          "capacity 100 < advertised 500 returns ESP_ERR_INVALID_STATE");

    /* Capacity meeting or exceeding advertised max passes */
    CHECK(michi_audio_check_capacity_invariant(500, 500) == ESP_OK,
          "capacity 500 == advertised 500 returns ESP_OK");
    CHECK(michi_audio_check_capacity_invariant(1000, 500) == ESP_OK,
          "capacity 1000 >= advertised 500 returns ESP_OK");

    /* Canonical contract bounds match constants */
    CHECK(MICHI_AUDIO_BUFFER_MS_MIN == 50, "MICHI_AUDIO_BUFFER_MS_MIN is 50");
    CHECK(MICHI_AUDIO_BUFFER_MS_MAX == 500, "MICHI_AUDIO_BUFFER_MS_MAX is 500");

    /* Full capacity ensures 50 packets for 500 ms without clamping */
    CHECK(michi_audio_calculate_prefill_target_ext(500, 50) == 50,
          "500 ms with 50 capacity yields 50 packets (not clamped)");
}

int main(void)
{
    printf("=== michi_audio buffer and recovery tests (BUF-01..BUF-07) ===\n");
    test_buf_01_50ms();
    test_buf_02_300ms();
    test_buf_03_500ms();
    test_buf_04_recovery_respects_target();
    test_buf_05_invalid_49_rejected();
    test_buf_06_invalid_501_rejected();
    test_buf_07_engine_capacity_invariant();

    if (failures != 0) {
        printf("\nFAILED: %d check(s) failed\n", failures);
        return 1;
    }
    printf("\nPASSED: all BUF-01..BUF-07 checks passed\n");
    return 0;
}
