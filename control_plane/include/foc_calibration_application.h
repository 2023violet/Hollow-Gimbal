/**
 * @file    foc_calibration_application.h
 * @brief   Bind reviewed calibration to explicit fresh Host Runtime initialization.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Start against a coherent, inhibited original Runtime. Prepare detached
 * Control Plane values and check existing Config Store compatibility.
 *
 * Usage example
 * -------------
 * Start, collect and review through the workflow, then compare the prepared
 * values with the captured effective configuration using Config Inspector.
 * Explicitly apply to fresh Runtime and sensor storage and read back.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The application borrows one original Runtime and workflow for their
 * lifetimes without reinitializing either in place. Caller serializes
 * Foreground and quiesces original Runtime service and event ingress
 * throughout apply. Retire the binding before either 32-bit Session
 * generation can cycle back to its captured value. Keep the initialized
 * sensor at its original address and its LUT immutable for its lifetime.
 *
 * Failure / recovery
 * ------------------
 * Pending STOP, fault, lifecycle, stale binding and unsafe configurations
 * block application. Runtime preemption hooks are refused before observation
 * because a telemetry read can invoke them. Failed calls leave destination
 * Runtime and sensor untouched. Prepare and apply perform no Config Store writes.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_workflow.h"
#include "foc_config_diagnostics.h"

/* Public Types ----------------------------------------------------------------- */

enum foc_cal_application_status {
    FOC_CAL_APPLICATION_OK = 0,
    FOC_CAL_APPLICATION_PREPARED,
    FOC_CAL_APPLICATION_APPLIED,
    FOC_CAL_APPLICATION_ERR_INPUT = -1,
    /** The original Runtime cannot be observed without interference. */
    FOC_CAL_APPLICATION_ERR_RUNTIME = -2,
    FOC_CAL_APPLICATION_ERR_INHIBIT = -3,
    FOC_CAL_APPLICATION_ERR_FAULT = -4,
    FOC_CAL_APPLICATION_ERR_LIFECYCLE = -5,
    FOC_CAL_APPLICATION_ERR_STALE = -6,
    FOC_CAL_APPLICATION_ERR_IDENTITY = -7,
    FOC_CAL_APPLICATION_ERR_REVIEW = -8,
    FOC_CAL_APPLICATION_ERR_MAPPING = -9,
    FOC_CAL_APPLICATION_ERR_CONFIG = -10,
    FOC_CAL_APPLICATION_ERR_LUT = -11,
    FOC_CAL_APPLICATION_ERR_WORKFLOW = -12,
};

/** Caller-owned binding. Treat captured values as read-only after start. */
struct foc_cal_application {
    struct foc_runtime *original;
    const struct foc_cal_workflow *workflow;
    struct foc_runtime_diagnostic original_view;
    struct foc_pipeline_config pipeline;
    struct foc_encoder_config encoder;
    struct foc_cal_workflow_identity identity;
    uint32_t session;
    uint32_t boot_us;
};

/** Detached reviewed values. No prepared value has been applied or persisted. */
struct foc_cal_application_candidate {
    struct foc_parameter_candidate runtime;
    struct foc_pipeline_config pipeline;
    struct foc_encoder_config encoder;
    /** SOFT-03 validation against the original trusted setup, for schema-covered fields. */
    int store_status;
    bool has_lut;
};

/** Caller-owned sensor for the new Runtime. Preserve its address while in use. */
struct foc_cal_application_sensor {
    struct foc_encoder_context encoder;
    int16_t lut_offsets[FOC_ENCODER_CAL_MAX_NODES];
};

/* Public Functions ------------------------------------------------------------- */

/**
 * Capture a fault-free DISABLED original with confirmed INHIBIT and start a
 * workflow against the supplied identity. Only ADC phase order and current
 * scale from pipeline are used to validate acquisition. The initialized
 * original supplies the effective Pipeline policy. encoder describes the
 * existing mechanical direction and uncompensated encoder settings. A
 * reviewed LUT can be installed when its stage passes. Failed start
 * preserves the binding.
 */
int foc_cal_application_start(struct foc_cal_application *application,
                              struct foc_cal_workflow *workflow,
                              const struct foc_cal_workflow_request *request,
                              struct foc_runtime *original,
                              const struct foc_pipeline_config *pipeline,
                              const struct foc_encoder_config *encoder);

/**
 * Validate a live binding and prepare the current reviewed workflow values.
 * Use original_view.effective and candidate.runtime.config with
 * foc_config_diff(). A successful result always requires fresh init.
 */
int foc_cal_application_prepare(const struct foc_cal_application *application,
                                struct foc_cal_application_candidate *candidate);

/**
 * Explicitly initialize distinct, zero-initialized, quiescent Runtime and
 * sensor storage with the current reviewed workflow values. The new
 * Runtime uses the closed Host Mock; the original retains its output owner.
 * Use foc_runtime_diagnose() to read back the new effective Runtime values.
 */
int foc_cal_application_apply(const struct foc_cal_application *application,
                              struct foc_runtime *destination,
                              struct foc_cal_application_sensor *sensor,
                              uint32_t now_us);

/* ---------------------------------------- end of file ----------------------------------------- */
