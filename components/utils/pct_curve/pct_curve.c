/**
 * @file    pct_curve.c
 * @brief   Portable piecewise percent-curve helpers
 * @version v1.0.0
 * @date    2026.04.23
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "pct_curve.h"
#include <stdbool.h>

static uint8_t _pct_curve_clamp_percent(uint8_t percent)
{
    return (percent > 100U) ? 100U : percent;
}

static bool _pct_curve_is_monotonic(const struct pct_curve_anchor *anchors, size_t anchor_count)
{
    size_t index;

    if ((anchors == NULL) || (anchor_count == 0U)) {
        return true;
    }

    for (index = 1U; index < anchor_count; ++index) {
        uint8_t prev_input  = _pct_curve_clamp_percent(anchors[index - 1U].input_percent);
        uint8_t next_input  = _pct_curve_clamp_percent(anchors[index].input_percent);
        uint8_t prev_output = _pct_curve_clamp_percent(anchors[index - 1U].output_percent);
        uint8_t next_output = _pct_curve_clamp_percent(anchors[index].output_percent);

        if ((next_input < prev_input) || (next_output < prev_output)) {
            return false;
        }
    }

    return true;
}

uint8_t pct_curve_interpolate(const struct pct_curve_anchor *anchors,
                              size_t                         anchor_count,
                              uint8_t                        logical_percent)
{
    uint32_t logical = _pct_curve_clamp_percent(logical_percent);

    if ((anchors == NULL) || (anchor_count == 0U)) {
        return (uint8_t)logical;
    }
    if (!_pct_curve_is_monotonic(anchors, anchor_count)) {
        return (uint8_t)logical;
    }

    if (logical <= _pct_curve_clamp_percent(anchors[0].input_percent)) {
        return _pct_curve_clamp_percent(anchors[0].output_percent);
    }

    for (size_t index = 1U; index < anchor_count; ++index) {
        uint32_t lower_input  = _pct_curve_clamp_percent(anchors[index - 1U].input_percent);
        uint32_t upper_input  = _pct_curve_clamp_percent(anchors[index].input_percent);
        uint32_t lower_output = _pct_curve_clamp_percent(anchors[index - 1U].output_percent);
        uint32_t upper_output = _pct_curve_clamp_percent(anchors[index].output_percent);

        if (logical > upper_input) {
            continue;
        }

        if (upper_input <= lower_input) {
            return (uint8_t)upper_output;
        }

        /*
         * Rounding-to-nearest keeps the helper aligned with user-facing
         * percent displays instead of systematically biasing downward.
         */
        uint32_t input_offset = logical - lower_input;
        uint32_t input_span   = upper_input - lower_input;
        uint32_t output_span  = upper_output - lower_output;
        uint32_t mapped =
            lower_output + ((input_offset * output_span) + (input_span / 2U)) / input_span;

        return (uint8_t)_pct_curve_clamp_percent((uint8_t)mapped);
    }

    return _pct_curve_clamp_percent(anchors[anchor_count - 1U].output_percent);
}

uint16_t pct_curve_scale_permille(uint16_t duty_permille, uint8_t effective_volume_percent)
{
    uint32_t effective = _pct_curve_clamp_percent(effective_volume_percent);
    uint32_t scaled    = ((uint32_t)duty_permille * effective) / 100U;

    if ((duty_permille > 0U) && (effective > 0U) && (scaled == 0U)) {
        scaled = 1U;
    }

    return (uint16_t)scaled;
}

/* ---------------------------------------- end of file ----------------------------------------- */
