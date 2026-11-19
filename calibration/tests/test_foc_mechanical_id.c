/**
 * @file    test_foc_mechanical_id.c
 * @brief   Independent J/B reference data and capture contract tests.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Evaluate analytical signed motion, independent estimation contexts, and
 * atomic rejection of bad endpoints or unidentifiable window data.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_mechanical_id.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            fprintf(stderr, "mechanical ID:%d: %s\n", __LINE__, #c); \
            return 1;                                                \
        }                                                            \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const struct foc_mechanical_id_config config = {
    .setup_id = 7U,
    .torque_model_id = 13U,
    .torque_source = FOC_MECHANICAL_ID_TORQUE_OBSERVED_DQ,
    .load_source = FOC_MECHANICAL_ID_LOAD_OBSERVED,
    .min_interval_us = 900U,
    .max_interval_us = 1100U,
    .intervals_per_window = 100U,
    .min_windows = 6U,
    .torque_constant_nm_per_a = 0.15f,
    .max_abs_id_a = 0.1f,
    .speed_resolution_rad_s = 0.001f,
    .max_abs_speed_rad_s = 20.0f,
    .max_abs_torque_nm = 2.0f,
    .min_accel_rms_rad_s2 = 0.1f,
    .min_speed_rms_rad_s = 0.1f,
    .min_excitation = 0.1f,
    .max_residual_rms_nm = 0.0001f,
    .min_inertia_kg_m2 = 0.0005f,
    .max_inertia_kg_m2 = 0.1f,
    .max_damping_nm_s = 0.1f,
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_mechanical_id_sample
endpoint(const struct foc_mechanical_id_context *context, uint32_t index, uint32_t timestamp)
{
    return (struct foc_mechanical_id_sample){
        .setup_id = config.setup_id,
        .torque_model_id = config.torque_model_id,
        .capture_id = context->result.capture_id,
        .torque_source = context->config.torque_source,
        .load_source = context->config.load_source,
        .sequence = UINT32_MAX - 1U + index,
        .speed_us = timestamp,
        .torque_us = timestamp,
        .interval_begin_us = index == 0U ? timestamp : context->result.last_us,
        .speed_valid = true,
        .torque_valid = true,
        .frame_confirmed = true,
        .interval_confirmed = true,
    };
}

static int analytic(
    struct foc_mechanical_id_context *context,
    double inertia,
    double damping,
    bool disturbance
)
{
    uint32_t elapsed = 0U;
    for (unsigned i = 0U; i <= 2400U; ++i) {
        if (i != 0U) {
            elapsed += i % 2U ? 940U : 1060U;
        }
        const double t = elapsed * 1.0e-6;
        const double p = 6.283185307179586 * t;
        const double speed = 2.0 * sin(p) + 0.5 * cos(0.5 * p);
        const double acceleration = 12.566370614359172 * cos(p) - 1.570796326794897 * sin(0.5 * p);
        struct foc_mechanical_id_sample sample = endpoint(context, i, UINT32_MAX - 500U + elapsed);
        const double load = 0.025 + 0.004 * cos(p);
        const double actual = inertia * acceleration + damping * speed + load +
            (disturbance ? 0.025 * sin(1.37 * p) : 0.0);
        sample.speed_rad_s = (float)speed;
        sample.load_nm = (float)load;
        sample.iq_a = (float)(actual / config.torque_constant_nm_per_a);
        sample.torque_nm = (float)actual;
        CHECK(foc_mechanical_id_push(context, &sample) == FOC_MECHANICAL_ID_OK);
    }
    return 0;
}

static int test_analytic_and_quality(void)
{
    struct foc_mechanical_id_context first, second;
    struct foc_mechanical_id_config other = config;
    other.torque_source = FOC_MECHANICAL_ID_TORQUE_EQUIVALENT;
    other.torque_constant_nm_per_a = 0.0f;
    CHECK(foc_mechanical_id_init(&first, &config, 21U) == 0);
    CHECK(foc_mechanical_id_init(&second, &other, 22U) == 0);
    CHECK(analytic(&first, 0.002, 0.006, false) == 0);
    CHECK(analytic(&second, 0.008, 0.021, false) == 0);
    struct foc_mechanical_id_result a, b;
    CHECK(foc_mechanical_id_finish(&first, &a) == 0 && a.usable);
    CHECK(foc_mechanical_id_finish(&second, &b) == 0 && b.usable);
    CHECK(b.torque_constant_nm_per_a == 0.0f);
    CHECK(
        a.capture_id == 21U && b.capture_id == 22U && a.torque_model_id == config.torque_model_id
    );
    CHECK(a.windows == 24U && a.samples == 2401U && a.accel_windows && a.decel_windows);
    CHECK(a.first_us > a.last_us && a.excitation > 0.9 && a.gram_condition < 1.5);
    CHECK(fabs(a.inertia_kg_m2 - 0.002) < 1e-6 && fabs(a.damping_nm_s - 0.006) < 1e-6);
    CHECK(fabs(b.inertia_kg_m2 - 0.008) < 1e-6 && fabs(b.damping_nm_s - 0.021) < 1e-6);
    CHECK(a.residual_rms_nm < 1e-6 && b.residual_rms_nm < 1e-6);
    CHECK(foc_mechanical_id_reset(&first, 21U) == FOC_MECHANICAL_ID_ERR_CONFIG);
    CHECK(foc_mechanical_id_reset(&first, 23U) == 0 && first.result.samples == 0U);
    CHECK(analytic(&first, 0.002, 0.006, true) == 0);
    CHECK(foc_mechanical_id_finish(&first, &a) == FOC_MECHANICAL_ID_ERR_QUALITY);
    CHECK(
        a.failure == FOC_CALIBRATION_FAILURE_RESIDUAL && !a.usable && a.inertia_kg_m2 == 0.0f &&
        a.damping_nm_s == 0.0f && a.residual_rms_nm > 0.01
    );
    CHECK(foc_mechanical_id_finish(&second, &b) == 0 && b.usable && b.capture_id == 22U);

    CHECK(foc_mechanical_id_reset(&first, 24U) == 0);
    CHECK(analytic(&first, 0.002, -0.006, false) == 0);
    CHECK(foc_mechanical_id_finish(&first, &a) == FOC_MECHANICAL_ID_ERR_QUALITY);
    CHECK(a.failure == FOC_CALIBRATION_FAILURE_RANGE && !a.usable);
    other.max_inertia_kg_m2 = 0.001f;
    CHECK(foc_mechanical_id_init(&second, &other, 25U) == 0);
    CHECK(analytic(&second, 0.002, 0.01, false) == 0);
    CHECK(foc_mechanical_id_finish(&second, &a) == FOC_MECHANICAL_ID_ERR_QUALITY);
    CHECK(a.failure == FOC_CALIBRATION_FAILURE_RANGE && !a.usable && a.damping_nm_s == 0.0f);
    return 0;
}

static int test_refusal_and_atomicity(void)
{
    for (unsigned i = 0U; i < 16U; ++i) {
        struct foc_mechanical_id_context context;
        CHECK(foc_mechanical_id_init(&context, &config, 43U) == 0);
        const struct foc_mechanical_id_sample first = endpoint(&context, 0U, 1000U);
        CHECK(foc_mechanical_id_push(&context, &first) == 0);
        struct foc_mechanical_id_sample bad = endpoint(&context, 1U, 2000U);
        enum foc_calibration_failure failure = FOC_CALIBRATION_FAILURE_TIME_ORDER;
        switch (i) {
        case 0:
            bad.sequence = first.sequence;
            break;
        case 1:
            bad.speed_us = bad.torque_us = 1000U;
            break;
        case 2:
            bad.speed_us = bad.torque_us = 999U;
            break;
        case 3:
            bad.speed_us = bad.torque_us = 2101U;
            break;
        case 4:
            bad.torque_us++;
            break;
        case 5:
            bad.interval_begin_us++;
            break;
        case 6:
            bad.torque_model_id++;
            failure = FOC_CALIBRATION_FAILURE_INPUT_CHANGED;
            break;
        case 7:
            bad.capture_id++;
            failure = FOC_CALIBRATION_FAILURE_INPUT_CHANGED;
            break;
        case 8:
            bad.torque_source = FOC_MECHANICAL_ID_TORQUE_EQUIVALENT;
            failure = FOC_CALIBRATION_FAILURE_INPUT_CHANGED;
            break;
        case 9:
            bad.load_source = FOC_MECHANICAL_ID_LOAD_UNKNOWN;
            failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 10:
            bad.torque_valid = false;
            failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 11:
            bad.speed_valid = false;
            failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 12:
            bad.interval_confirmed = false;
            failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 13:
            bad.id_a = 0.11f;
            failure = FOC_CALIBRATION_FAILURE_PRECONDITION;
            break;
        case 14:
            bad.iq_a = NAN;
            failure = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
            break;
        case 15:
            bad.speed_rad_s = INFINITY;
            failure = FOC_CALIBRATION_FAILURE_INVALID_SAMPLE;
            break;
        }
        CHECK(foc_mechanical_id_push(&context, &bad) < 0 && context.result.failure == failure);
        CHECK(
            context.result.samples == 1U && context.result.last_us == 1000U &&
            context.intervals == 0U && context.gram.xx == 0.0
        );
        const struct foc_mechanical_id_sample valid = endpoint(&context, 1U, 2000U);
        CHECK(foc_mechanical_id_push(&context, &valid) == FOC_MECHANICAL_ID_ERR_STATE);
        struct foc_mechanical_id_result result =
            {.usable = true, .inertia_kg_m2 = 1.0f, .damping_nm_s = 1.0f};
        CHECK(
            foc_mechanical_id_finish(&context, &result) == FOC_MECHANICAL_ID_ERR_STATE &&
            !result.usable && result.inertia_kg_m2 == 0.0f && result.damping_nm_s == 0.0f
        );
    }
    return 0;
}

static int test_excitation_and_overflow(void)
{
    struct foc_mechanical_id_config short_window = config;
    short_window.intervals_per_window = 2U;
    short_window.min_windows = 3U;
    /* Exact linear plant at J=.002, B=.01 and dt=.8 s can yield a
     * near-perfect residual with J biased by >100% on triangular torque. */
    struct foc_mechanical_id_config aliased = short_window;
    aliased.max_interval_us = 800000U;
    aliased.min_inertia_kg_m2 = 0.002f;
    aliased.max_damping_nm_s = 0.01f;
    struct foc_mechanical_id_context rejected = {0};
    CHECK(foc_mechanical_id_init(&rejected, &aliased, 99U) < 0);
    CHECK(!rejected.result.usable);
    for (unsigned variant = 0U; variant < 4U; ++variant) {
        struct foc_mechanical_id_context context;
        CHECK(foc_mechanical_id_init(&context, &short_window, 77U) == 0);
        for (unsigned i = 0U; i <= 8U; ++i) {
            struct foc_mechanical_id_sample sample = endpoint(&context, i, i * 1000U);
            if (variant == 1U) {
                sample.speed_rad_s = 1.0f; /* No acceleration. */
            } else if (variant == 2U) {
                /* Endpoints alternate -1 and +1. The midpoint makes mean
                 * speed proportional to acceleration in both directions. */
                static const float repeated[] = {-1, 1, 1, -1};
                sample.speed_rad_s = repeated[i % 4U];
            } else if (variant == 3U && i == 2U) {
                /* White-box numeric fault injection, not a legal capture. */
                context.speed_integral_rad = DBL_MAX;
                CHECK(foc_mechanical_id_push(&context, &sample) == FOC_MECHANICAL_ID_ERR_SAMPLE);
                CHECK(
                    context.result.failure == FOC_CALIBRATION_FAILURE_NUMERIC &&
                    context.result.windows == 0U && context.gram.yy == 0.0 &&
                    context.result.samples == 2U && context.speed_integral_rad == DBL_MAX
                );
                break;
            }
            CHECK(foc_mechanical_id_push(&context, &sample) == 0);
        }
        struct foc_mechanical_id_result result;
        CHECK(foc_mechanical_id_finish(&context, &result) < 0 && !result.usable);
        if (variant < 3U) {
            CHECK(result.failure == FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);
            if (variant == 2U) {
                CHECK(
                    result.accel_windows > 0U && result.decel_windows > 0U &&
                    result.excitation < 1e-8
                );
            }
        }
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_analytic_and_quality() || test_refusal_and_atomicity() ||
        test_excitation_and_overflow();
}

/* ---------------------------------------- end of file ----------------------------------------- */
