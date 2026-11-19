/**
 * @file    foc_calibration_application.c
 * @brief   Prepare reviewed calibration and initialize a distinct Host Runtime.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Bind the calibration identity to the original inhibited Runtime and reuse
 * the Control Plane, Inspector, Encoder and Runtime initialization contracts.
 *
 * Includes
 * --------
 * Portable C11 application boundary. The Host adapter is the Pipeline Mock.
 *
 * Private Helpers
 * ---------------
 * Capture the original lifecycle, validate the still-reviewed workflow and
 * stage all configuration before touching destination storage.
 *
 * Public Functions
 * ----------------
 * Capture a baseline, prepare a detached candidate, and explicitly initialize.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_application.h"

#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

static int capture(struct foc_runtime *runtime, struct foc_runtime_diagnostic *view)
{
    if (!runtime->initialized || runtime->config.hook != NULL) {
        return FOC_CAL_APPLICATION_ERR_RUNTIME;
    }
    struct foc_runtime_lifecycle_result lifecycle;
    if (foc_runtime_read_lifecycle_result(runtime, &lifecycle) != FOC_RUNTIME_OK ||
        foc_runtime_diagnose(runtime, view) != FOC_RUNTIME_OK) {
        return FOC_CAL_APPLICATION_ERR_RUNTIME;
    }
    const struct foc_runtime_snapshot *snapshot = &view->snapshot;
    if (snapshot->faults != 0U || snapshot->state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_CAL_APPLICATION_ERR_FAULT;
    }
    if (snapshot->state != FOC_SAFE_STATE_DISABLED || !snapshot->output_confirmed ||
        snapshot->action != FOC_DEVICE_OUTPUT_INHIBIT || snapshot->inhibit_pending) {
        return FOC_CAL_APPLICATION_ERR_INHIBIT;
    }
    if (lifecycle.result == FOC_RUNTIME_REQUEST_PENDING ||
        snapshot->clear_result == FOC_RUNTIME_CLEAR_PENDING) {
        return FOC_CAL_APPLICATION_ERR_LIFECYCLE;
    }
    return FOC_CAL_APPLICATION_OK;
}

static bool source_matches(
    const struct foc_cal_application *application,
    const struct foc_pipeline_config *acquisition_pipeline
)
{
    const struct foc_cal_workflow_identity *identity = &application->identity;
    const struct foc_runtime_motion_config *motion = &application->original_view.effective.motion;
    const struct foc_calibration_identity *acquisition = &identity->acquisition;
    const struct foc_device_adc_config *adc = &application->pipeline.adc;
    return motion->enabled && identity->motor_setup_id != 0U && identity->encoder_id != 0U &&
        identity->mechanical_frame_id != 0U &&
        (motion->limits.mechanical_frame_id == 0U ||
         motion->limits.mechanical_frame_id == identity->mechanical_frame_id) &&
        (!motion->impedance.enabled ||
         motion->impedance.mechanical_frame_id == identity->mechanical_frame_id) &&
        acquisition->encoder_counts_per_revolution == application->encoder.counts_per_revolution &&
        acquisition->current_a_per_count == acquisition_pipeline->adc.current_a_per_count &&
        acquisition->current_a_per_count == adc->current_a_per_count &&
        acquisition->phase_order == (uint32_t)acquisition_pipeline->adc.phase_order &&
        acquisition->phase_order == (uint32_t)adc->phase_order;
}

static int check_original(const struct foc_cal_application *application)
{
    struct foc_runtime_diagnostic view;
    const int status = capture(application->original, &view);
    if (status != FOC_CAL_APPLICATION_OK) {
        return status;
    }
    const struct foc_config_diff_input original = {
        .config = &application->original_view.effective,
        .status = FOC_CONFIG_STORE_OK,
    };
    const struct foc_runtime_snapshot *bound = &application->original_view.snapshot;
    if (application->boot_us != application->original->boot_us ||
        view.snapshot.motion_session != bound->motion_session ||
        view.snapshot.mode != bound->mode || view.snapshot.updates != bound->updates ||
        foc_config_application_status(&original, &view) != FOC_DIAG_MATCHES_EFFECTIVE) {
        return FOC_CAL_APPLICATION_ERR_STALE;
    }
    return FOC_CAL_APPLICATION_OK;
}

static int prepare_values(
    const struct foc_cal_application *application,
    struct foc_cal_application_candidate *candidate,
    struct foc_encoder_lut *lut
)
{
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(application->workflow);
    if (bundle == NULL || bundle->request.session != application->session ||
        !foc_cal_workflow_identity_matches(&application->identity, &bundle->request.identity)) {
        return FOC_CAL_APPLICATION_ERR_IDENTITY;
    }
    if (bundle->state != FOC_CAL_WORKFLOW_COMPLETED || !bundle->complete || !bundle->applicable ||
        !bundle->reviewed) {
        return FOC_CAL_APPLICATION_ERR_REVIEW;
    }
    const struct foc_calibration_snapshot *snapshot = &bundle->snapshot;
    if (!foc_calibration_snapshot_valid(snapshot) ||
        !foc_calibration_identity_matches(
            &snapshot->source_identity,
            &application->identity.acquisition
        )) {
        return FOC_CAL_APPLICATION_ERR_IDENTITY;
    }
    if (snapshot->runtime_encoder_direction != (int32_t)application->encoder.direction) {
        return FOC_CAL_APPLICATION_ERR_MAPPING;
    }

    struct foc_cal_application_candidate trial = {
        .pipeline = application->pipeline,
        .encoder = application->encoder,
        .has_lut = (bundle->passed & FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_LUT)) != 0U,
    };
    if (trial.has_lut &&
        ((int32_t)bundle->lut.setup.direction != snapshot->runtime_encoder_direction ||
         foc_encoder_cal_make_lut(&bundle->lut, lut) != FOC_ENCODER_CAL_OK ||
         foc_encoder_lut_validate(
             lut,
             trial.encoder.counts_per_revolution,
             application->identity.encoder_id
         ) != FOC_ENCODER_STATUS_OK)) {
        return FOC_CAL_APPLICATION_ERR_LUT;
    }
    trial.pipeline.output_submit = NULL;
    trial.pipeline.output_arg = NULL;
    trial.pipeline.adc.phase_a_offset_count = snapshot->phase_a_offset_count;
    trial.pipeline.adc.phase_b_offset_count = snapshot->phase_b_offset_count;
    trial.pipeline.adc.phase_c_offset_count = snapshot->phase_c_offset_count;

    struct foc_runtime_config base = application->original_view.effective;
    base.hook = NULL;
    base.hook_arg = NULL;
    base.motion.rotor.pole_pairs = snapshot->pole_pairs;
    base.motion.rotor.electrical_offset_rad = snapshot->electrical_offset_rad;
    if (base.motion.impedance.enabled && bundle->kt.qualified) {
        if (!foc_cal_workflow_identity_matches(&application->identity, &bundle->kt.identity)) {
            return FOC_CAL_APPLICATION_ERR_IDENTITY;
        }
        base.motion.impedance.torque_constant_nm_per_a = bundle->kt.nm_per_a;
    }

    /* Read-only calibration fields belong to the qualified init base. Reassert
     * the existing outer current limit to validate the detached configuration. */
    const struct foc_parameter_edit limit = {
        .id = FOC_PARAMETER_MOTION_IQ_LIMIT,
        .value = {.type = FOC_PARAMETER_F32, .data = {.f32 = base.motion.iq_limit_a}},
    };
    if (foc_parameter_prepare_initial(&trial.pipeline, &base, &limit, 1U, &trial.runtime) !=
        FOC_PARAMETER_VALIDATED) {
        return FOC_CAL_APPLICATION_ERR_CONFIG;
    }

    const struct foc_runtime_config *original = &application->original_view.effective;
    const struct foc_config_store_policy policy = {
        .pipeline = &application->pipeline,
        .base = original,
        .identity = {
            .encoder_cpr = application->encoder.counts_per_revolution,
            .encoder_direction = application->encoder.direction,
            .mechanical_frame_id = application->identity.mechanical_frame_id,
            .encoder_identity = application->identity.encoder_id,
            .motor_setup_id = application->identity.motor_setup_id,
            .pole_pairs = original->motion.rotor.pole_pairs,
        },
    };
    trial.store_status = foc_config_store_validate(&policy, &trial.runtime);
    *candidate = trial;
    return FOC_CAL_APPLICATION_PREPARED;
}

/* Public Functions ------------------------------------------------------------- */

int foc_cal_application_start(
    struct foc_cal_application *application,
    struct foc_cal_workflow *workflow,
    const struct foc_cal_workflow_request *request,
    struct foc_runtime *original,
    const struct foc_pipeline_config *pipeline,
    const struct foc_encoder_config *encoder
)
{
    if (application == NULL || workflow == NULL || request == NULL || original == NULL ||
        pipeline == NULL || encoder == NULL) {
        return FOC_CAL_APPLICATION_ERR_INPUT;
    }
    struct foc_cal_application trial = {
        .original = original,
        .workflow = workflow,
        .encoder = *encoder,
        .identity = request->identity,
        .session = request->session,
    };
    const int status = capture(original, &trial.original_view);
    if (status != FOC_CAL_APPLICATION_OK) {
        return status;
    }
    const struct foc_pipeline *effective = &original->pipeline;
    trial.pipeline = (struct foc_pipeline_config){
        .current = effective->current.config,
        .adc = effective->adc_config,
        .pwm = effective->pwm_config,
        .safety = effective->safety.config,
        .feedforward = {
            .model = effective->feedforward.model,
            .enabled = effective->feedforward.enabled,
            .max_age_us = effective->feedforward_max_age_us,
        },
    };
    if (!source_matches(&trial, pipeline)) {
        return FOC_CAL_APPLICATION_ERR_IDENTITY;
    }
    struct foc_encoder_context probe;
    if (foc_encoder_init(&probe, encoder) != FOC_ENCODER_STATUS_OK) {
        return FOC_CAL_APPLICATION_ERR_CONFIG;
    }
    if (foc_cal_workflow_start(workflow, request) != FOC_CAL_WORKFLOW_OK) {
        return FOC_CAL_APPLICATION_ERR_WORKFLOW;
    }
    trial.boot_us = original->boot_us;
    *application = trial;
    return FOC_CAL_APPLICATION_OK;
}

int foc_cal_application_prepare(
    const struct foc_cal_application *application,
    struct foc_cal_application_candidate *candidate
)
{
    if (application == NULL || application->original == NULL || application->workflow == NULL ||
        candidate == NULL) {
        return FOC_CAL_APPLICATION_ERR_INPUT;
    }
    const int status = check_original(application);
    struct foc_encoder_lut lut;
    return status == FOC_CAL_APPLICATION_OK ? prepare_values(application, candidate, &lut) : status;
}

int foc_cal_application_apply(
    const struct foc_cal_application *application,
    struct foc_runtime *destination,
    struct foc_cal_application_sensor *sensor,
    uint32_t now_us
)
{
    if (application == NULL || application->original == NULL || application->workflow == NULL ||
        destination == NULL || destination == application->original || destination->initialized ||
        sensor == NULL || sensor->encoder.initialized) {
        return FOC_CAL_APPLICATION_ERR_INPUT;
    }
    const int bound = check_original(application);
    if (bound != FOC_CAL_APPLICATION_OK) {
        return bound;
    }
    struct foc_cal_application_candidate candidate;
    struct foc_encoder_lut lut;
    const int status = prepare_values(application, &candidate, &lut);
    if (status != FOC_CAL_APPLICATION_PREPARED) {
        return status;
    }
    struct foc_encoder_context encoder;
    const int encoder_status = candidate.has_lut ? foc_encoder_init_with_lut(
                                                       &encoder,
                                                       &candidate.encoder,
                                                       application->identity.encoder_id,
                                                       &lut
                                                   )
                                                 : foc_encoder_init(&encoder, &candidate.encoder);
    if (encoder_status != FOC_ENCODER_STATUS_OK ||
        foc_runtime_init(destination, &candidate.pipeline, &candidate.runtime.config, now_us) !=
            FOC_RUNTIME_OK) {
        return FOC_CAL_APPLICATION_ERR_CONFIG;
    }
    if (candidate.has_lut) {
        memcpy(sensor->lut_offsets, lut.offset_counts, lut.length * sizeof(sensor->lut_offsets[0]));
        encoder.lut_offsets = sensor->lut_offsets;
    }
    sensor->encoder = encoder;
    return FOC_CAL_APPLICATION_APPLIED;
}

/* ---------------------------------------- end of file ----------------------------------------- */
