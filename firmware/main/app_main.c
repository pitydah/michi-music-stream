#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "michi_board.h"
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
    ESP_LOGI(TAG, "dac_present=%s (detection lands in phase 2)",
             st->dac_present ? "true" : "false");
    ESP_LOGI(TAG, "selftest=%s", st->overall ? "PASS" : "DEGRADED");
}

static void log_pending_subsystems(void)
{
    ESP_LOGI(TAG, "subsystem=bsp state=ok phase=1");
    ESP_LOGW(TAG, "subsystem=dac state=pending phase=2");
    ESP_LOGW(TAG, "subsystem=product_profile state=pending phase=3");
    ESP_LOGW(TAG, "subsystem=display state=pending phase=6");
    ESP_LOGW(TAG, "subsystem=led state=pending phase=7");
    ESP_LOGW(TAG, "subsystem=button state=pending phase=8");
    ESP_LOGW(TAG, "subsystem=network state=pending phase=9");
    ESP_LOGW(TAG, "subsystem=audio state=pending phase=11");
    ESP_LOGW(TAG, "subsystem=api state=pending phase=12");
}

void app_main(void)
{
    ESP_LOGI(TAG, "michi-music-stream firmware v%s target=%s",
             MICHI_FW_VERSION_STR, CONFIG_IDF_TARGET);

    esp_err_t err = init_nvs();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "FATAL: NVS is unusable (%s), halting - subsystems depending on NVS cannot start",
                 esp_err_to_name(err));
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(10000));
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
    log_selftest_rows(info, &st);

    if (st.display_ok) {
        err = michi_board_display_boot_screen(&st);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot screen render failed: %s (continuing degraded)",
                     esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "display unavailable, boot screen skipped (degraded mode)");
    }

    log_pending_subsystems();

    ESP_LOGI(TAG, "boot=ok mode=diagnostic audio_available=false");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
