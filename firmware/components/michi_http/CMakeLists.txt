idf_component_register(
    SRCS "http_server.c" "json_helpers.c" "canonical_json.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_product_profile michi_pairing michi_session michi_state
             michi_audio michi_audio_output
             michi_wifi michi_dac michi_ota michi_sd esp_http_server json
    PRIV_REQUIRES esp_timer esp_system heap
)
