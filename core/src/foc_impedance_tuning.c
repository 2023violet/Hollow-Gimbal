/**
 * @file    foc_impedance_tuning.c
 * @brief   Model-based impedance gain design for cold-path use.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Calculate the unsaturated continuous-time design in Double, then validate
 * and publish an immutable Float32 candidate for an independent consumer.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_impedance_tuning.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

static bool foc_impedance_tuning_float_range(double value)
{
    return isfinite(value) && fabs(value) <= FLT_MAX;
}

/* Public Functions ------------------------------------------------------------- */

int foc_impedance_tuning_design(
    const struct foc_impedance_tuning_input *input,
    struct foc_impedance_tuning_candidate *candidate
)
{
    if (candidate == NULL) {
        return FOC_IMPEDANCE_TUNING_ERR_NULL;
    }
    *candidate = (struct foc_impedance_tuning_candidate){0};
    if (input == NULL) {
        return FOC_IMPEDANCE_TUNING_ERR_NULL;
    }
    if (!isfinite(input->inertia_kg_m2) || !isfinite(input->viscous_damping_nm_s_per_rad) ||
        !isfinite(input->target_natural_frequency_rad_s) ||
        !isfinite(input->target_damping_ratio) || !isfinite(input->torque_constant_nm_per_a) ||
        !isfinite(input->max_stiffness_nm_per_rad) || !isfinite(input->max_damping_nm_s_per_rad) ||
        !isfinite(input->iq_limit_a) || !isfinite(input->sample_period_s) ||
        !isfinite(input->extra_delay_s)) {
        return FOC_IMPEDANCE_TUNING_ERR_NUMERIC;
    }
    if (input->model_id == 0U ||
        (input->damping_source != FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED &&
         (input->damping_source != FOC_IMPEDANCE_TUNING_DAMPING_ASSUMED_ZERO ||
          input->viscous_damping_nm_s_per_rad != 0.0f)) ||
        input->inertia_kg_m2 <= 0.0f || input->viscous_damping_nm_s_per_rad < 0.0f ||
        input->target_natural_frequency_rad_s <= 0.0f || input->target_damping_ratio <= 0.0f ||
        input->torque_constant_nm_per_a <= 0.0f || input->max_stiffness_nm_per_rad <= 0.0f ||
        input->max_damping_nm_s_per_rad < 0.0f || input->iq_limit_a <= 0.0f ||
        input->sample_period_s <= 0.0f || input->extra_delay_s < 0.0f) {
        return FOC_IMPEDANCE_TUNING_ERR_CONFIG;
    }
    const double omega = input->target_natural_frequency_rad_s;
    const double time_budget = (double)input->sample_period_s + input->extra_delay_s;
    if (omega * time_budget > 0.25) {
        return FOC_IMPEDANCE_TUNING_ERR_TIMING;
    }
    const double inertia = input->inertia_kg_m2;
    const double passive_damping = input->viscous_damping_nm_s_per_rad;
    const double stiffness = inertia * omega * omega;
    const double damping = 2.0 * input->target_damping_ratio * inertia * omega - passive_damping;
    const double current_torque = (double)input->torque_constant_nm_per_a * input->iq_limit_a;
    if (damping < 0.0) {
        return FOC_IMPEDANCE_TUNING_ERR_NEGATIVE_DAMPING;
    }
    if (!foc_impedance_tuning_float_range(stiffness) ||
        !foc_impedance_tuning_float_range(damping) ||
        !foc_impedance_tuning_float_range(current_torque)) {
        return FOC_IMPEDANCE_TUNING_ERR_NUMERIC;
    }
    if (stiffness > input->max_stiffness_nm_per_rad || damping > input->max_damping_nm_s_per_rad) {
        return FOC_IMPEDANCE_TUNING_ERR_GAIN_LIMIT;
    }

    const float k = (float)stiffness;
    const float d = (float)damping;
    const float torque = (float)current_torque;
    if (k <= 0.0f || torque <= 0.0f) {
        return FOC_IMPEDANCE_TUNING_ERR_NUMERIC;
    }
    const double ideal_omega = sqrt((double)k / inertia);
    const double ideal_zeta = (passive_damping + d) / (2.0 * inertia * ideal_omega);
    if (!foc_impedance_tuning_float_range(ideal_omega) ||
        !foc_impedance_tuning_float_range(ideal_zeta) || (float)ideal_omega <= 0.0f ||
        (float)ideal_zeta <= 0.0f) {
        return FOC_IMPEDANCE_TUNING_ERR_NUMERIC;
    }
    /* Rounded gains can move ideal omega appreciably at the subnormal edge. */
    if (ideal_omega * time_budget > 0.25) {
        return FOC_IMPEDANCE_TUNING_ERR_TIMING;
    }
    *candidate = (struct foc_impedance_tuning_candidate){
        .model_id = input->model_id,
        .damping_source = input->damping_source,
        .stiffness_nm_per_rad = k,
        .damping_nm_s_per_rad = d,
        .ideal_natural_frequency_rad_s = (float)ideal_omega,
        .ideal_damping_ratio = (float)ideal_zeta,
        .current_limited_torque_nm = torque,
        .valid = true,
    };
    return FOC_IMPEDANCE_TUNING_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
