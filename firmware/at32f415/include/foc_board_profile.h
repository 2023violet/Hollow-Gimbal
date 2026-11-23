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
 * PI from the model at the profile bandwidth and tightens the limits the profile
 * bounds. The caller publishes the complete candidate; H0 applies it at once.
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

/**
 * Refresh a candidate after a profile change.
 *
 * @param period_us H0 control period in µs.
 * @param voltage_limit_v Current-loop voltage headroom in V.
 * @param tuning Candidate whose profile changed, updated in place.
 * @param redesign_pi Re-derive the current PI when the model is complete.
 * @param pi Optional destination for the designed gains.
 * @return False when the PI design refuses the model; the gains stay unchanged.
 */
bool foc_board_profile_refresh(uint32_t period_us,
                               float voltage_limit_v,
                               struct foc_runtime_tuning *tuning,
                               bool redesign_pi,
                               struct foc_motor_pi_candidate *pi);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
