/**
 * @file    foc_motion.c
 * @brief   Independent rotor coordinate, prediction, and outer-loop calculations.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Compute in SI units and commit velocity state only after successful math.
 * Conditional integral admission accounts for the active q current limit.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_motion.h"

#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

static float foc_motion_clamp(float value, float limit)
{
    return fmaxf(-limit, fminf(value, limit));
}

/* Public Functions ------------------------------------------------------------- */

int foc_rotor_phase_from_counts(
    const struct foc_rotor_config *config,
    uint32_t mechanical_phase_count,
    uint32_t counts_per_revolution,
    float *electrical_phase_rad
)
{
    if (electrical_phase_rad == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *electrical_phase_rad = 0.0f;
    if (config == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (counts_per_revolution < 2U || counts_per_revolution > UINT32_C(65536) ||
        mechanical_phase_count >= counts_per_revolution || config->pole_pairs == 0U) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }
    float offset;
    const int status = foc_angle_normalize(config->electrical_offset_rad, &offset);
    if (status != FOC_STATUS_OK) {
        return status;
    }
    /* Both factors <= 65535: their product fits uint32_t without wrapping. */
    const uint32_t electrical_count =
        (mechanical_phase_count * (uint32_t)config->pole_pairs) % counts_per_revolution;
    const float phase =
        (float)electrical_count * (6.28318530717958647692f / (float)counts_per_revolution);
    return foc_angle_normalize(phase + offset, electrical_phase_rad);
}

int foc_rotor_electrical(
    const struct foc_rotor_config *config,
    float mechanical_rad,
    float mechanical_rad_s,
    struct foc_electrical_state *output
)
{
    if (output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *output = (struct foc_electrical_state){0};
    if (config == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (config->pole_pairs == 0U) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }
    const float poles = (float)config->pole_pairs;
    const float velocity = poles * mechanical_rad_s;
    float phase;
    float offset;
    if (!isfinite(velocity) || foc_angle_normalize(mechanical_rad, &phase) != FOC_STATUS_OK ||
        foc_angle_normalize(config->electrical_offset_rad, &offset) != FOC_STATUS_OK) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    /* Wrap the mechanical phase for conversion only. Position control retains full turns. */
    (void)foc_angle_normalize(poles * phase + offset, &output->angle_rad);
    output->velocity_rad_s = velocity;
    return FOC_STATUS_OK;
}

int foc_rotor_predict(
    const struct foc_prediction_config *config,
    const struct foc_electrical_state *sample,
    uint32_t elapsed_us,
    float *angle_rad
)
{
    if (angle_rad == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *angle_rad = 0.0f;
    if (config == NULL || sample == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!isfinite(config->max_accel_rad_s2) || !isfinite(config->max_error_rad) ||
        !isfinite(sample->angle_rad) || !isfinite(sample->velocity_rad_s)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    if (config->max_accel_rad_s2 < 0.0f || config->max_error_rad < 0.0f ||
        config->max_prediction_us >= UINT32_C(0x80000000)) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }
    if (elapsed_us > config->max_prediction_us) {
        return FOC_STATUS_ERR_INVALID_DT;
    }
    const float dt_s = (float)elapsed_us * 1.0e-6f;
    const float error_rad = 0.5f * dt_s * dt_s * config->max_accel_rad_s2;
    if (error_rad > config->max_error_rad) {
        return FOC_STATUS_ERR_PREDICTION;
    }
    return foc_angle_normalize(sample->angle_rad + sample->velocity_rad_s * dt_s, angle_rad);
}

int foc_velocity_init(
    struct foc_velocity_context *context,
    const struct foc_velocity_config *config
)
{
    if (context == NULL || config == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!isfinite(config->kp_a_per_rad_s) || !isfinite(config->ki_a_per_rad) ||
        !isfinite(config->max_dt_s)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    if (config->kp_a_per_rad_s < 0.0f || config->ki_a_per_rad < 0.0f || config->max_dt_s <= 0.0f) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }
    *context = (struct foc_velocity_context){.config = *config, .initialized = true};
    return FOC_STATUS_OK;
}

int foc_velocity_reset(struct foc_velocity_context *context)
{
    if (context == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_STATUS_ERR_NOT_INITIALIZED;
    }
    context->integral_a = 0.0f;
    return FOC_STATUS_OK;
}

int foc_velocity_step(
    struct foc_velocity_context *context,
    const struct foc_velocity_input *input,
    struct foc_velocity_output *output
)
{
    if (output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *output = (struct foc_velocity_output){0};
    if (context == NULL || input == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_STATUS_ERR_NOT_INITIALIZED;
    }
    if (!isfinite(input->dt_s) || input->dt_s <= 0.0f || input->dt_s > context->config.max_dt_s) {
        return FOC_STATUS_ERR_INVALID_DT;
    }
    if (!isfinite(input->ref_rad_s) || !isfinite(input->measured_rad_s) ||
        !isfinite(input->iq_limit_a)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    const float limit = input->iq_limit_a;
    if (limit <= 0.0f) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }
    if (!input->enabled) {
        return FOC_STATUS_OK;
    }

    const float error = input->ref_rad_s - input->measured_rad_s;
    const float proportional = context->config.kp_a_per_rad_s * error;
    const float previous = foc_motion_clamp(context->integral_a, limit);
    const float trial = previous + context->config.ki_a_per_rad * input->dt_s * error;
    const float requested = proportional + trial;
    if (!isfinite(trial) || !isfinite(requested)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    /* Admit only the trial increment supported by the actuator, allowing unwind. */
    float integral = trial;
    if (requested > limit && trial > previous) {
        integral = fmaxf(previous, limit - proportional);
    } else if (requested < -limit && trial < previous) {
        integral = fminf(previous, -limit - proportional);
    }
    integral = foc_motion_clamp(integral, limit);
    const float target = proportional + integral;
    if (!isfinite(target)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    output->iq_ref_a = foc_motion_clamp(target, limit);
    output->saturated =
        integral != trial || previous != context->integral_a || target != output->iq_ref_a;
    output->enabled = true;
    context->integral_a = integral;
    return FOC_STATUS_OK;
}

int foc_position_step(
    const struct foc_position_config *config,
    float ref_rad,
    float measured_rad,
    struct foc_position_output *output
)
{
    if (output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *output = (struct foc_position_output){0};
    if (config == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!isfinite(config->kp_per_s) || !isfinite(config->velocity_limit_rad_s) ||
        !isfinite(ref_rad) || !isfinite(measured_rad)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    if (config->kp_per_s < 0.0f || config->velocity_limit_rad_s <= 0.0f) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }
    const float target = config->kp_per_s * (ref_rad - measured_rad);
    if (!isfinite(target)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    output->velocity_ref_rad_s = foc_motion_clamp(target, config->velocity_limit_rad_s);
    output->limited = target != output->velocity_ref_rad_s;
    return FOC_STATUS_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
