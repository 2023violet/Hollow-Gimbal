/**
 * @file    foc_damping_noise_filter.h
 * @brief   Host candidate for filtering only impedance damping feedback.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Use a bounded backward-Euler filter for the Host damping branch.
 *
 * Usage example
 * -------------
 * Zero-initialize a filter, then call step with a qualified Sensor speed and
 * the elapsed source interval. Use the result for damping feedback only.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The Host caller resets at a Session boundary and steps a copy until H0
 * confirms the corresponding target.
 *
 * Failure / recovery
 * ------------------
 * An invalid speed, cutoff or duration preserves the candidate state.
 */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

/* Public Types ---------------------------------------------------------------- */

struct damping_noise_filter {
    float velocity_rad_s;
    bool ready;
};

/* Public Functions ------------------------------------------------------------- */

static inline bool damping_noise_filter_step(struct damping_noise_filter *state,
                                             float velocity_rad_s,
                                             float cutoff_hz,
                                             uint32_t dt_us)
{
    if (!isfinite(velocity_rad_s) || fabsf(velocity_rad_s) > 1000.0f || !isfinite(cutoff_hz) ||
        cutoff_hz < 1.0f || cutoff_hz > 200.0f || dt_us == 0U || dt_us > 2000U) {
        return false;
    }
    if (!state->ready) {
        *state = (struct damping_noise_filter){velocity_rad_s, true};
        return true;
    }
    const float omega_dt = (6.283185307179586f * 1.0e-6f) * cutoff_hz * (float)dt_us;
    state->velocity_rad_s +=
        omega_dt / (1.0f + omega_dt) * (velocity_rad_s - state->velocity_rad_s);
    return true;
}

/* ---------------------------------------- end of file ----------------------------------------- */
