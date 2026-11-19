/**
 * @file    foc_motion_limits.c
 * @brief   Continuous mechanical position and signed request admission.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Classify absolute targets and boundary direction in the caller's single
 * shaft frame. All quantities are evaluated as supplied.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_motion_limits.h"

#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

static bool
foc_motion_limits_contains(const struct foc_motion_limits_config *config, float position_rad)
{
    return isfinite(position_rad) && position_rad >= config->position_min_rad &&
        position_rad <= config->position_max_rad;
}

/* Public Functions ------------------------------------------------------------- */

bool foc_motion_limits_validate(const struct foc_motion_limits_config *config)
{
    if (config == NULL) {
        return false;
    }
    if (!config->enabled) {
        return true;
    }
    const float minimum = config->position_min_rad;
    const float maximum = config->position_max_rad;
    const float tolerance = config->tolerance_rad;
    if (config->mechanical_frame_id == 0U || !isfinite(minimum) || !isfinite(maximum) ||
        !isfinite(tolerance) || maximum <= minimum || tolerance < 0.0f) {
        return false;
    }
    const float span = maximum - minimum;
    return isfinite(span) && tolerance < span - tolerance &&
        minimum + tolerance < maximum - tolerance;
}

enum foc_motion_rejection
foc_motion_limits_reference(const struct foc_motion_limits_config *config, float position_rad)
{
    if (!foc_motion_limits_validate(config)) {
        return FOC_MOTION_REJECT_CONFIG;
    }
    return !config->enabled || foc_motion_limits_contains(config, position_rad)
        ? FOC_MOTION_REJECT_NONE
        : FOC_MOTION_REJECT_POSITION;
}

enum foc_motion_rejection foc_motion_limits_check(
    const struct foc_motion_limits_config *config,
    float measured_rad,
    const struct foc_motion_limits_request *request
)
{
    if (!foc_motion_limits_validate(config)) {
        return FOC_MOTION_REJECT_CONFIG;
    }
    if (!config->enabled) {
        return FOC_MOTION_REJECT_NONE;
    }
    if (request == NULL) {
        return FOC_MOTION_REJECT_CONFIG;
    }
    if (!isfinite(measured_rad)) {
        return FOC_MOTION_REJECT_FEEDBACK;
    }
    if (request->has_position && !foc_motion_limits_contains(config, request->position_ref_rad)) {
        return FOC_MOTION_REJECT_POSITION;
    }
    if (!isfinite(request->velocity_ref_rad_s) || !isfinite(request->requested_velocity_rad_s) ||
        !isfinite(request->iq_ref_a)) {
        return FOC_MOTION_REJECT_DIRECTION;
    }

    const bool lower_outward = measured_rad <= config->position_min_rad + config->tolerance_rad &&
        ((request->has_position && request->position_ref_rad < measured_rad) ||
         request->velocity_ref_rad_s < 0.0f || request->requested_velocity_rad_s < 0.0f ||
         request->iq_ref_a < 0.0f);
    const bool upper_outward = measured_rad >= config->position_max_rad - config->tolerance_rad &&
        ((request->has_position && request->position_ref_rad > measured_rad) ||
         request->velocity_ref_rad_s > 0.0f || request->requested_velocity_rad_s > 0.0f ||
         request->iq_ref_a > 0.0f);
    return lower_outward || upper_outward ? FOC_MOTION_REJECT_DIRECTION : FOC_MOTION_REJECT_NONE;
}

/* ---------------------------------------- end of file ----------------------------------------- */
