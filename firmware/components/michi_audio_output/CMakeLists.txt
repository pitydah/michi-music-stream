idf_component_register(
    SRCS "michi_audio_output.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos esp_driver_i2s michi_volume heap soc
)
