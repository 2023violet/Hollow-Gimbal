/**
 * @file    foc_encoder.h
 * @brief   Float32 magnetic encoder angle, velocity, filtering, and quality.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Design overview
 * ---------------
 * This module converts timestamped absolute encoder codes into directed
 * single-turn angle, continuous mechanical position, and three comparable
 * velocity estimates: raw difference, difference plus LPF, and an alpha-beta
 * position/velocity tracker. It has no MCU, SPI, RTOS, Runtime, or hardware
 * dependency.
 *
 * Usage example
 * -------------
 * Initialize caller-owned storage with foc_encoder_init(), pass each complete
 * source-time frame to foc_encoder_update(), and publish only feedback whose
 * quality is FOC_ENCODER_QUALITY_VALID. Call foc_encoder_reset() for a new
 * motion session after continuity is lost.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns one context per independent encoder and serializes updates.
 * No dynamic allocation or global mutable state is used. The source timestamp
 * is the capture time, not the transport completion time. Configuration is
 * copied at initialization and is read-only afterward.
 *
 * Failure / recovery
 * ------------------
 * Invalid transport, timestamps, samples, speed, or continuity are reported
 * with an explicit quality result. Invalid observations never replace the
 * last accepted position or advance either filter. A continuity failure is
 * latched until an explicit reset; a later valid frame cannot guess missed
 * revolutions or silently re-enable continuity.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

/** Direction applied to raw code differences and the initial single-turn angle. */
enum foc_encoder_direction {
    /** Increasing raw code means increasing mechanical angle. */
    FOC_ENCODER_DIRECTION_NORMAL = 1,
    /** Increasing raw code means decreasing mechanical angle. */
    FOC_ENCODER_DIRECTION_REVERSED = -1,
};

/** Transport disposition supplied by the SPI/driver owner. */
enum foc_encoder_transport {
    /** One complete angle frame was captured. */
    FOC_ENCODER_TRANSPORT_OK = 0,
    /** The bounded transaction did not complete in time. */
    FOC_ENCODER_TRANSPORT_TIMEOUT,
    /** The peripheral reported a frame or protocol error. */
    FOC_ENCODER_TRANSPORT_FRAME_ERROR,
};

/** Quality of the most recently supplied observation. */
enum foc_encoder_quality {
    /** No accepted frame has established an angle reference. */
    FOC_ENCODER_QUALITY_UNINITIALIZED = 0,
    /** A position and velocity observation is valid for publication. */
    FOC_ENCODER_QUALITY_VALID,
    /** The transport timed out. */
    FOC_ENCODER_QUALITY_TIMEOUT,
    /** The transport returned an invalid frame. */
    FOC_ENCODER_QUALITY_TRANSPORT_ERROR,
    /** The source timestamp repeated. */
    FOC_ENCODER_QUALITY_DUPLICATE,
    /** The source timestamp moved backward or exceeded the half-range. */
    FOC_ENCODER_QUALITY_TIME_ORDER_ERROR,
    /** The interval or displacement is ambiguous for continuous position. */
    FOC_ENCODER_QUALITY_DISCONTINUITY,
    /** The measured velocity exceeds the configured physical bound. */
    FOC_ENCODER_QUALITY_SPEED_OUT_OF_RANGE,
    /** The module configuration is invalid. */
    FOC_ENCODER_QUALITY_INVALID_CONFIG,
    /** The raw sample or calculated value is not finite or representable. */
    FOC_ENCODER_QUALITY_NUMERIC_ERROR,
    /** The raw code is outside the configured code range. */
    FOC_ENCODER_QUALITY_INVALID_SAMPLE,
    /** The interval is positive but below the configured minimum. */
    FOC_ENCODER_QUALITY_INVALID_INTERVAL,
};

/** Status returned by initialization, reset, and an update call. */
enum foc_encoder_status {
    /** The operation completed; the feedback quality still describes readiness. */
    FOC_ENCODER_STATUS_OK = 0,
    /** A required pointer was NULL. */
    FOC_ENCODER_STATUS_ERR_NULL = -1,
    /** Configuration validation failed. */
    FOC_ENCODER_STATUS_ERR_CONFIG = -2,
    /** The context has not been initialized. */
    FOC_ENCODER_STATUS_ERR_NOT_INITIALIZED = -3,
    /** The transport did not provide a usable frame. */
    FOC_ENCODER_STATUS_ERR_TRANSPORT = -4,
    /** The source timestamp was repeated or out of order. */
    FOC_ENCODER_STATUS_ERR_ORDER = -5,
    /** The interval or displacement cannot establish continuity. */
    FOC_ENCODER_STATUS_ERR_DISCONTINUITY = -6,
    /** The calculated speed exceeds the configured bound. */
    FOC_ENCODER_STATUS_ERR_SPEED = -7,
    /** The sample or calculated value was invalid. */
    FOC_ENCODER_STATUS_ERR_SAMPLE = -8,
};

/** Static policy for one absolute encoder instance. */
struct foc_encoder_config {
    /** Number of raw codes in one mechanical revolution, up to 65536. */
    uint32_t counts_per_revolution;
    /** Direction applied to code differences and initial angle. */
    enum foc_encoder_direction direction;
    /** Maximum physically credible mechanical speed in rad/s. */
    float max_speed_rad_s;
    /** Minimum accepted source interval in microseconds. */
    uint32_t min_dt_us;
    /** Maximum accepted source interval before continuity becomes ambiguous. */
    uint32_t max_dt_us;
    /** First-order speed LPF cutoff in Hz. */
    float speed_lpf_cutoff_hz;
    /** Alpha-beta position correction gain in (0, 1]. */
    float tracker_alpha;
    /** Alpha-beta velocity correction gain in (0, 2]. */
    float tracker_beta;
};

/** Complete result for one source observation. */
struct foc_encoder_feedback {
    /** Source capture timestamp in the common microsecond epoch. */
    uint32_t captured_us;
    /** Raw absolute code supplied by the transport owner. */
    uint32_t raw_code;
    /** Directed single-turn angle in [0, 2*pi) when the sample is accepted. */
    float single_turn_rad;
    /** Continuous mechanical position in radians; Float32 resolution falls with magnitude. */
    float mechanical_rad;
    /** Directed position change from the previous accepted frame. */
    float delta_rad;
    /** Actual source interval in seconds. */
    float dt_s;
    /** Difference velocity before filtering. */
    float raw_velocity_rad_s;
    /** Difference velocity after the variable-dt first-order LPF. */
    float filtered_velocity_rad_s;
    /** Alpha-beta tracked velocity. */
    float tracked_velocity_rad_s;
    /** True only after a valid difference velocity is available. */
    bool speed_valid;
    /** True only when the position and speed are valid for H1 publication. */
    bool valid;
    /** True after a continuity failure until explicit reset. */
    bool continuity_lost;
    /** Bounded count of rejected observations since initialization/reset. */
    uint32_t invalid_count;
    /** Quality of this result. */
    enum foc_encoder_quality quality;
    /**
     * Directed integer cycle in [0, config.counts_per_revolution), from this
     * accepted raw observation; equal to mathematical_mod(position_counts, CPR).
     * Present on the first position-only frame without making speed/valid true.
     * Cleared on rejected observations; zero alone never implies validity.
     */
    uint32_t mechanical_phase_count;
};

/** Caller-owned state for one encoder instance. */
struct foc_encoder_context {
    /** Copied immutable policy. */
    struct foc_encoder_config config;
    /** Cached LPF time constant in seconds. */
    float lpf_tau_s;
    /** Integer continuous position; conversion to Float32 occurs at output. */
    int64_t position_counts;
    /** Last accepted source timestamp and raw code. */
    uint32_t previous_us;
    uint32_t previous_code;
    /** Cached filter states. */
    float lpf_velocity_rad_s;
    float tracker_position_rad;
    float tracker_velocity_rad_s;
    /** Lifecycle and quality state. */
    uint32_t invalid_count;
    bool initialized;
    bool previous_seen;
    bool velocity_seen;
    bool tracker_seen;
    bool continuity_lost;
};

/* Public Functions ------------------------------------------------------------- */

/** Validate configuration and initialize one encoder context. */
int foc_encoder_init(struct foc_encoder_context *context, const struct foc_encoder_config *config);

/** Reset position, timing, filter history, and quality while retaining policy. */
int foc_encoder_reset(struct foc_encoder_context *context);

/**
 * Consume one source-timestamped absolute code and produce all estimates.
 *
 * The first transport-successful frame establishes position only and returns
 * UNINITIALIZED quality. A valid velocity requires a second accepted frame.
 */
int foc_encoder_update(struct foc_encoder_context *context,
                       uint32_t captured_us,
                       uint32_t raw_code,
                       enum foc_encoder_transport transport,
                       struct foc_encoder_feedback *feedback);

/* ---------------------------------------- end of file ----------------------------------------- */
