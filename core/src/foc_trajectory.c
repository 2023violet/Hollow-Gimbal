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
        isfinite(config->max_decel_rad_s2) && isfinite(config->max_dt_s) &&
        isfinite(config->position_tolerance_rad) && config->max_velocity_rad_s > 0.0f &&
        config->max_accel_rad_s2 > 0.0f && config->max_decel_rad_s2 >= 0.0f &&
        config->max_dt_s > 0.0f && config->position_tolerance_rad >= 0.0f;
}

static float foc_trajectory_decel(const struct foc_trajectory_context *context)
{
    return context->config.max_decel_rad_s2 > 0.0f ? context->config.max_decel_rad_s2
                                                   : context->config.max_accel_rad_s2;
}

/* Move a planned velocity toward a desired one: the acceleration bounds speeding up in
 * the same direction, the deceleration bounds slowing down and reversing. */
static float foc_trajectory_slew(
    const struct foc_trajectory_context *context,
    float current,
    float desired,
    float dt_s
)
{
    const bool speeding_up = desired * current >= 0.0f && fabsf(desired) > fabsf(current);
    const float rate =
        speeding_up ? context->config.max_accel_rad_s2 : foc_trajectory_decel(context);
    return current + foc_trajectory_clamp(desired - current, rate * dt_s);
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
    float dt_s,
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
    output->acceleration_rad_s2 = context->state == FOC_TRAJECTORY_STATE_ACTIVE && dt_s > 0.0f
        ? (next_velocity_rad_s - context->planned_velocity_rad_s) / dt_s
        : 0.0f;
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
    context->position_seeded = false;
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
    return foc_trajectory_commit(
        context,
        foc_trajectory_slew(context, context->planned_velocity_rad_s, target, dt_s),
        dt_s,
        output
    );
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
    return foc_trajectory_commit(
        context,
        foc_trajectory_slew(context, context->planned_velocity_rad_s, target_velocity, dt_s),
        dt_s,
        output
    );
}

int foc_trajectory_track_step(
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
        return FOC_TRAJECTORY_STATUS_ERR_CONFIG;
    }
    const float position =
        context->position_seeded ? context->planned_position_rad : measured_position_rad;
    const float velocity = context->planned_velocity_rad_s;
    const float decel = foc_trajectory_decel(context);
    const float distance = target_position_rad - position;
    /* The fastest speed that still stops at the target in discrete time: this step's
     * travel plus a stop at the deceleration (v·dt + v²/2a = distance). */
    const float step_decel = decel * dt_s;
    const float braking =
        sqrtf(step_decel * step_decel + 2.0f * decel * fabsf(distance)) - step_decel;
    const float desired = distance == 0.0f
        ? 0.0f
        : copysignf(fminf(context->config.max_velocity_rad_s, braking), distance);
    float next_velocity = foc_trajectory_slew(context, velocity, desired, dt_s);
    next_velocity = foc_trajectory_clamp(next_velocity, context->config.max_velocity_rad_s);
    float next_position = position + 0.5f * (velocity + next_velocity) * dt_s;
    /* Arrival: reaching or passing the target from a speed one step can remove, so the
     * stop itself stays within the deceleration. A pass before that is micro-radians. */
    if ((target_position_rad - next_position) * distance <= 0.0f && fabsf(velocity) <= step_decel) {
        next_position = target_position_rad;
        next_velocity = 0.0f;
    }
    if (!isfinite(next_position) || !isfinite(next_velocity)) {
        return FOC_TRAJECTORY_STATUS_ERR_NONFINITE;
    }
    context->planned_position_rad = next_position;
    context->position_seeded = true;
    context->planned_velocity_rad_s = next_velocity;
    context->state = FOC_TRAJECTORY_STATE_ACTIVE;
    *output = (struct foc_trajectory_output){
        .velocity_ref_rad_s = next_velocity,
        .active = true,
        .position_ref_rad = next_position,
        .position_valid = true,
        .acceleration_rad_s2 = (next_velocity - velocity) / dt_s,
    };
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
