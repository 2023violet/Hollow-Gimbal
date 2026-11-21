/**
 * @file    test_foc_flux_id.c
 * @brief   Analytical PMSM flux identification and observation contract tests.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Test role
 * ---------
 * Exercise signed fits and observation rejection on data generated without a
 * motor model or acquisition adapter, then quantify injected systematic errors.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_flux_id.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(c)                                               \
    do {                                                       \
        if (!(c)) {                                            \
            fprintf(stderr, "flux ID:%d: %s\n", __LINE__, #c); \
            return 1;                                          \
        }                                                      \
    } while (0)

#define INTERVALS_PER_WINDOW 8U
#define WINDOW_COUNT         8U
#define INTERVAL_COUNT       (INTERVALS_PER_WINDOW * WINDOW_COUNT)
#define TRUE_INTERVAL_US     UINT32_C(100)
#define FIRST_US             (UINT32_MAX - 199U)

/* Private Constants ------------------------------------------------------------ */

static const struct foc_flux_id_config clean_config = {
    .identity =
        {
            .session = 1U,
            .capture = 2U,
            .motor = 3U,
            .dq_frame = 4U,
            .current_chain = 5U,
            .voltage_chain = 6U,
            .speed_chain = 7U,
            .clock = 8U,
            .rl_revision = 9U,
            .poles_revision = 10U,
            .phase_order = 0U,
            .pole_pairs = 4U,
        },
    .rl = {.rs_ohm = 0.43f, .ld_h = 0.0011f, .lq_h = 0.0023f},
    .rl_qualified = true,
    .source = FOC_MOTOR_ID_VIRTUAL,
    .voltage_source = FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL,
    .interval_us = TRUE_INTERVAL_US,
    .intervals_per_window = INTERVALS_PER_WINDOW,
    .min_windows_per_direction = 2U,
    .max_abs_current_a = 3.0,
    .max_abs_vq_v = 6.0,
    .max_abs_speed_rad_s = 60.0,
    .min_abs_window_speed_rad_s = 4.0,
    .min_speed_span_rad_s = 2.0,
    .min_speed_rms_rad_s = 5.0,
    .min_flux_wb = 0.001,
    .max_flux_wb = 0.2,
    .max_residual_rms_v = 0.02,
    .max_direction_difference_relative = 0.05,
};

static const double signed_speeds[WINDOW_COUNT] = {
    16.0,
    16.0,
    25.0,
    25.0,
    -12.0,
    -12.0,
    -21.0,
    -21.0,
};

/* Private Types ---------------------------------------------------------------- */

struct observation_error {
    double vq_offset_v;
    double vq_gain_error;
    double current_offset_a;
    double speed_gain_error;
    double angle_error_rad;
    double voltage_noise_v;
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_flux_id_interval analytic_interval(
    const struct foc_flux_id_config *capture,
    const struct foc_motor_rl_model *plant_rl,
    double flux_wb,
    double omega,
    uint32_t index,
    const struct observation_error *error
)
{
    /* Currents are continuous and linear within each aperture interval. The
     * analytical mean voltage follows the forward d/q equations for that interval. */
    const double dt_s = TRUE_INTERVAL_US * 1e-6;
    const double id0 = 0.50 + 0.30 * sin(0.21 * index);
    const double id1 = 0.50 + 0.30 * sin(0.21 * (index + 1U));
    const double iq0 = -0.19 + 0.31 * cos(0.09 * index);
    const double iq1 = -0.19 + 0.31 * cos(0.09 * (index + 1U));
    const double id = 0.5 * (id0 + id1);
    const double iq = 0.5 * (iq0 + iq1);
    const double vd =
        plant_rl->rs_ohm * id + plant_rl->ld_h * (id1 - id0) / dt_s - omega * plant_rl->lq_h * iq;
    const double vq = plant_rl->rs_ohm * iq + plant_rl->lq_h * (iq1 - iq0) / dt_s +
        omega * (plant_rl->ld_h * id + flux_wb);
    const double sine = sin(error->angle_error_rad);
    const double cosine = cos(error->angle_error_rad);
    const uint32_t begin = FIRST_US + index * capture->interval_us;
    const uint32_t end = begin + capture->interval_us;
    return (struct foc_flux_id_interval){
        .identity = capture->identity,
        .source = capture->source,
        .voltage_source = capture->voltage_source,
        .sequence = UINT32_MAX - 1U + index,
        .begin_us = begin,
        .end_us = end,
        .voltage_begin_us = begin,
        .voltage_end_us = end,
        .speed_begin_us = begin,
        .speed_end_us = end,
        .id_begin_a = cosine * id0 + sine * iq0 + error->current_offset_a,
        .iq_begin_a = cosine * iq0 - sine * id0 + error->current_offset_a,
        .id_end_a = cosine * id1 + sine * iq1 + error->current_offset_a,
        .iq_end_a = cosine * iq1 - sine * id1 + error->current_offset_a,
        .mean_omega_e_rad_s = omega * (1.0 + error->speed_gain_error),
        .mean_vq_v = (cosine * vq - sine * vd) * (1.0 + error->vq_gain_error) + error->vq_offset_v +
            error->voltage_noise_v * sin(2.399963229728653 * index),
        .current_valid = true,
        .voltage_valid = true,
        .speed_valid = true,
        .frame_confirmed = true,
        .output_confirmed = true,
        .terminal_voltage_witnessed = true,
    };
}

static int fit_capture(
    const struct foc_flux_id_config *config,
    const struct foc_motor_rl_model *plant_rl,
    double flux_wb,
    const struct observation_error *error,
    struct foc_flux_id_result *result,
    enum foc_calibration_failure expected
)
{
    struct foc_flux_id context = {0};
    CHECK(foc_flux_id_init(&context, config) == FOC_CALIBRATION_FAILURE_NONE);
    for (uint32_t i = 0U; i < INTERVAL_COUNT; ++i) {
        const struct foc_flux_id_interval interval = analytic_interval(
            config,
            plant_rl,
            flux_wb,
            signed_speeds[i / INTERVALS_PER_WINDOW],
            i,
            error
        );
        CHECK(foc_flux_id_push(&context, &interval) == FOC_CALIBRATION_FAILURE_NONE);
    }
    CHECK(foc_flux_id_finish(&context, result) == expected);
    return 0;
}

static bool has_no_candidate(const struct foc_flux_id_result *result)
{
    return result->state != FOC_CALIBRATION_STATE_VALID && result->flux_wb == 0.0f &&
        result->kt_nm_per_peak_q_amp == 0.0f &&
        result->ke_phase_peak_v_per_mechanical_rad_s == 0.0f &&
        result->flux_provenance == FOC_FLUX_ID_PROVENANCE_NONE &&
        result->kt_provenance == FOC_FLUX_ID_PROVENANCE_NONE;
}

static int test_analytic_and_provenance(void)
{
    const struct observation_error clean = {0};
    struct foc_flux_id_config alternate = clean_config;
    alternate.identity.capture = 12U;
    alternate.identity.pole_pairs = 11U;
    alternate.rl = (struct foc_motor_rl_model){.rs_ohm = 0.71f, .ld_h = 0.0029f, .lq_h = 0.0016f};
    alternate.source = FOC_MOTOR_ID_MEASURED;
    alternate.voltage_source = FOC_MOTOR_ID_VOLTAGE_CALIBRATED_TERMINAL;
    struct foc_flux_id_config sensed = alternate;
    sensed.identity.capture = 13U;
    sensed.voltage_source = FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL;
    const struct {
        const struct foc_flux_id_config *config;
        double expected_flux_wb;
        double expected_kt_nm_per_peak_q_amp;
        double expected_ke_phase_peak_v_per_mechanical_rad_s;
    } cases[] = {
        {&clean_config, 0.042, 0.252, 0.168},
        {&alternate, 0.026, 0.429, 0.286},
        {&sensed, 0.026, 0.429, 0.286},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const struct foc_flux_id_config *config = cases[i].config;
        struct foc_flux_id_result result;
        CHECK(
            fit_capture(
                config,
                &config->rl,
                cases[i].expected_flux_wb,
                &clean,
                &result,
                FOC_CALIBRATION_FAILURE_NONE
            ) == 0
        );
        CHECK(result.state == FOC_CALIBRATION_STATE_VALID);
        CHECK(result.failure == FOC_CALIBRATION_FAILURE_NONE);
        CHECK(result.flux_provenance == FOC_FLUX_FROM_MEASUREMENT);
        CHECK(result.kt_provenance == FOC_KT_DERIVED_FROM_FLUX);
        CHECK(result.kt_provenance != FOC_KT_FROM_INDEPENDENT_TORQUE);
        CHECK(result.source == config->source && result.voltage_source == config->voltage_source);
        CHECK(result.identity.capture == config->identity.capture);
        CHECK(result.identity.pole_pairs == config->identity.pole_pairs);
        CHECK(result.identity.rl_revision == config->identity.rl_revision);
        CHECK(
            result.rl.rs_ohm == config->rl.rs_ohm && result.rl.ld_h == config->rl.ld_h &&
            result.rl.lq_h == config->rl.lq_h
        );
        CHECK(result.first_us == FIRST_US);
        CHECK(result.last_us == FIRST_US + INTERVAL_COUNT * TRUE_INTERVAL_US);
        CHECK(result.intervals == INTERVAL_COUNT);
        CHECK(result.positive.windows == 4U && result.negative.windows == 4U);
        CHECK(result.positive.minimum_abs_speed_rad_s == 16.0);
        CHECK(result.positive.maximum_abs_speed_rad_s == 25.0);
        CHECK(result.negative.minimum_abs_speed_rad_s == 12.0);
        CHECK(result.negative.maximum_abs_speed_rad_s == 21.0);
        CHECK(fabs(result.flux_wb - cases[i].expected_flux_wb) < 1e-8);
        CHECK(fabs(result.positive.flux_wb - cases[i].expected_flux_wb) < 1e-8);
        CHECK(fabs(result.negative.flux_wb - cases[i].expected_flux_wb) < 1e-8);
        CHECK(fabs(result.kt_nm_per_peak_q_amp - cases[i].expected_kt_nm_per_peak_q_amp) < 1e-7);
        CHECK(
            fabs(
                result.ke_phase_peak_v_per_mechanical_rad_s -
                cases[i].expected_ke_phase_peak_v_per_mechanical_rad_s
            ) < 1e-7
        );
        CHECK(result.residual_rms_v < 1e-7 && result.direction_difference_relative < 1e-9);
        printf(
            "analytic,source=%d,voltage_source=%d,poles=%u,flux_wb=%.10f,"
            "positive_wb=%.10f,negative_wb=%.10f,"
            "kt_nm_per_peak_q_amp=%.10f,ke_v_per_mechanical_rad_s=%.10f,residual_v=%.10f,"
            "dir_delta_relative=%.10f,first_us=%lu,last_us=%lu\n",
            (int)result.source,
            (int)result.voltage_source,
            (unsigned)result.identity.pole_pairs,
            (double)result.flux_wb,
            result.positive.flux_wb,
            result.negative.flux_wb,
            (double)result.kt_nm_per_peak_q_amp,
            (double)result.ke_phase_peak_v_per_mechanical_rad_s,
            result.residual_rms_v,
            result.direction_difference_relative,
            (unsigned long)result.first_us,
            (unsigned long)result.last_us
        );
    }
    return 0;
}

static int test_refused_configurations(void)
{
    struct foc_flux_id context = {0};
    struct foc_flux_id_config config = clean_config;
    config.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
    CHECK(foc_flux_id_init(&context, &config) == FOC_CALIBRATION_FAILURE_INVALID_CONFIG);
    config.voltage_source = FOC_MOTOR_ID_VOLTAGE_MEASURED_TERMINAL;
    CHECK(foc_flux_id_init(&context, &config) == FOC_CALIBRATION_FAILURE_INVALID_CONFIG);
    config = clean_config;
    config.rl_qualified = false;
    CHECK(foc_flux_id_init(&context, &config) != FOC_CALIBRATION_FAILURE_NONE);
    config = clean_config;
    config.rl.lq_h = 0.0f;
    CHECK(foc_flux_id_init(&context, &config) != FOC_CALIBRATION_FAILURE_NONE);
    config = clean_config;
    config.rl.rs_ohm = NAN;
    CHECK(foc_flux_id_init(&context, &config) == FOC_CALIBRATION_FAILURE_INVALID_CONFIG);
    config = clean_config;
    config.identity.pole_pairs = 0U;
    CHECK(foc_flux_id_init(&context, &config) != FOC_CALIBRATION_FAILURE_NONE);
    config = clean_config;
    config.interval_us = 0U;
    CHECK(foc_flux_id_init(&context, &config) == FOC_CALIBRATION_FAILURE_INVALID_CONFIG);
    return 0;
}

static int test_refused_observations(void)
{
    const struct observation_error clean = {0};
    struct foc_flux_id_config config = clean_config;
    config.source = FOC_MOTOR_ID_MEASURED;
    config.voltage_source = FOC_MOTOR_ID_VOLTAGE_CALIBRATED_TERMINAL;
    const struct foc_flux_id_interval first =
        analytic_interval(&config, &config.rl, 0.042, 16.0, 0U, &clean);
    const struct foc_flux_id_interval valid =
        analytic_interval(&config, &config.rl, 0.042, 16.0, 1U, &clean);
    for (unsigned int i = 0U; i < 34U; ++i) {
        struct foc_flux_id context = {0};
        CHECK(foc_flux_id_init(&context, &config) == FOC_CALIBRATION_FAILURE_NONE);
        CHECK(foc_flux_id_push(&context, &first) == FOC_CALIBRATION_FAILURE_NONE);
        struct foc_flux_id_interval bad = valid;
        enum foc_calibration_failure expected = FOC_CALIBRATION_FAILURE_CONFIG_MISMATCH;
        switch (i) {
        case 0:
            ++bad.identity.motor;
            break;
        case 1:
            ++bad.identity.capture;
            break;
        case 2:
            ++bad.identity.session;
            break;
        case 3:
            ++bad.identity.dq_frame;
            break;
        case 4:
            ++bad.identity.phase_order;
            break;
        case 5:
            ++bad.identity.pole_pairs;
            break;
        case 6:
            ++bad.identity.poles_revision;
            break;
        case 7:
            ++bad.identity.rl_revision;
            break;
        case 8:
            ++bad.identity.current_chain;
            break;
        case 9:
            ++bad.identity.voltage_chain;
            break;
        case 10:
            ++bad.identity.speed_chain;
            break;
        case 11:
            ++bad.identity.clock;
            break;
        case 12:
            bad.source = FOC_MOTOR_ID_VIRTUAL;
            break;
        case 13:
            /* An output-confirmed request still has unverified voltage provenance. */
            bad.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
            break;
        case 14:
            bad.sequence = first.sequence;
            expected = FOC_CALIBRATION_FAILURE_TIME_ORDER;
            break;
        case 15:
            ++bad.sequence;
            expected = FOC_CALIBRATION_FAILURE_TIME_ORDER;
            break;
        case 16:
            bad = first;
            bad.sequence = valid.sequence;
            expected = FOC_CALIBRATION_FAILURE_TIME_ORDER;
            break;
        case 17:
            bad.end_us = bad.begin_us;
            bad.voltage_end_us = bad.speed_end_us = bad.end_us;
            expected = FOC_CALIBRATION_FAILURE_TIME_ORDER;
            break;
        case 18:
            bad.end_us = bad.begin_us - 1U;
            bad.voltage_end_us = bad.speed_end_us = bad.end_us;
            expected = FOC_CALIBRATION_FAILURE_TIME_ORDER;
            break;
        case 19:
            ++bad.speed_end_us;
            expected = FOC_CALIBRATION_FAILURE_TIME_ORDER;
            break;
        case 20:
            --bad.voltage_begin_us;
            expected = FOC_CALIBRATION_FAILURE_TIME_ORDER;
            break;
        case 21:
            bad.iq_begin_a += 0.01;
            /* Both endpoints claim the same sampled aperture. */
            expected = FOC_CALIBRATION_FAILURE_INPUT_CHANGED;
            break;
        case 22:
            bad.current_valid = false;
            expected = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
            break;
        case 23:
            bad.voltage_valid = false;
            expected = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
            break;
        case 24:
            bad.speed_valid = false;
            expected = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
            break;
        case 25:
            bad.frame_confirmed = false;
            expected = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 26:
            bad.output_confirmed = false;
            expected = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 27:
            /* The reconstruction label cannot substitute for an independent witness. */
            bad.terminal_voltage_witnessed = false;
            expected = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 28:
            bad.saturated = true;
            expected = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 29:
            bad.id_end_a = NAN;
            expected = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
            break;
        case 30:
            bad.mean_vq_v = INFINITY;
            expected = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
            break;
        case 31:
            bad.mean_omega_e_rad_s = NAN;
            expected = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
            break;
        case 32:
            bad.end_us += 5U;
            bad.speed_end_us = bad.voltage_end_us = bad.end_us;
            expected = FOC_CALIBRATION_FAILURE_TIME_ORDER;
            break;
        case 33:
            bad.mean_omega_e_rad_s = -bad.mean_omega_e_rad_s;
            expected = FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE;
            break;
        }
        const enum foc_calibration_failure pushed = foc_flux_id_push(&context, &bad);
        printf("refused_observation,case=%u,failure=%d\n", i, (int)pushed);
        CHECK(pushed == expected);
        CHECK(foc_flux_id_push(&context, &valid) != FOC_CALIBRATION_FAILURE_NONE);
        struct foc_flux_id_result result = {.state = FOC_CALIBRATION_STATE_VALID, .flux_wb = 1.0f};
        CHECK(foc_flux_id_finish(&context, &result) != FOC_CALIBRATION_FAILURE_NONE);
        CHECK(has_no_candidate(&result));
        CHECK(result.intervals == 1U && result.last_us == first.end_us);
    }
    return 0;
}

static int test_unidentifiable_and_cancelled(void)
{
    const struct observation_error clean = {0};
    static const struct {
        const char *name;
        uint32_t count;
        double fixed_speed;
    } cases[] = {
        {"zero-speed", INTERVAL_COUNT, 0.0},
        {"subthreshold-speed", INTERVAL_COUNT, 2.0},
        {"single-speed-per-sign", INTERVAL_COUNT, 16.0},
        {"positive-only", 4U * INTERVALS_PER_WINDOW, 0.0},
        {"negative-only", 4U * INTERVALS_PER_WINDOW, 0.0},
        {"partial-window", INTERVAL_COUNT - 1U, 0.0},
        {"too-few-samples", 2U, 0.0},
        {"cancelled", INTERVAL_COUNT, 0.0},
    };
    for (size_t variant = 0U; variant < sizeof(cases) / sizeof(cases[0]); ++variant) {
        struct foc_flux_id context = {0};
        CHECK(foc_flux_id_init(&context, &clean_config) == FOC_CALIBRATION_FAILURE_NONE);
        enum foc_calibration_failure status = FOC_CALIBRATION_FAILURE_NONE;
        for (uint32_t i = 0U; i < cases[variant].count; ++i) {
            double speed = signed_speeds[i / INTERVALS_PER_WINDOW];
            if (variant < 3U) {
                speed = copysign(cases[variant].fixed_speed, speed);
            } else if (variant == 4U) {
                speed = -speed;
            }
            const struct foc_flux_id_interval interval =
                analytic_interval(&clean_config, &clean_config.rl, 0.042, speed, i, &clean);
            status = foc_flux_id_push(&context, &interval);
            if (status != FOC_CALIBRATION_FAILURE_NONE) {
                break;
            }
        }
        if (variant == 7U) {
            CHECK(status == FOC_CALIBRATION_FAILURE_NONE);
            foc_flux_id_cancel(&context);
        }
        struct foc_flux_id_result result;
        const enum foc_calibration_failure finish = foc_flux_id_finish(&context, &result);
        printf(
            "no_candidate,%s,push_failure=%d,finish_failure=%d,stored_failure=%d\n",
            cases[variant].name,
            (int)status,
            (int)finish,
            (int)result.failure
        );
        CHECK(finish != FOC_CALIBRATION_FAILURE_NONE && has_no_candidate(&result));
        if (variant < 3U) {
            CHECK(result.failure == FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);
        } else if (variant < 7U) {
            CHECK(result.failure == FOC_CALIBRATION_FAILURE_SAMPLE_COUNT);
        } else {
            CHECK(result.failure == FOC_CALIBRATION_FAILURE_PRECONDITION);
        }
    }
    return 0;
}

static int test_sensitivity(void)
{
    const double true_flux_wb = 0.042;
    static const struct {
        const char *name;
        struct observation_error error;
        enum foc_calibration_failure expected_failure;
        double expected_signed_error_percent;
    } cases[] = {
        {"clean", {.vq_offset_v = 0.0}, FOC_CALIBRATION_FAILURE_NONE, 0.000},
        {"voltage-bias:+25mV", {.vq_offset_v = 0.025}, FOC_CALIBRATION_FAILURE_RESIDUAL, 0.325},
        {"voltage-gain:+4pct", {.vq_gain_error = 0.04}, FOC_CALIBRATION_FAILURE_NONE, 1.862},
        {"current-bias:+60mA-d-and-q",
         {.current_offset_a = 0.06},
         FOC_CALIBRATION_FAILURE_RESIDUAL,
         -0.492},
        {"speed-gain:+4pct", {.speed_gain_error = 0.04}, FOC_CALIBRATION_FAILURE_NONE, -3.890},
        {"rl-bias:Rs+12pct-Ld-15pct-Lq+15pct",
         {.vq_offset_v = 0.0},
         FOC_CALIBRATION_FAILURE_RESIDUAL,
         8.341},
        {"reported-interval:+5pct", {.vq_offset_v = 0.0}, FOC_CALIBRATION_FAILURE_NONE, -2.569},
        {"angle-offset:+0.35rad",
         {.angle_error_rad = 0.35},
         FOC_CALIBRATION_FAILURE_RESIDUAL,
         -6.775},
        {"voltage-noise:20mV-peak",
         {.voltage_noise_v = 0.02},
         FOC_CALIBRATION_FAILURE_NONE,
         -0.031},
    };
    puts(
        "sensitivity,case,status,truth_wb,published_wb,fit_wb,signed_error_pct,"
        "positive_wb,negative_wb,residual_v,dir_delta_relative,flux_provenance,kt_provenance"
    );
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        struct foc_flux_id_config config = clean_config;
        if (i == 5U) {
            config.rl.rs_ohm *= 1.12f;
            config.rl.ld_h *= 0.85f;
            config.rl.lq_h *= 1.15f;
        } else if (i == 6U) {
            /* All reported subintervals agree, while the acquisition clock scale is wrong. */
            config.interval_us = 105U;
        }
        struct foc_flux_id_result result;
        CHECK(
            fit_capture(
                &config,
                &clean_config.rl,
                true_flux_wb,
                &cases[i].error,
                &result,
                cases[i].expected_failure
            ) == 0
        );
        /* Positive windows contribute 2*(16^2 + 25^2) and negative windows
         * contribute 2*(12^2 + 21^2) to the speed squared weight. All speed gains
         * apply uniformly, including when both fits are retained after rejection. */
        const double fit_wb =
            (881.0 * result.positive.flux_wb + 585.0 * result.negative.flux_wb) / 1466.0;
        const double signed_error_pct = 100.0 * (fit_wb / true_flux_wb - 1.0);
        const bool accepted = cases[i].expected_failure == FOC_CALIBRATION_FAILURE_NONE;
        printf(
            "sensitivity,%s,%s,%.10f,%.10f,%.10f,%+.5f,%.10f,%.10f,%.10f,%.10f,%d,%d\n",
            cases[i].name,
            accepted ? "pass" : "reject",
            true_flux_wb,
            (double)result.flux_wb,
            fit_wb,
            signed_error_pct,
            result.positive.flux_wb,
            result.negative.flux_wb,
            result.residual_rms_v,
            result.direction_difference_relative,
            (int)result.flux_provenance,
            (int)result.kt_provenance
        );
        CHECK(fabs(signed_error_pct - cases[i].expected_signed_error_percent) < 0.001);
        if (accepted) {
            CHECK(
                result.state == FOC_CALIBRATION_STATE_VALID && result.source == FOC_MOTOR_ID_VIRTUAL
            );
            CHECK(result.voltage_source == FOC_MOTOR_ID_VOLTAGE_VIRTUAL_TERMINAL);
            CHECK(result.flux_provenance == FOC_FLUX_FROM_MEASUREMENT);
            CHECK(result.kt_provenance == FOC_KT_DERIVED_FROM_FLUX);
            CHECK(fabs(result.flux_wb - fit_wb) < 1e-8);
        } else {
            CHECK(has_no_candidate(&result));
        }
    }
    return 0;
}

static int test_direction_disagreement(void)
{
    const struct observation_error clean = {0};
    struct foc_flux_id context = {0};
    CHECK(foc_flux_id_init(&context, &clean_config) == FOC_CALIBRATION_FAILURE_NONE);
    for (uint32_t i = 0U; i < INTERVAL_COUNT; ++i) {
        /* The opposite signed biases give acceptable RMS residual at these speeds. */
        const double speed = 0.6 * signed_speeds[i / INTERVALS_PER_WINDOW];
        const double direction_flux_wb = speed > 0.0 ? 0.0433 : 0.0407;
        const struct foc_flux_id_interval interval =
            analytic_interval(&clean_config, &clean_config.rl, direction_flux_wb, speed, i, &clean);
        CHECK(foc_flux_id_push(&context, &interval) == FOC_CALIBRATION_FAILURE_NONE);
    }
    struct foc_flux_id_result result;
    CHECK(foc_flux_id_finish(&context, &result) == FOC_CALIBRATION_FAILURE_RESIDUAL);
    CHECK(has_no_candidate(&result));
    CHECK(result.residual_rms_v < clean_config.max_residual_rms_v);
    CHECK(result.direction_difference_relative > clean_config.max_direction_difference_relative);
    printf(
        "direction_disagreement,positive_wb=%.10f,negative_wb=%.10f,residual_v=%.10f,"
        "dir_delta_relative=%.10f\n",
        result.positive.flux_wb,
        result.negative.flux_wb,
        result.residual_rms_v,
        result.direction_difference_relative
    );
    return 0;
}

static int test_accumulator_overflow(void)
{
    struct foc_flux_id_config config = clean_config;
    config.max_abs_vq_v = 1.0e201;
    struct foc_flux_id estimator;
    struct foc_flux_id_result result;
    const struct observation_error clean = {0};
    CHECK(foc_flux_id_init(&estimator, &config) == FOC_CALIBRATION_FAILURE_NONE);
    for (uint32_t i = 0U; i < config.intervals_per_window; ++i) {
        struct foc_flux_id_interval sample =
            analytic_interval(&config, &config.rl, 0.042, 16.0, i, &clean);
        sample.mean_vq_v = 1.0e200;
        CHECK(
            foc_flux_id_push(&estimator, &sample) ==
            (i + 1U == config.intervals_per_window ? FOC_CALIBRATION_FAILURE_NUMERIC
                                                   : FOC_CALIBRATION_FAILURE_NONE)
        );
    }
    CHECK(foc_flux_id_finish(&estimator, &result) == FOC_CALIBRATION_FAILURE_NUMERIC);
    CHECK(result.flux_provenance == FOC_FLUX_ID_PROVENANCE_NONE && result.flux_wb == 0.0f);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_analytic_and_provenance() || test_refused_configurations() ||
        test_refused_observations() || test_unidentifiable_and_cancelled() || test_sensitivity() ||
        test_direction_disagreement() || test_accumulator_overflow();
}

/* ---------------------------------------- end of file ----------------------------------------- */
