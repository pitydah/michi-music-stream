idf_component_register(
    SRCS "dac_manager.c"
         "dac_registry.c"
         "dac_classifier.c"
         "drivers/pcm512x.c"
         "drivers/pcm5102a.c"
         "drivers/mock_dac.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c esp_driver_gpio nvs_flash
)
