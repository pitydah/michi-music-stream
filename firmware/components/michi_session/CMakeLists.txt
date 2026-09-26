idf_component_register(
    SRCS "michi_session.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_audio michi_state michi_display michi_volume michi_ota freertos
             esp_hw_support esp_timer
)
