#pragma once
/* ESP-IDF's ip4_addr helpers represented using POSIX IPv4 in this test. */
#include <stdint.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
typedef struct { uint32_t addr; } ip4_addr_t;
static inline int ip4addr_aton(const char *text, ip4_addr_t *out)
{
    struct in_addr addr = {0};
    if (!text || !out || inet_pton(AF_INET, text, &addr) != 1) return 0;
    out->addr = addr.s_addr;
    return 1;
}
static inline char *ip4addr_ntoa_r(const ip4_addr_t *ip, char *out, int size)
{
    if (!ip || !out || size <= 0) return NULL;
    struct in_addr addr = {.s_addr = ip->addr};
    return (char *)inet_ntop(AF_INET, &addr, out, (socklen_t)size);
}
