/*
 * Michi Music Stream Standard — firmware prototype.
 *
 * ESTADO: PROTOTYPE. No probado en hardware real.
 * - Sin reproduccion de audio estable validada.
 * - Sin multiroom.
 * - Sin Opus real implementado (solo stub).
 * - Sin produccion Hi-Fi.
 * - Usar solo para early prototyping con breadboard + PCM5102A.
 */

#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "michi_link_lite.h"
#include "discovery.h"
#include "pairing.h"
#include "heartbeat.h"
#include "session.h"
#include "config.h"

static const char *TAG = "michi_standard";

static void wifi_evt(void *a, esp_event_base_t b, int32_t id, void *d)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) { esp_wifi_connect(); }
}

static void wifi_init(void)
{
    esp_netif_init(); esp_event_loop_create_default(); esp_netif_create_default_wifi_sta();
    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT(); esp_wifi_init(&c);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL);
    wifi_config_t w = {.sta = {.ssid = CONFIG_MICHI_WIFI_SSID, .password = CONFIG_MICHI_WIFI_PASSWORD}};
    esp_wifi_set_mode(WIFI_MODE_STA); esp_wifi_set_config(WIFI_IF_STA, &w); esp_wifi_start(); esp_wifi_connect();
}

static void btn_isr(void *a) { pairing_button_pressed(); }

static void hb_cb(void *a) { ESP_LOGW(TAG, "Heartbeat timeout"); session_stop(); }

void app_main(void)
{
    ESP_LOGI(TAG, "Michi Music Stream Standard v%s [PROTOTYPE]", FIRMWARE_VERSION);
    ESP_LOGW(TAG, "Este firmware es prototype. Sin audio validado ni multiroom.");
    nvs_flash_init(); wifi_init();
    gpio_config_t b = {.pin_bit_mask = 1ULL<<PAIRING_BUTTON_PIN, .mode = GPIO_MODE_INPUT, .pull_up_en = true, .intr_type = GPIO_INTR_NEGEDGE};
    gpio_config(&b); gpio_install_isr_service(0); gpio_isr_handler_add(PAIRING_BUTTON_PIN, btn_isr, NULL);
    pairing_init(); session_init();
    heartbeat_init(); heartbeat_set_callback(hb_cb, NULL);
    michi_link_lite_init(); michi_link_lite_register_endpoints(michi_link_lite_get_server());
    discovery_init(); discovery_announce(CONFIG_DEVICE_ID_STR, DEVICE_TYPE, API_VERSION, FIRMWARE_VERSION, DEVICE_NAME);
    ESP_LOGI(TAG, "Ready");
    while (1) { pairing_is_window_open(); if (session_is_active()) heartbeat_is_active(); vTaskDelay(pdMS_TO_TICKS(1000)); }
}
