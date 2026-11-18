/**
 * @file    foc_impedance_tuning.h
 * @brief   Cold-path motor-axis impedance gain candidate design.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Match a linear unsaturated mechanical model to a natural angular frequency
 * and damping ratio. Screen timing and static configuration separately.
 *
 * Usage example
 * -------------
 * Supply qualified motor-axis parameters and call foc_impedance_tuning_design.
 * Validate the returned gains on the intended closed loop before applying them.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Stateless. Timing bounds and model provenance are supplied by the caller.
 *
 * Failure / recovery
 * ------------------
 * Failure clears the complete candidate. The result grants no output authority.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

enum foc_impedance_tuning_damping_source {
    FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED = 0,
    FOC_IMPEDANCE_TUNING_DAMPING_ASSUMED_ZERO,
};

enum foc_impedance_tuning_status {
    FOC_IMPEDANCE_TUNING_OK = 0,
    FOC_IMPEDANCE_TUNING_ERR_NULL = -1,
    FOC_IMPEDANCE_TUNING_ERR_CONFIG = -2,
    FOC_IMPEDANCE_TUNING_ERR_NUMERIC = -3,
    FOC_IMPEDANCE_TUNING_ERR_TIMING = -4,
    FOC_IMPEDANCE_TUNING_ERR_NEGATIVE_DAMPING = -5,
    FOC_IMPEDANCE_TUNING_ERR_GAIN_LIMIT = -6,
};

/** Qualified motor-shaft model, desired dynamics, and static admission bounds. */
struct foc_impedance_tuning_input {
    /** Nonzero source/model identity; the caller maintains its provenance. */
    uint32_t model_id;
    enum foc_impedance_tuning_damping_source damping_source;
    /** Positive equivalent inertia, kg*m^2. */
    float inertia_kg_m2;
    /** Nonnegative viscous damping, N*m*s/rad. Supply zero for assumed-zero models. */
    float viscous_damping_nm_s_per_rad;
    /** Positive target natural angular frequency, rad/s. */
    float target_natural_frequency_rad_s;
    /** Positive dimensionless ideal damping ratio. */
    float target_damping_ratio;
    /** Positive qualified motor-shaft torque constant, N*m/A. */
    float torque_constant_nm_per_a;
    /** Positive configured Runtime stiffness ceiling, N*m/rad. */
    float max_stiffness_nm_per_rad;
    /** Nonnegative configured Runtime damping ceiling, N*m*s/rad. */
    float max_damping_nm_s_per_rad;
    /** Positive available q current, A. */
    float iq_limit_a;
    /** Positive outer-loop period in seconds. */
    float sample_period_s;
    /** Nonnegative additional delay budget in seconds, supplied end-to-end. */
    float extra_delay_s;
};

/** Successful model candidate, prior to closed-loop or hardware admission. */
struct foc_impedance_tuning_candidate {
    uint32_t model_id;
    enum foc_impedance_tuning_damping_source damping_source;
    /** Runtime stiffness and damping to review, in N*m/rad and N*m*s/rad. */
    float stiffness_nm_per_rad;
    float damping_nm_s_per_rad;
    /** Predicted ideal dynamics using the returned Float32 gains. */
    float ideal_natural_frequency_rad_s;
    float ideal_damping_ratio;
    /** Kt times available q current, N*m, before any independent torque cap. */
    float current_limited_torque_nm;
    /** True when the ideal design is valid and fits the supplied gain ceilings. */
    bool valid;
};

/* Public Functions ------------------------------------------------------------- */

/**
 * Match poles of J*s^2 + (B+D)*s + K. For requested and predicted omega_n,
 * screen omega_n*(period + delay) <= 0.25.
 * Negative ideal D and excess gains are rejected. The timing screen is a
 * conservative design budget. It does not certify sampled stability.
 * The current-derived torque budget does not guarantee unsaturated operation.
 * @return A status with the rejection reason. A supplied candidate is zero on error.
 */
int foc_impedance_tuning_design(const struct foc_impedance_tuning_input *input,
                                struct foc_impedance_tuning_candidate *candidate);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
