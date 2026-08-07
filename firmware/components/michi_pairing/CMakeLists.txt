idf_component_register(
    SRCS "michi_pairing.c" "validators.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_state freertos nvs_flash esp_timer esp_hw_support mbedtls
)
