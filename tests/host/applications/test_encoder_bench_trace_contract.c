/**
 * @file    test_encoder_bench_trace_contract.c
 * @brief   Host contract tests for the bench-only encoder RAM trace
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder.h"

#define TEST_ASSERT(expr)                                                        \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(stderr,                                                       \
                    "[test_encoder_bench_trace_contract] assertion failed: %s " \
                    "(%s:%d)\n",                                                 \
                    #expr,                                                        \
                    __FILE__,                                                     \
                    __LINE__);                                                    \
            exit(1);                                                              \
        }                                                                         \
    } while (0)

static void test_trace_is_empty_after_clear(void)
{
    struct encoder_bench_sample sample;

    encoder_bench_trace_clear();
    TEST_ASSERT(encoder_bench_trace_count() == 0U);
    TEST_ASSERT(!encoder_bench_trace_read(0U, &sample));
}

static void test_trace_retains_latest_32_samples_in_order(void)
{
    struct encoder_bench_input input;
    struct encoder_bench_sample first;
    struct encoder_bench_sample last;
    uint32_t index;

    memset(&input, 0, sizeof(input));
    encoder_bench_trace_clear();
    for (index = 0U; index < 40U; ++index) {
        input.raw_word = (uint16_t)(1000U + index);
        input.angle_mech_rad = (float)index * 0.01f;
        input.angle_elec_rad = (float)index * 0.02f;
        input.velocity_rads = (index < 20U) ? 2.0f : -3.0f;
        input.total_enc = (float)index;
        encoder_bench_trace_update(&input, (int32_t)(index + 10U), 0U);
    }

    TEST_ASSERT(encoder_bench_trace_count() == ENCODER_BENCH_TRACE_DEPTH);
    TEST_ASSERT(encoder_bench_trace_read(0U, &first));
    TEST_ASSERT(encoder_bench_trace_read(31U, &last));
    TEST_ASSERT(first.sequence == 9U);
    TEST_ASSERT(first.raw_word == 1008U);
    TEST_ASSERT(first.corrected_count == 18);
    TEST_ASSERT(fabsf(first.angle_mech_rad - 0.08f) < 0.0001f);
    TEST_ASSERT(first.direction == 1);
    TEST_ASSERT(last.sequence == 40U);
    TEST_ASSERT(last.raw_word == 1039U);
    TEST_ASSERT(last.corrected_count == 49);
    TEST_ASSERT(fabsf(last.angle_elec_rad - 0.78f) < 0.0001f);
    TEST_ASSERT(last.direction == -1);
}

static void test_trace_captures_error_flags_and_zero_direction(void)
{
    struct encoder_bench_input input;
    struct encoder_bench_sample sample;

    memset(&input, 0, sizeof(input));
    input.raw_word = 0x4321U;
    input.velocity_rads = 0.0f;
    encoder_bench_trace_clear();
    encoder_bench_trace_update(&input,
                               0x1234,
                               (uint8_t)(ENCODER_BENCH_TRACE_ERROR_SPI |
                                         ENCODER_BENCH_TRACE_ERROR_PLL));

    TEST_ASSERT(encoder_bench_trace_read(0U, &sample));
    TEST_ASSERT(sample.error_flags ==
                (uint8_t)(ENCODER_BENCH_TRACE_ERROR_SPI |
                          ENCODER_BENCH_TRACE_ERROR_PLL));
    TEST_ASSERT(sample.direction == 0);
    TEST_ASSERT(sample.corrected_count == 0x1234);
}

static void test_trace_freeze_preserves_last_running_sample_until_clear(void)
{
    struct encoder_bench_input input;
    struct encoder_bench_sample sample;

    memset(&input, 0, sizeof(input));
    encoder_bench_trace_clear();

    input.raw_word = 0x1000U;
    encoder_bench_trace_update(&input, 100, 0U);
    encoder_bench_trace_freeze();

    input.raw_word = 0x2000U;
    encoder_bench_trace_update(&input, 200, 0U);
    TEST_ASSERT(encoder_bench_trace_count() == 1U);
    TEST_ASSERT(encoder_bench_trace_read(0U, &sample));
    TEST_ASSERT(sample.raw_word == 0x1000U);
    TEST_ASSERT(sample.corrected_count == 100);

    encoder_bench_trace_resume();
    encoder_bench_trace_update(&input, 200, 0U);
    TEST_ASSERT(encoder_bench_trace_count() == 2U);
    TEST_ASSERT(encoder_bench_trace_read(1U, &sample));
    TEST_ASSERT(sample.raw_word == 0x2000U);
    TEST_ASSERT(sample.corrected_count == 200);

    encoder_bench_trace_clear();
    TEST_ASSERT(encoder_bench_trace_count() == 0U);
    encoder_bench_trace_update(&input, 300, 0U);
    TEST_ASSERT(encoder_bench_trace_count() == 1U);
}

int main(void)
{
    test_trace_is_empty_after_clear();
    test_trace_retains_latest_32_samples_in_order();
    test_trace_captures_error_flags_and_zero_direction();
    test_trace_freeze_preserves_last_running_sample_until_clear();
    puts("[test_encoder_bench_trace_contract] passed");
    return 0;
}
