/**
 * @file    foc_virtual_calibration_electrical.c
 * @brief   Virtual acquisition of constrained-shaft R/L and spinning back-EMF.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * Drive and observe the existing plant from a bounded Foreground experiment.
 * Work on a copy of the dedicated plant so a failed acquisition cannot leave
 * its caller's shaft fixture or terminal drive active.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_calibration_reference.h"
#include "foc_virtual_calibration_trial.h"

#include <math.h>

/* Private Constants ------------------------------------------------------------ */

#define TWO_PI                    6.2831853071795864769
#define INTERVAL_US               UINT32_C(50)
#define INTERVAL_S                0.00005
#define RL_INTERVALS              UINT32_C(768)
#define FLUX_INTERVALS_PER_WINDOW UINT32_C(32)
#define FLUX_WINDOWS_PER_SPEED    UINT32_C(4)
#define FRAME_TOLERANCE_RAD       0.01
#define FLUX_VD                   0.15
#define FLUX_VQ                   0.12

/* Private Helpers -------------------------------------------------------------- */

static bool adc_matches(
    const struct foc_device_adc_config *adc,
    const struct foc_calibration_identity *identity
)
{
    return foc_device_adc_config_validate(adc) == FOC_DEVICE_STATUS_OK &&
        adc->current_a_per_count == identity->current_a_per_count &&
        (unsigned int)adc->phase_order == identity->phase_order;
}

static bool virtual_config_valid(
    const struct foc_virtual_cal_electrical_config *config,
    const struct foc_calibration_identity *identity
)
{
    return config != NULL && adc_matches(&config->sensor_adc, identity) &&
        adc_matches(&config->decoded_adc, identity) && isfinite(config->vbus_v) &&
        config->vbus_v >= config->sensor_adc.vbus_min_v &&
        config->vbus_v >= config->decoded_adc.vbus_min_v && isfinite(config->max_current_a) &&
        config->max_current_a > 0.0f;
}

static bool current_at_aperture(
    const struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    uint32_t full_scale,
    uint32_t sequence,
    uint32_t aperture_us,
    double angle_rad,
    struct foc_vector_dq *current,
    const struct foc_virtual_phase_source *reference
)
{
    struct foc_device_adc_raw raw;
    struct foc_device_measurement measured;
    struct foc_vector_alpha_beta current_ab;
    const struct foc_virtual_cal_trial *trial = reference != NULL ? reference->trial : NULL;
    const struct foc_device_adc_config sensed = trial != NULL
        ? foc_virtual_cal_trial_adc(&config->sensor_adc, trial->seed, aperture_us)
        : config->sensor_adc;
    return foc_virtual_motor_make_adc(
               motor,
               &sensed,
               config->vbus_v,
               sequence,
               aperture_us,
               &raw
           ) == 0 &&
        raw.rank2_raw <= full_scale && raw.rank3_raw <= full_scale && raw.rank4_raw <= full_scale &&
        raw.vbus_raw <= full_scale &&
        foc_device_decode_sample(&raw, &config->decoded_adc, &measured) == FOC_DEVICE_STATUS_OK &&
        foc_clarke(&measured.phase_current_a, &current_ab) == FOC_STATUS_OK &&
        foc_park(&current_ab, (float)remainder(angle_rad, TWO_PI), current) == FOC_STATUS_OK &&
        hypotf(current->d, current->q) <= config->max_current_a;
}

static int excite(struct foc_virtual_motor *motor, double vd, double vq, double theta)
{
    const double cosine = cos(theta);
    const double sine = sin(theta);
    return foc_virtual_motor_step(
        motor,
        cosine * vd - sine * vq,
        sine * vd + cosine * vq,
        INTERVAL_S
    );
}

static double terminal_voltage(
    const struct foc_virtual_motor_observation *end,
    double angle,
    enum foc_motor_id_axis axis
)
{
    const double cosine = cos(angle);
    const double sine = sin(angle);
    return axis == FOC_MOTOR_ID_D ? cosine * end->v_alpha_v + sine * end->v_beta_v
                                  : cosine * end->v_beta_v - sine * end->v_alpha_v;
}

static bool stop_fixture(struct foc_virtual_motor *motor)
{
    if (foc_virtual_motor_set_fixture_speed(motor, 0.0) != 0 ||
        foc_virtual_motor_step(motor, 0.0, 0.0, INTERVAL_S) != 0) {
        return false;
    }
    foc_virtual_motor_release_fixture(motor);
    return true;
}

static struct foc_motor_id_config rl_policy(
    const struct foc_cal_workflow_identity *identity,
    const struct foc_virtual_cal_electrical_config *config,
    enum foc_motor_id_axis axis
)
{
    return (struct foc_motor_id_config){
        .axis = axis,
        .source = FOC_MOTOR_ID_VIRTUAL,
        .voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
        .setup_id = identity->motor_setup_id,
        .min_interval_us = INTERVAL_US,
        .max_interval_us = INTERVAL_US,
        .intervals_per_window = 8U,
        .min_windows = 32U,
        .max_abs_current_a = config->max_current_a,
        .max_abs_voltage_v = config->vbus_v * 0.57f,
        .min_current_rms_a = 0.05f,
        .min_slew_rms_a_per_s = 5.0f,
        .min_voltage_rms_v = 0.05f,
        .max_residual_rms_v = 0.025f,
        .min_rs_ohm = 0.03f,
        .max_rs_ohm = 2.0f,
        .min_inductance_h = 0.0004f,
        .max_inductance_h = 0.01f,
    };
}

static bool later_than(uint32_t time, uint32_t previous)
{
    const uint32_t delta = time - previous;
    return delta != 0U && delta < UINT32_C(0x80000000);
}

static bool capture_available(const struct foc_cal_workflow_bundle *bundle, uint32_t capture)
{
    if (capture == bundle->kt.capture || capture == bundle->electrical_q_capture.id) {
        return false;
    }
    for (unsigned int stage = 0U; stage < FOC_CAL_WORKFLOW_STAGE_COUNT; ++stage) {
        if (capture == bundle->stages[stage].primary.id ||
            capture == bundle->stages[stage].holdout.id) {
            return false;
        }
    }
    return true;
}

static enum foc_calibration_failure collect_flux(
    struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    const struct foc_cal_workflow_bundle *bundle,
    struct foc_virtual_cal_flux_result *result,
    const struct foc_virtual_flux_options *options,
    struct foc_flux_id_result *evidence,
    const struct foc_virtual_phase_source *reference
)
{
    const uint32_t session = bundle->request.session;
    const uint16_t pole_pairs = bundle->rotor.pole_pairs;
    const uint32_t full_scale = bundle->request.identity.acquisition.adc_full_scale_counts;
    const struct foc_flux_id_config policy = {
        .identity =
            {
                .session = session,
                .capture = result->capture,
                .motor = bundle->request.identity.motor_setup_id,
                .dq_frame = options != NULL ? options->angle_reference : 1U,
                .current_chain = options != NULL ? options->current_revision : session,
                .voltage_chain = options != NULL ? options->voltage_chain : 1U,
                .speed_chain = options != NULL ? options->angle_reference : 1U,
                .clock = 1U,
                .rl_revision = options != NULL ? options->rl_revision : session,
                .poles_revision = options != NULL ? options->poles_revision : session,
                .phase_order = bundle->request.identity.acquisition.phase_order,
                .pole_pairs = pole_pairs,
            },
        .rl = bundle->electrical_model,
        .rl_qualified = true,
        .source = FOC_MOTOR_ID_VIRTUAL,
        .voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
        .interval_us = INTERVAL_US,
        .intervals_per_window = FLUX_INTERVALS_PER_WINDOW,
        .min_windows_per_direction = 2U * FLUX_WINDOWS_PER_SPEED,
        .max_abs_current_a = config->max_current_a,
        .max_abs_vq_v = config->vbus_v * 0.57,
        .max_abs_speed_rad_s = 100.0,
        .min_abs_window_speed_rad_s = 1.0,
        .min_speed_span_rad_s = 1.0,
        .min_speed_rms_rad_s = 5.0,
        .min_flux_wb = 0.001,
        .max_flux_wb = 0.2,
        .max_residual_rms_v = 0.02,
        .max_direction_difference_relative = 0.05,
    };
    struct foc_flux_id estimator;
    const enum foc_calibration_failure initialized = foc_flux_id_init(&estimator, &policy);
    if (initialized != FOC_CALIBRATION_FAILURE_NONE) {
        return initialized;
    }
    struct foc_virtual_motor_observation prior;
    struct foc_vector_dq previous;
    double theta = 0.0;
    double shaft_speed = 0.0;
    if (foc_virtual_motor_observe(motor, &prior) != 0) {
        return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
    }
    if (reference == NULL) {
        theta = prior.electrical_position_rad;
    } else if (!reference
                    ->read(reference->argument, motor, result->first_us, &theta, &shaft_speed)) {
        return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
    }
    if (!current_at_aperture(motor, config, full_scale, 0U, result->first_us, theta, &previous,
                            reference)) {
        return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
    }
    result->samples = 1U;
    result->last_us = result->first_us;

    static const double speed_levels[] = {1.0, -1.0, 2.0, -2.0};
    const double direction = reference != NULL && reference->trial != NULL &&
            reference->trial->reverse_first ? -1.0 : 1.0;
    for (unsigned int speed_index = 0U; speed_index < 4U; ++speed_index) {
        const double fixture_speed =
            direction * config->flux_electrical_speed_rad_s * speed_levels[speed_index] /
            (double)pole_pairs;
        if (foc_virtual_motor_set_fixture_speed(motor, fixture_speed) != 0) {
            return FOC_CALIBRATION_FAILURE_PRECONDITION;
        }

        for (uint32_t window = 0U; window < FLUX_WINDOWS_PER_SPEED; ++window) {
            const double excitation_vq = (window % 2U == 0U ? FLUX_VQ : -FLUX_VQ);
            for (uint32_t interval = 0U; interval < FLUX_INTERVALS_PER_WINDOW; ++interval) {
                struct foc_virtual_motor_observation end;
                struct foc_vector_dq current;
                const uint32_t time = result->last_us + INTERVAL_US;
                if (config->permit != NULL && !config->permit(config->permit_argument, time)) {
                    return FOC_CALIBRATION_FAILURE_PRECONDITION;
                }
                if (excite(motor, FLUX_VD, excitation_vq, theta) != 0 ||
                    foc_virtual_motor_observe(motor, &end) != 0 || !end.fixture_active) {
                    return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
                }
                double next_theta;
                if (reference == NULL) {
                    next_theta = end.electrical_position_rad;
                    shaft_speed = end.mechanical_velocity_rad_s;
                } else {
                    double encoder_theta;
                    if (!reference->read(
                            reference->argument,
                            motor,
                            time,
                            &encoder_theta,
                            &shaft_speed
                        )) {
                        return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
                    }
                    /* A 50 us interval is shorter than the encoder aperture step
                     * at some speeds. Run the independently witnessed speed clock
                     * and require an encoder-backed phase check at every aperture. */
                    next_theta = theta + pole_pairs * shaft_speed * INTERVAL_S;
                    if (fabs(remainder(encoder_theta - next_theta, TWO_PI)) > 0.008) {
                        return FOC_CALIBRATION_FAILURE_PRECONDITION;
                    }
                }
                const double omega_e = pole_pairs * shaft_speed;
                if (fabs((next_theta - theta) - omega_e * INTERVAL_S) > 1e-7 ||
                    omega_e * fixture_speed <= 0.0) {
                    return FOC_CALIBRATION_FAILURE_PRECONDITION;
                }
                if (!current_at_aperture(
                        motor,
                        config,
                        full_scale,
                        result->samples,
                        time,
                        next_theta,
                        &current,
                        reference
                    )) {
                    return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
                }
                const double vq = 0.5 *
                    (terminal_voltage(&end, theta, FOC_MOTOR_ID_Q) +
                     terminal_voltage(&end, next_theta, FOC_MOTOR_ID_Q));
                const struct foc_flux_id_interval sample = {
                    .identity = policy.identity,
                    .source = policy.source,
                    .voltage_source = policy.voltage_source,
                    .sequence = result->samples,
                    .begin_us = result->last_us,
                    .end_us = time,
                    .voltage_begin_us = result->last_us,
                    .voltage_end_us = time,
                    .speed_begin_us = result->last_us,
                    .speed_end_us = time,
                    .id_begin_a = previous.d,
                    .iq_begin_a = previous.q,
                    .id_end_a = current.d,
                    .iq_end_a = current.q,
                    .mean_omega_e_rad_s = omega_e,
                    .mean_vq_v = vq,
                    .current_valid = true,
                    .voltage_valid = true,
                    .speed_valid = true,
                    .frame_confirmed = true,
                    .output_confirmed = true,
                    .terminal_voltage_witnessed = true,
                };
                const enum foc_calibration_failure status = foc_flux_id_push(&estimator, &sample);
                if (status != FOC_CALIBRATION_FAILURE_NONE) {
                    return foc_flux_id_finish(&estimator, evidence);
                }
                if (options != NULL && options->interval != NULL) {
                    options->interval(options->argument, &sample);
                }
                previous = current;
                theta = next_theta;
                result->last_us = time;
                ++result->samples;
            }
        }
    }
    const enum foc_calibration_failure status = foc_flux_id_finish(&estimator, evidence);
    result->residual_rms_v = evidence->residual_rms_v;
    result->omega_e_rms_rad_s = evidence->omega_e_rms_rad_s;
    result->flux_wb = evidence->flux_wb;
    result->ke_phase_peak_v_per_mechanical_rad_s = evidence->ke_phase_peak_v_per_mechanical_rad_s;
    result->kt_nm_per_peak_q_amp = evidence->kt_nm_per_peak_q_amp;
    return status;
}

/* Public Functions ------------------------------------------------------------- */

struct foc_virtual_cal_electrical_config foc_virtual_cal_electrical_default_config(void)
{
    const struct foc_device_adc_config adc = {
        .vbus_v_per_count = 0.01f,
        .current_a_per_count = 0.002f,
        .vbus_min_v = 1.0f,
        .phase_a_offset_count = 2048.0f,
        .phase_b_offset_count = 2048.0f,
        .phase_c_offset_count = 2048.0f,
        .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
    };
    return (struct foc_virtual_cal_electrical_config){
        .sensor_adc = adc,
        .decoded_adc = adc,
        .vbus_v = 12.0f,
        .rl_amplitude_v = 0.90f,
        .max_current_a = 2.5f,
        .flux_electrical_speed_rad_s = 10.0f,
    };
}

int foc_virtual_calibrate_rl_observed(
    struct foc_cal_workflow *workflow,
    struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    enum foc_motor_id_axis axis,
    uint32_t capture,
    uint32_t first_us,
    double aligned_electrical_rad,
    const struct foc_virtual_phase_source *reference
)
{
    const struct foc_cal_workflow_bundle *bundle = foc_cal_workflow_result(workflow);
    if (bundle == NULL || motor == NULL || (axis != FOC_MOTOR_ID_D && axis != FOC_MOTOR_ID_Q) ||
        !virtual_config_valid(config, &bundle->request.identity.acquisition) ||
        !isfinite(aligned_electrical_rad) || !isfinite(config->rl_amplitude_v) ||
        config->rl_amplitude_v <= 0.0f || config->rl_amplitude_v >= config->vbus_v * 0.57f) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    struct foc_virtual_motor trial = *motor;
    struct foc_virtual_motor_observation origin;
    if (foc_virtual_motor_observe(&trial, &origin) != 0 || origin.fixture_active ||
        origin.mechanical_velocity_rad_s != 0.0 ||
        (reference == NULL &&
         fabs(remainder(origin.electrical_position_rad - aligned_electrical_rad, TWO_PI)) >
             FRAME_TOLERANCE_RAD) ||
        foc_virtual_motor_set_fixture_speed(&trial, 0.0) != 0) {
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    if (reference != NULL) {
        double measured_theta;
        double measured_speed;
        if (!reference
                 ->read(reference->argument, &trial, first_us, &measured_theta, &measured_speed) ||
            fabs(measured_speed) > 1e-6 ||
            fabs(remainder(measured_theta - aligned_electrical_rad, TWO_PI)) >
                FRAME_TOLERANCE_RAD) {
            return FOC_CAL_WORKFLOW_ERR_QUALITY;
        }
    }
    const struct foc_cal_workflow_stage_request stage = {
        .stage = FOC_CAL_WORKFLOW_ELECTRICAL,
        .source = FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT,
        .capture = capture,
        .policy.electrical = rl_policy(&bundle->request.identity, config, axis),
    };
    int status = foc_cal_workflow_open(workflow, &stage);
    if (status != FOC_CAL_WORKFLOW_OK) {
        return status;
    }

    static const double pulses[] = {1.0, 1.0, -0.75, -0.75, 0.33, -1.0, 0.65, -0.26};
    for (uint32_t index = 0U; index <= RL_INTERVALS; ++index) {
        const uint32_t time = first_us + index * INTERVAL_US;
        if (config->permit != NULL && !config->permit(config->permit_argument, time)) {
            (void)stop_fixture(&trial);
            return FOC_CAL_WORKFLOW_ERR_STATE;
        }
        struct foc_virtual_motor_observation end = origin;
        struct foc_vector_dq current = {0};
        bool applied = false;
        if (index != 0U) {
            const double voltage = config->rl_amplitude_v * pulses[((index - 1U) / 24U) % 8U];
            const double vd = axis == FOC_MOTOR_ID_D ? voltage : 0.0;
            const double vq = axis == FOC_MOTOR_ID_Q ? voltage : 0.0;
            applied = excite(&trial, vd, vq, aligned_electrical_rad) == 0 &&
                foc_virtual_motor_observe(&trial, &end) == 0;
        } else {
            foc_virtual_motor_observe(&trial, &end);
        }
        const bool stationary = end.fixture_active && end.mechanical_velocity_rad_s == 0.0 &&
            end.mechanical_position_rad == origin.mechanical_position_rad;
        const bool sampled = current_at_aperture(
            &trial,
            config,
            bundle->request.identity.acquisition.adc_full_scale_counts,
            index,
            time,
            aligned_electrical_rad,
            &current,
            reference
        );
        const struct foc_cal_workflow_sample sample = {
            .session = bundle->request.session,
            .capture = capture,
            .identity = bundle->request.identity,
            .source = stage.source,
            .units = FOC_CAL_WORKFLOW_ELECTRICAL_SI,
            .valid = sampled && stationary,
            .output_confirmed = applied,
            .data.electrical = {
                .setup_id = bundle->request.identity.motor_setup_id,
                .capture_id = capture,
                .source = stage.policy.electrical.source,
                .sequence = index,
                .captured_us = time,
                .voltage_begin_us = time - INTERVAL_US,
                .voltage_end_us = time,
                .voltage_source = stage.policy.electrical.voltage_source,
                .current_a = axis == FOC_MOTOR_ID_D ? current.d : current.q,
                .mean_voltage_v = (float)terminal_voltage(&end, aligned_electrical_rad, axis),
                .omega_e_rad_s = 0.0f,
                .current_valid = sampled,
                .voltage_valid = applied,
                .output_confirmed = applied,
                .frame_confirmed = stationary,
                .stationary_confirmed = stationary,
            },
        };
        status = foc_cal_workflow_push(workflow, &sample, time);
        if (status != FOC_CAL_WORKFLOW_OK) {
            return status;
        }
        if (config->rl_endpoint != NULL) {
            config->rl_endpoint(config->rl_endpoint_argument, axis, &sample.data.electrical);
        }
    }
    if (!stop_fixture(&trial)) {
        foc_cal_workflow_data_required(workflow, FOC_CAL_WORKFLOW_ELECTRICAL);
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    status = foc_cal_workflow_finish_stage(workflow);
    if (status == FOC_CAL_WORKFLOW_OK || status == FOC_CAL_WORKFLOW_PARTIAL) {
        *motor = trial;
    }
    return status;
}

int foc_virtual_calibrate_rl_axis(
    struct foc_cal_workflow *workflow,
    struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    enum foc_motor_id_axis axis,
    uint32_t capture,
    uint32_t first_us,
    double aligned_electrical_rad
)
{
    return foc_virtual_calibrate_rl_observed(
        workflow,
        motor,
        config,
        axis,
        capture,
        first_us,
        aligned_electrical_rad,
        NULL
    );
}

int foc_virtual_calibrate_flux_referenced(
    struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    const struct foc_cal_workflow_bundle *bundle,
    uint32_t capture,
    uint32_t first_us,
    struct foc_virtual_cal_flux_result *result,
    const struct foc_virtual_flux_options *options,
    struct foc_flux_id_result *evidence,
    const struct foc_virtual_phase_source *reference
)
{
    struct foc_flux_id_result local;
    if (evidence == NULL) {
        evidence = &local;
    }
    *evidence = (struct foc_flux_id_result){.state = FOC_CALIBRATION_STATE_INVALID};
    if (result == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    *result = (struct foc_virtual_cal_flux_result){.capture = capture, .first_us = first_us};
    if (motor == NULL || bundle == NULL || capture == 0U ||
        !virtual_config_valid(config, &bundle->request.identity.acquisition) ||
        !isfinite(config->flux_electrical_speed_rad_s) ||
        config->flux_electrical_speed_rad_s < 4.0f || config->flux_electrical_speed_rad_s > 50.0f) {
        result->failure = FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
        evidence->failure = result->failure;
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    const uint32_t required = FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ELECTRICAL) |
        FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR);
    if (!bundle->request.require_dual_axis || (bundle->passed & required) != required ||
        bundle->state == FOC_CAL_WORKFLOW_CANCELLED || bundle->state == FOC_CAL_WORKFLOW_REJECTED ||
        !bundle->rotor.identifiable || bundle->rotor.pole_pairs == 0U ||
        bundle->stages[FOC_CAL_WORKFLOW_ELECTRICAL].source !=
            FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT) {
        result->failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
        evidence->failure = result->failure;
        return FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
    }
    struct foc_motor_rl_model model;
    if (foc_motor_id_make_model(
            &bundle->electrical,
            &bundle->electrical_q,
            bundle->request.max_rs_relative_difference,
            &model
        ) != FOC_MOTOR_ID_OK ||
        model.rs_ohm != bundle->electrical_model.rs_ohm ||
        model.ld_h != bundle->electrical_model.ld_h ||
        model.lq_h != bundle->electrical_model.lq_h) {
        result->failure = FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH;
        evidence->failure = result->failure;
        return FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
    }
    if (!capture_available(bundle, capture) ||
        !later_than(first_us, bundle->electrical_q_capture.last_us) ||
        !later_than(first_us, bundle->stages[FOC_CAL_WORKFLOW_ROTOR].primary.last_us)) {
        result->failure = FOC_CALIBRATION_FAILURE_TIME_ORDER;
        evidence->failure = result->failure;
        return FOC_CAL_WORKFLOW_ERR_ORDER;
    }
    struct foc_virtual_motor trial = *motor;
    struct foc_virtual_motor_observation initial;
    if (foc_virtual_motor_observe(&trial, &initial) != 0 || initial.fixture_active ||
        initial.mechanical_velocity_rad_s != 0.0) {
        result->failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
        evidence->failure = result->failure;
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    result->failure = collect_flux(&trial, config, bundle, result, options, evidence, reference);
    if (result->failure != FOC_CALIBRATION_FAILURE_NONE) {
        evidence->failure = result->failure;
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    if (!stop_fixture(&trial)) {
        *evidence = (struct foc_flux_id_result){
            .state = FOC_CALIBRATION_STATE_INVALID,
            .failure = FOC_CALIBRATION_FAILURE_NUMERIC,
        };
        *result = (struct foc_virtual_cal_flux_result){
            .capture = capture,
            .first_us = first_us,
            .failure = FOC_CALIBRATION_FAILURE_NUMERIC,
        };
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    result->qualified = true;
    result->kt = (struct foc_cal_workflow_kt){
        .identity = bundle->request.identity,
        .torque_model_id = bundle->request.identity.motor_setup_id,
        .nm_per_a = result->kt_nm_per_peak_q_amp,
        .qualified = true,
        .source = FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT,
        .capture = capture,
        .captured_us = result->last_us,
    };
    *motor = trial;
    return FOC_CAL_WORKFLOW_OK;
}

int foc_virtual_calibrate_flux_observed(
    struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    const struct foc_cal_workflow_bundle *bundle,
    uint32_t capture,
    uint32_t first_us,
    struct foc_virtual_cal_flux_result *result,
    const struct foc_virtual_flux_options *options,
    struct foc_flux_id_result *evidence
)
{
    return foc_virtual_calibrate_flux_referenced(
        motor,
        config,
        bundle,
        capture,
        first_us,
        result,
        options,
        evidence,
        NULL
    );
}

int foc_virtual_calibrate_flux(
    struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    const struct foc_cal_workflow_bundle *bundle,
    uint32_t capture,
    uint32_t first_us,
    struct foc_virtual_cal_flux_result *result
)
{
    return foc_virtual_calibrate_flux_observed(
        motor,
        config,
        bundle,
        capture,
        first_us,
        result,
        NULL,
        NULL
    );
}

/* ---------------------------------------- end of file ----------------------------------------- */
