idf_component_register(
    SRCS "michi_ota.c" "semver.c"
    INCLUDE_DIRS "include"
    # michi_sd dependency (review F3): the boot-time local check waits on
    # michi_sd_mounted() (async mount flag) with a bounded timeout. The
    # update files themselves are still read through the VFS (/sdcard/<n>)
    # - an unmounted card makes fopen fail with ENOENT.
    REQUIRES michi_state michi_product_profile michi_session michi_led
             esp_http_client esp_https_ota app_update mbedtls
             esp_partition nvs_flash freertos json michi_sd
)
