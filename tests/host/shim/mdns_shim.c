/* Shim for host-side tests: mdns stand-in (see mdns.h). TEST-ONLY. */

#include "mdns.h"

static int s_add_count;
static int s_remove_count;

void test_mdns_reset(void)
{
    s_add_count = 0;
    s_remove_count = 0;
}

int test_mdns_service_add_count(void)
{
    return s_add_count;
}

int test_mdns_service_remove_count(void)
{
    return s_remove_count;
}

esp_err_t mdns_init(void)
{
    return ESP_OK;
}

esp_err_t mdns_hostname_set(const char *hostname)
{
    (void)hostname;
    return ESP_OK;
}

esp_err_t mdns_service_add(const char *instance_name, const char *service_type,
                           const char *proto, uint16_t port,
                           mdns_txt_item_t txt[], size_t num_items)
{
    (void)instance_name;
    (void)service_type;
    (void)proto;
    (void)port;
    (void)txt;
    (void)num_items;
    s_add_count++;
    return ESP_OK;
}

esp_err_t mdns_service_remove(const char *service_type, const char *proto)
{
    (void)service_type;
    (void)proto;
    s_remove_count++;
    return ESP_OK;
}

void mdns_free(void)
{
}
