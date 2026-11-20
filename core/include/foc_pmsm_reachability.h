/**
 * @file    foc_pmsm_reachability.h
 * @brief   Cold-path PMSM steady-state operating-point evaluation.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Use the Current Core's peak-amplitude d/q convention and centered SVPWM.
 * The conservative voltage circle applies across electrical angles. A
 * point between it and the maximum angle-dependent circle is indeterminate.
 *
 * Usage example
 * -------------
 * Supply qualified motor parameters, the effective Current PWM configuration
 * and a signed operating point. Treat the result as a steady-state diagnostic.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * All storage is caller-owned. The evaluator has no mutable state and is
 * intended for configuration and diagnostics in the foreground.
 *
 * Failure / recovery
 * ------------------
 * An invalid, unqualified or ambiguous observation yields an indeterminate
 * result. The function never requests output or accepts a control command.
 */
#pragma once

#include "foc_core.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ----------------------------------------------------------------- */

/** Model result, limited to the reported steady-state assumptions. */
enum foc_pmsm_reachability_verdict {
    FOC_PMSM_INDETERMINATE = 0,
    FOC_PMSM_MODEL_FEASIBLE,
    FOC_PMSM_MODEL_INFEASIBLE,
};

/** Independent flags supporting the verdict. More than one limit may bind. */
enum foc_pmsm_reachability_reason {
    FOC_PMSM_REASON_MODEL_UNQUALIFIED = 1U,
    FOC_PMSM_REASON_INVALID_INPUT = 2U,
    FOC_PMSM_REASON_BUS_MINIMUM = 4U,
    FOC_PMSM_REASON_CURRENT_LIMIT = 8U,
    FOC_PMSM_REASON_TORQUE_LIMIT = 16U,
    FOC_PMSM_REASON_VOLTAGE_LIMIT = 32U,
    FOC_PMSM_REASON_ANGLE_DEPENDENT = 64U,
    FOC_PMSM_REASON_TORQUE_UNDEFINED = 128U,
};

/** Select the quantity supplied by the caller. */
enum foc_pmsm_target_kind {
    FOC_PMSM_TARGET_CURRENT,
    FOC_PMSM_TARGET_TORQUE,
};

/** Model estimates and caller-supplied provenance in the shaft frame. */
struct foc_pmsm_qualified_model {
    float rs_ohm;
    float ld_h;
    float lq_h;
    float flux_wb;
    uint32_t pole_pairs;
    uint32_t motor_id;
    uint32_t review_id;
    bool qualified;
};

/** Effective limits of the caller's chosen control mode. */
struct foc_pmsm_reachability_limits {
    struct foc_config pwm;
    float current_limit_a;
    /** Positive symmetric shaft torque cap. Zero disables this additional cap. */
    float torque_limit_nm;
};

/** A speed/Id slice and a requested Iq or shaft torque. */
struct foc_pmsm_operating_point {
    enum foc_pmsm_target_kind kind;
    float mechanical_rad_s;
    float id_a;
    /** Signed peak amperes for CURRENT or signed N*m for TORQUE. */
    float target;
    float vbus_v;
};

/** Complete model result. Range endpoints are meaningful when range_valid. */
struct foc_pmsm_reachability {
    enum foc_pmsm_reachability_verdict verdict;
    uint32_t reasons;
    uint32_t motor_id;
    uint32_t review_id;
    bool range_valid;
    /** A nonnegative voltage margin within 5% of the conservative limit. */
    bool near_voltage_limit;
    double electrical_rad_s;
    double id_a;
    double iq_a;
    double current_a;
    /** Torque per ampere at the specified Id, including reluctance torque. */
    double torque_per_q_amp;
    double torque_nm;
    double resistive_d_v;
    double resistive_q_v;
    double coupled_d_v;
    double coupled_q_v;
    double back_emf_q_v;
    double vd_v;
    double vq_v;
    double demanded_v;
    double all_angle_limit_v;
    double best_angle_limit_v;
    double voltage_margin_v;
    double iq_min_a;
    double iq_max_a;
    double torque_min_nm;
    double torque_max_nm;
};

/* Public Functions ------------------------------------------------------------- */

/** Evaluate one point and its all-angle Iq/torque interval, with zero current derivatives. */
enum foc_pmsm_reachability_verdict foc_pmsm_reachability_evaluate(
    const struct foc_pmsm_qualified_model *model,
    const struct foc_pmsm_reachability_limits *limits,
    const struct foc_pmsm_operating_point *point,
    struct foc_pmsm_reachability *result);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
