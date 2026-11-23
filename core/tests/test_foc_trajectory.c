/**
 * @file    test_foc_trajectory.c
 * @brief   Validate bounded Float32 velocity and position references.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Test role
 * ---------
 * Verify speed limits, position braking, lifecycle failures, multi-turn
 * coordinates, independent instances, and disabled-path compatibility.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_trajectory.h"

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

static struct foc_trajectory_config config(void)
{
    return (struct foc_trajectory_config){
        .enabled = true,
        .max_velocity_rad_s = 4.0f,
        .max_accel_rad_s2 = 2.0f,
        .max_dt_s = 0.25f,
        .position_tolerance_rad = 0.01f,
    };
}

static int near(float actual, float expected, float tolerance)
{
    return fabsf(actual - expected) <= tolerance;
}

static int test_velocity_limits(void)
{
    struct foc_trajectory_context context = {0};
    struct foc_trajectory_output output;
    const struct foc_trajectory_config settings = config();
    TEST_ASSERT(foc_trajectory_init(&context, &settings) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(foc_trajectory_reset(&context, 0.0f) == FOC_TRAJECTORY_STATUS_OK);

    for (unsigned int index = 0U; index < 30U; ++index) {
        const float previous = context.planned_velocity_rad_s;
        TEST_ASSERT(
            foc_trajectory_velocity_step(&context, 20.0f, 0.1f, &output) == FOC_TRAJECTORY_STATUS_OK
        );
        TEST_ASSERT(output.active);
        TEST_ASSERT(fabsf(output.velocity_ref_rad_s) <= settings.max_velocity_rad_s + 1.0e-6f);
        TEST_ASSERT(
            fabsf(output.velocity_ref_rad_s - previous) <=
            settings.max_accel_rad_s2 * 0.1f + 1.0e-6f
        );
    }
    TEST_ASSERT(near(context.planned_velocity_rad_s, 4.0f, 1.0e-6f));

    TEST_ASSERT(
        foc_trajectory_velocity_step(&context, -20.0f, 0.1f, &output) == FOC_TRAJECTORY_STATUS_OK
    );
    TEST_ASSERT(near(output.velocity_ref_rad_s, 3.8f, 1.0e-6f));
    for (unsigned int index = 0U; index < 40U; ++index) {
        const float previous = context.planned_velocity_rad_s;
        TEST_ASSERT(
            foc_trajectory_velocity_step(&context, -20.0f, 0.1f, &output) ==
            FOC_TRAJECTORY_STATUS_OK
        );
        TEST_ASSERT(fabsf(output.velocity_ref_rad_s - previous) <= 0.200001f);
    }
    TEST_ASSERT(near(context.planned_velocity_rad_s, -4.0f, 1.0e-6f));
    return 0;
}

static int test_position_braking_and_multiturn(void)
{
    struct foc_trajectory_context context = {0};
    struct foc_trajectory_output output;
    const struct foc_trajectory_config settings = config();
    float position = 0.0f;
    float maximum_velocity = 0.0f;
    float maximum_accel = 0.0f;
    float previous_velocity = 0.0f;

    TEST_ASSERT(foc_trajectory_init(&context, &settings) == FOC_TRAJECTORY_STATUS_OK);
    for (unsigned int index = 0U; index < 3000U; ++index) {
        TEST_ASSERT(
            foc_trajectory_position_step(&context, 6.0f, position, 0.01f, &output) ==
            FOC_TRAJECTORY_STATUS_OK
        );
        maximum_velocity = fmaxf(maximum_velocity, fabsf(output.velocity_ref_rad_s));
        maximum_accel =
            fmaxf(maximum_accel, fabsf(output.velocity_ref_rad_s - previous_velocity) / 0.01f);
        previous_velocity = output.velocity_ref_rad_s;
        position += output.velocity_ref_rad_s * 0.01f;
    }
    TEST_ASSERT(maximum_velocity <= settings.max_velocity_rad_s + 1.0e-5f);
    TEST_ASSERT(maximum_accel <= settings.max_accel_rad_s2 + 1.0e-4f);
    TEST_ASSERT(fabsf(position - 6.0f) <= 0.08f);
    TEST_ASSERT(fabsf(context.planned_velocity_rad_s) <= 0.1f);

    TEST_ASSERT(foc_trajectory_reset(&context, 0.0f) == FOC_TRAJECTORY_STATUS_OK);
    position = 6.0f;
    previous_velocity = 0.0f;
    for (unsigned int index = 0U; index < 1800U; ++index) {
        TEST_ASSERT(
            foc_trajectory_position_step(&context, -3.0f, position, 0.01f, &output) ==
            FOC_TRAJECTORY_STATUS_OK
        );
        TEST_ASSERT(fabsf(output.velocity_ref_rad_s - previous_velocity) <= 0.020001f);
        previous_velocity = output.velocity_ref_rad_s;
        position += output.velocity_ref_rad_s * 0.01f;
    }
    TEST_ASSERT(fabsf(position + 3.0f) <= 0.08f);
    TEST_ASSERT(fabsf(context.planned_velocity_rad_s) <= 0.1f);
    return 0;
}

static int test_invalid_and_lifecycle(void)
{
    struct foc_trajectory_context context = {0};
    struct foc_trajectory_context second = {0};
    struct foc_trajectory_output output = {.velocity_ref_rad_s = 99.0f, .active = true};
    struct foc_trajectory_config invalid = config();
    float before;
    const struct foc_trajectory_config settings = config();

    TEST_ASSERT(
        foc_trajectory_velocity_step(&context, 1.0f, 0.1f, &output) ==
        FOC_TRAJECTORY_STATUS_ERR_STATE
    );
    TEST_ASSERT(output.velocity_ref_rad_s == 0.0f && !output.active);
    invalid.max_accel_rad_s2 = 0.0f;
    TEST_ASSERT(foc_trajectory_init(&context, &invalid) == FOC_TRAJECTORY_STATUS_ERR_CONFIG);
    TEST_ASSERT(foc_trajectory_init(&context, &settings) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(foc_trajectory_init(&second, &settings) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(foc_trajectory_reset(&context, 0.0f) == FOC_TRAJECTORY_STATUS_OK);
    before = context.planned_velocity_rad_s;
    TEST_ASSERT(
        foc_trajectory_velocity_step(&context, NAN, 0.1f, &output) ==
        FOC_TRAJECTORY_STATUS_ERR_NONFINITE
    );
    TEST_ASSERT(context.planned_velocity_rad_s == before);
    TEST_ASSERT(
        foc_trajectory_velocity_step(&context, 1.0f, 0.0f, &output) ==
        FOC_TRAJECTORY_STATUS_ERR_INVALID_DT
    );
    TEST_ASSERT(
        foc_trajectory_position_step(&context, INFINITY, 0.0f, 0.1f, &output) ==
        FOC_TRAJECTORY_STATUS_ERR_NONFINITE
    );
    TEST_ASSERT(foc_trajectory_invalidate(&context) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(
        foc_trajectory_velocity_step(&context, 1.0f, 0.1f, &output) ==
        FOC_TRAJECTORY_STATUS_ERR_STATE
    );
    TEST_ASSERT(foc_trajectory_reset(&context, 0.0f) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(foc_trajectory_reset(&context, NAN) == FOC_TRAJECTORY_STATUS_ERR_NONFINITE);
    TEST_ASSERT(second.planned_velocity_rad_s == 0.0f);
    return 0;
}

static int test_disabled_compatibility(void)
{
    struct foc_trajectory_context context = {0};
    struct foc_trajectory_output output;
    const struct foc_trajectory_config settings = {
        .enabled = false,
        .max_velocity_rad_s = 0.0f,
        .max_accel_rad_s2 = 0.0f,
        .max_dt_s = 0.0f,
        .position_tolerance_rad = 0.0f,
    };

    TEST_ASSERT(foc_trajectory_init(&context, &settings) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(
        foc_trajectory_velocity_step(&context, 7.0f, 0.1f, &output) == FOC_TRAJECTORY_STATUS_OK
    );
    TEST_ASSERT(output.velocity_ref_rad_s == 7.0f && !output.active);
    TEST_ASSERT(context.planned_velocity_rad_s == 0.0f);
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

/* Speeding up follows the acceleration; slowing down and reversing the deceleration. */
static int test_asymmetric_velocity(void)
{
    struct foc_trajectory_config settings = config();
    settings.max_accel_rad_s2 = 2.0f;
    settings.max_decel_rad_s2 = 8.0f;
    struct foc_trajectory_context context;
    struct foc_trajectory_output output;
    TEST_ASSERT(foc_trajectory_init(&context, &settings) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(foc_trajectory_reset(&context, 0.0f) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(foc_trajectory_velocity_step(&context, 4.0f, 0.25f, &output) == 0);
    TEST_ASSERT(fabsf(output.velocity_ref_rad_s - 0.5f) < 1e-6f);
    for (int i = 0; i < 10; ++i) {
        TEST_ASSERT(foc_trajectory_velocity_step(&context, 4.0f, 0.25f, &output) == 0);
    }
    TEST_ASSERT(fabsf(output.velocity_ref_rad_s - 4.0f) < 1e-6f);
    TEST_ASSERT(foc_trajectory_velocity_step(&context, 0.0f, 0.25f, &output) == 0);
    TEST_ASSERT(fabsf(output.velocity_ref_rad_s - 2.0f) < 1e-6f);
    TEST_ASSERT(foc_trajectory_velocity_step(&context, -4.0f, 0.25f, &output) == 0);
    TEST_ASSERT(fabsf(output.velocity_ref_rad_s) < 1e-6f);
    /* From standstill the reverse direction speeds up again at the acceleration. */
    TEST_ASSERT(foc_trajectory_velocity_step(&context, -4.0f, 0.25f, &output) == 0);
    TEST_ASSERT(fabsf(output.velocity_ref_rad_s + 0.5f) < 1e-6f);
    settings.max_decel_rad_s2 = -1.0f;
    TEST_ASSERT(foc_trajectory_init(&context, &settings) == FOC_TRAJECTORY_STATUS_ERR_CONFIG);
    return 0;
}

/* The tracking planner runs a trapezoid on its own reference and stops at the target. */
static int test_tracking_profile(void)
{
    struct foc_trajectory_config settings = config();
    settings.max_velocity_rad_s = 2.0f;
    settings.max_accel_rad_s2 = 4.0f;
    settings.max_decel_rad_s2 = 8.0f;
    settings.max_dt_s = 0.002f;
    struct foc_trajectory_context context;
    struct foc_trajectory_output output;
    TEST_ASSERT(foc_trajectory_init(&context, &settings) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(foc_trajectory_reset(&context, 0.0f) == FOC_TRAJECTORY_STATUS_OK);
    /* The first step seeds from the measurement; later measurements are ignored. */
    TEST_ASSERT(foc_trajectory_track_step(&context, 10.0f, 7.0f, 0.001f, &output) == 0);
    TEST_ASSERT(output.position_valid && fabsf(output.position_ref_rad - 7.0f) < 1e-5f);
    float previous = output.velocity_ref_rad_s;
    float peak = 0.0f;
    float max_rise = 0.0f;
    float max_fall = 0.0f;
    float last_position = output.position_ref_rad;
    int steps = 0;
    while (steps < 10000) {
        TEST_ASSERT(foc_trajectory_track_step(&context, 10.0f, -100.0f, 0.001f, &output) == 0);
        const float change = (output.velocity_ref_rad_s - previous) / 0.001f;
        max_rise = fmaxf(max_rise, change);
        max_fall = fmaxf(max_fall, -change);
        peak = fmaxf(peak, output.velocity_ref_rad_s);
        TEST_ASSERT(output.position_ref_rad <= 10.0f + 1e-4f);
        TEST_ASSERT(output.position_ref_rad >= last_position - 1e-4f);
        last_position = output.position_ref_rad;
        previous = output.velocity_ref_rad_s;
        ++steps;
        if (output.position_ref_rad == 10.0f && output.velocity_ref_rad_s == 0.0f) {
            break;
        }
    }
    TEST_ASSERT(output.position_ref_rad == 10.0f && output.velocity_ref_rad_s == 0.0f);
    TEST_ASSERT(fabsf(peak - 2.0f) < 1e-4f);
    TEST_ASSERT(max_rise <= 4.0f + 1e-2f && max_fall <= 8.0f + 1e-2f);
    /* Trapezoid time: 0.5 s up, 0.25 s down, (3 - 0.5 - 0.25) / 2 = 1.125 s cruise. */
    TEST_ASSERT(steps > 1850 && steps < 1900);
    /* A target change mid-move reverses through the deceleration. */
    TEST_ASSERT(foc_trajectory_reset(&context, 0.0f) == FOC_TRAJECTORY_STATUS_OK);
    for (int i = 0; i < 600; ++i) {
        TEST_ASSERT(foc_trajectory_track_step(&context, 5.0f, 0.0f, 0.001f, &output) == 0);
    }
    TEST_ASSERT(output.velocity_ref_rad_s > 1.9f);
    previous = output.velocity_ref_rad_s;
    for (int i = 0; i < 4000; ++i) {
        TEST_ASSERT(foc_trajectory_track_step(&context, -1.0f, 0.0f, 0.001f, &output) == 0);
        TEST_ASSERT(fabsf(output.velocity_ref_rad_s - previous) <= 8.0f * 0.001f + 1e-5f);
        previous = output.velocity_ref_rad_s;
    }
    TEST_ASSERT(output.position_ref_rad == -1.0f && output.velocity_ref_rad_s == 0.0f);
    /* A disabled planner refuses tracking. */
    settings.enabled = false;
    TEST_ASSERT(foc_trajectory_init(&context, &settings) == FOC_TRAJECTORY_STATUS_OK);
    TEST_ASSERT(
        foc_trajectory_track_step(&context, 1.0f, 0.0f, 0.001f, &output) ==
        FOC_TRAJECTORY_STATUS_ERR_CONFIG
    );
    return 0;
}

int main(void)
{
    return test_velocity_limits() || test_position_braking_and_multiturn() ||
        test_invalid_and_lifecycle() || test_disabled_compatibility() ||
        test_asymmetric_velocity() || test_tracking_profile();
}

/* ---------------------------------------- end of file ----------------------------------------- */
