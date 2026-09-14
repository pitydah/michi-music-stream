idf_component_register(
    SRCS "app_main.c"
    REQUIRES nvs_flash michi_board michi_dac michi_product_profile
             michi_http michi_audio_output michi_volume michi_state
             michi_display michi_led michi_button michi_wifi michi_pairing
             michi_audio michi_session michi_ota michi_sd michi_log
             michi_identity
)

execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(GIT_SHA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE MICHI_FW_GIT_SHA="${GIT_SHA}")
endif()
