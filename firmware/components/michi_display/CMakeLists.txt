idf_component_register(
    SRCS "michi_display.c"
         "michi_ui_theme.c"
         "michi_ui_fonts.c"
         "michi_ui_text.c"
         "michi_ui_icons.c"
         "michi_ui_components.c"
         "michi_ui_screens.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_board michi_state michi_volume michi_product_profile freertos
    PRIV_REQUIRES michi_wifi michi_session michi_ota
)
