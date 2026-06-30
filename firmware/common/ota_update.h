#ifndef OTA_UPDATE_H
#define OTA_UPDATE_H

#include <stdbool.h>

void ota_init(void);
bool ota_start(const char *url);

#endif
