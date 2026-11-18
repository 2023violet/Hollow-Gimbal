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
#include <string.h>

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

#define REF_CHECK(condition)                                                          \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "tracker reference line %d: %s\n", __LINE__, #condition); \
            return 1;                                                                 \
        }                                                                             \
    } while (0)

struct tracker_case {
    uint32_t cpr;
    enum foc_encoder_direction direction;
    bool lut;
    bool jitter;
};

struct tracker_segment {
    const char *name;
    uint32_t duration_us;
    double start_speed;
    double end_speed;
};

static const int16_t reference_knots[] = {4, -7, 5, -2};

static uint32_t reference_corrected(uint32_t raw, uint32_t cpr, bool lut)
{
    if (!lut) {
        return raw;
    }
    const double x = 4.0 * raw / cpr;
    const unsigned k = (unsigned)x;
    const double lo = reference_knots[k];
    const double hi = reference_knots[(k + 1U) % 4U];
    const int32_t rounded = (int32_t)floor(lo + (hi - lo) * (x - k) + 0.5);
    return (uint32_t)(((int32_t)raw + rounded + (int32_t)cpr) % (int32_t)cpr);
}

static int tracker_translation(const struct tracker_case *test, bool trace)
{
    static const struct tracker_segment segments[] = {
        {"quiet", 40000U, 0.0, 0.0},
        {"crawl", 300000U, 0.9, 0.9},
        {"step", 40000U, 22.0, 22.0},
        {"accel", 70000U, 22.0, 45.0},
        {"decel", 65000U, 45.0, 0.0},
        {"reverse", 60000U, -18.0, -18.0},
        {"stop", 40000U, 0.0, 0.0},
        {"restart", 60000U, 0.9, 0.9},
    };
    static const int32_t jitter[] = {0, -23, -4, -14, 10, 16};
    static const int64_t turns[] = {0, 1000000, -1000000};
    /* Fixed before the production change; the same code/timestamp cannot
     * legitimately move the local tracker by changing its integer epoch. */
    const double origin_tolerance_rad_s = 1.0e-6;
    const double oracle_abs_rad_s = 5.0e-5;
    const double oracle_relative = 2.0e-6;
    const struct foc_encoder_config config =
        {test->cpr, test->direction, 100.0f, 100U, 2000U, 80.0f, 0.85f, 0.2f};
    const struct foc_encoder_lut lut = {reference_knots, 4U, test->cpr, 71U};
    struct foc_encoder_context sensor[3];
    struct foc_encoder_feedback frames[3];
    const double orientation = test->direction;
    double start = -orientation * 0.010;
    uint32_t tick = UINT32_MAX - 900U;
    uint32_t raw = code_for_angle(orientation * start, test->cpr);
    uint32_t previous = reference_corrected(raw, test->cpr, test->lut);
    for (unsigned lane = 0U; lane < 3U; ++lane) {
        REF_CHECK(
            (test->lut ? foc_encoder_init_with_lut(&sensor[lane], &config, 71U, &lut)
                       : foc_encoder_init(&sensor[lane], &config)) == 0
        );
        REF_CHECK(
            foc_encoder_update(&sensor[lane], tick, raw, FOC_ENCODER_TRANSPORT_OK, &frames[lane]) ==
            0
        );
        REF_CHECK(!frames[lane].valid && !frames[lane].speed_valid && !sensor[lane].tracker_seen);
        sensor[lane].position_counts += turns[lane] * test->cpr;
    }
    int64_t measured_counts = 0;
    const double scale = REF_TWO_PI / test->cpr;
    double oracle_position = 0.0;
    double oracle_velocity = 0.0;
    bool oracle_seen = false;
    double worst_origin = 0.0;
    double worst_oracle_excess = 0.0;
    double worst_oracle = 0.0;
    double max_offset = 0.0;
    double max_speed = 0.0;
    unsigned repeats = 0U;
    unsigned wraps = 0U;
    for (unsigned s = 0U; s < sizeof segments / sizeof segments[0]; ++s) {
        const struct tracker_segment *phase = &segments[s];
        const double a = (phase->end_speed - phase->start_speed) * 1.0e6 / phase->duration_us;
        double error2[3] = {0};
        double peak[3] = {0};
        uint32_t elapsed = 0U;
        const unsigned samples = phase->duration_us / 500U;
        for (unsigned k = 1U; k <= samples; ++k) {
            const int32_t shift = test->jitter && k != samples ? jitter[k % 6U] : 0;
            const uint32_t next = (uint32_t)((int32_t)(500U * k) + shift);
            const uint32_t dt_us = next - elapsed;
            elapsed = next;
            tick += dt_us;
            const double t = next * 1.0e-6;
            const double speed = orientation * (phase->start_speed + a * t);
            const double angle = start + orientation * (phase->start_speed * t + 0.5 * a * t * t);
            raw = code_for_angle(orientation * angle, test->cpr);
            const uint32_t corrected = reference_corrected(raw, test->cpr, test->lut);
            int32_t delta = (int32_t)corrected - (int32_t)previous;
            if (2 * delta > (int32_t)test->cpr) {
                delta -= (int32_t)test->cpr;
                ++wraps;
            } else if (2 * delta < -(int32_t)test->cpr) {
                delta += (int32_t)test->cpr;
                ++wraps;
            }
            delta *= test->direction;
            repeats += delta == 0;
            previous = corrected;
            measured_counts += delta;
            /* Independent Double absolute position relative to the small
             * fixture origin; integrate expected directed integer increments. */
            const double measured = measured_counts * scale;
            const double h = dt_us * 1.0e-6;
            if (!oracle_seen) {
                oracle_velocity = delta * scale / h;
                oracle_position = measured;
                oracle_seen = true;
            } else {
                const double predicted = oracle_position + h * oracle_velocity;
                const double difference = measured - predicted;
                oracle_position = predicted + config.tracker_alpha * difference;
                oracle_velocity += config.tracker_beta * difference / h;
            }
            for (unsigned lane = 0U; lane < 3U; ++lane) {
                struct foc_encoder_feedback *out = &frames[lane];
                REF_CHECK(
                    foc_encoder_update(&sensor[lane], tick, raw, FOC_ENCODER_TRANSPORT_OK, out) == 0
                );
                REF_CHECK(out->valid && out->speed_valid && out->captured_us == tick);
                REF_CHECK(out->raw_code == raw && out->corrected_code == corrected);
                const uint32_t direction_phase =
                    test->direction == FOC_ENCODER_DIRECTION_NORMAL || corrected == 0U
                    ? corrected
                    : test->cpr - corrected;
                REF_CHECK(out->mechanical_phase_count == direction_phase);
                REF_CHECK(
                    sensor[lane].position_counts ==
                    sensor[0].position_counts + turns[lane] * test->cpr
                );
                REF_CHECK(out->raw_velocity_rad_s == frames[0].raw_velocity_rad_s);
                REF_CHECK(out->filtered_velocity_rad_s == frames[0].filtered_velocity_rad_s);
                const double track = out->tracked_velocity_rad_s;
                REF_CHECK(isfinite(sensor[lane].tracker_local_offset_rad));
                REF_CHECK(
                    sensor[lane].tracker_local_offset_rad == sensor[0].tracker_local_offset_rad
                );
                max_offset = fmax(max_offset, fabs(sensor[lane].tracker_local_offset_rad));
                max_speed = fmax(max_speed, fabs(track));
                const double error = track - speed;
                error2[lane] += error * error;
                peak[lane] = fmax(peak[lane], fabs(error));
                worst_origin = fmax(worst_origin, fabs(track - frames[0].tracked_velocity_rad_s));
                const double against_reference = fabs(track - oracle_velocity);
                worst_oracle = fmax(worst_oracle, against_reference);
                worst_oracle_excess = fmax(
                    worst_oracle_excess,
                    against_reference - (oracle_abs_rad_s + oracle_relative * fabs(oracle_velocity))
                );
            }
            if (trace) {
                printf(
                    "PAIR,%u,%d,%u,%u,%s,%u,%u,%u,%u,%.12g,%.12g,%.9g,%.9g,%.9g\n",
                    test->cpr,
                    test->direction,
                    (unsigned)test->lut,
                    (unsigned)test->jitter,
                    phase->name,
                    k,
                    tick,
                    raw,
                    corrected,
                    speed,
                    oracle_velocity,
                    frames[0].tracked_velocity_rad_s,
                    frames[1].tracked_velocity_rad_s,
                    frames[2].tracked_velocity_rad_s
                );
            }
        }
        start += orientation * 0.5 * (phase->start_speed + phase->end_speed) * phase->duration_us *
            1.0e-6;
        printf(
            "LOCAL,%u,%d,%u,%u,%s,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
            test->cpr,
            test->direction,
            (unsigned)test->lut,
            (unsigned)test->jitter,
            phase->name,
            samples,
            sqrt(error2[0] / samples),
            sqrt(error2[1] / samples),
            sqrt(error2[2] / samples),
            peak[0],
            peak[1],
            peak[2]
        );
    }
    printf(
        "TRANSLATION,%u,%d,%u,%u,%.9g,%.9g,%u,%u\n",
        test->cpr,
        test->direction,
        (unsigned)test->lut,
        (unsigned)test->jitter,
        worst_origin,
        worst_oracle,
        repeats,
        wraps
    );
    printf(
        "STATE,%u,%d,%u,%u,%.9g,%.9g\n",
        test->cpr,
        test->direction,
        (unsigned)test->lut,
        (unsigned)test->jitter,
        max_offset,
        max_speed
    );
    REF_CHECK(repeats != 0U && wraps != 0U);
    REF_CHECK(worst_origin <= origin_tolerance_rad_s);
    REF_CHECK(worst_oracle_excess == 0.0);
    return 0;
}

static int tracker_local_reference(bool trace)
{
    static const struct tracker_case cases[] = {
        {16384U, FOC_ENCODER_DIRECTION_NORMAL, true, true},
        {16384U, FOC_ENCODER_DIRECTION_NORMAL, false, false},
        {50001U, FOC_ENCODER_DIRECTION_REVERSED, true, true},
        {50001U, FOC_ENCODER_DIRECTION_REVERSED, false, false},
    };
    for (unsigned i = 0U; i < sizeof cases / sizeof cases[0]; ++i) {
        REF_CHECK(tracker_translation(&cases[i], trace) == 0);
    }
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(int argc, char **argv)
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
    return tracker_local_reference(argc == 2 && strcmp(argv[1], "--trace") == 0);
}

/* ---------------------------------------- end of file ----------------------------------------- */
