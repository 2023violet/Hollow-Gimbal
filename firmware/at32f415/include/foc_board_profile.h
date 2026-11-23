/**
 * @file    foc_board_profile.h
 * @brief   Board1 unified refresh of the motor profile into a tuning candidate.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Design overview
 * ---------------
 * The motor profile travels inside the Runtime tuning (parameter contract
 * section 13). After a profile field changes, the refresh derives the current
 * PI from the model at the profile bandwidth, the velocity PI and position gain
 * from the inertia and Kt at the velocity bandwidth (contract section 18), and
 * tightens the limits the profile bounds. The caller publishes the complete candidate; H0 applies it at once.
 *
 * Usage example
 * -------------
 * Read the live tuning, change one profile field, call
 * foc_board_profile_refresh(), then publish the candidate. Calibration does the
 * same after writing the identified model.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Pure functions over caller-owned values. Foreground only.
 *
 * Failure / recovery
 * ------------------
 * A model the PI design refuses leaves the candidate's gains unchanged and
 * returns false; the caller drops the candidate.
 */
#pragma once

#include "foc_motor_id.h"
#include "foc_runtime.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

enum foc_board_profile_limits {
    /** Largest bandwidth the 200 µs H0 period admits (2π·f·T ≤ 0.3 gives 238 Hz), in Hz. */
    FOC_BOARD_PROFILE_BANDWIDTH_MAX_HZ = 235,
    /** Smallest current-loop bandwidth in Hz. */
    FOC_BOARD_PROFILE_BANDWIDTH_MIN_HZ = 10,
    /** Largest velocity-loop bandwidth: 1/20 of the 2 kHz H1 rate, in Hz. It is also at
     * most 1/5 of the current-loop bandwidth. */
    FOC_BOARD_PROFILE_VELOCITY_BANDWIDTH_MAX_HZ = 100,
};

/** Outer gains derived from the mechanical model. */
struct foc_board_velocity_gains {
    float velocity_kp_a_per_rad_s;
    float velocity_ki_a_per_rad;
    float position_kp_per_s;
};

/* Public Functions ------------------------------------------------------------- */

/** True when resistance and both inductances are set. */
bool foc_board_profile_model_complete(const struct foc_runtime_profile *profile);

/**
 * Design the current PI from the profile model at the profile bandwidth.
 *
 * @param period_us H0 control period in µs; the sample-to-application delay is bounded
 *        by one period.
 * @param voltage_limit_v Guaranteed voltage headroom of the current loop in V.
 * @param profile Complete model and bandwidth.
 * @param pi Destination for the designed gains.
 * @return True when the design accepts the model and bandwidth.
 */
bool foc_board_profile_design_pi(uint32_t period_us,
                                 float voltage_limit_v,
                                 const struct foc_runtime_profile *profile,
                                 struct foc_motor_pi_candidate *pi);

/** Kt = 1.5 x pole pairs x flux in N·m/A; 0 without a flux. */
float foc_board_profile_kt(uint32_t pole_pairs, const struct foc_runtime_profile *profile);

/**
 * Derive the outer gains: velocity Kp = J·ω / Kt, Ki = Kp·ω / 4, position Kp = ω / 4,
 * with ω = 2π × the velocity bandwidth.
 * @return True when inertia, Kt and the bandwidth are set and the bandwidth is within
 *         FOC_BOARD_PROFILE_VELOCITY_BANDWIDTH_MAX_HZ and 1/5 of the current bandwidth.
 */
bool foc_board_profile_design_velocity(uint32_t pole_pairs,
                                       const struct foc_runtime_profile *profile,
                                       struct foc_board_velocity_gains *gains);

/**
 * Refresh a candidate after a profile change.
 *
 * @param period_us H0 control period in µs.
 * @param voltage_limit_v Current-loop voltage headroom in V.
 * @param pole_pairs Active pole pairs, for Kt.
 * @param tuning Candidate whose profile changed, updated in place.
 * @param redesign_pi Re-derive the current PI when the model is complete.
 * @param redesign_velocity Re-derive the outer gains when inertia and Kt are set.
 * @param pi Optional destination for the designed gains.
 * @return False when a design refuses the profile; the gains stay unchanged.
 */
bool foc_board_profile_refresh(uint32_t period_us,
                               float voltage_limit_v,
                               uint32_t pole_pairs,
                               struct foc_runtime_tuning *tuning,
                               bool redesign_pi,
                               bool redesign_velocity,
                               struct foc_motor_pi_candidate *pi);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
