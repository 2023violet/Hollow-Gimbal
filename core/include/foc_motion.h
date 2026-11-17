/**
 * @file    foc_motion.h
 * @brief   Rotor coordinates, bounded angle prediction and portable motion loops.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Mechanical position is continuous in rad relative to a caller-defined zero.
 * Positive mechanical and electrical directions agree with positive q current
 * in the phase/Park convention of foc_core.h. At mechanical zero, electrical
 * phase equals the configured offset. Position P uses the continuous input.
 *
 * Usage example
 * -------------
 * Convert a coherent mechanical sample to electrical coordinates. Validate
 * its source time and quality before predicting for a later ADC sample.
 * For position control, feed the limited position P output to velocity PI.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * One caller owns each velocity instance. Position P and rotor functions are
 * stateless. Callers may evaluate a velocity copy pending downstream acceptance.
 * This module neither reads clocks nor commands hardware.
 *
 * Failure / recovery
 * ------------------
 * Invalid inputs clear result storage and leave dynamic state intact. Reset
 * velocity before a new run session. Position P has no state to reset.
 */
#pragma once

#include "foc_core.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Mapping from continuous mechanical coordinates to the electrical frame. */
struct foc_rotor_config {
    /** Positive count of electrical cycles per mechanical revolution. */
    uint16_t pole_pairs;
    /** Electrical angle in rad at mechanical zero, before normalization. */
    float electrical_offset_rad;
};

/** Coherent electrical phase and signed speed at one source instant. */
struct foc_electrical_state {
    /** Electrical phase in rad. Conversion normalizes it to [0, 2π). */
    float angle_rad;
    /** Signed electrical angular speed in rad/s. */
    float velocity_rad_s;
};

/** Forward prediction policy for a trusted angle and speed observation. */
struct foc_prediction_config {
    /** Inclusive horizon in µs, below the 32-bit half range. Zero is exact-only. */
    uint32_t max_prediction_us;
    /** Assumed upper bound on absolute electrical acceleration in rad/s². */
    float max_accel_rad_s2;
    /** Maximum permitted extrapolation error contribution in rad. */
    float max_error_rad;
};

/** Velocity PI gains mapping mechanical rad/s error to q current. */
struct foc_velocity_config {
    /** Proportional gain in A per (rad/s). */
    float kp_a_per_rad_s;
    /** Integral gain in A/rad. */
    float ki_a_per_rad;
    /** Inclusive maximum positive integration interval in seconds. */
    float max_dt_s;
};

/** Instance-owned velocity controller storage. */
struct foc_velocity_context {
    /** Validated copied settings. Treat as read-only outside this module. */
    struct foc_velocity_config config;
    /** Signed integral contribution in amperes. */
    float integral_a;
    /** Set by successful initialization. Zero-initialize before first use. */
    bool initialized;
};

/** Coherent velocity control input for one actual interval. */
struct foc_velocity_input {
    /** Mechanical velocity target in rad/s. */
    float ref_rad_s;
    /** Mechanical velocity measurement in rad/s. */
    float measured_rad_s;
    /** Actual outer-loop interval in seconds. */
    float dt_s;
    /** Positive symmetric q current limit in amperes. */
    float iq_limit_a;
    /** False yields disabled output and freezes the integral. */
    bool enabled;
};

/** Limited velocity PI result, cleared on failure. */
struct foc_velocity_output {
    /** Signed q current request in amperes. */
    float iq_ref_a;
    /** True if this cycle restricted output or integral admission. */
    bool saturated;
    /** True when this result is a usable current target. */
    bool enabled;
};

/** Stateless position P policy in mechanical coordinates. */
struct foc_position_config {
    /** Nonnegative position gain in inverse seconds. */
    float kp_per_s;
    /** Positive symmetric speed target limit in mechanical rad/s. */
    float velocity_limit_rad_s;
};

/** Position P output for the velocity controller. */
struct foc_position_output {
    /** Signed velocity request in mechanical rad/s. */
    float velocity_ref_rad_s;
    /** True when the configured speed limit clipped the request. */
    bool limited;
};

/* Public Functions ------------------------------------------------------------- */

/* Rotor coordinates ------------------------------------------------------------ */

/**
 * Convert a directed integer mechanical cycle to bounded electrical phase.
 *
 * The caller supplies a same-observation count in [0, counts_per_revolution).
 * CPR must be in [2, 65536]; direction is already applied by the sensor.
 * Integer multiplication/modulo precedes Float32 scaling. The offset belongs
 * to the rotor config and is applied exactly once. Large finite offsets have
 * the precision limits of foc_angle_normalize(); success grants no sensor
 * validity or output authority. Continuous mechanical position is untouched.
 *
 * @param config Immutable pole pairs (nonzero uint16) and finite offset.
 * @param mechanical_phase_count Directed nonnegative cycle count.
 * @param counts_per_revolution Resolution owned by the source sensor instance.
 * @param electrical_phase_rad Output in [0, Float32 2*pi), cleared on error.
 * @return FOC_STATUS_OK, ERR_NULL, ERR_INVALID_CONFIG, or ERR_NONFINITE.
 */
int foc_rotor_phase_from_counts(const struct foc_rotor_config *config,
                                uint32_t mechanical_phase_count,
                                uint32_t counts_per_revolution,
                                float *electrical_phase_rad);

/** Convert continuous position and speed in mechanical rad and rad/s to electrical units. */
int foc_rotor_electrical(const struct foc_rotor_config *config,
                         float mechanical_rad,
                         float mechanical_rad_s,
                         struct foc_electrical_state *output);

/**
 * Extrapolate a trusted electrical sample with constant velocity.
 *
 * @param config Horizon, acceleration assumption and error limit.
 * @param sample Finite electrical angle and speed at the same instant.
 * @param elapsed_us Forward age in µs, established by caller's modular clock check.
 * @param angle_rad Destination for the predicted phase, cleared on failure.
 * @return FOC_STATUS_OK or a validation, horizon, or prediction error.
 */
int foc_rotor_predict(const struct foc_prediction_config *config,
                      const struct foc_electrical_state *sample,
                      uint32_t elapsed_us,
                      float *angle_rad);

/* Motion loops ----------------------------------------------------------------- */

/** Validate settings and initialize one velocity instance without integral history. */
int foc_velocity_init(struct foc_velocity_context *context,
                      const struct foc_velocity_config *config);

/** Clear the integral of an initialized instance without changing its settings. */
int foc_velocity_reset(struct foc_velocity_context *context);

/**
 * Compute one limited q current request and conditionally integrate.
 *
 * @param context Initialized instance or its caller-owned trial copy.
 * @param input Read-only snapshot with an actual interval and a live current limit.
 * @param output Destination cleared on failure. Disabled input freezes state.
 * @return FOC_STATUS_OK or a deterministic validation/calculation error.
 */
int foc_velocity_step(struct foc_velocity_context *context,
                      const struct foc_velocity_input *input,
                      struct foc_velocity_output *output);

/** Convert continuous mechanical position error in rad to a limited rad/s reference. */
int foc_position_step(const struct foc_position_config *config,
                      float ref_rad,
                      float measured_rad,
                      struct foc_position_output *output);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
