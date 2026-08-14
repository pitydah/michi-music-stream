#pragma once
/* Shim for host-side tests: lwip sockets.h stand-in for the UDP
 * multicast send path of michi_discovery.c. Every datagram handed to
 * sendto() is captured for the tests (test_socket_last_datagram), and
 * failures can be injected (test_socket_fail_next). TEST-ONLY: never
 * compiled into firmware.
 *
 * NOTE: this header deliberately does NOT include <sys/socket.h> - the
 * shim defines socket/close/sendto itself and a libc declaration of
 * the same names would clash. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* ssize_t */

#ifdef __cplusplus
extern "C" {
#endif

#define AF_INET 2
#define SOCK_DGRAM 2
#define IPPROTO_UDP 17
#define IPPROTO_IP 0
#define IP_MULTICAST_TTL 33

struct in_addr {
    uint32_t s_addr;
};

struct sockaddr {
    uint8_t sa_len;
    uint8_t sa_family;
    char sa_data[14];
};

struct sockaddr_in {
    uint8_t sin_len;
    uint8_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};

int socket(int domain, int type, int protocol);
int close(int fd);
int setsockopt(int s, int level, int optname, const void *optval,
               unsigned int optlen);
ssize_t sendto(int s, const void *dataptr, size_t size, int flags,
               const struct sockaddr *to, unsigned int tolen);

/* --- test hooks (TEST-ONLY) --- */

void test_socket_reset(void);
int test_socket_sent_count(void);

/* Copies the last captured datagram (up to out_len bytes, NUL-terminated).
 * Returns false when nothing was sent yet. */
bool test_socket_last_datagram(char *out, size_t out_len,
                               size_t *out_written);

/* Fail the next N sendto() calls (counts down). */
void test_socket_fail_next(int n);

/* Fail the next N socket() calls (counts down). */
void test_socket_fail_open_next(int n);

#ifdef __cplusplus
}
#endif
