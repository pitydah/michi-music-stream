#pragma once
/* Shim for host-side tests: lwip inet.h stand-in with the two symbols
 * michi_discovery.c uses (inet_addr, htons). TEST-ONLY. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t inet_addr(const char *cp);

uint16_t htons(uint16_t hostshort);
#define ntohs(x) htons(x)

#ifdef __cplusplus
}
#endif
