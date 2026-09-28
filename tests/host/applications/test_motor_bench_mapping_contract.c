/**
 * @file    test_motor_bench_mapping_contract.c
 * @brief   Host contract tests for bench-only PWM/ADC mapping tables
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "motor_bench_mapping.h"

#define TEST_ASSERT(expr)                                                               \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            fprintf(stderr,                                                             \
                    "[test_motor_bench_mapping_contract] assertion failed: %s\n",     \
                    #expr);                                                             \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

static void test_all_pwm_permutations(void)
{
    static const uint8_t expected[6][3] = {
        {3U, 2U, 1U},
        {3U, 1U, 2U},
        {2U, 3U, 1U},
        {2U, 1U, 3U},
        {1U, 3U, 2U},
        {1U, 2U, 3U},
    };

    for (uint8_t map_id = 0U; map_id < 6U; ++map_id) {
        for (uint8_t phase = 0U; phase < 3U; ++phase) {
            TEST_ASSERT(motor_bench_pwm_channel_for_map(map_id, phase) == expected[map_id][phase]);
        }
    }
}

static void test_all_adc_permutations(void)
{
    /* 2026-08-30 bench evidence: the CH3-driven (A) phase current appears on
     * ADC rank2, CH2 (B) on rank3 and CH1 (C) on rank4, so map0 is {2,3,4}
     * and the former {4,3,2} wiring assumption moved to map5. */
    static const uint8_t expected[6][3] = {
        {2U, 3U, 4U},
        {4U, 2U, 3U},
        {3U, 4U, 2U},
        {3U, 2U, 4U},
        {2U, 4U, 3U},
        {4U, 3U, 2U},
    };

    for (uint8_t map_id = 0U; map_id < 6U; ++map_id) {
        for (uint8_t phase = 0U; phase < 3U; ++phase) {
            TEST_ASSERT(motor_bench_adc_rank_for_map(map_id, phase) == expected[map_id][phase]);
        }
    }
}

static void test_invalid_mapping_indices_are_rejected(void)
{
    TEST_ASSERT(motor_bench_pwm_channel_for_map(6U, 0U) == 0U);
    TEST_ASSERT(motor_bench_pwm_channel_for_map(0U, 3U) == 0U);
    TEST_ASSERT(motor_bench_adc_rank_for_map(6U, 0U) == 0U);
    TEST_ASSERT(motor_bench_adc_rank_for_map(0U, 3U) == 0U);
}

static void test_adc_trigger_contract(void)
{
    static const uint16_t allowed[] = {
        512U, 1024U, 1536U, 1874U, 2300U, 2812U, 3200U, 3500U, 3740U,
    };

    for (size_t i = 0U; i < (sizeof(allowed) / sizeof(allowed[0])); ++i) {
        TEST_ASSERT(motor_bench_adc_trigger_is_valid(allowed[i]));
    }

    TEST_ASSERT(motor_bench_adc_trigger_is_valid(256U));
    TEST_ASSERT(motor_bench_adc_trigger_is_valid(3740U));
    TEST_ASSERT(!motor_bench_adc_trigger_is_valid(255U));
    TEST_ASSERT(!motor_bench_adc_trigger_is_valid(3741U));
    TEST_ASSERT(!motor_bench_adc_trigger_is_valid(UINT16_MAX));
}

static void test_default_adc_trigger_uses_the_validated_low_side_window(void)
{
    TEST_ASSERT(MOTOR_BENCH_ADC_TRIGGER_DEFAULT == 3740U);
}

int main(void)
{
    test_all_pwm_permutations();
    test_all_adc_permutations();
    test_invalid_mapping_indices_are_rejected();
    test_adc_trigger_contract();
    test_default_adc_trigger_uses_the_validated_low_side_window();
    puts("[test_motor_bench_mapping_contract] PASS");
    return 0;
}
