/**
 * @file    test_foc_math_reference.c
 * @brief   Independent double-precision reference checks for Core transforms.
 * @version v0.1.0
 * @date    2026.11.14
 *
 * Design overview
 * ---------------
 * Recompute Clarke, Park, inverse Park, and bounded SVPWM with double
 * precision formulas instead of calling private production helpers. The
 * production API is exercised only as the implementation under test.
 *
 * Test role
 * ---------
 * Provide an independent numerical oracle for the public Core transforms.
 * This test must not call private production helpers to construct expectations.
 *
 * Implementation role
 * -------------------
 * Exercise public Core APIs over deterministic random and invalid-input sets.
 *
 * Failure / recovery
 * ------------------
 * Any mismatch aborts the test with the sample index and values. Invalid
 * input checks also require the complete safe output contract.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_core.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Private Constants ------------------------------------------------------------ */

static const double REF_INV_SQRT3 = 0.57735026918962576451;
static const double REF_SQRT3_OVER_TWO = 0.86602540378443864676;
static const double REF_VECTOR_LIMIT = 2.0 / 3.0;
static const double REF_PI = 3.14159265358979323846;
static const float TEST_PI = 3.14159265358979323846f;

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

static unsigned int reference_sector(double alpha, double beta)
{
    const double angle = atan2(beta, alpha);
    double wrapped = angle;

    if (wrapped < 0.0) {
        wrapped += 2.0 * REF_PI;
    }
    if (wrapped >= 2.0 * REF_PI) {
        wrapped -= 2.0 * REF_PI;
    }
    return (unsigned int)floor(wrapped / (REF_PI / 3.0)) + 1U;
}

static int check_transforms(void)
{
    uint32_t seed = 0x2468ace1U;
    double max_park_d_error = 0.0;
    double max_park_q_error = 0.0;
    double max_inverse_alpha_error = 0.0;
    double max_inverse_beta_error = 0.0;

    for (unsigned int index = 0U; index < 10000U; ++index) {
        const struct foc_vector_abc abc = {
            random_float(&seed, -20.0f, 20.0f),
            random_float(&seed, -20.0f, 20.0f),
            random_float(&seed, -20.0f, 20.0f),
        };
        const float theta = random_float(&seed, -20.0f * TEST_PI, 20.0f * TEST_PI);
        struct foc_vector_alpha_beta alpha_beta;
        struct foc_vector_dq dq;
        struct foc_vector_alpha_beta recovered;
        const double reference_alpha = (double)abc.a;
        const double reference_beta = ((double)abc.b - (double)abc.c) * REF_INV_SQRT3;
        const double cosine = cos((double)theta);
        const double sine = sin((double)theta);
        const double reference_d = reference_alpha * cosine + reference_beta * sine;
        const double reference_q = reference_beta * cosine - reference_alpha * sine;

        if (foc_clarke(&abc, &alpha_beta) != FOC_STATUS_OK) {
            fprintf(stderr, "Clarke status mismatch index=%u\n", index);
            return 1;
        }
        if (!close_enough(alpha_beta.alpha, reference_alpha, 3.0e-6)) {
            return fail_value("Clarke alpha", index, alpha_beta.alpha, reference_alpha);
        }
        if (!close_enough(alpha_beta.beta, reference_beta, 3.0e-6)) {
            return fail_value("Clarke beta", index, alpha_beta.beta, reference_beta);
        }
        if (foc_park(&alpha_beta, theta, &dq) != FOC_STATUS_OK) {
            fprintf(stderr, "Park status mismatch index=%u\n", index);
            return 1;
        }
        max_park_d_error = fmax(max_park_d_error, fabs((double)dq.d - reference_d));
        max_park_q_error = fmax(max_park_q_error, fabs((double)dq.q - reference_q));
        if (!close_enough(dq.d, reference_d, 1.0e-4)) {
            return fail_value("Park d", index, dq.d, reference_d);
        }
        if (!close_enough(dq.q, reference_q, 1.0e-4)) {
            return fail_value("Park q", index, dq.q, reference_q);
        }
        if (foc_inverse_park(&dq, theta, &recovered) != FOC_STATUS_OK) {
            fprintf(stderr, "inverse Park status mismatch index=%u\n", index);
            return 1;
        }
        max_inverse_alpha_error =
            fmax(max_inverse_alpha_error, fabs((double)recovered.alpha - (double)alpha_beta.alpha));
        max_inverse_beta_error =
            fmax(max_inverse_beta_error, fabs((double)recovered.beta - (double)alpha_beta.beta));
        if (!close_enough(recovered.alpha, alpha_beta.alpha, 1.0e-4)) {
            return fail_value("inverse Park alpha", index, recovered.alpha, alpha_beta.alpha);
        }
        if (!close_enough(recovered.beta, alpha_beta.beta, 1.0e-4)) {
            return fail_value("inverse Park beta", index, recovered.beta, alpha_beta.beta);
        }
    }
    printf(
        "reference transforms max_abs park_d=%.9g park_q=%.9g inverse_alpha=%.9g "
        "inverse_beta=%.9g\n",
        max_park_d_error,
        max_park_q_error,
        max_inverse_alpha_error,
        max_inverse_beta_error
    );
    return 0;
}

static int check_svpwm(void)
{
    const struct foc_config config = {
        .vbus_min_v = 4.0f,
        .voltage_limit_v = 20.0f,
        .duty_min = 0.02f,
        .duty_max = 0.98f,
        .max_dt_s = 0.001f,
    };
    uint32_t seed = 0x13579bdfU;

    for (unsigned int index = 0U; index < 10000U; ++index) {
        const struct foc_vector_alpha_beta input = {
            random_float(&seed, -40.0f, 40.0f),
            random_float(&seed, -40.0f, 40.0f),
        };
        const float vbus = random_float(&seed, 5.0f, 30.0f);
        struct foc_svpwm_output actual;
        double alpha = (double)input.alpha / (double)vbus;
        double beta = (double)input.beta / (double)vbus;
        double magnitude = sqrt(alpha * alpha + beta * beta);
        double vector_limit = (double)config.voltage_limit_v / (double)vbus;
        unsigned int expected_saturated = 0U;
        double phase_a;
        double phase_b;
        double phase_c;
        double phase_max;
        double phase_min;
        double common_mode;
        double expected_a;
        double expected_b;
        double expected_c;

        if (vector_limit > REF_VECTOR_LIMIT) {
            vector_limit = REF_VECTOR_LIMIT;
        }
        if (magnitude > vector_limit) {
            const double scale = vector_limit / magnitude;
            alpha *= scale;
            beta *= scale;
            expected_saturated = 1U;
        }
        phase_a = alpha;
        phase_b = -0.5 * alpha + REF_SQRT3_OVER_TWO * beta;
        phase_c = -0.5 * alpha - REF_SQRT3_OVER_TWO * beta;
        phase_max = fmax(phase_a, fmax(phase_b, phase_c));
        phase_min = fmin(phase_a, fmin(phase_b, phase_c));
        common_mode = 0.5 * (phase_max + phase_min);
        expected_a = 0.5 + phase_a - common_mode;
        expected_b = 0.5 + phase_b - common_mode;
        expected_c = 0.5 + phase_c - common_mode;
        if (expected_a < config.duty_min) {
            expected_a = config.duty_min;
            expected_saturated = 1U;
        } else if (expected_a > config.duty_max) {
            expected_a = config.duty_max;
            expected_saturated = 1U;
        }
        if (expected_b < config.duty_min) {
            expected_b = config.duty_min;
            expected_saturated = 1U;
        } else if (expected_b > config.duty_max) {
            expected_b = config.duty_max;
            expected_saturated = 1U;
        }
        if (expected_c < config.duty_min) {
            expected_c = config.duty_min;
            expected_saturated = 1U;
        } else if (expected_c > config.duty_max) {
            expected_c = config.duty_max;
            expected_saturated = 1U;
        }
        if (foc_svpwm(&input, vbus, &config, &actual) != FOC_STATUS_OK) {
            fprintf(stderr, "SVPWM status mismatch index=%u\n", index);
            return 1;
        }
        if (!close_enough(actual.duty_a, expected_a, 5.0e-6)) {
            return fail_value("SVPWM duty_a", index, actual.duty_a, expected_a);
        }
        if (!close_enough(actual.duty_b, expected_b, 5.0e-6)) {
            return fail_value("SVPWM duty_b", index, actual.duty_b, expected_b);
        }
        if (!close_enough(actual.duty_c, expected_c, 5.0e-6)) {
            return fail_value("SVPWM duty_c", index, actual.duty_c, expected_c);
        }
        if (actual.saturated != expected_saturated) {
            fprintf(
                stderr,
                "SVPWM saturation mismatch index=%u actual=%u expected=%u\n",
                index,
                actual.saturated,
                expected_saturated
            );
            return 1;
        }
        if (actual.sector != reference_sector((double)input.alpha, (double)input.beta)) {
            const double angle = atan2((double)input.beta, (double)input.alpha);
            const double boundary = fmod(angle + 2.0 * REF_PI, REF_PI / 3.0);
            if (fabs(boundary) > 1.0e-5 && fabs(boundary - REF_PI / 3.0) > 1.0e-5) {
                fprintf(stderr, "SVPWM sector mismatch index=%u actual=%u\n", index, actual.sector);
                return 1;
            }
        }
    }
    return 0;
}

static int check_failures(void)
{
    struct foc_svpwm_output output;
    const struct foc_config config = {
        .vbus_min_v = 4.0f,
        .voltage_limit_v = 20.0f,
        .duty_min = 0.02f,
        .duty_max = 0.98f,
        .max_dt_s = 0.001f,
    };
    const struct foc_vector_alpha_beta input = {1.0f, 0.0f};

    if (foc_svpwm(&input, 0.0f, &config, &output) != FOC_STATUS_ERR_INVALID_VBUS ||
        output.sector != 0U || output.saturated == 0U || output.duty_a != 0.5f ||
        output.duty_b != 0.5f || output.duty_c != 0.5f) {
        fprintf(stderr, "invalid VBUS safe output mismatch\n");
        return 1;
    }
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    if (check_transforms() != 0 || check_svpwm() != 0 || check_failures() != 0) {
        return 1;
    }
    puts("test_foc_math_reference passed transforms=10000 svpwm=10000 failures=1");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
