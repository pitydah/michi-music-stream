#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *model;             /*!< Board model name */
    const char *revision;          /*!< Board revision string */
    uint32_t flash_bytes_expected; /*!< Expected flash size in bytes */
    uint32_t psram_bytes_expected; /*!< Expected PSRAM size in bytes */
    uint16_t display_width;        /*!< Display width in pixels */
    uint16_t display_height;       /*!< Display height in pixels */
    const char *display_controller; /*!< Display controller model */
    int lcd_sclk;                  /*!< LCD SPI clock pin */
    int lcd_mosi;                  /*!< LCD SPI MOSI pin */
    int lcd_miso;                  /*!< LCD SPI MISO pin */
    int lcd_dc;                    /*!< LCD data/command pin */
    int lcd_cs;                    /*!< LCD chip select pin */
    int lcd_rst;                   /*!< LCD reset pin, -1 if not wired */
    int backlight_gpio;            /*!< Backlight pin, active high */
    int sd_cs;                     /*!< microSD chip select (shared SPI bus, reserved) */
    int board_i2c_sda;             /*!< Board I2C (IMU) SDA pin, bus reserved */
    int board_i2c_scl;             /*!< Board I2C (IMU) SCL pin, bus reserved */
    int boot_button_gpio;          /*!< On-board BOOT button pin, reserved */
} michi_board_info_t;

typedef struct {
    bool chip_ok;          /*!< Detected chip matches the expected ESP32-S3 */
    bool flash_ok;         /*!< Flash size matches the expected value */
    uint32_t flash_bytes;  /*!< Detected flash size in bytes */
    bool psram_ok;         /*!< PSRAM size matches the expected value */
    uint32_t psram_bytes;  /*!< Detected PSRAM size in bytes */
    bool display_ok;       /*!< Panel driver initialized and framebuffer allocated */
    bool backlight_ok;     /*!< Backlight GPIO configured and on */
    bool wifi_supported;   /*!< Chip feature: Wi-Fi (not a connection check) */
    bool ble_supported;    /*!< Chip feature: BLE (not a connection check) */
    char dac_model[32];    /*!< DAC model from michi_dac_get_caps(), filled by app_main (BSP only renders) */
    bool dac_ok;           /*!< DAC initialized, filled by app_main (BSP only renders) */
    bool overall;          /*!< chip + flash + psram + display + backlight all OK */
} michi_board_selftest_t;

typedef struct {
    int dac_i2c_sda; /*!< External DAC I2C SDA pin (Kconfig, needs physical validation) */
    int dac_i2c_scl; /*!< External DAC I2C SCL pin (Kconfig, needs physical validation) */
    int i2s_bclk;    /*!< External DAC I2S BCLK pin (Kconfig, needs physical validation) */
    int i2s_lrck;    /*!< External DAC I2S LRCK pin (Kconfig, needs physical validation) */
    int i2s_din;     /*!< External DAC I2S DIN pin (Kconfig, needs physical validation) */
    int i2s_mclk;    /*!< External DAC I2S MCLK pin, -1 if unused (Kconfig) */
    int led_gpio;    /*!< External SK6812 LED data pin (Kconfig, needs physical validation) */
    int button_gpio; /*!< External pairing button pin, active low (Kconfig) */
} michi_board_external_pins_t;

/**
 * @brief Initialize the board: backlight, SPI bus, ST7789 panel and PSRAM framebuffer.
 *
 * On display-related failure the board keeps running in degraded mode (self test
 * reports display_ok/backlight_ok = false); the returned error tells the caller
 * what failed so it can log and continue.
 *
 * @return ESP_OK on success; otherwise the first error encountered. On failure
 *         s_inited stays false, so init can be retried.
 */
esp_err_t michi_board_init(void);

/**
 * @brief Release the framebuffer, panel, SPI bus and turn the backlight off.
 *
 * @return ESP_OK; best-effort, errors are logged.
 */
esp_err_t michi_board_shutdown(void);

/**
 * @brief Get the static board model information.
 *
 * @return Pointer to the static board info struct.
 */
const michi_board_info_t *michi_board_get_info(void);

/**
 * @brief Get the external peripheral pins (from Kconfig).
 *
 * @return Pointer to the static external pins struct.
 */
const michi_board_external_pins_t *michi_board_get_external_pins(void);

/**
 * @brief Run the board self test with real queries (chip, flash, PSRAM, display).
 */
michi_board_selftest_t michi_board_self_test(void);

/**
 * @brief Render the boot screen (text rows) and flush it to the display.
 *
 * @param info Board info used for the flash/PSRAM size rows.
 * @param st   Self-test results used for the verdict rows.
 * @return ESP_ERR_INVALID_STATE if the display is not available.
 */
esp_err_t michi_board_display_boot_screen(const michi_board_info_t *info,
                                          const michi_board_selftest_t *st);

/**
 * @brief Fill the display with the background color.
 *
 * @return ESP_ERR_INVALID_STATE if the display is not available.
 */
esp_err_t michi_board_display_clear(void);

#ifdef __cplusplus
}
#endif
