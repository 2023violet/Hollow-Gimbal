/**
 * @file    pll_enc.h
 * @brief   Phase-Locked Loop (PLL) Position & Velocity Estimator
 * @version v3.2.0
 * @date    2026.02.22
 *
 * Algorithm Overview
 * ──────────────────
 * A second-order PLL observer estimates continuous position and velocity from
 * a discrete, noisy encoder count:
 *
 *   Measurement ──► Phase Detector (error = meas − estimate)
 *                        │
 *             ┌──── Kp ──┴── Ki ────┐
 *             │  (position corr.)  (velocity corr.)
 *             ▼                     ▼
 *       pos_estimate           vel_estimate
 *             │                     │
 *             └───── Integrator ────┘  (pos += vel × dt)
 *
 * Key Design Choices
 * ──────────────────
 * 1. Mixed-precision accumulator: velocity in counts/s uses float32;
 *    total position uses int64 (integer counts) + float32 (fractional),
 *    avoiding the ~8-million-count precision ceiling of pure float32.
 *
 * 2. Gains fully precomputed in pll_enc_init():
 *      kp = 2 × pll_bandwidth    (position proportional gain)
 *      ki = 0.25 × kp²           (velocity integral gain)
 *    Note on pll_bandwidth units: the value is an empirically-tuned
 *    parameter (≈ ωn/2 in rad/s). The factor of 2π is deliberately
 *    omitted — the system has been tuned to work with this simplified
 *    formula, and changing it would require re-tuning all parameters.
 *
 * 3. Fixed dt: sampling period is precomputed as dt = 1/sample_rate_hz
 *    in pll_enc_init() and stored in state. pll_enc_update() no longer
 *    accepts a dt argument, eliminating any risk of time-base mismatch
 *    between the PLL gains and the velocity LPF.
 *
 * 4. Zero-velocity snap: |vel_estimate| < snap_thr → clamped to zero.
 *    snap_thr = snap_factor × dt × ki (precomputed, units: counts/s).
 *
 * 5. Interpolation: sub-count position is propagated by integrating the
 *    LPF-smoothed velocity between integer encoder steps.
 *
 * Revision History
 * ──────────────────
 * v3.2.0 - Opt #1: Replace while-loop carry with floorf() — O(1), fixes
 *          WCET blow-up at high RPM where pos_change >> 1.0 count/step.
 *        - Opt #2: Remove dead-code pos_cpr_counts field and its fmodf
 *          update; no getter function ever read it.
 *        - Opt #3: Precompute kp, ki, snap_thr, dt in init(); remove
 *          their repeated recalculation from the 20 kHz hot path.
 *        - Opt #4: Replace CPR bitmask (& cpr-1) with generic if-else
 *          wrap; bitmask silently breaks for non-power-of-2 CPR values.
 *        - Opt #5: Remove dt parameter from pll_enc_update(); use the
 *          precomputed state.dt to guarantee time-base consistency.
 *
 * v3.1.0 - Fix: pll_enc_get_angle_elec_rad now accepts mech_offset.
 *          Omitting mech_offset caused Park frame misalignment after
 *          encoder calibration, preventing the motor from spinning.
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>
#include <stdbool.h>
#include "foc_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief PLL Configuration Structure (ROM)
 *
 * All fields must be set before calling pll_enc_init().
 * They are treated as read-only after initialization.
 */
struct pll_enc_config {
    float cpr;               /* Counts per Revolution (e.g. 65536.0f for KTH7823)      */
    float pll_bandwidth;     /* PLL tuning parameter (empirical, ≈ ωn/2).
                              * Typical range: 100 – 500. Higher = faster response,
                              * lower = more noise rejection.                            */
    float vel_lpf_bandwidth; /* Velocity output LPF cutoff frequency (Hz).
                              * Typical range: 1 – 20 Hz.                               */
    float sample_rate_hz;    /* Update rate (Hz). MUST be Hz, e.g. 20000.0f.
                              * Used to precompute dt = 1/sample_rate_hz in init().     */
    float snap_factor;       /* Zero-velocity snap factor [0.0, 1.0].
                              * snap_thr = snap_factor × dt × ki (counts/s).            */
};

/**
 * @brief PLL Internal State (RAM)
 *
 * All fields written exclusively by pll_enc_init() or pll_enc_update().
 * External code must treat this as opaque; use getter functions.
 */
struct pll_enc_state {
    /* ── Measurement ─────────────────────────────────────────────────────── */
    int32_t count_in_cpr; /* Latest corrected count in one revolution [0, CPR-1]       */
    int64_t shadow_count; /* Accumulated total count — int64 prevents overflow for
                           * > 100 years of continuous operation at max speed            */

    /* ── Mixed-Precision Position Accumulator ─────────────────────────────
     * Why: float32 loses sub-count precision after ~256 revolutions.
     * We split position into integer counts (int64) + fractional counts
     * (float32 kept in [0.0, 1.0)), so relative precision is always maximum
     * regardless of total distance traveled.                                 */
    int64_t pos_estimate_int;  /* Estimated position — integer part (counts)            */
    float   pos_estimate_frac; /* Estimated position — fractional part [0.0, 1.0)      */

    /* ── Velocity & Filter ───────────────────────────────────────────────── */
    float      vel_estimate_counts; /* PLL-estimated velocity (counts/s, unfiltered)    */
    struct lpf vel_lpf;             /* Velocity Low-Pass Filter instance                */

    /* ── Interpolation Output ─────────────────────────────────────────────── */
    float interpolation;    /* Sub-count interpolation phase [0.0, 1.0]                */
    float interpolated_enc; /* Final high-resolution single-turn position (counts)     */

    /* ── Precomputed Constants (written once in init, never changed) ──────── */
    float dt;       /* Sampling period (s) = 1.0f / sample_rate_hz                    */
    float kp;       /* PLL proportional gain = 2.0f × pll_bandwidth                   */
    float ki;       /* PLL integral gain     = 0.25f × kp²                            */
    float snap_thr; /* Zero-velocity threshold (counts/s) = snap_factor × dt × ki     */
};

/**
 * @brief PLL Object Instance
 */
struct pll_enc_dev {
    struct pll_enc_config *config; /* Pointer to ROM configuration (must outlive dev)  */
    struct pll_enc_state   state;  /* Internal mutable state                           */
};

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the PLL estimator and precompute all derived constants.
 *
 * Must be called once before pll_enc_update().
 * After this call, config must NOT be modified (precomputed constants would
 * become stale).
 *
 * @param  pll   PLL instance to initialize
 * @param  cfg   Configuration — must remain valid for the lifetime of pll
 */
void pll_enc_init(struct pll_enc_dev *pll, struct pll_enc_config *cfg);

/**
 * @brief  Update PLL state with a new encoder measurement.
 *
 * Call at exactly sample_rate_hz (e.g. every 50 µs at 20 kHz) from the
 * ADC ISR.  The time step dt is internally fixed to 1/sample_rate_hz;
 * it is no longer accepted as a parameter to prevent time-base mismatch.
 *
 * [ISR-SAFE] — no RTOS primitives, no dynamic allocation, no printf.
 *
 * @param  pll  PLL instance
 * @param  cnt  Direction-corrected, LUT-compensated position count [0, CPR-1]
 *
 * @return  0 on success, -1 on invalid arguments
 */
int pll_enc_update(struct pll_enc_dev *pll, int32_t cnt);

/**
 * @brief  Get LPF-smoothed velocity in rad/s.
 *
 * [ISR-SAFE]
 *
 * @param  pll  PLL instance
 * @return Velocity (rad/s), positive = forward, negative = reverse
 */
float pll_enc_get_velocity_rads(struct pll_enc_dev *pll);

/**
 * @brief  Get interpolated mechanical angle in rad, offset-corrected.
 *
 * [ISR-SAFE]
 *
 * @param  pll         PLL instance
 * @param  mech_offset Mechanical zero offset (rad) from encoder calibration
 * @return Mechanical angle [0, 2π)
 */
float pll_enc_get_angle_mech_rad(struct pll_enc_dev *pll, float mech_offset);

/**
 * @brief  Get electrical angle in rad for FOC current loop.
 *
 * The formula applied is:
 *   theta_elec = (theta_mech_raw − mech_offset) × pole_pairs   (mod 2π)
 *
 * mech_offset MUST be applied here, not just in the mechanical angle getter.
 * Omitting it leaves the Park transform frame misaligned with the rotor flux
 * axis after calibration, which prevents the motor from producing torque.
 *
 * [ISR-SAFE]
 *
 * @param  pll         PLL instance
 * @param  mech_offset Mechanical zero offset (rad) from encoder calibration
 * @param  pole_pairs  Motor pole pairs count
 * @return Electrical angle [0, 2π)
 */
float pll_enc_get_angle_elec_rad(struct pll_enc_dev *pll, float mech_offset, float pole_pairs);

/**
 * @brief  Get total multi-turn position in turns.
 *
 * Based on the int64 shadow counter, so it never saturates.
 *
 * [ISR-SAFE]
 *
 * @param  pll  PLL instance
 * @return Signed total shaft position (turns)
 */
float pll_enc_get_total_enc(struct pll_enc_dev *pll);

/**
 * @brief  Dynamically update PLL bandwidth at runtime.
 *
 * This function recalculates kp, ki, and snap_thr based on the new bandwidth.
 * Can be called from ISR or task context.
 *
 * [ISR-SAFE]
 *
 * @param  pll           PLL instance
 * @param  pll_bandwidth New PLL bandwidth (Hz)
 */
void pll_enc_set_bandwidth(struct pll_enc_dev *pll, float pll_bandwidth);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */