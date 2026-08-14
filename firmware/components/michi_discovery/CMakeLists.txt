idf_component_register(
    SRCS "michi_discovery.c" "announce.c" "discovery_nvs.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash esp_timer esp_hw_support freertos
             mdns michi_identity michi_product_profile json lwip
             michi_time
)
