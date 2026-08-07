idf_component_register(
    SRCS "michi_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_audio_output michi_volume michi_dac michi_product_profile
             michi_board michi_state lwip freertos esp_timer heap
)
