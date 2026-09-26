#include "esp_http_server.h"
#include <string.h>
#include <stdio.h>

extern void test_esp_timer_advance(int64_t us);

static char s_hdr_clen[32] = "";
static bool s_has_hdr_clen = false;
static const char *s_payload = NULL;
static size_t s_payload_len = 0;
static size_t s_payload_pos = 0;
static size_t s_trickle_chunk = 0;
static int64_t s_trickle_advance_us = 0;
static int s_timeouts_remaining = 0;
static int s_recv_err = 0;

void test_http_reset(void)
{
    s_hdr_clen[0] = '\0';
    s_has_hdr_clen = false;
    s_payload = NULL;
    s_payload_len = 0;
    s_payload_pos = 0;
    s_trickle_chunk = 0;
    s_trickle_advance_us = 0;
    s_timeouts_remaining = 0;
    s_recv_err = 0;
}

void test_http_set_content_length(const char *clen_str)
{
    if (clen_str != NULL) {
        snprintf(s_hdr_clen, sizeof(s_hdr_clen), "%s", clen_str);
        s_has_hdr_clen = true;
    } else {
        s_has_hdr_clen = false;
    }
}

void test_http_set_payload(const char *data, size_t len)
{
    s_payload = data;
    s_payload_len = len;
    s_payload_pos = 0;
}

void test_http_set_trickle(size_t chunk_size, int64_t advance_us_per_chunk)
{
    s_trickle_chunk = chunk_size;
    s_trickle_advance_us = advance_us_per_chunk;
}

void test_http_set_timeout_count(int count)
{
    s_timeouts_remaining = count;
}

void test_http_set_recv_err(int err_code)
{
    s_recv_err = err_code;
}

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    (void)r;
    if (field == NULL || val == NULL || val_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(field, "Content-Length") == 0) {
        if (!s_has_hdr_clen) {
            return ESP_ERR_NOT_FOUND;
        }
        if (strlen(s_hdr_clen) >= val_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(val, s_hdr_clen, strlen(s_hdr_clen) + 1);
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (s_timeouts_remaining > 0) {
        s_timeouts_remaining--;
        /* A socket timeout in ESP-IDF advances time by recv_wait_timeout (1s) */
        test_esp_timer_advance(1000000LL);
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    if (s_recv_err != 0) {
        int err = s_recv_err;
        s_recv_err = 0;
        return err;
    }
    if (s_payload == NULL || s_payload_pos >= s_payload_len) {
        return 0;
    }
    size_t to_copy = s_payload_len - s_payload_pos;
    if (s_trickle_chunk > 0 && to_copy > s_trickle_chunk) {
        to_copy = s_trickle_chunk;
    }
    if (to_copy > buf_len) {
        to_copy = buf_len;
    }
    memcpy(buf, s_payload + s_payload_pos, to_copy);
    s_payload_pos += to_copy;
    if (s_trickle_advance_us > 0) {
        test_esp_timer_advance(s_trickle_advance_us);
    }
    return (int)to_copy;
}
