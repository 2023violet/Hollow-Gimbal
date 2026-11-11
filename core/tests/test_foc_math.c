/**
 * @file    test_foc_math.c
 * @brief   Host contract tests for pure G2 Core transforms.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Test role
 * ---------
 * This executable is the host acceptance owner for the pure transform contract.
 *
 * This test owns no production state and validates expected vectors, angle
 * wrapping, non-finite rejection, and input immutability.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_core.h"

#include <float.h>
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

static int nearly_equal(float lhs, float rhs, float tolerance)
{
    return fabsf(lhs - rhs) <= tolerance;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    const float tolerance = 1.0e-5f;
    const float two_pi = 6.28318530717958647692f;
    struct foc_vector_abc abc = {1.0f, 0.0f, -1.0f};
    struct foc_vector_abc abc_before = abc;
    struct foc_vector_alpha_beta alpha_beta = {0.0f, 0.0f};
    struct foc_vector_dq dq = {0.0f, 0.0f};
    struct foc_vector_alpha_beta round_trip = {0.0f, 0.0f};
    struct foc_vector_alpha_beta source = {0.8f, -0.2f};
    struct foc_vector_alpha_beta source_before = source;

    TEST_ASSERT(foc_clarke(&abc, &alpha_beta) == FOC_STATUS_OK);
    TEST_ASSERT(nearly_equal(alpha_beta.alpha, 1.0f, tolerance));
    TEST_ASSERT(nearly_equal(alpha_beta.beta, 0.57735026f, tolerance));
    TEST_ASSERT(memcmp(&abc, &abc_before, sizeof(abc)) == 0);

    TEST_ASSERT(foc_park(&source, 0.73f, &dq) == FOC_STATUS_OK);
    TEST_ASSERT(
        nearly_equal(dq.d, source.alpha * cosf(0.73f) + source.beta * sinf(0.73f), tolerance)
    );
    TEST_ASSERT(
        nearly_equal(dq.q, source.beta * cosf(0.73f) - source.alpha * sinf(0.73f), tolerance)
    );
    TEST_ASSERT(foc_inverse_park(&dq, 0.73f, &round_trip) == FOC_STATUS_OK);
    TEST_ASSERT(nearly_equal(round_trip.alpha, source.alpha, tolerance));
    TEST_ASSERT(nearly_equal(round_trip.beta, source.beta, tolerance));
    TEST_ASSERT(memcmp(&source, &source_before, sizeof(source)) == 0);

    TEST_ASSERT(foc_park(&source, 0.0f, &dq) == FOC_STATUS_OK);
    TEST_ASSERT(nearly_equal(dq.d, source.alpha, tolerance));
    TEST_ASSERT(nearly_equal(dq.q, source.beta, tolerance));
    {
        struct foc_vector_dq negative_angle = {0.0f, 0.0f};
        struct foc_vector_dq wrapped_angle = {0.0f, 0.0f};

        TEST_ASSERT(foc_park(&source, -1.0f, &negative_angle) == FOC_STATUS_OK);
        TEST_ASSERT(foc_park(&source, two_pi - 1.0f, &wrapped_angle) == FOC_STATUS_OK);
        TEST_ASSERT(nearly_equal(negative_angle.d, wrapped_angle.d, tolerance));
        TEST_ASSERT(nearly_equal(negative_angle.q, wrapped_angle.q, tolerance));
    }

    source.alpha = NAN;
    TEST_ASSERT(foc_park(&source, 0.0f, &dq) == FOC_STATUS_ERR_NONFINITE);
    source.alpha = 0.8f;
    source.beta = INFINITY;
    TEST_ASSERT(foc_park(&source, 0.0f, &dq) == FOC_STATUS_ERR_NONFINITE);
    {
        const struct foc_vector_dq invalid_dq = {NAN, 1.0f};

        TEST_ASSERT(foc_inverse_park(&invalid_dq, 0.0f, &round_trip) == FOC_STATUS_ERR_NONFINITE);
    }
    TEST_ASSERT(foc_clarke(NULL, &alpha_beta) == FOC_STATUS_ERR_NULL);
    TEST_ASSERT(foc_inverse_park(&dq, INFINITY, &round_trip) == FOC_STATUS_ERR_NONFINITE);

    {
        const struct foc_vector_abc overflowing_abc = {FLT_MAX, FLT_MAX, -FLT_MAX};

        TEST_ASSERT(foc_clarke(&overflowing_abc, &alpha_beta) == FOC_STATUS_ERR_NONFINITE);
        TEST_ASSERT(isfinite(alpha_beta.alpha) != 0);
        TEST_ASSERT(isfinite(alpha_beta.beta) != 0);
    }

    puts("test_foc_math passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
