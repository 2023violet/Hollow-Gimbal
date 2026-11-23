/**
 * @file    foc_board_profile.c
 * @brief   Board1 unified refresh of the motor profile into a tuning candidate.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Implementation role
 * -------------------
 * Derive the current PI with foc_motor_pi_design() at the caller's H0 period,
 * and tighten the current and speed limits to the profile. Never publishes.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_profile.h"

#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

/* None: the refresh is two pure steps over caller-owned values. */

/* Public Functions ------------------------------------------------------------- */

bool foc_board_profile_model_complete(const struct foc_runtime_profile *profile)
{
    return profile != NULL && profile->rs_ohm > 0.0f && profile->ld_h > 0.0f &&
        profile->lq_h > 0.0f;
}

bool foc_board_profile_design_pi(
    uint32_t period_us,
    float voltage_limit_v,
    const struct foc_runtime_profile *profile,
    struct foc_motor_pi_candidate *pi
)
{
    if (!foc_board_profile_model_complete(profile) || pi == NULL || period_us == 0U) {
        return false;
    }
    /* One H0 period per sample; the update reaches the bridge within one period. */
    const struct foc_motor_rl_model model = {
        .rs_ohm = profile->rs_ohm,
        .ld_h = profile->ld_h,
        .lq_h = profile->lq_h,
    };
    const struct foc_motor_pi_request request = {
        .bandwidth_hz = profile->current_bandwidth_hz,
        .sample_period_s = (float)period_us * 1.0e-6f,
        .update_delay_s = (float)period_us * 1.0e-6f,
        .initial_error_a = 0.5f,
        .available_voltage_v = voltage_limit_v,
    };
    return foc_motor_pi_design(&model, &request, pi) == FOC_MOTOR_ID_OK;
}

bool foc_board_profile_refresh(
    uint32_t period_us,
    float voltage_limit_v,
    struct foc_runtime_tuning *tuning,
    bool redesign_pi,
    struct foc_motor_pi_candidate *pi
)
{
    if (tuning == NULL) {
        return false;
    }
    const struct foc_runtime_profile *profile = &tuning->profile;
    if (redesign_pi && foc_board_profile_model_complete(profile)) {
        struct foc_motor_pi_candidate designed;
        if (!foc_board_profile_design_pi(period_us, voltage_limit_v, profile, &designed)) {
            return false;
        }
        tuning->current_kp_v_per_a = (struct foc_vector_dq){
            designed.kp_d_v_per_a,
            designed.kp_q_v_per_a,
        };
        tuning->current_ki_v_per_a_s = (struct foc_vector_dq){
            designed.ki_v_per_a_s,
            designed.ki_v_per_a_s,
        };
        if (pi != NULL) {
            *pi = designed;
        }
    }
    if (profile->peak_current_a > 0.0f) {
        tuning->current_limit_a = fminf(tuning->current_limit_a, profile->peak_current_a);
        tuning->iq_limit_a = fminf(tuning->iq_limit_a, tuning->current_limit_a);
    }
    if (profile->max_speed_rad_s > 0.0f) {
        tuning->velocity_limit_rad_s =
            fminf(tuning->velocity_limit_rad_s, profile->max_speed_rad_s);
    }
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
