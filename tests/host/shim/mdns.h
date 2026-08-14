#pragma once
/* Shim for host-side tests: mdns.h stand-in for the mDNS calls of
 * michi_discovery.c (init/hostname/service add/remove/free). All
 * calls succeed; the tests count them. TEST-ONLY: never compiled
 * into firmware. */

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *key;
    const char *value;
} mdns_txt_item_t;

esp_err_t mdns_init(void);
esp_err_t mdns_hostname_set(const char *hostname);
esp_err_t mdns_service_add(const char *instance_name, const char *service_type,
                           const char *proto, uint16_t port,
                           mdns_txt_item_t txt[], size_t num_items);
esp_err_t mdns_service_remove(const char *service_type, const char *proto);
void mdns_free(void);

/* --- test hooks (TEST-ONLY) --- */

void test_mdns_reset(void);
int test_mdns_service_add_count(void);
int test_mdns_service_remove_count(void);

#ifdef __cplusplus
}
#endif
