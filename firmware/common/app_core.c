#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "app_core.h"
#include "michi_link_lite.h"
#include "discovery.h"
#include "pairing.h"
#include "heartbeat.h"
#include "session.h"
#include "led.h"
#include "config.h"

static const char *TAG = "michi_core";

static void wifi_evt(void *a, esp_event_base_t b, int32_t id, void *d)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected, reconnecting in 3s...");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_wifi_connect();
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t c = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&c));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL));

    wifi_config_t w = {
        .sta = {
            .ssid = CONFIG_MICHI_WIFI_SSID,
            .password = CONFIG_MICHI_WIFI_PASSWORD,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &w));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());

    ESP_LOGI(TAG, "Wi-Fi connecting to %s...", CONFIG_MICHI_WIFI_SSID);
}

static void btn_isr(void *a)
{
    pairing_button_pressed();
}

static void hb_cb(void *a)
{
    ESP_LOGW(TAG, "Heartbeat timeout — stopping session");
    session_stop();
}

static void pairing_state_cb(bool open, const char *initiator_id)
{
    if (open) {
        led_set(LED_YELLOW_BLINK);
    } else {
        led_set(LED_BLUE);
    }
}

static void session_state_cb(bool started, const char *session_id)
{
    if (started) {
        led_set(LED_GREEN);
    } else {
        led_set(LED_BLUE);
    }
}

void app_core_init(const char *device_type, const char *device_id,
                   const char *device_name)
{
    ESP_LOGI(TAG, "Michi Music Stream %s v%s [PROTOTYPE]", device_type, FIRMWARE_VERSION);
    ESP_LOGW(TAG, "Este firmware es prototype. Sin hardware validado.");

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    /* LED */
    led_init();

    /* Boton pairing */
    gpio_config_t b = {
        .pin_bit_mask = 1ULL << PAIRING_BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&b);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(PAIRING_BUTTON_PIN, btn_isr, NULL);

    /* Modulos */
    pairing_init();
    pairing_set_state_callback(pairing_state_cb);
    session_init();
    session_set_state_callback(session_state_cb);
    heartbeat_init();
    heartbeat_set_callback(hb_cb, NULL);

    /* HTTP server */
    michi_link_lite_init();
    michi_link_lite_register_endpoints(michi_link_lite_get_server());

    /* Discovery */
    discovery_init();
    discovery_announce(device_id, device_type, API_VERSION, FIRMWARE_VERSION, device_name);

    ESP_LOGI(TAG, "System ready");

    while (1) {
        pairing_is_window_open();
        if (session_is_active()) {
            heartbeat_is_active();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
