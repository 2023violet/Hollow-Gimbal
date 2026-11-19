/**
 * @file    foc_following_error.h
 * @brief   Bounded position following-error monitor using Sensor capture time.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Monitor position-reference minus measured position. Distinct over-threshold
 * observations with bounded source gaps establish a continuous interval.
 *
 * Usage example
 * -------------
 * Validate settings and zero-initialize state. Step with the effective
 * position reference and a qualified observation from one mechanical frame.
 * A caller may update a trial state before committing the accepted cycle.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Caller-owned state per instance and motion session. Reset at a session or
 * mode boundary. The caller owns STOP and any diagnostic retention.
 *
 * Failure / recovery
 * ------------------
 * Loss of usable input breaks the interval. Duplicate source times never
 * accumulate duration. A backward source time cannot advance the interval.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

struct foc_following_error_config {
    /** Strictly exceeded magnitude threshold in rad when enabled. */
    float threshold_rad;
    /** Required uninterrupted observed duration in microseconds. */
    uint32_t duration_us;
    /** Largest allowed interval between distinct captures in microseconds. */
    uint32_t max_gap_us;
    bool enabled;
};

enum foc_following_state {
    FOC_FOLLOWING_INACTIVE = 0,
    FOC_FOLLOWING_TRACKING,
    FOC_FOLLOWING_PENDING,
    FOC_FOLLOWING_TRIGGERED,
    FOC_FOLLOWING_NO_REFERENCE,
    FOC_FOLLOWING_INVALID_SENSOR,
    FOC_FOLLOWING_TIME_ORDER,
};

struct foc_following_error_state {
    /** Most recent accepted capture in the running monitor. */
    uint32_t sample_us;
    /** Observed continuous above-threshold time, saturated at the duration. */
    uint32_t elapsed_us;
    enum foc_following_state status;
    bool seen;
};

struct foc_following_error_input {
    float reference_rad;
    float measured_rad;
    uint32_t captured_us;
    bool applicable;
    bool reference_valid;
    bool sensor_valid;
};

/* Public Functions ------------------------------------------------------------- */

/** Enabled thresholds must be positive and times must fit the uint32 half range. */
bool foc_following_error_validate(const struct foc_following_error_config *config);

/**
 * Advance a monitor with one captured observation. Duplicate usable samples
 * leave the monitor unchanged. A gap beyond max_gap_us starts a new interval.
 * Invalid configuration or a null pointer returns INACTIVE without writing.
 */
enum foc_following_state foc_following_error_step(const struct foc_following_error_config *config,
                                                  struct foc_following_error_state *state,
                                                  const struct foc_following_error_input *input);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
