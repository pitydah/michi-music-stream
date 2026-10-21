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

/* =========================================================================
 * H23-A boot classification.
 *
 * The boot decision used to be a bool ("pending verify?"), and any failure to
 * read the OTA state was folded into "pending". That conflates three very
 * different situations: an image awaiting verification (rollback available), a
 * device with no OTA history at all (first USB flash: ESP_ERR_NOT_FOUND or
 * ESP_ERR_NOT_SUPPORTED), and a genuine read failure where nothing about the
 * image can be proven. Only the first one may restart for rollback; the others
 * must never loop.
 *
 * The pure layer below takes neutral inputs so the whole walk is host-testable;
 * michi_ota_classify_boot() is the thin adapter that performs the IDF calls.
 * ========================================================================= */

/** Running image state, mirrored from esp_ota_img_states_t (IDF-free). */
typedef enum {
    MICHI_OTA_IMG_UNKNOWN = 0,
    MICHI_OTA_IMG_NEW,            /* written but never booted */
    MICHI_OTA_IMG_PENDING_VERIFY, /* awaiting verification: rollback available */
    MICHI_OTA_IMG_VALID,          /* confirmed */
    MICHI_OTA_IMG_INVALID,        /* explicitly rejected */
    MICHI_OTA_IMG_ABORTED,        /* rolled back / aborted */
    MICHI_OTA_IMG_UNDEFINED,      /* no state recorded */
} michi_ota_image_state_t;

/** Normalised result of reading the image state (see esp_ota_get_state_partition). */
typedef enum {
    MICHI_OTA_READ_OK = 0,
    MICHI_OTA_READ_NOT_FOUND,      /* otadata absent/unreadable, or no entry for this slot */
    MICHI_OTA_READ_NOT_SUPPORTED,  /* running partition is not an OTA slot (factory image) */
    MICHI_OTA_READ_INVALID_ARG,
    MICHI_OTA_READ_ERROR,          /* any other failure: nothing can be proven */
} michi_ota_read_result_t;

/** Boot class: the only input the restart/rollback policy may act on. */
typedef enum {
    MICHI_BOOT_OTA_NORMAL = 0,     /* confirmed or factory image, rollback not applicable */
    MICHI_BOOT_OTA_PENDING_VERIFY, /* awaiting verification: a rollback target exists */
    MICHI_BOOT_OTA_UNINITIALIZED,  /* no OTA history (first USB install, erased otadata) */
    MICHI_BOOT_OTA_UNKNOWN_ERROR,  /* the image state cannot be proven */
} michi_boot_ota_state_t;

/** What app_main must do for this boot; produced by one pure decision call. */
typedef struct {
    ota_selftest_res_t verdict;  /* PASS / DEGRADED / FATAL for logs and gates */
    bool report_verdict;         /* hand the verdict to michi_ota_boot_selftest_done():
                                  * PASS/DEGRADED confirm the image, FATAL on a
                                  * pending image triggers the rollback. False only
                                  * when the image state is unprovable, because then
                                  * neither confirming nor rolling back is justified. */
    bool restart_for_rollback;   /* restart is justified: a rollback target exists */
    bool restricted;             /* safe diagnostic mode: no session, no OTA write */
} michi_boot_decision_t;

/** Short name for logs. */
const char *michi_boot_ota_state_name(michi_boot_ota_state_t state);

/** True only when a rollback target exists (image awaiting verification). */
bool michi_boot_ota_rollback_available(michi_boot_ota_state_t state);

/**
 * @brief Classifies the boot from the raw inputs the IDF layer observed.
 *
 * Contract: only MICHI_BOOT_OTA_PENDING_VERIFY can lead to a rollback restart.
 * A missing OTA history is UNINITIALIZED (first flash), never pending. Anything
 * inconsistent or unreadable is UNKNOWN_ERROR - never silently downgraded to a
 * normal boot, and never treated as pending either.
 */
michi_boot_ota_state_t michi_ota_classify_boot_state(
    bool running_known,
    bool running_is_ota_slot,
    michi_ota_read_result_t read_result,
    michi_ota_image_state_t image_state);

/**
 * @brief Normalise the raw IDF results into the neutral enums above.
 *
 * Exposed so the adapter's mapping is covered by host tests using the REAL
 * numeric codes rather than a reimplementation:
 *   esp_err.h:          ESP_OK 0, ESP_FAIL -1, ESP_ERR_INVALID_ARG 0x102,
 *                       ESP_ERR_NOT_FOUND 0x105, ESP_ERR_NOT_SUPPORTED 0x106
 *   esp_flash_partitions.h: NEW 0x0, PENDING_VERIFY 0x1, VALID 0x2,
 *                       INVALID 0x3, ABORTED 0x4, UNDEFINED 0xFFFFFFFF
 */
michi_ota_read_result_t michi_ota_normalize_read_result(int err_code);
michi_ota_image_state_t michi_ota_normalize_image_state(unsigned int state_code);

/**
 * @brief Full boot policy: boot class + self-test inputs -> verdict and actions.
 *
 * - Restart for rollback only on PENDING_VERIFY with a failed critical check or
 *   with audio expected but unavailable.
 * - On a normal/uninitialized boot a failed critical check does NOT restart:
 *   there is nowhere to roll back to, so it enters the restricted diagnostic
 *   mode and stays reachable instead of rebooting forever.
 * - UNKNOWN_ERROR never confirms the image and never restarts.
 */
michi_boot_decision_t michi_boot_decide(michi_boot_ota_state_t boot_state,
                                        bool critical_checks_ok,
                                        bool expected_audio,
                                        bool audio_available);
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
