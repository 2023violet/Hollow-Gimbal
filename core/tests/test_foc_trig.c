/**
 * @file    test_foc_trig.c
 * @brief   Host accuracy and edge-case tests for foc_sincosf().
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Test role
 * ---------
 * Compares foc_sincosf() with a double-precision reference evaluated on the same
 * Float32 input, over a dense grid in the direct range (|x| <= 6400 rad), every
 * quadrant boundary and non-finite inputs; the bound is 1.5e-7 absolute. Beyond
 * 6400 rad the documented fmodf() wrap by the Float32 2*pi is checked against
 * |x| * 2.8e-8 + 1.5e-7.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_trig.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

static const double bound = 1.5e-7;
static double worst;
static float worst_angle;

/* Reference on the exact Float32 value; fmod in double is exact, so large inputs reduce first.
 * Beyond the direct range the bound grows with the Float32 2*pi wrap error. */
static int check(float angle)
{
    const double limit =
        fabs((double)angle) <= 6400.0 ? bound : fabs((double)angle) * 2.8e-8 + bound;
    float s;
    float c;
    const double reduced = fmod((double)angle, 6.283185307179586476925286766559);

    foc_sincosf(angle, &s, &c);
    const double error_s = fabs((double)s - sin(reduced));
    const double error_c = fabs((double)c - cos(reduced));
    const double error = error_s > error_c ? error_s : error_c;
    if (fabs((double)angle) <= 6400.0 && error > worst) {
        worst = error;
        worst_angle = angle;
    }
    return error <= limit;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    const double quarter = 0.78539816339744830962;

    /* Dense grid over the direct (no fmodf) range. */
    for (int index = -6400000; index <= 6400000; ++index) {
        TEST_ASSERT(check((float)(index * 6400.0 / 6400000.0)));
    }
    /* Every multiple of pi/4 up to the direct limit, with its neighbouring Float32 values. */
    for (int k = -8148; k <= 8148; ++k) {
        const float centre = (float)(k * quarter);
        TEST_ASSERT(check(centre));
        TEST_ASSERT(check(nextafterf(centre, INFINITY)));
        TEST_ASSERT(check(nextafterf(centre, -INFINITY)));
    }
    /* Direct limit and the fmodf() wrap range, including a continuous multi-turn angle. */
    TEST_ASSERT(check(6400.0f) && check(-6400.0f));
    TEST_ASSERT(check(nextafterf(6400.0f, INFINITY)) && check(nextafterf(-6400.0f, -INFINITY)));
    for (int index = 0; index <= 200000; ++index) {
        TEST_ASSERT(check(6400.0f + (float)index * 0.731f));
        TEST_ASSERT(check(-6400.0f - (float)index * 0.731f));
    }
    TEST_ASSERT(check(1.0e5f) && check(-3.0e6f) && check(6.2831855e6f) && check(FLT_MAX));
    TEST_ASSERT(check(0.0f) && check(-0.0f) && check(FLT_MIN) && check(1.0e-30f));

    {
        float s = 0.0f;
        float c = 0.0f;

        foc_sincosf(0.0f, &s, &c);
        TEST_ASSERT(s == 0.0f && c == 1.0f);
        foc_sincosf(NAN, &s, &c);
        TEST_ASSERT(isnan(s) && isnan(c));
        foc_sincosf(INFINITY, &s, &c);
        TEST_ASSERT(isnan(s) && isnan(c));
        foc_sincosf(-INFINITY, &s, &c);
        TEST_ASSERT(isnan(s) && isnan(c));
    }

    printf("test_foc_trig passed: worst %.3e at %.9g\n", worst, (double)worst_angle);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
