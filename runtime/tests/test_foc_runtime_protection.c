/**
 * @file    test_foc_runtime_protection.c
 * @brief   Test following protection and existing output transaction priorities.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Test role
 * ---------
 * Advance H1 and H0 at different rates and inject competing events on the
 * first following-error trip cycle.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Types ---------------------------------------------------------------- */

enum trip_event {
    FOLLOWING_TRIP,
    INVALID_ADC,
    STALE_SENSOR,
    INDEPENDENT_STOP,
    REJECTED_INHIBIT,
};

/* Private Helpers -------------------------------------------------------------- */

static void
stop_at_submit(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *argument)
{
    const enum trip_event *event = argument;
    if (*event == INDEPENDENT_STOP && point == FOC_RUNTIME_HOOK_BEFORE_SUBMIT) {
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_WDT);
    }
}

static int sense(struct foc_runtime *runtime, uint32_t now)
{
    const struct foc_runtime_sensor sensor = {
        .theta_elec_rad = 0.37f,
        .captured_us = now,
        .seq = now / 200U,
        .valid = true,
        .electrical_phase_valid = true,
        .mechanical_frame_id = 7U,
    };
    return foc_runtime_h1_step(runtime, &sensor, now, 0U);
}

static int
cycle(struct foc_runtime *runtime, uint32_t now, enum foc_runtime_action action, bool valid_adc)
{
    const struct foc_runtime_fast_input input = {
        .adc =
            {1200U,
             2048U,
             2048U,
             2048U,
             valid_adc ? FOC_DEVICE_SAMPLE_ALL_VALID : 0U,
             now / 100U,
             now},
        .now_us = now,
        .action = action,
        .requested_mode = FOC_RUNTIME_MODE_POSITION,
    };
    struct foc_runtime_foreground_result foreground;
    const int status = foc_runtime_h0_step(runtime, &input);
    (void)foc_runtime_foreground_poll(runtime, now, 0U, 0U, &foreground);
    return status;
}

static int trip_case(enum trip_event event, int expected, uint32_t expected_fault)
{
    const struct foc_pipeline_config pipeline = {
        .current = {{1.0f, 2.4f, 0.0f, 1.0f, 0.0003f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f},
        .adc = {0.01f, 0.002f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
        .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
        .safety = {5000U, 500U, 350U, 350U, 1U},
    };
    enum trip_event injection = FOLLOWING_TRIP;
    const struct foc_runtime_config config = {
        .h0 = {100U, 20U, 10U},
        .h1 = {200U, 20U, 10U},
        .supervision = {100U, 20U, 10U},
        .telemetry = {10000U, 20U, 10U},
        .foreground_budget_us = 15U,
        .angle_timeout_us = 150U,
        .hook = stop_at_submit,
        .hook_arg = &injection,
        .motion = {
            .enabled = true,
            .rotor = {4U, 0.37f},
            .prediction = {150U, 2000.0f, 0.001f},
            .velocity = {0.18f, 1.4f, 0.002f},
            .position = {4.0f, 12.0f},
            .iq_limit_a = 1.25f,
            .limits =
                {
                    .position_min_rad = -1.0f,
                    .position_max_rad = 1.0f,
                    .mechanical_frame_id = 7U,
                    .enabled = true,
                },
            .following_error = {
                .threshold_rad = 0.05f,
                .duration_us = 400U,
                .max_gap_us = 250U,
                .enabled = true,
            },
        },
    };
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_POSITION,
        .seq = 1U,
        .produced_us = 101U,
        .target.motion_ref = 0.4f,
    };
    struct foc_runtime runtime;
    struct foc_runtime_snapshot view;
    CHECK(foc_runtime_init(&runtime, &pipeline, &config, 0U) == FOC_RUNTIME_OK);
    CHECK(sense(&runtime, 0U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 0U, FOC_RUNTIME_ACTION_NONE, true) == FOC_RUNTIME_IDLE);
    CHECK(cycle(&runtime, 100U, FOC_RUNTIME_ACTION_SET_MODE, true) == FOC_RUNTIME_IDLE);
    CHECK(foc_runtime_publish_motion_command(&runtime, &command, 101U) == FOC_RUNTIME_OK);
    CHECK(sense(&runtime, 200U) == FOC_RUNTIME_OK);
    CHECK(cycle(&runtime, 200U, FOC_RUNTIME_ACTION_ARM, true) == FOC_RUNTIME_IDLE);
    CHECK(cycle(&runtime, 300U, FOC_RUNTIME_ACTION_START, true) == FOC_RUNTIME_IDLE);
    for (uint32_t now = 400U; now < 800U; now += 200U) {
        CHECK(sense(&runtime, now) == FOC_RUNTIME_OK);
        CHECK(cycle(&runtime, now, FOC_RUNTIME_ACTION_NONE, true) == FOC_RUNTIME_OK);
        CHECK(cycle(&runtime, now + 100U, FOC_RUNTIME_ACTION_NONE, true) == FOC_RUNTIME_OK);
        CHECK(runtime.following.elapsed_us == now - 400U);
    }
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(view.following_state == FOC_FOLLOWING_PENDING && runtime.pipeline.mock.updates == 4U);
    const uint32_t session = view.motion_session;
    const struct foc_vector_dq integral = runtime.pipeline.current.integral_v;

    if (event != STALE_SENSOR) {
        CHECK(sense(&runtime, 800U) == FOC_RUNTIME_OK);
    }
    injection = event;
    runtime.pipeline.mock.reject_inhibit = event == REJECTED_INHIBIT;
    CHECK(cycle(&runtime, 800U, FOC_RUNTIME_ACTION_NONE, event != INVALID_ADC) == expected);
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(view.motion_session != session && runtime.pipeline.mock.updates == 4U);
    CHECK(runtime.pipeline.current.integral_v.d == integral.d);
    CHECK(runtime.pipeline.current.integral_v.q == integral.q);
    CHECK(view.action == FOC_DEVICE_OUTPUT_INHIBIT);
    CHECK(view.output_confirmed == (event != REJECTED_INHIBIT));
    CHECK(view.faults == expected_fault);

    if (event == FOLLOWING_TRIP || event == REJECTED_INHIBIT) {
        CHECK(view.following_state == FOC_FOLLOWING_TRIGGERED);
        CHECK(view.motion_rejection == FOC_MOTION_REJECT_FOLLOWING);
        CHECK(
            view.pipeline_status ==
            (event == REJECTED_INHIBIT ? FOC_PIPELINE_ERR_INHIBIT : FOC_PIPELINE_ERR_STOP)
        );
    } else if (event == STALE_SENSOR) {
        CHECK(view.following_state == FOC_FOLLOWING_INVALID_SENSOR);
        CHECK(view.motion_rejection == FOC_MOTION_REJECT_FEEDBACK);
    } else {
        CHECK(view.following_state == FOC_FOLLOWING_INACTIVE);
        CHECK(view.motion_rejection == FOC_MOTION_REJECT_NONE);
    }
    const int replay = expected_fault == 0U ? FOC_RUNTIME_ERR_ORDER : FOC_RUNTIME_ERR_CONTROL;
    CHECK(foc_runtime_publish_motion_command(&runtime, &command, 801U) == replay);
    printf(
        "event=%d h0=%d pipeline=%d updates=4 session=%lu->%lu confirmed=%d faults=0x%lx\n",
        event,
        expected,
        view.pipeline_status,
        (unsigned long)session,
        (unsigned long)view.motion_session,
        view.output_confirmed,
        (unsigned long)view.faults
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    CHECK(trip_case(FOLLOWING_TRIP, FOC_RUNTIME_ERR_STOP, 0U) == 0);
    CHECK(trip_case(INVALID_ADC, FOC_RUNTIME_ERR_CONTROL, FOC_SAFE_FAULT_SAMPLE_INVALID) == 0);
    CHECK(trip_case(STALE_SENSOR, FOC_RUNTIME_ERR_ALIGNMENT, FOC_SAFE_FAULT_SAMPLE_INVALID) == 0);
    CHECK(trip_case(INDEPENDENT_STOP, FOC_RUNTIME_ERR_STOP, FOC_SAFE_FAULT_WDT) == 0);
    CHECK(trip_case(REJECTED_INHIBIT, FOC_RUNTIME_ERR_STOP, 0U) == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
