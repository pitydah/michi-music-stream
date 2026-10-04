idf_component_register(
    SRCS "michi_home.c" "michi_auth.c" "factory_cfg.c" "validators.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_identity nvs_flash json esp_timer
)
