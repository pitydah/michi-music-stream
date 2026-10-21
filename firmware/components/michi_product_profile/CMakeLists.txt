idf_component_register(
    SRCS "michi_product_profile.c" "capabilities.c" "profile_logic.c" "ops_gate.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_dac michi_board esp_partition
)
