#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "semver.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_START_OK,
    OTA_START_BUSY,
    OTA_START_PENDING_VERIFY,
} ota_start_gate_t;

typedef enum {
    OTA_SELFTEST_PASS,
    OTA_SELFTEST_DEGRADED,
    OTA_SELFTEST_FATAL,
} ota_selftest_res_t;

typedef ota_selftest_res_t ota_test_selftest_res_t;
#define OTA_TEST_SELFTEST_PASS     OTA_SELFTEST_PASS
#define OTA_TEST_SELFTEST_DEGRADED OTA_SELFTEST_DEGRADED
#define OTA_TEST_SELFTEST_FATAL    OTA_SELFTEST_FATAL

#define PARSE_FAILED (-999)

ota_start_gate_t ota_gate_check(bool task_running, bool is_pending_verify);

int nvs_version_cmp_to_running(const char *nvs_ver, const char *running_ver);

bool latch_should_block(const char *pending_version,
                        const char *running_version,
                        const char *target_version);

ota_selftest_res_t evaluate_trial_boot_gate(bool critical_checks_ok,
                                            bool expected_audio,
                                            bool audio_available);

#ifdef __cplusplus
}
#endif
