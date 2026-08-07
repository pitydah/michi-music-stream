idf_component_register(
    SRCS "michi_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_state michi_product_profile esp_wifi esp_netif esp_event
             nvs_flash mdns wifi_provisioning bt freertos esp_timer json
)
