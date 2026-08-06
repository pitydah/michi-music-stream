#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "nvs_flash.h"

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

static const char *chip_model_name(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:
        return "ESP32";
    case CHIP_ESP32S2:
        return "ESP32-S2";
    case CHIP_ESP32S3:
        return "ESP32-S3";
    case CHIP_ESP32C3:
        return "ESP32-C3";
    case CHIP_ESP32C6:
        return "ESP32-C6";
    case CHIP_ESP32H2:
        return "ESP32-H2";
    default:
        return "unknown";
    }
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

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "chip model=%s cores=%d revision=%d",
             chip_model_name(chip_info.model), chip_info.cores, chip_info.revision);
    ESP_LOGI(TAG, "chip features wifi=%s ble=%s embedded_flash=%s embedded_psram=%s",
             (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no",
             (chip_info.features & CHIP_FEATURE_BLE) ? "yes" : "no",
             (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "yes" : "no",
             (chip_info.features & CHIP_FEATURE_EMB_PSRAM) ? "yes" : "no");

    uint32_t flash_size = 0;
    err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_flash_get_size failed: %s (flash size reported as 0)",
                 esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "flash size=%" PRIu32 " bytes (%" PRIu32 " MiB)",
                 flash_size, flash_size / (1024 * 1024));
    }

    size_t psram_size = esp_psram_get_size();
    ESP_LOGI(TAG, "psram size=%zu bytes (%zu MiB)", psram_size, psram_size / (1024 * 1024));

    ESP_LOGW(TAG, "subsystem=bsp state=pending phase=1");
    ESP_LOGW(TAG, "subsystem=dac state=pending phase=2");
    ESP_LOGW(TAG, "subsystem=product_profile state=pending phase=3");
    ESP_LOGW(TAG, "subsystem=display state=pending phase=6");
    ESP_LOGW(TAG, "subsystem=led state=pending phase=7");
    ESP_LOGW(TAG, "subsystem=button state=pending phase=8");
    ESP_LOGW(TAG, "subsystem=network state=pending phase=9");
    ESP_LOGW(TAG, "subsystem=api state=pending phase=12");
    ESP_LOGW(TAG, "subsystem=audio state=pending phase=11");

    ESP_LOGI(TAG, "boot=ok mode=diagnostic audio_available=false");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
