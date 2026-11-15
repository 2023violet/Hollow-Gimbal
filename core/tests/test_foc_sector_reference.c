/**
 * @file    test_foc_sector_reference.c
 * @brief   Independent atan2f SVPWM sector and duty differential test.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Test role
 * ---------
 * Reconstruct the PERF-01A sector and common-mode duty rules independently,
 * then compare the optimized public SVPWM result over directed boundaries and
 * 100,000 deterministic finite vectors. The reference never calls the
 * optimized sector helper.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_core.h"

#include <math.h>
#include <stdio.h>

/* Private Constants ----------------------------------------------------------- */
static const float REFERENCE_PI = 3.14159265358979323846f;
static const float REFERENCE_TWO_PI = 6.28318530717958647692f;
static const float REFERENCE_SQRT3_OVER_TWO = 0.86602540378443864676f;
static const float REFERENCE_VECTOR_LIMIT = 0.66666666666666666667f;

/* Private Helpers -------------------------------------------------------------- */

static unsigned int reference_sector(float alpha, float beta)
{
    float angle = atan2f(beta, alpha);
    if (angle < 0.0f) {
        angle += REFERENCE_TWO_PI;
    }
    unsigned int sector = (unsigned int)(angle / (REFERENCE_PI / 3.0f)) + 1U;
    if (sector > 6U) {
        sector = 6U;
    }
    return sector;
}

static unsigned int reference_svpwm(
    const struct foc_vector_alpha_beta *input,
    float vbus_v,
    const struct foc_config *config,
    struct foc_svpwm_output *output
)
{
    float alpha = input->alpha / vbus_v;
    float beta = input->beta / vbus_v;
    float magnitude = sqrtf(alpha * alpha + beta * beta);
    float vector_limit = config->voltage_limit_v / vbus_v;
    unsigned int saturated = 0U;

    if (vector_limit > REFERENCE_VECTOR_LIMIT) {
        vector_limit = REFERENCE_VECTOR_LIMIT;
    }
    if (magnitude > vector_limit) {
        const float scale = vector_limit / magnitude;
        alpha *= scale;
        beta *= scale;
        saturated = 1U;
    }

    const float phase_a = alpha;
    const float phase_b = -0.5f * alpha + REFERENCE_SQRT3_OVER_TWO * beta;
    const float phase_c = -0.5f * alpha - REFERENCE_SQRT3_OVER_TWO * beta;
    const float phase_max = fmaxf(phase_a, fmaxf(phase_b, phase_c));
    const float phase_min = fminf(phase_a, fminf(phase_b, phase_c));
    const float common_mode = 0.5f * (phase_max + phase_min);
    float duties[3] = {
        0.5f + phase_a - common_mode,
        0.5f + phase_b - common_mode,
        0.5f + phase_c - common_mode,
    };

    for (unsigned int index = 0U; index < 3U; ++index) {
        if (duties[index] < config->duty_min) {
            duties[index] = config->duty_min;
            saturated = 1U;
        } else if (duties[index] > config->duty_max) {
            duties[index] = config->duty_max;
            saturated = 1U;
        }
    }
    output->duty_a = duties[0];
    output->duty_b = duties[1];
    output->duty_c = duties[2];
    output->sector = reference_sector(input->alpha, input->beta);
    output->saturated = saturated;
    return FOC_STATUS_OK;
}

static uint32_t next_random(uint32_t *state)
{
    *state = *state * UINT32_C(1664525) + UINT32_C(1013904223);
    return *state;
}

static float random_signed(uint32_t *state, float scale)
{
    const float unit =
        (float)(next_random(state) & UINT32_C(0x00ffffff)) / (float)UINT32_C(0x01000000);
    return (2.0f * unit - 1.0f) * scale;
}

static int compare_case(
    const struct foc_vector_alpha_beta *input,
    float vbus_v,
    const struct foc_config *config,
    unsigned int index
)
{
    struct foc_svpwm_output actual = {0};
    struct foc_svpwm_output expected = {0};
    const int actual_status = foc_svpwm(input, vbus_v, config, &actual);
    const int expected_status = reference_svpwm(input, vbus_v, config, &expected);

    if (actual_status != expected_status || actual.sector != expected.sector ||
        actual.saturated != expected.saturated ||
        fabsf(actual.duty_a - expected.duty_a) > 2.0e-6f ||
        fabsf(actual.duty_b - expected.duty_b) > 2.0e-6f ||
        fabsf(actual.duty_c - expected.duty_c) > 2.0e-6f) {
        fprintf(
            stderr,
            "sector mismatch index=%u alpha=%.9g beta=%.9g vbus=%.9g "
            "actual=(%d,%u,%u,%.9g,%.9g,%.9g) expected=(%d,%u,%u,%.9g,%.9g,%.9g)\n",
            index,
            input->alpha,
            input->beta,
            vbus_v,
            actual_status,
            actual.sector,
            actual.saturated,
            actual.duty_a,
            actual.duty_b,
            actual.duty_c,
            expected_status,
            expected.sector,
            expected.saturated,
            expected.duty_a,
            expected.duty_b,
            expected.duty_c
        );
        return 1;
    }
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    const struct foc_config config = {1.0f, 2.5f, 0.28f, 0.72f, 0.001f};
    const float pi = REFERENCE_PI;
    const float directed_angles[] = {
        0.0f,
        nextafterf(0.0f, 1.0f),
        nextafterf(pi / 3.0f, 0.0f),
        pi / 3.0f,
        nextafterf(pi / 3.0f, INFINITY),
        2.0f * pi / 3.0f,
        pi,
        4.0f * pi / 3.0f,
        5.0f * pi / 3.0f,
        nextafterf(2.0f * pi, 0.0f),
    };
    const struct foc_vector_alpha_beta signed_zero[] = {
        {0.0f, 0.0f},
        {-0.0f, 0.0f},
        {0.0f, -0.0f},
        {-0.0f, -0.0f},
        {-1.0f, -0.0f},
    };
    uint32_t random_state = UINT32_C(0x13579bdf);
    unsigned int total = 0U;

    for (unsigned int index = 0U; index < sizeof(directed_angles) / sizeof(directed_angles[0]);
         ++index) {
        const struct foc_vector_alpha_beta vector = {
            cosf(directed_angles[index]),
            sinf(directed_angles[index]),
        };
        if (compare_case(&vector, 12.0f, &config, total++) != 0) {
            return 1;
        }
    }
    for (unsigned int index = 0U; index < sizeof(signed_zero) / sizeof(signed_zero[0]); ++index) {
        if (compare_case(&signed_zero[index], 12.0f, &config, total++) != 0) {
            return 1;
        }
    }
    for (unsigned int index = 0U; index < 100000U; ++index) {
        const struct foc_vector_alpha_beta vector = {
            random_signed(&random_state, 8.0f),
            random_signed(&random_state, 8.0f),
        };
        const float vbus = 1.001f +
            (float)(next_random(&random_state) & UINT32_C(0x000fffff)) / 1048576.0f * 14.0f;
        if (compare_case(&vector, vbus, &config, total++) != 0) {
            return 1;
        }
    }
    {
        struct foc_svpwm_output output = {0};
        struct foc_vector_alpha_beta invalid = {NAN, 0.0f};
        if (foc_svpwm(&invalid, 12.0f, &config, &output) != FOC_STATUS_ERR_NONFINITE ||
            output.sector != 0U || output.saturated == 0U) {
            return 1;
        }
    }
    printf("test_foc_sector_reference passed samples=%u seed=0x13579bdf\n", total);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
