#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

#include "michi_board.h"
#include "michi_dac.h"
#include "michi_http.h"
#include "michi_product_profile.h"
#include "michi_state.h"
#include "michi_version.h"

static const char *TAG = "michi_app";

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "nvs_flash_init returned %s, erasing NVS and retrying once",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_erase failed: %s", esp_err_to_name(err));
            return err;
        }
        err = nvs_flash_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_flash_init retry failed: %s", esp_err_to_name(err));
            return err;
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "nvs_flash initialized");
    return ESP_OK;
}

static void log_selftest_rows(const michi_board_info_t *info,
                              const michi_board_selftest_t *st)
{
    ESP_LOGI(TAG, "board=%s chip=%s",
             info->model, st->chip_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "flash=%" PRIu32 " bytes (expected %" PRIu32 ") status=%s",
             st->flash_bytes, info->flash_bytes_expected,
             st->flash_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "psram=%" PRIu32 " bytes (expected %" PRIu32 ") status=%s",
             st->psram_bytes, info->psram_bytes_expected,
             st->psram_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "display=%s", st->display_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "backlight=%s", st->backlight_ok ? "ok" : "FAIL");
    ESP_LOGI(TAG, "wifi_supported=%s", st->wifi_supported ? "yes" : "no");
    ESP_LOGI(TAG, "ble_supported=%s", st->ble_supported ? "yes" : "no");
    ESP_LOGI(TAG, "dac_model=%s dac_ok=%s",
             st->dac_model[0] != '\0' ? st->dac_model : "none",
             st->dac_ok ? "true" : "false");
    ESP_LOGI(TAG, "selftest=%s", st->overall ? "PASS" : "DEGRADED");
}

static void log_pending_subsystems(void)
{
    ESP_LOGI(TAG, "subsystem=bsp state=ok phase=1");
    ESP_LOGW(TAG, "subsystem=display state=pending phase=6");
    ESP_LOGW(TAG, "subsystem=led state=pending phase=7");
    ESP_LOGW(TAG, "subsystem=button state=pending phase=8");
    ESP_LOGW(TAG, "subsystem=network state=pending phase=9");
    ESP_LOGW(TAG, "subsystem=audio state=pending phase=11");
    ESP_LOGW(TAG, "subsystem=api state=pending phase=12");
}

/* DAC phase 2 bring-up: init -> detect -> start. Honest at every step: no
 * DAC, no clocks, init failure are all reported instead of faked. */
static void init_dac(void)
{
    esp_err_t err = michi_dac_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_dac_init failed: %s (audio unavailable)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=dac state=failed phase=2");
        return;
    }
    err = michi_dac_detect();
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "no DAC detected (probe 0x4D..0x4F + sanity): audio unavailable");
        ESP_LOGI(TAG, "subsystem=dac state=absent phase=2");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_dac_detect failed: %s (audio unavailable)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=dac state=error phase=2");
        return;
    }

    ESP_LOGI(TAG, "subsystem=dac state=detected phase=2");

    err = michi_dac_start(48000, 16, 2);
    if (err != ESP_OK) {
        /* Honest diagnostic: the DAC answered I2C but cannot be initialized.
         * For the PCM5122 this is expected while no I2S master is running
         * (PLL cannot lock without BCLK/LRCK); phase 11 starts the clocks. */
        ESP_LOGE(TAG, "michi_dac_start(48000,16,2) failed: %s - "
                 "dac detected but NOT initialized (audio_available=false)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=dac state=detected_init_failed phase=2");
        return;
    }
    ESP_LOGI(TAG, "dac=ok");
    ESP_LOGI(TAG, "subsystem=dac state=initialized phase=2");
}

void app_main(void)
{
#ifdef CONFIG_MICHI_DAC_MOCK
    ESP_LOGW(TAG, "MICHI_DAC_MOCK is ENABLED - this build fakes a DAC and "
             "must NOT be used in production");
#endif
    ESP_LOGI(TAG, "michi-music-stream firmware v%s target=%s",
             MICHI_FW_VERSION_STR, CONFIG_IDF_TARGET);

    esp_err_t err;

    /* State machine (phase 5): the single global coordinator. Init FIRST,
     * before NVS, so the NVS-fatal path can land on the real terminal state
     * (BOOTING->FATAL_ERROR is in the table) and every later producer can
     * post events. */
    err = michi_state_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "state bus unavailable - all events will be dropped");
        ESP_LOGI(TAG, "subsystem=state state=failed phase=5");
    }
    const bool state_ok = (err == ESP_OK);

    err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "FATAL: NVS is unusable (%s), halting - subsystems depending on NVS cannot start",
                 esp_err_to_name(err));
        /* The FSM was initialized before NVS, so this request is real and
         * lands on the terminal state; the halt loop feeds the task watchdog
         * to avoid a false WDT reset. */
        michi_state_request(MICHI_STATE_FATAL_ERROR);
        for (;;) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    err = michi_board_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "michi_board_init failed (%s), continuing in degraded mode",
                 esp_err_to_name(err));
    }

    const michi_board_info_t *info = michi_board_get_info();
    michi_board_selftest_t st = michi_board_self_test();

    init_dac();

    const michi_dac_caps_t *caps = michi_dac_get_caps();
    snprintf(st.dac_model, sizeof(st.dac_model), "%s",
             caps->detected ? caps->model : "");
    st.dac_ok = caps->initialized;

    log_selftest_rows(info, &st);

    /* Product profile: the single source of truth derived from DAC caps and
     * board evidence. Everything that announces the product (logs, boot
     * screen, later API/mDNS/BLE/sessions) reads from this profile - no
     * duplicated strings. */
    err = michi_product_profile_init();
    if (err != ESP_OK) {
        /* Defensive only: init cannot fail with the current evidence sources. */
        ESP_LOGE(TAG, "michi_product_profile_init failed: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=product_profile state=failed phase=3");
    } else {
        ESP_LOGI(TAG, "subsystem=product_profile state=ok phase=3");
    }
    const michi_product_profile_t *profile = michi_product_profile_get();
    char codecs_str[40] = {0};
    char rates_str[40] = {0};
    michi_product_profile_format_codecs(profile, codecs_str, sizeof(codecs_str));
    michi_product_profile_format_rates(profile, rates_str, sizeof(rates_str));
    ESP_LOGI(TAG, "profile: name=%s tier=%s audio_available=%s dac=%s "
             "codecs=%s sample_rates=%s display=%s lighting_rgb=%s "
             "cat_contour=%s",
             profile->product_name, michi_product_profile_tier_name(),
             profile->audio_available ? "true" : "false",
             profile->dac_model, codecs_str, rates_str,
             profile->display_present ? "true" : "false",
             profile->lighting_status_rgb ? "true" : "false",
             profile->lighting_cat_contour ? "true" : "false");

    /* Boot events, posted after all boot-critical inits (NVS, board, self
     * test, DAC, profile): BOOT_COMPLETE drives BOOTING->SELF_TEST and
     * SELF_TEST_DONE drives SELF_TEST->IDLE with ANY data. The self-test
     * already ran before these events are posted - the SELF_TEST state is
     * modeled retrospectively, so observers must not expect to observe the
     * test window; the overall result is surfaced by the log below.
     * RECOVERABLE_ERROR has no boot path: it is reserved for runtime
     * producers arriving from phase 9. */
    if (state_ok) {
        err = michi_state_post(MICHI_EVENT_BOOT_COMPLETE, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MICHI_EVENT_BOOT_COMPLETE post failed: %s",
                     esp_err_to_name(err));
        }
        ESP_LOGI(TAG, "self_test: overall=%u", (unsigned)(st.overall ? 1u : 0u));
        err = michi_state_post(MICHI_EVENT_SELF_TEST_DONE, st.overall ? 1u : 0u);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "MICHI_EVENT_SELF_TEST_DONE post failed: %s",
                     esp_err_to_name(err));
        }
    }

    if (st.display_ok) {
        err = michi_board_display_boot_screen(info, &st, profile->product_name);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot screen render failed: %s (continuing degraded)",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "display unavailable, boot screen skipped (degraded mode)");
    }

    /* HTTP API (phase 4): read-only migrated endpoints (/info, /firmware).
     * A failure is logged and boot continues - no halt. */
    err = michi_http_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "michi_http_init failed: %s (API /info and /firmware unavailable)",
                 esp_err_to_name(err));
        ESP_LOGI(TAG, "subsystem=http state=failed phase=4");
    } else {
        ESP_LOGI(TAG, "subsystem=http state=ok phase=4");
    }

    log_pending_subsystems();

    ESP_LOGI(TAG, "boot=ok mode=%s audio_available=%s",
             michi_product_profile_tier_name(),
             profile->audio_available ? "true" : "false");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
