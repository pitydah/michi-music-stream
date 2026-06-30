#ifndef WIFI_PROV_H
#define WIFI_PROV_H

#include <stdbool.h>

void wifi_prov_init(void);
bool wifi_prov_is_configured(void);
void wifi_prov_start_smartconfig(void);

#endif
