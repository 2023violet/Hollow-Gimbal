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
    FOC_CAL_APPLICATION_ERR_TUNING = -13,
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

/** Explicit, caller-owned Host output binding for the new instance. */
struct foc_cal_application_output {
    foc_pipeline_output_submit_t submit;
    void *argument;
};

/** Timing and headroom supported by the intended Host execution envelope. */
struct foc_cal_application_tuning_policy {
    struct foc_motor_pi_request current;
    float max_rs_relative_difference;
    float velocity_natural_frequency_rad_s;
    float velocity_damping_ratio;
    float position_bandwidth_rad_s;
    float outer_extra_delay_s;
    /** Require usable J/B, qualified Kt and a reviewed impedance candidate. */
    bool tune_outer;
#if defined(FOC_RUNTIME_HOST_COGGING)
    /** Optional qualified Host LUT from independent captures in this setup. */
    const struct foc_runtime_cogging_lut *cogging_lut;
    /** Positive q-axis budget within the existing motion current limit. */
    float cogging_iq_limit_a;
#endif
};

/** Values presented for separate gain approval after workflow review. */
struct foc_cal_application_gains {
    struct foc_cal_workflow_identity identity;
    uint32_t session;
    uint32_t d_capture;
    uint32_t q_capture;
    uint32_t mechanical_capture;
    uint32_t kt_capture;
    struct foc_motor_rl_model model;
    struct foc_motor_pi_candidate current;
    struct foc_velocity_config velocity;
    struct foc_position_config position;
    /** Publish K/D through the usual IMPEDANCE command and admission path. */
    struct foc_impedance_tuning_candidate impedance;
    float torque_constant_nm_per_a;
    bool outer_tuned;
#if defined(FOC_RUNTIME_HOST_COGGING)
    struct foc_runtime_cogging_config cogging;
#endif
};

/** Detached reviewed values. No prepared value has been applied or persisted. */
struct foc_cal_application_candidate {
    struct foc_parameter_candidate runtime;
    struct foc_pipeline_config pipeline;
    struct foc_encoder_config encoder;
    /** SOFT-03 validation against the original trusted setup, for schema-covered fields. */
    int store_status;
    bool has_lut;
    struct foc_cal_application_gains gains;
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

/**
 * Preview gains from a reviewed, dual-axis workflow. The current policy must
 * cover admitted H0 timing and fit the existing current and voltage bounds.
 * When tune_outer is set, retain all Runtime caps and screen the requested
 * velocity and position dynamics against H1 timing and loop separation.
 */
int foc_cal_application_prepare_tuned(const struct foc_cal_application *application,
                                      const struct foc_cal_application_tuning_policy *policy,
                                      struct foc_cal_application_candidate *candidate);

/**
 * Recompute velocity and position from qualified shaft dynamics and the
 * admitted timing envelope. Both outputs stay unchanged on failure.
 */
bool foc_cal_application_design_outer(const struct foc_cal_application_tuning_policy *policy,
                                      const struct foc_runtime_config *runtime,
                                      float inertia_kg_m2,
                                      float damping_nm_s,
                                      float kt_nm_per_a,
                                      struct foc_velocity_config *velocity,
                                      struct foc_position_config *position);

/**
 * Explicitly approve the exact previewed gains and initialize a fresh Host
 * Runtime and Encoder. A legacy workflow can supply NULL for policy and
 * approved_gains together. The adapter must differ from the original output
 * binding and may be invoked on initial INHIBIT before this call returns.
 * The callback is serialized, non-reentrant and owns its argument for the
 * lifetime of the new Runtime. Failed calls preserve destination storage.
 */
int foc_cal_application_apply_with_output(const struct foc_cal_application *application,
                                          const struct foc_cal_application_tuning_policy *policy,
                                          const struct foc_cal_application_gains *approved_gains,
                                          const struct foc_cal_application_output *output,
                                          struct foc_runtime *destination,
                                          struct foc_cal_application_sensor *sensor,
                                          uint32_t now_us);

/**
 * Initialize a fresh Host instance from a detached configuration after the
 * caller verifies the persisted package version, integrity, review and setup
 * identity. No persisted callback is accepted. A supplied LUT is validated
 * against the expected Encoder identity and copied into sensor storage.
 */
int foc_cal_application_restore(const struct foc_cal_application_candidate *candidate,
                                uint32_t encoder_id,
                                const struct foc_encoder_lut *lut,
                                const struct foc_cal_application_output *output,
                                struct foc_runtime *destination,
                                struct foc_cal_application_sensor *sensor,
                                uint32_t now_us);

/* ---------------------------------------- end of file ----------------------------------------- */
