#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct httpd_req httpd_req_t;

typedef struct {
    unsigned server_port;
    bool lru_purge_enable;
    uint16_t max_uri_handlers;
    size_t stack_size;
    int recv_wait_timeout;
    int send_wait_timeout;
} httpd_config_t;

#define HTTPD_DEFAULT_CONFIG() { \
    .server_port = 80, \
    .lru_purge_enable = false, \
    .max_uri_handlers = 8, \
    .stack_size = 4096, \
    .recv_wait_timeout = 10, \
    .send_wait_timeout = 10 \
}
