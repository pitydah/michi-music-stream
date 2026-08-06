idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_product_profile esp_http_server json
    PRIV_REQUIRES esp_timer
)
