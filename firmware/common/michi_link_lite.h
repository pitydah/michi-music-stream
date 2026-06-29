#ifndef MICHI_LINK_LITE_H
#define MICHI_LINK_LITE_H

#include <stdbool.h>
#include <esp_http_server.h>

#define MICHI_API_VERSION "v1-lite"
#define MICHI_REST_PORT   80

void michi_link_lite_init(void);
void michi_link_lite_register_endpoints(httpd_handle_t server);
bool michi_link_lite_validate_token(const char *token);
httpd_handle_t michi_link_lite_get_server(void);

#endif
