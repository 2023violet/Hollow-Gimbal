/**
 * @file    test_foc_motor_id_virtual.c
 * @brief   Virtual PMSM observations and Runtime-backed PI candidate comparison.
 * @version v0.1.0
 * @date    2026.11.17
 *
 * Test role
 * ---------
 * Feed independent PMSM observations to calibration and exercise three gain sets.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_motor_id.h"
#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                       \
    do {                                                       \
        if (!(condition)) {                                    \
            fprintf(stderr, "%d: %s\n", __LINE__, #condition); \
            return 1;                                          \
        }                                                      \
    } while (0)

enum capture_case {
    IDEAL,
    QUANTIZED,
    NO_EXCITATION,
    CLIPPED,
    MOVING,
};

struct fit_scenario {
    const char *name;
    struct foc_motor_rl_model truth;
    uint32_t interval_us;
    enum capture_case kind;
};

static double relative_error(double actual, double expected)
{
    return fabs(actual - expected) / expected;
}

static float noise(uint32_t *state, float amplitude)
{
    *state = UINT32_C(1664525) * *state + UINT32_C(1013904223);
    return ((float)(*state >> 16U) / 65535.0f * 2.0f - 1.0f) * amplitude;
}

static int read_current(
    const struct foc_virtual_motor *motor,
    const struct foc_virtual_current_profile *profile,
    struct foc_motor_id_sample *sample,
    enum foc_motor_id_axis axis,
    uint32_t *noise_state
)
{
    struct foc_device_adc_raw raw;
    struct foc_device_measurement measured;
    struct foc_vector_alpha_beta current;
    CHECK(
        foc_virtual_motor_make_adc(
            motor,
            &profile->adc,
            profile->vbus_v,
            sample->sequence,
            sample->captured_us,
            &raw
        ) == FOC_STATUS_OK
    );
    CHECK(foc_device_decode_sample(&raw, &profile->adc, &measured) == FOC_DEVICE_STATUS_OK);
    CHECK(foc_clarke(&measured.phase_current_a, &current) == FOC_STATUS_OK);
    sample->current_a =
        (axis == FOC_MOTOR_ID_D ? current.alpha : current.beta) + noise(noise_state, 0.001f);
    sample->mean_voltage_v =
        roundf(sample->mean_voltage_v / 0.004f) * 0.004f + noise(noise_state, 0.004f);
    return 0;
}

static struct foc_motor_id_config fit_config(enum foc_motor_id_axis axis, uint32_t setup_id)
{
    return (struct foc_motor_id_config){
        .axis = axis,
        .source = FOC_MOTOR_ID_VIRTUAL,
        .voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
        .setup_id = setup_id,
        .min_interval_us = 25U,
        .max_interval_us = 150U,
        .intervals_per_window = 8U,
        .min_windows = 24U,
        .max_abs_current_a = 3.0f,
        .max_abs_voltage_v = 1.5f,
        .min_current_rms_a = 0.15f,
        .min_slew_rms_a_per_s = 35.0f,
        .min_voltage_rms_v = 0.2f,
        .max_residual_rms_v = 0.018f,
        .min_rs_ohm = 0.1f,
        .max_rs_ohm = 1.35f,
        .min_inductance_h = 0.00075f,
        .max_inductance_h = 0.004f,
    };
}

static float excitation(unsigned int interval)
{
    static const float levels[] = {1.11f, 1.11f, -0.83f, -0.83f, 0.37f, -1.13f, 0.73f, -0.29f};
    return levels[(interval / 24U) % (sizeof levels / sizeof levels[0])];
}

static const int jitter_us[] = {-12, 5, 12, -5, 0};

static int capture_axis(
    const struct fit_scenario *scenario,
    enum foc_motor_id_axis axis,
    uint32_t capture_id,
    struct foc_motor_id_result *result
)
{
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    struct foc_virtual_motor_params params = profile.motor;
    params.rs_ohm = scenario->truth.rs_ohm;
    params.ld_h = scenario->truth.ld_h;
    params.lq_h = scenario->truth.lq_h;
    params.electrical_offset_rad = 0.0;
    /* With isolated q-axis excitation, remove PM torque in the Host experiment.
     * id is identically zero, so reluctance torque is also zero. Keep PM flux on
     * d-axis runs and on the free-rotor rejection control. */
    if (axis == FOC_MOTOR_ID_Q && scenario->kind != MOVING) {
        params.psi_f_wb = 0.0;
    }
    struct foc_virtual_motor motor;
    CHECK(foc_virtual_motor_init(&motor, &params, &(struct foc_virtual_motor_state){0}) == 0);
    const struct foc_motor_id_config config = fit_config(axis, 1U + capture_id / 2U);
    struct foc_motor_id_context fit = {0};
    CHECK(foc_motor_id_init(&fit, &config, capture_id) == FOC_MOTOR_ID_OK);
    uint32_t now_us = 0U;
    uint32_t noise_state = 123U;
    double max_abs_speed = 0.0;
    int status = FOC_MOTOR_ID_OK;
    for (unsigned int k = 0U; k <= 768U; ++k) {
        const uint32_t begin_us = now_us;
        float voltage = 0.0f;
        if (k != 0U) {
            if (scenario->kind != NO_EXCITATION) {
                voltage = excitation(k - 1U);
            }
            const uint32_t dt = scenario->kind == QUANTIZED
                ? (uint32_t)((int)scenario->interval_us + jitter_us[k % 5U])
                : scenario->interval_us;
            const double v_alpha = axis == FOC_MOTOR_ID_D ? voltage : 0.0;
            const double v_beta = axis == FOC_MOTOR_ID_Q ? voltage : 0.0;
            CHECK(fabs(voltage) < profile.vbus_v);
            CHECK(foc_virtual_motor_step(&motor, v_alpha, v_beta, (double)dt * 1.0e-6) == 0);
            now_us += dt;
        }
        const double speed = (double)params.pole_pairs * motor.state.mechanical_velocity_rad_s;
        if (fabs(speed) > max_abs_speed) {
            max_abs_speed = fabs(speed);
        }
        struct foc_motor_id_sample sample = {
            .setup_id = config.setup_id,
            .capture_id = capture_id,
            .source = config.source,
            .sequence = k,
            .voltage_source = config.voltage_source,
            .voltage_begin_us = begin_us,
            .voltage_end_us = now_us,
            .output_confirmed = k != 0U,
            .frame_confirmed = true,
            .captured_us = now_us,
            .current_a = (float)(axis == FOC_MOTOR_ID_D ? motor.state.id_a : motor.state.iq_a),
            .mean_voltage_v = voltage,
            .omega_e_rad_s = (float)speed,
            .current_valid = true,
            .voltage_valid = true,
            .stationary_confirmed = speed == 0.0 && motor.state.mechanical_position_rad == 0.0,
            .saturated = scenario->kind == CLIPPED && k == 40U,
        };
        if (scenario->kind == QUANTIZED) {
            CHECK(read_current(&motor, &profile, &sample, axis, &noise_state) == 0);
        }
        status = foc_motor_id_push(&fit, &sample);
        if (status != FOC_MOTOR_ID_OK) {
            break;
        }
    }
    const int finish = foc_motor_id_finish(&fit, result);
    const double expected_l = axis == FOC_MOTOR_ID_D ? scenario->truth.ld_h : scenario->truth.lq_h;
    if (scenario->kind == NO_EXCITATION || scenario->kind == CLIPPED || scenario->kind == MOVING) {
        printf(
            "REJECT,%s,%d,%d,%u,%u,%.9g\n",
            scenario->name,
            status,
            result->failure,
            (unsigned int)result->samples,
            (unsigned int)result->windows,
            max_abs_speed
        );
        CHECK(finish == FOC_MOTOR_ID_ERR_QUALITY && !result->usable);
        CHECK(result->rs_ohm == 0.0f && result->inductance_h == 0.0f);
        CHECK(
            result->failure ==
            (scenario->kind == NO_EXCITATION ? FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE
                                             : FOC_CALIBRATION_FAILURE_PRECONDITION)
        );
        return 0;
    }
    CHECK(status == FOC_MOTOR_ID_OK && finish == FOC_MOTOR_ID_OK && result->usable);
    printf(
        "FIT,%s,%u,%d,%d,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
        scenario->name,
        (unsigned int)axis,
        status,
        result->failure,
        (unsigned int)result->samples,
        (unsigned int)result->windows,
        result->rs_ohm,
        result->inductance_h,
        100.0 * relative_error(result->rs_ohm, scenario->truth.rs_ohm),
        100.0 * relative_error(result->inductance_h, expected_l),
        result->residual_rms_v,
        result->excitation,
        max_abs_speed
    );
    CHECK(max_abs_speed == 0.0 && result->windows == 96U && result->samples == 769U);
    CHECK(relative_error(result->rs_ohm, scenario->truth.rs_ohm) < 0.02);
    CHECK(relative_error(result->inductance_h, expected_l) < 0.02);
    return 0;
}

/* Host-only perturbations around the existing direct-RK4 capture path. */
static int
init_study_motor(struct foc_virtual_motor *motor, const struct foc_virtual_current_profile *profile)
{
    struct foc_virtual_motor_params params = profile->motor;
    params.electrical_offset_rad = 0.0;
    params.psi_f_wb = 0.0;
    CHECK(foc_virtual_motor_init(motor, &params, &(struct foc_virtual_motor_state){0}) == 0);
    return 0;
}

static int observe_study_axis(
    struct foc_virtual_motor *motor,
    const struct foc_virtual_current_profile *profile,
    const struct foc_motor_id_context *fit,
    unsigned int k,
    uint32_t *noise_state,
    float applied_v,
    struct foc_motor_id_sample *sample
)
{
    const enum foc_motor_id_axis axis = fit->config.axis;
    const uint32_t begin_us = sample->captured_us;
    uint32_t end_us = begin_us;
    if (k != 0U) {
        const uint32_t dt_us = (uint32_t)(80 + jitter_us[k % 5U]);
        const double alpha = axis == FOC_MOTOR_ID_D ? applied_v : 0.0;
        const double beta = axis == FOC_MOTOR_ID_Q ? applied_v : 0.0;
        CHECK(foc_virtual_motor_step(motor, alpha, beta, (double)dt_us * 1.0e-6) == 0);
        end_us += dt_us;
    }
    CHECK(
        motor->state.mechanical_velocity_rad_s == 0.0 && motor->state.mechanical_position_rad == 0.0
    );
    *sample = (struct foc_motor_id_sample){
        .setup_id = fit->config.setup_id,
        .capture_id = fit->result.capture_id,
        .source = FOC_MOTOR_ID_VIRTUAL,
        .sequence = k,
        .captured_us = end_us,
        .voltage_begin_us = begin_us,
        .voltage_end_us = end_us,
        .voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
        .mean_voltage_v = applied_v,
        .current_valid = true,
        .voltage_valid = true,
        .output_confirmed = k != 0U,
        .frame_confirmed = true,
        .stationary_confirmed = true,
    };
    CHECK(read_current(motor, profile, sample, axis, noise_state) == 0);
    return 0;
}

static void print_study(const char *label, uint32_t seed, const struct foc_motor_id_result *result)
{
    printf(
        "QUALITY,%s,%u,%u,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g\n",
        label,
        (unsigned int)seed,
        (unsigned int)result->axis,
        (unsigned int)result->failure,
        (unsigned int)result->usable,
        (unsigned int)result->samples,
        (unsigned int)result->windows,
        (unsigned int)result->voltage_source,
        result->rs_ohm,
        result->inductance_h,
        result->residual_rms_v,
        result->excitation
    );
}

static int test_voltage_capture(enum foc_motor_id_axis axis, uint32_t seed, float terminal_gain)
{
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    const struct foc_motor_id_config config = fit_config(axis, seed);
    struct foc_virtual_motor ideal_motor;
    struct foc_virtual_motor derated_motor;
    CHECK(init_study_motor(&ideal_motor, &profile) == 0);
    CHECK(init_study_motor(&derated_motor, &profile) == 0);
    struct foc_motor_id_context ideal = {0};
    struct foc_motor_id_context terminal = {0};
    struct foc_motor_id_context request = {0};
    struct foc_motor_id_context mislabeled = {0};
    struct foc_motor_id_context wrong_window = {0};
    CHECK(foc_motor_id_init(&ideal, &config, 10U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&terminal, &config, 11U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&request, &config, 12U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&mislabeled, &config, 13U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&wrong_window, &config, 14U) == FOC_MOTOR_ID_OK);
    struct foc_motor_id_sample ideal_sample = {0};
    struct foc_motor_id_sample terminal_sample = {0};
    uint32_t ideal_noise = seed;
    uint32_t derated_noise = seed;
    for (unsigned int k = 0U; k <= 768U; ++k) {
        const float requested_v = k == 0U ? 0.0f : excitation(k - 1U);
        /* Deliberately test a voltage transfer gain error, not a device loss fit. */
        CHECK(
            observe_study_axis(
                &ideal_motor,
                &profile,
                &ideal,
                k,
                &ideal_noise,
                requested_v,
                &ideal_sample
            ) == 0
        );
        CHECK(
            observe_study_axis(
                &derated_motor,
                &profile,
                &terminal,
                k,
                &derated_noise,
                terminal_gain * requested_v,
                &terminal_sample
            ) == 0
        );
        CHECK(foc_motor_id_push(&ideal, &ideal_sample) == FOC_MOTOR_ID_OK);
        CHECK(foc_motor_id_push(&terminal, &terminal_sample) == FOC_MOTOR_ID_OK);
        struct foc_motor_id_sample raw_request = terminal_sample;
        raw_request.mean_voltage_v = requested_v;
        if (k <= 1U) {
            raw_request.capture_id = 12U;
            raw_request.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
            CHECK(
                foc_motor_id_push(&request, &raw_request) ==
                (k == 0U ? FOC_MOTOR_ID_OK : FOC_MOTOR_ID_ERR_PRECONDITION)
            );
        }
        /* Counterfactual: the acquisition layer falsely vouches for the request. */
        raw_request.capture_id = 13U;
        raw_request.voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL;
        CHECK(foc_motor_id_push(&mislabeled, &raw_request) == FOC_MOTOR_ID_OK);
        if (k <= 17U) {
            struct foc_motor_id_sample late = terminal_sample;
            late.capture_id = 14U;
            if (k == 17U) {
                late.voltage_begin_us -= 5U;
                late.voltage_end_us -= 5U;
            }
            CHECK(
                foc_motor_id_push(&wrong_window, &late) ==
                (k == 17U ? FOC_MOTOR_ID_ERR_SAMPLE : FOC_MOTOR_ID_OK)
            );
        }
    }
    struct foc_motor_id_result a;
    struct foc_motor_id_result b;
    struct foc_motor_id_result c;
    struct foc_motor_id_result false_fit;
    struct foc_motor_id_result shifted;
    CHECK(foc_motor_id_finish(&ideal, &a) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_finish(&request, &b) == FOC_MOTOR_ID_ERR_QUALITY);
    CHECK(foc_motor_id_finish(&terminal, &c) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_finish(&mislabeled, &false_fit) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_finish(&wrong_window, &shifted) == FOC_MOTOR_ID_ERR_QUALITY);
    CHECK(!b.usable && b.failure == FOC_CALIBRATION_FAILURE_PRECONDITION && b.samples == 1U);
    CHECK(
        !shifted.usable && shifted.failure == FOC_CALIBRATION_FAILURE_TIME_ORDER &&
        shifted.samples == 17U && shifted.windows == 2U
    );
    CHECK(!b.rs_ohm && !b.inductance_h && !shifted.rs_ohm && !shifted.inductance_h);
    const double l_h = axis == FOC_MOTOR_ID_D ? profile.motor.ld_h : profile.motor.lq_h;
    CHECK(a.samples == 769U && c.samples == 769U && a.windows == 96U && c.windows == 96U);
    CHECK(relative_error(a.rs_ohm, profile.motor.rs_ohm) < 0.02);
    CHECK(relative_error(a.inductance_h, l_h) < 0.02);
    CHECK(relative_error(c.rs_ohm, profile.motor.rs_ohm) < 0.02);
    CHECK(relative_error(c.inductance_h, l_h) < 0.02);
    CHECK(c.residual_rms_v < 0.018 && c.excitation >= 0.02);
    CHECK(relative_error(false_fit.rs_ohm, profile.motor.rs_ohm / terminal_gain) < 0.02);
    CHECK(relative_error(false_fit.inductance_h, l_h / terminal_gain) < 0.02);
    CHECK(false_fit.residual_rms_v < 0.006);
    print_study("ideal-terminal", seed, &a);
    print_study("request-rejected", seed, &b);
    print_study("derated-terminal", seed, &c);
    print_study("request-mislabeled", seed, &false_fit);
    print_study("wrong-window", seed, &shifted);
    return 0;
}

static int test_frame_alignment(void)
{
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    const uint32_t seed = 123U;
    const float angle_error_rad = 0.4f;
    const float small_error_rad = 0.1f;
    struct foc_virtual_motor d_motor;
    struct foc_virtual_motor q_motor;
    CHECK(init_study_motor(&d_motor, &profile) == 0);
    CHECK(init_study_motor(&q_motor, &profile) == 0);
    const struct foc_motor_id_config d_config = fit_config(FOC_MOTOR_ID_D, 81U);
    const struct foc_motor_id_config q_config = fit_config(FOC_MOTOR_ID_Q, 81U);
    struct foc_motor_id_context aligned = {0};
    struct foc_motor_id_context quadrature = {0};
    struct foc_motor_id_context unconfirmed = {0};
    struct foc_motor_id_context offset = {0};
    struct foc_motor_id_context small_offset = {0};
    CHECK(foc_motor_id_init(&aligned, &d_config, 15U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&quadrature, &q_config, 16U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&unconfirmed, &d_config, 17U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&offset, &d_config, 18U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&small_offset, &d_config, 19U) == FOC_MOTOR_ID_OK);
    struct foc_motor_id_sample d = {0};
    struct foc_motor_id_sample q = {0};
    uint32_t d_noise = seed;
    uint32_t q_noise = seed + 1U;
    const float sine = sinf(angle_error_rad);
    const float cosine = cosf(angle_error_rad);
    const float small_sine = sinf(small_error_rad);
    const float small_cosine = cosf(small_error_rad);
    for (unsigned int k = 0U; k <= 768U; ++k) {
        const float vd = k == 0U ? 0.0f : excitation(k - 1U);
        const float vq = k == 0U ? 0.0f : excitation(k + 71U);
        /* Superpose two independent RK4 axes at fixed rotor angle. Each
         * separate plant stays at rest; only the observation frame is rotated. */
        CHECK(observe_study_axis(&d_motor, &profile, &aligned, k, &d_noise, vd, &d) == 0);
        CHECK(observe_study_axis(&q_motor, &profile, &quadrature, k, &q_noise, vq, &q) == 0);
        CHECK(foc_motor_id_push(&aligned, &d) == FOC_MOTOR_ID_OK);
        CHECK(foc_motor_id_push(&quadrature, &q) == FOC_MOTOR_ID_OK);
        struct foc_motor_id_sample rotated = d;
        rotated.current_a = cosine * d.current_a + sine * q.current_a;
        rotated.mean_voltage_v = cosine * d.mean_voltage_v + sine * q.mean_voltage_v;
        if (k <= 1U) {
            rotated.capture_id = 17U;
            rotated.frame_confirmed = k == 0U;
            CHECK(
                foc_motor_id_push(&unconfirmed, &rotated) ==
                (k == 0U ? FOC_MOTOR_ID_OK : FOC_MOTOR_ID_ERR_PRECONDITION)
            );
        }
        rotated.capture_id = 18U;
        rotated.frame_confirmed = true; /* Incorrect external assertion. */
        CHECK(foc_motor_id_push(&offset, &rotated) == FOC_MOTOR_ID_OK);
        rotated.capture_id = 19U;
        rotated.current_a = small_cosine * d.current_a + small_sine * q.current_a;
        rotated.mean_voltage_v = small_cosine * d.mean_voltage_v + small_sine * q.mean_voltage_v;
        CHECK(foc_motor_id_push(&small_offset, &rotated) == FOC_MOTOR_ID_OK);
    }
    struct foc_motor_id_result d_fit;
    struct foc_motor_id_result q_fit;
    struct foc_motor_id_result bad_frame;
    struct foc_motor_id_result wrong_axis;
    struct foc_motor_id_result small_angle;
    CHECK(foc_motor_id_finish(&aligned, &d_fit) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_finish(&quadrature, &q_fit) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_finish(&unconfirmed, &bad_frame) == FOC_MOTOR_ID_ERR_QUALITY);
    CHECK(!bad_frame.usable && bad_frame.failure == FOC_CALIBRATION_FAILURE_PRECONDITION);
    const int finish = foc_motor_id_finish(&offset, &wrong_axis);
    print_study("aligned-d", seed, &d_fit);
    print_study("aligned-q", seed + 1U, &q_fit);
    print_study("frame-unconfirmed", seed, &bad_frame);
    print_study("frame-mislabeled-0.4rad", seed, &wrong_axis);
    CHECK(foc_motor_id_finish(&small_offset, &small_angle) == FOC_MOTOR_ID_OK);
    CHECK(small_angle.usable && small_angle.residual_rms_v > d_fit.residual_rms_v);
    CHECK(small_angle.rs_ohm != d_fit.rs_ohm && small_angle.inductance_h != d_fit.inductance_h);
    print_study("frame-mislabeled-0.1rad", seed, &small_angle);
    CHECK(
        finish == FOC_MOTOR_ID_ERR_QUALITY && wrong_axis.failure == FOC_CALIBRATION_FAILURE_RESIDUAL
    );
    struct foc_motor_rl_model model;
    CHECK(foc_motor_id_make_model(&d_fit, &q_fit, 0.02f, &model) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_make_model(&bad_frame, &q_fit, 0.02f, &model) == FOC_MOTOR_ID_ERR_QUALITY);
    const struct foc_motor_pi_request request = {150.0f, 0.0002f, 0.0002f, 0.65f, 1.2f};
    struct foc_motor_pi_candidate pi;
    CHECK(foc_motor_pi_design(&model, &request, &pi) == FOC_MOTOR_ID_ERR_CONFIG);
    CHECK(pi.kp_d_v_per_a == 0.0f && pi.kp_q_v_per_a == 0.0f && pi.ki_v_per_a_s == 0.0f);
    return 0;
}

static int emit_trace(
    const char *profile,
    const char *segment,
    struct foc_virtual_current *context,
    bool trace
)
{
    struct foc_virtual_current_observation o;
    CHECK(foc_virtual_current_observe(context, &o) == 0);
    CHECK(isfinite(o.iq_a) && isfinite(o.id_a));
    const struct foc_vector_dq integral = context->runtime.pipeline.current.integral_v;
    CHECK(isfinite(integral.d) && isfinite(integral.q));
    if (trace) {
        printf(
            "PI,%s,%s,%u,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u,%.9g,%.9g\n",
            profile,
            segment,
            (unsigned int)o.virtual_time_us,
            o.id_ref_a,
            o.iq_ref_a,
            o.id_a,
            o.iq_a,
            (unsigned int)o.safety_state,
            (unsigned int)context->output.request.action,
            (unsigned int)o.faults,
            (unsigned int)o.accepted_updates,
            (unsigned int)o.rejected_updates,
            (unsigned int)o.saturated_updates,
            integral.d,
            integral.q
        );
    }
    return 0;
}

static int compare_pi(const char *name, const struct foc_motor_pi_candidate *candidate, bool trace)
{
    struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    profile.current.pwm.voltage_limit_v = 1.2f;
    if (candidate != NULL) {
        profile.current.kp_v_per_a =
            (struct foc_vector_dq){candidate->kp_d_v_per_a, candidate->kp_q_v_per_a};
        profile.current.ki_v_per_a_s =
            (struct foc_vector_dq){candidate->ki_v_per_a_s, candidate->ki_v_per_a_s};
    }
    struct foc_virtual_current context;
    CHECK(foc_virtual_current_init(&context, &profile) == 0);
    CHECK(foc_virtual_current_publish_iq(&context, 0.1f, 0.45f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&context, 0U) == 0);
    CHECK(foc_virtual_current_queue_action(&context, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(&context, 200U) == 0);
    CHECK(foc_virtual_current_queue_action(&context, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(&context, 400U) == 0);
    CHECK(emit_trace(name, "startup", &context, trace) == 0);
    const struct {
        const char *name;
        float d;
        float q;
        uint32_t until_us;
    } segments[] = {
        {"positive", 0.1f, 0.45f, 10400U},
        {"negative", -0.1f, -0.45f, 20400U},
        {"saturation", 0.1f, 2.35f, 27400U},
        {"recovery", 0.0f, 0.15f, 44400U},
    };
    for (unsigned int i = 0U; i < sizeof segments / sizeof segments[0]; ++i) {
        const uint32_t begin = context.virtual_time_us;
        const uint32_t saturated_before = context.output.saturated_updates;
        for (uint32_t t = begin; t < segments[i].until_us; t += profile.runtime.h0.period_us) {
            if ((t - begin) % 1000U == 0U) {
                CHECK(
                    foc_virtual_current_publish_iq(&context, segments[i].d, segments[i].q) ==
                    FOC_RUNTIME_OK
                );
            }
            const uint32_t accepted = context.output.accepted_updates;
            CHECK(foc_virtual_current_run_until(&context, t + profile.runtime.h0.period_us) == 0);
            CHECK(context.output.accepted_updates == accepted + 1U);
            CHECK(context.output.mode == FOC_VIRTUAL_OUTPUT_UPDATE);
            CHECK(
                context.runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
                context.runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_NONE
            );
            CHECK(emit_trace(name, segments[i].name, &context, trace) == 0);
        }
        if (i == 2U) {
            CHECK(context.output.saturated_updates > saturated_before);
        } else if (i == 3U) {
            struct foc_virtual_current_observation observation;
            CHECK(foc_virtual_current_observe(&context, &observation) == 0);
            CHECK(fabsf(observation.iq_a - segments[i].q) < 0.1f);
            CHECK(fabsf(observation.id_a) < 0.1f);
        }
    }
    const uint32_t accepted = context.output.accepted_updates;
    CHECK(foc_virtual_current_request_stop(&context) == FOC_RUNTIME_OK);
    CHECK(
        foc_virtual_current_run_until(
            &context,
            context.virtual_time_us + profile.runtime.h0.period_us
        ) == 0
    );
    CHECK(
        context.runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED &&
        context.runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_NONE &&
        context.output.mode == FOC_VIRTUAL_OUTPUT_HIGH_Z
    );
    CHECK(context.output.accepted_updates == accepted && context.output.rejected_updates == 0U);
    CHECK(emit_trace(name, "stop", &context, trace) == 0);
    return 0;
}

/* Test Entry ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const bool trace = argc == 2 && strcmp(argv[1], "--trace") == 0;
    const bool quality = argc == 2 && strcmp(argv[1], "--quality") == 0;
    CHECK(argc == 1 || trace || quality);
    if (!trace) {
        if (quality) {
            puts(
                "record,case,seed,axis,failure,usable,samples,windows,configured_voltage_source,"
                "rs_ohm,inductance_h,residual_rms_v,excitation"
            );
        }
        static const uint32_t seeds[] = {123U, 2027U, 5179U};
        for (unsigned int k = 0U; k < sizeof seeds / sizeof seeds[0]; ++k) {
            CHECK(test_voltage_capture(FOC_MOTOR_ID_D, seeds[k], 0.8f) == 0);
            CHECK(test_voltage_capture(FOC_MOTOR_ID_Q, seeds[k], 0.8f) == 0);
        }
        CHECK(test_frame_alignment() == 0);
        if (quality) {
            return 0;
        }
    }
    const struct fit_scenario scenarios[] = {
        {"nominal", {0.6f, 0.0012f, 0.0015f}, 50U, IDEAL},
        {"quantized-80", {0.42f, 0.00085f, 0.0020f}, 80U, QUANTIZED},
        {"quantized-120", {0.92f, 0.0020f, 0.00095f}, 120U, QUANTIZED},
        {"quiet", {0.6f, 0.0012f, 0.0015f}, 50U, NO_EXCITATION},
        {"clipped", {0.6f, 0.0012f, 0.0015f}, 50U, CLIPPED},
        {"free-rotor", {0.6f, 0.0012f, 0.0015f}, 50U, MOVING},
    };
    struct foc_motor_rl_model nominal;
    for (unsigned int i = 0U; i < 3U; ++i) {
        struct foc_motor_id_result d;
        struct foc_motor_id_result q;
        struct foc_motor_rl_model model;
        CHECK(capture_axis(&scenarios[i], FOC_MOTOR_ID_D, 2U * i, &d) == 0);
        CHECK(capture_axis(&scenarios[i], FOC_MOTOR_ID_Q, 2U * i + 1U, &q) == 0);
        CHECK(foc_motor_id_make_model(&d, &q, 0.02f, &model) == FOC_MOTOR_ID_OK);
        if (i == 0U) {
            nominal = model;
        }
    }
    for (unsigned int i = 3U; i < 6U; ++i) {
        struct foc_motor_id_result result;
        CHECK(capture_axis(&scenarios[i], FOC_MOTOR_ID_Q, 2U * i, &result) == 0);
    }
    const struct foc_motor_pi_request request = {150.0f, 0.0002f, 0.0002f, 0.65f, 1.2f};
    struct foc_motor_pi_candidate truth;
    struct foc_motor_pi_candidate fitted;
    CHECK(foc_motor_pi_design(&scenarios[0].truth, &request, &truth) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_pi_design(&nominal, &request, &fitted) == FOC_MOTOR_ID_OK);
    printf(
        "GAINS,truth,%.9g,%.9g,%.9g,%.9g,%.9g\n",
        truth.kp_d_v_per_a,
        truth.kp_q_v_per_a,
        truth.ki_v_per_a_s,
        truth.estimated_initial_voltage_v,
        truth.estimated_delay_phase_rad
    );
    printf(
        "GAINS,fitted,%.9g,%.9g,%.9g,%.9g,%.9g\n",
        fitted.kp_d_v_per_a,
        fitted.kp_q_v_per_a,
        fitted.ki_v_per_a_s,
        fitted.estimated_initial_voltage_v,
        fitted.estimated_delay_phase_rad
    );
    CHECK(compare_pi("fixed", NULL, trace) == 0);
    CHECK(compare_pi("truth", &truth, trace) == 0);
    CHECK(compare_pi("fitted", &fitted, trace) == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
