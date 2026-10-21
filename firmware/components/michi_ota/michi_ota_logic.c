#include "michi_ota_logic.h"
#include <string.h>

ota_start_gate_t ota_gate_check(bool task_running, bool is_pending_verify)
{
    if (task_running) {
        return OTA_START_BUSY;
    }
    if (is_pending_verify) {
        return OTA_START_PENDING_VERIFY;
    }
    return OTA_START_OK;
}

int nvs_version_cmp_to_running(const char *nvs_ver, const char *running_ver)
{
    semver_t pend, run;
    if (!semver_parse(nvs_ver, &pend) || !semver_parse(running_ver, &run)) {
        return PARSE_FAILED;
    }
    return semver_cmp(&pend, &run);
}

bool latch_should_block(const char *pending_version,
                        const char *running_version,
                        const char *target_version)
{
    semver_t running, target;
    if (!semver_parse(running_version, &running) ||
        !semver_parse(target_version, &target)) {
        /* Unparseable version strings: be conservative and block. */
        return true;
    }

    /* Basic anti-downgrade: target must be strictly newer than running. */
    if (semver_cmp(&target, &running) <= 0) {
        return true;
    }

    /* Latch idempotency guard: if the latch records a version we are already
     * running (pending == running), and the target is not strictly newer than
     * that, block (we already have it). */
    semver_t pending;
    if (semver_parse(pending_version, &pending)) {
        if (semver_cmp(&pending, &running) == 0 &&
            semver_cmp(&target, &pending) <= 0) {
            return true;
        }
    }

    return false;
}

bool michi_ota_decide_expected_audio(bool sku_expects_audio,
                                     const char *configured_profile,
                                     michi_dac_profile_source_t source)
{
    /* 1. Explicit profile configured via HW_ID, NVS, or Kconfig always expects audio */
    if (configured_profile != NULL && configured_profile[0] != '\0') {
        return true;
    }
    /* 2. Autodetect target: if hardware profile resolution assigned AUTODETECT
     *    and the SKU expectation is an audio unit, audio is expected regardless
     *    of runtime detection outcome */
    if (source == MICHI_DAC_PROFILE_SOURCE_AUTODETECT && sku_expects_audio) {
        return true;
    }
    /* 3. Base build-time SKU expectation */
    return sku_expects_audio;
}

ota_selftest_res_t evaluate_trial_boot_gate(bool critical_checks_ok,
                                            bool trial_boot,
                                            bool expected_audio,
                                            bool audio_available)
{
    if (!critical_checks_ok) {
        return OTA_SELFTEST_FATAL;
    }
    if (!audio_available) {
        /* FATAL only where it buys a rollback. Outside a trial boot a restart
         * loops forever with nothing to fall back to, so a DAC-less unit would
         * never reach diagnostics: degrade honestly instead. */
        if (expected_audio && trial_boot) {
            return OTA_SELFTEST_FATAL;
        }
        return OTA_SELFTEST_DEGRADED;
    }
    return OTA_SELFTEST_PASS;
}

/* =========================================================================
 * H23-A boot classification and policy
 * ========================================================================= */

michi_ota_read_result_t michi_ota_normalize_read_result(int err_code)
{
    /* Values from esp_err.h (IDF 5.3.5). Kept as literals so this file stays
     * free of ESP headers; the host test asserts them against the header
     * values documented above. */
    switch (err_code) {
    case 0:      /* ESP_OK */
        return MICHI_OTA_READ_OK;
    case 0x105:  /* ESP_ERR_NOT_FOUND */
        return MICHI_OTA_READ_NOT_FOUND;
    case 0x106:  /* ESP_ERR_NOT_SUPPORTED */
        return MICHI_OTA_READ_NOT_SUPPORTED;
    case 0x102:  /* ESP_ERR_INVALID_ARG */
        return MICHI_OTA_READ_INVALID_ARG;
    default:     /* ESP_FAIL and anything unexpected: nothing can be proven */
        return MICHI_OTA_READ_ERROR;
    }
}

michi_ota_image_state_t michi_ota_normalize_image_state(unsigned int state_code)
{
    /* Values from esp_flash_partitions.h (esp_ota_img_states_t). */
    switch (state_code) {
    case 0x0u:
        return MICHI_OTA_IMG_NEW;
    case 0x1u:
        return MICHI_OTA_IMG_PENDING_VERIFY;
    case 0x2u:
        return MICHI_OTA_IMG_VALID;
    case 0x3u:
        return MICHI_OTA_IMG_INVALID;
    case 0x4u:
        return MICHI_OTA_IMG_ABORTED;
    case 0xFFFFFFFFu:
        return MICHI_OTA_IMG_UNDEFINED;
    default:
        return MICHI_OTA_IMG_UNKNOWN;
    }
}

const char *michi_boot_ota_state_name(michi_boot_ota_state_t state)
{
    switch (state) {
    case MICHI_BOOT_OTA_NORMAL:
        return "normal";
    case MICHI_BOOT_OTA_PENDING_VERIFY:
        return "pending_verify";
    case MICHI_BOOT_OTA_UNINITIALIZED:
        return "uninitialized";
    case MICHI_BOOT_OTA_UNKNOWN_ERROR:
    default:
        return "unknown_error";
    }
}

bool michi_boot_ota_rollback_available(michi_boot_ota_state_t state)
{
    return state == MICHI_BOOT_OTA_PENDING_VERIFY;
}

michi_boot_ota_state_t michi_ota_classify_boot_state(
    bool running_known,
    bool running_is_ota_slot,
    michi_ota_read_result_t read_result,
    michi_ota_image_state_t image_state)
{
    /* Nothing is known about the running partition: prove nothing. */
    if (!running_known) {
        return MICHI_BOOT_OTA_UNKNOWN_ERROR;
    }

    /* A factory image is confirmed by definition and has no OTA metadata; the
     * IDF reports exactly that as NOT_SUPPORTED. Any other combination with a
     * non-OTA slot means the inputs disagree, which is an unknown state. */
    if (!running_is_ota_slot) {
        return read_result == MICHI_OTA_READ_NOT_SUPPORTED
                   ? MICHI_BOOT_OTA_NORMAL
                   : MICHI_BOOT_OTA_UNKNOWN_ERROR;
    }

    switch (read_result) {
    case MICHI_OTA_READ_OK:
        break;
    case MICHI_OTA_READ_NOT_FOUND:
        /* No OTA history: first USB install, erased otadata, or this slot has
         * no recorded entry. There is no rollback target - and, above all,
         * this is NOT a pending verification. */
        return MICHI_BOOT_OTA_UNINITIALIZED;
    case MICHI_OTA_READ_NOT_SUPPORTED:
        /* An OTA slot that the IDF refuses to describe: inconsistent. */
        return MICHI_BOOT_OTA_UNKNOWN_ERROR;
    case MICHI_OTA_READ_INVALID_ARG:
    case MICHI_OTA_READ_ERROR:
    default:
        return MICHI_BOOT_OTA_UNKNOWN_ERROR;
    }

    switch (image_state) {
    case MICHI_OTA_IMG_PENDING_VERIFY:
        return MICHI_BOOT_OTA_PENDING_VERIFY;
    case MICHI_OTA_IMG_VALID:
        return MICHI_BOOT_OTA_NORMAL;
    case MICHI_OTA_IMG_UNDEFINED:
        /* Readable but unset: same meaning as no history. */
        return MICHI_BOOT_OTA_UNINITIALIZED;
    case MICHI_OTA_IMG_NEW:
    case MICHI_OTA_IMG_INVALID:
    case MICHI_OTA_IMG_ABORTED:
        /* Running an image that the metadata calls new, rejected or aborted is
         * self-contradictory: never act on it as if it were trusted. */
        return MICHI_BOOT_OTA_UNKNOWN_ERROR;
    case MICHI_OTA_IMG_UNKNOWN:
    default:
        return MICHI_BOOT_OTA_UNKNOWN_ERROR;
    }
}

michi_boot_decision_t michi_boot_decide(michi_boot_ota_state_t boot_state,
                                        bool critical_checks_ok,
                                        bool expected_audio,
                                        bool audio_available)
{
    michi_boot_decision_t d = {
        .verdict = OTA_SELFTEST_DEGRADED,
        .report_verdict = false,
        .restart_for_rollback = false,
        .restricted = false,
    };

    /* Unprovable image: report nothing (neither confirm nor roll back), never
     * restart. Keep the unit reachable for diagnostics with no operational
     * services. The verdict still reflects the self-test so a real critical
     * failure is not hidden behind the unknown OTA state. */
    if (boot_state == MICHI_BOOT_OTA_UNKNOWN_ERROR) {
        d.verdict = critical_checks_ok ? OTA_SELFTEST_DEGRADED : OTA_SELFTEST_FATAL;
        d.report_verdict = false;
        d.restart_for_rollback = false;
        d.restricted = true;
        return d;
    }

    const bool rollback_available = michi_boot_ota_rollback_available(boot_state);

    if (!critical_checks_ok) {
        d.verdict = OTA_SELFTEST_FATAL;
        /* Report it: on a pending image this is what triggers the rollback; on
         * any other class michi_ota_boot_selftest_done() is a noop. */
        d.report_verdict = true;
        /* Only restart where it achieves a rollback; otherwise it would be an
         * endless loop with no recovery target. */
        d.restart_for_rollback = rollback_available;
        d.restricted = !rollback_available;
        return d;
    }

    if (audio_available) {
        d.verdict = OTA_SELFTEST_PASS;
        d.report_verdict = true;
        return d;
    }

    /* Audio missing. On a pending image an audio SKU must not be confirmed, or
     * the rollback is cancelled for a build that cannot do its job. The verdict
     * must be reported: michi_ota_boot_selftest_done() is what marks the image
     * invalid and restarts into the rollback. */
    if (expected_audio && rollback_available) {
        d.verdict = OTA_SELFTEST_FATAL;
        d.report_verdict = true;
        d.restart_for_rollback = true;
        return d;
    }

    /* Everything else degrades honestly: normal boot without a DAC, or a
     * diagnostic SKU. DEGRADED may be confirmed (nothing is pending or the
     * image is acceptable for this SKU) and never restarts. */
    d.verdict = OTA_SELFTEST_DEGRADED;
    d.report_verdict = true;
    d.restart_for_rollback = false;
    d.restricted = false;
    return d;
}
