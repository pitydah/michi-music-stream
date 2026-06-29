#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

#define HEARTBEAT_TIMEOUT_MS 90000

typedef void (*heartbeat_cb_t)(void *ctx);

void heartbeat_init(void);
void heartbeat_set_callback(heartbeat_cb_t cb, void *ctx);
void heartbeat_reset(void);
void heartbeat_stop(void);
bool heartbeat_is_active(void);
uint64_t heartbeat_get_uptime_ms(void);

#endif
