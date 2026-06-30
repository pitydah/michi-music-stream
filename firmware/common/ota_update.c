#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_update.h"

static const char *TAG = "michi_ota";

static void ota_task(void *pv)
{
    const char *url = (const char *)pv;

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 10000,
        .keep_alive_enable = false,
    };

    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA update successful. Rebooting in 3s...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA update failed: %s", esp_err_to_name(err));
    }
    free((void *)url);
    vTaskDelete(NULL);
}

void ota_init(void)
{
    ESP_LOGI(TAG, "OTA module initialized");
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    ESP_LOGI(TAG, "Running: %s, Boot: %s",
             running ? running->label : "?",
             boot ? boot->label : "?");
}

bool ota_start(const char *url)
{
    char *url_copy = strdup(url);
    if (!url_copy) return false;

    if (xTaskCreate(ota_task, "ota_update", 8192, url_copy, 5, NULL) != pdPASS) {
        free(url_copy);
        return false;
    }
    return true;
}
