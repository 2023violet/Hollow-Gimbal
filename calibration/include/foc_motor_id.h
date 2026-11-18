/**
 * @file    foc_motor_id.h
 * @brief   Offline standstill R/L identification and model-based PI candidates.
 * @version v0.2.0
 * @date    2026.11.17
 *
 * Design overview
 * ---------------
 * Identify one axis from window-integrated standstill voltage and current.
 * Build a two-axis model and design PI candidates for explicit caller review.
 *
 * Usage example
 * -------------
 * Initialize, push complete capture windows, and finish. Pair usable d/q
 * captures from the same setup before passing the model to PI design.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * One caller owns and serializes each zero-initialized context. Treat fields
 * as read-only and keep API inputs and outputs disjoint. The acquisition
 * owner verifies applied voltage, sampling clock, fixed axes and standstill.
 * The estimator checks the supplied evidence fields for consistency.
 *
 * Failure / recovery
 * ------------------
 * Invalid captures publish no parameters and require reset before reuse.
 * PI and model-design errors clear the supplied output.
 */
#pragma once

#include "foc_calibration.h"

/* Public Types ---------------------------------------------------------------- */

/** Axis of a single capture in the same d/q convention as Current Core. */
enum foc_motor_id_axis {
    FOC_MOTOR_ID_D,
    FOC_MOTOR_ID_Q,
};

/** Claimed acquisition origin carried with the result. */
enum foc_motor_id_source {
    FOC_MOTOR_ID_VIRTUAL,
    FOC_MOTOR_ID_MEASURED,
};

/** Provenance of the applied interval-average voltage. */
enum foc_motor_id_voltage_source {
    /** Unknown source, request, duty estimate or otherwise unproven voltage. */
    FOC_MOTOR_ID_VOLTAGE_UNVERIFIED,
    /** Simulator's applied voltage observation. */
    FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
    /** Independently sensed and correctly timed terminal voltage. */
    FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL,
    /** Interval voltage reconstructed with independently validated calibration. */
    FOC_MOTOR_ID_VOLTAGE_CALIBRATED_TERMINAL,
};

/** Operation status. Fit diagnostics use the shared calibration failure enum. */
enum foc_motor_id_status {
    FOC_MOTOR_ID_OK = 0,
    FOC_MOTOR_ID_ERR_NULL = -1,
    FOC_MOTOR_ID_ERR_CONFIG = -2,
    FOC_MOTOR_ID_ERR_STATE = -3,
    FOC_MOTOR_ID_ERR_SAMPLE = -4,
    FOC_MOTOR_ID_ERR_PRECONDITION = -5,
    FOC_MOTOR_ID_ERR_QUALITY = -6,
};

/**
 * Caller-selected limits and quality policy for one axis.
 * The maximum allowed interval in seconds times max_rs_ohm / min_inductance_h
 * must not exceed 0.3. Current must be smooth enough for endpoint quadrature
 * with the supplied held or appropriately averaged voltage.
 */
struct foc_motor_id_config {
    enum foc_motor_id_axis axis;
    enum foc_motor_id_source source;
    enum foc_motor_id_voltage_source voltage_source;
    /** Nonzero motor, fixed frame and acquisition calibration identity. */
    uint32_t setup_id;
    uint32_t min_interval_us;
    uint32_t max_interval_us;
    /** Times between endpoints in each non-overlapping fit window, at least two. */
    uint32_t intervals_per_window;
    /** Complete windows required, at least three. */
    uint32_t min_windows;
    float max_abs_current_a;
    float max_abs_voltage_v;
    /** RMS excitation floors over complete windows. */
    float min_current_rms_a;
    float min_slew_rms_a_per_s;
    float min_voltage_rms_v;
    /** Maximum RMS error in window-average voltage. */
    float max_residual_rms_v;
    float min_rs_ohm;
    float max_rs_ohm;
    float min_inductance_h;
    float max_inductance_h;
};

/**
 * One signed current endpoint and its preceding interval's applied voltage.
 * The first sample has no preceding interval and ignores voltage fields
 * and output confirmation. Identity and frame/standstill evidence still apply.
 */
struct foc_motor_id_sample {
    uint32_t setup_id;
    uint32_t capture_id;
    enum foc_motor_id_source source;
    uint32_t sequence;
    /** ADC aperture time on the acquisition clock, modulo uint32_t. */
    uint32_t captured_us;
    /** Coverage on the ADC clock, [previous ADC aperture, this ADC aperture). */
    uint32_t voltage_begin_us;
    uint32_t voltage_end_us;
    enum foc_motor_id_voltage_source voltage_source;
    float current_a;
    /** Time-average actual axis voltage over the interval. Unused on first sample. */
    float mean_voltage_v;
    /** Signed electrical speed at capture. Zero is required. */
    float omega_e_rad_s;
    bool current_valid;
    bool voltage_valid;
    /** Acquisition evidence that output took effect over the entire interval. */
    bool output_confirmed;
    /** Acquisition evidence of a common fixed, aligned d/q frame for I and V. */
    bool frame_confirmed;
    /** Acquisition evidence of standstill over the whole interval. */
    bool stationary_confirmed;
    /** Reject this capture if excitation was clipped or saturated. */
    bool saturated;
};

/** Result and provenance of one capture. Parameters are zero on failure. */
struct foc_motor_id_result {
    enum foc_calibration_state state;
    enum foc_calibration_failure failure;
    enum foc_motor_id_axis axis;
    enum foc_motor_id_source source;
    /** Capture policy. Failed observations do not replace this provenance. */
    enum foc_motor_id_voltage_source voltage_source;
    uint32_t setup_id;
    uint32_t capture_id;
    uint32_t first_sequence;
    uint32_t last_sequence;
    uint32_t first_us;
    uint32_t last_us;
    uint32_t samples;
    uint32_t windows;
    float rs_ohm;
    float inductance_h;
    /** RMS residual for the published Float32 fit, with a roundoff allowance. */
    double residual_rms_v;
    /** Normalized Gram determinant, 0 for collinear and 1 for orthogonal. */
    double excitation;
    bool usable;
};

/** Caller-owned storage. The six sums describe window-average regressors. */
struct foc_motor_id_context {
    struct foc_motor_id_config config;
    struct foc_motor_id_result result;
    double i2;
    double slew2;
    double i_slew;
    double i_v;
    double slew_v;
    double v2;
    double current_as;
    double voltage_vs;
    uint64_t window_us;
    uint32_t intervals;
    float first_current_a;
    float previous_current_a;
};

/** Model values in the same electrical coordinate and units as Current Core. */
struct foc_motor_rl_model {
    float rs_ohm;
    float ld_h;
    float lq_h;
};

/** Operating bounds for one ideal-model PI design. */
struct foc_motor_pi_request {
    float bandwidth_hz;
    /** Worst admitted sample period in seconds, including jitter. */
    float sample_period_s;
    /** Worst sample-to-voltage-application delay in seconds, excluding hold. */
    float update_delay_s;
    /** Largest initial d/q current error vector magnitude in A for this request. */
    float initial_error_a;
    /** Guaranteed incremental voltage headroom in V after existing bias. */
    float available_voltage_v;
};

/** Candidate values for foc_current_config. Ki is shared by both axes. */
struct foc_motor_pi_candidate {
    float kp_d_v_per_a;
    float kp_q_v_per_a;
    float ki_v_per_a_s;
    float estimated_initial_voltage_v;
    float estimated_delay_phase_rad;
};

/* Public Functions ------------------------------------------------------------- */

/**
 * Start a new capture. Invalid configuration preserves the context.
 * Allocate a fresh capture ID for each acquisition within a setup, including
 * across reset, before samples can be routed to the context.
 */
int foc_motor_id_init(struct foc_motor_id_context *context,
                      const struct foc_motor_id_config *config,
                      uint32_t capture_id);

/** Clear fit state, retaining the configuration and adopting a new capture ID. */
int foc_motor_id_reset(struct foc_motor_id_context *context, uint32_t capture_id);

/** Append an endpoint. A bad observation invalidates the capture until reset. */
int foc_motor_id_push(struct foc_motor_id_context *context,
                      const struct foc_motor_id_sample *sample);

/** Finalize complete windows, publishing only a complete quality-checked fit. */
int foc_motor_id_finish(struct foc_motor_id_context *context, struct foc_motor_id_result *result);

/**
 * Assemble a model from usable d and q fits sharing setup, origin and
 * voltage source but with distinct capture IDs. The relative Rs difference is
 * abs(Rd - Rq) / max(Rd, Rq). Retain both results as the model's quality and
 * acquisition records. Failure clears model.
 */
int foc_motor_id_make_model(const struct foc_motor_id_result *d,
                            const struct foc_motor_id_result *q,
                            float max_rs_relative_difference,
                            struct foc_motor_rl_model *model);

/**
 * Compute an ideal-model gain candidate for finite, trusted model values.
 * Checks sampling, delay and first-update voltage bounds. Failure clears output.
 * The caller separately validates and applies gains for the target controller.
 */
int foc_motor_pi_design(const struct foc_motor_rl_model *model,
                        const struct foc_motor_pi_request *request,
                        struct foc_motor_pi_candidate *candidate);

/* ---------------------------------------- end of file ----------------------------------------- */
