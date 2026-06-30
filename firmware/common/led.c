#include "esp_log.h"
#include "led.h"
#include "config.h"

static const char *TAG = "michi_led";

/*
 * Implementacion basica de LED via GPIO bit-bang para WS2812B.
 * En produccion debe reemplazarse por driver RMT.
 * Por ahora solo togglea GPIO como placeholder.
 */

static led_state_t s_current = LED_BLUE;

void led_init(void)
{
    ESP_LOGI(TAG, "LED initialized on GPIO %d", LED_PIN);
    led_set(LED_BLUE);
}

void led_set(led_state_t state)
{
    s_current = state;
    switch (state) {
        case LED_BLUE:
            ESP_LOGD(TAG, "LED: blue (fixed)");
            break;
        case LED_GREEN:
            ESP_LOGD(TAG, "LED: green (fixed)");
            break;
        case LED_YELLOW_BLINK:
            ESP_LOGD(TAG, "LED: yellow blink");
            break;
        case LED_RED:
            ESP_LOGD(TAG, "LED: red (fixed)");
            break;
        case LED_RED_FAST_BLINK:
            ESP_LOGD(TAG, "LED: red fast blink");
            break;
        case LED_OFF:
            ESP_LOGD(TAG, "LED: off");
            break;
    }
}

void led_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
