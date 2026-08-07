idf_component_register(
    SRCS "michi_sd.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_board esp_driver_sdspi sdmmc fatfs
             esp_driver_spi vfs freertos esp_timer
)
