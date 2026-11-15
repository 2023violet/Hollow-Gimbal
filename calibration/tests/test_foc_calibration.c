/**
 * @file    test_foc_calibration.c
 * @brief   Deterministic Host tests for current-zero and rotor calibration.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Test role
 * ---------
 * Validate bounded current offset statistics, explicit preconditions, rotor
 * direction/pole-pair fitting, circular electrical offset estimation, failure
 * states, identity matching, and independent instances using synthetic data.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static int near(float actual, float expected, float tolerance)
{
    return fabsf(actual - expected) <= tolerance;
}

static float wrap_2pi(float angle)
{
    float wrapped = fmodf(angle, 6.28318530717958647692f);

    if (wrapped < 0.0f) {
        wrapped += 6.28318530717958647692f;
    }
    return wrapped >= 6.28318530717958647692f ? 0.0f : wrapped;
}

static struct foc_calibration_identity identity(void)
{
    return (struct foc_calibration_identity){
        .adc_full_scale_counts = 4095U,
        .current_a_per_count = 0.001f,
        .encoder_counts_per_revolution = 65536U,
        .encoder_direction = 0,
        .phase_order = 0U,
        .pole_pairs = 0U,
    };
}

static struct foc_calibration_adc_config adc_config(void)
{
    return (struct foc_calibration_adc_config){
        .identity = identity(),
        .required_valid_mask = 0x07U,
        .minimum_samples = 32U,
        .maximum_samples = 64U,
        .maximum_sample_step_counts = 20.0f,
        .maximum_noise_rms_counts = 2.0f,
        .minimum_offset_counts = 32.0f,
        .maximum_offset_counts = 4063.0f,
        .maximum_interval_us = 1000U,
    };
}

static struct foc_calibration_rotor_config rotor_config(void)
{
    return (struct foc_calibration_rotor_config){
        .identity = identity(),
        .minimum_pole_pairs = 1U,
        .maximum_pole_pairs = 20U,
        .minimum_mechanical_span_rad = 8.0f,
        .maximum_residual_rad = 0.02f,
        .minimum_candidate_separation_rad = 0.05f,
        .maximum_interval_us = 100000U,
        .maximum_samples = 64U,
    };
}

static struct foc_calibration_offset_config offset_config(int32_t direction, uint16_t poles)
{
    struct foc_calibration_offset_config config = {
        .identity = identity(),
        .maximum_residual_rad = 0.02f,
        .maximum_interval_us = 100000U,
        .maximum_samples = 64U,
    };
    config.identity.encoder_direction = direction;
    config.identity.pole_pairs = poles;
    return config;
}

static int collect_rotor_reference(
    struct foc_calibration_rotor_context *context,
    float offset,
    int32_t direction,
    uint16_t poles
)
{
    for (uint32_t index = 0U; index < 40U; ++index) {
        const float mechanical = -4.2f + (float)index * 0.73f;
        const float noise = (float)((int)(index % 3U) - 1) * 0.0005f;
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = mechanical,
            .electrical_rad =
                wrap_2pi((float)direction * (float)poles * mechanical + offset + noise),
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        if (foc_calibration_rotor_collect(context, &sample) != 0) {
            return 1;
        }
    }
    return 0;
}

static int test_current_zero(void)
{
    struct foc_calibration_adc_context context = {0};
    struct foc_calibration_adc_result result;
    struct foc_calibration_adc_config config = adc_config();
    const float expected[3] = {2043.0f, 2052.0f, 2048.0f};

    TEST_ASSERT(foc_calibration_adc_init(&context, &config) == 0);
    for (uint32_t index = 0U; index < 64U; ++index) {
        const int noise = (int)(index % 3U) - 1;
        const struct foc_calibration_adc_sample sample = {
            .phase_a = (uint16_t)(expected[0] + (float)noise),
            .phase_b = (uint16_t)(expected[1] - (float)noise),
            .phase_c = (uint16_t)(expected[2] + (float)(index % 2U)),
            .valid_mask = 0x07U,
            .sequence = index,
            .captured_us = index * 10U + 1U,
            .zero_current_confirmed = true,
        };
        TEST_ASSERT(foc_calibration_adc_collect(&context, &sample) == 0);
    }
    TEST_ASSERT(foc_calibration_adc_finalize(&context, &result) == 0);
    TEST_ASSERT(result.usable && result.state == FOC_CALIBRATION_STATE_VALID);
    TEST_ASSERT(near(result.offset_a_counts, expected[0], 0.1f));
    TEST_ASSERT(near(result.offset_b_counts, expected[1], 0.1f));
    TEST_ASSERT(near(result.offset_c_counts, expected[2] + 0.5f, 0.1f));
    TEST_ASSERT(result.noise_rms_a_counts < 1.0f && result.noise_rms_b_counts < 1.0f);
    printf(
        "adc offsets=(%.6g,%.6g,%.6g) noise_rms=(%.6g,%.6g,%.6g) samples=%u\n",
        result.offset_a_counts,
        result.offset_b_counts,
        result.offset_c_counts,
        result.noise_rms_a_counts,
        result.noise_rms_b_counts,
        result.noise_rms_c_counts,
        result.samples
    );

    struct foc_calibration_adc_context precondition = {0};
    TEST_ASSERT(foc_calibration_adc_init(&precondition, &config) == 0);
    struct foc_calibration_adc_sample invalid = {
        .phase_a = 2048U,
        .phase_b = 2048U,
        .phase_c = 2048U,
        .valid_mask = 0x07U,
        .sequence = 0U,
        .captured_us = 1U,
        .zero_current_confirmed = false,
    };
    TEST_ASSERT(foc_calibration_adc_collect(&precondition, &invalid) < 0);
    TEST_ASSERT(precondition.result.failure == FOC_CALIBRATION_FAILURE_PRECONDITION);
    return 0;
}

static int test_current_zero_rejection(void)
{
    struct foc_calibration_adc_config config = adc_config();
    struct foc_calibration_adc_context context = {0};
    struct foc_calibration_adc_result result;
    TEST_ASSERT(foc_calibration_adc_init(&context, &config) == 0);
    struct foc_calibration_adc_sample sample = {
        .phase_a = 2048U,
        .phase_b = 2048U,
        .phase_c = 2048U,
        .valid_mask = 0x07U,
        .zero_current_confirmed = true,
    };
    for (uint32_t index = 0U; index < 32U; ++index) {
        sample.sequence = index;
        sample.captured_us = index * 10U + 1U;
        sample.phase_b = index == 20U ? 2200U : 2048U;
        if (index < 20U) {
            TEST_ASSERT(foc_calibration_adc_collect(&context, &sample) == 0);
        } else {
            TEST_ASSERT(foc_calibration_adc_collect(&context, &sample) < 0);
        }
        if (index == 20U) {
            TEST_ASSERT(context.result.failure == FOC_CALIBRATION_FAILURE_INPUT_CHANGED);
            break;
        }
    }
    TEST_ASSERT(foc_calibration_adc_finalize(&context, &result) < 0);
    TEST_ASSERT(result.state == FOC_CALIBRATION_STATE_INVALID);

    TEST_ASSERT(foc_calibration_adc_reset(&context) == 0);
    sample.phase_b = 2048U;
    sample.sequence = 0U;
    sample.captured_us = 1U;
    sample.valid_mask = 0x03U;
    TEST_ASSERT(foc_calibration_adc_collect(&context, &sample) < 0);
    TEST_ASSERT(context.result.failure == FOC_CALIBRATION_FAILURE_INVALID_SAMPLE);

    TEST_ASSERT(foc_calibration_adc_reset(&context) == 0);
    sample.valid_mask = 0x07U;
    for (uint32_t index = 0U; index < 32U; ++index) {
        sample.sequence = index;
        sample.captured_us = index * 10U + 1U;
        sample.phase_a = (uint16_t)(2048U + index / 2U);
        sample.phase_b = 2048U;
        sample.phase_c = 2048U;
        TEST_ASSERT(foc_calibration_adc_collect(&context, &sample) == 0);
    }
    TEST_ASSERT(foc_calibration_adc_finalize(&context, &result) < 0);
    TEST_ASSERT(result.failure == FOC_CALIBRATION_FAILURE_NOISE);
    return 0;
}

static int test_rotor_identification(void)
{
    struct foc_calibration_rotor_context context = {0};
    struct foc_calibration_rotor_result result;
    const struct foc_calibration_rotor_config config = rotor_config();

    TEST_ASSERT(foc_calibration_rotor_init(&context, &config) == 0);
    TEST_ASSERT(collect_rotor_reference(&context, 1.2f, -1, 7U) == 0);
    TEST_ASSERT(foc_calibration_rotor_finalize(&context, &result) == 0);
    TEST_ASSERT(result.identifiable && result.state == FOC_CALIBRATION_STATE_VALID);
    TEST_ASSERT(result.direction == -1 && result.pole_pairs == 7U);
    TEST_ASSERT(near(result.electrical_offset_rad, 1.2f, 0.002f));
    TEST_ASSERT(result.residual_rms_rad < 0.01f);
    printf(
        "rotor direction=%d poles=%u offset=%.7g rms=%.7g max=%.7g separation=%.7g samples=%u\n",
        result.direction,
        result.pole_pairs,
        result.electrical_offset_rad,
        result.residual_rms_rad,
        result.residual_max_rad,
        result.candidate_separation_rad,
        result.samples
    );

    struct foc_calibration_rotor_context insufficient = {0};
    TEST_ASSERT(foc_calibration_rotor_init(&insufficient, &config) == 0);
    for (uint32_t index = 0U; index < 8U; ++index) {
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = (float)index * 0.1f,
            .electrical_rad = 0.3f,
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        TEST_ASSERT(foc_calibration_rotor_collect(&insufficient, &sample) == 0);
    }
    TEST_ASSERT(foc_calibration_rotor_finalize(&insufficient, &result) < 0);
    TEST_ASSERT(result.failure == FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);

    struct foc_calibration_rotor_context passive = {0};
    TEST_ASSERT(foc_calibration_rotor_init(&passive, &config) == 0);
    for (uint32_t index = 0U; index < 20U; ++index) {
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = -3.0f + (float)index * 0.41f,
            .electrical_rad = 0.0f,
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        TEST_ASSERT(foc_calibration_rotor_collect(&passive, &sample) == 0);
    }
    TEST_ASSERT(foc_calibration_rotor_finalize(&passive, &result) < 0);
    TEST_ASSERT(result.failure == FOC_CALIBRATION_FAILURE_UNIDENTIFIABLE);
    return 0;
}

static int test_electrical_offset_and_identity(void)
{
    struct foc_calibration_offset_context context = {0};
    struct foc_calibration_offset_result result;
    const struct foc_calibration_offset_config config = offset_config(-1, 7U);

    TEST_ASSERT(foc_calibration_offset_init(&context, &config) == 0);
    for (uint32_t index = 0U; index < 20U; ++index) {
        const float mechanical = -2.0f + (float)index * 0.37f;
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = mechanical,
            .electrical_rad =
                wrap_2pi(-7.0f * mechanical + 6.2f + (float)((int)(index % 3U) - 1) * 0.0005f),
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        TEST_ASSERT(foc_calibration_offset_collect(&context, &sample) == 0);
    }
    TEST_ASSERT(foc_calibration_offset_finalize(&context, &result) == 0);
    TEST_ASSERT(result.usable && near(result.electrical_offset_rad, 6.2f, 0.002f));
    TEST_ASSERT(result.residual_rms_rad < 0.01f);
    printf(
        "electrical_offset=%.7g rms=%.7g max=%.7g samples=%u\n",
        result.electrical_offset_rad,
        result.residual_rms_rad,
        result.residual_max_rad,
        result.samples
    );

    struct foc_calibration_identity expected = config.identity;
    struct foc_calibration_identity actual = expected;
    TEST_ASSERT(foc_calibration_identity_matches(&expected, &actual));
    actual.pole_pairs = 8U;
    TEST_ASSERT(!foc_calibration_identity_matches(&expected, &actual));

    struct foc_calibration_offset_context inconsistent = {0};
    TEST_ASSERT(foc_calibration_offset_init(&inconsistent, &config) == 0);
    for (uint32_t index = 0U; index < 5U; ++index) {
        const struct foc_calibration_angle_sample sample = {
            .mechanical_rad = (float)index * 0.5f,
            .electrical_rad = index == 4U ? 2.0f : 0.5f,
            .captured_us = (index + 1U) * 1000U,
            .sequence = index,
            .valid = true,
        };
        TEST_ASSERT(foc_calibration_offset_collect(&inconsistent, &sample) == 0);
    }
    TEST_ASSERT(foc_calibration_offset_finalize(&inconsistent, &result) < 0);
    TEST_ASSERT(result.failure == FOC_CALIBRATION_FAILURE_RESIDUAL);
    return 0;
}

static int test_instances_and_order(void)
{
    struct foc_calibration_rotor_context first = {0};
    struct foc_calibration_rotor_context second = {0};
    struct foc_calibration_rotor_config config = rotor_config();
    struct foc_calibration_angle_sample sample = {
        .mechanical_rad = 0.0f,
        .electrical_rad = 0.0f,
        .captured_us = 1000U,
        .sequence = 1U,
        .valid = true,
    };

    TEST_ASSERT(foc_calibration_rotor_init(&first, &config) == 0);
    TEST_ASSERT(foc_calibration_rotor_init(&second, &config) == 0);
    TEST_ASSERT(foc_calibration_rotor_collect(&first, &sample) == 0);
    sample.sequence = 1U;
    sample.captured_us = 1000U;
    TEST_ASSERT(foc_calibration_rotor_collect(&second, &sample) == 0);
    sample.sequence = 1U;
    sample.captured_us = 2000U;
    TEST_ASSERT(foc_calibration_rotor_collect(&first, &sample) < 0);
    TEST_ASSERT(first.result.failure == FOC_CALIBRATION_FAILURE_TIME_ORDER);
    TEST_ASSERT(second.sample_count == 1U);
    return 0;
}

static int test_configuration_admission(void)
{
    const struct foc_calibration_identity expected = {
        .adc_full_scale_counts = 4095U,
        .current_a_per_count = 0.001f,
        .encoder_counts_per_revolution = 65536U,
        .encoder_direction = -1,
        .phase_order = 0U,
        .pole_pairs = 7U,
    };
    const struct foc_calibration_adc_result adc = {
        .identity = expected,
        .offset_a_counts = 2042.98f,
        .offset_b_counts = 2052.02f,
        .offset_c_counts = 2048.50f,
        .noise_rms_a_counts = 0.82f,
        .noise_rms_b_counts = 0.83f,
        .noise_rms_c_counts = 0.50f,
        .samples = 64U,
        .state = FOC_CALIBRATION_STATE_VALID,
        .failure = FOC_CALIBRATION_FAILURE_NONE,
        .usable = true,
    };
    const struct foc_calibration_rotor_result rotor = {
        .identity = expected,
        .direction = -1,
        .pole_pairs = 7U,
        .electrical_offset_rad = 1.2f,
        .residual_rms_rad = 0.001f,
        .residual_max_rad = 0.002f,
        .candidate_separation_rad = 1.5f,
        .mechanical_span_rad = 12.0f,
        .samples = 40U,
        .state = FOC_CALIBRATION_STATE_VALID,
        .failure = FOC_CALIBRATION_FAILURE_NONE,
        .identifiable = true,
    };
    const struct foc_calibration_offset_result offset = {
        .identity = expected,
        .electrical_offset_rad = 1.2f,
        .residual_rms_rad = 0.001f,
        .residual_max_rad = 0.002f,
        .samples = 20U,
        .state = FOC_CALIBRATION_STATE_VALID,
        .failure = FOC_CALIBRATION_FAILURE_NONE,
        .usable = true,
    };
    const struct foc_calibration_admission_input input = {
        .adc = &adc,
        .rotor = &rotor,
        .offset = &offset,
        .expected_identity = &expected,
        .limits = {
            .minimum_adc_samples = 32U,
            .minimum_offset_counts = 32.0f,
            .maximum_offset_counts = 4063.0f,
            .maximum_noise_rms_counts = 2.0f,
            .maximum_rotor_residual_rad = 0.02f,
            .maximum_offset_residual_rad = 0.02f,
        },
    };
    struct foc_calibration_snapshot snapshot = {
        .phase_a_offset_count = -1.0f,
        .phase_b_offset_count = -1.0f,
        .phase_c_offset_count = -1.0f,
    };
    enum foc_calibration_admission_failure failure = FOC_CALIBRATION_ADMISSION_NONE;
    TEST_ASSERT(foc_calibration_admit(&input, &snapshot, &failure) == 0);
    TEST_ASSERT(failure == FOC_CALIBRATION_ADMISSION_NONE);
    TEST_ASSERT(snapshot.accepted && snapshot.runtime_encoder_direction == 1);
    TEST_ASSERT(snapshot.pole_pairs == 7U && near(snapshot.electrical_offset_rad, 1.2f, 1.0e-6f));
    TEST_ASSERT(foc_calibration_snapshot_valid(&snapshot));

    const struct foc_calibration_adc_result bad_adc = {
        .identity = expected,
        .offset_a_counts = 4090.0f,
        .offset_b_counts = adc.offset_b_counts,
        .offset_c_counts = adc.offset_c_counts,
        .noise_rms_a_counts = adc.noise_rms_a_counts,
        .noise_rms_b_counts = adc.noise_rms_b_counts,
        .noise_rms_c_counts = adc.noise_rms_c_counts,
        .samples = adc.samples,
        .state = FOC_CALIBRATION_STATE_VALID,
        .usable = true,
    };
    const struct foc_calibration_admission_input rejected = {
        .adc = &bad_adc,
        .rotor = &rotor,
        .offset = &offset,
        .expected_identity = &expected,
        .limits = input.limits,
    };
    const struct foc_calibration_snapshot previous = snapshot;
    TEST_ASSERT(
        foc_calibration_admit(&rejected, &snapshot, &failure) ==
        FOC_CALIBRATION_ADMISSION_ERR_CANDIDATE
    );
    TEST_ASSERT(failure == FOC_CALIBRATION_ADMISSION_ADC_RANGE);
    TEST_ASSERT(memcmp(&snapshot, &previous, sizeof(snapshot)) == 0);

    struct foc_calibration_identity wrong_identity = expected;
    wrong_identity.phase_order = 1U;
    const struct foc_calibration_admission_input mismatch = {
        .adc = &adc,
        .rotor = &rotor,
        .offset = &offset,
        .expected_identity = &wrong_identity,
        .limits = input.limits,
    };
    TEST_ASSERT(
        foc_calibration_admit(&mismatch, &snapshot, &failure) ==
        FOC_CALIBRATION_ADMISSION_ERR_CANDIDATE
    );
    TEST_ASSERT(failure == FOC_CALIBRATION_ADMISSION_ADC_IDENTITY);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_current_zero() || test_current_zero_rejection() || test_rotor_identification() ||
        test_electrical_offset_and_identity() || test_instances_and_order() ||
        test_configuration_admission();
}

/* ---------------------------------------- end of file ----------------------------------------- */
