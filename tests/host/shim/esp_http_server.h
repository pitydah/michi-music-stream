#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

struct httpd_req {
    void *dummy;
};
typedef struct httpd_req httpd_req_t;

#define HTTPD_SOCK_ERR_TIMEOUT 0x1001
#define HTTPD_SOCK_ERR_INVALID 0x1002
#define HTTPD_SOCK_ERR_FAIL    0x1003

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

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);

/* Test hooks for HTTP body read testing */
void test_http_reset(void);
void test_http_set_content_length(const char *clen_str);
void test_http_set_payload(const char *data, size_t len);
void test_http_set_trickle(size_t chunk_size, int64_t advance_us_per_chunk);
void test_http_set_timeout_count(int count);
void test_http_set_recv_err(int err_code);
