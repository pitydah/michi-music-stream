idf_component_register(
    SRCS "michi_led.c"
    INCLUDE_DIRS "include"
    REQUIRES led_strip michi_state freertos
)
