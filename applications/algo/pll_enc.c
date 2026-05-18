/**
 * @file    pll_enc.c
 * @brief   Phase Locked Loop (PLL) Estimator for Position and Velocity
 * @version v3.2.0
 * @date    2026.02.22
 *
 * Revision History
 * ──────────────────
 * v3.2.0 - Opt #1 [WCET Fix]: Replace while-loop carry on pos_estimate_frac
 *          with floorf() call.  At 6000 RPM (65536 CPR, 20 kHz), pos_change
 *          is ~328 counts/step; the old while-loop executed 328 iterations in
 *          a single ISR, blowing WCET and threatening 20 kHz timing integrity.
 *          floorf() is O(1) — compiles to a single FRINTM/VCVT instruction
 *          on Cortex-M4 with FPU.
 *
 *        - Opt #2 [Dead Code]: Removed pos_cpr_counts field and its fmodf
 *          update.  No getter function ever read pos_cpr_counts.  The fmodf
 *          call (software divide on M4) was consuming ~50-100 cycles for
 *          nothing.
 *
 *        - Opt #3 [Hot Path]: kp, ki, snap_thr, dt are now precomputed in
 *          pll_enc_init() and stored in state.  The 20 kHz update path no
 *          longer recalculates 5 floating-point multiplications on every ISR.
 *
 *        - Opt #4 [Robustness]: Replaced CPR bitmask (& cpr-1) with a
 *          generic if-else wrap.  The bitmask only works for power-of-2 CPR
 *          (like KTH7823's 65536); it silently corrupts data for any other
 *          CPR value (e.g. 4000-line incremental encoders → CPR=16000).
 *
 *        - Opt #5 [API]: Removed dt parameter from pll_enc_update().  dt is
 *          now always state.dt (= 1/sample_rate_hz).  This guarantees the PLL
 *          gains and velocity LPF always share the same time base.
 *
 * v2.1.0 - Fix: pll_enc_get_angle_elec_rad now accepts mech_offset and
 *          applies it before multiplying by pole_pairs.  Without this fix,
 *          the calibrated encoder zero-offset was silently discarded, causing
 *          Park frame misalignment and preventing the motor from spinning
 *          after encoder calibration.
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "pll_enc.h"
#include <string.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the PLL estimator and precompute derived constants.
 */
void pll_enc_init(struct pll_enc_dev *pll, struct pll_enc_config *cfg)
{
    if (!pll || !cfg || cfg->sample_rate_hz <= 0.0f) {
        return;
    }

    pll->config = cfg;

    /* Clear all runtime state */
    memset(&pll->state, 0, sizeof(struct pll_enc_state));

    /* ── Precompute time step ──────────────────────────────────────────────
     * Why: dt is always 1/sample_rate_hz.  Storing it here means update()
     * never needs it as a parameter, preventing any call-site mismatch. */
    pll->state.dt = 1.0f / cfg->sample_rate_hz;

    /* ── Precompute PLL gains ─────────────────────────────────────────────
     * Formula: kp = 2 × pll_bandwidth,  ki = 0.25 × kp²
     * Note: pll_bandwidth here is an empirically-tuned parameter (≈ ωn/2),
     * not a strict Hz bandwidth.  The 2π factor is intentionally absent —
     * existing tuned parameters assume this simplified formula.             */
    pll->state.kp = 2.0f * cfg->pll_bandwidth;
    pll->state.ki = 0.25f * FOC_SQ(pll->state.kp);

    /* ── Precompute zero-velocity snap threshold ──────────────────────────
     * snap_thr = snap_factor × dt × ki  (units: counts/s)
     * Interpretation: if |vel_estimate| is smaller than the velocity change
     * ki can produce in one step scaled by snap_factor, snap to zero.       */
    pll->state.snap_thr = cfg->snap_factor * pll->state.dt * pll->state.ki;

    /* ── Initialize velocity Low-Pass Filter ─────────────────────────────
     * lpf_init() expects bandwidth (Hz) and sample_rate (Hz).               */
    lpf_init(&pll->state.vel_lpf, cfg->vel_lpf_bandwidth, cfg->sample_rate_hz);
}

/**
 * @brief  Update PLL state with a new encoder measurement.
 *
 * Hot path — called at 20 kHz from the ADC ISR.  Every operation here
 * has been audited for WCET.  No loops, no fmodf, no repeated constant
 * computation.
 */
int pll_enc_update(struct pll_enc_dev *pll, int32_t cnt)
{
    if (!pll || !pll->config) {
        return -1;
    }

    /* Load precomputed constants into local variables for clarity.
     * The compiler will likely keep these in FPU registers anyway. */
    const float dt       = pll->state.dt;
    const float pll_kp   = pll->state.kp;
    const float pll_ki   = pll->state.ki;
    const float snap_thr = pll->state.snap_thr;

    /* ── Step 1: Measurement — compute encoder delta ──────────────────────
     *
     * Opt #4: Use generic if-else wrap instead of bitmask.
     * The old code used (delta & (cpr-1)) which only works for power-of-2
     * CPR.  The if-else handles any CPR value correctly.
     *
     * Edge case: if the encoder jumped more than CPR/2 in one step, something
     * has gone wrong (SPI glitch, mechanical fault).  The clamp to ±CPR/2
     * naturally handles this by choosing the shorter arc — same behavior the
     * bitmask produced, but now correct for all CPR values.                  */
    const int32_t cpr_i = (int32_t)pll->config->cpr;

    int32_t delta_enc = cnt - pll->state.count_in_cpr;

    if (delta_enc > (cpr_i / 2)) {
        delta_enc -= cpr_i;
    } else if (delta_enc < -(cpr_i / 2)) {
        delta_enc += cpr_i;
    }

    /* Update shadow count (int64 — never overflows) */
    pll->state.shadow_count += delta_enc;
    pll->state.count_in_cpr = cnt;

    /* ── Step 2: Prediction — advance position estimate ───────────────────
     *
     * Prediction: pos_estimate += vel_estimate × dt
     *
     * Mixed-precision carry:
     *   Opt #1: Replace while-loop with floorf() — O(1) regardless of speed.
     *   floorf() maps to FRINTM (single instruction) on Cortex-M4 with FPU.
     *
     *   Old while-loop iterated up to N times where N = floor(pos_change).
     *   At 6000 RPM (65536 CPR, 20 kHz): N ≈ 328 — a WCET disaster.         */
    float pos_change = dt * pll->state.vel_estimate_counts;

    pll->state.pos_estimate_frac += pos_change;

    /* Carry fractional overflow into integer part — O(1) with floorf */
    float whole = floorf(pll->state.pos_estimate_frac);
    if (whole != 0.0f) {
        pll->state.pos_estimate_int += (int64_t)whole;
        pll->state.pos_estimate_frac -= whole;
        /* pos_estimate_frac is now guaranteed to be in [0.0, 1.0) */
    }

    /* ── Step 3: Phase Detector — compute position error ─────────────────
     *
     * Error = (shadow_count − pos_estimate_int) − pos_estimate_frac
     * Numerically safe: the PLL tracks closely, so the difference is tiny. */
    float delta_pos_counts = (float)(pll->state.shadow_count - pll->state.pos_estimate_int) -
                             pll->state.pos_estimate_frac;

    /* ── Step 4: Correction — apply PLL feedback ─────────────────────────
     *
     * Proportional: nudge position estimate toward measurement             */
    pll->state.pos_estimate_frac += dt * pll_kp * delta_pos_counts;

    /* Integral: nudge velocity estimate toward true velocity               */
    pll->state.vel_estimate_counts += dt * pll_ki * delta_pos_counts;

    /* ── Step 5: Zero-velocity snap ──────────────────────────────────────
     *
     * If the PLL velocity estimate oscillates around zero (stopped motor),
     * hard-clamp it to zero to prevent buzzing / false slow-drift output.   */
    bool snap_active = false;

    if (FOC_ABS(pll->state.vel_estimate_counts) < snap_thr) {
        pll->state.vel_estimate_counts = 0.0f;
        snap_active                    = true;
    }

    /* ── Step 6: Velocity Low-Pass Filter ────────────────────────────────
     *
     * Feed (possibly snapped) velocity into LPF for smooth velocity output.
     * When snap clamps vel to 0, the LPF output decays toward 0 naturally.  */
    float vel_filtered = lpf_update(&pll->state.vel_lpf, pll->state.vel_estimate_counts);

    /* ── Step 7: Sub-count interpolation output ──────────────────────────
     *
     * Purpose: provide a high-resolution single-turn position that is
     * smooth between integer encoder steps.
     *
     * Logic:
     *   - Snap active (stopped): park interpolation at 0.5 to suppress jitter
     *   - New integer step: reset phase to edge of step (0.0 or 1.0)
     *   - Between steps: advance phase using LPF velocity              */
    if (snap_active) {
        pll->state.interpolation = 0.5f;
    } else {
        if (delta_enc != 0) {
            /* Integer step occurred — reset to step boundary */
            pll->state.interpolation = (delta_enc > 0) ? 0.0f : 1.0f;
        } else {
            /* Between integer steps: advance using filtered velocity */
            float new_interp         = pll->state.interpolation + dt * vel_filtered;
            pll->state.interpolation = FOC_CLAMP(new_interp, 0.0f, 1.0f);
        }
    }

    /* Final high-resolution single-turn position (integer + sub-count) */
    pll->state.interpolated_enc = (float)pll->state.count_in_cpr + pll->state.interpolation;

    return 0;
}

/**
 * @brief  Get LPF-smoothed velocity in rad/s.
 */
float pll_enc_get_velocity_rads(struct pll_enc_dev *pll)
{
    /* Use LPF output to suppress encoder quantisation noise in the output.
     * vel_lpf.output is in counts/s; convert to rad/s via CPR. */
    float frac = pll->state.vel_lpf.output / pll->config->cpr;
    return (frac * FOC_2PI);
}

/**
 * @brief  Get interpolated mechanical angle in rad, with mech_offset applied.
 */
float pll_enc_get_angle_mech_rad(struct pll_enc_dev *pll, float mech_offset)
{
    float mech_frac      = pll->state.interpolated_enc / pll->config->cpr;
    float angle_mech_rad = (mech_frac * FOC_2PI) - mech_offset;
    // return foc_fmod_pos(angle_mech_rad, FOC_2PI);
    return foc_wrap_0_2pi_near(angle_mech_rad);
}

/**
 * @brief  Get electrical angle in rad for FOC Park transform.
 *
 * Correct formula: theta_elec = (theta_mech_raw − mech_offset) × pole_pairs
 *
 * mech_offset must be applied inside pll_enc_get_angle_mech_rad() before
 * scaling by pole_pairs, not after.  Applying it after would shift the
 * electrical angle by (mech_offset × pole_pairs) instead of (mech_offset × 1),
 * which is wrong except for single-pole-pair motors.
 */
float pll_enc_get_angle_elec_rad(struct pll_enc_dev *pll, float mech_offset, float pole_pairs)
{
    float elec_angle = pll_enc_get_angle_mech_rad(pll, mech_offset) * pole_pairs;
    return foc_fmod_pos(elec_angle, FOC_2PI);
}

/**
 * @brief  Get total multi-turn position in turns.
 *
 * Uses the int64 pos_estimate_int (in counts).  Never saturates.
 */
float pll_enc_get_total_enc(struct pll_enc_dev *pll)
{
    return (float)pll->state.pos_estimate_int / pll->config->cpr;
}

/**
 * @brief  Dynamically update PLL bandwidth at runtime.
 */
void pll_enc_set_bandwidth(struct pll_enc_dev *pll, float pll_bandwidth)
{
    if (!pll || !pll->config) {
        return;
    }

    /* Update config value */
    pll->config->pll_bandwidth = pll_bandwidth;

    /* Recalculate PLL gains based on new bandwidth */
    pll->state.kp = 2.0f * pll_bandwidth;
    pll->state.ki = 0.25f * pll->state.kp * pll->state.kp;

    /* Recalculate snap threshold */
    pll->state.snap_thr = pll->config->snap_factor * pll->state.dt * pll->state.ki;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
