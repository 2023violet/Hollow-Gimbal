/**
 * @file    test_foc_encoder.c
 * @brief   Host contract, differential, and deterministic dataset tests.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Test role
 * ---------
 * Exercise raw-code unwrapping, bounded timestamp arithmetic, failure latching,
 * variable-dt LPF behavior, alpha-beta tracking, and independent instances.
 * The synthetic sequences are deterministic and do not represent board or
 * motor performance measurements.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_encoder.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

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

static struct foc_encoder_config test_config(void)
{
    return (struct foc_encoder_config){
        .counts_per_revolution = 65536U,
        .direction = FOC_ENCODER_DIRECTION_NORMAL,
        .max_speed_rad_s = 100.0f,
        .min_dt_us = 100U,
        .max_dt_us = 20000U,
        .speed_lpf_cutoff_hz = 80.0f,
        .tracker_alpha = 0.85f,
        .tracker_beta = 0.2f,
    };
}

static int test_lifecycle_and_single_turn(void)
{
    struct foc_encoder_context context = {0};
    struct foc_encoder_feedback feedback;
    struct foc_encoder_config config = test_config();

    TEST_ASSERT(foc_encoder_init(NULL, &config) == FOC_ENCODER_STATUS_ERR_NULL);
    TEST_ASSERT(foc_encoder_init(&context, NULL) == FOC_ENCODER_STATUS_ERR_NULL);
    config.tracker_alpha = 0.0f;
    TEST_ASSERT(foc_encoder_init(&context, &config) == FOC_ENCODER_STATUS_ERR_CONFIG);
    config = test_config();
    config.max_speed_rad_s = NAN;
    TEST_ASSERT(foc_encoder_init(&context, &config) == FOC_ENCODER_STATUS_ERR_CONFIG);
    config = test_config();
    TEST_ASSERT(
        foc_encoder_update(&context, 0U, 0U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_ERR_NOT_INITIALIZED
    );
    TEST_ASSERT(foc_encoder_init(&context, &config) == FOC_ENCODER_STATUS_OK);
    TEST_ASSERT(foc_encoder_reset(&context) == FOC_ENCODER_STATUS_OK);
    TEST_ASSERT(
        foc_encoder_update(&context, 100U, 1234U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_OK
    );
    TEST_ASSERT(feedback.quality == FOC_ENCODER_QUALITY_UNINITIALIZED);
    TEST_ASSERT(!feedback.valid && !feedback.speed_valid);
    TEST_ASSERT(
        near(feedback.single_turn_rad, 1234.0f * 6.28318530717958647692f / 65536.0f, 1.0e-6f)
    );
    return 0;
}

static int test_wrap_direction_and_multiturn(void)
{
    struct foc_encoder_context normal = {0};
    struct foc_encoder_context reverse = {0};
    struct foc_encoder_feedback a;
    struct foc_encoder_feedback b;
    struct foc_encoder_config config = test_config();
    const float count_rad = 6.28318530717958647692f / 65536.0f;

    config.direction = FOC_ENCODER_DIRECTION_NORMAL;
    TEST_ASSERT(foc_encoder_init(&normal, &config) == FOC_ENCODER_STATUS_OK);
    config.direction = FOC_ENCODER_DIRECTION_REVERSED;
    TEST_ASSERT(foc_encoder_init(&reverse, &config) == FOC_ENCODER_STATUS_OK);
    TEST_ASSERT(foc_encoder_update(&normal, 0U, 65535U, FOC_ENCODER_TRANSPORT_OK, &a) == 0);
    TEST_ASSERT(foc_encoder_update(&reverse, 0U, 65535U, FOC_ENCODER_TRANSPORT_OK, &a) == 0);
    TEST_ASSERT(foc_encoder_update(&normal, 1000U, 0U, FOC_ENCODER_TRANSPORT_OK, &a) == 0);
    TEST_ASSERT(foc_encoder_update(&reverse, 1000U, 0U, FOC_ENCODER_TRANSPORT_OK, &b) == 0);
    TEST_ASSERT(a.quality == FOC_ENCODER_QUALITY_VALID && b.quality == FOC_ENCODER_QUALITY_VALID);
    TEST_ASSERT(near(a.delta_rad, count_rad, 1.0e-8f));
    TEST_ASSERT(near(b.delta_rad, -count_rad, 1.0e-8f));
    TEST_ASSERT(near(a.mechanical_rad, 2.0f * 3.14159265358979323846f, 1.0e-5f));
    TEST_ASSERT(near(b.mechanical_rad, 0.0f, 1.0e-7f));

    for (uint32_t index = 2U; index < 1002U; ++index) {
        TEST_ASSERT(
            foc_encoder_update(
                &normal,
                index * 1000U,
                index & 0xffffU,
                FOC_ENCODER_TRANSPORT_OK,
                &a
            ) == 0
        );
    }
    TEST_ASSERT(normal.position_counts == 66537);
    TEST_ASSERT(a.valid && a.speed_valid);
    return 0;
}

static int test_timestamps_and_continuity(void)
{
    struct foc_encoder_context context = {0};
    struct foc_encoder_feedback feedback;
    struct foc_encoder_config config = test_config();
    const uint32_t first = UINT32_MAX - 400U;

    config.max_dt_us = 5000U;
    config.max_speed_rad_s = 1000.0f;
    TEST_ASSERT(foc_encoder_init(&context, &config) == 0);
    TEST_ASSERT(
        foc_encoder_update(&context, first, 100U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0
    );
    TEST_ASSERT(foc_encoder_update(&context, 600U, 101U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    TEST_ASSERT(
        feedback.quality == FOC_ENCODER_QUALITY_VALID && near(feedback.dt_s, 0.001001f, 1.0e-7f)
    );
    const float position = feedback.mechanical_rad;
    TEST_ASSERT(
        foc_encoder_update(&context, 600U, 102U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_ERR_ORDER
    );
    TEST_ASSERT(
        feedback.quality == FOC_ENCODER_QUALITY_DUPLICATE && feedback.mechanical_rad == position
    );
    TEST_ASSERT(
        foc_encoder_update(&context, 500U, 102U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_ERR_ORDER
    );
    TEST_ASSERT(feedback.quality == FOC_ENCODER_QUALITY_TIME_ORDER_ERROR);
    TEST_ASSERT(context.continuity_lost);
    TEST_ASSERT(
        foc_encoder_update(&context, 1600U, 103U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_ERR_DISCONTINUITY
    );
    TEST_ASSERT(feedback.quality == FOC_ENCODER_QUALITY_DISCONTINUITY);
    TEST_ASSERT(foc_encoder_reset(&context) == 0);
    TEST_ASSERT(foc_encoder_update(&context, 0U, 0U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    TEST_ASSERT(
        foc_encoder_update(&context, 6000U, 1U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_ERR_DISCONTINUITY
    );
    TEST_ASSERT(feedback.quality == FOC_ENCODER_QUALITY_DISCONTINUITY && context.continuity_lost);
    return 0;
}

static int test_transport_and_invalid_samples(void)
{
    struct foc_encoder_context context = {0};
    struct foc_encoder_feedback feedback;
    const struct foc_encoder_config config = test_config();

    TEST_ASSERT(foc_encoder_init(&context, &config) == 0);
    TEST_ASSERT(
        foc_encoder_update(&context, 0U, 10U, FOC_ENCODER_TRANSPORT_TIMEOUT, &feedback) ==
        FOC_ENCODER_STATUS_ERR_TRANSPORT
    );
    TEST_ASSERT(feedback.quality == FOC_ENCODER_QUALITY_TIMEOUT && !context.previous_seen);
    TEST_ASSERT(
        foc_encoder_update(&context, 10U, 10U, FOC_ENCODER_TRANSPORT_FRAME_ERROR, &feedback) ==
        FOC_ENCODER_STATUS_ERR_TRANSPORT
    );
    TEST_ASSERT(feedback.quality == FOC_ENCODER_QUALITY_TRANSPORT_ERROR);
    TEST_ASSERT(
        foc_encoder_update(&context, 20U, 65536U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_ERR_SAMPLE
    );
    TEST_ASSERT(feedback.quality == FOC_ENCODER_QUALITY_INVALID_SAMPLE && !context.previous_seen);
    TEST_ASSERT(foc_encoder_update(&context, 100U, 10U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    TEST_ASSERT(
        foc_encoder_update(&context, 150U, 11U, FOC_ENCODER_TRANSPORT_OK, &feedback) ==
        FOC_ENCODER_STATUS_ERR_ORDER
    );
    TEST_ASSERT(feedback.quality == FOC_ENCODER_QUALITY_INVALID_INTERVAL);
    return 0;
}

static int test_speed_filters_and_deterministic_dataset(void)
{
    struct foc_encoder_context context = {0};
    struct foc_encoder_feedback feedback;
    struct foc_encoder_config config = test_config();
    const float count_rad = 6.28318530717958647692f / 65536.0f;
    const float commanded_speed = 4.0f;
    const uint32_t step_counts = (uint32_t)lroundf(commanded_speed * 0.0005f / count_rad);
    float raw_error_sum = 0.0f;
    float lpf_error_sum = 0.0f;
    float tracker_error_sum = 0.0f;
    unsigned int samples = 0U;

    config.max_dt_us = 5000U;
    TEST_ASSERT(foc_encoder_init(&context, &config) == 0);
    TEST_ASSERT(foc_encoder_update(&context, 0U, 1000U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    for (unsigned int index = 1U; index <= 2000U; ++index) {
        const uint32_t code = (1000U + index * step_counts) & UINT32_C(0xffff);
        const uint32_t time = index * 500U;
        TEST_ASSERT(
            foc_encoder_update(&context, time, code, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0
        );
        if (feedback.valid) {
            raw_error_sum += (feedback.raw_velocity_rad_s - commanded_speed) *
                (feedback.raw_velocity_rad_s - commanded_speed);
            lpf_error_sum += (feedback.filtered_velocity_rad_s - commanded_speed) *
                (feedback.filtered_velocity_rad_s - commanded_speed);
            tracker_error_sum += (feedback.tracked_velocity_rad_s - commanded_speed) *
                (feedback.tracked_velocity_rad_s - commanded_speed);
            ++samples;
        }
    }
    TEST_ASSERT(samples == 2000U);
    TEST_ASSERT(sqrtf(raw_error_sum / (float)samples) < 0.1f);
    TEST_ASSERT(sqrtf(lpf_error_sum / (float)samples) < 0.1f);
    TEST_ASSERT(sqrtf(tracker_error_sum / (float)samples) < 0.2f);
    TEST_ASSERT(near(feedback.dt_s, 0.0005f, 1.0e-7f));

    printf(
        "encoder dataset samples=%u raw_rms=%.7g lpf_rms=%.7g tracker_rms=%.7g "
        "context_bytes=%zu feedback_bytes=%zu\n",
        samples,
        sqrtf(raw_error_sum / (float)samples),
        sqrtf(lpf_error_sum / (float)samples),
        sqrtf(tracker_error_sum / (float)samples),
        sizeof(struct foc_encoder_context),
        sizeof(struct foc_encoder_feedback)
    );
    return 0;
}

static int test_algorithm_comparison_metrics(void)
{
    const float count_rad = 6.28318530717958647692f / 65536.0f;
    const float dt_s = 0.0005f;
    const uint32_t dt_us = 500U;
    const struct foc_encoder_config config = test_config();
    struct foc_encoder_context context = {0};
    struct foc_encoder_feedback feedback;
    float static_raw = 0.0f;
    float static_lpf = 0.0f;
    float static_tracker = 0.0f;
    float acceleration_lpf = 0.0f;
    float acceleration_tracker = 0.0f;
    float low_speed_raw_error = 0.0f;
    float low_speed_lpf_error = 0.0f;
    float low_speed_tracker_error = 0.0f;
    float drop_recovery_error = 0.0f;
    unsigned int lpf_step_delay = 0U;
    unsigned int tracker_step_delay = 0U;
    unsigned int lpf_reverse_delay = 0U;
    unsigned int tracker_reverse_delay = 0U;
    bool lpf_step_seen = false;
    bool tracker_step_seen = false;
    bool lpf_reverse_seen = false;
    bool tracker_reverse_seen = false;
    unsigned int samples = 0U;

    TEST_ASSERT(foc_encoder_init(&context, &config) == 0);
    TEST_ASSERT(foc_encoder_update(&context, 0U, 20000U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    for (unsigned int index = 1U; index <= 2000U; ++index) {
        const int32_t noise = (int32_t)(index % 3U) - 1;
        const uint32_t code = (uint32_t)((int32_t)20000 + noise);
        TEST_ASSERT(
            foc_encoder_update(
                &context,
                index * dt_us,
                code,
                FOC_ENCODER_TRANSPORT_OK,
                &feedback
            ) == 0
        );
        static_raw += feedback.raw_velocity_rad_s * feedback.raw_velocity_rad_s;
        static_lpf += feedback.filtered_velocity_rad_s * feedback.filtered_velocity_rad_s;
        static_tracker += feedback.tracked_velocity_rad_s * feedback.tracked_velocity_rad_s;
    }

    TEST_ASSERT(foc_encoder_reset(&context) == 0);
    TEST_ASSERT(foc_encoder_update(&context, 0U, 1000U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    const uint32_t step_counts = (uint32_t)lroundf(8.0f * dt_s / count_rad);
    for (unsigned int index = 1U; index <= 600U; ++index) {
        const uint32_t code =
            index < 201U ? 1000U : (1000U + (index - 200U) * step_counts) & 0xffffU;
        TEST_ASSERT(
            foc_encoder_update(
                &context,
                index * dt_us,
                code,
                FOC_ENCODER_TRANSPORT_OK,
                &feedback
            ) == 0
        );
        if (index >= 201U) {
            if (!lpf_step_seen && fabsf(feedback.filtered_velocity_rad_s - 8.0f) < 0.8f) {
                lpf_step_delay = index - 200U;
                lpf_step_seen = true;
            }
            if (!tracker_step_seen && fabsf(feedback.tracked_velocity_rad_s - 8.0f) < 0.8f) {
                tracker_step_delay = index - 200U;
                tracker_step_seen = true;
            }
        }
    }

    TEST_ASSERT(foc_encoder_reset(&context) == 0);
    TEST_ASSERT(foc_encoder_update(&context, 0U, 30000U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    const uint32_t forward_counts = (uint32_t)lroundf(4.0f * dt_s / count_rad);
    const uint32_t reverse_counts = (uint32_t)lroundf(-4.0f * dt_s / count_rad);
    for (unsigned int index = 1U; index <= 800U; ++index) {
        const uint32_t code = index < 301U
            ? (30000U + index * forward_counts) & 0xffffU
            : (30000U + 300U * forward_counts + (index - 300U) * reverse_counts) & 0xffffU;
        TEST_ASSERT(
            foc_encoder_update(
                &context,
                index * dt_us,
                code,
                FOC_ENCODER_TRANSPORT_OK,
                &feedback
            ) == 0
        );
        if (index >= 301U) {
            if (!lpf_reverse_seen && feedback.filtered_velocity_rad_s < -3.6f) {
                lpf_reverse_delay = index - 300U;
                lpf_reverse_seen = true;
            }
            if (!tracker_reverse_seen && feedback.tracked_velocity_rad_s < -3.6f) {
                tracker_reverse_delay = index - 300U;
                tracker_reverse_seen = true;
            }
        }
    }

    TEST_ASSERT(foc_encoder_reset(&context) == 0);
    TEST_ASSERT(foc_encoder_update(&context, 0U, 4000U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    float angle_rad = 4000.0f * count_rad;
    for (unsigned int index = 1U; index <= 1000U; ++index) {
        const float target_speed = 8.0f * (float)index / 1000.0f;
        angle_rad += target_speed * dt_s;
        const uint32_t code = (uint32_t)lroundf(angle_rad / count_rad) & 0xffffU;
        TEST_ASSERT(
            foc_encoder_update(
                &context,
                index * dt_us,
                code,
                FOC_ENCODER_TRANSPORT_OK,
                &feedback
            ) == 0
        );
        acceleration_lpf += (feedback.filtered_velocity_rad_s - target_speed) *
            (feedback.filtered_velocity_rad_s - target_speed);
        acceleration_tracker += (feedback.tracked_velocity_rad_s - target_speed) *
            (feedback.tracked_velocity_rad_s - target_speed);
        ++samples;
    }

    TEST_ASSERT(lpf_step_seen && tracker_step_seen && lpf_reverse_seen && tracker_reverse_seen);
    TEST_ASSERT(foc_encoder_reset(&context) == 0);
    TEST_ASSERT(foc_encoder_update(&context, 0U, 12000U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    const uint32_t low_speed_counts = (uint32_t)lroundf(0.2f * dt_s / count_rad);
    for (unsigned int index = 1U; index <= 1000U; ++index) {
        const uint32_t code = (12000U + index * low_speed_counts) & 0xffffU;
        TEST_ASSERT(
            foc_encoder_update(
                &context,
                index * dt_us,
                code,
                FOC_ENCODER_TRANSPORT_OK,
                &feedback
            ) == 0
        );
        low_speed_raw_error +=
            (feedback.raw_velocity_rad_s - 0.2f) * (feedback.raw_velocity_rad_s - 0.2f);
        low_speed_lpf_error +=
            (feedback.filtered_velocity_rad_s - 0.2f) * (feedback.filtered_velocity_rad_s - 0.2f);
        low_speed_tracker_error +=
            (feedback.tracked_velocity_rad_s - 0.2f) * (feedback.tracked_velocity_rad_s - 0.2f);
    }

    TEST_ASSERT(foc_encoder_reset(&context) == 0);
    TEST_ASSERT(foc_encoder_update(&context, 0U, 14000U, FOC_ENCODER_TRANSPORT_OK, &feedback) == 0);
    for (unsigned int index = 1U; index <= 200U; ++index) {
        const uint32_t code = (14000U + index * forward_counts) & 0xffffU;
        const enum foc_encoder_transport transport =
            index == 101U ? FOC_ENCODER_TRANSPORT_TIMEOUT : FOC_ENCODER_TRANSPORT_OK;
        (void)foc_encoder_update(&context, index * dt_us, code, transport, &feedback);
        if (index == 102U) {
            TEST_ASSERT(feedback.valid);
            drop_recovery_error = fabsf(feedback.raw_velocity_rad_s - 4.0f);
        }
    }
    TEST_ASSERT(drop_recovery_error < 0.2f);
    printf(
        "encoder metrics static_rms_raw=%.7g static_rms_lpf=%.7g "
        "static_rms_tracker=%.7g step_delay_lpf=%u step_delay_tracker=%u "
        "reverse_delay_lpf=%u reverse_delay_tracker=%u accel_rmse_lpf=%.7g "
        "accel_rmse_tracker=%.7g low_rms_raw=%.7g low_rms_lpf=%.7g "
        "low_rms_tracker=%.7g drop_recovery_abs_error=%.7g\n",
        sqrtf(static_raw / 2000.0f),
        sqrtf(static_lpf / 2000.0f),
        sqrtf(static_tracker / 2000.0f),
        lpf_step_delay,
        tracker_step_delay,
        lpf_reverse_delay,
        tracker_reverse_delay,
        sqrtf(acceleration_lpf / (float)samples),
        sqrtf(acceleration_tracker / (float)samples),
        sqrtf(low_speed_raw_error / 1000.0f),
        sqrtf(low_speed_lpf_error / 1000.0f),
        sqrtf(low_speed_tracker_error / 1000.0f),
        drop_recovery_error
    );
    return 0;
}

static int test_instances_and_reset(void)
{
    struct foc_encoder_context first = {0};
    struct foc_encoder_context second = {0};
    struct foc_encoder_feedback a;
    struct foc_encoder_feedback b;
    struct foc_encoder_config first_config = test_config();
    struct foc_encoder_config second_config = test_config();

    second_config.counts_per_revolution = 4096U;
    second_config.direction = FOC_ENCODER_DIRECTION_REVERSED;
    second_config.max_speed_rad_s = 20.0f;
    TEST_ASSERT(foc_encoder_init(&first, &first_config) == 0);
    TEST_ASSERT(foc_encoder_init(&second, &second_config) == 0);
    TEST_ASSERT(foc_encoder_update(&first, 0U, 100U, FOC_ENCODER_TRANSPORT_OK, &a) == 0);
    TEST_ASSERT(foc_encoder_update(&second, 0U, 100U, FOC_ENCODER_TRANSPORT_OK, &b) == 0);
    TEST_ASSERT(foc_encoder_update(&first, 1000U, 101U, FOC_ENCODER_TRANSPORT_OK, &a) == 0);
    TEST_ASSERT(foc_encoder_update(&second, 1000U, 99U, FOC_ENCODER_TRANSPORT_OK, &b) == 0);
    TEST_ASSERT(a.valid && b.valid && a.delta_rad > 0.0f && b.delta_rad > 0.0f);
    TEST_ASSERT(first.position_counts != second.position_counts);
    TEST_ASSERT(foc_encoder_reset(&first) == 0 && !first.previous_seen && !first.continuity_lost);
    TEST_ASSERT(second.previous_seen && !second.continuity_lost);
    return 0;
}

static int test_integer_phase_source(void)
{
    const uint32_t cprs[] = {3U, 7U, 1000U, 16384U, 65535U, 65536U};
    for (unsigned c = 0; c < sizeof cprs / sizeof *cprs; ++c) {
        for (int direction = -1; direction <= 1; direction += 2) {
            struct foc_encoder_config policy = test_config();
            policy.counts_per_revolution = cprs[c];
            policy.direction = (enum foc_encoder_direction)direction;
            policy.max_speed_rad_s = 6000.0f;
            struct foc_encoder_context encoder = {0};
            struct foc_encoder_feedback frame;
            TEST_ASSERT(foc_encoder_init(&encoder, &policy) == 0);
            uint32_t time = UINT32_MAX - 1000U;
            uint32_t raw = 1U;
            int64_t expected = direction > 0 ? 1 : (int64_t)cprs[c] - 1;
            TEST_ASSERT(
                foc_encoder_update(&encoder, time, raw, FOC_ENCODER_TRANSPORT_OK, &frame) == 0
            );
            TEST_ASSERT(
                frame.mechanical_phase_count == (uint32_t)expected && !frame.valid &&
                !frame.speed_valid
            );
            const int step = cprs[c] < 64U ? 1 : (int)(cprs[c] / 64U);
            for (unsigned i = 0; i < 2000U; ++i) {
                const int travel = i < 1000U ? step : -step;
                const int64_t next_raw = (int64_t)raw + travel + cprs[c];
                raw = (uint32_t)(next_raw % cprs[c]);
                expected += (int64_t)direction * travel;
                time += 500U;
                TEST_ASSERT(
                    foc_encoder_update(&encoder, time, raw, FOC_ENCODER_TRANSPORT_OK, &frame) ==
                        0 &&
                    frame.valid && frame.speed_valid
                );
                int64_t remainder = expected % (int64_t)cprs[c];
                if (remainder < 0) {
                    remainder += cprs[c];
                }
                TEST_ASSERT(
                    encoder.position_counts == expected && frame.captured_us == time &&
                    frame.raw_code == raw && frame.mechanical_phase_count == (uint32_t)remainder
                );
            }
            const int64_t accepted = encoder.position_counts;
            TEST_ASSERT(
                foc_encoder_update(&encoder, time, raw, FOC_ENCODER_TRANSPORT_OK, &frame) ==
                FOC_ENCODER_STATUS_ERR_ORDER
            );
            TEST_ASSERT(
                !frame.valid && frame.mechanical_phase_count == 0U &&
                encoder.position_counts == accepted
            );
            TEST_ASSERT(
                foc_encoder_update(
                    &encoder,
                    time + 500U,
                    raw,
                    FOC_ENCODER_TRANSPORT_TIMEOUT,
                    &frame
                ) < 0
            );
            TEST_ASSERT(!frame.valid && frame.mechanical_phase_count == 0U);
            TEST_ASSERT(foc_encoder_reset(&encoder) == 0);
            TEST_ASSERT(
                foc_encoder_update(&encoder, time + 1000U, 1U, FOC_ENCODER_TRANSPORT_OK, &frame) ==
                0
            );
            TEST_ASSERT(
                !frame.valid && !frame.speed_valid &&
                frame.mechanical_phase_count == (direction > 0 ? 1U : cprs[c] - 1U)
            );
        }
    }
    return 0;
}

static int test_phase_at_count_overflow(void)
{
    for (unsigned negative = 0U; negative < 2U; ++negative) {
        struct foc_encoder_context encoder;
        struct foc_encoder_feedback frame;
        const struct foc_encoder_config policy = test_config();
        const uint32_t first = negative ? 3U : 65532U;
        const uint32_t second = negative ? 2U : 65533U;
        const uint32_t edge = negative ? 0U : 65535U;
        const uint32_t next = negative ? 65535U : 0U;
        const int64_t limit = negative ? INT64_MIN : INT64_MAX;
        TEST_ASSERT(foc_encoder_init(&encoder, &policy) == 0);
        TEST_ASSERT(foc_encoder_update(&encoder, 0U, first, FOC_ENCODER_TRANSPORT_OK, &frame) == 0);
        /* Seed complete turns, then reach the exact limit using valid updates. */
        encoder.position_counts = negative ? limit + 3 : limit - 3;
        TEST_ASSERT(
            foc_encoder_update(&encoder, 1000U, second, FOC_ENCODER_TRANSPORT_OK, &frame) == 0
        );
        TEST_ASSERT(
            foc_encoder_update(&encoder, 2000U, edge, FOC_ENCODER_TRANSPORT_OK, &frame) == 0
        );
        TEST_ASSERT(encoder.position_counts == limit && frame.speed_valid && encoder.tracker_seen);
        TEST_ASSERT(
            frame.mechanical_phase_count == edge && encoder.tracker_local_offset_rad != 0.0f
        );
        const struct foc_encoder_context accepted = encoder;
        TEST_ASSERT(
            foc_encoder_update(&encoder, 2500U, next, FOC_ENCODER_TRANSPORT_OK, &frame) ==
            FOC_ENCODER_STATUS_ERR_SAMPLE
        );
        TEST_ASSERT(
            encoder.position_counts == limit && encoder.previous_us == 2000U &&
            encoder.previous_code == edge && !frame.valid && !frame.speed_valid &&
            frame.mechanical_phase_count == 0U && frame.quality == FOC_ENCODER_QUALITY_NUMERIC_ERROR
        );
        TEST_ASSERT(encoder.previous_corrected_code == edge && !encoder.continuity_lost);
        TEST_ASSERT(encoder.tracker_local_offset_rad == accepted.tracker_local_offset_rad);
        TEST_ASSERT(encoder.tracker_velocity_rad_s == accepted.tracker_velocity_rad_s);
        TEST_ASSERT(encoder.lpf_velocity_rad_s == accepted.lpf_velocity_rad_s);
        TEST_ASSERT(encoder.invalid_count == 1U && frame.invalid_count == 1U);
    }
    return 0;
}

static int test_tracker_rejection_and_reset(void)
{
    static const struct {
        uint32_t us;
        uint32_t code;
        enum foc_encoder_transport transport;
        enum foc_encoder_quality quality;
        int status;
    } rejected[] = {
        {2000U,
         6U,
         FOC_ENCODER_TRANSPORT_OK,
         FOC_ENCODER_QUALITY_DUPLICATE,
         FOC_ENCODER_STATUS_ERR_ORDER},
        {2050U,
         6U,
         FOC_ENCODER_TRANSPORT_OK,
         FOC_ENCODER_QUALITY_INVALID_INTERVAL,
         FOC_ENCODER_STATUS_ERR_ORDER},
        {2300U,
         6U,
         FOC_ENCODER_TRANSPORT_TIMEOUT,
         FOC_ENCODER_QUALITY_TIMEOUT,
         FOC_ENCODER_STATUS_ERR_TRANSPORT},
        {2600U,
         6U,
         FOC_ENCODER_TRANSPORT_FRAME_ERROR,
         FOC_ENCODER_QUALITY_TRANSPORT_ERROR,
         FOC_ENCODER_STATUS_ERR_TRANSPORT},
        {2800U,
         65536U,
         FOC_ENCODER_TRANSPORT_OK,
         FOC_ENCODER_QUALITY_INVALID_SAMPLE,
         FOC_ENCODER_STATUS_ERR_SAMPLE},
        {3000U,
         5000U,
         FOC_ENCODER_TRANSPORT_OK,
         FOC_ENCODER_QUALITY_SPEED_OUT_OF_RANGE,
         FOC_ENCODER_STATUS_ERR_SPEED},
        {4000U,
         6U,
         FOC_ENCODER_TRANSPORT_OK,
         FOC_ENCODER_QUALITY_DISCONTINUITY,
         FOC_ENCODER_STATUS_ERR_DISCONTINUITY},
    };
    const struct foc_encoder_config config = test_config();
    struct foc_encoder_context encoder;
    struct foc_encoder_feedback frame;
    TEST_ASSERT(foc_encoder_init(&encoder, &config) == 0);
    TEST_ASSERT(foc_encoder_update(&encoder, 0U, 0U, FOC_ENCODER_TRANSPORT_OK, &frame) == 0);
    TEST_ASSERT(foc_encoder_update(&encoder, 1000U, 1U, FOC_ENCODER_TRANSPORT_OK, &frame) == 0);
    TEST_ASSERT(foc_encoder_update(&encoder, 2000U, 4U, FOC_ENCODER_TRANSPORT_OK, &frame) == 0);
    TEST_ASSERT(encoder.tracker_seen && encoder.tracker_local_offset_rad != 0.0f);
    const struct foc_encoder_context accepted = encoder;
    for (unsigned i = 0U; i < sizeof rejected / sizeof rejected[0]; ++i) {
        TEST_ASSERT(
            foc_encoder_update(
                &encoder,
                rejected[i].us,
                rejected[i].code,
                rejected[i].transport,
                &frame
            ) == rejected[i].status
        );
        TEST_ASSERT(!frame.valid && !frame.speed_valid && frame.quality == rejected[i].quality);
        TEST_ASSERT(
            frame.continuity_lost == (i >= 5U) && encoder.continuity_lost == frame.continuity_lost
        );
        TEST_ASSERT(
            encoder.invalid_count == i + 1U && encoder.invalid_count == frame.invalid_count
        );
        TEST_ASSERT(
            encoder.position_counts == accepted.position_counts &&
            encoder.previous_us == accepted.previous_us
        );
        TEST_ASSERT(
            encoder.previous_code == accepted.previous_code &&
            encoder.previous_corrected_code == accepted.previous_corrected_code
        );
        TEST_ASSERT(encoder.tracker_seen && encoder.velocity_seen);
        TEST_ASSERT(encoder.tracker_local_offset_rad == accepted.tracker_local_offset_rad);
        TEST_ASSERT(
            encoder.tracker_velocity_rad_s == accepted.tracker_velocity_rad_s &&
            frame.tracked_velocity_rad_s == accepted.tracker_velocity_rad_s
        );
        TEST_ASSERT(encoder.lpf_velocity_rad_s == accepted.lpf_velocity_rad_s);
    }
    TEST_ASSERT(foc_encoder_reset(&encoder) == 0);
    TEST_ASSERT(!encoder.tracker_seen && !encoder.continuity_lost && encoder.invalid_count == 0U);
    TEST_ASSERT(encoder.tracker_velocity_rad_s == 0.0f && encoder.tracker_local_offset_rad == 0.0f);
    TEST_ASSERT(foc_encoder_update(&encoder, 5000U, 300U, FOC_ENCODER_TRANSPORT_OK, &frame) == 0);
    TEST_ASSERT(!frame.valid && !frame.speed_valid && !encoder.tracker_seen);
    TEST_ASSERT(foc_encoder_update(&encoder, 6000U, 310U, FOC_ENCODER_TRANSPORT_OK, &frame) == 0);
    TEST_ASSERT(
        frame.speed_valid && encoder.tracker_seen && encoder.tracker_local_offset_rad == 0.0f
    );
    TEST_ASSERT(frame.tracked_velocity_rad_s == frame.raw_velocity_rad_s);
    return 0;
}

static int test_tracker_numeric_rejection(void)
{
    struct foc_encoder_config config = test_config();
    config.max_speed_rad_s = 1.5f;
    config.max_dt_us = 2000000U;
    for (unsigned i = 0U; i < 3U; ++i) {
        struct foc_encoder_context encoder;
        struct foc_encoder_feedback frame;
        TEST_ASSERT(foc_encoder_init(&encoder, &config) == 0);
        TEST_ASSERT(foc_encoder_update(&encoder, 0U, 100U, FOC_ENCODER_TRANSPORT_OK, &frame) == 0);
        TEST_ASSERT(
            foc_encoder_update(&encoder, 1000000U, 101U, FOC_ENCODER_TRANSPORT_OK, &frame) == 0
        );
        TEST_ASSERT(encoder.tracker_seen);
        /* Inject terminal values only after establishing an accepted anchor. */
        if (i == 0U) {
            encoder.tracker_local_offset_rad = NAN;
        } else {
            encoder.tracker_velocity_rad_s = i == 1U ? INFINITY : FLT_MAX;
        }
        const struct foc_encoder_context accepted = encoder;
        TEST_ASSERT(
            foc_encoder_update(&encoder, 2500000U, 102U, FOC_ENCODER_TRANSPORT_OK, &frame) ==
            FOC_ENCODER_STATUS_ERR_SAMPLE
        );
        TEST_ASSERT(
            frame.quality == FOC_ENCODER_QUALITY_NUMERIC_ERROR && !frame.valid && !frame.speed_valid
        );
        TEST_ASSERT(!frame.continuity_lost && !encoder.continuity_lost && encoder.tracker_seen);
        TEST_ASSERT(frame.invalid_count == 1U && encoder.invalid_count == 1U);
        TEST_ASSERT(
            encoder.position_counts == accepted.position_counts &&
            encoder.previous_us == accepted.previous_us
        );
        TEST_ASSERT(
            encoder.previous_code == accepted.previous_code &&
            encoder.previous_corrected_code == accepted.previous_corrected_code
        );
        TEST_ASSERT(encoder.lpf_velocity_rad_s == accepted.lpf_velocity_rad_s);
        TEST_ASSERT(encoder.tracker_velocity_rad_s == accepted.tracker_velocity_rad_s);
        TEST_ASSERT(
            i == 0U ? isnan(encoder.tracker_local_offset_rad)
                    : encoder.tracker_local_offset_rad == accepted.tracker_local_offset_rad
        );
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

static int test_encoder_lut(void)
{
    static const int16_t offsets[] = {6, 12, 3, -6};
    static const int16_t zero[] = {0, 0, 0, 0};
    static const int16_t out_of_range[] = {6, 13, 3, -6};
    static const int16_t seam_jump[] = {12, 4, -4, -12};
    static const int16_t reverse_mapping[] = {12, -12, 0, 0};
    const struct foc_encoder_lut table = {offsets, 4U, 100U, 41U};
    const struct foc_encoder_lut no_change = {zero, 4U, 100U, 42U};
    struct foc_encoder_config config = test_config();
    config.counts_per_revolution = 100U;
    config.max_speed_rad_s = 1000.0f;
    config.max_dt_us = 2400U;
    struct foc_encoder_context normal = {0}, reverse = {0}, disabled = {0}, zeros = {0};
    TEST_ASSERT(foc_encoder_lut_validate(&table, 100U, 41U) == FOC_ENCODER_STATUS_OK);
    TEST_ASSERT(foc_encoder_lut_validate(&table, 101U, 41U) == FOC_ENCODER_STATUS_ERR_LUT_IDENTITY);
    TEST_ASSERT(foc_encoder_lut_validate(&table, 100U, 42U) == FOC_ENCODER_STATUS_ERR_LUT_IDENTITY);
    TEST_ASSERT(foc_encoder_lut_validate(&table, 100U, 0U) == FOC_ENCODER_STATUS_ERR_LUT_IDENTITY);
    struct foc_encoder_lut bad = table;
    bad.offset_counts = out_of_range;
    TEST_ASSERT(foc_encoder_lut_validate(&bad, 100U, 41U) == FOC_ENCODER_STATUS_ERR_LUT_RANGE);
    bad.offset_counts = seam_jump;
    TEST_ASSERT(foc_encoder_lut_validate(&bad, 100U, 41U) == FOC_ENCODER_STATUS_ERR_LUT_SLOPE);
    bad.offset_counts = reverse_mapping;
    TEST_ASSERT(foc_encoder_lut_validate(&bad, 100U, 41U) == FOC_ENCODER_STATUS_ERR_LUT_SLOPE);
    bad.length = 1U;
    TEST_ASSERT(foc_encoder_lut_validate(&bad, 100U, 41U) == FOC_ENCODER_STATUS_ERR_CONFIG);
    TEST_ASSERT(foc_encoder_init_with_lut(&normal, &config, 41U, &table) == FOC_ENCODER_STATUS_OK);
    TEST_ASSERT(
        foc_encoder_init_with_lut(&zeros, &config, 42U, &no_change) == FOC_ENCODER_STATUS_OK
    );
    TEST_ASSERT(foc_encoder_init(&disabled, &config) == FOC_ENCODER_STATUS_OK);
    config.direction = FOC_ENCODER_DIRECTION_REVERSED;
    TEST_ASSERT(foc_encoder_init_with_lut(&reverse, &config, 41U, &table) == FOC_ENCODER_STATUS_OK);

    static const uint32_t raw[] = {89U, 95U, 96U, 99U, 0U, 1U};
    static const uint32_t corrected[] = {90U, 99U, 0U, 5U, 6U, 7U};
    struct foc_encoder_feedback n, r, off, z;
    uint32_t us = UINT32_MAX - 2500U;
    for (unsigned k = 0U; k < sizeof raw / sizeof raw[0]; ++k, us += 1000U) {
        uint32_t direct;
        TEST_ASSERT(foc_encoder_correct_code(&normal, raw[k], &direct) == 0);
        TEST_ASSERT(direct == corrected[k]);
        TEST_ASSERT(foc_encoder_update(&normal, us, raw[k], FOC_ENCODER_TRANSPORT_OK, &n) == 0);
        TEST_ASSERT(foc_encoder_update(&reverse, us, raw[k], FOC_ENCODER_TRANSPORT_OK, &r) == 0);
        TEST_ASSERT(foc_encoder_update(&disabled, us, raw[k], FOC_ENCODER_TRANSPORT_OK, &off) == 0);
        TEST_ASSERT(foc_encoder_update(&zeros, us, raw[k], FOC_ENCODER_TRANSPORT_OK, &z) == 0);
        const uint32_t opposite = direct == 0U ? 0U : 100U - direct;
        TEST_ASSERT(
            n.raw_code == raw[k] && n.corrected_code == direct && r.corrected_code == direct
        );
        TEST_ASSERT(n.mechanical_phase_count == direct && r.mechanical_phase_count == opposite);
        TEST_ASSERT(normal.position_counts == (int64_t)(direct + (k >= 2U ? 100U : 0U)));
        TEST_ASSERT(normal.position_counts + reverse.position_counts == 100);
        TEST_ASSERT(n.raw_velocity_rad_s == -r.raw_velocity_rad_s);
        TEST_ASSERT(
            off.corrected_code == z.corrected_code &&
            off.mechanical_phase_count == z.mechanical_phase_count && off.valid == z.valid &&
            off.mechanical_rad == z.mechanical_rad && off.single_turn_rad == z.single_turn_rad &&
            off.delta_rad == z.delta_rad && off.raw_velocity_rad_s == z.raw_velocity_rad_s &&
            off.filtered_velocity_rad_s == z.filtered_velocity_rad_s &&
            off.tracked_velocity_rad_s == z.tracked_velocity_rad_s
        );
    }
    TEST_ASSERT(foc_encoder_init_with_lut(&normal, &config, 41U, &bad) != FOC_ENCODER_STATUS_OK);
    TEST_ASSERT(normal.config.direction == FOC_ENCODER_DIRECTION_NORMAL);
    const float last_lpf = normal.lpf_velocity_rad_s;
    const float last_tracker = normal.tracker_velocity_rad_s;
    TEST_ASSERT(
        foc_encoder_update(&normal, us, 2U, FOC_ENCODER_TRANSPORT_TIMEOUT, &n) ==
        FOC_ENCODER_STATUS_ERR_TRANSPORT
    );
    TEST_ASSERT(
        foc_encoder_update(&normal, us - 1000U, 2U, FOC_ENCODER_TRANSPORT_OK, &n) ==
        FOC_ENCODER_STATUS_ERR_ORDER
    );
    TEST_ASSERT(!n.valid && n.corrected_code == 0U && n.mechanical_phase_count == 0U);
    TEST_ASSERT(
        normal.position_counts == 107 && normal.previous_code == 1U &&
        normal.previous_corrected_code == 7U && normal.lpf_velocity_rad_s == last_lpf &&
        normal.tracker_velocity_rad_s == last_tracker
    );
    TEST_ASSERT(foc_encoder_update(&normal, us + 1000U, 3U, FOC_ENCODER_TRANSPORT_OK, &n) == 0);
    TEST_ASSERT(n.valid && n.corrected_code == 10U && normal.position_counts == 110);
    TEST_ASSERT(foc_encoder_reset(&normal) == 0);
    TEST_ASSERT(foc_encoder_update(&normal, 0U, 95U, FOC_ENCODER_TRANSPORT_OK, &n) == 0);
    TEST_ASSERT(!n.valid && n.corrected_code == 99U && normal.position_counts == 99);
    TEST_ASSERT(foc_encoder_init(&normal, &config) == 0);
    TEST_ASSERT(foc_encoder_update(&normal, 0U, 95U, FOC_ENCODER_TRANSPORT_OK, &n) == 0);
    TEST_ASSERT(n.corrected_code == 95U && n.mechanical_phase_count == 5U);

    /* The same raw short arc can cross half a turn after correction.
     * Do not admit the opposite corrected short arc even if speed allows it. */
    static const int16_t stretched[] = {-10, 10};
    const struct foc_encoder_lut ambiguity = {stretched, 2U, 100U, 41U};
    config.direction = FOC_ENCODER_DIRECTION_NORMAL;
    config.max_speed_rad_s = 2500.0f;
    TEST_ASSERT(foc_encoder_init_with_lut(&normal, &config, 41U, &ambiguity) == 0);
    TEST_ASSERT(foc_encoder_update(&normal, 0U, 0U, FOC_ENCODER_TRANSPORT_OK, &n) == 0);
    TEST_ASSERT(
        foc_encoder_update(&normal, 1000U, 49U, FOC_ENCODER_TRANSPORT_OK, &n) ==
        FOC_ENCODER_STATUS_ERR_DISCONTINUITY
    );
    TEST_ASSERT(n.continuity_lost && normal.position_counts == 90 && normal.previous_code == 0U);
    /* Quantization can push the corrected arc past half a turn even when
     * both shortest arcs agree. The exact calibration maps analog
     * coordinates 5.20 -> 66.51 to +49.994 counts, while raw 5 -> 67
     * maps to 13 -> 64 and the shorter apparent corrected arc is -49. */
    static const int16_t compressed[] = {10, -10};
    const struct foc_encoder_lut quantized_ambiguity = {compressed, 2U, 100U, 41U};
    config.max_speed_rad_s = 3141.5f;
    TEST_ASSERT(foc_encoder_init_with_lut(&normal, &config, 41U, &quantized_ambiguity) == 0);
    TEST_ASSERT(foc_encoder_update(&normal, 0U, 5U, FOC_ENCODER_TRANSPORT_OK, &n) == 0);
    TEST_ASSERT(
        foc_encoder_update(&normal, 1000U, 67U, FOC_ENCODER_TRANSPORT_OK, &n) ==
        FOC_ENCODER_STATUS_ERR_DISCONTINUITY
    );
    TEST_ASSERT(n.continuity_lost && normal.position_counts == 13);
    config.max_speed_rad_s = 1000.0f;

    /* Non-divisible CPR, both senses of travel and well beyond float32 exact integer range. */
    const struct foc_encoder_lut odd = {offsets, 4U, 101U, 41U};
    config.counts_per_revolution = 101U;
    for (int direction = -1; direction <= 1; direction += 2) {
        config.direction = (enum foc_encoder_direction)direction;
        TEST_ASSERT(foc_encoder_init_with_lut(&normal, &config, 41U, &odd) == 0);
        TEST_ASSERT(foc_encoder_update(&normal, 0U, 0U, FOC_ENCODER_TRANSPORT_OK, &n) == 0);
        const int64_t turns = (int64_t)direction * 1000000 * 101;
        normal.position_counts += turns; /* Seed complete integer turns. */
        const int64_t initial = normal.position_counts;
        uint32_t prev = n.corrected_code;
        for (unsigned k = 1U; k <= 10100U; ++k) {
            TEST_ASSERT(
                foc_encoder_update(&normal, k * 1000U, k % 101U, FOC_ENCODER_TRANSPORT_OK, &n) == 0
            );
            const uint32_t advance = (n.corrected_code + 101U - prev) % 101U;
            TEST_ASSERT(advance <= 2U);
            const int64_t phase = (normal.position_counts % 101 + 101) % 101;
            TEST_ASSERT(n.mechanical_phase_count == (uint32_t)phase);
            TEST_ASSERT(near(
                n.delta_rad,
                (float)(direction * (int)advance) * (6.28318530718f / 101.0f),
                1.0e-6f
            ));
            prev = n.corrected_code;
        }
        TEST_ASSERT(normal.position_counts == initial + direction * 10100);
    }
    return 0;
}

int main(void)
{
    if (test_encoder_lut() != 0) {
        return 1;
    }
    return test_lifecycle_and_single_turn() || test_wrap_direction_and_multiturn() ||
        test_timestamps_and_continuity() || test_transport_and_invalid_samples() ||
        test_speed_filters_and_deterministic_dataset() || test_algorithm_comparison_metrics() ||
        test_instances_and_reset() || test_integer_phase_source() ||
        test_phase_at_count_overflow() || test_tracker_rejection_and_reset() ||
        test_tracker_numeric_rejection();
}

/* ---------------------------------------- end of file ----------------------------------------- */
