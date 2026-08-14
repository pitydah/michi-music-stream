/* Host-side tests for the deterministic button gesture contract (P1-07).
 *
 * Compiles the REAL firmware gesture module (michi_button_gesture.c -
 * the exact decision code the debounce task runs, no reimplementation)
 * plus the REAL identity component (michi_identity.c + identity_nvs.c +
 * Monocypher/BLAKE3): the corrupt-identity recovery path is proven end
 * to end - a factory reset wipes a CORRUPT store and a fresh init mints
 * a working identity. The pairing side is a call-counter test double
 * (the REAL pairing erase_all is already covered by test_michi_pairing);
 * the button test proves the WIRING: a factory reset calls
 * michi_pairing_erase_all() + michi_identity_factory_reset() +
 * nvs_flash_erase() + esp_restart().
 *
 * Boundary contract (Kconfig defaults mirrored in shim/sdkconfig.h):
 *   < 5000 ms        -> PAIRING
 *   5000..9999 ms    -> RECOVERY only when released in RECOVERABLE_ERROR
 *   >= 10000 ms      -> FACTORY_RESET (armed: press started >= 10000 ms
 *                       after boot)
 *   press/release in BOOTING, SELF_TEST or UPDATING -> ignored, every band
 */

#include <stdio.h>
#include <string.h>

#include "michi_button_gesture.h"
#include "michi_identity.h"
#include "identity_storage.h"
#include "michi_pairing_fake.h"
#include "michi_state.h" /* shim: test_state_set for the FSM model */
#include "nvs.h" /* fake NVS shim: test hooks */
#include "nvs_flash.h" /* shim: erase counter */
#include "esp_system.h" /* shim: restart counter */
#include "sdkconfig.h" /* the Kconfig defaults under test */

static int failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("  FAIL %s\n", msg);                                     \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static const uint32_t RECOVERY_MS = CONFIG_MICHI_BUTTON_RECOVERY_PRESS_MS;
static const uint32_t FACTORY_MS = CONFIG_MICHI_BUTTON_FACTORY_RESET_PRESS_MS;
static const uint32_t ARM_MS = CONFIG_MICHI_BUTTON_FACTORY_ARM_MS;

static michi_button_action_t classify(uint32_t press_ms,
                                      michi_state_t press_st,
                                      michi_state_t release_st,
                                      int64_t boot_elapsed_ms)
{
    return michi_button_gesture_classify(press_ms, press_st, release_st,
                                         boot_elapsed_ms, RECOVERY_MS,
                                         FACTORY_MS, ARM_MS);
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

/* Seed a structurally wrong identity blob (8 bytes instead of 40): the
 * exact corruption contract of michi_identity (wrong length -> CORRUPT). */
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

static void test_gesture_boundaries(void)
{
    printf("button: exact gesture boundaries (4999/5000/9999/10000 ms)\n");

    /* 4999 ms: short press band -> pairing. */
    CHECK(classify(4999, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 20000) ==
              MICHI_BUTTON_ACTION_PAIRING,
          "4999 ms -> pairing");
    CHECK(classify(4999, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR, 20000) ==
              MICHI_BUTTON_ACTION_PAIRING,
          "4999 ms (RECOVERABLE) -> still pairing");

    /* 5000 ms: long press band begins; recovery ONLY in RECOVERABLE_ERROR. */
    CHECK(classify(5000, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR, 20000) ==
              MICHI_BUTTON_ACTION_RECOVERY,
          "5000 ms (RECOVERABLE) -> recovery");
    CHECK(classify(5000, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 20000) ==
              MICHI_BUTTON_ACTION_IGNORED_STATE,
          "5000 ms (IDLE) -> ignored (not recoverable)");
    CHECK(classify(5000, MICHI_STATE_RECOVERABLE_ERROR, MICHI_STATE_IDLE,
                   20000) == MICHI_BUTTON_ACTION_IGNORED_STATE,
          "5000 ms (released in IDLE) -> ignored");

    /* 9999 ms: still the recovery band. */
    CHECK(classify(9999, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR, 20000) ==
              MICHI_BUTTON_ACTION_RECOVERY,
          "9999 ms (RECOVERABLE) -> recovery");
    CHECK(classify(9999, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 20000) ==
              MICHI_BUTTON_ACTION_IGNORED_STATE,
          "9999 ms (IDLE) -> ignored");

    /* 10000 ms: factory reset band, with priority over recovery. */
    CHECK(classify(10000, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 20000) ==
              MICHI_BUTTON_ACTION_FACTORY_RESET,
          "10000 ms (IDLE) -> factory reset");
    CHECK(classify(10000, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR, 20000) ==
              MICHI_BUTTON_ACTION_FACTORY_RESET,
          "10000 ms (RECOVERABLE) -> factory reset (band priority)");
    CHECK(classify(30000, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 60000) ==
              MICHI_BUTTON_ACTION_FACTORY_RESET,
          "30000 ms -> factory reset");
}

static void test_protected_states(void)
{
    printf("button: protected states (BOOTING/SELF_TEST/UPDATING)\n");

    static const michi_state_t protected_states[] = {
        MICHI_STATE_BOOTING, MICHI_STATE_SELF_TEST, MICHI_STATE_UPDATING,
    };
    static const char *const names[] = {"BOOTING", "SELF_TEST", "UPDATING"};

    for (int i = 0; i < 3; i++) {
        const michi_state_t p = protected_states[i];
        char msg[128];

        snprintf(msg, sizeof(msg), "%s: press started protected -> no reset",
                 names[i]);
        CHECK(classify(10000, p, MICHI_STATE_IDLE, 60000) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
              msg);

        snprintf(msg, sizeof(msg), "%s: release protected -> no reset",
                 names[i]);
        CHECK(classify(10000, MICHI_STATE_IDLE, p, 60000) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
              msg);

        snprintf(msg, sizeof(msg), "%s: both flanks protected -> no reset",
                 names[i]);
        CHECK(classify(10000, p, p, 60000) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
              msg);

        snprintf(msg, sizeof(msg),
                 "%s: recovery blocked when press started protected", names[i]);
        CHECK(classify(6000, p, MICHI_STATE_RECOVERABLE_ERROR, 60000) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
              msg);

        snprintf(msg, sizeof(msg),
                 "%s: recovery blocked when released protected", names[i]);
        CHECK(classify(6000, MICHI_STATE_RECOVERABLE_ERROR, p, 60000) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
              msg);

        snprintf(msg, sizeof(msg), "%s: pairing blocked too", names[i]);
        CHECK(classify(100, p, MICHI_STATE_IDLE, 60000) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
              msg);

        snprintf(msg, sizeof(msg),
                 "%s: boot-hold very long press -> no reset", names[i]);
        CHECK(classify(30000, p, MICHI_STATE_IDLE, 0) ==
                  MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
              msg);
    }

    /* OTA is the critical case: a factory reset must NEVER run during an
     * update, even when the press started before UPDATING. */
    CHECK(classify(15000, MICHI_STATE_IDLE, MICHI_STATE_UPDATING, 60000) ==
              MICHI_BUTTON_ACTION_IGNORED_PROTECTED,
          "UPDATING: press crossing into OTA -> no reset");
}

static void test_arm_window(void)
{
    printf("button: factory-reset arm window (10000 ms)\n");

    CHECK(classify(10000, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 9999) ==
              MICHI_BUTTON_ACTION_IGNORED_ARM,
          "elapsed 9999 ms -> armed");
    CHECK(classify(10000, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 10000) ==
              MICHI_BUTTON_ACTION_FACTORY_RESET,
          "elapsed 10000 ms -> reset allowed");
    CHECK(classify(30000, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 0) ==
              MICHI_BUTTON_ACTION_IGNORED_ARM,
          "boot-hold (0 ms elapsed) -> armed");

    /* Recovery is deliberately NOT armed. */
    CHECK(classify(5000, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR, 0) ==
              MICHI_BUTTON_ACTION_RECOVERY,
          "recovery NOT armed (0 ms elapsed)");
    CHECK(classify(9999, MICHI_STATE_RECOVERABLE_ERROR,
                   MICHI_STATE_RECOVERABLE_ERROR, 0) ==
              MICHI_BUTTON_ACTION_RECOVERY,
          "recovery NOT armed at 9999 ms");
}

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

    /* The reboot path: a fresh init mints a NEW identity. */
    CHECK(michi_identity_init() == ESP_OK, "fresh init after reset");
    char new_id[MICHI_IDENTITY_MICHI_ID_LEN];
    CHECK(michi_identity_michi_id(new_id, sizeof(new_id)) == ESP_OK,
          "michi_id after reset");
    CHECK(strlen(new_id) == 43, "michi_id is 43 chars");
    CHECK(strcmp(old_id, new_id) != 0, "fresh identity differs from the old one");
}

static void test_factory_reset_fresh_device(void)
{
    printf("button: factory reset on a fresh device (no identity persisted)\n");

    test_reset_all();
    /* No identity init: the store is empty. The benign NOT_FOUND path of
     * michi_identity_factory_reset must not abort the reset. */
    CHECK(michi_button_factory_reset_run() == ESP_OK, "factory reset runs");
    CHECK(test_pairing_erase_all_calls() == 1, "pairing erase called once");
    CHECK(test_nvs_flash_erase_count() == 1, "full NVS erase called once");
    CHECK(test_esp_restart_count() == 1, "restart called once");
}

static void test_corrupt_identity_factory_reset(void)
{
    printf("button: corrupt identity -> factory reset physically available\n");

    test_reset_all();

    seed_corrupt_identity_store();
    CHECK(michi_identity_init() != ESP_OK, "init fails on the corrupt store");
    CHECK(michi_identity_get_state() == MICHI_IDENTITY_CORRUPT,
          "identity CORRUPT");

    /* Identity corruption does NOT move the FSM into a protected state:
     * the device keeps running (IDLE) and the gesture is available. */
    test_state_set(MICHI_STATE_IDLE);
    CHECK(classify(10000, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 20000) ==
              MICHI_BUTTON_ACTION_FACTORY_RESET,
          "corrupt identity: 10000 ms -> factory reset");
    CHECK(classify(7000, MICHI_STATE_IDLE, MICHI_STATE_IDLE, 20000) ==
              MICHI_BUTTON_ACTION_IGNORED_STATE,
          "corrupt identity: 7000 ms -> ignored (not a recoverable FSM state)");

    /* Execute the physical recovery: identity wipe + pairing wipe + full
     * NVS erase + restart. */
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

    /* The physical recovery completes: a fresh init mints a working
     * identity and persists a well-formed blob. */
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
    test_gesture_boundaries();
    test_protected_states();
    test_arm_window();
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
