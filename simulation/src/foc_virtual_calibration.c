/**
 * @file    foc_virtual_calibration.c
 * @brief   Own Foreground excitation, acquisition and shutdown for Host calibration.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * All sources are observations of an evolving Virtual Motor. The calibrated
 * estimators live in the existing Workflow, including its source and identity
 * checks. Ideal virtual reference instruments are dedicated to this fixture.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

static const double FOC_CAL_TAU = 6.28318530717958647692;
static const uint32_t FOC_CAL_DEADLINE_US = 12000000U;
static const uint32_t FOC_CAL_REFERENCE_ID = 401U;
static const uint32_t FOC_CAL_FLUX_CAPTURE = 10U;

/* Private Helpers -------------------------------------------------------------- */

static double wrapped(double angle)
{
    const double result = fmod(angle, FOC_CAL_TAU);
    return result < 0.0 ? result + FOC_CAL_TAU : result;
}

static int
observe(const struct foc_virtual_calibration *session, struct foc_virtual_motor_observation *data)
{
    return foc_virtual_motor_observe(&session->motor, data);
}

static int plant_reset(struct foc_virtual_calibration *session)
{
    return foc_virtual_motor_init(&session->motor, &session->profile.motor, NULL);
}

static bool capture_is_exclusive(struct foc_virtual_calibration *session)
{
    struct foc_virtual_current *original = &session->original;
    const struct foc_cal_application *binding = &session->application;
    const struct foc_cal_workflow_request *request = &session->workflow.result.request;
    const struct foc_runtime_snapshot *bound = &binding->original_view.snapshot;
    struct foc_runtime_lifecycle_result lifecycle;
    struct foc_runtime_snapshot snapshot;
    return binding->session == request->session && binding->boot_us == original->runtime.boot_us &&
        foc_cal_workflow_identity_matches(&binding->identity, &request->identity) &&
        foc_runtime_read_lifecycle_result(&original->runtime, &lifecycle) == FOC_RUNTIME_OK &&
        lifecycle.result != FOC_RUNTIME_REQUEST_PENDING &&
        foc_virtual_current_snapshot(original, &snapshot) == FOC_RUNTIME_OK &&
        snapshot.state == FOC_SAFE_STATE_DISABLED && snapshot.faults == 0U &&
        snapshot.output_confirmed && snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        !snapshot.inhibit_pending && snapshot.clear_result != FOC_RUNTIME_CLEAR_PENDING &&
        snapshot.motion_session == bound->motion_session && snapshot.mode == bound->mode &&
        snapshot.updates == bound->updates;
}

static bool capture_permitted(void *argument, uint32_t aperture_us)
{
    struct foc_virtual_calibration *session = argument;
    if (aperture_us > FOC_CAL_DEADLINE_US) {
        session->deadline_exceeded = true;
        return false;
    }
    return capture_is_exclusive(session);
}

static void halt_experiment(struct foc_virtual_motor *motor)
{
    if (foc_virtual_motor_set_fixture_speed(motor, 0.0) == 0) {
        (void)foc_virtual_motor_step(motor, 0.0, 0.0, 0.0002);
    }
    foc_virtual_motor_release_fixture(motor);
}

static struct foc_cal_workflow_stage_request capture(enum foc_cal_workflow_stage stage, uint32_t id)
{
    return (struct foc_cal_workflow_stage_request){
        .stage = stage,
        .source = FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT,
        .capture = id,
        .max_age_us = 200U,
    };
}

static struct foc_cal_workflow_sample sample_for(
    const struct foc_virtual_calibration *session,
    uint32_t id,
    enum foc_cal_workflow_units units
)
{
    const struct foc_cal_workflow_request *request = &session->workflow.result.request;
    return (struct foc_cal_workflow_sample){
        .session = request->session,
        .capture = id,
        .identity = request->identity,
        .source = FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT,
        .units = units,
        .valid = true,
    };
}

static int adc_zero(struct foc_virtual_calibration *session)
{
    struct foc_cal_workflow_stage_request request = capture(FOC_CAL_WORKFLOW_ADC, 1U);
    request.policy.adc = (struct foc_calibration_adc_config){
        .identity = session->workflow.result.request.identity.acquisition,
        .required_valid_mask = 7U,
        .minimum_samples = 48U,
        .maximum_samples = 48U,
        .maximum_sample_step_counts = 6.0f,
        .maximum_noise_rms_counts = 2.0f,
        .minimum_offset_counts = 32.0f,
        .maximum_offset_counts = 4063.0f,
        .maximum_interval_us = 250U,
    };
    int status = foc_cal_workflow_open(&session->workflow, &request);
    for (uint32_t i = 0U; i < 48U && status == 0; ++i) {
        struct foc_virtual_motor_observation actual;
        struct foc_device_adc_raw raw;
        if (!capture_permitted(session, session->time_us + 200U)) {
            return FOC_CAL_WORKFLOW_ERR_STATE;
        }
        session->time_us += 200U;
        if (foc_virtual_motor_step(&session->motor, 0.0, 0.0, 0.0002) != 0 ||
            observe(session, &actual) != 0 ||
            foc_virtual_current_acquire_adc(
                &session->profile,
                &session->motor,
                i,
                session->time_us,
                &raw
            ) != 0) {
            return FOC_CAL_WORKFLOW_ERR_QUALITY;
        }
        const bool abc = session->profile.adc.phase_order == FOC_DEVICE_PHASE_ORDER_ABC;
        struct foc_cal_workflow_sample sample =
            sample_for(session, request.capture, FOC_CAL_WORKFLOW_RAW_COUNTS);
        sample.data.adc = (struct foc_calibration_adc_sample){
            .phase_a = raw.rank2_raw,
            .phase_b = abc ? raw.rank3_raw : raw.rank4_raw,
            .phase_c = abc ? raw.rank4_raw : raw.rank3_raw,
            .valid_mask = raw.valid_mask == FOC_DEVICE_SAMPLE_ALL_VALID ? 7U : 0U,
            .sequence = i,
            .captured_us = raw.sample_tick,
            .zero_current_confirmed = hypot(actual.id_a, actual.iq_a) < 1.0e-6 &&
                fabs(actual.mechanical_velocity_rad_s) < 1.0e-6,
        };
        status = foc_cal_workflow_push(&session->workflow, &sample, session->time_us);
    }
    return status == 0 ? foc_cal_workflow_finish_stage(&session->workflow) : status;
}

static int move(struct foc_virtual_calibration *session, uint32_t elapsed_us)
{
    /* The virtual fixture holds the requested shaft speed through each RK4 stage. */
    for (uint32_t i = 0U; i < elapsed_us; i += 200U) {
        if (!capture_permitted(session, session->time_us + i + 200U) ||
            foc_virtual_motor_step(&session->motor, 0.0, 0.0, 0.0002) != 0) {
            return -1;
        }
    }
    session->time_us += elapsed_us;
    return 0;
}

static int lut_samples(struct foc_virtual_calibration *session, uint32_t id, bool holdout)
{
    const struct foc_cal_workflow_identity *identity = &session->workflow.result.request.identity;
    const uint32_t interval = holdout ? 1800U : 2000U;
    const double speed = holdout ? -7.25 : 6.4;
    if (foc_virtual_motor_set_fixture_speed(&session->motor, speed) != 0) {
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    int status = 0;
    for (uint32_t i = 0U; i < 512U && status == 0; ++i) {
        uint16_t code;
        struct foc_virtual_motor_observation origin;
        if (move(session, interval) != 0 || observe(session, &origin) != 0 ||
            foc_virtual_current_acquire_encoder(&session->profile, &session->motor, &code) != 0) {
            status = FOC_CAL_WORKFLOW_ERR_QUALITY;
            break;
        }
        struct foc_cal_workflow_sample sample =
            sample_for(session, id, FOC_CAL_WORKFLOW_ENCODER_COUNTS_RAD);
        sample.data.lut = (struct foc_encoder_cal_sample){
            .setup =
                {
                    .encoder_identity = identity->encoder_id,
                    .reference_identity = FOC_CAL_REFERENCE_ID,
                    .counts_per_revolution = session->profile.encoder.counts_per_revolution,
                    .direction = session->profile.encoder.direction,
                    .source = FOC_ENCODER_CAL_SOURCE_VIRTUAL,
                },
            .capture_id = id,
            .sequence = i,
            .encoder_us = session->time_us,
            .reference_us = session->time_us,
            .raw_code = code,
            .reference_rad = wrapped(origin.mechanical_position_rad),
            .encoder_valid = true,
            .reference_valid = origin.fixture_active,
            .reference_confirmed = origin.fixture_active,
        };
        status = foc_cal_workflow_push(&session->workflow, &sample, session->time_us);
    }
    foc_virtual_motor_release_fixture(&session->motor);
    return status;
}

static int encoder_lut(struct foc_virtual_calibration *session)
{
    struct foc_cal_workflow_stage_request request = capture(FOC_CAL_WORKFLOW_LUT, 2U);
    request.policy.lut = (struct foc_encoder_cal_config){
        .encoder = session->profile.encoder,
        .encoder_identity = session->workflow.result.request.identity.encoder_id,
        .reference_identity = FOC_CAL_REFERENCE_ID,
        .source = FOC_ENCODER_CAL_SOURCE_VIRTUAL,
        .nodes = 32U,
        .min_samples_per_segment = 8U,
        .reference_zero_rad = 0.0,
        .max_zero_bias_rad = 0.015,
        .max_training_segment_rms_rad = 0.002,
        .max_holdout_rms_rad = 0.002,
        .max_holdout_peak_rad = 0.003,
    };
    int status = foc_cal_workflow_open(&session->workflow, &request);
    if (status == 0) {
        status = lut_samples(session, request.capture, false);
    }
    if (status == 0) {
        status = foc_cal_workflow_begin_holdout(&session->workflow, request.capture + 1U);
    }
    if (status == 0) {
        status = lut_samples(session, request.capture + 1U, true);
    }
    return status == 0 ? foc_cal_workflow_finish_stage(&session->workflow) : status;
}

static int angles(struct foc_virtual_calibration *session, bool rotor)
{
    const struct foc_calibration_identity identity =
        session->workflow.result.request.identity.acquisition;
    struct foc_cal_workflow_stage_request request =
        capture(rotor ? FOC_CAL_WORKFLOW_ROTOR : FOC_CAL_WORKFLOW_OFFSET, rotor ? 4U : 5U);
    if (rotor) {
        request.policy.rotor = (struct foc_calibration_rotor_config){
            .identity = identity,
            .minimum_pole_pairs = 1U,
            .maximum_pole_pairs = 14U,
            .minimum_mechanical_span_rad = 0.4f,
            .maximum_residual_rad = 0.025f,
            .minimum_candidate_separation_rad = 0.08f,
            .maximum_interval_us = 2400U,
            .maximum_samples = 48U,
        };
    } else {
        request.policy.offset = (struct foc_calibration_offset_config){
            .identity = identity,
            .maximum_residual_rad = 0.025f,
            .maximum_interval_us = 2400U,
            .maximum_samples = 48U,
        };
    }
    int status = foc_cal_workflow_open(&session->workflow, &request);
    if (status != 0 ||
        foc_virtual_motor_set_fixture_speed(&session->motor, rotor ? 8.0 : -6.0) != 0) {
        return status != 0 ? status : FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED;
    }
    for (uint32_t i = 0U; i < 48U && status == 0; ++i) {
        uint16_t code;
        struct foc_virtual_motor_observation origin;
        if (move(session, 2000U) != 0 || observe(session, &origin) != 0 || !origin.fixture_active ||
            foc_virtual_current_acquire_encoder(&session->profile, &session->motor, &code) != 0) {
            status = FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED;
            break;
        }
        struct foc_cal_workflow_sample sample =
            sample_for(session, request.capture, FOC_CAL_WORKFLOW_RAD_PAIR);
        sample.encoder_raw_valid = true;
        sample.encoder_raw_code = code;
        sample.data.angle = (struct foc_calibration_angle_sample){
            .mechanical_rad = (float)origin.mechanical_position_rad,
            .electrical_rad = (float)wrapped(origin.electrical_position_rad),
            .captured_us = session->time_us,
            .sequence = i,
            .valid = true,
        };
        status = foc_cal_workflow_push(&session->workflow, &sample, session->time_us);
    }
    foc_virtual_motor_release_fixture(&session->motor);
    return status == 0 ? foc_cal_workflow_finish_stage(&session->workflow) : status;
}

static int electrical_axis(struct foc_virtual_calibration *session, enum foc_motor_id_axis axis)
{
    struct foc_virtual_motor_observation reference;
    if (plant_reset(session) != 0 || observe(session, &reference) != 0) {
        return FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED;
    }
    const struct foc_calibration_adc_result *adc = &session->workflow.result.adc;
    session->electrical.decoded_adc.phase_a_offset_count = adc->offset_a_counts;
    session->electrical.decoded_adc.phase_b_offset_count = adc->offset_b_counts;
    session->electrical.decoded_adc.phase_c_offset_count = adc->offset_c_counts;
    const int status = foc_virtual_calibrate_rl_axis(
        &session->workflow,
        &session->motor,
        &session->electrical,
        axis,
        axis == FOC_MOTOR_ID_D ? 6U : 7U,
        session->time_us + 200U,
        reference.electrical_position_rad
    );
    const uint32_t last = axis == FOC_MOTOR_ID_D ? session->workflow.result.electrical.last_us
                                                 : session->workflow.result.electrical_q.last_us;
    if (last > session->time_us) {
        session->time_us = last;
    }
    return axis == FOC_MOTOR_ID_D && status == FOC_CAL_WORKFLOW_PARTIAL ? 0 : status;
}

static int flux(struct foc_virtual_calibration *session)
{
    if (plant_reset(session) != 0) {
        return FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED;
    }
    const int status = foc_virtual_calibrate_flux(
        &session->motor,
        &session->electrical,
        &session->workflow.result,
        FOC_CAL_FLUX_CAPTURE,
        session->time_us + 200U,
        &session->flux
    );
    if (status == 0) {
        session->time_us = session->flux.last_us;
    }
    return status;
}

static int mechanics(struct foc_virtual_calibration *session)
{
    const struct foc_cal_workflow_kt *kt = &session->flux.kt;
    if (!session->flux.qualified || !kt->qualified || kt->capture != FOC_CAL_FLUX_CAPTURE ||
        kt->source != FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT || plant_reset(session) != 0) {
        return FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED;
    }
    if (!foc_cal_workflow_identity_matches(
            &kt->identity,
            &session->workflow.result.request.identity
        )) {
        return FOC_CAL_WORKFLOW_ERR_IDENTITY;
    }
    struct foc_cal_workflow_stage_request request = capture(FOC_CAL_WORKFLOW_MECHANICAL, 11U);
    request.policy.mechanical = (struct foc_mechanical_id_config){
        .setup_id = kt->identity.motor_setup_id,
        .torque_model_id = kt->torque_model_id,
        .torque_source = FOC_MECHANICAL_ID_TORQUE_EQUIVALENT,
        .load_source = FOC_MECHANICAL_ID_LOAD_OBSERVED,
        .min_interval_us = 200U,
        .max_interval_us = 200U,
        .intervals_per_window = 50U,
        .min_windows = 40U,
        .torque_constant_nm_per_a = kt->nm_per_a,
        .speed_resolution_rad_s = 0.001f,
        .max_abs_speed_rad_s = 40.0f,
        .max_abs_torque_nm = 2.0f,
        .min_accel_rms_rad_s2 = 0.5f,
        .min_speed_rms_rad_s = 0.5f,
        .min_excitation = 0.1f,
        .max_residual_rms_nm = 0.001f,
        .min_inertia_kg_m2 = 0.0005f,
        .max_inertia_kg_m2 = 0.1f,
        .max_damping_nm_s = 0.1f,
    };
    int status = foc_cal_workflow_open(&session->workflow, &request);
    session->time_us += 200U;
    for (uint32_t i = 0U; i <= 8000U && status == 0; ++i) {
        if (!capture_permitted(session, session->time_us + (i == 0U ? 0U : 200U))) {
            return FOC_CAL_WORKFLOW_ERR_STATE;
        }
        struct foc_virtual_motor_observation actual;
        if (observe(session, &actual) != 0 || actual.fixture_active) {
            return FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED;
        }
        if (i != 0U) {
            const double t_s = (double)i * 0.0002;
            const double vq =
                1.45 * sin(FOC_CAL_TAU * 2.1 * t_s) + 0.4 * sin(FOC_CAL_TAU * 5.3 * t_s);
            /* Hold a varying load plus smooth Coulomb friction for this interval.
             * The fit consumes the observed load channel, separate from viscous B. */
            const double load = 0.015 * sin(FOC_CAL_TAU * 1.3 * t_s) +
                0.008 * tanh(actual.mechanical_velocity_rad_s / 0.35);
            if (foc_virtual_motor_set_load(&session->motor, load) != 0 ||
                foc_virtual_motor_step(
                    &session->motor,
                    -vq * sin(actual.electrical_position_rad),
                    vq * cos(actual.electrical_position_rad),
                    0.0002
                ) != 0 ||
                observe(session, &actual) != 0) {
                return FOC_CAL_WORKFLOW_ERR_QUALITY;
            }
            session->time_us += 200U;
        }
        struct foc_cal_workflow_sample sample =
            sample_for(session, request.capture, FOC_CAL_WORKFLOW_MECHANICAL_SI);
        sample.output_confirmed = true;
        /* Deterministic one-count, zero-mean noise on the quantized speed witness. */
        const double speed_counts =
            round(actual.mechanical_velocity_rad_s * 1000.0) + (double)((i * 5U) % 3U) - 1.0;
        sample.data.mechanical = (struct foc_mechanical_id_sample){
            .setup_id = kt->identity.motor_setup_id,
            .torque_model_id = kt->torque_model_id,
            .capture_id = request.capture,
            .torque_source = FOC_MECHANICAL_ID_TORQUE_EQUIVALENT,
            .load_source = FOC_MECHANICAL_ID_LOAD_OBSERVED,
            .sequence = i,
            .speed_us = session->time_us,
            .torque_us = session->time_us,
            .interval_begin_us = i == 0U ? session->time_us : session->time_us - 200U,
            .speed_rad_s = (float)(speed_counts * 0.001),
            .torque_nm = (float)actual.electromagnetic_torque_nm,
            .load_nm = (float)actual.applied_load_torque_nm,
            .speed_valid = true,
            .torque_valid = true,
            .frame_confirmed = true,
            .interval_confirmed = true,
        };
        status = foc_cal_workflow_push(&session->workflow, &sample, session->time_us);
    }
    return status == 0 ? foc_cal_workflow_finish_stage(&session->workflow) : status;
}

static int tune(struct foc_virtual_calibration *session)
{
    const struct foc_impedance_tuning_input demand =
        foc_virtual_calibration_impedance_policy(&session->profile);
    const int status = foc_cal_workflow_tune(&session->workflow, &session->flux.kt, &demand);
    return status == 0 ? foc_cal_workflow_finish(&session->workflow) : status;
}

/* Public Functions ------------------------------------------------------------- */

struct foc_impedance_tuning_input foc_virtual_calibration_impedance_policy(
    const struct foc_virtual_current_profile *profile
)
{
    const struct foc_runtime_impedance_config *impedance = &profile->runtime.motion.impedance;
    return (struct foc_impedance_tuning_input){
        .target_natural_frequency_rad_s = 15.0f,
        .target_damping_ratio = 0.8f,
        .max_stiffness_nm_per_rad = impedance->max_stiffness_nm_per_rad,
        .max_damping_nm_s_per_rad = impedance->max_damping_nm_s_per_rad,
        .iq_limit_a = profile->runtime.motion.iq_limit_a,
        .sample_period_s = 0.0005f,
        .extra_delay_s = 0.0006f,
    };
}

int foc_virtual_calibration_profile(
    const char *name,
    struct foc_virtual_current_profile *profile,
    struct foc_cal_workflow_request *request
)
{
    if (name == NULL || profile == NULL || request == NULL) {
        return FOC_VIRTUAL_CAL_ERR_SETUP;
    }
    const bool salient = strcmp(name, "CALIBRATION_SALIENT_MOTOR") == 0;
    if (!salient && strcmp(name, "GENERIC_RESEARCH_MOTOR") != 0) {
        return FOC_VIRTUAL_CAL_ERR_SETUP;
    }
    *profile = foc_virtual_current_default_profile();
    profile->adc.phase_a_offset_count = 2090.0f;
    profile->adc.phase_b_offset_count = 2030.0f;
    profile->adc.phase_c_offset_count = 2024.0f;
    profile->adc_noise_counts = 1U;
    profile->signed_mechanical_origin = true;
    profile->encoder_fundamental_error_rad = 0.012;
    profile->encoder_third_error_rad = -0.004;
    if (salient) {
        profile->motor.rs_ohm = 0.78;
        profile->motor.ld_h = 0.0017;
        profile->motor.lq_h = 0.0022;
        profile->motor.psi_f_wb = 0.032;
        profile->motor.inertia_kg_m2 = 0.0032;
        profile->motor.damping_nm_s = 0.009;
        profile->motor.electrical_offset_rad = 0.52;
        profile->encoder.direction = FOC_ENCODER_DIRECTION_REVERSED;
        profile->adc.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
        profile->pwm.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
    }
    profile->runtime.motion.rotor.electrical_offset_rad = 0.0f;
    profile->runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = 310U,
        .torque_constant_nm_per_a = 0.13f,
        .torque_limit_nm = 0.33f,
        .max_stiffness_nm_per_rad = 3.0f,
        .max_damping_nm_s_per_rad = 0.3f,
        .max_feedforward_nm = 0.1f,
    };
    *request = (struct foc_cal_workflow_request){
        .session = 1U,
        .profile = FOC_CAL_WORKFLOW_FULL_PROFILE,
        .identity =
            {
                .acquisition =
                    {
                        .adc_full_scale_counts = 4095U,
                        .current_a_per_count = profile->adc.current_a_per_count,
                        .encoder_counts_per_revolution = profile->encoder.counts_per_revolution,
                        .encoder_direction = profile->encoder.direction,
                        .phase_order = profile->adc.phase_order,
                        /* Profile-declared expectation, tested against the independent reference. */
                        .pole_pairs = 4U,
                    },
                .motor_setup_id = salient ? 202U : 201U,
                .encoder_id = salient ? 102U : 101U,
                .mechanical_frame_id = 310U,
            },
        .admission =
            {
                .minimum_adc_samples = 48U,
                .minimum_offset_counts = 32.0f,
                .maximum_offset_counts = 4063.0f,
                .maximum_noise_rms_counts = 2.0f,
                .maximum_rotor_residual_rad = 0.025f,
                .maximum_offset_residual_rad = 0.025f,
            },
        .require_dual_axis = true,
        .max_rs_relative_difference = 0.05f,
    };
    return FOC_VIRTUAL_CAL_OK;
}

int foc_virtual_calibration_baseline(
    void *argument,
    struct foc_runtime *runtime,
    struct foc_encoder_context *encoder,
    const struct foc_pipeline_config *pipeline,
    const struct foc_virtual_current_profile *profile
)
{
    (void)argument;
    struct foc_pipeline_config guess = *pipeline;
    guess.adc.phase_a_offset_count = 2048.0f;
    guess.adc.phase_b_offset_count = 2048.0f;
    guess.adc.phase_c_offset_count = 2048.0f;
    return foc_encoder_init(encoder, &profile->encoder) == FOC_ENCODER_STATUS_OK &&
            foc_runtime_init(runtime, &guess, &profile->runtime, 0U) == FOC_RUNTIME_OK
        ? 0
        : -1;
}

int foc_virtual_calibration_start(
    struct foc_virtual_calibration *session,
    const struct foc_virtual_current_profile *profile,
    const struct foc_cal_workflow_request *request
)
{
    if (session == NULL || profile == NULL || request == NULL ||
        request->profile != FOC_CAL_WORKFLOW_FULL_PROFILE || !request->require_dual_axis ||
        session->status == FOC_VIRTUAL_CAL_PENDING || session->original.initialized) {
        return FOC_VIRTUAL_CAL_ERR_SETUP;
    }
    session->profile = *profile;
    session->electrical = foc_virtual_cal_electrical_default_config();
    session->electrical.permit = capture_permitted;
    session->electrical.permit_argument = session;
    session->electrical.sensor_adc = profile->adc;
    session->electrical.decoded_adc = profile->adc;
    session->electrical.vbus_v = profile->vbus_v;
    const struct foc_pipeline_config physical = {
        .adc = profile->adc,
    };
    if (plant_reset(session) != 0 ||
        foc_virtual_current_init_with(
            &session->original,
            profile,
            foc_virtual_calibration_baseline,
            NULL
        ) != 0 ||
        foc_cal_workflow_init(&session->workflow) != 0 ||
        foc_cal_application_start(
            &session->application,
            &session->workflow,
            request,
            &session->original.runtime,
            &physical,
            &profile->encoder
        ) != 0) {
        session->status = FOC_VIRTUAL_CAL_ERR_SETUP;
        return session->status;
    }
    session->stage = FOC_VIRTUAL_CAL_ADC;
    session->status = FOC_VIRTUAL_CAL_PENDING;
    return FOC_VIRTUAL_CAL_PENDING;
}

void foc_virtual_calibration_cancel(struct foc_virtual_calibration *session)
{
    if (session != NULL && session->original.initialized &&
        (session->status == FOC_VIRTUAL_CAL_PENDING || session->status == FOC_VIRTUAL_CAL_OK)) {
        (void)foc_virtual_current_request_stop(&session->original);
        (void)foc_cal_workflow_cancel(&session->workflow);
        session->flux.qualified = false;
        session->flux.kt.qualified = false;
        halt_experiment(&session->motor);
        session->status = FOC_VIRTUAL_CAL_ERR_TERMINATED;
    }
}

int foc_virtual_calibration_poll(struct foc_virtual_calibration *session)
{
    if (session == NULL || !session->original.initialized) {
        return FOC_VIRTUAL_CAL_ERR_SETUP;
    }
    if (session->status != FOC_VIRTUAL_CAL_PENDING && session->status != FOC_VIRTUAL_CAL_OK) {
        return session->status;
    }
    if (!capture_is_exclusive(session)) {
        foc_virtual_calibration_cancel(session);
        return session->status;
    }
    if (session->status == FOC_VIRTUAL_CAL_OK) {
        return session->status;
    }
    if (session->time_us >= FOC_CAL_DEADLINE_US) {
        foc_virtual_calibration_cancel(session);
        session->status = FOC_VIRTUAL_CAL_ERR_TIMEOUT;
        return session->status;
    }
    int status = 0;
    switch (session->stage) {
    case FOC_VIRTUAL_CAL_ADC:
        status = adc_zero(session);
        break;
    case FOC_VIRTUAL_CAL_LUT:
        status = encoder_lut(session);
        break;
    case FOC_VIRTUAL_CAL_ROTOR:
        status = angles(session, true);
        break;
    case FOC_VIRTUAL_CAL_OFFSET:
        status = angles(session, false);
        break;
    case FOC_VIRTUAL_CAL_ELECTRICAL_D:
        status = electrical_axis(session, FOC_MOTOR_ID_D);
        break;
    case FOC_VIRTUAL_CAL_ELECTRICAL_Q:
        status = electrical_axis(session, FOC_MOTOR_ID_Q);
        break;
    case FOC_VIRTUAL_CAL_FLUX:
        status = flux(session);
        break;
    case FOC_VIRTUAL_CAL_MECHANICAL:
        status = mechanics(session);
        break;
    case FOC_VIRTUAL_CAL_TUNE:
        status = tune(session);
        break;
    case FOC_VIRTUAL_CAL_REVIEW:
    default:
        status = FOC_CAL_WORKFLOW_ERR_STATE;
        break;
    }
    session->native_status = status;
    if (!capture_is_exclusive(session)) {
        foc_virtual_calibration_cancel(session);
    } else if (session->deadline_exceeded || session->time_us > FOC_CAL_DEADLINE_US) {
        foc_virtual_calibration_cancel(session);
        session->status = FOC_VIRTUAL_CAL_ERR_TIMEOUT;
    } else if (status != 0) {
        foc_virtual_calibration_cancel(session);
        enum foc_calibration_failure failure = FOC_CALIBRATION_FAILURE_NONE;
        if (session->stage == FOC_VIRTUAL_CAL_ROTOR) {
            failure = session->workflow.result.stages[FOC_CAL_WORKFLOW_ROTOR].failure;
        } else if (session->stage == FOC_VIRTUAL_CAL_OFFSET) {
            failure = session->workflow.result.stages[FOC_CAL_WORKFLOW_OFFSET].failure;
        } else if (session->stage == FOC_VIRTUAL_CAL_LUT) {
            failure = session->workflow.result.stages[FOC_CAL_WORKFLOW_LUT].failure;
        }
        session->status = status == FOC_CAL_WORKFLOW_ERR_DATA_REQUIRED ||
                failure == FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE ||
                failure == FOC_CALIBRATION_FAILURE_SAMPLE_COUNT
            ? FOC_VIRTUAL_CAL_ERR_REFERENCE
            : FOC_VIRTUAL_CAL_ERR_CAPTURE;
    } else if (++session->stage == FOC_VIRTUAL_CAL_REVIEW) {
        halt_experiment(&session->motor);
        session->status = FOC_VIRTUAL_CAL_OK;
    }
    return session->status;
}

const char *foc_virtual_calibration_stage_name(enum foc_virtual_calibration_stage stage)
{
    static const char *const names[] = {
        "ADC_ZERO",
        "ENCODER_LUT",
        "ROTOR",
        "ELECTRICAL_OFFSET",
        "ELECTRICAL_D",
        "ELECTRICAL_Q",
        "FLUX",
        "MECHANICAL",
        "TUNING",
        "REVIEW",
    };
    return (unsigned int)stage < sizeof names / sizeof names[0] ? names[stage] : "UNKNOWN";
}

/* ---------------------------------------- end of file ----------------------------------------- */
