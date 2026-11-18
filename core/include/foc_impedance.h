/**
 * @file    foc_impedance.h
 * @brief   Stateless single-axis mechanical impedance request in SI units.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Convert an elastic, damping, and feedforward torque request to q current
 * using the caller's positive motor-shaft torque constant and live limit.
 *
 * Usage example
 * -------------
 * Form a continuous signed position error in a common mechanical coordinate
 * system before converting it to Float32. Provide coherent velocities and
 * evaluate the result before passing the target to a downstream Current loop.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * All values are caller-owned. The calculation has no state or output authority.
 *
 * Failure / recovery
 * ------------------
 * Errors clear a supplied output. The caller owns sample admission and recovery.
 */
#pragma once

#include "foc_core.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Motor-shaft controller settings. */
struct foc_impedance_config {
    /** Nonnegative stiffness, N*m/rad. */
    float stiffness_nm_per_rad;
    /** Nonnegative viscous damping, N*m*s/rad. */
    float damping_nm_s_per_rad;
    /** Positive torque constant, N*m/A, with positive q current producing positive torque. */
    float torque_constant_nm_per_a;
    /** Nonnegative symmetric torque request limit, N*m. */
    float torque_limit_nm;
};

/** Coherent mechanical inputs for one evaluation. */
struct foc_impedance_input {
    /** Reference minus measured continuous mechanical position, rad. Never wrapped. */
    float position_error_rad;
    /** Reference mechanical speed, rad/s. */
    float reference_velocity_rad_s;
    /** Measured mechanical speed, rad/s. */
    float measured_velocity_rad_s;
    /** Signed feedforward torque, N*m. */
    float torque_feedforward_nm;
    /** Nonnegative symmetric live q current limit, A. Zero permits only zero torque. */
    float live_iq_limit_a;
};

/** Requested motor-shaft torque and q current. */
struct foc_impedance_output {
    /** Spring plus damping plus feedforward, N*m, before limiting. */
    float requested_torque_nm;
    /** Torque request after limits and before q conversion, N*m. */
    float limited_torque_nm;
    /** q current target after conversion and enforcement of the live limit, A. */
    float iq_ref_a;
    /** True if either request was reduced by a limit. */
    bool limited;
    /** True for a complete numerical result, including a zero torque request. */
    bool valid;
};

/* Public Functions ------------------------------------------------------------- */

/**
 * Calculate one Float32 impedance request.
 * The caller owns angle error precision, Kt provenance, and Current/Safety admission.
 * Rounding and the final current cap can make Kt * iq_ref_a differ slightly
 * from the pre-conversion torque request.
 * @return FOC_STATUS_OK, FOC_STATUS_ERR_NULL, FOC_STATUS_ERR_INVALID_CONFIG for
 *         invalid gains, Kt or limits, or FOC_STATUS_ERR_NONFINITE for nonfinite
 *         inputs or arithmetic. Output storage is distinct from config and input.
 */
int foc_impedance_step(const struct foc_impedance_config *config,
                       const struct foc_impedance_input *input,
                       struct foc_impedance_output *output);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
