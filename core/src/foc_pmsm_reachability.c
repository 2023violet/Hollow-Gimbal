/**
 * @file    foc_pmsm_reachability.c
 * @brief   Evaluate steady-state PMSM voltage and current margins.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * At fixed speed and Id, voltage is affine in Iq. Intersect its all-angle
 * circle with the current vector and optional torque bounds.
 *
 * Usage example
 * -------------
 * See foc_pmsm_reachability.h for input provenance and result semantics.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * This foreground calculation owns no retained state.
 *
 * Failure / recovery
 * ------------------
 * Invalid or ambiguous input returns an indeterminate result.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_pmsm_reachability.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Private Functions ------------------------------------------------------------ */

static bool model_valid(const struct foc_pmsm_qualified_model *model)
{
    return model != NULL && model->qualified && model->motor_id != 0U &&
        model->pole_pairs != 0U && isfinite(model->rs_ohm) && model->rs_ohm > 0.0f &&
        isfinite(model->ld_h) && model->ld_h > 0.0f && isfinite(model->lq_h) &&
        model->lq_h > 0.0f && isfinite(model->flux_wb) && model->flux_wb > 0.0f;
}

static bool input_valid(
    const struct foc_pmsm_reachability_limits *limits,
    const struct foc_pmsm_operating_point *point
)
{
    return limits != NULL && point != NULL && foc_config_validate(&limits->pwm) == FOC_STATUS_OK &&
        limits->pwm.duty_min < 0.5f && limits->pwm.duty_max > 0.5f &&
        isfinite(limits->current_limit_a) && limits->current_limit_a > 0.0f &&
        isfinite(limits->torque_limit_nm) && limits->torque_limit_nm >= 0.0f &&
        (point->kind == FOC_PMSM_TARGET_CURRENT || point->kind == FOC_PMSM_TARGET_TORQUE) &&
        isfinite(point->mechanical_rad_s) && isfinite(point->id_a) && isfinite(point->target) &&
        isfinite(point->vbus_v) && point->vbus_v > 0.0f;
}

static void iq_interval(
    const struct foc_pmsm_reachability_limits *limits,
    double rs,
    double negative_omega_lq,
    struct foc_pmsm_reachability *result
)
{
    const double id = fabs(result->id_a);
    const double current_limit = limits->current_limit_a;
    if (id > current_limit) {
        return;
    }
    double iq_cap = sqrt((current_limit - id) * (current_limit + id));
    if (limits->torque_limit_nm > 0.0f && result->torque_per_q_amp != 0.0) {
        iq_cap = fmin(iq_cap, limits->torque_limit_nm / fabs(result->torque_per_q_amp));
    }

    /* v(q) = offset + direction*q. Use the perpendicular distance to the
     * line and avoid subtracting nearly equal squared voltage components. */
    const double offset_d = result->resistive_d_v;
    const double offset_q = result->back_emf_q_v + result->coupled_q_v;
    const double norm = hypot(negative_omega_lq, rs);
    const double center = -(offset_d * negative_omega_lq + offset_q * rs) / (norm * norm);
    const double distance = fabs(offset_d * rs - offset_q * negative_omega_lq) / norm;
    const double voltage = result->all_angle_limit_v;
    if (distance > voltage) {
        return;
    }
    const double half_span = sqrt((voltage - distance) * (voltage + distance)) / norm;
    const double lo = fmax(center - half_span, -iq_cap);
    const double hi = fmin(center + half_span, iq_cap);
    if (lo <= hi) {
        result->range_valid = true;
        result->iq_min_a = lo;
        result->iq_max_a = hi;
        result->torque_min_nm = fmin(lo * result->torque_per_q_amp, hi * result->torque_per_q_amp);
        result->torque_max_nm = fmax(lo * result->torque_per_q_amp, hi * result->torque_per_q_amp);
    }
}

/* Public Functions ------------------------------------------------------------- */

enum foc_pmsm_reachability_verdict foc_pmsm_reachability_evaluate(
    const struct foc_pmsm_qualified_model *model,
    const struct foc_pmsm_reachability_limits *limits,
    const struct foc_pmsm_operating_point *point,
    struct foc_pmsm_reachability *result
)
{
    if (result == NULL) {
        return FOC_PMSM_INDETERMINATE;
    }
    *result = (struct foc_pmsm_reachability){0};
    if (model != NULL) {
        result->motor_id = model->motor_id;
        result->review_id = model->review_id;
    }
    if (!model_valid(model)) {
        result->reasons = FOC_PMSM_REASON_MODEL_UNQUALIFIED;
        return result->verdict;
    }
    if (!input_valid(limits, point)) {
        result->reasons = FOC_PMSM_REASON_INVALID_INPUT;
        return result->verdict;
    }

    const double id = point->id_a;
    const double pole_pairs = model->pole_pairs;
    const double kt = 1.5 * pole_pairs * (model->flux_wb + ((double)model->ld_h - model->lq_h) * id);
    result->torque_per_q_amp = kt;
    if (point->kind == FOC_PMSM_TARGET_TORQUE && kt == 0.0) {
        result->reasons = FOC_PMSM_REASON_TORQUE_UNDEFINED;
        if (point->target != 0.0f) {
            result->verdict = FOC_PMSM_MODEL_INFEASIBLE;
        }
        return result->verdict;
    }

    const double iq = point->kind == FOC_PMSM_TARGET_CURRENT ? point->target : point->target / kt;
    if (!isfinite(iq) || fabs(iq) > FLT_MAX) {
        result->reasons = FOC_PMSM_REASON_INVALID_INPUT;
        return result->verdict;
    }
    const double omega = pole_pairs * point->mechanical_rad_s;
    const double negative_omega_lq = -omega * model->lq_h;
    result->electrical_rad_s = omega;
    result->id_a = id;
    result->iq_a = iq;
    result->current_a = hypot(id, iq);
    result->torque_nm = point->kind == FOC_PMSM_TARGET_TORQUE ? point->target : kt * iq;
    result->resistive_d_v = model->rs_ohm * id;
    result->resistive_q_v = model->rs_ohm * iq;
    result->coupled_d_v = negative_omega_lq * iq;
    result->coupled_q_v = omega * model->ld_h * id;
    result->back_emf_q_v = omega * model->flux_wb;
    result->vd_v = result->resistive_d_v + result->coupled_d_v;
    result->vq_v = result->resistive_q_v + result->coupled_q_v + result->back_emf_q_v;
    result->demanded_v = hypot(result->vd_v, result->vq_v);

    /* The existing SVPWM centers phase extrema around 0.5. Clipping is
     * determined by the narrower side of an asymmetric duty window. */
    const double inv_sqrt3 = 0.57735026918962576451;
    const double dc_bus = point->vbus_v;
    const double duty_span =
        2.0 * fmin(0.5 - limits->pwm.duty_min, limits->pwm.duty_max - 0.5);
    const double core_cap = fmin(limits->pwm.voltage_limit_v, dc_bus * inv_sqrt3);
    result->all_angle_limit_v = fmin(core_cap, dc_bus * duty_span * inv_sqrt3);
    result->best_angle_limit_v = fmin(core_cap, dc_bus * duty_span * (2.0 / 3.0));
    result->voltage_margin_v = result->all_angle_limit_v - result->demanded_v;
    result->near_voltage_limit = result->voltage_margin_v >= 0.0 &&
        result->voltage_margin_v <= 0.05 * result->all_angle_limit_v;

    if (point->vbus_v < limits->pwm.vbus_min_v) {
        result->reasons |= FOC_PMSM_REASON_BUS_MINIMUM;
    } else {
        iq_interval(limits, model->rs_ohm, negative_omega_lq, result);
    }
    if (result->current_a > limits->current_limit_a) {
        result->reasons |= FOC_PMSM_REASON_CURRENT_LIMIT;
    }
    if (limits->torque_limit_nm > 0.0f && fabs(result->torque_nm) > limits->torque_limit_nm) {
        result->reasons |= FOC_PMSM_REASON_TORQUE_LIMIT;
    }
    if (result->demanded_v > result->best_angle_limit_v) {
        result->reasons |= FOC_PMSM_REASON_VOLTAGE_LIMIT;
    }

    if (result->reasons != 0U) {
        result->verdict = FOC_PMSM_MODEL_INFEASIBLE;
    } else if (result->voltage_margin_v < 0.0) {
        result->reasons = FOC_PMSM_REASON_ANGLE_DEPENDENT;
    } else {
        result->verdict = FOC_PMSM_MODEL_FEASIBLE;
    }
    return result->verdict;
}

/* ---------------------------------------- end of file ----------------------------------------- */
