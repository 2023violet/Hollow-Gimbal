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

#include "foc_virtual_calibration_electrical.h"

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

/* Private Types ---------------------------------------------------------------- */

struct flux_fit {
    double speed2;
    double voltage2;
    double voltage_speed;
};

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
    struct foc_vector_dq *current
)
{
    struct foc_device_adc_raw raw;
    struct foc_device_measurement measured;
    struct foc_vector_alpha_beta current_ab;
    return foc_virtual_motor_make_adc(
               motor,
               &config->sensor_adc,
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

static enum foc_calibration_failure fit_flux(
    const struct flux_fit fits[2],
    struct foc_virtual_cal_flux_result *result,
    uint16_t pole_pairs
)
{
    const double windows = 4.0 * (double)FLUX_WINDOWS_PER_SPEED;
    const double speed2 = fits[0].speed2 + fits[1].speed2;
    result->omega_e_rms_rad_s = sqrt(speed2 / windows);
    if (fits[0].speed2 <= 0.0 || fits[1].speed2 <= 0.0 || result->omega_e_rms_rad_s < 5.0) {
        return FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE;
    }
    const double dot = fits[0].voltage_speed + fits[1].voltage_speed;
    const double flux_wb = dot / speed2;
    const double energy = fits[0].voltage2 + fits[1].voltage2;
    result->residual_rms_v = sqrt(fmax(0.0, energy - flux_wb * dot) / windows);
    if (!isfinite(flux_wb) || !isfinite(result->residual_rms_v)) {
        return FOC_CALIBRATION_FAILURE_NUMERIC;
    }
    if (flux_wb < 0.001 || flux_wb > 0.2) {
        return FOC_CALIBRATION_FAILURE_RANGE;
    }
    if (result->residual_rms_v > 0.02 ||
        fabs(fits[0].voltage_speed / fits[0].speed2 - fits[1].voltage_speed / fits[1].speed2) >
            0.05 * flux_wb) {
        return FOC_CALIBRATION_FAILURE_RESIDUAL;
    }
    result->flux_wb = (float)flux_wb;
    result->ke_phase_peak_v_per_mechanical_rad_s = (float)((double)pole_pairs * flux_wb);
    result->kt_nm_per_peak_q_amp = (float)(1.5 * (double)pole_pairs * flux_wb);
    return FOC_CALIBRATION_FAILURE_NONE;
}

static enum foc_calibration_failure collect_flux(
    struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    const struct foc_cal_workflow_bundle *bundle,
    struct foc_virtual_cal_flux_result *result
)
{
    const struct foc_motor_rl_model *rl = &bundle->electrical_model;
    const uint16_t pole_pairs = bundle->rotor.pole_pairs;
    const uint32_t full_scale = bundle->request.identity.acquisition.adc_full_scale_counts;
    struct foc_virtual_motor_observation prior;
    struct foc_vector_dq previous;
    if (foc_virtual_motor_observe(motor, &prior) != 0 ||
        !current_at_aperture(
            motor,
            config,
            full_scale,
            0U,
            result->first_us,
            prior.electrical_position_rad,
            &previous
        )) {
        return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
    }
    result->samples = 1U;
    result->last_us = result->first_us;

    struct flux_fit fits[2] = {0};
    static const double speed_levels[] = {1.0, -1.0, 2.0, -2.0};
    for (unsigned int speed_index = 0U; speed_index < 4U; ++speed_index) {
        const double fixture_speed =
            config->flux_electrical_speed_rad_s * speed_levels[speed_index] / (double)pole_pairs;
        if (foc_virtual_motor_set_fixture_speed(motor, fixture_speed) != 0) {
            return FOC_CALIBRATION_FAILURE_PRECONDITION;
        }

        for (uint32_t window = 0U; window < FLUX_WINDOWS_PER_SPEED; ++window) {
            const double q_start = previous.q;
            const double excitation_vq = (window % 2U == 0U ? FLUX_VQ : -FLUX_VQ);
            double voltage_sum = 0.0;
            double speed_sum = 0.0;
            for (uint32_t interval = 0U; interval < FLUX_INTERVALS_PER_WINDOW; ++interval) {
                struct foc_virtual_motor_observation end;
                struct foc_vector_dq current;
                const uint32_t time = result->last_us + INTERVAL_US;
                if (config->permit != NULL && !config->permit(config->permit_argument, time)) {
                    return FOC_CALIBRATION_FAILURE_PRECONDITION;
                }
                if (excite(motor, FLUX_VD, excitation_vq, prior.electrical_position_rad) != 0 ||
                    foc_virtual_motor_observe(motor, &end) != 0 || !end.fixture_active ||
                    !current_at_aperture(
                        motor,
                        config,
                        full_scale,
                        result->samples,
                        time,
                        end.electrical_position_rad,
                        &current
                    )) {
                    return FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
                }
                const double angle_step =
                    end.electrical_position_rad - prior.electrical_position_rad;
                const double omega_e = pole_pairs * end.mechanical_velocity_rad_s;
                if (fabs(angle_step - omega_e * INTERVAL_S) > 1e-7 ||
                    omega_e * fixture_speed <= 0.0) {
                    return FOC_CALIBRATION_FAILURE_PRECONDITION;
                }

                /* The applied alpha/beta terminal voltage is held. Its q component
                 * rotates with the observed aperture angles. Integrate the q-axis
                 * equation over a full window to avoid differentiating ADC noise. */
                const double vq = 0.5 *
                    (terminal_voltage(&end, prior.electrical_position_rad, FOC_MOTOR_ID_Q) +
                     terminal_voltage(&end, end.electrical_position_rad, FOC_MOTOR_ID_Q));
                const double iq = 0.5 * ((double)previous.q + current.q);
                const double id = 0.5 * ((double)previous.d + current.d);
                voltage_sum += vq - rl->rs_ohm * iq - omega_e * rl->ld_h * id;
                speed_sum += omega_e;
                previous = current;
                prior = end;
                result->last_us = time;
                ++result->samples;
            }
            const double n = FLUX_INTERVALS_PER_WINDOW;
            const double emf = (voltage_sum - rl->lq_h * (previous.q - q_start) / INTERVAL_S) / n;
            const double speed = speed_sum / n;
            struct flux_fit *fit = &fits[speed_index % 2U];
            fit->speed2 += speed * speed;
            fit->voltage2 += emf * emf;
            fit->voltage_speed += emf * speed;
        }
    }
    return fit_flux(fits, result, pole_pairs);
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
        fabs(remainder(origin.electrical_position_rad - aligned_electrical_rad, TWO_PI)) >
            FRAME_TOLERANCE_RAD ||
        foc_virtual_motor_set_fixture_speed(&trial, 0.0) != 0) {
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
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
            &current
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

int foc_virtual_calibrate_flux(
    struct foc_virtual_motor *motor,
    const struct foc_virtual_cal_electrical_config *config,
    const struct foc_cal_workflow_bundle *bundle,
    uint32_t capture,
    uint32_t first_us,
    struct foc_virtual_cal_flux_result *result
)
{
    if (result == NULL) {
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    *result = (struct foc_virtual_cal_flux_result){.capture = capture, .first_us = first_us};
    if (motor == NULL || bundle == NULL || capture == 0U ||
        !virtual_config_valid(config, &bundle->request.identity.acquisition) ||
        !isfinite(config->flux_electrical_speed_rad_s) ||
        config->flux_electrical_speed_rad_s < 4.0f || config->flux_electrical_speed_rad_s > 50.0f) {
        result->failure = FOC_CALIBRATION_FAILURE_INVALID_CONFIG;
        return FOC_CAL_WORKFLOW_ERR_INPUT;
    }
    const uint32_t required = FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ELECTRICAL) |
        FOC_CAL_WORKFLOW_STAGE_BIT(FOC_CAL_WORKFLOW_ROTOR);
    if (!bundle->request.require_dual_axis || (bundle->passed & required) != required ||
        !bundle->rotor.identifiable || bundle->rotor.pole_pairs == 0U ||
        bundle->stages[FOC_CAL_WORKFLOW_ELECTRICAL].source !=
            FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT) {
        result->failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
        return FOC_CAL_WORKFLOW_ERR_DEPENDENCY;
    }
    if (!capture_available(bundle, capture) ||
        !later_than(first_us, bundle->electrical_q_capture.last_us) ||
        !later_than(first_us, bundle->stages[FOC_CAL_WORKFLOW_ROTOR].primary.last_us)) {
        result->failure = FOC_CALIBRATION_FAILURE_TIME_ORDER;
        return FOC_CAL_WORKFLOW_ERR_ORDER;
    }
    struct foc_virtual_motor trial = *motor;
    struct foc_virtual_motor_observation initial;
    if (foc_virtual_motor_observe(&trial, &initial) != 0 || initial.fixture_active ||
        initial.mechanical_velocity_rad_s != 0.0) {
        result->failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    result->failure = collect_flux(&trial, config, bundle, result);
    if (result->failure != FOC_CALIBRATION_FAILURE_NONE) {
        return FOC_CAL_WORKFLOW_ERR_QUALITY;
    }
    if (!stop_fixture(&trial)) {
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

/* ---------------------------------------- end of file ----------------------------------------- */
