/**
 * @file    foc_mechanical_id.h
 * @brief   Bounded offline motor-shaft inertia and viscous damping fit.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Fit integrated torque and speed over contiguous, non-overlapping windows.
 * Output is valid only for the qualified linear viscous model at this setup.
 *
 * Usage example
 * -------------
 * Initialize, push aligned endpoints, and finish after complete windows.
 * Pass usable results to a separate model-design caller.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * One serialized caller owns the context. Input/output storage is disjoint.
 * The acquisition owner qualifies Kt, the frame, load and effective alignment.
 *
 * Failure / recovery
 * ------------------
 * A bad endpoint invalidates the capture until reset. No partial J/B is usable.
 */
#pragma once

#include "foc_calibration.h"

/* Public Types ---------------------------------------------------------------- */

enum foc_mechanical_id_torque_source {
    FOC_MECHANICAL_ID_TORQUE_UNVERIFIED = 0,
    /** Actual dq observation, independently qualified for the Kt*Iq approximation. */
    FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ,
    /** Independently qualified complete electromagnetic motor torque. */
    FOC_MECHANICAL_ID_TORQUE_EQUIVALENT,
};

enum foc_mechanical_id_load_source {
    FOC_MECHANICAL_ID_LOAD_UNKNOWN = 0,
    /** Acquisition owner confirms negligible external torque. */
    FOC_MECHANICAL_ID_LOAD_NEGLIGIBLE,
    /** Signed external motor-axis torque at the shared aperture. */
    FOC_MECHANICAL_ID_LOAD_OBSERVED,
};

enum foc_mechanical_id_status {
    FOC_MECHANICAL_ID_OK = 0,
    FOC_MECHANICAL_ID_ERR_NULL = -1,
    FOC_MECHANICAL_ID_ERR_CONFIG = -2,
    FOC_MECHANICAL_ID_ERR_STATE = -3,
    FOC_MECHANICAL_ID_ERR_SAMPLE = -4,
    FOC_MECHANICAL_ID_ERR_PRECONDITION = -5,
    FOC_MECHANICAL_ID_ERR_QUALITY = -6,
};

/** One setup/frame, torque calibration/model and acquisition policy. */
struct foc_mechanical_id_config {
    uint32_t setup_id;
    uint32_t torque_model_id;
    enum foc_mechanical_id_torque_source torque_source;
    enum foc_mechanical_id_load_source load_source;
    uint32_t min_interval_us;
    uint32_t max_interval_us;
    uint32_t intervals_per_window;
    uint32_t min_windows;
    /** Independent motor-shaft Kt in N*m/A. Zero when unknown and equivalent torque is measured. */
    float torque_constant_nm_per_a;
    /** Maximum |Id| for the qualified Kt*Iq model, in A. */
    float max_abs_id_a;
    /** Effective speed resolution of the qualified observation, in rad/s. */
    float speed_resolution_rad_s;
    /** Absolute admissible speed and electromagnetic/load torque magnitudes. */
    float max_abs_speed_rad_s;
    float max_abs_torque_nm;
    /** RMS floors for window-average acceleration and speed. */
    float min_accel_rms_rad_s2;
    float min_speed_rms_rad_s;
    /** Minimum 1-r^2 of the normalized two-column Gram matrix, in (0,1]. */
    float min_excitation;
    /** Maximum RMS window-average torque residual, in N*m. */
    float max_residual_rms_nm;
    float min_inertia_kg_m2;
    float max_inertia_kg_m2;
    float max_damping_nm_s;
};

/** One same-aperture speed/torque observation and the preceding interval. */
struct foc_mechanical_id_sample {
    uint32_t setup_id;
    uint32_t torque_model_id;
    uint32_t capture_id;
    enum foc_mechanical_id_torque_source torque_source;
    enum foc_mechanical_id_load_source load_source;
    uint32_t sequence;
    uint32_t speed_us;
    uint32_t torque_us;
    /** Previous accepted aperture, or speed_us on the first endpoint. */
    uint32_t interval_begin_us;
    float speed_rad_s;
    /** Actual d/q currents in A, used only with OBSERVED_DQ. */
    float id_a;
    float iq_a;
    /** Actual equivalent motor torque in N*m, used only with EQUIVALENT. */
    float torque_nm;
    /** Opposing external torque, in N*m. Zero when confirmed negligible. */
    float load_nm;
    bool speed_valid;
    bool torque_valid;
    bool frame_confirmed;
    /** Confirms the interval belongs to this setup and both endpoints describe it.
     * The producer must account for acquisition, filter lag and actuation timing. */
    bool interval_confirmed;
};

/** Published fit or failure diagnostics. Both parameters are zero on failure. */
struct foc_mechanical_id_result {
    enum foc_calibration_state state;
    enum foc_calibration_failure failure;
    enum foc_mechanical_id_torque_source torque_source;
    enum foc_mechanical_id_load_source load_source;
    uint32_t setup_id;
    uint32_t torque_model_id;
    uint32_t capture_id;
    uint32_t first_us;
    uint32_t last_us;
    uint32_t last_sequence;
    uint32_t samples;
    uint32_t windows;
    uint32_t accel_windows;
    uint32_t decel_windows;
    float torque_constant_nm_per_a;
    float speed_resolution_rad_s;
    float inertia_kg_m2;
    float damping_nm_s;
    double residual_rms_nm;
    double accel_rms_rad_s2;
    double speed_rms_rad_s;
    double excitation;
    /** Condition number of the unit-column Gram matrix, when defined. */
    double gram_condition;
    bool usable;
};

/** Six sufficient statistics for complete mean-torque regression rows. */
struct foc_mechanical_id_gram {
    double xx;
    double yy;
    double zz;
    double xy;
    double xz;
    double yz;
};

/** Caller owned storage. Fields are read-only outside this API. */
struct foc_mechanical_id_context {
    struct foc_mechanical_id_config config;
    struct foc_mechanical_id_result result;
    struct foc_mechanical_id_gram gram;
    double speed_integral_rad;
    double torque_integral_nm_s;
    double previous_net_torque_nm;
    uint64_t window_us;
    uint32_t intervals;
    float window_start_speed_rad_s;
    float previous_speed_rad_s;
};

/* Public Functions ------------------------------------------------------------- */

/** Start a capture. Invalid configuration leaves an existing context intact. */
int foc_mechanical_id_init(struct foc_mechanical_id_context *context,
                           const struct foc_mechanical_id_config *config,
                           uint32_t capture_id);

/** Retain the configuration and start with a different, nonzero capture ID. */
int foc_mechanical_id_reset(struct foc_mechanical_id_context *context, uint32_t capture_id);

/** Validate and consume one endpoint. A bad endpoint requires reset. */
int foc_mechanical_id_push(struct foc_mechanical_id_context *context,
                           const struct foc_mechanical_id_sample *sample);

/** Finalize a complete set of windows. Failed fits retain diagnostics only. */
int foc_mechanical_id_finish(struct foc_mechanical_id_context *context,
                             struct foc_mechanical_id_result *result);

/* ---------------------------------------- end of file ----------------------------------------- */
