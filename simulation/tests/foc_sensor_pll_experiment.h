/**
 * @file    foc_sensor_pll_experiment.h
 * @brief   Host research-only mechanical speed PLL.
 * @version v0.1.0
 * @date    2026.11.27
 *
 * Design overview
 * ---------------
 * Track local phase relative to the last accepted Sensor observation.
 *
 * Usage example
 * -------------
 * Initialize on a Host research lane, then pass the complete feedback from
 * foc_encoder_update(). Publish speed only when the experiment accepts it.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns the instance and resets it with the Encoder after loss of
 * continuity. Do not consume velocity from a rejected observation.
 *
 * Failure / recovery
 * ------------------
 * Rejected samples leave the accepted endpoint and dynamic state untouched.
 * Reinitialize before reseeding if a Sensor-valid endpoint was skipped.
 */
#pragma once

#include "foc_encoder.h"

/* Public Types ----------------------------------------------------------------- */

struct foc_sensor_pll_experiment {
    float omega_rad_s;
    float max_speed_rad_s;
    /** Estimate minus observation at the latest accepted endpoint, in rad. */
    float phase_error_rad;
    float velocity_rad_s;
    uint32_t captured_us;
    bool seeded;
};

/* Public Functions ------------------------------------------------------------- */

/** Start/reset a research candidate. Invalid policy preserves the instance. */
bool foc_sensor_pll_experiment_init(struct foc_sensor_pll_experiment *pll,
                                    float omega_rad_s,
                                    float max_speed_rad_s);

/**
 * Accept a valid, continuous Encoder velocity observation atomically.
 * If the Sensor advances but this step fails, reinitialize before reseeding.
 * The next partial increment cannot advance a stale relative endpoint.
 */
bool foc_sensor_pll_experiment_step(struct foc_sensor_pll_experiment *pll,
                                    const struct foc_encoder_feedback *sample);

/* ---------------------------------------- end of file ----------------------------------------- */
