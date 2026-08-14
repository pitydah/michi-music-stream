#pragma once
/* Shim for host-side tests: deterministic stand-in for esp_random.h.
 *
 * The firmware uses esp_fill_random() (hardware RNG) as its ONLY entropy
 * source for the identity seed; on the host it is replaced by a
 * deterministic xorshift stream so identity tests are reproducible.
 * TEST-ONLY: never compiled into firmware. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint64_t *test_esp_random_state(void)
{
    static uint64_t s_state = 0x9E3779B97F4A7C15ULL; /* test-only */
    return &s_state;
}

/* Deterministic xorshift64*. Tests may reseed the stream to force
 * distinct boot sequences. */
static inline void test_esp_random_seed(uint64_t seed)
{
    *test_esp_random_state() = seed;
}

static inline uint32_t test_esp_random_next(void)
{
    uint64_t s = *test_esp_random_state();
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    *test_esp_random_state() = s;
    return (uint32_t)((s * 0x2545F4914F6CDD1DULL) >> 32);
}

static inline void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = (uint8_t)(test_esp_random_next() & 0xFFu);
    }
}

static inline uint32_t esp_random(void)
{
    return test_esp_random_next();
}

#ifdef __cplusplus
}
#endif
