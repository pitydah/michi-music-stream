/* Host-side tests for the button gesture contract (P0-01 + P1-07).
 *
 * Compiles the REAL firmware gesture module (michi_button_gesture.c -
 * the exact decision code the debounce task runs, no reimplementation)
 * plus the REAL identity component (michi_identity.c + identity_nvs.c +
 * Monocypher/BLAKE3): the corrupt-identity recovery path is proven end
 * to end. The pairing side is a call-counter test double.
 *
 * NEW CONTRACT (P0-01) — hold-on-threshold:
 *   < PAIRING_HOLD_MS (5000)   → no action yet (NONE)
 *   at PAIRING_HOLD_MS         → PAIRING fires (threshold crossing)
 *   > PAIRING_HOLD_MS, same press → still NONE (gesture consumed)
 *   at FACTORY_WARN_MS (10000) → FACTORY_WARN (visual, not consumed)
 *   at FACTORY_RESET_MS (15000) → FACTORY_RESET
 *   In RECOVERABLE_ERROR: PAIRING_HOLD_MS threshold → RECOVERY
 *   Protected state (BOOTING/SELF_TEST/UPDATING): IGNORED_PROTECTED
 *   Boot-hold (arm window not met): IGNORED_ARM for factory reset
 *
 * Gate: PAIRING_BUTTON_5S_PASS
 */

#include <stdio.h>
#include <string.h>

#include "michi_button_gesture.h"
#include "michi_identity.h"
#include "identity_storage.h"
#include "michi_pairing_fake.h"
#include "michi_state.h"   /* shim: test_state_set */
#include "nvs.h"           /* fake NVS shim */
#include "nvs_flash.h"     /* shim: erase counter */
#include "esp_system.h"    /* shim: restart counter */
#include "sdkconfig.h"     /* Kconfig defaults under test */

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static const int64_t PAIRING_MS   = CONFIG_MICHI_BUTTON_PAIRING_HOLD_MS;
static const int64_t WARN_MS      = CONFIG_MICHI_BUTTON_FACTORY_WARN_MS;
static const int64_t FACTORY_MS   = CONFIG_MICHI_BUTTON_FACTORY_RESET_PRESS_MS;
static const int64_t ARM_MS       = CONFIG_MICHI_BUTTON_FACTORY_ARM_MS;

/* Build an idle ctx (no previous action, not protected). */
static michi_button_press_ctx_t make_ctx(michi_state_t press_st,
                                         int64_t press_boot_ms,
                                         bool action_fired,
                                         bool factory_warned)
{
    michi_button_press_ctx_t c = {0};
    c.pressed        = true;
    c.action_fired   = action_fired;
    c.factory_warned = factory_warned;
    c.press_state    = press_st;
    c.press_boot_ms  = press_boot_ms;
    c.pressed_at_us  = 0; /* elapsed passed separately */
    return c;
}

static michi_button_action_t hold_cls(int64_t elapsed_ms,
                                      michi_state_t press_st,
                                      michi_state_t cur_st,
                                      int64_t boot_ms,
                                      bool action_fired,
                                      bool factory_warned)
{
    michi_button_press_ctx_t ctx = make_ctx(press_st, boot_ms,
                                            action_fired, factory_warned);
    return michi_button_hold_classify(
        elapsed_ms, press_st, cur_st, boot_ms, &ctx,
        (uint32_t)PAIRING_MS, (uint32_t)WARN_MS, (uint32_t)FACTORY_MS,
        (uint32_t)ARM_MS);
}

/* Convenience: idle state, not consumed. */
static michi_button_action_t simple_cls(int64_t elapsed_ms,
                                        michi_state_t st)
{
    return hold_cls(elapsed_ms, st, st, (int64_t)ARM_MS + 1000, false, false);
}

static void test_reset_all(void)
{
    test_nvs_reset();
    test_nvs_flash_erase_count_reset();
    test_esp_restart_count_reset();
    test_pairing_fake_reset();
    test_state_reset();
    michi_identity_test_reset();
}

/* Seed a structurally wrong identity blob (8 bytes instead of 40). */
static void seed_corrupt_identity_store(void)
{
    nvs_handle_t h;
    CHECK(nvs_open(MICHI_IDENTITY_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK,
          "corrupt seed: open identity ns");
    const uint8_t bad[8] = {0xBA, 0xD0, 0x5E, 0xED, 0xBA, 0xD0, 0x5E, 0xED};
    CHECK(nvs_set_blob(h, MICHI_IDENTITY_NVS_KEY, bad, sizeof(bad)) == ESP_OK,
          "corrupt seed: write wrong-length blob");
    CHECK(nvs_commit(h) == ESP_OK, "corrupt seed: commit");
    nvs_close(h);
}

/* --- Test: hold-on-threshold exact boundaries (P0-01 gate) --- */
static void test_hold_threshold_exact(void)
{
    printf("button: P0-01 hold-on-threshold exact boundaries\n");

    /* 4999 ms: no action yet. */
    CHECK(simple_cls(4999, MICHI_STATE_IDLE) == MICHI_BUTTON_ACTION_NONE,
          "4999 ms -> NONE (below pairing threshold)");

    /* 5000 ms: pairing fires (threshold crossing). */
    CHECK(simple_cls(5000, MICHI_STATE_IDLE) == MICHI_BUTTON_ACTION_PAIRING,
          "5000 ms -> PAIRING (threshold crossing)");

    /* 5001 ms: still PAIRING (not yet consumed in simple_cls). */
    CHECK(simple_cls(5001, MICHI_STATE_IDLE) == MICHI_BUTTON_ACTION_PAIRING,
          "5001 ms -> PAIRING (still above threshold, not consumed)");

    /* After pairing fires (action_fired=true): NONE at 5500 ms. */
    CHECK(hold_cls(5500, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, true, false) ==
              MICHI_BUTTON_ACTION_NONE,
          "5500 ms after fire (consumed) -> NONE");

    /* Release after 5200 ms (consumed): no second fire. */
    CHECK(hold_cls(5200, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, true, false) ==
              MICHI_BUTTON_ACTION_NONE,
          "release 5200 ms after consumed -> NONE");

    /* 20 consecutive presses: each fires exactly once at 5000 ms. */
    printf("button: 20 consecutive hold-fire cycles\n");
    for (int i = 0; i < 20; i++) {
        /* Below threshold: NONE. */
        CHECK(simple_cls(4999, MICHI_STATE_IDLE) == MICHI_BUTTON_ACTION_NONE,
              "cycle: 4999 ms -> NONE");
        /* At threshold: PAIRING. */
        CHECK(simple_cls(5000, MICHI_STATE_IDLE) == MICHI_BUTTON_ACTION_PAIRING,
              "cycle: 5000 ms -> PAIRING");
        /* After consumed: NONE. */
        CHECK(hold_cls(5500, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                       (int64_t)ARM_MS + 1000, true, false) ==
                  MICHI_BUTTON_ACTION_NONE,
              "cycle: consumed -> NONE");
    }
}

/* --- Test: RECOVERABLE_ERROR context (recovery fires, not pairing) --- */
static void test_recovery_context(void)
{
    printf("button: recovery in RECOVERABLE_ERROR context\n");

    /* Below threshold: NONE. */
    CHECK(hold_cls(4999, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR,
                   (int64_t)ARM_MS + 1000, false, false) ==
              MICHI_BUTTON_ACTION_NONE,
          "RECOVERABLE: 4999 ms -> NONE");

    /* At threshold in RECOVERABLE_ERROR: RECOVERY, not PAIRING. */
    CHECK(hold_cls(5000, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR,
                   (int64_t)ARM_MS + 1000, false, false) ==
              MICHI_BUTTON_ACTION_RECOVERY,
          "RECOVERABLE: 5000 ms -> RECOVERY");

    /* IDLE state with same elapsed: PAIRING, not RECOVERY. */
    CHECK(simple_cls(5000, MICHI_STATE_IDLE) == MICHI_BUTTON_ACTION_PAIRING,
          "IDLE: 5000 ms -> PAIRING (not recovery)");

    /* After recovery consumed: NONE. */
    CHECK(hold_cls(7000, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR,
                   (int64_t)ARM_MS + 1000, true, false) ==
              MICHI_BUTTON_ACTION_NONE,
          "RECOVERABLE consumed: 7000 ms -> NONE");
}

/* --- Test: factory reset warning (10s) and fire (15s) --- */
static void test_factory_reset_escalonado(void)
{
    printf("button: factory reset escalonado (warn 10s, fire 15s)\n");

    /* 9999 ms (warn not yet reached, but pairing already crossed): PAIRING.
     * The action fires at 5000 ms and keeps returning PAIRING until consumed.
     * To test the warn threshold in isolation, use a consumed context. */
    CHECK(hold_cls(9999, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, true, false) ==
              MICHI_BUTTON_ACTION_NONE,
          "9999 ms (consumed, below warn) -> NONE");

    /* 10000 ms (warning threshold): FACTORY_WARN. */
    CHECK(hold_cls(10000, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, false, false) ==
              MICHI_BUTTON_ACTION_FACTORY_WARN,
          "10000 ms -> FACTORY_WARN");

    /* 10001 ms, already warned: NONE (visual already shown). */
    CHECK(hold_cls(10001, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, false, true) ==
              MICHI_BUTTON_ACTION_NONE,
          "10001 ms (already warned) -> NONE");

    /* 14999 ms (not yet fire threshold): NONE (already warned). */
    CHECK(hold_cls(14999, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, false, true) ==
              MICHI_BUTTON_ACTION_NONE,
          "14999 ms -> NONE (below factory threshold)");

    /* 15000 ms: FACTORY_RESET (armed, not consumed). */
    CHECK(hold_cls(15000, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, false, false) ==
              MICHI_BUTTON_ACTION_FACTORY_RESET,
          "15000 ms -> FACTORY_RESET");

    /* 15000 ms but action_fired (pairing consumed): blocked. */
    CHECK(hold_cls(15000, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, true, false) ==
              MICHI_BUTTON_ACTION_NONE,
          "15000 ms but gesture consumed -> NONE (no factory-reset escalation)");
}

/* --- Test: protected states (BOOTING/SELF_TEST/UPDATING) --- */
static void test_protected_states(void)
{
    printf("button: protected states (BOOTING/SELF_TEST/UPDATING)\n");

    static const michi_state_t protected[] = {
        MICHI_STATE_BOOTING, MICHI_STATE_SELF_TEST, MICHI_STATE_UPDATING,
    };
    static const char *const names[] = {"BOOTING", "SELF_TEST", "UPDATING"};

    for (int i = 0; i < 3; i++) {
        const michi_state_t p = protected[i];
        char msg[128];

        snprintf(msg, sizeof(msg), "%s: press started protected -> IGNORED",
                 names[i]);
        CHECK(hold_cls(15000, p, MICHI_STATE_IDLE,
                       (int64_t)ARM_MS + 1000, false, false) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED, msg);

        snprintf(msg, sizeof(msg), "%s: current state protected -> IGNORED",
                 names[i]);
        CHECK(hold_cls(15000, MICHI_STATE_IDLE, p,
                       (int64_t)ARM_MS + 1000, false, false) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED, msg);

        /* Critical: OTA mid-press. */
        snprintf(msg, sizeof(msg), "%s: OTA starts during hold -> IGNORED",
                 names[i]);
        CHECK(hold_cls(5000, MICHI_STATE_IDLE, p,
                       (int64_t)ARM_MS + 1000, false, false) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED, msg);
    }

    /* The critical case: press at IDLE, OTA starts before threshold. */
    CHECK(hold_cls(15000, MICHI_STATE_IDLE, MICHI_STATE_UPDATING,
                   (int64_t)ARM_MS + 1000, false, false) ==
              MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
          "UPDATING mid-press: no factory reset");
}

/* --- Test: arm window (boot-hold protection) --- */
static void test_arm_window(void)
{
    printf("button: factory-reset arm window (%d ms)\n", (int)ARM_MS);

    /* Below arm window: IGNORED_ARM. */
    CHECK(hold_cls(15000, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   ARM_MS - 1, false, false) ==
              MICHI_BUTTON_ACTION_IGNORED_ARM,
          "arm_ms-1 elapsed -> IGNORED_ARM");

    /* At arm window: FACTORY_RESET allowed. */
    CHECK(hold_cls(15000, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   ARM_MS, false, false) ==
              MICHI_BUTTON_ACTION_FACTORY_RESET,
          "arm_ms elapsed -> FACTORY_RESET allowed");

    /* Boot-hold (0 ms): IGNORED_ARM. */
    CHECK(hold_cls(15000, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   0, false, false) ==
              MICHI_BUTTON_ACTION_IGNORED_ARM,
          "boot-hold (0 ms elapsed) -> IGNORED_ARM");

    /* Recovery is NOT gated by arm window. */
    CHECK(hold_cls(5000, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR, 0, false, false) ==
              MICHI_BUTTON_ACTION_RECOVERY,
          "recovery NOT armed (0 ms boot elapsed)");
}

/* --- Test: hold during OTA (bounce cannot advance threshold) --- */
static void test_hold_during_ota(void)
{
    printf("button: hold during OTA -> ignored\n");
    /* Press started in IDLE, firmware moves to UPDATING after 2 s.
     * At the 5 s crossing, current state is UPDATING → IGNORED. */
    CHECK(hold_cls(5000, MICHI_STATE_IDLE, MICHI_STATE_UPDATING,
                   (int64_t)ARM_MS + 1000, false, false) ==
              MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
          "OTA during hold: 5000 ms with current=UPDATING -> IGNORED");
}

/* --- Test: factory reset wiring (identity) --- */
static void test_factory_reset_run_wiring(void)
{
    printf("button: factory reset wiring (identity READY)\n");

    test_reset_all();

    CHECK(michi_identity_init() == ESP_OK, "identity init -> READY");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_READY,
          "identity READY before reset");
    char old_id[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(old_id, sizeof(old_id)) == ESP_OK,
          "michi_id before reset");

    CHECK(michi_button_factory_reset_run() == ESP_OK, "factory reset runs");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_UNINITIALIZED,
          "identity RAM wiped");
    CHECK(test_pairing_erase_all_calls() == 1, "pairing erase called once");
    CHECK(test_nvs_flash_erase_count() == 1, "full NVS erase called once");
    CHECK(test_esp_restart_count() == 1, "restart called once");

    size_t len = 0;
    CHECK(!test_nvs_get_blob(MICHI_IDENTITY_NVS_NAMESPACE,
                             MICHI_IDENTITY_NVS_KEY, NULL, 0, &len),
          "persisted seed gone after reset");

    /* Fresh init mints a NEW identity. */
    CHECK(michi_identity_init() == ESP_OK, "fresh init after reset");
    char new_id[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(new_id, sizeof(new_id)) == ESP_OK,
          "michi_id after reset");
    CHECK(strlen(new_id) == 43, "michi_id is 43 chars");
    CHECK(strcmp(old_id, new_id) != 0, "fresh identity differs from old");
}

static void test_factory_reset_fresh_device(void)
{
    printf("button: factory reset on a fresh device (no identity persisted)\n");

    test_reset_all();
    CHECK(michi_button_factory_reset_run() == ESP_OK, "factory reset runs");
    CHECK(test_pairing_erase_all_calls() == 1, "pairing erase called once");
    CHECK(test_nvs_flash_erase_count() == 1, "full NVS erase called once");
    CHECK(test_esp_restart_count() == 1, "restart called once");
}

static void test_corrupt_identity_factory_reset(void)
{
    printf("button: corrupt identity -> factory reset available\n");

    test_reset_all();

    seed_corrupt_identity_store();
    CHECK(michi_identity_init() != ESP_OK, "init fails on corrupt store");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_CORRUPT,
          "identity CORRUPT");

    /* Corrupt identity does NOT move FSM to protected state:
     * factory reset must still fire at 15 s. */
    test_state_set(MICHI_STATE_IDLE);
    CHECK(hold_cls(15000, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, false, false) ==
              MICHI_BUTTON_ACTION_FACTORY_RESET,
          "corrupt identity: 15000 ms -> FACTORY_RESET");

    /* Gesture consumed blocks escalation. */
    CHECK(hold_cls(15000, MICHI_STATE_IDLE, MICHI_STATE_IDLE,
                   (int64_t)ARM_MS + 1000, true, false) ==
              MICHI_BUTTON_ACTION_NONE,
          "corrupt identity: 15000 ms consumed -> NONE");

    /* Execute the physical recovery. */
    CHECK(michi_button_factory_reset_run() == ESP_OK, "factory reset runs");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_UNINITIALIZED,
          "corrupt identity wiped");
    CHECK(test_pairing_erase_all_calls() == 1, "pairing erase called once");
    CHECK(test_nvs_flash_erase_count() == 1, "full NVS erase called once");
    CHECK(test_esp_restart_count() == 1, "restart called once");

    size_t len = 0;
    CHECK(!test_nvs_get_blob(MICHI_IDENTITY_NVS_NAMESPACE,
                             MICHI_IDENTITY_NVS_KEY, NULL, 0, &len),
          "corrupt blob gone after reset");

    /* Fresh init recovers identity. */
    CHECK(michi_identity_init() == ESP_OK, "fresh init succeeds after reset");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_READY,
          "identity READY again");
    char id[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(id, sizeof(id)) == ESP_OK,
          "michi_id available after recovery");
    CHECK(strlen(id) == 43, "recovered michi_id is 43 chars");
    len = 0;
    CHECK(test_nvs_get_blob(MICHI_IDENTITY_NVS_NAMESPACE,
                            MICHI_IDENTITY_NVS_KEY, NULL, 0, &len) &&
              len == sizeof(michi_identity_blob_t),
          "well-formed seed persisted after recovery");
}

int main(void)
{
    test_hold_threshold_exact();
    test_recovery_context();
    test_factory_reset_escalonado();
    test_protected_states();
    test_arm_window();
    test_hold_during_ota();
    test_factory_reset_run_wiring();
    test_factory_reset_fresh_device();
    test_corrupt_identity_factory_reset();

    if (failures == 0) {
        printf("button: ALL TESTS PASSED\n");
        return 0;
    }
    printf("button: %d FAILURE(S)\n", failures);
    return 1;
}
