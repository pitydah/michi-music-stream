#pragma once
/* Shim for host-side tests: Kconfig configuration for host-compiled components.
 *
 * Symbols are explicitly divided into two classes:
 *  1. TEST_OVERRIDE: Fast/compact values intentionally overridden for host test suites
 *     to ensure fast execution and bounded host resource usage.
 *  2. PRODUCTION_DEFAULT: Exact production values matching firmware Kconfig and
 *     sdkconfig.defaults.
 *
 * TEST-ONLY: never compiled into firmware.
 */

/* === 1. TEST_OVERRIDE === */
/* Fast 5s pairing window so host expiration tests run deterministically in ms (firmware default is 120s) */
#define CONFIG_MICHI_PAIRING_WINDOW_SECONDS 5

/* Fast 250ms SNTP sync timeout so host bounded-wait tests complete quickly (firmware default is 10000ms) */
#define CONFIG_MICHI_TIME_SYNC_TIMEOUT_MS 250

/* Host test retry count (firmware default is 2) */
#define CONFIG_MICHI_TIME_SYNC_RETRIES 3

/* Bounded 64 KB audio ring buffer for host memory efficiency (firmware default is 1024 KB in PSRAM) */
#define CONFIG_MICHI_AUDIO_RING_BUFFER_KB 64

/* Host test fallback profile override (firmware Kconfig default is "") */
#define CONFIG_MICHI_DAC_DEFAULT_PROFILE "pcm5102a"


/* === 2. PRODUCTION_DEFAULT === */
/* michi_pairing */
#define CONFIG_MICHI_PAIRING_MAX_CONTROLLERS 8

/* michi_time */
#define CONFIG_MICHI_TIME_SNTP_SERVER "pool.ntp.org"
#define CONFIG_MICHI_TIME_TASK_STACK_BYTES 3072

/* michi_button */
#define CONFIG_MICHI_BUTTON_DEBOUNCE_MS 30
#define CONFIG_MICHI_BUTTON_MIN_PRESS_MS 50
#define CONFIG_MICHI_BUTTON_PAIRING_HOLD_MS 5000
#define CONFIG_MICHI_BUTTON_FACTORY_WARN_MS 10000
#define CONFIG_MICHI_BUTTON_FACTORY_RESET_PRESS_MS 15000
#define CONFIG_MICHI_BUTTON_FACTORY_ARM_MS 10000
#define CONFIG_MICHI_BUTTON_POLL_MS 5
#define CONFIG_MICHI_BUTTON_TASK_STACK_BYTES 3072

/* lwIP SNTP shim */
#define CONFIG_LWIP_SNTP_MAX_SERVERS 1

/* michi_dac */
#define CONFIG_MICHI_DAC_I2C_SDA 21
#define CONFIG_MICHI_DAC_I2C_SCL 16
#define CONFIG_MICHI_DAC_I2C_SPEED_HZ 100000

/* michi_board / pinout */
#define CONFIG_MICHI_I2S_BCLK 3
#define CONFIG_MICHI_I2S_LRCK 18
#define CONFIG_MICHI_I2S_DIN 5
#define CONFIG_MICHI_I2S_MCLK -1
#define CONFIG_MICHI_LED_GPIO 4
#define CONFIG_MICHI_BUTTON_GPIO 17

/* SKU */
#define CONFIG_MICHI_SKU_EXPECTS_AUDIO 1
