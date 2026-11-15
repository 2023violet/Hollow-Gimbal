/**
 * @file    test_foc_current_reference.c
 * @brief   Independent double reference for the d/q current PI controller.
 * @version v0.1.0
 * @date    2026.11.14
 *
 * Design overview
 * ---------------
 * Recompute phase-to-d/q feedback, target limiting, PI integration, and the
 * unsaturated voltage request without calling production helpers for expected
 * values. The production controller is exercised only to obtain the result
 * under test.
 *
 * Test role
 * ---------
 * Cover deterministic normal-current trajectories, target limiting, and
 * transactional rejection of invalid timing.
 *
 * Implementation role
 * -------------------
 * Keep reference arithmetic in double precision with a fixed pseudo-random
 * sequence so a mismatch identifies a reproducible sample.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_current.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

static const double REF_PI = 3.14159265358979323846;
static const double REF_TWO_PI = 6.28318530717958647692;
static const double REF_INV_SQRT3 = 0.57735026918962576451;

/* Private Helpers -------------------------------------------------------------- */

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static float random_float(uint32_t *state, float low, float high)
{
    const double unit = (double)(next_random(state) & 0x00ffffffU) / 16777215.0;
    return (float)((double)low + ((double)high - (double)low) * unit);
}

static double wrap_angle(double angle)
{
    double wrapped = fmod(angle, REF_TWO_PI);
    if (wrapped < 0.0) {
        wrapped += REF_TWO_PI;
    }
    return wrapped;
}

static void limit_vector(double *d, double *q, double limit, unsigned int *limited)
{
    const double largest = fmax(fabs(*d), fabs(*q));
    *limited = 0U;
    if (largest > 0.0) {
        const double unit_d = *d / largest;
        const double unit_q = *q / largest;
        const double unit_length = sqrt(unit_d * unit_d + unit_q * unit_q);
        const double allowed = limit / unit_length;
        if (largest > allowed) {
            *d = unit_d * allowed;
            *q = unit_q * allowed;
            *limited = 1U;
        }
    }
}

static void phase_to_dq(struct foc_vector_abc phase, double theta, double *d, double *q)
{
    const double alpha = (double)phase.a;
    const double beta = ((double)phase.b - (double)phase.c) * REF_INV_SQRT3;
    const double wrapped = wrap_angle(theta);
    const double cosine = cos(wrapped);
    const double sine = sin(wrapped);
    *d = alpha * cosine + beta * sine;
    *q = beta * cosine - alpha * sine;
}

static int close_enough(double actual, double expected, double tolerance)
{
    const double scale = fmax(1.0, fmax(fabs(actual), fabs(expected)));
    return fabs(actual - expected) <= tolerance * scale;
}

static int fail_value(const char *name, unsigned int index, double actual, double expected)
{
    fprintf(
        stderr,
        "%s mismatch index=%u actual=%.9g expected=%.9g\n",
        name,
        index,
        actual,
        expected
    );
    return 1;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    const struct foc_current_config config = {
        .pwm =
            {
                .vbus_min_v = 6.0f,
                .voltage_limit_v = 12.0f,
                .duty_min = 0.02f,
                .duty_max = 0.98f,
                .max_dt_s = 0.002f,
            },
        .kp_v_per_a = {1.3f, 1.7f},
        .ki_v_per_a_s = {18.0f, 23.0f},
        .current_limit_a = 8.0f,
    };
    struct foc_current_context context = {0};
    uint32_t seed = 0x2d8f11a7U;
    unsigned int limited_samples = 0U;

    if (foc_current_init(&context, &config) != FOC_STATUS_OK) {
        fprintf(stderr, "current reference initialization failed\n");
        return 1;
    }

    for (unsigned int index = 0U; index < 4096U; ++index) {
        const float theta = random_float(&seed, (float)(-8.0 * REF_PI), (float)(8.0 * REF_PI));
        const float dt = random_float(&seed, 0.00005f, 0.0015f);
        const float target_d = random_float(&seed, -2.0f, 2.0f);
        const float target_q = random_float(&seed, -2.0f, 2.0f);
        const float current_d = random_float(&seed, -1.0f, 1.0f);
        const float current_q = random_float(&seed, -1.0f, 1.0f);
        const double angle = wrap_angle((double)theta);
        const double cosine = cos(angle);
        const double sine = sin(angle);
        const struct foc_vector_abc phase = {
            (float)(current_d * cosine - current_q * sine),
            (float)(-0.5 * (current_d * cosine - current_q * sine) +
                    0.86602540378443864676 * (current_d * sine + current_q * cosine)),
            (float)(-0.5 * (current_d * cosine - current_q * sine) -
                    0.86602540378443864676 * (current_d * sine + current_q * cosine)),
        };
        const struct foc_current_input input = {
            .phase_current_a = phase,
            .theta_elec_rad = theta,
            .target_a = {target_d, target_q},
            .vbus_v = 24.0f,
            .dt_s = dt,
        };
        struct foc_current_output output;
        double measured_d;
        double measured_q;
        double target_ref_d = target_d;
        double target_ref_q = target_q;
        unsigned int target_limited;
        const double previous_d = context.integral_v.d;
        const double previous_q = context.integral_v.q;
        double expected_integral_d;
        double expected_integral_q;
        double expected_voltage_d;
        double expected_voltage_q;

        phase_to_dq(phase, theta, &measured_d, &measured_q);
        limit_vector(&target_ref_d, &target_ref_q, config.current_limit_a, &target_limited);
        expected_integral_d =
            previous_d + (double)config.ki_v_per_a_s.d * (double)dt * (target_ref_d - measured_d);
        expected_integral_q =
            previous_q + (double)config.ki_v_per_a_s.q * (double)dt * (target_ref_q - measured_q);
        expected_voltage_d =
            (double)config.kp_v_per_a.d * (target_ref_d - measured_d) + expected_integral_d;
        expected_voltage_q =
            (double)config.kp_v_per_a.q * (target_ref_q - measured_q) + expected_integral_q;

        if (foc_current_step(&context, &input, &output) != FOC_STATUS_OK ||
            output.output_enable != 1U || output.svpwm.saturated != 0U) {
            fprintf(stderr, "unexpected current step status or saturation index=%u\n", index);
            return 1;
        }
        if (output.target_limited != target_limited) {
            return fail_value("target_limited", index, output.target_limited, target_limited);
        }
        if (!close_enough(output.measured_dq_a.d, measured_d, 4.0e-5) ||
            !close_enough(output.measured_dq_a.q, measured_q, 4.0e-5)) {
            fprintf(stderr, "measured dq mismatch index=%u\n", index);
            return 1;
        }
        if (!close_enough(output.voltage_dq_v.d, expected_voltage_d, 2.0e-4) ||
            !close_enough(output.voltage_dq_v.q, expected_voltage_q, 2.0e-4) ||
            !close_enough(context.integral_v.d, expected_integral_d, 2.0e-4) ||
            !close_enough(context.integral_v.q, expected_integral_q, 2.0e-4)) {
            fprintf(stderr, "current PI mismatch index=%u\n", index);
            return 1;
        }
        limited_samples += target_limited;
    }

    {
        struct foc_current_config limited_config = config;
        struct foc_current_context limited_context = {0};
        const struct foc_current_input limited_input = {
            .phase_current_a = {0.0f, 0.0f, 0.0f},
            .theta_elec_rad = 0.0f,
            .target_a = {3.0f, 4.0f},
            .vbus_v = 24.0f,
            .dt_s = 0.001f,
        };
        struct foc_current_output limited_output;

        limited_config.kp_v_per_a = (struct foc_vector_dq){0.2f, 0.2f};
        limited_config.ki_v_per_a_s = (struct foc_vector_dq){0.0f, 0.0f};
        limited_config.current_limit_a = 2.0f;
        if (foc_current_init(&limited_context, &limited_config) != FOC_STATUS_OK ||
            foc_current_step(&limited_context, &limited_input, &limited_output) != FOC_STATUS_OK ||
            limited_output.target_limited != 1U || limited_output.svpwm.saturated != 0U ||
            !close_enough(limited_output.voltage_dq_v.d, 0.24, 2.0e-4) ||
            !close_enough(limited_output.voltage_dq_v.q, 0.32, 2.0e-4)) {
            fprintf(stderr, "current target limiting reference failed\n");
            return 1;
        }
        ++limited_samples;
    }

    {
        const struct foc_current_context before = context;
        const struct foc_current_input invalid = {
            .phase_current_a = {0.0f, 0.0f, 0.0f},
            .theta_elec_rad = 0.0f,
            .target_a = {0.0f, 0.0f},
            .vbus_v = 24.0f,
            .dt_s = 0.0f,
        };
        struct foc_current_output output = {.output_enable = 1U};
        if (foc_current_step(&context, &invalid, &output) != FOC_STATUS_ERR_INVALID_DT ||
            memcmp(&context, &before, sizeof(context)) != 0 || output.output_enable != 0U ||
            output.svpwm.duty_a != 0.0f || output.svpwm.duty_b != 0.0f ||
            output.svpwm.duty_c != 0.0f) {
            fprintf(stderr, "invalid dt transaction failed\n");
            return 1;
        }
    }

    printf(
        "test_foc_current_reference passed samples=4096 target_limited=%u transactional=1\n",
        limited_samples
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
