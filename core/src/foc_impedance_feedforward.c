/**
 * @file    foc_impedance_feedforward.c
 * @brief   Cold-path computation of qualified motion-only model torque.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Evaluate one signed motor-shaft model request without applying it.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance_feedforward.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

/* Preserve the sign of the residual when the rounded sum equals the cap. */
static double foc_impedance_feedforward_roundoff(double first, double second, double sum)
{
    const double second_part = sum - first;
    return (first - (sum - second_part)) + (second - second_part);
}

/* Public Functions ------------------------------------------------------------- */

int foc_impedance_feedforward_evaluate(
    const struct foc_impedance_feedforward_input *input,
    struct foc_impedance_feedforward_result *result
)
{
    if (result == NULL) {
        return FOC_IMPEDANCE_FEEDFORWARD_ERR_NULL;
    }
    *result = (struct foc_impedance_feedforward_result){0};
    if (input == NULL) {
        return FOC_IMPEDANCE_FEEDFORWARD_ERR_NULL;
    }

    const float j = input->inertia_kg_m2;
    const float b = input->viscous_damping_nm_s_per_rad;
    const float speed = input->reference_velocity_rad_s;
    const float acceleration = input->reference_acceleration_rad_s2;
    const double limit = input->limit_nm;
    if (!isfinite(j) || !isfinite(b) || !isfinite(speed) || !isfinite(acceleration) ||
        !isfinite(limit)) {
        return FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
    }
    if (j <= 0.0f || b < 0.0f || limit < 0.0 || input->model_id == 0U ||
        input->model_frame_id == 0U) {
        return FOC_IMPEDANCE_FEEDFORWARD_ERR_CONFIG;
    }
    if (!input->reference_qualified || input->reference_frame_id != input->model_frame_id) {
        return FOC_IMPEDANCE_FEEDFORWARD_ERR_REFERENCE;
    }

    const double inertial_nm = (double)j * (double)acceleration;
    const double viscous_nm = (double)b * (double)speed;
    const double torque = inertial_nm + viscous_nm;
    const double magnitude = fabs(torque);
    if (!isfinite(torque) || magnitude > FLT_MAX ||
        (magnitude != 0.0 && magnitude < FLT_TRUE_MIN)) {
        return FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC;
    }
    if (magnitude > limit) {
        return FOC_IMPEDANCE_FEEDFORWARD_ERR_LIMIT;
    }
    if (magnitude == limit) {
        const double missed = foc_impedance_feedforward_roundoff(inertial_nm, viscous_nm, torque);
        if ((torque >= 0.0 && missed > 0.0) || (torque <= 0.0 && missed < 0.0)) {
            return FOC_IMPEDANCE_FEEDFORWARD_ERR_LIMIT;
        }
    }

    *result = (struct foc_impedance_feedforward_result){
        .torque_nm = (float)torque,
        .model_id = input->model_id,
        .mechanical_frame_id = input->model_frame_id,
        .valid = true,
    };
    return FOC_IMPEDANCE_FEEDFORWARD_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
