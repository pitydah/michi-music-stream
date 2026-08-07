idf_component_register(
    SRCS "michi_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_state michi_product_profile michi_session michi_led
             esp_http_client esp_https_ota app_update mbedtls
             esp_partition nvs_flash freertos json
)
