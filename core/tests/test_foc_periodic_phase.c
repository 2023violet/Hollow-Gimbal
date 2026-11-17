/**
 * @file    test_foc_periodic_phase.c
 * @brief   Independent integer/double periodic phase and precision contracts.
 * @version v0.1.0
 * @date    2026.11.16
 *
 * Test role
 * ---------
 * Compare production phase mapping with an independent mathematical phasor.
 * Counts stay integer until the reference mechanical cycle is bounded.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_motion.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            fprintf(stderr, "phase assertion: %s at %d\n", #c, __LINE__); \
            return 1;                                                     \
        }                                                                 \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static uint32_t cycle_count(int64_t counts, uint32_t cpr)
{
    const int64_t remainder = counts % (int64_t)cpr;
    return (uint32_t)(remainder < 0 ? remainder + (int64_t)cpr : remainder);
}

static double periodic_distance(double actual, double expected)
{
    return fabs(atan2(sin(actual - expected), cos(actual - expected)));
}

static int test_oracle(void)
{
    const double tau = 6.283185307179586476925286766559005768;
    const uint32_t cprs[] = {2, 3, 7, 1000, 16384, 65535, 65536};
    const uint16_t poles[] = {1, 4, 7, 257, 65535};
    const int64_t counts[] = {
        INT64_MIN,
        INT64_MIN + 1,
        -INT64_C(17179803771),
        -INT64_C(1073676411),
        -1,
        0,
        1,
        123,
        INT64_C(17179869307),
        INT64_MAX
    };
    const float offsets[] = {
        0,
        .37f,
        -.71f,
        3.14159265358979323846f,
        6.28318530717958647692f,
        -6.28318530717958647692f
    };
    double maximum = 0;
    unsigned samples = 0;
    int64_t worst_count = 0;
    uint32_t worst_cpr = 0;
    uint16_t worst_poles = 0;
    float worst_offset = 0;
    for (unsigned c = 0; c < sizeof cprs / sizeof *cprs; ++c) {
        for (unsigned p = 0; p < sizeof poles / sizeof *poles; ++p) {
            for (unsigned n = 0; n < sizeof counts / sizeof *counts; ++n) {
                for (unsigned o = 0; o < sizeof offsets / sizeof *offsets; ++o) {
                    const struct foc_rotor_config rotor = {poles[p], offsets[o]};
                    const uint32_t cycle = cycle_count(counts[n], cprs[c]);
                    /* No production normalize or integer electrical modulo in the Oracle. */
                    const double expected =
                        (double)cycle / (double)cprs[c] * tau * poles[p] + (double)offsets[o];
                    float actual = -99;
                    CHECK(foc_rotor_phase_from_counts(&rotor, cycle, cprs[c], &actual) == 0);
                    const double error = periodic_distance(actual, expected);
                    if (error > maximum) {
                        maximum = error;
                        worst_count = counts[n];
                        worst_cpr = cprs[c];
                        worst_poles = poles[p];
                        worst_offset = offsets[o];
                    }
                    ++samples;
                    CHECK(isfinite(actual) && actual >= 0 && actual < 6.2831854820251465f);
                    CHECK(error <= 1e-5);
                }
            }
        }
    }
    printf("max_abs_periodic_phase_error_rad=%.12g sample_count=%u\n", maximum, samples);
    printf(
        "max_error_case=counts:%lld,CPR:%u,poles:%u,offset:%.9g\n",
        (long long)worst_count,
        worst_cpr,
        worst_poles,
        (double)worst_offset
    );
    puts("oracle_source=int64 mathematical modulo + independent double sin/cos phasor");
    return 0;
}

static int test_invalid(void)
{
    struct foc_rotor_config rotor = {7, .37f};
    float out = 99;
    CHECK(foc_rotor_phase_from_counts(&rotor, 0, 65536, NULL) == FOC_STATUS_ERR_NULL);
    CHECK(foc_rotor_phase_from_counts(NULL, 0, 65536, &out) == FOC_STATUS_ERR_NULL && out == 0);
    const uint32_t bad_cpr[] = {0, 1, 65537, UINT32_MAX};
    for (unsigned i = 0; i < sizeof bad_cpr / sizeof *bad_cpr; ++i) {
        out = 99;
        CHECK(
            foc_rotor_phase_from_counts(&rotor, 0, bad_cpr[i], &out) ==
                FOC_STATUS_ERR_INVALID_CONFIG &&
            out == 0
        );
    }
    CHECK(
        foc_rotor_phase_from_counts(&rotor, 65536, 65536, &out) == FOC_STATUS_ERR_INVALID_CONFIG &&
        out == 0
    );
    rotor.pole_pairs = 0;
    CHECK(
        foc_rotor_phase_from_counts(&rotor, 1, 65536, &out) == FOC_STATUS_ERR_INVALID_CONFIG &&
        out == 0
    );
    rotor.pole_pairs = 7;
    const float bad_offset[] = {NAN, INFINITY, -INFINITY};
    for (unsigned i = 0; i < sizeof bad_offset / sizeof *bad_offset; ++i) {
        rotor.electrical_offset_rad = bad_offset[i];
        out = 99;
        CHECK(
            foc_rotor_phase_from_counts(&rotor, 1, 65536, &out) == FOC_STATUS_ERR_NONFINITE &&
            out == 0
        );
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_oracle() || test_invalid();
}

/* ---------------------------------------- end of file ----------------------------------------- */
