/* Shim for host-side tests: lwip socket stand-ins with datagram
 * capture (see lwip/sockets.h). The firmware's close() is the lwip
 * socket close; on the host this object precedes -lc so the same
 * symbol serves both. TEST-ONLY: never compiled into firmware. */

#include "lwip/sockets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* close() for the fail-path fallthrough */

#include "lwip/inet.h"

static char s_capture[1200 + 1];
static size_t s_capture_len;
static int s_sent_count;
static int s_fail_send_next;
static int s_fail_open_next;
static struct sockaddr_in s_last_dst;
static bool s_has_dst;

void test_socket_reset(void)
{
    s_capture[0] = '\0';
    s_capture_len = 0;
    s_sent_count = 0;
    s_fail_send_next = 0;
    s_fail_open_next = 0;
    memset(&s_last_dst, 0, sizeof(s_last_dst));
    s_has_dst = false;
}

int test_socket_sent_count(void)
{
    return s_sent_count;
}

bool test_socket_last_datagram(char *out, size_t out_len,
                               size_t *out_written)
{
    if (out == NULL || out_written == NULL || s_sent_count == 0) {
        return false;
    }
    const size_t n = s_capture_len < out_len - 1 ? s_capture_len
                                                 : out_len - 1;
    memcpy(out, s_capture, n);
    out[n] = '\0';
    *out_written = n;
    return true;
}

void test_socket_fail_next(int n)
{
    s_fail_send_next = n;
}

void test_socket_fail_open_next(int n)
{
    s_fail_open_next = n;
}

bool test_socket_last_dest(uint32_t *ip_out, uint16_t *port_out)
{
    if (!s_has_dst) {
        return false;
    }
    if (ip_out != NULL) {
        *ip_out = s_last_dst.sin_addr.s_addr;
    }
    if (port_out != NULL) {
        *port_out = s_last_dst.sin_port;
    }
    return true;
}

int socket(int domain, int type, int protocol)
{
    (void)domain;
    (void)type;
    (void)protocol;
    if (s_fail_open_next > 0) {
        s_fail_open_next--;
        return -1;
    }
    static int next_fd = 3;
    return next_fd++;
}

int close(int fd)
{
    /* The firmware treats the socket as closed after close(): nothing
     * to release on the host. */
    (void)fd;
    return 0;
}

int setsockopt(int s, int level, int optname, const void *optval,
               unsigned int optlen)
{
    (void)s;
    (void)level;
    (void)optname;
    (void)optval;
    (void)optlen;
    return 0;
}

ssize_t sendto(int s, const void *dataptr, size_t size, int flags,
               const struct sockaddr *to, unsigned int tolen)
{
    (void)s;
    (void)flags;
    if (s_fail_send_next > 0) {
        s_fail_send_next--;
        return -1;
    }
    if (dataptr == NULL || size == 0) {
        return -1;
    }
    if (to != NULL && tolen >= sizeof(struct sockaddr_in)) {
        memcpy(&s_last_dst, to, sizeof(s_last_dst));
        s_has_dst = true;
    }
    const size_t n = size > sizeof(s_capture) - 1 ? sizeof(s_capture) - 1
                                                  : size;
    memcpy(s_capture, dataptr, n);
    s_capture[n] = '\0';
    s_capture_len = n;
    s_sent_count++;
    return (ssize_t)n;
}

uint32_t inet_addr(const char *cp)
{
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (cp == NULL ||
        sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)((a & 0xFFu) | ((b & 0xFFu) << 8) |
                      ((c & 0xFFu) << 16) | ((d & 0xFFu) << 24));
}

uint16_t htons(uint16_t hostshort)
{
    return (uint16_t)((hostshort >> 8) | (hostshort << 8));
}
