/**
 * @file    motor_bench_mapping.c
 * @brief   Bench-only permutation and trigger validation contracts
 */

#include "motor_bench_mapping.h"

#ifdef __cplusplus
extern "C" {
#endif

static const uint8_t _pwm_channels[MOTOR_BENCH_MAP_COUNT][3U] = {
    {3U, 2U, 1U},
    {3U, 1U, 2U},
    {2U, 3U, 1U},
    {2U, 1U, 3U},
    {1U, 3U, 2U},
    {1U, 2U, 3U},
};

/* 2026-08-30 bench evidence (T1 trigger scan, 2 seeds x 12 trigger points):
 * the CH3-driven (A) phase current appears on ADC rank2, CH2 (B) on rank3
 * and CH1 (C) on rank4, so map0 is {2,3,4}; the former {4,3,2} assumption
 * is kept as map5. */
static const uint8_t _adc_ranks[MOTOR_BENCH_MAP_COUNT][3U] = {
    {2U, 3U, 4U},
    {4U, 2U, 3U},
    {3U, 4U, 2U},
    {3U, 2U, 4U},
    {2U, 4U, 3U},
    {4U, 3U, 2U},
};

uint8_t motor_bench_pwm_channel_for_map(uint8_t map_id, uint8_t phase_index)
{
    if ((map_id >= MOTOR_BENCH_MAP_COUNT) || (phase_index >= 3U)) {
        return 0U;
    }

    return _pwm_channels[map_id][phase_index];
}

uint8_t motor_bench_adc_rank_for_map(uint8_t map_id, uint8_t phase_index)
{
    if ((map_id >= MOTOR_BENCH_MAP_COUNT) || (phase_index >= 3U)) {
        return 0U;
    }

    return _adc_ranks[map_id][phase_index];
}

bool motor_bench_adc_trigger_is_valid(uint16_t compare)
{
    return (compare >= MOTOR_BENCH_ADC_TRIGGER_MIN) &&
           (compare <= MOTOR_BENCH_ADC_TRIGGER_MAX);
}


#ifdef __cplusplus
}
#endif
