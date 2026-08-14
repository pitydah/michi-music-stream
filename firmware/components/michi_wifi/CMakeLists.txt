# michi_wifi no longer calls the mdns API directly (signed discovery moved
# to michi_discovery), but the espressif/mdns managed dependency stays
# declared in idf_component.yml (kept byte-identical so the project
# dependencies.lock stays valid): michi_discovery REQUIRES mdns and
# resolves it from this declaration.
idf_component_register(
    SRCS "michi_wifi.c"
    INCLUDE_DIRS "include"
    REQUIRES michi_state michi_discovery michi_time esp_wifi esp_netif esp_event
             nvs_flash wifi_provisioning bt freertos esp_timer json
)
