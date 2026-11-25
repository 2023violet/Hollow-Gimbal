/**
 * @file    test_foc_svpwm.c
 * @brief   Host contract tests for G2 SVPWM limits and sector behavior.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Test role
 * ---------
 * This executable is the host acceptance owner for the SVPWM contract.
 *
 * This test checks expected duties, sector continuity, limiting, and the
 * non-mutating input contract against the Core implementation alone.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static int duty_is_in_range(const struct foc_svpwm_output *output, float duty_min, float duty_max)
{
    return (output->duty_a >= duty_min) && (output->duty_a <= duty_max) &&
        (output->duty_b >= duty_min) && (output->duty_b <= duty_max) &&
        (output->duty_c >= duty_min) && (output->duty_c <= duty_max);
}

static int output_is_finite(const struct foc_svpwm_output *output)
{
    return isfinite(output->duty_a) != 0 && isfinite(output->duty_b) != 0 &&
        isfinite(output->duty_c) != 0;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    const struct foc_config config = {
        .vbus_min_v = 6.0f,
        .voltage_limit_v = 4.0f,
        .duty_min = 0.28f,
        .duty_max = 0.72f,
        .max_dt_s = 0.01f,
    };
    struct foc_vector_alpha_beta vector = {0.0f, 0.0f};
    struct foc_svpwm_output output = {0};

    TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &output) == FOC_STATUS_OK);
    TEST_ASSERT(fabsf(output.duty_a - 0.5f) < 1.0e-6f);
    TEST_ASSERT(fabsf(output.duty_b - 0.5f) < 1.0e-6f);
    TEST_ASSERT(fabsf(output.duty_c - 0.5f) < 1.0e-6f);
    TEST_ASSERT(output.saturated == 0U);
    TEST_ASSERT(duty_is_in_range(&output, config.duty_min, config.duty_max));
    TEST_ASSERT(fabsf(output.duty_a + output.duty_b + output.duty_c - 1.5f) < 1.0e-6f);

    vector.alpha = 2.0f;
    vector.beta = 0.0f;
    {
        const struct foc_vector_alpha_beta vector_before = vector;

        TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &output) == FOC_STATUS_OK);
        TEST_ASSERT(fabsf(output.duty_a - 0.625f) < 1.0e-5f);
        TEST_ASSERT(fabsf(output.duty_b - 0.375f) < 1.0e-5f);
        TEST_ASSERT(fabsf(output.duty_c - 0.375f) < 1.0e-5f);
        TEST_ASSERT(output.sector == 1U);
        TEST_ASSERT(memcmp(&vector, &vector_before, sizeof(vector)) == 0);
        TEST_ASSERT(fabsf(output.duty_a + output.duty_b + output.duty_c - 1.375f) < 1.0e-5f);
    }

    vector.alpha = 0.0f;
    vector.beta = 2.0f;
    TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &output) == FOC_STATUS_OK);
    TEST_ASSERT(fabsf(output.duty_a - 0.5f) < 1.0e-5f);
    TEST_ASSERT(fabsf(output.duty_b - 0.6443376f) < 1.0e-5f);
    TEST_ASSERT(fabsf(output.duty_c - 0.3556624f) < 1.0e-5f);
    TEST_ASSERT(output.sector == 2U);
    TEST_ASSERT(duty_is_in_range(&output, config.duty_min, config.duty_max));
    TEST_ASSERT(fabsf(output.duty_a + output.duty_b + output.duty_c - 1.5f) < 1.0e-5f);

    {
        const float pi = 3.14159265358979323846f;
        const struct {
            float alpha;
            float beta;
            unsigned int sector;
        } signed_zero[] = {
            {0.0f, 0.0f, 1U},
            {-0.0f, 0.0f, 4U},
            {0.0f, -0.0f, 1U},
            {-0.0f, -0.0f, 4U},
            {-1.0f, -0.0f, 4U}
        };
        const unsigned int boundary_sector[] = {1U, 2U, 3U, 4U, 5U, 6U, 1U};

        for (unsigned int index = 0U; index < sizeof(signed_zero) / sizeof(signed_zero[0]);
             ++index) {
            vector.alpha = signed_zero[index].alpha;
            vector.beta = signed_zero[index].beta;
            TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &output) == FOC_STATUS_OK);
            TEST_ASSERT(output.sector == signed_zero[index].sector);
        }
        for (unsigned int index = 0U; index < sizeof(boundary_sector) / sizeof(boundary_sector[0]);
             ++index) {
            const float angle = (float)index * pi / 3.0f;
            vector.alpha = cosf(angle);
            vector.beta = sinf(angle);
            TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &output) == FOC_STATUS_OK);
            TEST_ASSERT(output.sector == boundary_sector[index]);
        }
    }

    {
        const float pi = 3.14159265358979323846f;
        const float epsilon = 1.0e-4f;
        unsigned int boundary;

        for (boundary = 0U; boundary < 6U; ++boundary) {
            const float angle = (float)boundary * pi / 3.0f;
            struct foc_svpwm_output before = {0};
            struct foc_svpwm_output after = {0};

            vector.alpha = cosf(angle - epsilon);
            vector.beta = sinf(angle - epsilon);
            TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &before) == FOC_STATUS_OK);
            vector.alpha = cosf(angle + epsilon);
            vector.beta = sinf(angle + epsilon);
            TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &after) == FOC_STATUS_OK);
            TEST_ASSERT(output_is_finite(&before));
            TEST_ASSERT(output_is_finite(&after));
            TEST_ASSERT(fabsf(before.duty_a - after.duty_a) < 1.0e-4f);
            TEST_ASSERT(fabsf(before.duty_b - after.duty_b) < 1.0e-4f);
            TEST_ASSERT(fabsf(before.duty_c - after.duty_c) < 1.0e-4f);
        }
    }

    {
        const float angles[] = {
            0.5235988f,
            1.5707963f,
            2.6179938f,
            3.6651914f,
            4.7123890f,
            5.7595866f,
        };
        unsigned int expected_sector;

        for (expected_sector = 1U; expected_sector <= 6U; ++expected_sector) {
            vector.alpha = cosf(angles[expected_sector - 1U]);
            vector.beta = sinf(angles[expected_sector - 1U]);
            TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &output) == FOC_STATUS_OK);
            TEST_ASSERT(output.sector == expected_sector);
            TEST_ASSERT(duty_is_in_range(&output, config.duty_min, config.duty_max));
        }
    }

    vector.alpha = 20.0f;
    vector.beta = 20.0f;
    TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &output) == FOC_STATUS_OK);
    TEST_ASSERT(output.saturated != 0U);
    TEST_ASSERT(duty_is_in_range(&output, config.duty_min, config.duty_max));

    TEST_ASSERT(foc_svpwm(&vector, 0.0f, &config, &output) == FOC_STATUS_ERR_INVALID_VBUS);
    TEST_ASSERT(foc_svpwm(&vector, NAN, &config, &output) == FOC_STATUS_ERR_INVALID_VBUS);
    TEST_ASSERT(output_is_finite(&output));
    TEST_ASSERT(output.sector == 0U);
    TEST_ASSERT(output.saturated != 0U);
    vector.alpha = NAN;
    TEST_ASSERT(foc_svpwm(&vector, 12.0f, &config, &output) == FOC_STATUS_ERR_NONFINITE);
    TEST_ASSERT(output_is_finite(&output));
    TEST_ASSERT(foc_svpwm(NULL, 12.0f, &config, &output) == FOC_STATUS_ERR_NULL);

    puts("test_foc_svpwm passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
