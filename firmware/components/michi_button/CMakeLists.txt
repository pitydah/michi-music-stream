idf_component_register(
    SRCS "michi_button.c"
         "michi_button_gesture.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_system esp_timer freertos michi_state nvs_flash
             michi_pairing michi_identity
)
