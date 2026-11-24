/**
 * @file    test_foc_motor_id.c
 * @brief   Standstill and back-EMF fits, quality rejection and PI policy contracts.
 * @version v0.1.0
 * @date    2026.11.17
 *
 * Test role
 * ---------
 * Exercise fit, publication and rejection through the public calibration API.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_motor_id.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                       \
    do {                                                       \
        if (!(condition)) {                                    \
            fprintf(stderr, "%d: %s\n", __LINE__, #condition); \
            return 1;                                          \
        }                                                      \
    } while (0)

static const struct foc_motor_id_config config = {
    .axis = FOC_MOTOR_ID_D,
    .source = FOC_MOTOR_ID_VIRTUAL,
    .voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
    .setup_id = 71U,
    .min_interval_us = 25U,
    .max_interval_us = 50U,
    .intervals_per_window = 4U,
    .min_windows = 6U,
    .max_abs_current_a = 3.0f,
    .max_abs_voltage_v = 45.0f,
    .min_current_rms_a = 0.1f,
    .min_slew_rms_a_per_s = 5.0f,
    .min_voltage_rms_v = 0.05f,
    .max_residual_rms_v = 0.04f,
    .min_rs_ohm = 0.03f,
    .max_rs_ohm = 2.0f,
    .min_inductance_h = 0.0004f,
    .max_inductance_h = 0.01f,
};

static struct foc_motor_id_sample
sample_at(const struct foc_motor_id_context *context, unsigned int index)
{
    return (struct foc_motor_id_sample){
        .setup_id = context->config.setup_id,
        .capture_id = context->result.capture_id,
        .source = context->config.source,
        .sequence = UINT32_MAX - 1U + index,
        .captured_us = UINT32_MAX - 80U + 40U * index,
        .voltage_begin_us = UINT32_MAX - 120U + 40U * index,
        .voltage_end_us = UINT32_MAX - 80U + 40U * index,
        .voltage_source = context->config.voltage_source,
        .output_confirmed = true,
        .frame_confirmed = true,
        .current_valid = true,
        .voltage_valid = true,
        .stationary_confirmed = true,
    };
}

static int collect(struct foc_motor_id_context *context, float rs, float l, bool corrupt)
{
    static const float endpoints[] = {0.0f, 0.2f, 1.0f, -0.25f, -1.0f, 0.7f, -0.4f};
    float previous = endpoints[0];
    for (unsigned int k = 0U; k <= 48U; ++k) {
        const unsigned int segment = k / 4U;
        const float start = endpoints[segment % 7U];
        const float end = endpoints[(segment + 1U) % 7U];
        struct foc_motor_id_sample sample = sample_at(context, k);
        sample.current_a = start + (end - start) * (float)(k % 4U) / 4.0f;
        sample.mean_voltage_v = rs * (sample.current_a + previous) * 0.5f +
            l * (sample.current_a - previous) * 25000.0f;
        if (corrupt && segment % 3U == 2U) {
            sample.mean_voltage_v += 0.8f;
        }
        CHECK(foc_motor_id_push(context, &sample) == FOC_MOTOR_ID_OK);
        previous = sample.current_a;
    }
    return 0;
}

static int test_fit(void)
{
    struct foc_motor_id_context d = {0};
    struct foc_motor_id_context q = {0};
    struct foc_motor_id_config q_config = config;
    q_config.axis = FOC_MOTOR_ID_Q;
    CHECK(foc_motor_id_init(&d, &config, 12U) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&q, &q_config, 13U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&d, 0.6f, 0.0012f, false) == 0);
    CHECK(q.result.samples == 0U && q.result.capture_id == 13U);
    CHECK(collect(&q, 0.6f, 0.0015f, false) == 0);
    struct foc_motor_id_result dr;
    struct foc_motor_id_result qr;
    CHECK(foc_motor_id_finish(&d, &dr) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_finish(&q, &qr) == FOC_MOTOR_ID_OK);
    CHECK(dr.usable && dr.samples == 49U && dr.windows == 12U && dr.capture_id == 12U);
    CHECK(dr.first_sequence == UINT32_MAX - 1U && dr.last_sequence == 46U);
    CHECK(dr.first_us == UINT32_MAX - 80U && dr.last_us == 1839U);
    CHECK(fabsf(dr.rs_ohm - 0.6f) < 1.0e-6f && fabsf(dr.inductance_h - 0.0012f) < 1.0e-8f);
    CHECK(fabsf(qr.inductance_h - 0.0015f) < 1.0e-8f && qr.residual_rms_v < 1.0e-5);
    struct foc_motor_rl_model model;
    CHECK(foc_motor_id_make_model(&dr, &qr, 0.02f, &model) == FOC_MOTOR_ID_OK);
    CHECK(fabsf(model.rs_ohm - 0.6f) < 1.0e-6f && model.ld_h != model.lq_h);
    struct foc_motor_id_result repeat;
    CHECK(foc_motor_id_reset(&d, 12U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&d, 0.6f, 0.0012f, false) == 0);
    CHECK(foc_motor_id_finish(&d, &repeat) == FOC_MOTOR_ID_OK);
    CHECK(
        repeat.rs_ohm == dr.rs_ohm && repeat.inductance_h == dr.inductance_h &&
        repeat.residual_rms_v == dr.residual_rms_v && repeat.excitation == dr.excitation
    );
    CHECK(dr.setup_id == config.setup_id && dr.voltage_source == config.voltage_source);
    qr.setup_id++;
    CHECK(foc_motor_id_make_model(&dr, &qr, 0.02f, &model) == FOC_MOTOR_ID_ERR_QUALITY);
    qr.setup_id = dr.setup_id;
    qr.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
    CHECK(foc_motor_id_make_model(&dr, &qr, 0.02f, &model) == FOC_MOTOR_ID_ERR_QUALITY);
    qr.voltage_source = dr.voltage_source;
    qr.source = FOC_MOTOR_ID_MEASURED;
    CHECK(foc_motor_id_make_model(&dr, &qr, 0.02f, &model) == FOC_MOTOR_ID_ERR_QUALITY);
    qr.source = FOC_MOTOR_ID_VIRTUAL;
    qr.rs_ohm = 0.8f;
    CHECK(foc_motor_id_make_model(&dr, &qr, 0.02f, &model) == FOC_MOTOR_ID_ERR_QUALITY);
    CHECK(model.rs_ohm == 0.0f && model.ld_h == 0.0f && model.lq_h == 0.0f);
    CHECK(foc_motor_id_reset(&d, 14U) == FOC_MOTOR_ID_OK);
    CHECK(d.result.capture_id == 14U && !d.result.usable && d.result.samples == 0U);
    CHECK(q.result.usable && q.result.capture_id == 13U);
    return 0;
}

static int
expect_failure(struct foc_motor_id_context *context, enum foc_calibration_failure failure)
{
    struct foc_motor_id_result result = {.usable = true, .rs_ohm = 99.0f, .inductance_h = 99.0f};
    CHECK(foc_motor_id_finish(context, &result) != FOC_MOTOR_ID_OK);
    CHECK(result.state == FOC_CALIBRATION_STATE_INVALID && result.failure == failure);
    CHECK(!result.usable && result.rs_ohm == 0.0f && result.inductance_h == 0.0f);
    return 0;
}

static int test_acquisition_resolution(void)
{
    /* Wide bounds are accepted; resolution is judged on the fitted pair. */
    struct foc_motor_id_context context = {0};
    struct foc_motor_id_config wide = config;
    wide.min_inductance_h = 1.0e-6f;
    wide.max_rs_ohm = 10.0f;
    CHECK(foc_motor_id_init(&context, &wide, 1U) == FOC_MOTOR_ID_OK);
    /* 50 us x 1.0 Ohm / 0.5 mH = 0.1: resolved, the fit passes. */
    CHECK(collect(&context, 1.0f, 0.0005f, false) == 0);
    struct foc_motor_id_result result;
    CHECK(foc_motor_id_finish(&context, &result) == FOC_MOTOR_ID_OK && result.usable);
    /* 50 us x 2.0 Ohm / 0.2 mH = 0.5: the pole is not resolved, the fit is refused. */
    CHECK(foc_motor_id_init(&context, &wide, 2U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 2.0f, 0.0002f, false) == 0);
    CHECK(foc_motor_id_finish(&context, &result) != FOC_MOTOR_ID_OK);
    CHECK(result.failure == FOC_CALIBRATION_FAILURE_RANGE);
    return 0;
}

static int test_invalid_samples(void)
{
    struct foc_motor_id_context context = {0};
    CHECK(foc_motor_id_init(&context, &config, 1U) == FOC_MOTOR_ID_OK);
    const enum foc_calibration_failure expected[] = {
        FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
        FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
        FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
        FOC_CALIBRATION_FAILURE_INVALID_SAMPLE,
        FOC_CALIBRATION_FAILURE_PRECONDITION,
        FOC_CALIBRATION_FAILURE_PRECONDITION,
        FOC_CALIBRATION_FAILURE_PRECONDITION,
        FOC_CALIBRATION_FAILURE_TIME_ORDER,
        FOC_CALIBRATION_FAILURE_TIME_ORDER,
        FOC_CALIBRATION_FAILURE_TIME_ORDER,
        FOC_CALIBRATION_FAILURE_RANGE,
    };
    for (unsigned int k = 0U; k < sizeof expected / sizeof expected[0]; ++k) {
        CHECK(foc_motor_id_reset(&context, k) == FOC_MOTOR_ID_OK);
        struct foc_motor_id_sample sample = sample_at(&context, 0U);
        CHECK(foc_motor_id_push(&context, &sample) == FOC_MOTOR_ID_OK);
        sample = sample_at(&context, 1U);
        switch (k) {
        case 0:
            sample.current_a = NAN;
            break;
        case 1:
            sample.mean_voltage_v = INFINITY;
            break;
        case 2:
            sample.current_valid = false;
            break;
        case 3:
            sample.voltage_valid = false;
            break;
        case 4:
            sample.omega_e_rad_s = 0.01f;
            break;
        case 5:
            sample.stationary_confirmed = false;
            break;
        case 6:
            sample.saturated = true;
            break;
        case 7:
            sample.sequence++;
            break;
        case 8:
            sample.captured_us = sample_at(&context, 0U).captured_us;
            break;
        case 9:
            sample.captured_us += 2000U;
            break;
        case 10:
            sample.current_a = FLT_MAX;
            break;
        }
        CHECK(foc_motor_id_push(&context, &sample) != FOC_MOTOR_ID_OK);
        CHECK(context.result.samples == 1U);
        CHECK(foc_motor_id_push(&context, &sample) == FOC_MOTOR_ID_ERR_STATE);
        CHECK(expect_failure(&context, expected[k]) == 0);
    }
    return 0;
}

static int test_observation_contract(void)
{
    struct foc_motor_id_context context = {0};
    CHECK(foc_motor_id_init(&context, &config, 1U) == FOC_MOTOR_ID_OK);
    const enum foc_calibration_failure expected[] = {
        FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
        FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
        FOC_CALIBRATION_FAILURE_INPUT_CHANGED,
        FOC_CALIBRATION_FAILURE_PRECONDITION,
        FOC_CALIBRATION_FAILURE_PRECONDITION,
        FOC_CALIBRATION_FAILURE_PRECONDITION,
        FOC_CALIBRATION_FAILURE_PRECONDITION,
        FOC_CALIBRATION_FAILURE_TIME_ORDER,
        FOC_CALIBRATION_FAILURE_TIME_ORDER,
    };
    for (unsigned int k = 0U; k < sizeof expected / sizeof expected[0]; ++k) {
        CHECK(foc_motor_id_reset(&context, k) == FOC_MOTOR_ID_OK);
        struct foc_motor_id_sample sample = sample_at(&context, 0U);
        sample.voltage_valid = false;
        sample.output_confirmed = false;
        sample.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
        CHECK(foc_motor_id_push(&context, &sample) == FOC_MOTOR_ID_OK);
        for (unsigned int i = 1U; i < 4U; ++i) {
            sample = sample_at(&context, i);
            sample.current_a = 0.25f * (float)i;
            sample.mean_voltage_v = 0.5f;
            CHECK(foc_motor_id_push(&context, &sample) == FOC_MOTOR_ID_OK);
        }
        const struct foc_motor_id_context prior = context;
        sample = sample_at(&context, 4U);
        switch (k) {
        case 0:
            sample.capture_id++;
            break;
        case 1:
            sample.setup_id++;
            break;
        case 2:
            sample.source = FOC_MOTOR_ID_MEASURED;
            break;
        case 3:
            sample.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
            break;
        case 4:
            sample.voltage_source = FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL;
            break;
        case 5:
            sample.output_confirmed = false;
            break;
        case 6:
            sample.frame_confirmed = false;
            break;
        case 7:
            sample.voltage_begin_us++;
            break;
        case 8:
            sample.voltage_end_us--;
            break;
        }
        CHECK(foc_motor_id_push(&context, &sample) != FOC_MOTOR_ID_OK);
        CHECK(
            context.result.samples == prior.result.samples &&
            context.result.last_us == prior.result.last_us &&
            context.result.windows == prior.result.windows &&
            context.intervals == prior.intervals && context.window_us == prior.window_us &&
            context.current_as == prior.current_as && context.voltage_vs == prior.voltage_vs
        );
        CHECK(expect_failure(&context, expected[k]) == 0);
    }
    CHECK(foc_motor_id_reset(&context, 19U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 0.6f, 0.0012f, false) == 0);
    struct foc_motor_id_result result;
    CHECK(foc_motor_id_finish(&context, &result) == FOC_MOTOR_ID_OK);
    CHECK(result.usable && result.capture_id == 19U && result.samples == 49U);
    return 0;
}

static int test_voltage_policy(void)
{
    struct foc_motor_id_context context = {0};
    struct foc_motor_id_config measured = config;
    measured.source = FOC_MOTOR_ID_MEASURED;
    CHECK(foc_motor_id_init(&context, &measured, 1U) == FOC_MOTOR_ID_ERR_CONFIG);
    measured.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
    CHECK(foc_motor_id_init(&context, &measured, 1U) == FOC_MOTOR_ID_ERR_CONFIG);
    measured.voltage_source = FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL;
    CHECK(foc_motor_id_init(&context, &measured, 1U) == FOC_MOTOR_ID_OK);
    struct foc_motor_id_sample sample = sample_at(&context, 0U);
    CHECK(foc_motor_id_push(&context, &sample) == FOC_MOTOR_ID_OK);
    sample = sample_at(&context, 1U);
    sample.voltage_source = FOC_MOTOR_ID_VOLTAGE_CALIBRATED_TERMINAL;
    CHECK(foc_motor_id_push(&context, &sample) == FOC_MOTOR_ID_ERR_PRECONDITION);
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_PRECONDITION) == 0);
    measured.voltage_source = FOC_MOTOR_ID_VOLTAGE_CALIBRATED_TERMINAL;
    CHECK(foc_motor_id_init(&context, &measured, 2U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 0.6f, 0.0012f, false) == 0);
    struct foc_motor_id_result result;
    CHECK(foc_motor_id_finish(&context, &result) == FOC_MOTOR_ID_OK);
    CHECK(
        result.source == FOC_MOTOR_ID_MEASURED && result.voltage_source == measured.voltage_source
    );
    measured.setup_id = 0U;
    CHECK(foc_motor_id_init(&context, &measured, 3U) == FOC_MOTOR_ID_ERR_CONFIG);
    CHECK(context.result.usable && context.result.capture_id == 2U);
    return 0;
}

static int test_fit_rejection(void)
{
    struct foc_motor_id_context context = {0};
    CHECK(foc_motor_id_init(&context, &config, 1U) == FOC_MOTOR_ID_OK);
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_SAMPLE_COUNT) == 0);
    CHECK(foc_motor_id_reset(&context, 2U) == FOC_MOTOR_ID_OK);
    for (unsigned int k = 0U; k <= 48U; ++k) {
        struct foc_motor_id_sample sample = sample_at(&context, k);
        sample.current_a = 0.5f;
        sample.mean_voltage_v = 0.3f;
        CHECK(foc_motor_id_push(&context, &sample) == FOC_MOTOR_ID_OK);
    }
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE) == 0);
    CHECK(foc_motor_id_reset(&context, 3U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 4.0f, 0.0012f, false) == 0);
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_RANGE) == 0);
    CHECK(foc_motor_id_reset(&context, 4U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, -0.6f, 0.0012f, false) == 0);
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_RANGE) == 0);
    CHECK(foc_motor_id_reset(&context, 5U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 0.6f, 0.0012f, true) == 0);
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_RESIDUAL) == 0);

    /* A single exponential has nonzero current and slew but proportional
     * regression columns for equal-duration windows. */
    CHECK(foc_motor_id_reset(&context, 6U) == FOC_MOTOR_ID_OK);
    float current = 0.25f;
    for (unsigned int k = 0U; k <= 48U; ++k) {
        struct foc_motor_id_sample sample = sample_at(&context, k);
        sample.current_a = current;
        sample.mean_voltage_v = current;
        CHECK(foc_motor_id_push(&context, &sample) == FOC_MOTOR_ID_OK);
        current *= 1.04f;
    }
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE) == 0);

    CHECK(foc_motor_id_reset(&context, 7U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 0.6f, 0.0012f, false) == 0);
    const struct foc_motor_id_sample tail = sample_at(&context, 49U);
    CHECK(foc_motor_id_push(&context, &tail) == FOC_MOTOR_ID_OK);
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_SAMPLE_COUNT) == 0);

    struct foc_motor_id_config strict = config;
    strict.max_residual_rms_v = 1.0e-20f;
    CHECK(foc_motor_id_init(&context, &strict, 7U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 0.6f, 0.0012f, false) == 0);
    CHECK(expect_failure(&context, FOC_CALIBRATION_FAILURE_RESIDUAL) == 0);
    return 0;
}

static int test_pi(void)
{
    struct foc_motor_rl_model model = {0.6f, 0.0012f, 0.0015f};
    struct foc_motor_pi_request request = {150.0f, 0.0002f, 0.0002f, 0.65f, 1.2f};
    struct foc_motor_pi_candidate candidate;
    CHECK(foc_motor_pi_design(&model, &request, &candidate) == FOC_MOTOR_ID_OK);
    CHECK(fabsf(candidate.kp_d_v_per_a - 1.1309734f) < 1.0e-5f);
    CHECK(fabsf(candidate.kp_q_v_per_a - 1.4137167f) < 1.0e-5f);
    CHECK(fabsf(candidate.ki_v_per_a_s - 565.4867f) < 0.001f);
    CHECK(candidate.estimated_initial_voltage_v <= request.available_voltage_v);
    CHECK(
        candidate.estimated_delay_phase_rad > 0.28f && candidate.estimated_delay_phase_rad < 0.29f
    );
    request.bandwidth_hz = 900.0f;
    CHECK(foc_motor_pi_design(&model, &request, &candidate) == FOC_MOTOR_ID_ERR_QUALITY);
    CHECK(
        candidate.kp_d_v_per_a == 0.0f && candidate.kp_q_v_per_a == 0.0f &&
        candidate.ki_v_per_a_s == 0.0f
    );
    request.bandwidth_hz = 150.0f;
    request.update_delay_s = 0.001f;
    CHECK(foc_motor_pi_design(&model, &request, &candidate) == FOC_MOTOR_ID_ERR_QUALITY);
    request.update_delay_s = 0.0002f;
    request.available_voltage_v = 0.1f;
    CHECK(foc_motor_pi_design(&model, &request, &candidate) == FOC_MOTOR_ID_ERR_QUALITY);
    request.available_voltage_v = FLT_MAX;
    model.lq_h = FLT_MAX;
    CHECK(foc_motor_pi_design(&model, &request, &candidate) == FOC_MOTOR_ID_ERR_QUALITY);
    model.lq_h = 0.0001f;
    CHECK(foc_motor_pi_design(&model, &request, &candidate) == FOC_MOTOR_ID_ERR_QUALITY);
    model.lq_h = 0.0f;
    CHECK(foc_motor_pi_design(&model, &request, &candidate) == FOC_MOTOR_ID_ERR_CONFIG);
    model = (struct foc_motor_rl_model){1.0e-20f, 1.0e-20f, 1.0e-20f};
    request = (struct foc_motor_pi_request){1.0f, 1.0e-30f, 0.0f, 1.0f, 12.0f};
    CHECK(foc_motor_pi_design(&model, &request, &candidate) == FOC_MOTOR_ID_ERR_QUALITY);
    return 0;
}

/* Test Entry ----------------------------------------------------------------- */

/* Firmware-commanded voltage is a measured-origin category, never a virtual one. */
static int test_commanded_voltage_policy(void)
{
    struct foc_motor_id_context context;
    struct foc_motor_id_config commanded = config;
    commanded.source = FOC_MOTOR_ID_MEASURED;
    commanded.voltage_source = FOC_MOTOR_ID_VOLTAGE_COMMANDED;
    CHECK(foc_motor_id_init(&context, &commanded, 1U) == FOC_MOTOR_ID_OK);
    commanded.source = FOC_MOTOR_ID_VIRTUAL;
    CHECK(foc_motor_id_init(&context, &commanded, 2U) == FOC_MOTOR_ID_ERR_CONFIG);
    commanded.source = FOC_MOTOR_ID_MEASURED;
    commanded.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
    CHECK(foc_motor_id_init(&context, &commanded, 3U) == FOC_MOTOR_ID_ERR_CONFIG);
    return 0;
}

/* A q-axis pulse train on a free light rotor: di/dt = (v - R i - ke w) / L, J dw/dt = Kt i.
 * Integrated in 1 us steps; each 40 us interval holds one voltage. Returns the push status. */
static int collect_turning(
    struct foc_motor_id_context *context,
    bool claim_still,
    double inertia,
    double *peak
)
{
    static const float levels[8] = {1.0f, -1.0f, -1.0f, 1.0f, 0.5f, -0.5f, -0.5f, 0.5f};
    const double rs = 0.6;
    const double l = 0.0015;
    const double ke = 0.07;
    const double kt = 0.105;
    double current = 0.0;
    double speed = 0.0;
    double angle = 0.25;
    *peak = 0.0;
    for (unsigned int k = 0U; k <= 384U; ++k) {
        const float voltage = k == 0U ? 0.0f : 0.5f * levels[((k - 1U) / 24U) % 8U];
        for (unsigned int step = 0U; k != 0U && step < 40U; ++step) {
            const double di = (voltage - rs * current - ke * speed) / l;
            const double dw = kt * current / inertia;
            angle += (speed + 0.5 * dw * 1.0e-6) * 1.0e-6;
            current += di * 1.0e-6;
            speed += dw * 1.0e-6;
        }
        *peak = fmax(*peak, fabs(speed));
        struct foc_motor_id_sample sample = sample_at(context, k);
        sample.current_a = (float)current;
        sample.mean_voltage_v = voltage;
        sample.rotor_angle_rad = (float)angle;
        sample.omega_e_rad_s = claim_still ? 0.0f : (float)(7.0 * speed);
        sample.stationary_confirmed = claim_still;
        const int status = foc_motor_id_push(context, &sample);
        if (status != FOC_MOTOR_ID_OK) {
            return status;
        }
    }
    return FOC_MOTOR_ID_OK;
}

static int test_back_emf(void)
{
    struct foc_motor_id_config turning = config;
    turning.axis = FOC_MOTOR_ID_Q;
    turning.fit_back_emf = true;
    turning.max_abs_speed_rad_s = 50.0f;
    turning.min_speed_rms_rad_s = 0.05f;
    turning.max_rs_ohm = 2.0f;
    struct foc_motor_id_context context = {0};
    struct foc_motor_id_result result;
    double peak = 0.0;

    /* The shaft turns well past standstill; the three-term fit recovers R, L and ke. */
    CHECK(foc_motor_id_init(&context, &turning, 21U) == FOC_MOTOR_ID_OK);
    CHECK(collect_turning(&context, false, 2.0e-5, &peak) == 0);
    CHECK(peak > 1.0);
    CHECK(foc_motor_id_finish(&context, &result) == FOC_MOTOR_ID_OK && result.back_emf_fitted);
    CHECK(fabsf(result.rs_ohm / 0.6f - 1.0f) < 0.01f);
    CHECK(fabsf(result.inductance_h / 0.0015f - 1.0f) < 0.01f);
    CHECK(fabsf(result.back_emf_v_s_per_rad / 0.07f - 1.0f) < 0.02f);
    CHECK(result.excitation >= 0.02);

    /* With the d-axis R supplied, R is taken and L and ke are fitted. */
    struct foc_motor_id_config known = turning;
    known.known_rs_ohm = 0.6f;
    CHECK(foc_motor_id_init(&context, &known, 30U) == FOC_MOTOR_ID_OK);
    CHECK(collect_turning(&context, false, 2.0e-5, &peak) == 0);
    CHECK(foc_motor_id_finish(&context, &result) == FOC_MOTOR_ID_OK && result.rs_fixed);
    CHECK(result.rs_ohm == 0.6f && fabsf(result.inductance_h / 0.0015f - 1.0f) < 0.01f);
    CHECK(fabsf(result.back_emf_v_s_per_rad / 0.07f - 1.0f) < 0.02f);

    /* A very light rotor spins far faster; the supplied R still identifies L and ke. */
    CHECK(foc_motor_id_init(&context, &known, 32U) == FOC_MOTOR_ID_OK);
    CHECK(collect_turning(&context, false, 1.0e-6, &peak) == 0);
    CHECK(peak > 10.0);
    CHECK(foc_motor_id_finish(&context, &result) == FOC_MOTOR_ID_OK && result.rs_fixed);
    CHECK(fabsf(result.inductance_h / 0.0015f - 1.0f) < 0.01f);
    CHECK(fabsf(result.back_emf_v_s_per_rad / 0.07f - 1.0f) < 0.02f);

    /* The same motion fitted as standstill is biased: the reason for the third term. */
    struct foc_motor_id_config still = turning;
    still.fit_back_emf = false;
    still.max_residual_rms_v = 1.0f;
    CHECK(foc_motor_id_init(&context, &still, 22U) == FOC_MOTOR_ID_OK);
    CHECK(collect_turning(&context, true, 2.0e-5, &peak) == 0);
    CHECK(foc_motor_id_finish(&context, &result) == FOC_MOTOR_ID_OK && !result.back_emf_fitted);
    CHECK(fabsf(result.inductance_h / 0.0015f - 1.0f) > 0.05f);

    /* Without fit_back_emf, a turning shaft is refused at the first sample. */
    CHECK(foc_motor_id_init(&context, &still, 23U) == FOC_MOTOR_ID_OK);
    struct foc_motor_id_sample moving = sample_at(&context, 0U);
    moving.stationary_confirmed = false;
    CHECK(foc_motor_id_push(&context, &moving) == FOC_MOTOR_ID_ERR_PRECONDITION);

    /* A window faster than the bound is refused. */
    struct foc_motor_id_config slow = turning;
    slow.max_abs_speed_rad_s = 0.5f;
    CHECK(foc_motor_id_init(&context, &slow, 24U) == FOC_MOTOR_ID_OK);
    CHECK(collect_turning(&context, false, 2.0e-5, &peak) == FOC_MOTOR_ID_ERR_PRECONDITION);
    CHECK(context.result.failure == FOC_CALIBRATION_FAILURE_PRECONDITION);

    /* A still shaft under fit_back_emf publishes the two-term fit, bit for bit. */
    struct foc_motor_id_config plain = turning;
    plain.fit_back_emf = false;
    struct foc_motor_id_result reference;
    CHECK(foc_motor_id_init(&context, &plain, 25U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 0.6f, 0.0015f, false) == 0);
    CHECK(foc_motor_id_finish(&context, &reference) == FOC_MOTOR_ID_OK);
    CHECK(foc_motor_id_init(&context, &turning, 26U) == FOC_MOTOR_ID_OK);
    CHECK(collect(&context, 0.6f, 0.0015f, false) == 0);
    CHECK(foc_motor_id_finish(&context, &result) == FOC_MOTOR_ID_OK && !result.back_emf_fitted);
    CHECK(
        result.rs_ohm == reference.rs_ohm && result.inductance_h == reference.inductance_h &&
        result.residual_rms_v == reference.residual_rms_v && result.back_emf_v_s_per_rad == 0.0f
    );

    /* Configuration: q axis only, and a speed floor below the bound. */
    struct foc_motor_id_config bad = turning;
    bad.axis = FOC_MOTOR_ID_D;
    CHECK(foc_motor_id_init(&context, &bad, 27U) == FOC_MOTOR_ID_ERR_CONFIG);
    bad = turning;
    bad.min_speed_rms_rad_s = bad.max_abs_speed_rad_s;
    CHECK(foc_motor_id_init(&context, &bad, 28U) == FOC_MOTOR_ID_ERR_CONFIG);
    bad = turning;
    bad.fit_back_emf = false;
    bad.known_rs_ohm = 0.6f;
    CHECK(foc_motor_id_init(&context, &bad, 33U) == FOC_MOTOR_ID_ERR_CONFIG);
    bad = turning;
    bad.max_abs_speed_rad_s = 0.0f;
    bad.min_speed_rms_rad_s = 0.0f;
    CHECK(foc_motor_id_init(&context, &bad, 29U) == FOC_MOTOR_ID_ERR_CONFIG);
    return 0;
}

int main(void)
{
    return test_observation_contract() || test_voltage_policy() || test_fit() ||
        test_acquisition_resolution() || test_invalid_samples() || test_fit_rejection() ||
        test_pi() || test_commanded_voltage_policy() || test_back_emf();
}

/* ---------------------------------------- end of file ----------------------------------------- */
