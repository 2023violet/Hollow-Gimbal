/**
 * @file    foc_trajectory.c
 * @brief   Bounded Float32 velocity and point-to-point reference generation.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Implementation role
 * -------------------
 * Apply bounded Float32 reference updates without owning sensor, Runtime,
 * Safety, or output state.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_trajectory.h"

#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

static float foc_trajectory_clamp(float value, float limit)
{
    return fmaxf(-limit, fminf(value, limit));
}

static bool foc_trajectory_config_valid(const struct foc_trajectory_config *config)
{
    if (config == NULL) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    return isfinite(config->max_velocity_rad_s) && isfinite(config->max_accel_rad_s2) &&
        isfinite(config->max_dt_s) && isfinite(config->position_tolerance_rad) &&
        config->max_velocity_rad_s > 0.0f && config->max_accel_rad_s2 > 0.0f &&
        config->max_dt_s > 0.0f && config->position_tolerance_rad >= 0.0f;
}

static int foc_trajectory_dt_valid(const struct foc_trajectory_context *context, float dt_s)
{
    if (!isfinite(dt_s) || dt_s <= 0.0f ||
        (context->config.enabled && dt_s > context->config.max_dt_s)) {
        return FOC_TRAJECTORY_STATUS_ERR_INVALID_DT;
    }
    return 0;
}

static int foc_trajectory_prepare(
    struct foc_trajectory_context *context,
    float dt_s,
    struct foc_trajectory_output *output
)
{
    if (output == NULL) {
        return FOC_TRAJECTORY_STATUS_ERR_NULL;
    }
    *output = (struct foc_trajectory_output){0};
    if (context == NULL) {
        return FOC_TRAJECTORY_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_TRAJECTORY_STATUS_ERR_STATE;
    }
    if (context->state == FOC_TRAJECTORY_STATE_INVALID) {
        return FOC_TRAJECTORY_STATUS_ERR_STATE;
    }
    return foc_trajectory_dt_valid(context, dt_s);
}

static int foc_trajectory_commit(
    struct foc_trajectory_context *context,
    float next_velocity_rad_s,
    struct foc_trajectory_output *output
)
{
    if (!isfinite(next_velocity_rad_s)) {
        return FOC_TRAJECTORY_STATUS_ERR_NONFINITE;
    }
    if (context->config.enabled) {
        next_velocity_rad_s =
            foc_trajectory_clamp(next_velocity_rad_s, context->config.max_velocity_rad_s);
    }
    context->planned_velocity_rad_s = next_velocity_rad_s;
    context->state = FOC_TRAJECTORY_STATE_ACTIVE;
    output->velocity_ref_rad_s = next_velocity_rad_s;
    output->active = context->config.enabled;
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int foc_trajectory_init(
    struct foc_trajectory_context *context,
    const struct foc_trajectory_config *config
)
{
    if (context == NULL || config == NULL) {
        return FOC_TRAJECTORY_STATUS_ERR_NULL;
    }
    if (!foc_trajectory_config_valid(config)) {
        return FOC_TRAJECTORY_STATUS_ERR_CONFIG;
    }
    *context = (struct foc_trajectory_context){
        .config = *config,
        .state = FOC_TRAJECTORY_STATE_READY,
        .initialized = true,
    };
    return 0;
}

int foc_trajectory_reset(struct foc_trajectory_context *context, float initial_velocity_rad_s)
{
    if (context == NULL) {
        return FOC_TRAJECTORY_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_TRAJECTORY_STATUS_ERR_STATE;
    }
    if (!isfinite(initial_velocity_rad_s)) {
        return FOC_TRAJECTORY_STATUS_ERR_NONFINITE;
    }
    if (context->config.enabled &&
        fabsf(initial_velocity_rad_s) > context->config.max_velocity_rad_s) {
        return FOC_TRAJECTORY_STATUS_ERR_CONFIG;
    }
    context->planned_velocity_rad_s = initial_velocity_rad_s;
    context->state = FOC_TRAJECTORY_STATE_READY;
    return 0;
}

int foc_trajectory_invalidate(struct foc_trajectory_context *context)
{
    if (context == NULL) {
        return FOC_TRAJECTORY_STATUS_ERR_NULL;
    }
    if (!context->initialized) {
        return FOC_TRAJECTORY_STATUS_ERR_STATE;
    }
    context->state = FOC_TRAJECTORY_STATE_INVALID;
    return 0;
}

int foc_trajectory_velocity_step(
    struct foc_trajectory_context *context,
    float target_velocity_rad_s,
    float dt_s,
    struct foc_trajectory_output *output
)
{
    const int status = foc_trajectory_prepare(context, dt_s, output);
    if (status != 0) {
        return status;
    }
    if (!isfinite(target_velocity_rad_s)) {
        return FOC_TRAJECTORY_STATUS_ERR_NONFINITE;
    }
    if (!context->config.enabled) {
        output->velocity_ref_rad_s = target_velocity_rad_s;
        return 0;
    }
    const float target =
        foc_trajectory_clamp(target_velocity_rad_s, context->config.max_velocity_rad_s);
    const float step_limit = context->config.max_accel_rad_s2 * dt_s;
    if (!isfinite(step_limit)) {
        return FOC_TRAJECTORY_STATUS_ERR_NONFINITE;
    }
    const float delta = foc_trajectory_clamp(target - context->planned_velocity_rad_s, step_limit);
    return foc_trajectory_commit(context, context->planned_velocity_rad_s + delta, output);
}

int foc_trajectory_position_step(
    struct foc_trajectory_context *context,
    float target_position_rad,
    float measured_position_rad,
    float dt_s,
    struct foc_trajectory_output *output
)
{
    const int status = foc_trajectory_prepare(context, dt_s, output);
    if (status != 0) {
        return status;
    }
    if (!isfinite(target_position_rad) || !isfinite(measured_position_rad)) {
        return FOC_TRAJECTORY_STATUS_ERR_NONFINITE;
    }
    if (!context->config.enabled) {
        return 0;
    }
    const float distance = target_position_rad - measured_position_rad;
    const float absolute_distance = fabsf(distance);
    float target_velocity = 0.0f;
    if (absolute_distance > context->config.position_tolerance_rad) {
        const float brake_speed =
            sqrtf(2.0f * context->config.max_accel_rad_s2 * absolute_distance);
        if (!isfinite(brake_speed)) {
            return FOC_TRAJECTORY_STATUS_ERR_NONFINITE;
        }
        target_velocity =
            copysignf(fminf(context->config.max_velocity_rad_s, brake_speed), distance);
    }
    const float step_limit = context->config.max_accel_rad_s2 * dt_s;
    const float delta =
        foc_trajectory_clamp(target_velocity - context->planned_velocity_rad_s, step_limit);
    return foc_trajectory_commit(context, context->planned_velocity_rad_s + delta, output);
}

/* ---------------------------------------- end of file ----------------------------------------- */
