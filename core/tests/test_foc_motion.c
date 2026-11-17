/**
 * @file    test_foc_motion.c
 * @brief   Verify independent rotor coordinates, angle prediction and outer loops.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Exercise numerical limits and failure atomicity without Runtime or hardware.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_motion.h"

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

static int near(float actual, float expected)
{
    return fabsf(actual - expected) < 2.0e-5f;
}

static int test_coordinates(void)
{
    const float tau = 6.28318530717958647692f;
    const struct foc_rotor_config first = {3U, 0.5f};
    const struct foc_rotor_config second = {5U, -0.5f};
    struct foc_electrical_state sample;
    TEST_ASSERT(foc_rotor_electrical(&first, 0.0f, 2.0f, &sample) == FOC_STATUS_OK);
    TEST_ASSERT(near(sample.angle_rad, 0.5f) && near(sample.velocity_rad_s, 6.0f));
    TEST_ASSERT(foc_rotor_electrical(&first, -0.25f, -2.0f, &sample) == FOC_STATUS_OK);
    TEST_ASSERT(near(sample.angle_rad, tau - 0.25f) && near(sample.velocity_rad_s, -6.0f));
    TEST_ASSERT(foc_rotor_electrical(&first, tau + 0.25f, 2.0f, &sample) == FOC_STATUS_OK);
    TEST_ASSERT(near(sample.angle_rad, 1.25f));
    TEST_ASSERT(foc_rotor_electrical(&second, 0.25f, -2.0f, &sample) == FOC_STATUS_OK);
    TEST_ASSERT(near(sample.angle_rad, 0.75f) && near(sample.velocity_rad_s, -10.0f));
    TEST_ASSERT(foc_rotor_electrical(&first, NAN, 0.0f, &sample) == FOC_STATUS_ERR_NONFINITE);
    TEST_ASSERT(foc_rotor_electrical(&first, 0.0f, INFINITY, &sample) == FOC_STATUS_ERR_NONFINITE);
    TEST_ASSERT(foc_rotor_electrical(&first, 0.0f, FLT_MAX, &sample) == FOC_STATUS_ERR_NONFINITE);
    return 0;
}

static int test_prediction(void)
{
    const float tau = 6.28318530717958647692f;
    const struct foc_prediction_config policy = {10000U, 200.0f, 0.004f};
    const struct foc_electrical_state sample = {tau - 0.02f, 10.0f};
    float angle = 0.0f;
    TEST_ASSERT(foc_rotor_predict(&policy, &sample, 0U, &angle) == FOC_STATUS_OK);
    TEST_ASSERT(near(angle, sample.angle_rad));
    for (uint32_t us = 1000U; us <= 6000U; us += 1000U) {
        const float dt = (float)us * 1.0e-6f;
        TEST_ASSERT(foc_rotor_predict(&policy, &sample, us, &angle) == FOC_STATUS_OK);
        const float truth = fmodf(sample.angle_rad + sample.velocity_rad_s * dt, tau);
        TEST_ASSERT(near(angle, truth));
        const float error_bound = 100.0f * dt * dt;
        const float accelerated =
            fmodf(sample.angle_rad + sample.velocity_rad_s * dt + error_bound, tau);
        TEST_ASSERT(fabsf(remainderf(accelerated - angle, tau)) <= error_bound + 2.0e-6f);
    }
    angle = 1.0f;
    TEST_ASSERT(foc_rotor_predict(&policy, &sample, 7000U, &angle) == FOC_STATUS_ERR_PREDICTION);
    TEST_ASSERT(angle == 0.0f);
    angle = 1.0f;
    TEST_ASSERT(foc_rotor_predict(&policy, &sample, 10001U, &angle) == FOC_STATUS_ERR_INVALID_DT);
    TEST_ASSERT(angle == 0.0f);
    const struct foc_electrical_state reverse = {0.02f, -10.0f};
    TEST_ASSERT(foc_rotor_predict(&policy, &reverse, 4000U, &angle) == FOC_STATUS_OK);
    TEST_ASSERT(near(angle, tau - 0.02f));
    const struct foc_electrical_state invalid = {0.0f, NAN};
    angle = 1.0f;
    TEST_ASSERT(foc_rotor_predict(&policy, &invalid, 0U, &angle) == FOC_STATUS_ERR_NONFINITE);
    TEST_ASSERT(angle == 0.0f);
    return 0;
}

static int test_velocity(void)
{
    struct foc_velocity_context velocity = {0};
    struct foc_velocity_context other = {0};
    const struct foc_velocity_config pi = {2.0f, 4.0f, 0.25f};
    const struct foc_velocity_config p = {2.0f, 0.0f, 0.25f};
    struct foc_velocity_input input = {1.0f, 0.0f, 0.125f, 4.0f, true};
    struct foc_velocity_output output;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_ERR_NOT_INITIALIZED);
    TEST_ASSERT(foc_velocity_init(&velocity, &pi) == FOC_STATUS_OK);
    TEST_ASSERT(foc_velocity_init(&other, &p) == FOC_STATUS_OK);
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(output.iq_ref_a, 2.5f) && !output.saturated && output.enabled);
    input.dt_s = 0.25f;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(velocity.integral_a, 1.5f));
    input.ref_rad_s = -1.0f;
    TEST_ASSERT(foc_velocity_step(&other, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(output.iq_ref_a, -2.0f) && other.integral_a == 0.0f);
    TEST_ASSERT(foc_velocity_reset(&velocity) == FOC_STATUS_OK && velocity.integral_a == 0.0f);

    const struct foc_velocity_config i = {0.0f, 4.0f, 0.25f};
    TEST_ASSERT(foc_velocity_init(&velocity, &i) == FOC_STATUS_OK);
    input.ref_rad_s = 1.0f;
    for (unsigned int tick = 0U; tick < 1000U; ++tick) {
        TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
    }
    TEST_ASSERT(output.saturated && output.iq_ref_a == 4.0f && velocity.integral_a == 4.0f);
    input.iq_limit_a = 0.5f;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(velocity.integral_a == 0.5f && output.iq_ref_a == 0.5f);
    input.enabled = false;
    input.ref_rad_s = -1.0f;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(!output.enabled && output.iq_ref_a == 0.0f && velocity.integral_a == 0.5f);
    input.enabled = true;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(velocity.integral_a == -0.5f && output.iq_ref_a == -0.5f);
    input.ref_rad_s = 1.0f;
    input.dt_s = 0.05f;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(output.iq_ref_a, -0.3f) && !output.saturated);
    input.dt_s = 0.0f;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_ERR_INVALID_DT);
    input.dt_s = NAN;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_ERR_INVALID_DT);
    input.dt_s = 0.25f;
    input.measured_rad_s = NAN;
    TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_ERR_NONFINITE);
    TEST_ASSERT(!output.enabled && near(velocity.integral_a, -0.3f));
    const struct foc_velocity_config invalid = {-1.0f, 0.0f, 1.0f};
    TEST_ASSERT(foc_velocity_init(&velocity, &invalid) == FOC_STATUS_ERR_INVALID_CONFIG);
    TEST_ASSERT(near(velocity.integral_a, -0.3f));
    return 0;
}

static int velocity_state_equal(
    const struct foc_velocity_context *actual,
    const struct foc_velocity_context *expected
)
{
    return actual->integral_a == expected->integral_a &&
        actual->initialized == expected->initialized &&
        actual->config.kp_a_per_rad_s == expected->config.kp_a_per_rad_s &&
        actual->config.ki_a_per_rad == expected->config.ki_a_per_rad &&
        actual->config.max_dt_s == expected->config.max_dt_s;
}

static int velocity_output_cleared(const struct foc_velocity_output *output)
{
    return output->iq_ref_a == 0.0f && !output->saturated && !output->enabled;
}

static int test_velocity_error_atomicity(void)
{
    /* First overflow the integral trial, then the requested P+I with a finite trial. */
    const struct foc_velocity_config policies[] = {{0.0f, FLT_MAX, 1.0f}, {FLT_MAX, 1.0f, 1.0f}};
    const float references[] = {2.0f, -2.0f};
    const struct foc_velocity_output poison = {0.75f, true, true};
    for (unsigned policy = 0U; policy < 2U; ++policy) {
        for (unsigned direction = 0U; direction < 2U; ++direction) {
            struct foc_velocity_context velocity = {0};
            TEST_ASSERT(foc_velocity_init(&velocity, &policies[policy]) == FOC_STATUS_OK);
            velocity.integral_a = 0.25f;
            const struct foc_velocity_context before = velocity;
            struct foc_velocity_input input = {references[direction], 0.0f, 1.0f, 2.0f, true};
            struct foc_velocity_output output = poison;
            const int status = foc_velocity_step(&velocity, &input, &output);
            printf(
                "velocity_atomicity policy=%u ref=%.9g status=%d integral=%.9g expected=0.25\n",
                policy,
                (double)input.ref_rad_s,
                status,
                (double)velocity.integral_a
            );
            TEST_ASSERT(status == FOC_STATUS_ERR_NONFINITE);
            TEST_ASSERT(velocity_state_equal(&velocity, &before));
            TEST_ASSERT(velocity_output_cleared(&output));

            input.enabled = false;
            output = poison;
            TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
            TEST_ASSERT(velocity_state_equal(&velocity, &before));
            TEST_ASSERT(velocity_output_cleared(&output));

            input.enabled = true;
            input.ref_rad_s = NAN;
            output = poison;
            TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_ERR_NONFINITE);
            TEST_ASSERT(velocity_state_equal(&velocity, &before));
            TEST_ASSERT(velocity_output_cleared(&output));

            input.ref_rad_s = 0.0f;
            input.dt_s = 0.0f;
            output = poison;
            TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_ERR_INVALID_DT);
            TEST_ASSERT(velocity_state_equal(&velocity, &before));
            TEST_ASSERT(velocity_output_cleared(&output));

            /* Recover on this same instance without reset/reinitialization. */
            input.dt_s = 1.0f;
            output = poison;
            TEST_ASSERT(foc_velocity_step(&velocity, &input, &output) == FOC_STATUS_OK);
            TEST_ASSERT(velocity_state_equal(&velocity, &before));
            TEST_ASSERT(output.enabled && !output.saturated && output.iq_ref_a == 0.25f);
        }
    }
    return 0;
}

static int test_position(void)
{
    const struct foc_position_config config = {2.0f, 3.0f};
    struct foc_position_output output;
    TEST_ASSERT(foc_position_step(&config, 13.0f, 12.5f, &output) == FOC_STATUS_OK);
    TEST_ASSERT(near(output.velocity_ref_rad_s, 1.0f) && !output.limited);
    TEST_ASSERT(foc_position_step(&config, -13.0f, 13.0f, &output) == FOC_STATUS_OK);
    TEST_ASSERT(output.velocity_ref_rad_s == -3.0f && output.limited);
    TEST_ASSERT(foc_position_step(&config, 20.0f, -12.0f, &output) == FOC_STATUS_OK);
    TEST_ASSERT(output.velocity_ref_rad_s == 3.0f && output.limited);
    TEST_ASSERT(foc_position_step(&config, -13.0f, -13.0f, &output) == FOC_STATUS_OK);
    TEST_ASSERT(output.velocity_ref_rad_s == 0.0f && !output.limited);
    TEST_ASSERT(foc_position_step(&config, 0.0f, INFINITY, &output) == FOC_STATUS_ERR_NONFINITE);
    TEST_ASSERT(output.velocity_ref_rad_s == 0.0f);
    return 0;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    return test_coordinates() || test_prediction() || test_velocity() ||
        test_velocity_error_atomicity() || test_position();
}

/* ---------------------------------------- end of file ----------------------------------------- */
