/**
 * @file    test_foc_motion_protection.c
 * @brief   Check continuous position admission and source-time following error.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Exercise both boundaries, disabled and invalid settings, recovery direction,
 * signed combined effort, elapsed-time continuity, and independent state.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_following_error.h"
#include "foc_motion_limits.h"

#include <float.h>
#include <math.h>
#include <stdio.h>

#define CHECK(condition)                                                           \
    do {                                                                           \
        if (!(condition)) {                                                        \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #condition); \
            return 1;                                                              \
        }                                                                          \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static int test_limits(void)
{
    const struct foc_motion_limits_config config = {
        .position_min_rad = -80.0f,
        .position_max_rad = 95.0f,
        .tolerance_rad = 0.25f,
        .mechanical_frame_id = 9U,
        .enabled = true,
    };
    CHECK(foc_motion_limits_validate(&config));
    CHECK(foc_motion_limits_reference(&config, -80.0f) == FOC_MOTION_REJECT_NONE);
    CHECK(foc_motion_limits_reference(&config, 95.0f) == FOC_MOTION_REJECT_NONE);
    CHECK(foc_motion_limits_reference(&config, 95.5f) == FOC_MOTION_REJECT_POSITION);
    CHECK(foc_motion_limits_reference(&config, -81.0f) == FOC_MOTION_REJECT_POSITION);
    CHECK(foc_motion_limits_reference(&config, NAN) == FOC_MOTION_REJECT_POSITION);

    struct foc_motion_limits_request request = {
        .position_ref_rad = 90.0f,
        .velocity_ref_rad_s = -2.0f,
        .requested_velocity_rad_s = -3.0f,
        .iq_ref_a = -1.0f,
        .has_position = true,
    };
    CHECK(foc_motion_limits_check(&config, 95.0f, &request) == FOC_MOTION_REJECT_NONE);
    CHECK(foc_motion_limits_check(&config, 100.0f, &request) == FOC_MOTION_REJECT_NONE);
    request.iq_ref_a = 0.1f;
    CHECK(foc_motion_limits_check(&config, 94.70f, &request) == FOC_MOTION_REJECT_NONE);
    CHECK(foc_motion_limits_check(&config, 94.75f, &request) == FOC_MOTION_REJECT_DIRECTION);
    CHECK(foc_motion_limits_check(&config, 100.0f, &request) == FOC_MOTION_REJECT_DIRECTION);
    request.iq_ref_a = 0.0f;
    request.velocity_ref_rad_s = 0.1f;
    CHECK(foc_motion_limits_check(&config, 95.0f, &request) == FOC_MOTION_REJECT_DIRECTION);
    request.velocity_ref_rad_s = -0.1f;
    request.requested_velocity_rad_s = 0.1f;
    CHECK(foc_motion_limits_check(&config, 95.0f, &request) == FOC_MOTION_REJECT_DIRECTION);
    request.requested_velocity_rad_s = 0.0f;
    request.position_ref_rad = 95.0f;
    CHECK(foc_motion_limits_check(&config, 94.75f, &request) == FOC_MOTION_REJECT_DIRECTION);
    CHECK(foc_motion_limits_check(&config, 95.0f, &request) == FOC_MOTION_REJECT_NONE);

    request.has_position = false;
    request.position_ref_rad = NAN;
    request.velocity_ref_rad_s = 1.0f;
    request.requested_velocity_rad_s = 1.5f;
    request.iq_ref_a = 0.05f;
    CHECK(foc_motion_limits_check(&config, -80.0f, &request) == FOC_MOTION_REJECT_NONE);
    CHECK(foc_motion_limits_check(&config, -100.0f, &request) == FOC_MOTION_REJECT_NONE);
    request.iq_ref_a = -0.05f;
    CHECK(foc_motion_limits_check(&config, -79.75f, &request) == FOC_MOTION_REJECT_DIRECTION);
    CHECK(foc_motion_limits_check(&config, -79.70f, &request) == FOC_MOTION_REJECT_NONE);
    CHECK(foc_motion_limits_check(&config, INFINITY, &request) == FOC_MOTION_REJECT_FEEDBACK);
    request.velocity_ref_rad_s = NAN;
    CHECK(foc_motion_limits_check(&config, 0.0f, &request) == FOC_MOTION_REJECT_DIRECTION);

    const struct foc_motion_limits_config disabled = {0};
    CHECK(foc_motion_limits_validate(&disabled));
    CHECK(foc_motion_limits_check(&disabled, NAN, &request) == FOC_MOTION_REJECT_NONE);
    CHECK(foc_motion_limits_reference(&disabled, INFINITY) == FOC_MOTION_REJECT_NONE);
    struct foc_motion_limits_config invalid = config;
    invalid.position_max_rad = invalid.position_min_rad;
    CHECK(!foc_motion_limits_validate(&invalid));
    CHECK(foc_motion_limits_check(&invalid, -100.0f, &request) == FOC_MOTION_REJECT_CONFIG);
    invalid.position_max_rad = invalid.position_min_rad - 1.0f;
    CHECK(!foc_motion_limits_validate(&invalid));
    invalid = config;
    invalid.tolerance_rad = 87.5f;
    CHECK(!foc_motion_limits_validate(&invalid));
    invalid.tolerance_rad = -0.1f;
    CHECK(!foc_motion_limits_validate(&invalid));
    invalid.tolerance_rad = NAN;
    CHECK(!foc_motion_limits_validate(&invalid));
    invalid = config;
    invalid.position_max_rad = INFINITY;
    CHECK(!foc_motion_limits_validate(&invalid));
    invalid.position_min_rad = -FLT_MAX;
    invalid.position_max_rad = FLT_MAX;
    CHECK(!foc_motion_limits_validate(&invalid));
    invalid = config;
    invalid.mechanical_frame_id = 0U;
    CHECK(!foc_motion_limits_validate(&invalid));
    CHECK(!foc_motion_limits_validate(NULL));
    CHECK(foc_motion_limits_check(&config, 0.0f, NULL) == FOC_MOTION_REJECT_CONFIG);

    const struct foc_motion_limits_config distant = {
        .position_min_rad = -100000.0f,
        .position_max_rad = 100000.0f,
        .tolerance_rad = 0.125f,
        .mechanical_frame_id = 9U,
        .enabled = true,
    };
    const struct foc_motion_limits_request return_inward = {
        .position_ref_rad = 99999.0f,
        .velocity_ref_rad_s = -1.0f,
        .iq_ref_a = -0.2f,
        .has_position = true,
    };
    CHECK(foc_motion_limits_check(&distant, 100001.0f, &return_inward) == FOC_MOTION_REJECT_NONE);
    return 0;
}

static int test_following_error(void)
{
    const struct foc_following_error_config config = {
        .threshold_rad = 0.5f,
        .duration_us = 1000U,
        .max_gap_us = 600U,
        .enabled = true,
    };
    struct foc_following_error_state state = {0};
    struct foc_following_error_state independent = {0};
    struct foc_following_error_input input = {
        .reference_rad = 10.0f,
        .measured_rad = 9.5f,
        .captured_us = 0U,
        .applicable = true,
        .reference_valid = true,
        .sensor_valid = true,
    };
    CHECK(foc_following_error_validate(&config));
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_TRACKING);
    input.measured_rad = 9.0f;
    input.captured_us = 100U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_PENDING);
    input.captured_us = 600U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_PENDING);
    CHECK(state.elapsed_us == 500U);
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_PENDING);
    CHECK(state.elapsed_us == 500U);
    CHECK(independent.status == FOC_FOLLOWING_INACTIVE);
    CHECK(foc_following_error_step(&config, &independent, &input) == FOC_FOLLOWING_PENDING);
    CHECK(independent.elapsed_us == 0U);
    input.captured_us = 1100U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_TRIGGERED);
    CHECK(state.elapsed_us == 1000U && independent.elapsed_us == 0U);
    input.captured_us = 1600U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_TRIGGERED);
    CHECK(state.elapsed_us == 1000U);

    input.captured_us = 1700U;
    input.measured_rad = 10.5f;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_TRACKING);
    CHECK(state.elapsed_us == 0U);
    input.captured_us = 1800U;
    input.measured_rad = 11.0f;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_PENDING);
    input.captured_us = 2401U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_PENDING);
    CHECK(state.elapsed_us == 0U);
    input.captured_us = 2400U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_TIME_ORDER);
    CHECK(state.elapsed_us == 0U && state.sample_us == 2401U);
    input.captured_us = 2900U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_PENDING);
    CHECK(state.elapsed_us == 0U);
    input.captured_us = 2900U + UINT32_C(0x80000000);
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_TIME_ORDER);

    input.captured_us = 3000U;
    input.sensor_valid = false;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_INVALID_SENSOR);
    CHECK(state.elapsed_us == 0U);
    input.sensor_valid = true;
    input.measured_rad = INFINITY;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_INVALID_SENSOR);
    input.measured_rad = 11.0f;
    input.reference_valid = false;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_NO_REFERENCE);
    input.reference_valid = true;
    input.reference_rad = NAN;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_NO_REFERENCE);
    input.reference_rad = 10.0f;
    input.applicable = false;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_INACTIVE);
    CHECK(!state.seen && state.elapsed_us == 0U);

    input.applicable = true;
    input.captured_us = UINT32_MAX - 449U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_PENDING);
    input.captured_us = 0U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_PENDING);
    CHECK(state.elapsed_us == 450U);
    input.captured_us = 550U;
    CHECK(foc_following_error_step(&config, &state, &input) == FOC_FOLLOWING_TRIGGERED);

    struct foc_following_error_config invalid = config;
    invalid.threshold_rad = -1.0f;
    CHECK(!foc_following_error_validate(&invalid));
    CHECK(foc_following_error_step(&invalid, &state, &input) == FOC_FOLLOWING_INACTIVE);
    CHECK(state.status == FOC_FOLLOWING_TRIGGERED && state.elapsed_us == 1000U);
    invalid.threshold_rad = INFINITY;
    CHECK(!foc_following_error_validate(&invalid));
    invalid = config;
    invalid.duration_us = 0U;
    CHECK(!foc_following_error_validate(&invalid));
    invalid.duration_us = UINT32_C(0x80000000);
    CHECK(!foc_following_error_validate(&invalid));
    invalid = config;
    invalid.max_gap_us = 0U;
    CHECK(!foc_following_error_validate(&invalid));
    invalid.max_gap_us = UINT32_C(0x80000000);
    CHECK(!foc_following_error_validate(&invalid));
    const struct foc_following_error_config disabled = {0};
    CHECK(foc_following_error_validate(&disabled));
    CHECK(foc_following_error_step(&disabled, &state, &input) == FOC_FOLLOWING_INACTIVE);
    CHECK(!foc_following_error_validate(NULL));
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    return test_limits() || test_following_error();
}

/* ---------------------------------------- end of file ----------------------------------------- */
