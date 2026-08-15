idf_component_register(
    SRCS "michi_display.c"
         "michi_ui_theme.c"
         "michi_ui_fonts.c"
         "michi_ui_text.c"
         "michi_ui_icons.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_board michi_state michi_volume michi_product_profile freertos
)
