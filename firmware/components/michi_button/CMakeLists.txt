idf_component_register(
    SRCS "michi_button.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_system esp_timer freertos michi_state nvs_flash
)
