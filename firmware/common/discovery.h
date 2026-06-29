#ifndef DISCOVERY_H
#define DISCOVERY_H

#define MICHI_MDNS_SERVICE  "_michi-receiver"
#define MICHI_MDNS_PROTO    "_tcp"
#define MICHI_MDNS_PORT     80

void discovery_init(void);
void discovery_announce(const char *device_id, const char *device_type,
                        const char *api_version, const char *firmware,
                        const char *name);
void discovery_stop(void);

#endif
