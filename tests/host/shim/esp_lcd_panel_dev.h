#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LCD_RGB_ELEMENT_ORDER_RGB = 0,
    LCD_RGB_ELEMENT_ORDER_BGR = 1,
} lcd_rgb_element_order_t;

/* IDF 5.3 hal/lcd_types.h: the ST7789 driver maps this onto RAMCTRL. */
typedef enum {
    LCD_RGB_DATA_ENDIAN_BIG = 0,
    LCD_RGB_DATA_ENDIAN_LITTLE,
} lcd_rgb_data_endian_t;

typedef struct {
    int reset_gpio_num;
    lcd_rgb_element_order_t rgb_ele_order;
    uint32_t bits_per_pixel;
    lcd_rgb_data_endian_t data_endian;
} esp_lcd_panel_dev_config_t;

#ifdef __cplusplus
}
#endif
