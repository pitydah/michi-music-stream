#pragma once
#include "esp_lcd_panel_io.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_lcd_spi_bus_handle_t;

typedef struct {
    int cs_gpio_num;
    int dc_gpio_num;
    int spi_mode;
    unsigned int pclk_hz;
    size_t trans_queue_depth;
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
    int lcd_cmd_bits;
    int lcd_param_bits;
} esp_lcd_panel_io_spi_config_t;

esp_err_t esp_lcd_new_panel_io_spi(esp_lcd_spi_bus_handle_t bus,
                                   const esp_lcd_panel_io_spi_config_t *io_config,
                                   esp_lcd_panel_io_handle_t *ret_io);

#ifdef __cplusplus
}
#endif
