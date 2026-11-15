/**
 * @file    test_foc_sensor_reference.c
 * @brief   Independent encoder unwrap, LPF, and alpha-beta reference test.
 * @version v0.1.0
 * @date    2026.11.14
 *
 * Design overview
 * ---------------
 * Generate an analytic constant-speed encoder trajectory with integer code
 * quantization, then recompute position, raw speed, LPF, and tracker state
 * independently in double precision.
 *
 * Test role
 * ---------
 * Verify accepted samples, wrap crossings, direction policy, and rejection
 * invariants without calling production helpers for expected values.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_encoder.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Private Constants ------------------------------------------------------------ */

static const double REF_TWO_PI = 6.28318530717958647692;

/* Private Helpers -------------------------------------------------------------- */

static uint32_t code_for_angle(double angle, uint32_t counts)
{
    double wrapped = fmod(angle, REF_TWO_PI);
    if (wrapped < 0.0) {
        wrapped += REF_TWO_PI;
    }
    return (uint32_t)llround(wrapped * (double)counts / REF_TWO_PI) % counts;
}

static int close_enough(double actual, double expected, double tolerance)
{
    const double scale = fmax(1.0, fmax(fabs(actual), fabs(expected)));
    return fabs(actual - expected) <= tolerance * scale;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    const uint32_t counts = 65536U;
    const double speed = 4.0;
    const double dt_s = 0.001;
    const uint32_t dt_us = 1000U;
    const float cutoff_hz = 80.0f;
    const double tau = 1.0 / (REF_TWO_PI * (double)cutoff_hz);
    const double alpha = dt_s / (tau + dt_s);
    const double tracker_alpha = 0.85;
    const double tracker_beta = 0.2;
    const struct foc_encoder_config config = {
        .counts_per_revolution = counts,
        .direction = FOC_ENCODER_DIRECTION_NORMAL,
        .max_speed_rad_s = 100.0f,
        .min_dt_us = 1U,
        .max_dt_us = 2000U,
        .speed_lpf_cutoff_hz = cutoff_hz,
        .tracker_alpha = (float)tracker_alpha,
        .tracker_beta = (float)tracker_beta,
    };
    struct foc_encoder_context context;
    uint32_t previous_code = code_for_angle(REF_TWO_PI - speed * dt_s, counts);
    int64_t position_counts = (int64_t)previous_code;
    double lpf_velocity = 0.0;
    double tracker_position = 0.0;
    double tracker_velocity = 0.0;
    double max_tracker_error = 0.0;
    int velocity_seen = 0;
    int tracker_seen = 0;
    struct foc_encoder_feedback feedback;

    if (foc_encoder_init(&context, &config) != FOC_ENCODER_STATUS_OK) {
        fprintf(stderr, "encoder reference initialization failed\n");
        return 1;
    }
    if (foc_encoder_update(&context, 0U, previous_code, FOC_ENCODER_TRANSPORT_OK, &feedback) !=
        FOC_ENCODER_STATUS_OK) {
        fprintf(stderr, "encoder first sample failed\n");
        return 1;
    }

    for (unsigned int index = 1U; index <= 5000U; ++index) {
        const uint32_t timestamp = index * dt_us;
        const uint32_t raw_code = code_for_angle(speed * (double)index * dt_s, counts);
        int64_t delta = (int64_t)raw_code - (int64_t)previous_code;
        const int64_t half = (int64_t)counts / 2;
        double expected_raw_velocity;
        double expected_position;
        double expected_filtered;
        double expected_tracker_position;
        double expected_tracker_velocity;

        if (delta > half) {
            delta -= (int64_t)counts;
        } else if (delta < -half) {
            delta += (int64_t)counts;
        }
        position_counts += delta;
        expected_raw_velocity = ((double)delta * REF_TWO_PI / (double)counts) / dt_s;
        expected_position = (double)position_counts * REF_TWO_PI / (double)counts;
        expected_filtered = velocity_seen
            ? lpf_velocity + alpha * (expected_raw_velocity - lpf_velocity)
            : expected_raw_velocity;
        if (!tracker_seen) {
            expected_tracker_position = expected_position;
            expected_tracker_velocity = expected_raw_velocity;
        } else {
            const double predicted = tracker_position + tracker_velocity * dt_s;
            const double residual = expected_position - predicted;
            expected_tracker_position = predicted + tracker_alpha * residual;
            expected_tracker_velocity = tracker_velocity + tracker_beta / dt_s * residual;
        }
        if (foc_encoder_update(
                &context,
                timestamp,
                raw_code,
                FOC_ENCODER_TRANSPORT_OK,
                &feedback
            ) != FOC_ENCODER_STATUS_OK ||
            !feedback.valid ||
            !close_enough(feedback.raw_velocity_rad_s, expected_raw_velocity, 2.0e-5) ||
            !close_enough(feedback.mechanical_rad, expected_position, 2.0e-5) ||
            !close_enough(feedback.filtered_velocity_rad_s, expected_filtered, 2.0e-5) ||
            !close_enough(feedback.tracked_velocity_rad_s, expected_tracker_velocity, 1.0e-3)) {
            fprintf(
                stderr,
                "encoder reference mismatch index=%u raw=%u mech=%.9g/%.9g rawv=%.9g/%.9g "
                "filt=%.9g/%.9g track=%.9g/%.9g quality=%d\n",
                index,
                raw_code,
                feedback.mechanical_rad,
                expected_position,
                feedback.raw_velocity_rad_s,
                expected_raw_velocity,
                feedback.filtered_velocity_rad_s,
                expected_filtered,
                feedback.tracked_velocity_rad_s,
                expected_tracker_velocity,
                feedback.quality
            );
            return 1;
        }
        previous_code = raw_code;
        max_tracker_error = fmax(
            max_tracker_error,
            fabs((double)feedback.tracked_velocity_rad_s - expected_tracker_velocity)
        );
        lpf_velocity = expected_filtered;
        tracker_position = expected_tracker_position;
        tracker_velocity = expected_tracker_velocity;
        velocity_seen = 1;
        tracker_seen = 1;
    }

    if (foc_encoder_update(
            &context,
            5003000U,
            previous_code,
            FOC_ENCODER_TRANSPORT_OK,
            &feedback
        ) != FOC_ENCODER_STATUS_ERR_DISCONTINUITY ||
        !feedback.continuity_lost || context.position_counts != position_counts) {
        fprintf(stderr, "encoder timestamp rejection invariant failed\n");
        return 1;
    }
    printf(
        "test_foc_sensor_reference passed samples=5000 max_tracker_error=%.9g wrap=1 lpf=1 "
        "tracker=1 rejection=1\n",
        max_tracker_error
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
