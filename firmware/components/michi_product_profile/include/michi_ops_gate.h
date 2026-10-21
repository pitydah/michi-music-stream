#pragma once

/* Receiver operational authority (H23 Phase 1).
 *
 * Two independent conditions decide whether this unit may play audio:
 *
 *   1. hardware truth  - is there a usable audio output? (michi_product_profile)
 *   2. operational trust - did this boot prove itself, and is nothing holding
 *                          the receiver back? (this gate)
 *
 * They are deliberately separate: a board can have a perfectly good DAC and
 * still be forbidden to play because the running image has not been validated,
 * and a fully trusted boot on a board without a DAC must still serve
 * diagnostics.
 *
 * WHY THIS LIVES IN A SEPARATE, DEPENDENCY-FREE MODULE
 * ---------------------------------------------------
 * The same predicate has to be evaluated by the capability advertisement
 * (michi_product_profile, deliberately pure C), the HTTP handlers, the session
 * subsystem, the audio engine and the OTA start path. Putting the state in
 * michi_state would drag FreeRTOS into the pure capability tables and into the
 * host parity tests, and putting it in any one consumer would let the others
 * drift. This module therefore depends on nothing but C11 atomics, so every
 * layer can include it and the host tests can drive it directly.
 *
 * DEFAULT IS DENY
 * ---------------
 * The state starts at MICHI_OPS_BOOT_PENDING. HTTP is initialised before the
 * boot health gates finish, so a request that arrives early must be refused
 * rather than allowed to open a session on an unvalidated image. Nothing can
 * enable playback implicitly: only an explicit transition to READY does, and
 * RESTRICTED can only be left through michi_ops_recover().
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* Health gates have not finished. Deny playback. */
    MICHI_OPS_BOOT_PENDING = 0,
    /* Boot validated, audio usable: full service. */
    MICHI_OPS_READY,
    /* Boot validated but no usable audio output: diagnostics only. */
    MICHI_OPS_DIAGNOSTIC,
    /* Security restriction (e.g. unprovable OTA metadata): deny playback,
     * keep diagnostics. Leaves only through michi_ops_recover(). */
    MICHI_OPS_RESTRICTED,
    /* An essential subsystem failed. Terminal until reboot. */
    MICHI_OPS_FATAL,
    /* An update owns the device; no new playback. */
    MICHI_OPS_UPDATING,
} michi_ops_state_t;

/** Reset to MICHI_OPS_BOOT_PENDING. Idempotent; also used by host tests. */
void michi_ops_init(void);

/**
 * @brief Move the authority to @p state.
 *
 * Transition rules (fail-closed):
 *   - BOOT_PENDING may go anywhere (that is the boot decision).
 *   - Any state may tighten to RESTRICTED / FATAL / UPDATING.
 *   - READY and DIAGNOSTIC may swap (audio availability changed at runtime).
 *   - UPDATING may return to READY / DIAGNOSTIC when the update finishes.
 *   - FATAL is terminal until reboot.
 *   - RESTRICTED does NOT leave through this call: use michi_ops_recover().
 *
 * @return ESP_OK_equivalent 0 on success, non-zero when refused.
 */
int michi_ops_set(michi_ops_state_t state);

/**
 * @brief Authorised recovery out of MICHI_OPS_RESTRICTED.
 *
 * Only valid from RESTRICTED and only towards READY or DIAGNOSTIC. The caller
 * is responsible for having completed the recovery requirements (re-probed the
 * hardware, re-classified the boot, re-verified identity); this call cannot
 * check them, so it is deliberately separate from michi_ops_set().
 *
 * @return 0 on success, non-zero when refused.
 */
int michi_ops_recover(michi_ops_state_t target);

/** Current state (lock-free read, safe from any task/context). */
michi_ops_state_t michi_ops_get(void);

/** True only for MICHI_OPS_READY: the single playback gate. */
bool michi_ops_playback_allowed(void);

/**
 * @brief May the device write firmware now?
 *
 * Allowed in READY and DIAGNOSTIC (a DAC-less board must remain updatable) and
 * denied while the boot is unvalidated, restricted, fatal or already updating:
 * writing an image whose state cannot be proven is how a diagnosable unit turns
 * into a bricked one.
 */
bool michi_ops_ota_allowed(void);

/** Short name for logs and diagnostics. Never NULL. */
const char *michi_ops_state_name(michi_ops_state_t state);

#ifdef MICHI_HOST_TEST
/**
 * @brief Host-only hook: runs between validation and the swap in
 *        michi_ops_set(). Lets a test force the lost-update window that
 *        compare-and-swap is there to close. Not compiled into the firmware.
 */
void test_ops_set_between_validate_and_swap(void (*fn)(void));
#endif

#ifdef __cplusplus
}
#endif
