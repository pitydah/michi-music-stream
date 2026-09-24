#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "michi_dac.h"
#include "michi_dac_types.h"
#include "dac_internal.h"
#include "nvs.h"
#include "driver/i2c_master.h"

static int g_failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        g_failures++; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static esp_err_t fake_hw_id_pcm5122(char *out, size_t max_len)
{
    strncpy(out, "pcm5122", max_len - 1);
    out[max_len - 1] = '\0';
    return ESP_OK;
}

static void test_dac_precedence_and_resolution(void)
{
    printf("\n=== DAC-01: Precedence & Resolution Tests ===\n");
    char profile[64] = {0};
    michi_dac_profile_source_t source = MICHI_DAC_PROFILE_SOURCE_NONE;

    /* 1. Empty NVS, no HW ID -> should resolve Kconfig default or autodetect */
    michi_dac_set_nvs_profile(NULL);
    michi_dac_register_hw_id_source(NULL);

    esp_err_t err = michi_dac_resolve_profile(profile, sizeof(profile), &source);
    CHECK(err == ESP_OK, "resolve_profile succeeds on clean state");

#if defined(CONFIG_MICHI_DAC_DEFAULT_PROFILE)
    if (strlen(CONFIG_MICHI_DAC_DEFAULT_PROFILE) > 0) {
        CHECK(strcmp(profile, CONFIG_MICHI_DAC_DEFAULT_PROFILE) == 0,
              "DAC-01: default profile resolved when NVS is empty");
        CHECK(source == MICHI_DAC_PROFILE_SOURCE_KCONFIG,
              "source is KCONFIG when NVS is empty and default set");
    } else {
        CHECK(source == MICHI_DAC_PROFILE_SOURCE_AUTODETECT,
              "source is AUTODETECT when NVS is empty and default empty");
    }
#else
    CHECK(source == MICHI_DAC_PROFILE_SOURCE_AUTODETECT,
          "source is AUTODETECT when default not configured");
#endif

    /* 2. NVS override takes precedence over Kconfig default */
    michi_dac_set_nvs_profile("pcm5102a");
    err = michi_dac_resolve_profile(profile, sizeof(profile), &source);
    CHECK(err == ESP_OK, "resolve with NVS profile ok");
    CHECK(strcmp(profile, "pcm5102a") == 0, "resolved profile is pcm5102a from NVS");
    CHECK(source == MICHI_DAC_PROFILE_SOURCE_NVS, "source is NVS");

    /* 3. HW-ID takes precedence over NVS */
    michi_dac_register_hw_id_source(fake_hw_id_pcm5122);
    err = michi_dac_resolve_profile(profile, sizeof(profile), &source);
    CHECK(err == ESP_OK, "resolve with HW-ID ok");
    CHECK(strcmp(profile, "pcm5122") == 0, "resolved profile is pcm5122 from HW-ID");
    CHECK(source == MICHI_DAC_PROFILE_SOURCE_HW_ID, "source is HW_ID");

    /* Clean up */
    michi_dac_register_hw_id_source(NULL);
    michi_dac_set_nvs_profile(NULL);
}

static void test_dac_init_detect_lifecycle(void)
{
    printf("\n=== DAC-02 & DAC-03: Lifecycle, I2C bypass & Detect ===\n");
    test_i2c_reset();
    michi_dac_shutdown();
    michi_dac_set_nvs_profile(NULL);
    michi_dac_register_hw_id_source(NULL);

    /* Test PCM5102A profile binding */
    michi_dac_set_nvs_profile("pcm5102a");

    esp_err_t err = michi_dac_init();
    CHECK(err == ESP_OK, "michi_dac_init ok with pcm5102a");
    CHECK(!test_i2c_bus_created(), "DAC-03: no I2C bus created for PCM5102A non-I2C DAC");

    err = michi_dac_detect();
    CHECK(err == ESP_OK, "DAC-02: michi_dac_detect succeeds with identical profile");
    CHECK(michi_dac_get_caps() != NULL && michi_dac_get_caps()->detected, "DAC state is detected");

    const michi_dac_caps_t *caps = michi_dac_get_caps();
    CHECK(caps != NULL, "caps returned");
    CHECK(strcmp(caps->board_profile, "pcm5102a") == 0, "board_profile is pcm5102a");
    CHECK(caps->max_sample_rate == 384000, "silicon max_sample_rate is 384000");
    CHECK(caps->tier == MICHI_PRODUCT_DIAGNOSTIC, "tier is DIAGNOSTIC before start/init");

    err = michi_dac_start(48000, 16, 2);
    CHECK(err == ESP_OK, "michi_dac_start ok");
    caps = michi_dac_get_caps();
    CHECK(caps->tier == MICHI_PRODUCT_STANDARD, "tier is STANDARD after start/init");

    /* Shutdown restores clean state */
    michi_dac_shutdown();
    CHECK(michi_dac_get_caps() == NULL || !michi_dac_get_caps()->detected, "DAC not detected after shutdown");
    michi_dac_set_nvs_profile(NULL);
}

static void test_factory_reset_model(void)
{
    printf("\n=== DAC-04: Factory Reset Model ===\n");
    test_i2c_reset();
    michi_dac_shutdown();

    /* Simulate setting a custom profile in NVS */
    michi_dac_set_nvs_profile("custom_override");
    char profile[64] = {0};
    michi_dac_profile_source_t source = MICHI_DAC_PROFILE_SOURCE_NONE;
    michi_dac_resolve_profile(profile, sizeof(profile), &source);
    CHECK(strcmp(profile, "custom_override") == 0, "custom profile in NVS resolved");
    CHECK(source == MICHI_DAC_PROFILE_SOURCE_NVS, "source is NVS");

    /* Simulate factory reset: NVS erased */
    michi_dac_set_nvs_profile(NULL);

    /* After reset, resolve_profile must gracefully fall back to Kconfig or autodetect */
    esp_err_t err = michi_dac_resolve_profile(profile, sizeof(profile), &source);
    CHECK(err == ESP_OK, "DAC-04: resolve_profile survives NVS erase/factory reset");
    CHECK(source != MICHI_DAC_PROFILE_SOURCE_NVS, "source is no longer NVS after reset");
}

int main(void)
{
    test_dac_precedence_and_resolution();
    test_dac_init_detect_lifecycle();
    test_factory_reset_model();

    if (g_failures > 0) {
        printf("\nFAILED: %d failures\n", g_failures);
        return 1;
    }
    printf("\nALL DAC TESTS PASSED\n");
    return 0;
}
