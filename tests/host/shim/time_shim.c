/* Shim for host-side tests: overrides libc time() with the fake wall
 * clock (see time_shim.h). The object precedes -lc in the link line, so
 * michi_time.c's time(NULL) reads the INJECTED clock - which is what
 * the "timestamp within +-90 s of the injected time" test needs.
 * TEST-ONLY: never compiled into firmware. */

#include "time_shim.h"

#include <time.h>

static uint64_t s_wall_sec;

uint64_t test_time_get_sec(void)
{
    return s_wall_sec;
}

void test_time_set_sec(uint64_t unix_sec)
{
    s_wall_sec = unix_sec;
}

void test_time_advance_sec(uint64_t sec)
{
    s_wall_sec += sec;
}

void test_time_reset(void)
{
    s_wall_sec = 0;
}

time_t time(time_t *tloc)
{
    if (tloc != NULL) {
        *tloc = (time_t)s_wall_sec;
    }
    return (time_t)s_wall_sec;
}
