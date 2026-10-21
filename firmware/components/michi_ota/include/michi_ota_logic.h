#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "semver.h"
#include "michi_dac_types.h"

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

/**
 * @brief Evaluates whether audio is expected for this device based on
 *        manufacturing SKU expectation and static configuration,
 *        completely decoupled from runtime autodetection outcome.
 */
bool michi_ota_decide_expected_audio(bool sku_expects_audio,
                                     const char *configured_profile,
                                     michi_dac_profile_source_t source);

/**
 * @brief Trial-boot gate: decides whether this boot may cancel the pending
 *        OTA verification (PASS), must continue degraded (DEGRADED) or must
 *        refuse to mark the image valid so the bootloader rolls back (FATAL).
 *
 * @param critical_checks_ok  board/identity/http/state self-test verdict.
 * @param trial_boot          true only when the running image is in
 *                            ESP_OTA_IMG_PENDING_VERIFY, i.e. an update is
 *                            awaiting verification and FATAL has a rollback
 *                            target to fall back to.
 * @param expected_audio      manufacturing SKU / configured profile expects
 *                            working audio hardware.
 * @param audio_available     audio actually initialised.
 *
 * Missing audio is FATAL **only on a trial boot**, because that is the only
 * situation where refusing to cancel verification achieves something: the
 * bootloader rolls back to the previous image. On a normal boot there is no
 * rollback target, so halting would restart the device forever and hide the
 * diagnostics of an otherwise healthy unit (a DAC-less diagnostic board, for
 * instance). There the honest verdict is DEGRADED: audio_available=false and
 * the profile reports diagnostic.
 *
 * Critical self-test failures stay FATAL on every boot: a unit whose core
 * subsystems did not come up must not advertise itself as serviceable.
 */
ota_selftest_res_t evaluate_trial_boot_gate(bool critical_checks_ok,
                                            bool trial_boot,
                                            bool expected_audio,
                                            bool audio_available);

#ifdef __cplusplus
}
#endif
