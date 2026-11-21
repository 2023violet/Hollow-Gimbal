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
 * Portable C11 application boundary. Output remains behind the Pipeline Mock.
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

#include <float.h>
#include <math.h>
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

static int validate_initial(
    const struct foc_pipeline_config *pipeline,
    const struct foc_runtime_config *runtime,
    struct foc_parameter_candidate *candidate
)
{
    /* Read-only calibration fields belong to the qualified init base. Reassert
     * the existing outer current limit to validate the detached configuration. */
    const struct foc_parameter_edit limit = {
        .id = FOC_PARAMETER_MOTION_IQ_LIMIT,
        .value = {.type = FOC_PARAMETER_F32, .data = {.f32 = runtime->motion.iq_limit_a}},
    };
    return foc_parameter_prepare_initial(pipeline, runtime, &limit, 1U, candidate) ==
            FOC_PARAMETER_VALIDATED
        ? FOC_CAL_APPLICATION_OK
        : FOC_CAL_APPLICATION_ERR_CONFIG;
}

static float admitted_period(const struct foc_runtime_lane_config *lane)
{
    return (float)(((double)lane->period_us + lane->max_delay_us) * 1.0e-6);
}

static bool outer_tuning(
    const struct foc_cal_workflow_bundle *bundle,
    const struct foc_cal_application_tuning_policy *policy,
    struct foc_runtime_config *runtime,
    struct foc_cal_application_gains *gains
)
{
    const struct foc_mechanical_id_result *mechanical = &bundle->mechanical;
    const struct foc_impedance_tuning_candidate *impedance = &bundle->tuning;
    struct foc_runtime_motion_config *motion = &runtime->motion;
    const uint32_t stages = FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_MECHANICAL) |
        FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_TUNING);
    const double delay = policy->outer_extra_delay_s;
    const float period = admitted_period(&runtime->h1);
    if ((bundle->passed & stages) != stages || !mechanical->usable ||
        mechanical->setup_id != bundle->request.identity.motor_setup_id ||
        mechanical->capture_id == 0U ||
        mechanical->capture_id != bundle->stages[FOC_CAL_WORKFLOW_MECHANICAL].primary.id ||
        !bundle->kt.qualified || bundle->kt.capture == 0U ||
        bundle->kt.capture == mechanical->capture_id || bundle->kt.torque_model_id == 0U ||
        (mechanical->torque_source != FOC_MECHANICAL_ID_TORQUE_EQUIVALENT &&
         (bundle->kt.torque_model_id != mechanical->torque_model_id ||
          bundle->kt.nm_per_a != mechanical->torque_constant_nm_per_a)) ||
        !motion->impedance.enabled || !impedance->valid ||
        impedance->model_id != mechanical->torque_model_id ||
        !isfinite(impedance->stiffness_nm_per_rad) || impedance->stiffness_nm_per_rad <= 0.0f ||
        impedance->stiffness_nm_per_rad > motion->impedance.max_stiffness_nm_per_rad ||
        !isfinite(impedance->damping_nm_s_per_rad) || impedance->damping_nm_s_per_rad < 0.0f ||
        impedance->damping_nm_s_per_rad > motion->impedance.max_damping_nm_s_per_rad ||
        !isfinite(impedance->ideal_natural_frequency_rad_s) ||
        impedance->ideal_natural_frequency_rad_s <= 0.0f ||
        impedance->ideal_natural_frequency_rad_s * (period + delay) > 0.25) {
        return false;
    }

    if (!foc_cal_application_design_outer(
            policy,
            runtime,
            mechanical->inertia_kg_m2,
            mechanical->damping_nm_s,
            bundle->kt.nm_per_a,
            &motion->velocity,
            &motion->position
        )) {
        return false;
    }
    gains->velocity = motion->velocity;
    gains->position = motion->position;
    gains->impedance = *impedance;
    gains->mechanical_capture = mechanical->capture_id;
    gains->kt_capture = bundle->kt.capture;
    gains->torque_constant_nm_per_a = bundle->kt.nm_per_a;
    gains->outer_tuned = true;
    return true;
}

#if defined(FOC_RUNTIME_HOST_COGGING)
static bool capture_reserved(const struct foc_cal_workflow_bundle *bundle, uint32_t capture)
{
    if (capture == 0U || capture == bundle->kt.capture ||
        capture == bundle->electrical_q_capture.id) {
        return true;
    }
    for (unsigned int index = 0U; index < FOC_CAL_WORKFLOW_STAGE_COUNT; ++index) {
        if (capture == bundle->stages[index].primary.id ||
            capture == bundle->stages[index].holdout.id) {
            return true;
        }
    }
    return false;
}

static bool configure_cogging(
    const struct foc_cal_workflow_bundle *bundle,
    const struct foc_cal_application_tuning_policy *policy,
    struct foc_runtime_motion_config *motion,
    struct foc_cal_application_gains *gains
)
{
    const struct foc_runtime_cogging_lut *lut = policy->cogging_lut;
    if (lut == NULL) {
        return true;
    }
    if (!gains->outer_tuned || !lut->qualified ||
        lut->motor_id != bundle->request.identity.motor_setup_id ||
        lut->mechanical_frame_id != bundle->request.identity.mechanical_frame_id ||
        lut->training_capture_id == lut->holdout_capture_id ||
        capture_reserved(bundle, lut->training_capture_id) ||
        capture_reserved(bundle, lut->holdout_capture_id) ||
        !isfinite(policy->cogging_iq_limit_a) || policy->cogging_iq_limit_a <= 0.0f ||
        policy->cogging_iq_limit_a > motion->iq_limit_a) {
        return false;
    }
    for (unsigned int index = 0U; index < FOC_RUNTIME_COGGING_POINTS; ++index) {
        const float iq = lut->torque_nm[index] / gains->torque_constant_nm_per_a;
        if (!isfinite(iq) || fabsf(iq) > policy->cogging_iq_limit_a) {
            return false;
        }
    }
    gains->cogging = (struct foc_runtime_cogging_config){
        .enabled = true,
        .motor_id = lut->motor_id,
        .mechanical_frame_id = lut->mechanical_frame_id,
        .torque_constant_nm_per_a = gains->torque_constant_nm_per_a,
        .iq_limit_a = policy->cogging_iq_limit_a,
        .lut = *lut,
    };
    motion->cogging = gains->cogging;
    return true;
}

static bool cogging_matches(
    const struct foc_runtime_cogging_config *expected,
    const struct foc_runtime_cogging_config *approved
)
{
    return expected->enabled == approved->enabled && expected->motor_id == approved->motor_id &&
        expected->mechanical_frame_id == approved->mechanical_frame_id &&
        expected->torque_constant_nm_per_a == approved->torque_constant_nm_per_a &&
        expected->iq_limit_a == approved->iq_limit_a &&
        expected->lut.motor_id == approved->lut.motor_id &&
        expected->lut.mechanical_frame_id == approved->lut.mechanical_frame_id &&
        expected->lut.training_capture_id == approved->lut.training_capture_id &&
        expected->lut.holdout_capture_id == approved->lut.holdout_capture_id &&
        expected->lut.qualified == approved->lut.qualified &&
        memcmp(expected->lut.torque_nm, approved->lut.torque_nm, sizeof(expected->lut.torque_nm)) ==
        0;
}
#endif

static bool tuning(
    const struct foc_cal_workflow_bundle *bundle,
    const struct foc_cal_application_tuning_policy *policy,
    struct foc_pipeline_config *pipeline,
    struct foc_runtime_config *runtime,
    struct foc_cal_application_gains *gains
)
{
    const struct foc_current_config *current = &pipeline->current;
    const struct foc_config *pwm = &current->pwm;
    /* Use the guaranteed symmetric duty range at the minimum admitted bus. */
    const float duty_range = 2.0f * fminf(0.5f - pwm->duty_min, pwm->duty_max - 0.5f);
    const float voltage = fminf(pwm->voltage_limit_v, 0.577350269f * pwm->vbus_min_v * duty_range);
    const struct foc_motor_pi_request *pi = &policy->current;
    const struct foc_motor_id_result *d = &bundle->electrical;
    const struct foc_motor_id_result *q = &bundle->electrical_q;
    if (!bundle->request.require_dual_axis ||
        (bundle->passed & FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ELECTRICAL)) == 0U ||
        d->setup_id != bundle->request.identity.motor_setup_id || d->capture_id == 0U ||
        q->capture_id == 0U ||
        d->capture_id != bundle->stages[FOC_CAL_WORKFLOW_ELECTRICAL].primary.id ||
        q->capture_id != bundle->electrical_q_capture.id ||
        !isfinite(policy->max_rs_relative_difference) ||
        policy->max_rs_relative_difference > bundle->request.max_rs_relative_difference ||
        pi->sample_period_s < admitted_period(&runtime->h0) ||
        pi->sample_period_s > pwm->max_dt_s ||
        pi->update_delay_s <
            (float)(((double)runtime->h0.max_delay_us + runtime->h0.budget_us) * 1.0e-6) ||
        pi->initial_error_a > current->current_limit_a || pi->available_voltage_v > voltage ||
        foc_motor_id_make_model(d, q, policy->max_rs_relative_difference, &gains->model) !=
            FOC_MOTOR_ID_OK ||
        gains->model.rs_ohm != bundle->electrical_model.rs_ohm ||
        gains->model.ld_h != bundle->electrical_model.ld_h ||
        gains->model.lq_h != bundle->electrical_model.lq_h ||
        foc_motor_pi_design(&gains->model, pi, &gains->current) != FOC_MOTOR_ID_OK) {
        return false;
    }
    gains->identity = bundle->request.identity;
    gains->session = bundle->request.session;
    gains->d_capture = d->capture_id;
    gains->q_capture = q->capture_id;
    pipeline->current.kp_v_per_a =
        (struct foc_vector_dq){gains->current.kp_d_v_per_a, gains->current.kp_q_v_per_a};
    pipeline->current.ki_v_per_a_s =
        (struct foc_vector_dq){gains->current.ki_v_per_a_s, gains->current.ki_v_per_a_s};
    if (policy->tune_outer && !outer_tuning(bundle, policy, runtime, gains)) {
        return false;
    }
#if defined(FOC_RUNTIME_HOST_COGGING)
    return configure_cogging(bundle, policy, &runtime->motion, gains);
#else
    return true;
#endif
}

static bool gains_match(
    const struct foc_cal_application_gains *expected,
    const struct foc_cal_application_gains *approved
)
{
#if defined(FOC_RUNTIME_HOST_COGGING)
    if (!cogging_matches(&expected->cogging, &approved->cogging)) {
        return false;
    }
#endif
    return foc_cal_workflow_identity_matches(&expected->identity, &approved->identity) &&
        expected->session == approved->session && expected->d_capture == approved->d_capture &&
        expected->q_capture == approved->q_capture &&
        expected->mechanical_capture == approved->mechanical_capture &&
        expected->kt_capture == approved->kt_capture &&
        expected->outer_tuned == approved->outer_tuned &&
        expected->model.rs_ohm == approved->model.rs_ohm &&
        expected->model.ld_h == approved->model.ld_h &&
        expected->model.lq_h == approved->model.lq_h &&
        expected->current.kp_d_v_per_a == approved->current.kp_d_v_per_a &&
        expected->current.kp_q_v_per_a == approved->current.kp_q_v_per_a &&
        expected->current.ki_v_per_a_s == approved->current.ki_v_per_a_s &&
        expected->current.estimated_initial_voltage_v ==
        approved->current.estimated_initial_voltage_v &&
        expected->current.estimated_delay_phase_rad ==
        approved->current.estimated_delay_phase_rad &&
        expected->velocity.kp_a_per_rad_s == approved->velocity.kp_a_per_rad_s &&
        expected->velocity.ki_a_per_rad == approved->velocity.ki_a_per_rad &&
        expected->velocity.max_dt_s == approved->velocity.max_dt_s &&
        expected->position.kp_per_s == approved->position.kp_per_s &&
        expected->position.velocity_limit_rad_s == approved->position.velocity_limit_rad_s &&
        expected->impedance.model_id == approved->impedance.model_id &&
        expected->impedance.damping_source == approved->impedance.damping_source &&
        expected->impedance.stiffness_nm_per_rad == approved->impedance.stiffness_nm_per_rad &&
        expected->impedance.damping_nm_s_per_rad == approved->impedance.damping_nm_s_per_rad &&
        expected->impedance.ideal_natural_frequency_rad_s ==
        approved->impedance.ideal_natural_frequency_rad_s &&
        expected->impedance.ideal_damping_ratio == approved->impedance.ideal_damping_ratio &&
        expected->impedance.current_limited_torque_nm ==
        approved->impedance.current_limited_torque_nm &&
        expected->impedance.valid == approved->impedance.valid &&
        expected->torque_constant_nm_per_a == approved->torque_constant_nm_per_a;
}

static int prepare_values(
    const struct foc_cal_application *application,
    const struct foc_cal_application_tuning_policy *tuning_policy,
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
#if defined(FOC_RUNTIME_HOST_COGGING)
    base.motion.cogging = (struct foc_runtime_cogging_config){0};
#endif
    base.motion.rotor.pole_pairs = snapshot->pole_pairs;
    base.motion.rotor.electrical_offset_rad = snapshot->electrical_offset_rad;
    if (base.motion.impedance.enabled && bundle->kt.qualified) {
        if (!foc_cal_workflow_identity_matches(&application->identity, &bundle->kt.identity)) {
            return FOC_CAL_APPLICATION_ERR_IDENTITY;
        }
        base.motion.impedance.torque_constant_nm_per_a = bundle->kt.nm_per_a;
    }

    if ((tuning_policy == NULL && bundle->request.require_dual_axis) ||
        (tuning_policy != NULL &&
         !tuning(bundle, tuning_policy, &trial.pipeline, &base, &trial.gains))) {
        return FOC_CAL_APPLICATION_ERR_TUNING;
    }
    const int configuration = validate_initial(&trial.pipeline, &base, &trial.runtime);
    if (configuration != FOC_CAL_APPLICATION_OK) {
        return configuration;
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

static int initialize_candidate(
    const struct foc_cal_application_candidate *candidate,
    uint32_t encoder_id,
    const struct foc_encoder_lut *lut,
    const struct foc_cal_application_output *output,
    struct foc_runtime *destination,
    struct foc_cal_application_sensor *sensor,
    uint32_t now_us
)
{
    struct foc_encoder_context encoder;
    const int encoder_status = lut != NULL
        ? foc_encoder_init_with_lut(&encoder, &candidate->encoder, encoder_id, lut)
        : foc_encoder_init(&encoder, &candidate->encoder);
    if (encoder_status != FOC_ENCODER_STATUS_OK) {
        return lut != NULL ? FOC_CAL_APPLICATION_ERR_LUT : FOC_CAL_APPLICATION_ERR_CONFIG;
    }
    struct foc_pipeline_config pipeline = candidate->pipeline;
    if (output != NULL) {
        /* Validate all Runtime settings before the first external INHIBIT.
         * Native Runtime initialization preserves destination on rejection. */
        struct foc_runtime probe;
        if (foc_runtime_init(&probe, &pipeline, &candidate->runtime.config, now_us) !=
            FOC_RUNTIME_OK) {
            return FOC_CAL_APPLICATION_ERR_CONFIG;
        }
#if defined(FOC_RUNTIME_HOST_COGGING)
        if (candidate->runtime.config.motion.cogging.enabled &&
            !probe.config.motion.cogging.enabled) {
            return FOC_CAL_APPLICATION_ERR_CONFIG;
        }
#endif
        pipeline.output_submit = output->submit;
        pipeline.output_arg = output->argument;
    }
    if (foc_runtime_init(destination, &pipeline, &candidate->runtime.config, now_us) !=
        FOC_RUNTIME_OK) {
        return FOC_CAL_APPLICATION_ERR_CONFIG;
    }
    if (lut != NULL) {
        memcpy(
            sensor->lut_offsets,
            lut->offset_counts,
            lut->length * sizeof(sensor->lut_offsets[0])
        );
        encoder.lut_offsets = sensor->lut_offsets;
    }
    sensor->encoder = encoder;
    return FOC_CAL_APPLICATION_APPLIED;
}

struct application_initial_output {
    const struct foc_cal_application *application;
    const struct foc_cal_application_output *output;
    int status;
};

static bool confirm_initial_output(void *argument, const struct foc_device_pwm_request *request)
{
    struct application_initial_output *guard = argument;
    guard->status = check_original(guard->application);
    if (guard->status != FOC_CAL_APPLICATION_OK) {
        return false;
    }
    const bool confirmed = guard->output->submit(guard->output->argument, request);
    guard->status = check_original(guard->application);
    return confirmed && guard->status == FOC_CAL_APPLICATION_OK;
}

static int apply(
    const struct foc_cal_application *application,
    const struct foc_cal_application_tuning_policy *policy,
    const struct foc_cal_application_gains *approved_gains,
    const struct foc_cal_application_output *output,
    struct foc_runtime *destination,
    struct foc_cal_application_sensor *sensor,
    uint32_t now_us
)
{
    if (application == NULL || application->original == NULL || application->workflow == NULL ||
        destination == NULL || destination == application->original || destination->initialized ||
        sensor == NULL || sensor->encoder.initialized ||
        ((policy == NULL) != (approved_gains == NULL))) {
        return FOC_CAL_APPLICATION_ERR_INPUT;
    }
    const int bound = check_original(application);
    if (bound != FOC_CAL_APPLICATION_OK) {
        return bound;
    }
    struct foc_cal_application_candidate candidate;
    struct foc_encoder_lut lut;
    const int status = prepare_values(application, policy, &candidate, &lut);
    if (status != FOC_CAL_APPLICATION_PREPARED) {
        return status;
    }
    if (approved_gains != NULL && !gains_match(&candidate.gains, approved_gains)) {
        return FOC_CAL_APPLICATION_ERR_REVIEW;
    }
    struct application_initial_output guard = {application, output, FOC_CAL_APPLICATION_OK};
    const struct foc_cal_application_output guarded = {confirm_initial_output, &guard};
    const int initialized = initialize_candidate(
        &candidate,
        application->identity.encoder_id,
        candidate.has_lut ? &lut : NULL,
        output != NULL ? &guarded : NULL,
        destination,
        sensor,
        now_us
    );
    if (guard.status != FOC_CAL_APPLICATION_OK) {
        return guard.status;
    }
    if (initialized == FOC_CAL_APPLICATION_APPLIED && output != NULL) {
        /* Only the first INHIBIT uses the temporary guard. */
        destination->pipeline.output_submit = output->submit;
        destination->pipeline.output_arg = output->argument;
    }
    return initialized;
}

/* Public Functions ------------------------------------------------------------- */

bool foc_cal_application_design_outer(
    const struct foc_cal_application_tuning_policy *policy,
    const struct foc_runtime_config *runtime,
    float inertia_kg_m2,
    float damping_nm_s,
    float kt_nm_per_a,
    struct foc_velocity_config *velocity,
    struct foc_position_config *position
)
{
    if (policy == NULL || runtime == NULL || velocity == NULL || position == NULL) {
        return false;
    }
    const double omega = policy->velocity_natural_frequency_rad_s;
    const double zeta = policy->velocity_damping_ratio;
    const double position_bandwidth = policy->position_bandwidth_rad_s;
    const float period = admitted_period(&runtime->h1);
    const double delay = policy->outer_extra_delay_s;
    const struct foc_runtime_motion_config *motion = &runtime->motion;
    const double inertia = inertia_kg_m2;
    const double damping = damping_nm_s;
    const double kt = kt_nm_per_a;
    if (!isfinite(omega) || omega <= 0.0 || !isfinite(policy->current.bandwidth_hz) ||
        omega > 0.2 * 6.283185307179586 * policy->current.bandwidth_hz || !isfinite(zeta) ||
        zeta <= 0.0 || !isfinite(position_bandwidth) || position_bandwidth <= 0.0 ||
        position_bandwidth > 0.2 * omega || !isfinite(delay) || delay < 0.0 || period <= 0.0f ||
        !isfinite(motion->velocity.max_dt_s) || period > motion->velocity.max_dt_s ||
        omega * (period + delay) > 0.25 || !isfinite(inertia) || inertia <= 0.0 ||
        !isfinite(damping) || damping < 0.0 || !isfinite(kt) || kt <= 0.0) {
        return false;
    }
    /* The ideal velocity characteristic polynomial is J*s*s + (B + Kt*Kp)*s + Kt*Ki. */
    const double kp = (2.0 * zeta * omega * inertia - damping) / kt;
    const double ki = omega * omega * inertia / kt;
    if (kp < 0.0 || kp > FLT_MAX || ki < FLT_MIN || ki > FLT_MAX) {
        return false;
    }
    struct foc_velocity_config next_velocity = motion->velocity;
    struct foc_position_config next_position = motion->position;
    next_velocity.kp_a_per_rad_s = (float)kp;
    next_velocity.ki_a_per_rad = (float)ki;
    next_position.kp_per_s = policy->position_bandwidth_rad_s;
    *velocity = next_velocity;
    *position = next_position;
    return true;
}

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
    return foc_cal_application_prepare_tuned(application, NULL, candidate);
}

int foc_cal_application_apply(
    const struct foc_cal_application *application,
    struct foc_runtime *destination,
    struct foc_cal_application_sensor *sensor,
    uint32_t now_us
)
{
    return apply(application, NULL, NULL, NULL, destination, sensor, now_us);
}

int foc_cal_application_prepare_tuned(
    const struct foc_cal_application *application,
    const struct foc_cal_application_tuning_policy *policy,
    struct foc_cal_application_candidate *candidate
)
{
    if (application == NULL || application->original == NULL || application->workflow == NULL ||
        candidate == NULL) {
        return FOC_CAL_APPLICATION_ERR_INPUT;
    }
    const int status = check_original(application);
    struct foc_encoder_lut lut;
    return status == FOC_CAL_APPLICATION_OK ? prepare_values(application, policy, candidate, &lut)
                                            : status;
}

int foc_cal_application_apply_with_output(
    const struct foc_cal_application *application,
    const struct foc_cal_application_tuning_policy *policy,
    const struct foc_cal_application_gains *approved_gains,
    const struct foc_cal_application_output *output,
    struct foc_runtime *destination,
    struct foc_cal_application_sensor *sensor,
    uint32_t now_us
)
{
    if (application == NULL || application->original == NULL ||
        !application->original->initialized || output == NULL || output->submit == NULL ||
        (output->submit == application->original->pipeline.output_submit &&
         output->argument == application->original->pipeline.output_arg)) {
        return FOC_CAL_APPLICATION_ERR_INPUT;
    }
    return apply(application, policy, approved_gains, output, destination, sensor, now_us);
}

int foc_cal_application_restore(
    const struct foc_cal_application_candidate *candidate,
    uint32_t encoder_id,
    const struct foc_encoder_lut *lut,
    const struct foc_cal_application_output *output,
    struct foc_runtime *destination,
    struct foc_cal_application_sensor *sensor,
    uint32_t now_us
)
{
    if (candidate == NULL || encoder_id == 0U || candidate->has_lut != (lut != NULL) ||
        candidate->pipeline.output_submit != NULL || candidate->pipeline.output_arg != NULL ||
        candidate->runtime.config.hook != NULL || candidate->runtime.config.hook_arg != NULL ||
        output == NULL || output->submit == NULL || destination == NULL ||
        destination->initialized || sensor == NULL || sensor->encoder.initialized) {
        return FOC_CAL_APPLICATION_ERR_INPUT;
    }
    if (lut != NULL && lut->length > FOC_ENCODER_CAL_MAX_NODES) {
        return FOC_CAL_APPLICATION_ERR_LUT;
    }
    struct foc_parameter_candidate checked;
    const int configuration =
        validate_initial(&candidate->pipeline, &candidate->runtime.config, &checked);
    if (configuration != FOC_CAL_APPLICATION_OK) {
        return configuration;
    }
    return initialize_candidate(candidate, encoder_id, lut, output, destination, sensor, now_us);
}

/* ---------------------------------------- end of file ----------------------------------------- */
