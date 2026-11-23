/**
 * @file    foc_protect.h
 * @brief   Motor-protection rules: over-current, bus voltage, over-speed, sustained
 *          current, stall and tracking divergence.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Design overview
 * ---------------
 * A pure monitor. Each call takes one control cycle's measurements and returns
 * the FOC_SAFE_FAULT_* protection bits that tripped in that cycle. It never
 * latches, inhibits or touches registers: the caller latches the returned bits
 * through its Safety owner. Thresholds are effective values; deriving them from
 * user parameters belongs to the caller.
 *
 * Usage example
 * -------------
 * Validate a configuration and reset the state at the start of a running
 * session. Call foc_protect_step() once per accepted running cycle with the
 * scope of the active mode. Reset again at every session or mode boundary.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns configuration and state and serializes calls on one state.
 * Functions are non-blocking and allocate nothing.
 *
 * Failure / recovery
 * ------------------
 * Invalid configuration or a NULL pointer reports no fault and changes nothing;
 * callers validate configuration before enabling it. Timed rules restart their
 * window whenever their condition is interrupted.
 */
#pragma once

#include "foc_safe.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Rules that apply to the active control mode. */
enum foc_protect_scope {
    /** Voltage drive (calibration): over-current and bus voltage only. */
    FOC_PROTECT_SCOPE_ELECTRICAL = 0,
    /** Torque (current) mode: adds over-speed and sustained current. */
    FOC_PROTECT_SCOPE_TORQUE = 1,
    /** Velocity and position modes: adds stall and tracking divergence. */
    FOC_PROTECT_SCOPE_MOTION = 2,
};

/** Effective thresholds. A zero window disables its timed rule. */
struct foc_protect_config {
    /** False disables every rule. */
    bool enabled;
    /** Largest absolute phase current in A before an immediate trip. */
    float overcurrent_a;
    /** Bus voltage window in V. */
    float overvoltage_v;
    float undervoltage_v;
    /** Consecutive out-of-window bus samples required to trip. */
    uint32_t voltage_samples;
    /** Absolute mechanical speed in rad/s before an immediate trip. */
    float overspeed_rad_s;
    /** Current magnitude in A held for sustained_us. */
    float sustained_current_a;
    uint32_t sustained_us;
    /** Current magnitude in A at a speed below stall_speed_rad_s, held for stall_us. */
    float stall_current_a;
    float stall_speed_rad_s;
    uint32_t stall_us;
    /** Saturation level of the q-current reference in A. */
    float divergence_current_a;
    /** Tracking window in µs. A window in which the outer loop is saturated for at least
     * half of its samples, and whose mean speed error is not at least 2% below the previous
     * such window's, trips. */
    uint32_t divergence_us;
};

/** Measurements of one accepted running cycle. */
struct foc_protect_input {
    /** Cycle time in monotonic microseconds. */
    uint32_t now_us;
    enum foc_protect_scope scope;
    /** Decoded phase currents in A and the bus voltage in V. */
    struct foc_vector_abc phase_current_a;
    float vbus_v;
    /** Measured d/q current-vector magnitude in A. */
    float current_magnitude_a;
    /** Mechanical speed and the outer-loop speed reference in rad/s. */
    float speed_rad_s;
    float speed_ref_rad_s;
    /** Outer-loop q-current reference in A. */
    float iq_ref_a;
    /** speed_rad_s is a fresh, valid observation. */
    bool speed_valid;
    /** speed_ref_rad_s and iq_ref_a belong to this cycle's accepted outer target. */
    bool reference_valid;
};

/** Timed-rule state for one running session. */
struct foc_protect_state {
    uint32_t voltage_streak;
    uint32_t sustained_since_us;
    uint32_t stall_since_us;
    /** Current tracking window: start, samples, saturated samples and error sum. */
    uint32_t window_since_us;
    uint32_t window_samples;
    uint32_t window_saturated;
    float window_error_sum;
    /** Mean speed error of the previous mostly saturated window, in rad/s. */
    float baseline_error_rad_s;
    bool sustained_active;
    bool stall_active;
    bool window_active;
    bool baseline_valid;
};

/* Public Functions ------------------------------------------------------------- */

/** True for a disabled configuration or one whose thresholds are finite and ordered. */
bool foc_protect_validate(const struct foc_protect_config *config);

/** Restart every timed rule. */
void foc_protect_reset(struct foc_protect_state *state);

/**
 * Evaluate one cycle.
 *
 * @param config Validated effective thresholds.
 * @param state Session state, updated by this call.
 * @param input Measurements of one accepted running cycle.
 * @return FOC_SAFE_FAULT_* protection bits that tripped in this cycle, or zero.
 */
uint32_t foc_protect_step(const struct foc_protect_config *config,
                          struct foc_protect_state *state,
                          const struct foc_protect_input *input);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
