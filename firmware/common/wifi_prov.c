#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_smartconfig.h"
#include "wifi_prov.h"

static const char *TAG = "michi_wifi";

void wifi_prov_init(void)
{
    ESP_LOGI(TAG, "Wi-Fi provisioning module initialized");
}

bool wifi_prov_is_configured(void)
{
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) {
        return false;
    }
    return strlen((const char *)cfg.sta.ssid) > 0;
}

void wifi_prov_start_smartconfig(void)
{
    ESP_LOGI(TAG, "Starting SmartConfig...");
    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    smartconfig_start_config_t sc_cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&sc_cfg));
}
