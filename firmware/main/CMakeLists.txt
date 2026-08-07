idf_component_register(
    SRCS "app_main.c"
    REQUIRES nvs_flash michi_board michi_dac michi_product_profile
             michi_http michi_audio_output michi_volume michi_state
             michi_display michi_led michi_button michi_wifi michi_pairing
             michi_audio michi_session michi_ota michi_sd michi_log
)
