idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_product_profile michi_pairing michi_session michi_state
             michi_display michi_audio michi_volume michi_wifi michi_dac
             michi_ota esp_http_server json
    PRIV_REQUIRES esp_timer esp_system heap
)
