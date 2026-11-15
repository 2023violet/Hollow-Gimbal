/**
 * @file    test_foc_motion_reference.c
 * @brief   Independent double references for motion and trajectory policies.
 * @version v0.1.0
 * @date    2026.11.14
 *
 * Design overview
 * ---------------
 * Recompute rotor mapping, velocity PI, position P, and trajectory bounds
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

#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Private Constants ------------------------------------------------------------ */

static const double REF_PI = 3.14159265358979323846;
static const double REF_TWO_PI = 6.28318530717958647692;

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

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
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
