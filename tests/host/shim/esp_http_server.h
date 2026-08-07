#pragma once
/* Stub for host-side tests (F15): michi_http.h includes this header, but
 * the JSON helpers under test never use the HTTP server API. Only the
 * opaque request type is needed for the declarations to parse. */

typedef struct httpd_req httpd_req_t;
