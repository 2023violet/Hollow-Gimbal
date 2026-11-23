/**
 * @file    foc_protect.c
 * @brief   Motor-protection rule evaluation.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_protect.h` and owns only the timed-rule state
 * - evaluates explicit measurements against effective thresholds
 * - never latches, inhibits or accesses registers
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_protect.h"

#include <math.h>
#include <stddef.h>

/* Private Macros --------------------------------------------------------------- */

/* A saturated window trips unless its mean error fell below this share of the previous. */
#define FOC_PROTECT_CONVERGENCE_RATIO 0.98f

/* Private Helpers -------------------------------------------------------------- */

static bool foc_protect_positive(float value)
{
    return isfinite(value) && value > 0.0f;
}

static bool foc_protect_window_valid(uint32_t window_us)
{
    return window_us < 0x80000000U;
}

/* Hold a condition for window_us; a zero window disables the rule. */
static bool foc_protect_hold(
    bool condition,
    uint32_t window_us,
    uint32_t now_us,
    bool *active,
    uint32_t *since_us
)
{
    if (!condition || window_us == 0U) {
        *active = false;
        return false;
    }
    if (!*active) {
        *active = true;
        *since_us = now_us;
    }
    return now_us - *since_us >= window_us;
}

static uint32_t foc_protect_electrical(
    const struct foc_protect_config *config,
    struct foc_protect_state *state,
    const struct foc_protect_input *input
)
{
    uint32_t faults = FOC_SAFE_FAULT_NONE;
    const struct foc_vector_abc *phase = &input->phase_current_a;
    const float peak = fmaxf(fabsf(phase->a), fmaxf(fabsf(phase->b), fabsf(phase->c)));
    if (!isfinite(peak) || peak > config->overcurrent_a) {
        faults |= FOC_SAFE_FAULT_OVER_CURRENT;
    }
    const bool high = !(input->vbus_v <= config->overvoltage_v);
    const bool low = !(input->vbus_v >= config->undervoltage_v);
    if (high || low) {
        if (state->voltage_streak < config->voltage_samples) {
            state->voltage_streak++;
        }
        if (state->voltage_streak >= config->voltage_samples) {
            faults |= high ? FOC_SAFE_FAULT_OVER_VOLTAGE : FOC_SAFE_FAULT_UNDER_VOLTAGE;
        }
    } else {
        state->voltage_streak = 0U;
    }
    return faults;
}

/*
 * Tracking: a saturated outer loop must make progress. A deliberate acceleration or
 * reversal lowers the mean speed error window after window; a wrong commutation that
 * runs away or rocks the rotor does not. Sign-based checks alone miss the rocking case,
 * where saturation and direction come and go within every window.
 */
static bool foc_protect_divergence(
    const struct foc_protect_config *config,
    struct foc_protect_state *state,
    const struct foc_protect_input *input
)
{
    if (!input->speed_valid || !input->reference_valid || config->divergence_us == 0U) {
        state->window_active = false;
        state->baseline_valid = false;
        return false;
    }
    if (!state->window_active) {
        state->window_active = true;
        state->window_since_us = input->now_us;
        state->window_samples = 0U;
        state->window_saturated = 0U;
        state->window_error_sum = 0.0f;
    }
    state->window_samples++;
    state->window_saturated += fabsf(input->iq_ref_a) >= config->divergence_current_a ? 1U : 0U;
    state->window_error_sum += fabsf(input->speed_ref_rad_s - input->speed_rad_s);
    if (input->now_us - state->window_since_us < config->divergence_us) {
        return false;
    }
    const float mean = state->window_error_sum / (float)state->window_samples;
    const bool saturated = 2U * state->window_saturated >= state->window_samples;
    state->window_active = false;
    if (!saturated) {
        state->baseline_valid = false;
        return false;
    }
    if (state->baseline_valid &&
        !(mean < FOC_PROTECT_CONVERGENCE_RATIO * state->baseline_error_rad_s)) {
        return true;
    }
    state->baseline_error_rad_s = mean;
    state->baseline_valid = true;
    return false;
}

/* Public Functions ------------------------------------------------------------- */

bool foc_protect_validate(const struct foc_protect_config *config)
{
    if (config == NULL) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    return foc_protect_positive(config->overcurrent_a) &&
        foc_protect_positive(config->undervoltage_v) && isfinite(config->overvoltage_v) &&
        config->overvoltage_v > config->undervoltage_v && config->voltage_samples > 0U &&
        foc_protect_positive(config->overspeed_rad_s) &&
        foc_protect_positive(config->sustained_current_a) &&
        foc_protect_positive(config->stall_current_a) &&
        foc_protect_positive(config->stall_speed_rad_s) &&
        foc_protect_positive(config->divergence_current_a) &&
        foc_protect_window_valid(config->sustained_us) &&
        foc_protect_window_valid(config->stall_us) &&
        foc_protect_window_valid(config->divergence_us);
}

void foc_protect_reset(struct foc_protect_state *state)
{
    if (state != NULL) {
        *state = (struct foc_protect_state){0};
    }
}

uint32_t foc_protect_step(
    const struct foc_protect_config *config,
    struct foc_protect_state *state,
    const struct foc_protect_input *input
)
{
    if (config == NULL || state == NULL || input == NULL || !config->enabled) {
        return FOC_SAFE_FAULT_NONE;
    }
    uint32_t faults = foc_protect_electrical(config, state, input);
    if (input->scope == FOC_PROTECT_SCOPE_ELECTRICAL) {
        return faults;
    }
    const float speed = fabsf(input->speed_rad_s);
    if (input->speed_valid && !(speed <= config->overspeed_rad_s)) {
        faults |= FOC_SAFE_FAULT_OVER_SPEED;
    }
    if (foc_protect_hold(
            input->current_magnitude_a >= config->sustained_current_a,
            config->sustained_us,
            input->now_us,
            &state->sustained_active,
            &state->sustained_since_us
        )) {
        faults |= FOC_SAFE_FAULT_SUSTAINED_CURRENT;
    }
    if (input->scope != FOC_PROTECT_SCOPE_MOTION) {
        return faults;
    }
    if (foc_protect_hold(
            input->speed_valid && input->current_magnitude_a >= config->stall_current_a &&
                speed < config->stall_speed_rad_s,
            config->stall_us,
            input->now_us,
            &state->stall_active,
            &state->stall_since_us
        )) {
        faults |= FOC_SAFE_FAULT_STALL;
    }
    if (foc_protect_divergence(config, state, input)) {
        faults |= FOC_SAFE_FAULT_TRACKING_DIVERGENCE;
    }
    return faults;
}

/* ---------------------------------------- end of file ----------------------------------------- */
