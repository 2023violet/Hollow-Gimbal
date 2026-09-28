/**
 * @file    encoder_bench_trace.c
 * @brief   Bench-only RAM trace for the encoder path
 */

#include "encoder_bench_trace.h"

#include <string.h>

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)

static struct encoder_bench_sample _encoder_samples[ENCODER_BENCH_TRACE_DEPTH];
static uint32_t                   _encoder_sample_head;
static uint32_t                   _encoder_sample_count;
static uint32_t                   _encoder_sample_sequence;
static bool                       _encoder_trace_frozen;

_Static_assert(sizeof(_encoder_samples) <= (2U * 1024U),
               "encoder_bench_trace_storage_must_not_exceed_2_kib");

void encoder_bench_trace_clear(void)
{
    memset(_encoder_samples, 0, sizeof(_encoder_samples));
    _encoder_sample_head = 0U;
    _encoder_sample_count = 0U;
    _encoder_sample_sequence = 0U;
    _encoder_trace_frozen = false;
}

void encoder_bench_trace_freeze(void)
{
    _encoder_trace_frozen = true;
}

void encoder_bench_trace_resume(void)
{
    _encoder_trace_frozen = false;
}

void encoder_bench_trace_update(const struct encoder_bench_input *input,
                                int32_t corrected_count,
                                uint8_t error_flags)
{
    struct encoder_bench_sample *sample;
    uint32_t                    slot;
    int8_t                      direction;

    if ((input == NULL) || _encoder_trace_frozen) {
        return;
    }

    if (_encoder_sample_count < ENCODER_BENCH_TRACE_DEPTH) {
        slot = (_encoder_sample_head + _encoder_sample_count) % ENCODER_BENCH_TRACE_DEPTH;
        _encoder_sample_count++;
    } else {
        slot = _encoder_sample_head;
        _encoder_sample_head = (_encoder_sample_head + 1U) % ENCODER_BENCH_TRACE_DEPTH;
    }

    if (input->velocity_rads > 0.0f) {
        direction = 1;
    } else if (input->velocity_rads < 0.0f) {
        direction = -1;
    } else {
        direction = 0;
    }

    sample = &_encoder_samples[slot];
    sample->sequence = ++_encoder_sample_sequence;
    sample->raw_word = input->raw_word;
    sample->corrected_count = corrected_count;
    sample->angle_mech_rad = input->angle_mech_rad;
    sample->angle_elec_rad = input->angle_elec_rad;
    sample->velocity_rads = input->velocity_rads;
    sample->total_enc = input->total_enc;
    sample->direction = direction;
    sample->error_flags = error_flags;
}

bool encoder_bench_trace_read(uint32_t index, struct encoder_bench_sample *out)
{
    uint32_t slot;

    if ((out == NULL) || (index >= _encoder_sample_count)) {
        return false;
    }

    slot = (_encoder_sample_head + index) % ENCODER_BENCH_TRACE_DEPTH;
    memcpy(out, &_encoder_samples[slot], sizeof(*out));
    return true;
}

uint32_t encoder_bench_trace_count(void)
{
    return _encoder_sample_count;
}

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
