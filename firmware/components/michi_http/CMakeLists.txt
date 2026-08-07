idf_component_register(
    SRCS "http_server.c" "json_helpers.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_product_profile michi_pairing michi_session michi_state
             michi_display michi_audio michi_audio_output michi_volume
             michi_wifi michi_dac michi_ota michi_sd michi_log esp_http_server json
    PRIV_REQUIRES esp_timer esp_system heap
)
