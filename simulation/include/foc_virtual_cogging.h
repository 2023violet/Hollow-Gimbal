/**
 * @file    foc_virtual_cogging.h
 * @brief   Host-only motor-shaft cogging acquisition and qualification.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * Training and the offset-grid holdout each run two opposed fixture sweeps.
 * The shared motor-shaft frame defines the sign and angular origin of the LUT.
 *
 * Usage example
 * -------------
 * Supply an initialized Virtual Motor and a scan policy containing two distinct
 * capture IDs. Review a qualified result before initializing a Host Runtime.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller serializes Foreground acquisition and owns the input and result.
 * Opposing sweeps run on private plant copies.
 *
 * Failure / recovery
 * ------------------
 * A failed scan leaves the candidate unqualified. Retry with a new capture ID
 * pair after correcting the reported acquisition or holdout failure.
 */
#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"
#include "foc_virtual_motor.h"

/* Public Types ---------------------------------------------------------------- */

/** Outcome of the offline capture and fit. */
enum foc_virtual_cogging_status {
    FOC_VIRTUAL_COGGING_OK = 0,
    FOC_VIRTUAL_COGGING_ERR_INPUT = -1,
    FOC_VIRTUAL_COGGING_ERR_CAPTURE = -2,
    FOC_VIRTUAL_COGGING_ERR_HOLDOUT = -3,
};

/** Coordinate identity, capture identity, and explicit qualification limits. */
struct foc_virtual_cogging_scan_config {
    /** Identity of the motor under test. */
    uint32_t motor_id;
    /** Identity of the positive shaft direction and zero used by the fixture. */
    uint32_t mechanical_frame_id;
    /** Independent nonzero ID assigned to the training capture. */
    uint32_t training_capture_id;
    /** Independent nonzero ID assigned to the offset-grid holdout. */
    uint32_t holdout_capture_id;
    /** Magnitude of both opposed sweep speeds, from 0.5 to 20 rad/s. */
    double speed_rad_s;
    /** Positive maximum RMS of the compensated holdout in N*m. */
    double max_holdout_rms_nm;
    /** Optional serialized session check before advancing the virtual fixture. */
    bool (*permit)(void *argument);
    void *permit_argument;
};

/** The candidate compensation and the measured nuisance-load and holdout metrics. */
struct foc_virtual_cogging_result {
    /** Frozen candidate with capture IDs and independent qualification status. */
    struct foc_runtime_cogging_lut lut;
    /** Signed non-periodic load intercept at mechanical zero, in N*m. */
    double training_load_bias_nm;
    /** Signed non-periodic load change per radian in the same frame. */
    double training_load_slope_nm_per_rad;
    /** Reference RMS of the isolated periodic measurement, in N*m. */
    double holdout_baseline_rms_nm;
    /** Residual RMS after bounded linear LUT interpolation, in N*m. */
    double holdout_compensated_rms_nm;
};

/* Public Functions ------------------------------------------------------------- */

/** Acquire and qualify the table. Nonzero status always leaves it unqualified. */
int foc_virtual_cogging_calibrate(const struct foc_virtual_motor *motor,
                                  const struct foc_virtual_cogging_scan_config *config,
                                  struct foc_virtual_cogging_result *result);

/* ---------------------------------------- end of file ----------------------------------------- */
