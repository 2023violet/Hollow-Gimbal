/**
 * @file    encoder_bench_trace.h
 * @brief   Hardware-independent contract for the bench encoder trace
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENCODER_BENCH_TRACE_DEPTH      (32U)
#define ENCODER_BENCH_TRACE_ERROR_SPI  (1U << 0)
#define ENCODER_BENCH_TRACE_ERROR_PLL  (1U << 1)

struct encoder_bench_sample {
    uint32_t sequence;
    uint16_t raw_word;
    int32_t  corrected_count;
    float    angle_mech_rad;
    float    angle_elec_rad;
    float    velocity_rads;
    float    total_enc;
    int8_t   direction;
    uint8_t  error_flags;
};

struct encoder_bench_input {
    uint16_t raw_word;
    float    angle_mech_rad;
    float    angle_elec_rad;
    float    velocity_rads;
    float    total_enc;
};

bool encoder_bench_trace_read(uint32_t index, struct encoder_bench_sample *out);
uint32_t encoder_bench_trace_count(void);
void encoder_bench_trace_clear(void);
void encoder_bench_trace_freeze(void);
void encoder_bench_trace_resume(void);
void encoder_bench_trace_update(const struct encoder_bench_input *input,
                                int32_t corrected_count,
                                uint8_t error_flags);

#ifdef __cplusplus
}
#endif
