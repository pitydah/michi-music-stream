#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "michi_board.h"
#include "michi_version.h"
#include "font5x7.h"

#define MICHI_LCD_HOST SPI2_HOST
#define MICHI_LCD_PIXEL_CLK_HZ 20000000
#define MICHI_LCD_CMD_BITS 8
#define MICHI_LCD_PARAM_BITS 8
#define MICHI_LCD_TRANS_QUEUE_DEPTH 10
/* Band height of the banded framebuffer (MS-11): 240 x 40 x 2 = 19.2 KB,
 * which internal DMA RAM can always provide at boot. The full panel is
 * drawn as display_height / MICHI_LCD_BAND sequential band flushes.
 * display_height (320) must stay an exact multiple of this value. */
#define MICHI_LCD_BAND 40
#define MICHI_TEXT_SPACING 6

static const char *TAG = "michi_board";

static const michi_board_info_t s_board_info = {
    .model = "Waveshare ESP32-S3-LCD-2",
    .revision = "1.0",
    .flash_bytes_expected = 16U * 1024U * 1024U,
    .psram_bytes_expected = 8U * 1024U * 1024U,
    .display_width = 240,
    .display_height = 320,
    .display_controller = "ST7789T3",
    .lcd_sclk = 39,
    .lcd_mosi = 38,
    .lcd_miso = 40,
    .lcd_dc = 42,
    .lcd_cs = 45,
    .lcd_rst = -1,
    .backlight_gpio = 1,
    .sd_cs = 41,
    .board_i2c_sda = 48,
    .board_i2c_scl = 47,
    .boot_button_gpio = 0,
};

static esp_lcd_panel_io_handle_t s_panel_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static uint16_t *s_fb = NULL;
static size_t s_fb_bytes = 0;
static bool s_backlight_on = false;
static bool s_spi_bus_inited = false;
static bool s_inited = false;

static void draw_pixel(uint16_t *fb, uint16_t fb_w, uint16_t fb_h, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= fb_w || y >= fb_h) {
        return;
    }
    fb[(size_t)y * fb_w + (size_t)x] = color;
}

/* True when the row [y_abs, y_abs + h) intersects the band starting at
 * y_origin with band_h rows. Rows that only partially intersect are
 * drawn and clipped by draw_pixel, so the banded frame stays
 * pixel-identical to a full-frame render. */
static bool band_intersects(uint16_t y_origin, uint16_t band_h, int y_abs, int h)
{
    return y_abs < (int)y_origin + (int)band_h && y_abs + h > (int)y_origin;
}

void michi_board_display_draw_text(uint16_t *fb, uint16_t fb_w, uint16_t fb_h,
                                   int x, int y, const char *str,
                                   uint16_t fg, uint16_t bg)
{
    for (const char *p = str; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < MICHI_FONT5X7_FIRST_CHAR || c > MICHI_FONT5X7_LAST_CHAR) {
            c = MICHI_FONT5X7_FIRST_CHAR;
        }
        const uint8_t *glyph = michi_font5x7[c - MICHI_FONT5X7_FIRST_CHAR];
        for (int col = 0; col < MICHI_FONT5X7_WIDTH; col++) {
            uint8_t mask = glyph[col];
            for (int row = 0; row < MICHI_FONT5X7_HEIGHT; row++) {
                draw_pixel(fb, fb_w, fb_h, x + col, y + row,
                           (mask & (1U << row)) ? fg : bg);
            }
        }
        x += MICHI_TEXT_SPACING;
    }
}

static esp_err_t init_backlight(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << s_board_info.backlight_gpio,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight gpio_config failed: %s", esp_err_to_name(err));
        s_backlight_on = false;
        return err;
    }
    err = gpio_set_level(s_board_info.backlight_gpio, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight on failed: %s", esp_err_to_name(err));
        s_backlight_on = false;
        return err;
    }
    s_backlight_on = true;
    return ESP_OK;
}

static esp_err_t init_display(void)
{
    const michi_board_info_t *bi = &s_board_info;

    spi_bus_config_t buscfg = {
        .sclk_io_num = bi->lcd_sclk,
        .mosi_io_num = bi->lcd_mosi,
        .miso_io_num = bi->lcd_miso,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (size_t)bi->display_width * bi->display_height * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(MICHI_LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }
    s_spi_bus_inited = true;

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = bi->lcd_dc,
        .cs_gpio_num = bi->lcd_cs,
        .pclk_hz = MICHI_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = MICHI_LCD_CMD_BITS,
        .lcd_param_bits = MICHI_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = MICHI_LCD_TRANS_QUEUE_DEPTH,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)MICHI_LCD_HOST,
                                   &io_config, &s_panel_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        spi_bus_free(MICHI_LCD_HOST);
        s_spi_bus_inited = false;
        return err;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = bi->lcd_rst,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        spi_bus_free(MICHI_LCD_HOST);
        s_spi_bus_inited = false;
        return err;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_reset failed: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        spi_bus_free(MICHI_LCD_HOST);
        s_spi_bus_inited = false;
        return err;
    }
    err = esp_lcd_panel_init(s_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init failed: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        spi_bus_free(MICHI_LCD_HOST);
        s_spi_bus_inited = false;
        return err;
    }
    // Polarity per official Waveshare demo (Arduino_GFX IPS=true -> INVON). Final check
    // on real hardware: see README hardware-validation step 5.
    err = esp_lcd_panel_invert_color(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_invert_color failed: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        spi_bus_free(MICHI_LCD_HOST);
        s_spi_bus_inited = false;
        return err;
    }
    err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off failed: %s", esp_err_to_name(err));
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        esp_lcd_panel_io_del(s_panel_io);
        s_panel_io = NULL;
        spi_bus_free(MICHI_LCD_HOST);
        s_spi_bus_inited = false;
        return err;
    }
    return ESP_OK;
}

esp_err_t michi_board_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    const michi_board_info_t *bi = &s_board_info;

    esp_err_t err = init_backlight();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backlight init failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t fb_bytes = (size_t)bi->display_width * MICHI_LCD_BAND * sizeof(uint16_t);
    /* MS-11 (on-device, definitive): in this IDF build,
     * esp_ptr_dma_capable() only covers the INTERNAL DMA region; PSRAM
     * pointers are reported not-DMA-capable and the SPI driver then
     * bounces every color flush through an internal-RAM buffer sized for
     * the full 240x320 frame (150 KB contiguous), which internal DMA RAM
     * cannot provide at boot (measured: heap_caps_malloc fails ~1.4 s
     * in) - ESP_ERR_NO_MEM, black screen. Fix: a SMALL banded
     * framebuffer (240 x MICHI_LCD_BAND, 19.2 KB) in DMA RAM; the panel
     * is drawn as display_height / MICHI_LCD_BAND sequential band
     * flushes (see michi_board_display_render). No bounce buffer is ever
     * needed because each flush fits well below the internal pool. */
    s_fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_DMA);
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "framebuffer allocation failed (%zu bytes, DMA RAM): display disabled", fb_bytes);
        /* Symmetry with the display-init failure path below: no framebuffer,
         * no display, no reason to keep the backlight burning. */
        if (gpio_set_level(bi->backlight_gpio, 0) != ESP_OK) {
            ESP_LOGE(TAG, "backlight off failed after framebuffer allocation failure");
        }
        s_backlight_on = false;
        return ESP_ERR_NO_MEM;
    }

    s_fb_bytes = fb_bytes;

    err = init_display();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed (%s): display disabled, continuing degraded",
                 esp_err_to_name(err));
        if (gpio_set_level(bi->backlight_gpio, 0) != ESP_OK) {
            ESP_LOGE(TAG, "backlight off failed after display init failure");
        }
        s_backlight_on = false;
        heap_caps_free(s_fb);
        s_fb = NULL;
        s_fb_bytes = 0;
        return err;
    }

    s_inited = true;
    ESP_LOGI(TAG, "board init ok: %ux%u %s, banded framebuffer %ux%u (%zu bytes) in DMA RAM",
             bi->display_width, bi->display_height, bi->display_controller,
             bi->display_width, (unsigned)MICHI_LCD_BAND, fb_bytes);
    return ESP_OK;
}

esp_err_t michi_board_shutdown(void)
{
    if (s_panel != NULL) {
        esp_err_t err = esp_lcd_panel_disp_on_off(s_panel, false);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "panel disp_on_off(false) failed: %s", esp_err_to_name(err));
        }
        err = esp_lcd_panel_del(s_panel);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_panel_del failed: %s", esp_err_to_name(err));
        }
        s_panel = NULL;
    }
    if (s_panel_io != NULL) {
        esp_err_t err = esp_lcd_panel_io_del(s_panel_io);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_panel_io_del failed: %s", esp_err_to_name(err));
        }
        s_panel_io = NULL;
    }
    if (s_spi_bus_inited) {
        spi_bus_free(MICHI_LCD_HOST);
        s_spi_bus_inited = false;
    }
    if (s_fb != NULL) {
        heap_caps_free(s_fb);
        s_fb = NULL;
        s_fb_bytes = 0;
    }
    if (s_backlight_on) {
        esp_err_t err = gpio_set_level(s_board_info.backlight_gpio, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "backlight off failed: %s", esp_err_to_name(err));
        }
    }
    s_backlight_on = false;
    s_inited = false;
    ESP_LOGI(TAG, "board shutdown complete");
    return ESP_OK;
}

const michi_board_info_t *michi_board_get_info(void)
{
    return &s_board_info;
}

const michi_board_external_pins_t *michi_board_get_external_pins(void)
{
    static const michi_board_external_pins_t s_external_pins = {
        .dac_i2c_sda = CONFIG_MICHI_DAC_I2C_SDA,
        .dac_i2c_scl = CONFIG_MICHI_DAC_I2C_SCL,
        .i2s_bclk = CONFIG_MICHI_I2S_BCLK,
        .i2s_lrck = CONFIG_MICHI_I2S_LRCK,
        .i2s_din = CONFIG_MICHI_I2S_DIN,
        .i2s_mclk = CONFIG_MICHI_I2S_MCLK,
        .led_gpio = CONFIG_MICHI_LED_GPIO,
        .button_gpio = CONFIG_MICHI_BUTTON_GPIO,
    };
    return &s_external_pins;
}

michi_board_selftest_t michi_board_self_test(void)
{
    michi_board_selftest_t st = {0};
    const michi_board_info_t *bi = &s_board_info;

    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    st.chip_ok = (chip_info.model == CHIP_ESP32S3);
    st.wifi_supported = (chip_info.features & CHIP_FEATURE_WIFI_BGN) != 0;
    st.ble_supported = (chip_info.features & CHIP_FEATURE_BLE) != 0;

    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    st.flash_bytes = (err == ESP_OK) ? flash_size : 0;
    st.flash_ok = (err == ESP_OK) && (flash_size == bi->flash_bytes_expected);

    size_t psram_size = esp_psram_get_size();
    st.psram_bytes = (uint32_t)psram_size;
    st.psram_ok = (psram_size == bi->psram_bytes_expected);

    st.display_ok = (s_panel != NULL) && (s_fb != NULL);
    st.backlight_ok = s_backlight_on;

    st.overall = st.chip_ok && st.flash_ok && st.psram_ok &&
                 st.display_ok && st.backlight_ok;
    return st;
}

/* Flush the band framebuffer to the panel rows [y_origin, y_origin +
 * MICHI_LCD_BAND). esp_lcd_panel_draw_bitmap() sets the panel window
 * (CASET/RASET) internally before streaming pixels - x_end/y_end are
 * exclusive in the ST7789 driver, so the full 240-column band is
 * covered. */
static esp_err_t flush_band(uint16_t y_origin)
{
    const michi_board_info_t *bi = &s_board_info;
    return esp_lcd_panel_draw_bitmap(s_panel, 0, y_origin, bi->display_width,
                                     y_origin + MICHI_LCD_BAND, s_fb);
}

static void boot_screen_row(const michi_board_info_t *bi, uint16_t y_origin, int y,
                            const char *label, const char *status,
                            uint16_t label_color, uint16_t status_color)
{
    if (!band_intersects(y_origin, MICHI_LCD_BAND, y, MICHI_FONT5X7_HEIGHT)) {
        return;
    }
    const int y_local = y - (int)y_origin;
    michi_board_display_draw_text(s_fb, bi->display_width, MICHI_LCD_BAND,
                                  8, y_local, label, label_color, 0x0000);
    if (status != NULL) {
        michi_board_display_draw_text(s_fb, bi->display_width, MICHI_LCD_BAND,
                  8 + (int)strlen(label) * MICHI_TEXT_SPACING, y_local, status,
                  status_color, 0x0000);
    }
}

/* Draw the boot screen rows that intersect the band at y_origin. Rows
 * keep their absolute layout; only the intersection test and the local-y
 * conversion are band-specific. */
static void boot_screen_band(uint16_t y_origin, const michi_board_info_t *info,
                             const michi_board_selftest_t *st,
                             const char *title)
{
    const uint16_t white = 0xFFFF;
    const uint16_t green = 0x07E0;
    const uint16_t red = 0xF800;

    if (band_intersects(y_origin, MICHI_LCD_BAND, 20, MICHI_FONT5X7_HEIGHT)) {
        int title_x = ((int)info->display_width - (int)strlen(title) * MICHI_TEXT_SPACING) / 2;
        if (title_x < 0) {
            title_x = 0;
        }
        michi_board_display_draw_text(s_fb, info->display_width, MICHI_LCD_BAND,
                                      title_x, 20 - (int)y_origin, title,
                                      white, 0x0000);
    }

    boot_screen_row(info, y_origin, 48, "Board: Waveshare ESP32-S3-LCD-2", NULL, white, 0);

    char line[40];
    snprintf(line, sizeof(line), "Flash: %" PRIu32 " MiB",
             info->flash_bytes_expected / (1024U * 1024U));
    boot_screen_row(info, y_origin, 64, line, st->flash_ok ? "ok" : "FAIL", white,
                    st->flash_ok ? green : red);
    snprintf(line, sizeof(line), "PSRAM: %" PRIu32 " MiB",
             info->psram_bytes_expected / (1024U * 1024U));
    boot_screen_row(info, y_origin, 80, line, st->psram_ok ? "ok" : "FAIL", white,
                    st->psram_ok ? green : red);
    boot_screen_row(info, y_origin, 96, "Display:", st->display_ok ? "ok" : "FAIL", white,
                    st->display_ok ? green : red);
    boot_screen_row(info, y_origin, 112, "WiFi:", st->wifi_supported ? "supported" : "not supported",
                    white, st->wifi_supported ? green : red);
    boot_screen_row(info, y_origin, 128, "BLE:", st->ble_supported ? "supported" : "not supported",
                    white, st->ble_supported ? green : red);
    /* DAC row: app_main fills dac_model/dac_ok from michi_dac_get_caps();
     * the BSP only renders. Vocabulary matches the sibling rows (ok/FAIL);
     * no model -> "DAC: none" with no status suffix. */
    if (st->dac_model[0] != '\0') {
        snprintf(line, sizeof(line), "DAC: %.24s", st->dac_model);
        boot_screen_row(info, y_origin, 144, line, st->dac_ok ? "ok" : "FAIL", white,
                        st->dac_ok ? green : red);
    } else {
        boot_screen_row(info, y_origin, 144, "DAC: none", NULL, white, 0);
    }
    boot_screen_row(info, y_origin, 160, "Result:", st->overall ? "PASS" : "DEGRADED", white,
                    st->overall ? green : red);
}

esp_err_t michi_board_display_boot_screen(const michi_board_info_t *info,
                                          const michi_board_selftest_t *st,
                                          const char *product_name)
{
    if (s_panel == NULL || s_fb == NULL) {
        ESP_LOGW(TAG, "display unavailable, boot screen not rendered");
        return ESP_ERR_INVALID_STATE;
    }

    /* Title from the dynamic product profile (name) + the firmware version;
     * the BSP never hardcodes the product name. */
    char title[48];
    snprintf(title, sizeof(title), "%s v%s", product_name, MICHI_FW_VERSION_STR);

    for (uint16_t y_origin = 0; y_origin < info->display_height;
         y_origin += MICHI_LCD_BAND) {
        memset(s_fb, 0, s_fb_bytes);
        boot_screen_band(y_origin, info, st, title);
        esp_err_t err = flush_band(y_origin);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "boot screen band flush failed at y=%u: %s",
                     (unsigned)y_origin, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t michi_board_display_clear(void)
{
    if (s_panel == NULL || s_fb == NULL) {
        ESP_LOGW(TAG, "display unavailable, clear skipped");
        return ESP_ERR_INVALID_STATE;
    }
    const michi_board_info_t *bi = &s_board_info;
    memset(s_fb, 0, s_fb_bytes);
    for (uint16_t y_origin = 0; y_origin < bi->display_height;
         y_origin += MICHI_LCD_BAND) {
        esp_err_t err = flush_band(y_origin);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "clear band flush failed at y=%u: %s",
                     (unsigned)y_origin, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t michi_board_display_render(michi_board_render_fn fn)
{
    if (s_panel == NULL || s_fb == NULL) {
        ESP_LOGW(TAG, "display unavailable, render skipped");
        return ESP_ERR_INVALID_STATE;
    }
    if (fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const michi_board_info_t *bi = &s_board_info;
    for (uint16_t y_origin = 0; y_origin < bi->display_height;
         y_origin += MICHI_LCD_BAND) {
        memset(s_fb, 0, s_fb_bytes);
        fn(s_fb, bi->display_width, MICHI_LCD_BAND, y_origin);
        esp_err_t err = flush_band(y_origin);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "band flush failed at y=%u: %s",
                     (unsigned)y_origin, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
