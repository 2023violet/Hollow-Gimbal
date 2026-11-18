/**
 * @file    foc_impedance_feasibility.h
 * @brief   Cold-path static equilibrium diagnosis for motor-axis impedance.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Evaluate ideal static hold and the minimum equilibrium position error in
 * homogeneous motor-shaft coordinates with symmetric torque/current limits.
 *
 * Usage example
 * -------------
 * Use the same impedance configuration as the proposed command. Bind Kt,
 * static load and tracking qualifications to the actual motor-shaft setup
 * and evaluate with the currently allowed Iq limit.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Stateless. The caller owns provenance and physical model qualification.
 * The result does not change commands, gains, limits or output authority.
 *
 * Failure / recovery
 * ------------------
 * Invalid inputs clear the result. Unknown or moving cases publish a verdict
 * without a model budget or an equilibrium error.
 */
#pragma once

#include "foc_impedance.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

enum foc_impedance_feasibility_verdict {
    FOC_IMPEDANCE_FEASIBILITY_INVALID = 0,
    FOC_IMPEDANCE_FEASIBILITY_UNKNOWN,
    FOC_IMPEDANCE_FEASIBILITY_MOVING_TARGET,
    FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT,
    FOC_IMPEDANCE_FEASIBILITY_NO_EQUILIBRIUM,
    FOC_IMPEDANCE_FEASIBILITY_POSITION_TOLERANCE_EXCEEDED,
    FOC_IMPEDANCE_FEASIBILITY_REACHABLE,
};

/** All signals refer to the same signed motor-shaft axis. */
struct foc_impedance_feasibility_input {
    /** Symmetric admitted Iq limit in A. */
    float live_iq_limit_a;
    /** Signed opposing static torque in N*m, to be balanced by motor torque. */
    float external_load_nm;
    /** Signed feedforward torque in N*m. */
    float torque_feedforward_nm;
    /** Nonnegative magnitude of allowed steady position error, in rad. */
    float position_tolerance_rad;
    /** Nonzero identities reference qualified data. Zero means unknown. */
    uint32_t torque_model_id;
    uint32_t load_model_id;
    /** True for a fixed target (zero reference speed and acceleration). */
    bool stationary_target;
    /** Other static supports are excluded or included in the qualified net load. */
    bool support_torque_accounted;
    /** Tracking of the budgeted torque via the admitted current is qualified. */
    bool current_tracking_qualified;
};

struct foc_impedance_feasibility_result {
    enum foc_impedance_feasibility_verdict verdict;
    /** Nonnegative min(configured torque limit, Kt * live Iq limit), in N*m. */
    double torque_capacity_nm;
    /** Capacity minus magnitude of the net load, in N*m. Signed. */
    double torque_margin_nm;
    /** Signed reference-minus-measured error at the nearest ideal equilibrium. */
    float equilibrium_error_rad;
    /** A modeled equilibrium exists; the error field is meaningful. */
    bool has_equilibrium;
    /** True for a modeled equilibrium at the torque boundary, with zero reserve. */
    bool at_torque_limit;
    /** True when the ideal equilibrium fixes a unique position. */
    bool position_constrained;
};

/* Public Functions ------------------------------------------------------------- */

/**
 * Diagnose ideal zero-speed, zero-acceleration equilibrium. D is validated
 * with the rest of the impedance configuration; it has no static contribution.
 * K=0 permits equilibrium at any position when limited feedforward balances the
 * load. At the torque boundary, a saturated half-line (or, with zero capacity,
 * all errors) can balance the load. Reachable means that an equilibrium exists
 * in tolerance; it provides no guarantee of convergence, stability or safety.
 * Use distinct output storage. Unknown fields take finite placeholders.
 * @return FOC_STATUS_OK for a diagnosis (including UNKNOWN, MOVING_TARGET);
 *         FOC_STATUS_ERR_NULL, FOC_STATUS_ERR_NONFINITE for nonfinite or
 *         unrepresentable arithmetic, or FOC_STATUS_ERR_INVALID_CONFIG.
 */
int foc_impedance_feasibility_evaluate(const struct foc_impedance_config *config,
                                       const struct foc_impedance_feasibility_input *input,
                                       struct foc_impedance_feasibility_result *result);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
