idf_component_register(
    SRCS "waveshare_s3_lcd2/board_waveshare_s3_lcd2.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_lcd esp_driver_spi esp_driver_gpio esp_psram spi_flash
)
