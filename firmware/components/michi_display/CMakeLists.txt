idf_component_register(
    SRCS "michi_display.c"
         "michi_ui_theme.c"
         "michi_ui_fonts.c"
         "michi_ui_text.c"
         "michi_ui_icons.c"
         "michi_ui_components.c"
         "michi_ui_screens.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_board michi_state michi_volume michi_product_profile freertos michi_dac
    PRIV_REQUIRES michi_wifi michi_session michi_ota
)

# Compile-time check: if 2-bit raster is enabled, the generated header must declare 2-bit.
# We check MICHI_UI_SM_RASTER_BITS from the generated header.
if(CONFIG_MICHI_DISPLAY_RASTER_2BIT)
    # This check runs at cmake configuration time using the preprocessor
    # A mismatch will cause a build error explaining the issue.
    target_compile_definitions(${COMPONENT_LIB} PUBLIC MICHI_DISPLAY_RASTER_2BIT_REQUESTED=1)
endif()
