idf_component_register(
    SRCS "michi_time.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif lwip freertos esp_timer
)
