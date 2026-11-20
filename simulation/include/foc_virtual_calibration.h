/**
 * @file    foc_virtual_calibration.h
 * @brief   Execute one isolated Host calibration session against Virtual Motor.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * Run measured calibration against a dedicated virtual plant, using the
 * shared Workflow and Control Plane to qualify and bind the results.
 *
 * Usage example
 * -------------
 * Select a profile, start the session, and poll to REVIEW or a failure.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns and serializes Foreground service, the inhibited original
 * Runtime, and review. Each service call owns one bounded capture.
 *
 * Failure / recovery
 * ------------------
 * Cancel, timeout or invalid identity revokes the session. Restart with fresh
 * storage. Failed capture never applies a configuration.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_application.h"
#include "foc_virtual_calibration_electrical.h"
#include "foc_virtual_current.h"

/* Public Types ----------------------------------------------------------------- */

enum foc_virtual_calibration_stage {
    FOC_VIRTUAL_CAL_ADC,
    FOC_VIRTUAL_CAL_LUT,
    FOC_VIRTUAL_CAL_ROTOR,
    FOC_VIRTUAL_CAL_OFFSET,
    FOC_VIRTUAL_CAL_ELECTRICAL_D,
    FOC_VIRTUAL_CAL_ELECTRICAL_Q,
    FOC_VIRTUAL_CAL_FLUX,
    FOC_VIRTUAL_CAL_MECHANICAL,
    FOC_VIRTUAL_CAL_TUNE,
    FOC_VIRTUAL_CAL_REVIEW,
};

enum foc_virtual_calibration_status {
    FOC_VIRTUAL_CAL_OK = 0,
    FOC_VIRTUAL_CAL_PENDING = 1,
    FOC_VIRTUAL_CAL_ERR_SETUP = -1,
    FOC_VIRTUAL_CAL_ERR_CAPTURE = -2,
    FOC_VIRTUAL_CAL_ERR_REFERENCE = -3,
    FOC_VIRTUAL_CAL_ERR_TERMINATED = -4,
    FOC_VIRTUAL_CAL_ERR_TIMEOUT = -5,
};

/** Caller-owned session. Keep it at its original address. */
struct foc_virtual_calibration {
    struct foc_virtual_current_profile profile;
    struct foc_virtual_current original;
    struct foc_virtual_motor motor;
    struct foc_cal_workflow workflow;
    struct foc_cal_application application;
    struct foc_virtual_cal_electrical_config electrical;
    struct foc_virtual_cal_flux_result flux;
    enum foc_virtual_calibration_stage stage;
    uint32_t time_us;
    int status;
    int native_status;
    bool deadline_exceeded;
};

/* Public Functions ------------------------------------------------------------- */

/** Select supported Host fixture and the matching acquisition identity. */
int foc_virtual_calibration_profile(const char *name,
                                    struct foc_virtual_current_profile *profile,
                                    struct foc_cal_workflow_request *request);

/** Default Host impedance dynamics, shared by capture and package reload. */
struct foc_impedance_tuning_input foc_virtual_calibration_impedance_policy(
    const struct foc_virtual_current_profile *profile);

/** Initialize the deliberately uncalibrated production Host driver. */
int foc_virtual_calibration_baseline(void *argument,
                                     struct foc_runtime *runtime,
                                     struct foc_encoder_context *encoder,
                                     const struct foc_pipeline_config *pipeline,
                                     const struct foc_virtual_current_profile *profile);

/** Begin a new, exclusive session with no energized original Runtime. */
int foc_virtual_calibration_start(struct foc_virtual_calibration *session,
                                  const struct foc_virtual_current_profile *profile,
                                  const struct foc_cal_workflow_request *request);

/** Execute one Foreground stage. Return OK when a candidate can be reviewed. */
int foc_virtual_calibration_poll(struct foc_virtual_calibration *session);

/** Stop the session and revoke the candidate after cancel or fault. */
void foc_virtual_calibration_cancel(struct foc_virtual_calibration *session);

/** Stable machine-readable stage names. */
const char *foc_virtual_calibration_stage_name(enum foc_virtual_calibration_stage stage);

/* ---------------------------------------- end of file ----------------------------------------- */
