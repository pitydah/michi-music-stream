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
                                            bool expected_audio,
                                            bool audio_available)
{
    if (!critical_checks_ok) {
        return OTA_SELFTEST_FATAL;
    }
    if (!audio_available) {
        if (expected_audio) {
            return OTA_SELFTEST_FATAL;
        }
        return OTA_SELFTEST_DEGRADED;
    }
    return OTA_SELFTEST_PASS;
}
