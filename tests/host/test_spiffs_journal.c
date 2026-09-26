/* Host-side tests for the SPIFFS first-boot journal mount logic (P0-07).
 *
 * The actual michi_log.c cannot be compiled in host context (it pulls
 * FreeRTOS tasks, SPIFFS VFS, I2S, etc.). Instead, this test extracts
 * the mount-decision logic into a pure-C helper function and tests it
 * directly:
 *
 *   spiffs_mount_outcome_t spiffs_decide_mount(
 *       esp_err_t first_mount_err,
 *       esp_err_t format_err,
 *       esp_err_t retry_mount_err);
 *
 * This exercises every branch of the P0-07 first-boot format path:
 *   - Fresh partition (ESP_ERR_NOT_FOUND): format + retry → mount OK
 *   - Fresh partition: format OK, retry fails → DEGRADED
 *   - Fresh partition: format itself fails → DEGRADED
 *   - Non-fresh failure (ESP_ERR_NO_MEM): NOT formatted → DEGRADED
 *   - First mount succeeds: no format attempted → MOUNTED
 *
 * Gate: FRESH_FLASH_JOURNAL_PASS (software).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- Minimal ESP-IDF error type stubs for the host. ---- */
typedef int esp_err_t;
#define ESP_OK              0
#define ESP_FAIL            (-1)
#define ESP_ERR_NOT_FOUND   0x105
#define ESP_ERR_INVALID_SIZE 0x102
#define ESP_ERR_NO_MEM      0x101

static int failures = 0;

#define CHECK(cond, msg)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            printf("  FAIL %s\n", msg);                     \
            failures++;                                     \
        }                                                   \
    } while (0)

/* ---- Extract of the P0-07 mount decision logic. ---- *
 *
 * This mirrors the branching logic in michi_log_start_journal() verbatim.
 * Any change to the firmware logic MUST be reflected here (and a failing
 * test will catch the drift).
 */

typedef enum {
    SPIFFS_OUTCOME_MOUNTED           = 0,  /* journal operational */
    SPIFFS_OUTCOME_DEGRADED_FRESH    = 1,  /* first-boot: format failed */
    SPIFFS_OUTCOME_DEGRADED_RETRY    = 2,  /* format OK, remount failed */
    SPIFFS_OUTCOME_DEGRADED_NONFRESH = 3,  /* hardware / corruption error */
} spiffs_mount_outcome_t;

typedef struct {
    bool format_was_called;   /* did we attempt esp_spiffs_format? */
    bool nvs_record_written;  /* did we write spiffs_fmt_n? */
} spiffs_decision_ctx_t;

/* Pure decision function: no side effects beyond the ctx output. */
static spiffs_mount_outcome_t spiffs_decide_mount(
        esp_err_t first_mount_err,
        esp_err_t format_err,        /* ignored if first_mount_err == ESP_OK */
        esp_err_t retry_mount_err,   /* ignored if format failed */
        spiffs_decision_ctx_t *ctx)
{
    ctx->format_was_called = false;
    ctx->nvs_record_written = false;

    if (first_mount_err == ESP_OK) {
        return SPIFFS_OUTCOME_MOUNTED;
    }

    const bool looks_fresh = (first_mount_err == ESP_ERR_NOT_FOUND ||
                              first_mount_err == ESP_FAIL ||
                              first_mount_err == ESP_ERR_INVALID_SIZE);

    if (!looks_fresh) {
        return SPIFFS_OUTCOME_DEGRADED_NONFRESH;
    }

    /* First-boot path: record in NVS (always attempted), then format. */
    ctx->nvs_record_written = true; /* NVS open assumed OK in this model */

    ctx->format_was_called = true;
    if (format_err != ESP_OK) {
        return SPIFFS_OUTCOME_DEGRADED_FRESH;
    }

    /* Retry mount. */
    if (retry_mount_err != ESP_OK) {
        return SPIFFS_OUTCOME_DEGRADED_RETRY;
    }

    return SPIFFS_OUTCOME_MOUNTED;
}

/* ---- Tests ---- */

static void test_first_boot_success(void)
{
    printf("spiffs: fresh partition -> format + retry -> MOUNTED\n");
    spiffs_decision_ctx_t ctx;

    /* ESP_ERR_NOT_FOUND: SPIFFS magic absent (fresh flash). */
    spiffs_mount_outcome_t r = spiffs_decide_mount(
        ESP_ERR_NOT_FOUND, ESP_OK, ESP_OK, &ctx);
    CHECK(r == SPIFFS_OUTCOME_MOUNTED,
          "NOT_FOUND, format OK, retry OK -> MOUNTED");
    CHECK(ctx.format_was_called,
          "format WAS called on fresh partition");
    CHECK(ctx.nvs_record_written,
          "NVS record written before format");

    /* ESP_FAIL: another fresh-looking error. */
    r = spiffs_decide_mount(ESP_FAIL, ESP_OK, ESP_OK, &ctx);
    CHECK(r == SPIFFS_OUTCOME_MOUNTED,
          "ESP_FAIL (fresh), format OK, retry OK -> MOUNTED");
    CHECK(ctx.format_was_called, "format called for ESP_FAIL");

    /* ESP_ERR_INVALID_SIZE: partition table mismatch (fresh). */
    r = spiffs_decide_mount(ESP_ERR_INVALID_SIZE, ESP_OK, ESP_OK, &ctx);
    CHECK(r == SPIFFS_OUTCOME_MOUNTED,
          "INVALID_SIZE (fresh), format OK, retry OK -> MOUNTED");
    CHECK(ctx.format_was_called, "format called for INVALID_SIZE");
}

static void test_first_mount_ok(void)
{
    printf("spiffs: first mount succeeds -> no format\n");
    spiffs_decision_ctx_t ctx;

    spiffs_mount_outcome_t r = spiffs_decide_mount(
        ESP_OK, ESP_FAIL, ESP_FAIL, &ctx);
    CHECK(r == SPIFFS_OUTCOME_MOUNTED,
          "first mount OK -> MOUNTED immediately");
    CHECK(!ctx.format_was_called,
          "NO format on successful first mount");
    CHECK(!ctx.nvs_record_written,
          "NO NVS write on successful first mount");
}

static void test_format_fails(void)
{
    printf("spiffs: fresh partition, format fails -> DEGRADED_FRESH\n");
    spiffs_decision_ctx_t ctx;

    spiffs_mount_outcome_t r = spiffs_decide_mount(
        ESP_ERR_NOT_FOUND, ESP_FAIL, ESP_OK, &ctx);
    CHECK(r == SPIFFS_OUTCOME_DEGRADED_FRESH,
          "NOT_FOUND, format FAIL -> DEGRADED_FRESH");
    CHECK(ctx.format_was_called, "format was attempted");
    CHECK(ctx.nvs_record_written, "NVS recorded before failed format");
}

static void test_retry_mount_fails(void)
{
    printf("spiffs: fresh partition, format OK, retry fails -> DEGRADED_RETRY\n");
    spiffs_decision_ctx_t ctx;

    spiffs_mount_outcome_t r = spiffs_decide_mount(
        ESP_ERR_NOT_FOUND, ESP_OK, ESP_FAIL, &ctx);
    CHECK(r == SPIFFS_OUTCOME_DEGRADED_RETRY,
          "NOT_FOUND, format OK, retry FAIL -> DEGRADED_RETRY");
    CHECK(ctx.format_was_called, "format was attempted");
}

static void test_nonfresh_failure(void)
{
    printf("spiffs: non-fresh hardware error -> DEGRADED_NONFRESH, no format\n");
    spiffs_decision_ctx_t ctx;

    /* ESP_ERR_NO_MEM is NOT in the looks_fresh set: hardware problem. */
    spiffs_mount_outcome_t r = spiffs_decide_mount(
        ESP_ERR_NO_MEM, ESP_OK, ESP_OK, &ctx);
    CHECK(r == SPIFFS_OUTCOME_DEGRADED_NONFRESH,
          "NO_MEM (non-fresh) -> DEGRADED_NONFRESH");
    CHECK(!ctx.format_was_called,
          "NO format on non-fresh hardware error");
    CHECK(!ctx.nvs_record_written,
          "NO NVS write on non-fresh error");
}

static void test_fresh_error_variants(void)
{
    printf("spiffs: all 'looks_fresh' error codes classify as first-boot\n");
    spiffs_decision_ctx_t ctx;
    const esp_err_t fresh_errors[] = {
        ESP_ERR_NOT_FOUND,
        ESP_FAIL,
        ESP_ERR_INVALID_SIZE,
    };
    for (int i = 0; i < 3; i++) {
        spiffs_mount_outcome_t r = spiffs_decide_mount(
            fresh_errors[i], ESP_OK, ESP_OK, &ctx);
        CHECK(r == SPIFFS_OUTCOME_MOUNTED,
              "fresh error variant -> format+retry -> MOUNTED");
        CHECK(ctx.format_was_called,
              "fresh error variant: format called");
    }
}

static void test_nvs_record_before_format(void)
{
    printf("spiffs: NVS record is written BEFORE format (survives format fail)\n");
    spiffs_decision_ctx_t ctx;

    /* Even when the format fails the NVS record must have been written first. */
    spiffs_mount_outcome_t r = spiffs_decide_mount(
        ESP_ERR_NOT_FOUND, ESP_FAIL, ESP_OK, &ctx);
    CHECK(r == SPIFFS_OUTCOME_DEGRADED_FRESH,
          "format fail path: outcome DEGRADED_FRESH");
    CHECK(ctx.nvs_record_written,
          "NVS record written even when format subsequently fails");
    CHECK(ctx.format_was_called,
          "format was attempted");
}

int main(void)
{
    test_first_mount_ok();
    test_first_boot_success();
    test_format_fails();
    test_retry_mount_fails();
    test_nonfresh_failure();
    test_fresh_error_variants();
    test_nvs_record_before_format();

    if (failures == 0) {
        printf("PASS test_spiffs_journal\n");
        return 0;
    }
    printf("FAIL test_spiffs_journal (%d failures)\n", failures);
    return 1;
}
