#pragma once
/* Test-only output driver: reports REAL RTP worker interactions with a fake I2S state. */
#include <stdint.h>
#include <stdbool.h>
void test_rtp_output_reset(void);
uint64_t test_rtp_output_written_bytes(void);
uint32_t test_rtp_output_rejected_writes(void);
void test_rtp_output_force_quiesce_fail(bool failed);
void test_rtp_bind_fail_next(void);
void test_rtp_heap_fail_next_allocation(void);
