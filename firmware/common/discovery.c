#include <string.h>
#include "esp_log.h"
#include "mdns.h"
#include "discovery.h"

static const char *TAG = "michi_discovery";
static bool s_init = false;

void discovery_init(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("michi-stream"));
    s_init = true;
    ESP_LOGI(TAG, "mDNS initialized");
}

void discovery_announce(const char *device_id, const char *device_type,
                        const char *api_version, const char *firmware,
                        const char *name)
{
    if (!s_init) return;
    mdns_service_add(NULL, MICHI_MDNS_SERVICE, MICHI_MDNS_PROTO, MICHI_MDNS_PORT, NULL, 0);
    mdns_txt_item_t txt[] = {
        {"device_id", device_id}, {"type", device_type},
        {"api_version", api_version}, {"firmware", firmware}, {"name", name},
    };
    mdns_service_txt_set(MICHI_MDNS_SERVICE, MICHI_MDNS_PROTO, txt, sizeof(txt)/sizeof(txt[0]));
    ESP_LOGI(TAG, "mDNS announce: %s (%s)", name, device_id);
}

void discovery_stop(void)
{
    if (s_init) { mdns_service_remove(MICHI_MDNS_SERVICE, MICHI_MDNS_PROTO); mdns_free(); s_init = false; }
}
