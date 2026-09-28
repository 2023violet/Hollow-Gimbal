/**
 * @file    motor_bench_mapping.h
 * @brief   Pure bench mapping and trigger contracts shared by firmware/tests
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOTOR_BENCH_MAP_COUNT             (6U)
#define MOTOR_BENCH_MAP_DEFAULT           (0U)
#define MOTOR_BENCH_ADC_TRIGGER_DEFAULT   (3740U) /* trigger at the counter peak, see wk_tmr.c */
#define MOTOR_BENCH_ADC_TRIGGER_MIN       (256U)
#define MOTOR_BENCH_ADC_TRIGGER_MAX       (3740U)

enum motor_bench_phase_index {
    MOTOR_BENCH_PHASE_A = 0U,
    MOTOR_BENCH_PHASE_B = 1U,
    MOTOR_BENCH_PHASE_C = 2U,
};

uint8_t motor_bench_pwm_channel_for_map(uint8_t map_id, uint8_t phase_index);
uint8_t motor_bench_adc_rank_for_map(uint8_t map_id, uint8_t phase_index);
bool    motor_bench_adc_trigger_is_valid(uint16_t compare);

#ifdef __cplusplus
}
#endif
