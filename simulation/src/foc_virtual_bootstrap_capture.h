/**
 * @file    foc_virtual_bootstrap_capture.h
 * @brief   Bounded Host-only, open-circuit reference capture.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Own one isolated mechanical experiment. The caller confirms exclusive,
 * inhibited output and session eligibility at every requested aperture.
 *
 * Usage example
 * -------------
 * Supply a stationary Plant and an eligibility callback. On success, pass
 * the four sweeps to the portable bootstrap fit.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * One serialized Host caller owns the request and result in Foreground.
 * Plant motion occurs on a local copy discarded when capture returns.
 *
 * Failure / recovery
 * ------------------
 * A failed permit, instrument or following check ends the capture. Returned
 * samples from a failed capture are diagnostic data.
 */

#pragma once

#include "foc_bootstrap.h"
#include "foc_virtual_current.h"

/* Public Types ----------------------------------------------------------------- */

enum foc_virtual_bootstrap_capture_status {
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OK = 0,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_SETUP = -101,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_MEASUREMENT = -102,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_EXCITATION = -103,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_LIMIT = -104,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ERR_IDENTITY = -105,
};

enum foc_virtual_bootstrap_capture_reason {
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_NONE,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_SETUP,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_IDENTITY,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_GATE,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_OPEN_CIRCUIT,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_STALLED,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_FOLLOWING,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ENCODER,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_ADC,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LIMIT,
    FOC_VIRTUAL_BOOTSTRAP_CAPTURE_LOW_SIGNAL,
};

#define FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP FOC_CALIBRATION_MAX_REFERENCE_SAMPLES
#define FOC_VIRTUAL_BOOTSTRAP_SAMPLE_CAPACITY \
    (FOC_BOOTSTRAP_SWEEPS * FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP)

struct foc_virtual_cal_trial;

struct foc_virtual_bootstrap_capture_request {
    struct foc_cal_workflow_identity identity;
    uint32_t capture;
    uint32_t started_us;
    /** Optional Host encoder transport failure, applied at the first capture aperture. */
    bool encoder_unavailable;
    /** Return zero only while the original Runtime confirms exclusive INHIBIT. */
    int (*permit)(void *argument, uint32_t aperture_us);
    void *argument;
    const struct foc_virtual_cal_trial *trial;
};

struct foc_virtual_bootstrap_capture_result {
    struct foc_bootstrap_sample samples[FOC_VIRTUAL_BOOTSTRAP_SAMPLE_CAPACITY];
    uint32_t count;
    uint32_t completed_us;
    enum foc_virtual_bootstrap_capture_reason reason;
};

/* Public Functions ------------------------------------------------------------- */

/**
 * Run four alternating sweeps on a private copy of a stationary Plant.
 * Propagate a nonzero permit result directly. Partial output is diagnostic only.
 */
int foc_virtual_bootstrap_capture(const struct foc_virtual_motor *motor,
                                  const struct foc_virtual_current_profile *profile,
                                  const struct foc_virtual_bootstrap_capture_request *request,
                                  struct foc_virtual_bootstrap_capture_result *result);

const char *foc_virtual_bootstrap_capture_reason_name(
    enum foc_virtual_bootstrap_capture_reason reason);

/* ---------------------------------------- end of file ----------------------------------------- */
