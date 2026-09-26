#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct esp_lcd_panel_io_t *esp_lcd_panel_io_handle_t;
typedef struct {
} esp_lcd_panel_io_event_data_t;

typedef bool (*esp_lcd_panel_io_color_trans_done_cb_t)(esp_lcd_panel_io_handle_t panel_io,
                                                       esp_lcd_panel_io_event_data_t *edata,
                                                       void *user_ctx);

esp_err_t esp_lcd_panel_io_del(esp_lcd_panel_io_handle_t io);

#ifdef __cplusplus
}
#endif
