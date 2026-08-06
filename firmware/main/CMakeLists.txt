idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_psram spi_flash esp_hw_support
)
