idf_component_register(
    SRCS "michi_log.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_timer esp_system heap nvs_flash
             spiffs michi_state
)
