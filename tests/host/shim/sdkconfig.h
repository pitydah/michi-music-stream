#pragma once
/* Shim for host-side tests: the Kconfig values the host-compiled
 * components build against. Mirrors the firmware Kconfig defaults:
 *  - firmware/components/michi_pairing/Kconfig (exact defaults);
 *  - firmware/components/michi_time/Kconfig (fast host values so the
 *    bounded SNTP wait tests run in ms, not seconds - the firmware
 *    defaults stay 10000 ms / 2 retries);
 *  - firmware/components/michi_button/Kconfig (exact defaults - the
 *    button gesture tests assert the 5000/10000/10000 contract against
 *    these);
 *  - CONFIG_LWIP_SNTP_MAX_SERVERS mirrors the IDF 5.3 lwIP Kconfig
 *    default (the esp_netif_sntp shim struct needs it).
 * TEST-ONLY: never compiled into firmware. */

#define CONFIG_MICHI_PAIRING_WINDOW_SECONDS 5
#define CONFIG_MICHI_PAIRING_MAX_CONTROLLERS 8

#define CONFIG_MICHI_TIME_SNTP_SERVER "pool.ntp.org"
#define CONFIG_MICHI_TIME_SYNC_TIMEOUT_MS 250
#define CONFIG_MICHI_TIME_SYNC_RETRIES 3
#define CONFIG_MICHI_TIME_TASK_STACK_BYTES 3072

#define CONFIG_MICHI_BUTTON_DEBOUNCE_MS 20
#define CONFIG_MICHI_BUTTON_MIN_PRESS_MS 50
#define CONFIG_MICHI_BUTTON_PAIRING_HOLD_MS 5000
#define CONFIG_MICHI_BUTTON_FACTORY_WARN_MS 10000
#define CONFIG_MICHI_BUTTON_FACTORY_RESET_PRESS_MS 15000
#define CONFIG_MICHI_BUTTON_FACTORY_ARM_MS 10000
#define CONFIG_MICHI_BUTTON_POLL_MS 10
#define CONFIG_MICHI_BUTTON_TASK_STACK_BYTES 3072

#define CONFIG_LWIP_SNTP_MAX_SERVERS 1

#define CONFIG_MICHI_DAC_DEFAULT_PROFILE "pcm5102a"
#define CONFIG_MICHI_DAC_I2C_SDA 21
#define CONFIG_MICHI_DAC_I2C_SCL 22
#define CONFIG_MICHI_DAC_I2C_SPEED_HZ 100000

#define CONFIG_MICHI_I2S_BCLK 3
#define CONFIG_MICHI_I2S_LRCK 5
#define CONFIG_MICHI_I2S_DIN 18
#define CONFIG_MICHI_I2S_MCLK 46
#define CONFIG_MICHI_LED_GPIO 23
#define CONFIG_MICHI_BUTTON_GPIO 0

#define CONFIG_MICHI_SKU_EXPECTS_AUDIO 1
