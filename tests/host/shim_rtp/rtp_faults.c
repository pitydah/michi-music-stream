/* Test-only fault injection for real production RTP session preparation. */
#include <stdatomic.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/socket.h>
static _Atomic int s_fail_heap;
static _Atomic int s_fail_bind;
void test_rtp_heap_fail_next_allocation(void) { s_fail_heap = 1; }
bool test_rtp_heap_should_fail(void) { return atomic_exchange(&s_fail_heap, 0) != 0; }
void test_rtp_bind_fail_next(void) { s_fail_bind = 1; }
int __real_bind(int fd, const struct sockaddr *addr, socklen_t length);
int __wrap_bind(int fd, const struct sockaddr *addr, socklen_t length)
{
    if (atomic_exchange(&s_fail_bind, 0)) {
        errno = EADDRINUSE;
        return -1;
    }
    return __real_bind(fd, addr, length);
}
