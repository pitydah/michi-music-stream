#pragma once
/* Test-only heap with deterministic single allocation failure injection.
 * This header shadows generic shim ONLY in the real RTP engine target. */
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define MALLOC_CAP_8BIT (1u << 0)
#define MALLOC_CAP_DMA (1u << 3)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
void test_rtp_heap_fail_next_allocation(void);
bool test_rtp_heap_should_fail(void);
static inline void *heap_caps_malloc(size_t bytes, uint32_t caps)
{
    (void)caps;
    return test_rtp_heap_should_fail() ? NULL : malloc(bytes);
}
static inline void *heap_caps_calloc(size_t count, size_t size, uint32_t caps)
{
    (void)caps;
    return test_rtp_heap_should_fail() ? NULL : calloc(count, size);
}
static inline void heap_caps_free(void *ptr) { free(ptr); }
