/**
 * @file    test_foc_virtual_calibration_robustness.c
 * @brief   Host calibration measurements with controlled sensor and model errors.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Test role
 * ---------
 * Keep known Plant inputs independent of claimed measurements, then compare
 * estimator quality decisions with the declared Host oracles.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_bootstrap_capture.h"
#include "foc_virtual_bootstrap_instrument.h"
#include "foc_virtual_calibration_reference.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

#define RL_ROWS 769U
#define FLUX_ROWS 512U

static const double tau = 6.28318530717958647692;
static const uint32_t noise_seed = UINT32_C(42319);

/* Private Types ---------------------------------------------------------------- */

enum electrical_error {
    ELECTRICAL_NOMINAL,
    ELECTRICAL_CURRENT_ZERO,
    ELECTRICAL_CURRENT_GAIN,
    ELECTRICAL_BUS_GAIN,
    ELECTRICAL_TERMINAL_GAIN,
    ELECTRICAL_RECONSTRUCTION_OFFSET,
    ELECTRICAL_APERTURE_SKEW,
    ELECTRICAL_WRONG_VOLTAGE_INTERVAL,
    ELECTRICAL_UNWITNESSED_VOLTAGE,
    ELECTRICAL_CLOCK_SCALE,
    ELECTRICAL_CLOCK_SPEED_SCALE,
    ELECTRICAL_SPEED_SCALE,
    ELECTRICAL_DIRECTION_SPEED,
    ELECTRICAL_RS,
    ELECTRICAL_LD,
    ELECTRICAL_LQ,
    ELECTRICAL_EMF_NOISE,
    ELECTRICAL_DIRECTION_VOLTAGE,
    ELECTRICAL_SHARED_TERMINAL_GAIN,
    ELECTRICAL_CASE_COUNT,
};

struct electrical_rows {
    struct foc_motor_id_sample rl[2][RL_ROWS];
    struct foc_flux_id_interval flux[FLUX_ROWS];
    uint32_t rl_count[2];
    uint32_t flux_count;
    double bus_ratio;
};

struct hot_prediction {
    double voltage_sum;
    double current_as;
    double current_begin;
    double squared_residual[2];
    uint32_t windows;
};

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                        \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

static double noise(uint32_t *state)
{
    *state = UINT32_C(1664525) * *state + UINT32_C(1013904223);
    return (double)(*state >> 8U) / 8388608.0 - 1.0;
}

static double measurement_gain(enum electrical_error error, double bus_ratio)
{
    if (error == ELECTRICAL_BUS_GAIN) {
        return bus_ratio;
    }
    return (error == ELECTRICAL_TERMINAL_GAIN || error == ELECTRICAL_SHARED_TERMINAL_GAIN)
        ? 1.04 : 1.0;
}

static const char *electrical_name(enum electrical_error error)
{
    static const char *const names[] = {
        "nominal", "adc-zero-plus-0.024A", "current-gain-plus-7pct",
        "bus-gain-plus-4pct-reconstruction", "terminal-gain-plus-4pct",
        "terminal-offset-plus-0.012V", "reported-aperture-skew-plus-15us",
        "hidden-voltage-single-interval-lag", "unwitnessed-voltage",
        "clock-scale-plus-10pct", "clock-plus-10pct-speed-from-same-clock",
        "speed-gain-plus-8pct", "negative-speed-magnitude-minus-3pct",
        "rs-prior-plus-30pct", "ld-prior-plus-30pct", "lq-prior-plus-30pct",
        "emf-noise-uniform-plus-minus-0.06V", "negative-terminal-gain-plus-15pct", "shared-rl-flux-voltage-gain-plus-4pct",
    };
    return names[error];
}

static void collect_rl(
    void *argument, enum foc_motor_id_axis axis, const struct foc_motor_id_sample *sample
)
{
    struct electrical_rows *rows = argument;
    if (rows->rl_count[axis] < RL_ROWS) {
        rows->rl[axis][rows->rl_count[axis]++] = *sample;
    }
}

static void collect_flux(void *argument, const struct foc_flux_id_interval *sample)
{
    struct electrical_rows *rows = argument;
    if (rows->flux_count < FLUX_ROWS) {
        rows->flux[rows->flux_count++] = *sample;
    }
}

static int capture_electrical(
    const char *name,
    const struct foc_virtual_current_profile *profile,
    const struct foc_cal_workflow_request *request,
    struct foc_virtual_calibration *session,
    struct electrical_rows *rows
)
{
    CHECK(foc_virtual_calibration_start(session, profile, request) == FOC_VIRTUAL_CAL_PENDING);
    session->electrical.rl_endpoint = collect_rl;
    session->electrical.rl_endpoint_argument = rows;
    const struct foc_virtual_flux_options options = {
        .rl_revision = 11U,
        .poles_revision = 12U,
        .current_revision = 13U,
        .voltage_chain = 14U,
        .angle_reference = 15U,
        .interval = collect_flux,
        .argument = rows,
    };
    struct foc_flux_id_result flux;
    while (session->stage <= FOC_VIRTUAL_CAL_FLUX) {
        CHECK(
            foc_virtual_calibration_poll_observed(session, &options, &flux) ==
            FOC_VIRTUAL_CAL_PENDING
        );
    }
    CHECK(session->original.output.accepted_updates == 0U && session->flux.qualified);
    CHECK(rows->rl_count[0] == RL_ROWS && rows->rl_count[1] == RL_ROWS);
    CHECK(rows->flux_count == FLUX_ROWS);

    struct foc_device_adc_raw raw;
    struct foc_device_measurement nominal;
    struct foc_device_measurement gain_error;
    CHECK(foc_virtual_motor_make_adc(&session->motor, &profile->adc, profile->vbus_v, 0U,
                                     session->time_us, &raw) == 0);
    CHECK(foc_device_decode_sample(&raw, &profile->adc, &nominal) == FOC_DEVICE_STATUS_OK);
    struct foc_device_adc_config perturbed = profile->adc;
    perturbed.vbus_v_per_count *= 1.04f;
    CHECK(foc_device_decode_sample(&raw, &perturbed, &gain_error) == FOC_DEVICE_STATUS_OK);
    rows->bus_ratio = gain_error.core.vbus_v / nominal.core.vbus_v;
    printf("BUS,%s,%.9g,%.9g,%u\n", name, (double)nominal.core.vbus_v,
           (double)gain_error.core.vbus_v, (unsigned)raw.vbus_raw);
    return 0;
}

static int adc_cases(
    const char *name, const struct foc_calibration_identity *identity, bool trace
)
{
    const struct foc_calibration_adc_config policy = {
        .identity = *identity,
        .required_valid_mask = 7U,
        .minimum_samples = 64U,
        .maximum_samples = 64U,
        .maximum_sample_step_counts = 12.0f,
        .maximum_noise_rms_counts = 2.0f,
        .minimum_offset_counts = 1500.0f,
        .maximum_offset_counts = 2600.0f,
        .maximum_interval_us = 200U,
    };
    for (unsigned trial = 0U; trial < 4U; ++trial) {
        const char *scenario[] = {"zero-baseline", "in-range-bias-counts", "invalid-mask",
                                  "out-of-range-bias-counts"};
        struct foc_calibration_adc_context estimator;
        struct foc_calibration_adc_result fit;
        CHECK(foc_calibration_adc_init(&estimator, &policy) == 0);
        int status = 0;
        for (uint32_t i = 0U; i < 64U && status == 0; ++i) {
            const unsigned int a_bias = trial == 3U ? 600U : (trial == 1U ? 24U : 0U);
            const struct foc_calibration_adc_sample sample = {
                .phase_a = (uint16_t)(2048U + a_bias + i % 2U),
                .phase_b = 2051U,
                .phase_c = 2041U,
                .valid_mask = trial == 2U && i == 7U ? 6U : 7U,
                .captured_us = 100U * (i + 1U),
                .sequence = i,
                .zero_current_confirmed = true,
            };
            if (trace) {
                printf("RAW_ADC,%s,%s,%" PRIu32 ",%" PRIu32 ",%u,%u,%u,%" PRIu32 "\n",
                       name, scenario[trial], i, sample.captured_us, (unsigned)sample.phase_a,
                       (unsigned)sample.phase_b, (unsigned)sample.phase_c, sample.valid_mask);
            }
            status = foc_calibration_adc_collect(&estimator, &sample);
        }
        status = foc_calibration_adc_finalize(&estimator, &fit);
        printf("ADC,%s,%s,%d,%u,%" PRIu32 ",%.9g,%.9g,%.9g,%.9g\n", name, scenario[trial],
               status, (unsigned)fit.failure, fit.samples, (double)fit.offset_a_counts,
               (double)fit.offset_b_counts, (double)fit.offset_c_counts,
               (double)fit.noise_rms_a_counts);
        if (trial < 2U) {
            CHECK(fit.usable && fabs(fit.offset_a_counts - (trial ? 2072.5 : 2048.5)) < 0.001);
        } else {
            CHECK(!fit.usable);
        }
    }
    return 0;
}

static int replay_rl(
    const char *name, const struct electrical_rows *rows, enum electrical_error error, bool trace
)
{
    struct foc_motor_id_result fits[2];
    for (unsigned int axis = 0U; axis < 2U; ++axis) {
        const struct foc_motor_id_sample *original = rows->rl[axis];
        const uint32_t period = error == ELECTRICAL_CLOCK_SCALE ? 55U : 50U;
        const struct foc_motor_id_config policy = {
            .axis = (enum foc_motor_id_axis)axis,
            .source = original->source,
            .voltage_source = original->voltage_source,
            .setup_id = original->setup_id,
            .min_interval_us = period,
            .max_interval_us = period,
            .intervals_per_window = 8U,
            .min_windows = 32U,
            .max_abs_current_a = 2.5f,
            .max_abs_voltage_v = 6.84f,
            .min_current_rms_a = 0.05f,
            .min_slew_rms_a_per_s = 5.0f,
            .min_voltage_rms_v = 0.05f,
            .max_residual_rms_v = 0.025f,
            .min_rs_ohm = 0.03f,
            .max_rs_ohm = 2.0f,
            .min_inductance_h = 0.0004f,
            .max_inductance_h = 0.01f,
        };
        struct foc_motor_id_context estimator;
        CHECK(foc_motor_id_init(&estimator, &policy, original->capture_id) == FOC_MOTOR_ID_OK);
        for (uint32_t i = 0U; i < RL_ROWS; ++i) {
            struct foc_motor_id_sample sample = original[i];
            sample.captured_us = original->captured_us + i * period;
            sample.voltage_end_us = sample.captured_us;
            sample.voltage_begin_us = sample.captured_us - period;
            if (error == ELECTRICAL_CURRENT_ZERO) {
                sample.current_a += 0.024f;
            } else if (error == ELECTRICAL_CURRENT_GAIN) {
                sample.current_a *= 1.07f;
            } else if (error == ELECTRICAL_RECONSTRUCTION_OFFSET) {
                sample.mean_voltage_v += 0.012f;
            } else if (error == ELECTRICAL_APERTURE_SKEW) {
                sample.captured_us += 15U;
            } else if (error == ELECTRICAL_WRONG_VOLTAGE_INTERVAL && i >= 2U) {
                sample.mean_voltage_v = original[i - 1U].mean_voltage_v;
            } else if (error == ELECTRICAL_UNWITNESSED_VOLTAGE && i > 0U) {
                sample.voltage_source = FOC_MOTOR_ID_VOLTAGE_UNVERIFIED;
            }
            sample.mean_voltage_v *= (float)measurement_gain(error, rows->bus_ratio);
            if (trace) {
                printf("RAW_RL,%s,%s,%u,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
                       ",%.9g,%.9g,%u\n", name, electrical_name(error), axis, i,
                       sample.captured_us, sample.voltage_begin_us, sample.voltage_end_us,
                       (double)sample.current_a, (double)sample.mean_voltage_v,
                       (unsigned)sample.voltage_source);
            }
            if (foc_motor_id_push(&estimator, &sample) != FOC_MOTOR_ID_OK) {
                break;
            }
        }
        const int status = foc_motor_id_finish(&estimator, &fits[axis]);
        const struct foc_motor_id_result *fit = &fits[axis];
        printf("RL,%s,%s,%u,%d,%u,%" PRIu32 ",%" PRIu32 ",%.9g,%.9g,%.9g\n", name,
               electrical_name(error), axis, status, (unsigned)fit->failure, fit->samples,
               fit->windows, (double)fit->rs_ohm, (double)fit->inductance_h, fit->residual_rms_v);
    }
    struct foc_motor_rl_model model;
    const int paired = foc_motor_id_make_model(&fits[0], &fits[1], 0.03f, &model);
    printf("RL_PAIR,%s,%s,%d,%.9g,%.9g,%.9g\n", name, electrical_name(error), paired,
           (double)model.rs_ohm, (double)model.ld_h, (double)model.lq_h);
    if (error <= ELECTRICAL_BUS_GAIN || error == ELECTRICAL_TERMINAL_GAIN ||
        error == ELECTRICAL_CLOCK_SCALE) {
        CHECK(paired == FOC_MOTOR_ID_OK);
    }
    if (error == ELECTRICAL_APERTURE_SKEW || error == ELECTRICAL_UNWITNESSED_VOLTAGE) {
        CHECK(paired != FOC_MOTOR_ID_OK);
    }
    return 0;
}

static int replay_flux(
    const char *name,
    const struct electrical_rows *rows,
    struct foc_motor_rl_model rl,
    enum electrical_error error,
    bool trace,
    struct foc_flux_id_result *result
)
{
    const struct foc_flux_id_interval *original = rows->flux;
    const uint32_t period =
        error == ELECTRICAL_CLOCK_SCALE || error == ELECTRICAL_CLOCK_SPEED_SCALE ? 55U : 50U;
    if (error == ELECTRICAL_SHARED_TERMINAL_GAIN) {
        rl.rs_ohm *= 1.04f;
        rl.ld_h *= 1.04f;
        rl.lq_h *= 1.04f;
    } else if (error == ELECTRICAL_RS) {
        rl.rs_ohm *= 1.30f;
    } else if (error == ELECTRICAL_LD) {
        rl.ld_h *= 1.30f;
    } else if (error == ELECTRICAL_LQ) {
        rl.lq_h *= 1.30f;
    }
    const struct foc_flux_id_config policy = {
        .identity = original->identity,
        .rl = rl,
        .rl_qualified = true,
        .source = original->source,
        .voltage_source = original->voltage_source,
        .interval_us = period,
        .intervals_per_window = 32U,
        .min_windows_per_direction = 8U,
        .max_abs_current_a = 2.5,
        .max_abs_vq_v = 6.84,
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
    CHECK(foc_flux_id_init(&estimator, &policy) == FOC_CALIBRATION_FAILURE_NONE);
    uint32_t random = noise_seed;
    for (uint32_t i = 0U; i < FLUX_ROWS; ++i) {
        struct foc_flux_id_interval sample = original[i];
        sample.begin_us = original->begin_us + i * period;
        sample.end_us = sample.begin_us + period;
        sample.voltage_begin_us = sample.speed_begin_us = sample.begin_us;
        sample.voltage_end_us = sample.speed_end_us = sample.end_us;
        if (error == ELECTRICAL_CURRENT_ZERO) {
            sample.iq_begin_a += 0.024;
            sample.iq_end_a += 0.024;
        } else if (error == ELECTRICAL_CURRENT_GAIN) {
            sample.id_begin_a *= 1.07;
            sample.id_end_a *= 1.07;
            sample.iq_begin_a *= 1.07;
            sample.iq_end_a *= 1.07;
        } else if (error == ELECTRICAL_RECONSTRUCTION_OFFSET) {
            sample.mean_vq_v += 0.012;
        } else if (error == ELECTRICAL_APERTURE_SKEW) {
            sample.end_us += 15U;
            sample.begin_us += 15U;
        } else if (error == ELECTRICAL_WRONG_VOLTAGE_INTERVAL && i > 0U) {
            sample.mean_vq_v = original[i - 1U].mean_vq_v;
        } else if (error == ELECTRICAL_UNWITNESSED_VOLTAGE) {
            sample.terminal_voltage_witnessed = false;
        } else if (error == ELECTRICAL_CLOCK_SPEED_SCALE) {
            sample.mean_omega_e_rad_s /= 1.10;
        } else if (error == ELECTRICAL_SPEED_SCALE) {
            sample.mean_omega_e_rad_s *= 1.08;
        } else if (error == ELECTRICAL_DIRECTION_SPEED && sample.mean_omega_e_rad_s < 0.0) {
            sample.mean_omega_e_rad_s *= 0.97;
        } else if (error == ELECTRICAL_EMF_NOISE) {
            sample.mean_vq_v += 0.06 * noise(&random);
        } else if (error == ELECTRICAL_DIRECTION_VOLTAGE && sample.mean_omega_e_rad_s < 0.0) {
            sample.mean_vq_v *= 1.15;
        }
        sample.mean_vq_v *= measurement_gain(error, rows->bus_ratio);
        if (trace) {
            printf("RAW_FLUX,%s,%s,%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
                   ",%" PRIu32 ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u\n", name,
                   electrical_name(error), i, sample.begin_us, sample.end_us,
                   sample.voltage_begin_us, sample.voltage_end_us, sample.id_begin_a,
                   sample.iq_begin_a, sample.id_end_a, sample.iq_end_a, sample.mean_vq_v,
                   sample.mean_omega_e_rad_s, (unsigned)sample.terminal_voltage_witnessed);
        }
        if (foc_flux_id_push(&estimator, &sample) != FOC_CALIBRATION_FAILURE_NONE) {
            break;
        }
    }
    const enum foc_calibration_failure status = foc_flux_id_finish(&estimator, result);
    printf("FLUX,%s,%s,%u,%" PRIu32 ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
           name, electrical_name(error), (unsigned)status, result->intervals,
           (double)result->flux_wb, (double)result->ke_phase_peak_v_per_mechanical_rad_s,
           (double)result->kt_nm_per_peak_q_amp, result->residual_rms_v,
           result->positive.flux_wb, result->negative.flux_wb,
           result->direction_difference_relative, result->omega_e_rms_rad_s);
    if (error == ELECTRICAL_NOMINAL || error == ELECTRICAL_SPEED_SCALE ||
        error == ELECTRICAL_SHARED_TERMINAL_GAIN) {
        CHECK(status == FOC_CALIBRATION_FAILURE_NONE);
    } else if (error == ELECTRICAL_APERTURE_SKEW || error == ELECTRICAL_UNWITNESSED_VOLTAGE) {
        CHECK(status != FOC_CALIBRATION_FAILURE_NONE);
    }
    return 0;
}

static double hot_holdout_rms(
    const struct foc_motor_id_sample *rows,
    const struct foc_motor_rl_model *cold,
    const struct foc_motor_rl_model *hot,
    unsigned int axis,
    double rms[2]
)
{
    struct hot_prediction sums = {0};
    for (uint32_t i = 1U; i < RL_ROWS; ++i) {
        sums.voltage_sum += rows[i].mean_voltage_v;
        sums.current_as += 0.5 * (rows[i].current_a + rows[i - 1U].current_a);
        if (i % 8U != 0U) {
            continue;
        }
        const double current = sums.current_as / 8.0;
        const double derivative = (rows[i].current_a - rows[i - 8U].current_a) / 0.0004;
        const double voltage = sums.voltage_sum / 8.0;
        const struct foc_motor_rl_model *models[] = {cold, hot};
        for (unsigned int model = 0; model < 2U; ++model) {
            const double inductance = axis == 0U ? models[model]->ld_h : models[model]->lq_h;
            const double residual =
                voltage - models[model]->rs_ohm * current - inductance * derivative;
            sums.squared_residual[model] += residual * residual;
        }
        sums.voltage_sum = 0.0;
        sums.current_as = 0.0;
        ++sums.windows;
    }
    rms[0] = sqrt(sums.squared_residual[0] / sums.windows);
    rms[1] = sqrt(sums.squared_residual[1] / sums.windows);
    return sums.windows;
}

static int test_electrical(const char *name, bool trace)
{
    struct foc_virtual_current_profile profile;
    struct foc_cal_workflow_request request;
    CHECK(foc_virtual_calibration_profile(name, &profile, &request) == 0);
    CHECK(adc_cases(name, &request.identity.acquisition, trace) == 0);
    struct foc_virtual_calibration original = {0};
    struct electrical_rows cold_rows = {0};
    CHECK(capture_electrical(name, &profile, &request, &original, &cold_rows) == 0);
    const struct foc_motor_rl_model cold = original.workflow.result.electrical_model;
    printf("TRUTH,%s,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", name, profile.motor.rs_ohm,
           profile.motor.ld_h, profile.motor.lq_h, profile.motor.psi_f_wb,
           profile.motor.inertia_kg_m2, profile.motor.damping_nm_s);
    struct foc_flux_id_result result;
    double nominal_flux = 0.0;
    for (enum electrical_error error = ELECTRICAL_NOMINAL;
         error < ELECTRICAL_CASE_COUNT; ++error) {
        if (error <= ELECTRICAL_CLOCK_SCALE) {
            CHECK(replay_rl(name, &cold_rows, error, trace) == 0);
        }
        CHECK(replay_flux(name, &cold_rows, cold, error, trace, &result) == 0);
        if (error == ELECTRICAL_NOMINAL) {
            nominal_flux = result.flux_wb;
            CHECK(result.flux_wb == original.flux.flux_wb);
        } else if (error == ELECTRICAL_SPEED_SCALE) {
            CHECK(fabs(result.flux_wb / nominal_flux - 1.0 / 1.08) < 0.001);
        } else if (error == ELECTRICAL_SHARED_TERMINAL_GAIN) {

            CHECK(fabs(result.flux_wb / nominal_flux - 1.0) > 0.015);
        }
    }

    /* A separate model raises only winding resistance. Nothing writes back to the profile. */
    struct foc_virtual_current_profile heated = profile;
    heated.motor.rs_ohm *= 1.25;
    heated.initial_state.mechanical_position_rad = 0.37;
    ++request.session;
    struct foc_virtual_calibration heat_training = {0};
    struct electrical_rows hot_rows = {0};
    CHECK(capture_electrical(name, &heated, &request, &heat_training, &hot_rows) == 0);
    const struct foc_motor_rl_model hot = heat_training.workflow.result.electrical_model;
    heated.initial_state.mechanical_position_rad = -0.81;
    ++request.session;
    struct foc_virtual_calibration heat_holdout = {0};
    struct electrical_rows holdout_rows = {0};
    CHECK(capture_electrical(name, &heated, &request, &heat_holdout, &holdout_rows) == 0);
    CHECK(replay_rl("hot-training", &hot_rows, ELECTRICAL_NOMINAL, trace) == 0);
    CHECK(replay_rl("hot-holdout", &holdout_rows, ELECTRICAL_NOMINAL, trace) == 0);
    for (unsigned int axis = 0U; axis < 2U; ++axis) {
        double rms[2];
        const unsigned int windows =
            (unsigned int)hot_holdout_rms(holdout_rows.rl[axis], &cold, &hot, axis, rms);
        printf("THERMAL,%s,%u,%" PRIu32 ",%" PRIu32 ",%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
               name, axis, heat_training.workflow.result.request.session,
               heat_holdout.workflow.result.request.session, windows, profile.motor.rs_ohm,
               heated.motor.rs_ohm, (double)cold.rs_ohm, (double)hot.rs_ohm, rms[0], rms[1]);
        CHECK(windows == 96U && rms[1] < rms[0] * 0.20);
    }
    CHECK(fabs(hot.rs_ohm / heated.motor.rs_ohm - 1.0) < 0.03);
    CHECK(fabs(cold.rs_ohm / heated.motor.rs_ohm - 1.0) > 0.15);
    return 0;
}

static int permit_angle_capture(void *argument, uint32_t aperture_us)
{
    (void)aperture_us;
    struct foc_virtual_current *owner = argument;
    struct foc_runtime_snapshot view;
    return foc_virtual_current_snapshot(owner, &view) == FOC_RUNTIME_OK &&
        view.state == FOC_SAFE_STATE_DISABLED && view.faults == 0U &&
        view.output_confirmed && view.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        owner->output.accepted_updates == 0U ? 0 : -1;
}

static void rotate_terminal(struct foc_bootstrap_sample *sample, double angle)
{
    const double x = sample->emf_alpha_v;
    const double y = sample->emf_beta_v;
    sample->emf_alpha_v = (float)(x * cos(angle) - y * sin(angle));
    sample->emf_beta_v = (float)(x * sin(angle) + y * cos(angle));
}

static int test_reference(const char *name, bool trace)
{
    struct foc_virtual_current_profile profile;
    struct foc_cal_workflow_request request;
    CHECK(foc_virtual_calibration_profile(name, &profile, &request) == FOC_VIRTUAL_CAL_OK);
    struct foc_virtual_current original;
    CHECK(foc_virtual_current_init(&original, &profile) == 0);
    struct foc_cal_workflow_identity identity = request.identity;
    identity.acquisition.pole_pairs = 0U;
    identity.acquisition.encoder_direction = 0;
    const struct foc_virtual_bootstrap_capture_request schedule = {
        .identity = identity, .capture = 81U, .started_us = 9600U,
        .permit = permit_angle_capture, .argument = &original,
    };
    const struct foc_bootstrap_config config = {
        .identity = identity, .capture = schedule.capture,
        .source = FOC_CAL_WORKFLOW_HOST_VIRTUAL_MEASUREMENT,
        .minimum_pole_pairs = 1U, .maximum_pole_pairs = 14U,
        .minimum_samples_per_sweep = 40U, .maximum_interval_us = 5000U,
        .minimum_mechanical_span_rad = 0.7f, .maximum_residual_rad = 0.085f,
        .minimum_candidate_separation_rad = 0.08f, .maximum_offset_disagreement_rad = 0.10f,
        .minimum_emf_v = 0.04f, .minimum_abs_speed_rad_s = 1.0f,
        .maximum_abs_speed_rad_s = 10.0f, .maximum_motion_error_rad = 0.003f,
    };
    struct foc_virtual_bootstrap_capture_result capture;
    CHECK(foc_virtual_bootstrap_capture(&original.motor, &profile, &schedule, &capture) == 0);
    CHECK(capture.count == FOC_BOOTSTRAP_SWEEPS * FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP);
    struct foc_bootstrap_result nominal;
    CHECK(foc_bootstrap_fit(&config, capture.samples, capture.count, &nominal) ==
          FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED);
    static const char *const names[] = {"constant-reference-phase-plus-0.08rad",
                                       "directional-reference-phase-plus-minus-0.09rad",
                                       "limited-scan"};
    static const enum foc_bootstrap_status expected[] = {FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED,
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE, FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION};
    for (unsigned int experiment = 0U; experiment < sizeof names / sizeof names[0]; ++experiment) {
        struct foc_virtual_bootstrap_capture_result changed = capture;
        uint32_t count = changed.count;
        if (experiment < 2U) {
            for (uint32_t i = 0U; i < count; ++i) {
                struct foc_bootstrap_sample *row = &changed.samples[i];
                rotate_terminal(row, experiment == 0U ? 0.08 : copysign(0.09, row->shaft_rad_s));
                if (trace) {
                    printf("BOOT_ROW,%s,%s,%u,%u,%u,%u,%.9g,%.9g,%.9g\n", name, names[experiment],
                           row->sequence, row->sweep, row->captured_us, row->raw_code,
                           (double)row->shaft_rad_s, (double)row->emf_alpha_v,
                           (double)row->emf_beta_v);
                }
            }
        } else {
            count = 0U;
            for (uint32_t i = 0U; i < capture.count; ++i) {
                if (i % FOC_VIRTUAL_BOOTSTRAP_SAMPLES_PER_SWEEP < 16U) {
                    changed.samples[count] = capture.samples[i];
                    changed.samples[count].sequence = count + 1U;
                    ++count;
                }
            }
        }
        struct foc_bootstrap_result result;
        const enum foc_bootstrap_status status = foc_bootstrap_fit(&config, changed.samples, count, &result);
        const double delta = status == FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED
            ? remainder(result.electrical_offset_rad - nominal.electrical_offset_rad, tau) : 0.0;
        printf("BOOT_REF,%s,%s,%u,%u,%.9g,%.9g,%.9g,%.9g\n", name, names[experiment],
               (unsigned int)status, count, (double)nominal.electrical_offset_rad,
               (double)result.electrical_offset_rad, delta, (double)result.maximum_residual_rad);
        CHECK(status == expected[experiment]);
        if (experiment == 0U) {
            CHECK(fabs(delta - 0.08) < 0.00001 &&
                  fabs(result.maximum_residual_rad - nominal.maximum_residual_rad) < 0.00001);
        }
    }
    CHECK(permit_angle_capture(&original, capture.completed_us) == 0);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const bool trace = argc == 2 && strcmp(argv[1], "--trace") == 0;
    if (argc != 1 && !trace) {
        return 2;
    }
    CHECK(test_electrical("GENERIC_RESEARCH_MOTOR", trace) == 0);
    CHECK(test_electrical("CALIBRATION_SALIENT_MOTOR", trace) == 0);
    CHECK(test_reference("GENERIC_RESEARCH_MOTOR", trace) == 0);
    CHECK(test_reference("CALIBRATION_SALIENT_MOTOR", trace) == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
