/**
 * @file    foc_following_error.c
 * @brief   Bounded position following error over capture-time samples.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Maintain one caller-owned continuous excursion interval using the
 * unsigned half-range rule used for ordered FalconFOC source times.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_following_error.h"

#include <math.h>
#include <stddef.h>

#define FOC_FOLLOWING_HALF_RANGE UINT32_C(0x80000000)

/* Private Helpers -------------------------------------------------------------- */

static enum foc_following_state foc_following_error_interrupt(
    struct foc_following_error_state *state,
    enum foc_following_state status
)
{
    state->elapsed_us = 0U;
    state->status = status;
    return status;
}

/* Public Functions ------------------------------------------------------------- */

bool foc_following_error_validate(const struct foc_following_error_config *config)
{
    return config != NULL &&
        (!config->enabled ||
         (isfinite(config->threshold_rad) && config->threshold_rad > 0.0f &&
          config->duration_us != 0U && config->duration_us < FOC_FOLLOWING_HALF_RANGE &&
          config->max_gap_us != 0U && config->max_gap_us < FOC_FOLLOWING_HALF_RANGE));
}

enum foc_following_state foc_following_error_step(
    const struct foc_following_error_config *config,
    struct foc_following_error_state *state,
    const struct foc_following_error_input *input
)
{
    if (state == NULL || input == NULL || !foc_following_error_validate(config)) {
        return FOC_FOLLOWING_INACTIVE;
    }
    if (!config->enabled || !input->applicable) {
        *state = (struct foc_following_error_state){0};
        return FOC_FOLLOWING_INACTIVE;
    }
    if (!input->sensor_valid || !isfinite(input->measured_rad)) {
        return foc_following_error_interrupt(state, FOC_FOLLOWING_INVALID_SENSOR);
    }
    if (!input->reference_valid || !isfinite(input->reference_rad)) {
        return foc_following_error_interrupt(state, FOC_FOLLOWING_NO_REFERENCE);
    }

    const uint32_t elapsed = input->captured_us - state->sample_us;
    if (state->seen) {
        if (elapsed == 0U) {
            return state->status;
        }
        if (elapsed >= FOC_FOLLOWING_HALF_RANGE) {
            return foc_following_error_interrupt(state, FOC_FOLLOWING_TIME_ORDER);
        }
    }
    const bool uninterrupted = state->seen && elapsed <= config->max_gap_us &&
        (state->status == FOC_FOLLOWING_PENDING || state->status == FOC_FOLLOWING_TRIGGERED);
    state->sample_us = input->captured_us;
    state->seen = true;

    const float error = input->reference_rad - input->measured_rad;
    if (error >= -config->threshold_rad && error <= config->threshold_rad) {
        return foc_following_error_interrupt(state, FOC_FOLLOWING_TRACKING);
    }
    if (!uninterrupted) {
        return foc_following_error_interrupt(state, FOC_FOLLOWING_PENDING);
    }

    if (state->elapsed_us >= config->duration_us ||
        elapsed >= config->duration_us - state->elapsed_us) {
        state->elapsed_us = config->duration_us;
        state->status = FOC_FOLLOWING_TRIGGERED;
    } else {
        state->elapsed_us += elapsed;
        state->status = FOC_FOLLOWING_PENDING;
    }
    return state->status;
}

/* ---------------------------------------- end of file ----------------------------------------- */
