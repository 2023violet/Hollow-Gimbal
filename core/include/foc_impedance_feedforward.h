/**
 * @file    foc_impedance_feedforward.h
 * @brief   Motor-shaft model feedforward for a qualified motion reference.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Evaluate J*a_ref + B*v_ref as a model torque candidate, in N*m.
 *
 * Usage example
 * -------------
 * Supply J/B from a qualified model and a same-aperture velocity and
 * acceleration from a bounded, continuous position/velocity trajectory.
 * Check the candidate before explicitly supplying it to Impedance. Account
 * for any separately qualified load torque and total command ceiling there.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Stateless C11. The caller owns the model, coordinate and source timing
 * qualifications, and the allocation of the feedforward torque budget.
 *
 * Failure / recovery
 * ------------------
 * A supplied result is zero on any rejection. Excess torque is rejected
 * instead of clipped so it cannot masquerade as a full model contribution.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

enum foc_impedance_feedforward_status {
    FOC_IMPEDANCE_FEEDFORWARD_OK = 0,
    FOC_IMPEDANCE_FEEDFORWARD_ERR_NULL = -1,
    FOC_IMPEDANCE_FEEDFORWARD_ERR_CONFIG = -2,
    FOC_IMPEDANCE_FEEDFORWARD_ERR_REFERENCE = -3,
    FOC_IMPEDANCE_FEEDFORWARD_ERR_NUMERIC = -4,
    FOC_IMPEDANCE_FEEDFORWARD_ERR_LIMIT = -5,
};

/** Mechanical model, signed trajectory sample, and explicit torque budget. */
struct foc_impedance_feedforward_input {
    /** Positive equivalent motor-shaft inertia, kg*m^2. */
    float inertia_kg_m2;
    /** Nonnegative motor-shaft viscous damping, N*m*s/rad. */
    float viscous_damping_nm_s_per_rad;
    /** Signed motor-shaft references in rad/s and rad/s^2, from one aperture. */
    float reference_velocity_rad_s;
    float reference_acceleration_rad_s2;
    /** Nonnegative symmetric budget for this model term, N*m. */
    float limit_nm;
    /** Nonzero identity of the qualified J/B model. */
    uint32_t model_id;
    /** Nonzero identifiers for the same signed motor-shaft frame. */
    uint32_t model_frame_id;
    uint32_t reference_frame_id;
    /** Caller confirms the reference is coherent, continuous in p/v and bounded. */
    bool reference_qualified;
};

struct foc_impedance_feedforward_result {
    /** Signed inertia-plus-viscous torque only, N*m. */
    float torque_nm;
    uint32_t model_id;
    uint32_t mechanical_frame_id;
    /** True when the qualified model term fits the supplied budget. */
    bool valid;
};

/* Public Functions ------------------------------------------------------------- */

/**
 * Calculate a model torque candidate with no controller or output effects.
 * The reference qualification excludes an impulsive speed step. The API uses
 * caller attestations; it cannot establish frame, timing or provenance alone.
 * Use distinct output storage. Downstream current/torque limits still apply.
 */
int foc_impedance_feedforward_evaluate(const struct foc_impedance_feedforward_input *input,
                                       struct foc_impedance_feedforward_result *result);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
