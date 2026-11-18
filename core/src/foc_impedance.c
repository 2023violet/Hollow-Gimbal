/**
 * @file    foc_impedance.c
 * @brief   Float32 motor-shaft spring, damper, and feedforward torque.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Evaluate a stateless torque request and convert the limited request to q
 * current using explicit caller policy. Publish results only after validation.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance.h"

#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

static float foc_impedance_clamp(float value, float limit)
{
    if (value > limit) {
        return limit;
    }
    if (value < -limit) {
        return -limit;
    }
    return value;
}

/* Public Functions ------------------------------------------------------------- */

int foc_impedance_step(
    const struct foc_impedance_config *config,
    const struct foc_impedance_input *input,
    struct foc_impedance_output *output
)
{
    if (output == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *output = (struct foc_impedance_output){0};
    if (config == NULL || input == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    if (!isfinite(config->stiffness_nm_per_rad) || !isfinite(config->damping_nm_s_per_rad) ||
        !isfinite(config->torque_constant_nm_per_a) || !isfinite(config->torque_limit_nm) ||
        !isfinite(input->position_error_rad) || !isfinite(input->reference_velocity_rad_s) ||
        !isfinite(input->measured_velocity_rad_s) || !isfinite(input->torque_feedforward_nm) ||
        !isfinite(input->live_iq_limit_a)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    if (config->stiffness_nm_per_rad < 0.0f || config->damping_nm_s_per_rad < 0.0f ||
        config->torque_constant_nm_per_a <= 0.0f || config->torque_limit_nm < 0.0f ||
        input->live_iq_limit_a < 0.0f) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }

    const float velocity_error = input->reference_velocity_rad_s - input->measured_velocity_rad_s;
    const float spring = config->stiffness_nm_per_rad * input->position_error_rad;
    const float damper = config->damping_nm_s_per_rad * velocity_error;
    const float live_torque_limit = config->torque_constant_nm_per_a * input->live_iq_limit_a;
    if (!isfinite(velocity_error) || !isfinite(spring) || !isfinite(damper) ||
        !isfinite(live_torque_limit)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    const float restoring_torque = spring + damper;
    const float requested_torque = restoring_torque + input->torque_feedforward_nm;
    if (!isfinite(restoring_torque) || !isfinite(requested_torque)) {
        return FOC_STATUS_ERR_NONFINITE;
    }

    const float torque_limit =
        config->torque_limit_nm < live_torque_limit ? config->torque_limit_nm : live_torque_limit;
    const float limited_torque = foc_impedance_clamp(requested_torque, torque_limit);
    const float requested_iq = limited_torque / config->torque_constant_nm_per_a;
    if (!isfinite(requested_iq)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    /* Rounding in Kt * limit followed by division can cross the original bound. */
    const float iq = foc_impedance_clamp(requested_iq, input->live_iq_limit_a);
    *output = (struct foc_impedance_output){
        .requested_torque_nm = requested_torque,
        .limited_torque_nm = limited_torque,
        .iq_ref_a = iq,
        .limited = limited_torque != requested_torque || iq != requested_iq,
        .valid = true,
    };
    return FOC_STATUS_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
