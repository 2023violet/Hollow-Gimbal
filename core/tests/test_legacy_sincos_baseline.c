/**
 * @file    test_legacy_sincos_baseline.c
 * @brief   Legacy fast sine/cosine error and duty propagation baseline.
 * @version v0.1.0
 * @date    2026.11.12
 *
 * Test role
 * ---------
 * Scan the current Legacy approximation against host libm without replacing
 * the V2 implementation. The result is a numerical reference for PERF-01B.
 * The scan is host-only and makes no claim about ARM execution time.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_core.h"
#include "foc_math.h"
#include <math.h>
#include <stdio.h>

/* Private Constants ----------------------------------------------------------- */
static const float BASELINE_PI = 3.14159265358979323846f;
static const float BASELINE_TWO_PI = 6.28318530717958647692f;

/* Private Helpers -------------------------------------------------------------- */

/* Public Functions ------------------------------------------------------------- */
int main(void)
{
    const unsigned int samples = 1048576U;
    const float boundaries[] = {
        0.0f,
        nextafterf(0.0f, 1.0f),
        nextafterf(BASELINE_PI * 0.5f, 0.0f),
        BASELINE_PI * 0.5f,
        nextafterf(BASELINE_PI * 0.5f, BASELINE_PI),
        nextafterf(BASELINE_PI, 0.0f),
        BASELINE_PI,
        nextafterf(BASELINE_PI, BASELINE_TWO_PI),
        nextafterf(BASELINE_PI * 1.5f, 0.0f),
        BASELINE_PI * 1.5f,
        nextafterf(BASELINE_PI * 1.5f, BASELINE_TWO_PI),
        nextafterf(BASELINE_TWO_PI, 0.0f),
    };
    double sum_sq = 0.0;
    float max_abs = 0.0f;
    float max_norm = 0.0f;
    float max_park = 0.0f;
    float max_inverse = 0.0f;
    float max_duty = 0.0f;
    unsigned int sector_mismatches = 0U;

    for (unsigned int index = 0U; index < samples; ++index) {
        const float angle = BASELINE_TWO_PI * (float)index / (float)samples;
        float legacy_sine;
        float legacy_cosine;
        const float sine = sinf(angle);
        const float cosine = cosf(angle);
        const float vector_alpha = 0.8f;
        const float vector_beta = -0.2f;
        struct foc_config config = {1.0f, 2.5f, 0.28f, 0.72f, 0.001f};
        struct foc_svpwm_output standard_duty;
        struct foc_svpwm_output legacy_duty;
        const struct foc_vector_alpha_beta standard_vector = {1.2f * cosine, 1.2f * sine};
        struct foc_vector_alpha_beta legacy_vector;

        foc_fast_sincos(angle, &legacy_sine, &legacy_cosine);
        legacy_vector.alpha = 1.2f * legacy_cosine;
        legacy_vector.beta = 1.2f * legacy_sine;
        const float sine_error = fabsf(legacy_sine - sine);
        const float cosine_error = fabsf(legacy_cosine - cosine);
        const float error = fmaxf(sine_error, cosine_error);
        max_abs = fmaxf(max_abs, error);
        sum_sq += (double)sine_error * sine_error + (double)cosine_error * cosine_error;
        max_norm = fmaxf(
            max_norm,
            fabsf(legacy_sine * legacy_sine + legacy_cosine * legacy_cosine - 1.0f)
        );

        max_park = fmaxf(
            max_park,
            fabsf(
                (vector_alpha * legacy_cosine + vector_beta * legacy_sine) -
                (vector_alpha * cosine + vector_beta * sine)
            )
        );
        max_park = fmaxf(
            max_park,
            fabsf(
                (vector_beta * legacy_cosine - vector_alpha * legacy_sine) -
                (vector_beta * cosine - vector_alpha * sine)
            )
        );
        max_inverse = fmaxf(
            max_inverse,
            fabsf((0.7f * legacy_cosine + 0.3f * legacy_sine) - (0.7f * cosine + 0.3f * sine))
        );
        max_inverse = fmaxf(
            max_inverse,
            fabsf((0.7f * legacy_sine - 0.3f * legacy_cosine) - (0.7f * sine - 0.3f * cosine))
        );

        if (foc_svpwm(&standard_vector, 12.0f, &config, &standard_duty) != FOC_STATUS_OK ||
            foc_svpwm(&legacy_vector, 12.0f, &config, &legacy_duty) != FOC_STATUS_OK) {
            return 1;
        }
        max_duty = fmaxf(max_duty, fabsf(standard_duty.duty_a - legacy_duty.duty_a));
        max_duty = fmaxf(max_duty, fabsf(standard_duty.duty_b - legacy_duty.duty_b));
        max_duty = fmaxf(max_duty, fabsf(standard_duty.duty_c - legacy_duty.duty_c));
        sector_mismatches += standard_duty.sector != legacy_duty.sector;
    }

    for (unsigned int index = 0U; index < sizeof(boundaries) / sizeof(boundaries[0]); ++index) {
        float sine;
        float cosine;
        foc_fast_sincos(boundaries[index], &sine, &cosine);
        printf(
            "boundary,%.9g,%.9g,%.9g,%.9g\n",
            boundaries[index],
            sine,
            cosine,
            fabsf(sine * sine + cosine * cosine - 1.0f)
        );
    }
    printf(
        "scan,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u\n",
        samples,
        BASELINE_TWO_PI / (float)samples,
        max_abs,
        sqrt(sum_sq / (2.0 * (double)samples)),
        max_norm,
        max_park,
        max_inverse,
        max_duty,
        sector_mismatches
    );
    printf(
        "wrap,-2pi=%.9g,0=%.9g,2pi=%.9g,4pi+0.5=%.9g\n",
        foc_wrap_0_2pi(-BASELINE_TWO_PI),
        foc_wrap_0_2pi(0.0f),
        foc_wrap_0_2pi(BASELINE_TWO_PI),
        foc_wrap_0_2pi(2.0f * BASELINE_TWO_PI + 0.5f)
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
