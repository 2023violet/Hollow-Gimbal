/**
 * @file    test_foc_bootstrap.c
 * @brief   Admission and measured electrical reference fixtures.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Test role
 * ---------
 * Check typed eligibility, signed measurements, integer unwrap, and refusal.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_bootstrap.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CPR          65536U
#define PER_SWEEP    48U
#define SAMPLE_COUNT (FOC_BOOTSTRAP_SWEEPS * PER_SWEEP)
#define TWO_PI       6.28318530717958647692

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static struct foc_bootstrap_config fixture_config(void)
{
    return (struct foc_bootstrap_config){
        .identity =
            {
                .acquisition =
                    {
                        .adc_full_scale_counts = 4095U,
                        .current_a_per_count = 0.001f,
                        .encoder_counts_per_revolution = CPR,
                    },
                .motor_setup_id = 1U,
                .encoder_id = 2U,
                .mechanical_frame_id = 3U,
            },
        .capture = 7U,
        .source = FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA,
        .minimum_pole_pairs = 1U,
        .maximum_pole_pairs = 14U,
        .minimum_samples_per_sweep = 8U,
        .maximum_interval_us = 1100U,
        .minimum_mechanical_span_rad = 0.35f,
        .maximum_residual_rad = 0.08f,
        .minimum_candidate_separation_rad = 0.08f,
        .maximum_offset_disagreement_rad = 0.06f,
        .minimum_emf_v = 0.1f,
        .minimum_abs_speed_rad_s = 0.2f,
        .maximum_abs_speed_rad_s = 1000.0f,
        .maximum_motion_error_rad = 0.002f,
    };
}

static void fixture_samples(
    struct foc_bootstrap_sample *samples,
    const struct foc_bootstrap_config *config,
    uint16_t pole_pairs,
    int32_t encoder_direction,
    int32_t step_code,
    float error_rad
)
{
    for (uint32_t sweep = 0U; sweep < FOC_BOOTSTRAP_SWEEPS; ++sweep) {
        const int32_t shaft_sign = sweep % 2U == 0U ? 1 : -1;
        for (uint32_t index = 0U; index < PER_SWEEP; ++index) {
            const uint32_t sample = sweep * PER_SWEEP + index;
            const int32_t distance =
                shaft_sign > 0 ? (int32_t)index : (int32_t)(PER_SWEEP - 1U - index);
            const int64_t cumulative =
                INT64_C(400000000) * CPR + 127 + (int64_t)encoder_direction * distance * step_code;
            const uint32_t raw = (uint32_t)(cumulative % CPR);
            const double magnetic_axis_rad =
                encoder_direction * (double)pole_pairs * raw * (TWO_PI / CPR) + 6.25 +
                error_rad * sin(0.7 * sample);
            samples[sample] = (struct foc_bootstrap_sample){
                .identity = config->identity,
                .capture = config->capture,
                .captured_us = UINT32_MAX - UINT32_C(23000) + sample * UINT32_C(1000),
                .sequence = UINT32_MAX - 15U + sample,
                .sweep = sweep,
                .raw_code = raw,
                .shaft_rad_s = (float)(shaft_sign * step_code * (1000.0 * TWO_PI / CPR)),
                .emf_alpha_v = (float)(-shaft_sign * sin(magnetic_axis_rad)),
                .emf_beta_v = (float)(shaft_sign * cos(magnetic_axis_rad)),
                .valid = true,
                .output_inhibited = true,
            };
        }
    }
}

static int test_qualified_known_reference(void)
{
    const struct foc_bootstrap_config config = fixture_config();
    struct foc_bootstrap_prerequisites prerequisites = {
        .current_mapping_and_zero_verified = true,
        .phase_and_direction_verified = true,
        .encoder_and_clock_verified = true,
        .bus_and_limits_verified = true,
        .acquisition_verified = true,
        .safety_permitted = true,
    };
    struct foc_bootstrap_trusted_reference trusted = {
        .identity = config.identity,
        .pole_pairs = 6U,
        .encoder_direction = -1,
        .electrical_offset_rad = 4.2f,
        .reviewed = true,
    };
    TEST_ASSERT(
        foc_bootstrap_qualify(&config.identity, &config.identity, &prerequisites, &trusted) ==
        FOC_BOOTSTRAP_READY_WITH_TRUSTED_REFERENCE
    );
    TEST_ASSERT(
        foc_bootstrap_qualify(&config.identity, &config.identity, &prerequisites, NULL) ==
        FOC_BOOTSTRAP_REFERENCE_ACQUISITION_REQUIRED
    );
    trusted.reviewed = false;
    TEST_ASSERT(
        foc_bootstrap_qualify(&config.identity, &config.identity, &prerequisites, &trusted) ==
        FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED
    );
    trusted.identity.mechanical_frame_id++;
    TEST_ASSERT(
        foc_bootstrap_qualify(&config.identity, &config.identity, &prerequisites, &trusted) ==
        FOC_BOOTSTRAP_IDENTITY_MISMATCH
    );
    prerequisites.encoder_and_clock_verified = false;
    TEST_ASSERT(
        foc_bootstrap_qualify(&config.identity, &config.identity, &prerequisites, NULL) ==
        FOC_BOOTSTRAP_MEASUREMENT_INVALID
    );
    prerequisites.safety_permitted = false;
    TEST_ASSERT(
        foc_bootstrap_qualify(&config.identity, &config.identity, &prerequisites, NULL) ==
        FOC_BOOTSTRAP_SAFETY_BLOCKED
    );
    return 0;
}

static int test_fits_both_encoder_directions_at_high_poles(void)
{
    struct foc_bootstrap_sample samples[SAMPLE_COUNT];
    struct foc_bootstrap_config config = fixture_config();
    struct foc_bootstrap_result result;
    for (int32_t direction = -1; direction <= 1; direction += 2) {
        fixture_samples(samples, &config, 14U, direction, 1340, 0.035f);
        TEST_ASSERT(
            foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
            FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED
        );
        TEST_ASSERT(result.quality == FOC_BOOTSTRAP_COARSE_ELECTRICAL_REFERENCE);
        TEST_ASSERT(result.pole_pairs == 14U && result.encoder_direction == direction);
        TEST_ASSERT(
            result.capture == 7U && result.source == FOC_CAL_WORKFLOW_EXTERNAL_RECORDED_DATA
        );
        TEST_ASSERT(fabsf(result.electrical_offset_rad - 6.25f) < 0.01f);
        TEST_ASSERT(result.minimum_mechanical_span_rad > 6.0f);
        TEST_ASSERT(result.maximum_residual_rad < 0.04f);
    }
    config.expected_pole_pairs = 13U;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE
    );
    TEST_ASSERT(result.quality == FOC_BOOTSTRAP_NO_REFERENCE && result.pole_pairs == 0U);
    config.expected_pole_pairs = 14U;
    config.expected_encoder_direction = 1;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_REVIEW_REQUIRED
    );
    config.expected_encoder_direction = -1;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE
    );
    return 0;
}

static int test_rejects_low_excitation_noise_and_aliasing(void)
{
    struct foc_bootstrap_sample samples[SAMPLE_COUNT];
    struct foc_bootstrap_config config = fixture_config();
    struct foc_bootstrap_result result;
    fixture_samples(samples, &config, 4U, 1, 20, 0.0f);
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION
    );
    config.minimum_mechanical_span_rad = 0.01f;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE
    );
    fixture_samples(samples, &config, 4U, 1, 1340, 0.35f);
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE
    );
    fixture_samples(samples, &config, 4U, 1, 3000, 0.0f);
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE
    );
    fixture_samples(samples, &config, 4U, 1, 1340, 0.0f);
    config.maximum_abs_speed_rad_s = 4000.0f;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE
    );
    return 0;
}

static int test_rejects_inconsistent_apertures_and_sweeps(void)
{
    struct foc_bootstrap_sample samples[SAMPLE_COUNT];
    const struct foc_bootstrap_config config = fixture_config();
    struct foc_bootstrap_result result;
    fixture_samples(samples, &config, 4U, 1, 1340, 0.0f);
    samples[5].identity.encoder_id++;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_IDENTITY_MISMATCH
    );
    samples[5].identity = config.identity;
    samples[5].output_inhibited = false;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) == FOC_BOOTSTRAP_SAFETY_BLOCKED
    );
    samples[5].output_inhibited = true;
    samples[5].valid = false;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_MEASUREMENT_INVALID
    );
    samples[5].valid = true;
    const uint32_t sequence = samples[5].sequence;
    samples[5].sequence = samples[4].sequence;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_MEASUREMENT_INVALID
    );
    samples[5].sequence = sequence;
    samples[5].raw_code += 600U;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_MEASUREMENT_INVALID
    );
    samples[5].raw_code -= 600U;
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT - PER_SWEEP, &result) ==
        FOC_BOOTSTRAP_INSUFFICIENT_EXCITATION
    );
    for (uint32_t index = 3U * PER_SWEEP; index < SAMPLE_COUNT; ++index) {
        samples[index].emf_alpha_v = -samples[index].emf_alpha_v;
        samples[index].emf_beta_v = -samples[index].emf_beta_v;
    }
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE
    );
    fixture_samples(samples, &config, 4U, 1, 1340, 0.0f);
    for (uint32_t index = 0U; index < SAMPLE_COUNT; ++index) {
        samples[index].emf_beta_v = -samples[index].emf_beta_v;
    }
    TEST_ASSERT(
        foc_bootstrap_fit(&config, samples, SAMPLE_COUNT, &result) ==
        FOC_BOOTSTRAP_REFERENCE_UNIDENTIFIABLE
    );
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    return test_qualified_known_reference() || test_fits_both_encoder_directions_at_high_poles() ||
        test_rejects_low_excitation_noise_and_aliasing() ||
        test_rejects_inconsistent_apertures_and_sweeps();
}

/* ---------------------------------------- end of file ----------------------------------------- */
