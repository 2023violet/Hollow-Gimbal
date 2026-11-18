/**
 * @file    foc_impedance_feasibility.c
 * @brief   Stateless motor-axis torque budget and equilibrium calculation.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Apply the impedance model at zero velocity and zero acceleration. Validate
 * the qualified input and publish one complete model verdict.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance_feasibility.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

static bool foc_impedance_feasibility_float_range(double value)
{
    const double magnitude = fabs(value);
    return magnitude == 0.0 || (magnitude >= FLT_TRUE_MIN && magnitude <= FLT_MAX);
}

/* Public Functions ------------------------------------------------------------- */

int foc_impedance_feasibility_evaluate(
    const struct foc_impedance_config *config,
    const struct foc_impedance_feasibility_input *input,
    struct foc_impedance_feasibility_result *result
)
{
    if (result == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    *result = (struct foc_impedance_feasibility_result){0};
    if (config == NULL || input == NULL) {
        return FOC_STATUS_ERR_NULL;
    }
    const float kt = config->torque_constant_nm_per_a;
    const float stiffness = config->stiffness_nm_per_rad;
    const float iq_limit = input->live_iq_limit_a;
    const float load = input->external_load_nm;
    const float feedforward = input->torque_feedforward_nm;
    const float tolerance = input->position_tolerance_rad;
    if (!isfinite(kt) || !isfinite(stiffness) || !isfinite(config->damping_nm_s_per_rad) ||
        !isfinite(config->torque_limit_nm) || !isfinite(iq_limit) || !isfinite(load) ||
        !isfinite(feedforward) || !isfinite(tolerance)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    if (kt <= 0.0f || stiffness < 0.0f || config->damping_nm_s_per_rad < 0.0f ||
        config->torque_limit_nm < 0.0f || iq_limit < 0.0f || tolerance < 0.0f) {
        return FOC_STATUS_ERR_INVALID_CONFIG;
    }
    if (!input->stationary_target) {
        result->verdict = FOC_IMPEDANCE_FEASIBILITY_MOVING_TARGET;
        return FOC_STATUS_OK;
    }
    if (input->torque_model_id == 0U || input->load_model_id == 0U ||
        !input->support_torque_accounted || !input->current_tracking_qualified) {
        result->verdict = FOC_IMPEDANCE_FEASIBILITY_UNKNOWN;
        return FOC_STATUS_OK;
    }

    /* Screen the Float32 request path's numeric range, but compare the
     * physical budget before rounding Kt*Iq up to a Float32 request. */
    const double current_torque = (double)kt * (double)iq_limit;
    if (!foc_impedance_feasibility_float_range(current_torque)) {
        return FOC_STATUS_ERR_NONFINITE;
    }
    const double capacity = fmin((double)config->torque_limit_nm, current_torque);
    const double margin = capacity - fabs((double)load);
    struct foc_impedance_feasibility_result diagnosis = {
        .torque_capacity_nm = capacity,
        .torque_margin_nm = margin,
    };
    if (margin < 0.0) {
        diagnosis.verdict = FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT;
        *result = diagnosis;
        return FOC_STATUS_OK;
    }

    /* At a saturation boundary, any error on the saturated side can
     * balance the load. Test zero error first to find the nearest solution. */
    const double limited_feedforward = fmin(capacity, fmax(-capacity, (double)feedforward));
    double error = 0.0;
    if (limited_feedforward != load) {
        if (stiffness == 0.0f) {
            diagnosis.verdict = FOC_IMPEDANCE_FEASIBILITY_NO_EQUILIBRIUM;
            *result = diagnosis;
            return FOC_STATUS_OK;
        }
        error = ((double)load - (double)feedforward) / (double)stiffness;
        if (!foc_impedance_feasibility_float_range(error)) {
            return FOC_STATUS_ERR_NONFINITE;
        }
    }
    diagnosis.equilibrium_error_rad = (float)error;
    diagnosis.has_equilibrium = true;
    diagnosis.at_torque_limit = margin == 0.0;
    diagnosis.position_constrained = stiffness > 0.0f && margin > 0.0;
    diagnosis.verdict = fabs(error) <= (double)tolerance
        ? FOC_IMPEDANCE_FEASIBILITY_REACHABLE
        : FOC_IMPEDANCE_FEASIBILITY_POSITION_TOLERANCE_EXCEEDED;
    *result = diagnosis;
    return FOC_STATUS_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
