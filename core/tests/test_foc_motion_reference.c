/**
 * @file    test_foc_motion_reference.c
 * @brief   Independent double references for motion and trajectory policies.
 * @version v0.1.0
 * @date    2026.11.14
 *
 * Design overview
 * ---------------
 * Recompute rotor mapping, prediction, velocity PI, position P, and trajectory bounds
 * from their equations. Production functions are only used to obtain the
 * result under test.
 *
 * Test role
 * ---------
 * Cover deterministic random finite inputs and explicit saturation cases.
 *
 * Implementation role
 * -------------------
 * Keep the reference arithmetic in double precision and independent helper
 * functions, with fixed seeds and reproducible failure indices.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_motion.h"
#include "foc_trajectory.h"

#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Private Constants ------------------------------------------------------------ */

_Static_assert(
    FLT_RADIX == 2 && FLT_MANT_DIG == 24 && DBL_MANT_DIG >= 53,
    "Motion numeric regression requires binary32 float and binary64-or-wider double"
);

static const double REF_PI = 3.14159265358979323846;
static const double REF_TWO_PI = 6.28318530717958647692;
static const double PREDICTION_ERROR_BUDGET_RAD = 2.0e-6;

/* Private Types ---------------------------------------------------------------- */

struct prediction_reference_stats {
    unsigned int cases;
    double maximum_error_rad;
    struct foc_electrical_state worst;
    uint32_t worst_age_us;
};

/* Private Helpers -------------------------------------------------------------- */

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static float random_float(uint32_t *state, float low, float high)
{
    const double unit = (double)(next_random(state) & 0x00ffffffU) / 16777215.0;
    return (float)((double)low + ((double)high - (double)low) * unit);
}

static double wrap_angle(double value)
{
    double wrapped = fmod(value, REF_TWO_PI);
    if (wrapped < 0.0) {
        wrapped += REF_TWO_PI;
    }
    return wrapped;
}

static double clamp_double(double value, double limit)
{
    return fmax(-limit, fmin(value, limit));
}

static int close_enough(double actual, double expected, double tolerance)
{
    const double scale = fmax(1.0, fmax(fabs(actual), fabs(expected)));
    return fabs(actual - expected) <= tolerance * scale;
}

static int fail_value(const char *name, unsigned int index, double actual, double expected)
{
    fprintf(
        stderr,
        "%s mismatch index=%u actual=%.9g expected=%.9g\n",
        name,
        index,
        actual,
        expected
    );
    return 1;
}

static int check_prediction(
    float theta,
    float omega,
    uint32_t age_us,
    struct prediction_reference_stats *stats
)
{
    /* Board default model policy; reference time and phase never use Float32 math. */
    const struct foc_prediction_config config = {650U, 2000.0f, 0.00065f};
    const float tau = 6.28318530717958647692f;
    const struct foc_electrical_state sample = {theta, omega};
    const double dt_s = (double)age_us / 1000000.0;
    const double expected = wrap_angle((double)theta + (double)omega * dt_s);
    float actual = 1.0f;
    const int status = foc_rotor_predict(&config, &sample, age_us, &actual);
    const double error = fabs(remainder((double)actual - expected, REF_TWO_PI));
    if (!(theta >= 0.0f && theta < tau && fabsf(omega) <= 7000.0f && age_us <= 650U) ||
        status != FOC_STATUS_OK || !isfinite(actual) || actual < 0.0f || actual >= tau ||
        !isfinite(error) || error > PREDICTION_ERROR_BUDGET_RAD) {
        fprintf(
            stderr,
            "prediction mismatch case=%u theta=%.17g omega=%.17g age_us=%u actual=%.17g "
            "reference=%.17g circular_error=%.17g budget=%.17g model=%.17g status=%d\n",
            stats->cases,
            (double)theta,
            (double)omega,
            age_us,
            (double)actual,
            expected,
            error,
            PREDICTION_ERROR_BUDGET_RAD,
            0.5 * (double)config.max_accel_rad_s2 * dt_s * dt_s,
            status
        );
        return 1;
    }
    ++stats->cases;
    if (error > stats->maximum_error_rad) {
        stats->maximum_error_rad = error;
        stats->worst = sample;
        stats->worst_age_us = age_us;
    }
    return 0;
}

static int test_prediction_reference(void)
{
    const float tau = 6.28318530717958647692f;
    const float angles[] = {
        0.0f,
        nextafterf(0.0f, 1.0f),
        0.02f,
        0.35820722579956055f,
        (float)REF_PI,
        nextafterf(tau, 0.0f)
    };
    const float speeds[] = {
        -7000.0f,
        -4000.0f,
        -1000.0f,
        -10.0f,
        -1.0f,
        -1.0e-6f,
        0.0f,
        1.0e-6f,
        1.0f,
        10.0f,
        1000.0f,
        4000.0f,
        7000.0f
    };
    struct prediction_reference_stats stats = {0};
    for (unsigned angle = 0U; angle < sizeof(angles) / sizeof(angles[0]); ++angle) {
        for (unsigned speed = 0U; speed < sizeof(speeds) / sizeof(speeds[0]); ++speed) {
            for (uint32_t age = 0U; age <= 650U; ++age) {
                if (check_prediction(angles[angle], speeds[speed], age, &stats) != 0) {
                    return 1;
                }
            }
        }
    }
    /* Place samples on both sides of a predicted cycle boundary at every age. */
    for (int direction = -1; direction <= 1; direction += 2) {
        const float speed = (float)direction * 7000.0f;
        for (uint32_t age = 0U; age <= 650U; ++age) {
            const float boundary = (float)wrap_angle(-(double)speed * (double)age / 1000000.0);
            const float adjacent[] =
                {boundary, nextafterf(boundary, 0.0f), nextafterf(boundary, tau)};
            for (unsigned i = 0U; i < 3U; ++i) {
                if (check_prediction(adjacent[i], speed, age, &stats) != 0) {
                    return 1;
                }
            }
        }
    }
    uint32_t seed = UINT32_C(0x0c012026);
    for (unsigned int i = 0U; i < 20000U; ++i) {
        const float theta = random_float(&seed, 0.0f, nextafterf(tau, 0.0f));
        const float omega = random_float(&seed, -7000.0f, 7000.0f);
        const uint32_t age = next_random(&seed) % 651U;
        if (check_prediction(theta, omega, age, &stats) != 0) {
            return 1;
        }
    }
    printf(
        "prediction_nominal cases=%u ages=0..650 max_error=%.17g budget=%.17g "
        "theta=%.17g omega=%.17g age_us=%u seed=0x0c012026\n",
        stats.cases,
        stats.maximum_error_rad,
        PREDICTION_ERROR_BUDGET_RAD,
        (double)stats.worst.angle_rad,
        (double)stats.worst.velocity_rad_s,
        stats.worst_age_us
    );

    /* Observe a documented domain limit; do not require inaccurate behavior to persist. */
    const struct foc_prediction_config policy = {400U, 2000.0f, 0.0003f};
    const struct foc_electrical_state high_speed = {0.35820722579956055f, 65535000.0f};
    float actual = 0.0f;
    const int status = foc_rotor_predict(&policy, &high_speed, 400U, &actual);
    const double expected = wrap_angle(
        (double)high_speed.angle_rad + (double)high_speed.velocity_rad_s * (400.0 / 1000000.0)
    );
    const double error = fabs(remainder((double)actual - expected, REF_TWO_PI));
    printf(
        "prediction_outside_nominal theta=%.17g omega=%.17g age_us=400 actual=%.17g "
        "reference=%.17g circular_error=%.17g model=0.00016 nominal_budget_applies=0 status=%d\n",
        (double)high_speed.angle_rad,
        (double)high_speed.velocity_rad_s,
        (double)actual,
        expected,
        error,
        status
    );
    return status == FOC_STATUS_OK && isfinite(actual) && actual >= 0.0f && actual < tau &&
            isfinite(error)
        ? 0
        : 1;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    if (test_prediction_reference() != 0) {
        return 1;
    }
    uint32_t seed = 0x5a17c0deU;
    struct foc_rotor_config rotor = {.pole_pairs = 7U, .electrical_offset_rad = 0.37f};
    struct foc_velocity_config velocity_config = {
        .kp_a_per_rad_s = 0.8f,
        .ki_a_per_rad = 2.4f,
        .max_dt_s = 0.02f,
    };
    struct foc_velocity_context velocity;
    struct foc_trajectory_config trajectory_config = {
        .enabled = true,
        .max_velocity_rad_s = 12.0f,
        .max_accel_rad_s2 = 30.0f,
        .max_dt_s = 0.02f,
        .position_tolerance_rad = 0.001f,
    };
    struct foc_trajectory_context trajectory;

    if (foc_velocity_init(&velocity, &velocity_config) != FOC_STATUS_OK ||
        foc_trajectory_init(&trajectory, &trajectory_config) != FOC_TRAJECTORY_STATUS_OK) {
        fprintf(stderr, "reference initialization failed\n");
        return 1;
    }

    for (unsigned int index = 0U; index < 5000U; ++index) {
        const float mechanical = random_float(&seed, -80.0f, 80.0f);
        const float mechanical_speed = random_float(&seed, -20.0f, 20.0f);
        const float dt = random_float(&seed, 0.0001f, 0.02f);
        const float reference_speed = random_float(&seed, -15.0f, 15.0f);
        const float measured_speed = random_float(&seed, -15.0f, 15.0f);
        const float iq_limit = random_float(&seed, 0.2f, 4.0f);
        const float target_velocity = random_float(&seed, -15.0f, 15.0f);
        const float measured_position = random_float(&seed, -80.0f, 80.0f);
        const float target_position = random_float(&seed, -80.0f, 80.0f);
        struct foc_electrical_state electrical;
        struct foc_velocity_input velocity_input;
        struct foc_velocity_output velocity_output;
        struct foc_position_config position_config = {
            .kp_per_s = 2.0f,
            .velocity_limit_rad_s = 12.0f
        };
        struct foc_position_output position_output;
        struct foc_trajectory_output trajectory_output;
        const double expected_angle =
            wrap_angle(7.0 * wrap_angle((double)mechanical) + (double)rotor.electrical_offset_rad);
        const double expected_electrical_speed = 7.0 * (double)mechanical_speed;
        const double error = (double)reference_speed - (double)measured_speed;
        const double previous_integral =
            clamp_double((double)velocity.integral_a, (double)iq_limit);
        const double trial =
            previous_integral + (double)velocity_config.ki_a_per_rad * (double)dt * error;
        const double proportional = (double)velocity_config.kp_a_per_rad_s * error;
        double expected_integral = trial;
        if (proportional + trial > iq_limit && trial > previous_integral) {
            expected_integral = fmax(previous_integral, (double)iq_limit - proportional);
        } else if (proportional + trial < -iq_limit && trial < previous_integral) {
            expected_integral = fmin(previous_integral, -(double)iq_limit - proportional);
        }
        expected_integral = clamp_double(expected_integral, iq_limit);
        const double expected_iq = clamp_double(proportional + expected_integral, iq_limit);
        const double expected_position = clamp_double(
            (double)position_config.kp_per_s *
                ((double)target_position - (double)measured_position),
            position_config.velocity_limit_rad_s
        );

        if (foc_rotor_electrical(&rotor, mechanical, mechanical_speed, &electrical) !=
                FOC_STATUS_OK ||
            !close_enough(electrical.angle_rad, expected_angle, 2.0e-5) ||
            !close_enough(electrical.velocity_rad_s, expected_electrical_speed, 2.0e-5)) {
            fprintf(stderr, "rotor reference mismatch index=%u\n", index);
            return 1;
        }
        velocity_input = (struct foc_velocity_input){
            .ref_rad_s = reference_speed,
            .measured_rad_s = measured_speed,
            .dt_s = dt,
            .iq_limit_a = iq_limit,
            .enabled = true,
        };
        if (foc_velocity_step(&velocity, &velocity_input, &velocity_output) != FOC_STATUS_OK ||
            !close_enough(velocity_output.iq_ref_a, expected_iq, 5.0e-5) ||
            !close_enough(velocity.integral_a, expected_integral, 5.0e-5)) {
            fprintf(stderr, "velocity reference mismatch index=%u\n", index);
            return 1;
        }
        if (foc_position_step(
                &position_config,
                target_position,
                measured_position,
                &position_output
            ) != FOC_STATUS_OK ||
            !close_enough(position_output.velocity_ref_rad_s, expected_position, 5.0e-5)) {
            fprintf(stderr, "position reference mismatch index=%u\n", index);
            return 1;
        }
        if (foc_trajectory_velocity_step(&trajectory, target_velocity, dt, &trajectory_output) !=
            FOC_TRAJECTORY_STATUS_OK) {
            fprintf(stderr, "trajectory velocity status mismatch index=%u\n", index);
            return 1;
        }
        if (fabs((double)trajectory_output.velocity_ref_rad_s) > 12.0001) {
            fprintf(stderr, "trajectory velocity bound mismatch index=%u\n", index);
            return 1;
        }
        if (foc_trajectory_position_step(
                &trajectory,
                target_position,
                measured_position,
                dt,
                &trajectory_output
            ) != FOC_TRAJECTORY_STATUS_OK ||
            fabs((double)trajectory_output.velocity_ref_rad_s) > 12.0001) {
            fprintf(stderr, "trajectory position bound mismatch index=%u\n", index);
            return 1;
        }
    }
    puts("test_foc_motion_reference passed samples=5000 rotor=velocity=position=trajectory");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
