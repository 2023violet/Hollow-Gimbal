/**
 * @file    test_foc_virtual_current.c
 * @brief   Runtime-backed virtual PMSM current-loop scenarios.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Test role
 * ---------
 * Check accepted Current feedback, transaction rejection, STOP priority, and
 * deterministic replay through the production Runtime.
 */

#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>

/* Includes --------------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static int start_current(struct foc_virtual_current *context)
{
    CHECK(foc_virtual_current_publish_iq(context, 0.0f, 0.45f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(context, 0U) == 0);
    CHECK(foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(context, 200U) == 0);
    CHECK(foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(context, 400U) == 0);
    return 0;
}

static int run_with_command_refresh(struct foc_virtual_current *context, uint32_t target_us)
{
    while (context->virtual_time_us < target_us) {
        CHECK(foc_virtual_current_publish_iq(context, 0.0f, 0.45f) == FOC_RUNTIME_OK);
        uint32_t next = context->virtual_time_us + 1000U;
        if (next > target_us) {
            next = target_us;
        }
        CHECK(foc_virtual_current_run_until(context, next) == 0);
    }
    return 0;
}

static int
start_motion(struct foc_virtual_current *context, enum foc_runtime_mode mode, float motion_ref)
{
    CHECK(foc_virtual_current_queue_set_mode(context, mode) == 0);
    CHECK(foc_virtual_current_run_until(context, 0U) == 0);
    CHECK(foc_virtual_current_run_until(context, 100U) == 0);
    CHECK(foc_virtual_current_publish_motion(context, mode, motion_ref) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(context, 500U) == 0);
    CHECK(foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(context, 600U) == 0);
    CHECK(foc_virtual_current_queue_action(context, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(context, 800U) == 0);
    return 0;
}

static int run_motion_with_refresh(
    struct foc_virtual_current *context,
    enum foc_runtime_mode mode,
    float motion_ref,
    uint32_t target_us
)
{
    while (context->virtual_time_us < target_us) {
        CHECK(foc_virtual_current_publish_motion(context, mode, motion_ref) == FOC_RUNTIME_OK);
        uint32_t next = context->virtual_time_us + 1000U;
        if (next > target_us) {
            next = target_us;
        }
        CHECK(foc_virtual_current_run_until(context, next) == 0);
    }
    return 0;
}

static int test_current_feedback(void)
{
    struct foc_virtual_current context;
    struct foc_virtual_current_observation observation;
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    CHECK(foc_virtual_current_init(&context, &profile) == 0);
    CHECK(start_current(&context) == 0);
    CHECK(run_with_command_refresh(&context, 12000U) == 0);
    CHECK(foc_virtual_current_observe(&context, &observation) == 0);
    CHECK(observation.accepted_updates > 0U);
    CHECK(isfinite(observation.iq_a) && fabsf(observation.iq_a) > 0.01f);
    CHECK(observation.safety_state == FOC_SAFE_STATE_RUNNING);
    return 0;
}

static int test_rejected_update_and_stop(void)
{
    struct foc_virtual_current context;
    struct foc_virtual_current_observation observation;
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    CHECK(foc_virtual_current_init(&context, &profile) == 0);
    CHECK(start_current(&context) == 0);
    CHECK(run_with_command_refresh(&context, 3000U) == 0);
    const struct foc_vector_dq before = context.runtime.pipeline.current.integral_v;
    const uint32_t accepted_before = context.output.accepted_updates;
    CHECK(foc_virtual_current_reject_next_update(&context) == 0);
    CHECK(foc_virtual_current_publish_iq(&context, 0.0f, 0.55f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&context, 3200U) == 0);
    CHECK(context.output.rejected_updates == 1U);
    CHECK(context.output.accepted_updates == accepted_before);
    CHECK(
        context.runtime.pipeline.current.integral_v.d == before.d &&
        context.runtime.pipeline.current.integral_v.q == before.q
    );
    CHECK(foc_virtual_current_request_stop(&context) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&context, 3400U) == 0);
    CHECK(foc_virtual_current_observe(&context, &observation) == 0);
    CHECK(observation.safety_state == FOC_SAFE_STATE_DISABLED);
    CHECK(observation.accepted_updates == accepted_before);
    return 0;
}

static int test_replay(void)
{
    struct foc_virtual_current first;
    struct foc_virtual_current second;
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    CHECK(foc_virtual_current_init(&first, &profile) == 0);
    CHECK(foc_virtual_current_init(&second, &profile) == 0);
    CHECK(start_current(&first) == 0 && start_current(&second) == 0);
    CHECK(run_with_command_refresh(&first, 10000U) == 0);
    CHECK(run_with_command_refresh(&second, 10000U) == 0);
    CHECK(foc_virtual_current_hash(&first) == foc_virtual_current_hash(&second));
    return 0;
}

static int test_velocity_position_and_mode_session(void)
{
    struct foc_virtual_current velocity;
    struct foc_virtual_current position;
    struct foc_virtual_current_observation observation;
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    CHECK(foc_virtual_current_init(&velocity, &profile) == 0);
    CHECK(start_motion(&velocity, FOC_RUNTIME_MODE_VELOCITY, 4.0f) == 0);
    CHECK(run_motion_with_refresh(&velocity, FOC_RUNTIME_MODE_VELOCITY, 4.0f, 16000U) == 0);
    CHECK(foc_virtual_current_observe(&velocity, &observation) == 0);
    CHECK(
        observation.mode == FOC_RUNTIME_MODE_VELOCITY &&
        observation.safety_state == FOC_SAFE_STATE_RUNNING
    );
    CHECK(fabs(observation.mechanical_velocity_rad_s) > 0.5);
    CHECK(foc_virtual_current_request_stop(&velocity) == 0);
    CHECK(foc_virtual_current_run_until(&velocity, 16200U) == 0);
    const uint32_t old_session = observation.motion_session;
    CHECK(foc_virtual_current_queue_set_mode(&velocity, FOC_RUNTIME_MODE_POSITION) == 0);
    CHECK(foc_virtual_current_run_until(&velocity, 16400U) == 0);
    CHECK(foc_virtual_current_run_until(&velocity, 16500U) == 0);
    CHECK(
        foc_virtual_current_publish_motion(&velocity, FOC_RUNTIME_MODE_POSITION, 1.0f) ==
        FOC_RUNTIME_OK
    );
    CHECK(foc_virtual_current_run_until(&velocity, 16500U) == 0);
    CHECK(foc_virtual_current_queue_action(&velocity, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(&velocity, 16600U) == 0);
    CHECK(foc_virtual_current_queue_action(&velocity, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(&velocity, 16800U) == 0);
    CHECK(foc_virtual_current_observe(&velocity, &observation) == 0);
    CHECK(
        observation.mode == FOC_RUNTIME_MODE_POSITION && observation.motion_session != old_session
    );

    CHECK(foc_virtual_current_init(&position, &profile) == 0);
    CHECK(start_motion(&position, FOC_RUNTIME_MODE_POSITION, 1.0f) == 0);
    CHECK(run_motion_with_refresh(&position, FOC_RUNTIME_MODE_POSITION, 1.0f, 200000U) == 0);
    CHECK(foc_virtual_current_observe(&position, &observation) == 0);
    CHECK(
        observation.mode == FOC_RUNTIME_MODE_POSITION &&
        observation.safety_state == FOC_SAFE_STATE_RUNNING
    );
    CHECK(observation.mechanical_position_rad > 0.05);

    const uint32_t position_session = observation.motion_session;
    CHECK(foc_virtual_current_request_stop(&position) == 0);
    CHECK(foc_virtual_current_run_until(&position, 200200U) == 0);
    CHECK(foc_virtual_current_queue_set_mode(&position, FOC_RUNTIME_MODE_CURRENT) == 0);
    CHECK(foc_virtual_current_run_until(&position, 200400U) == 0);
    CHECK(foc_virtual_current_run_until(&position, 200450U) == 0);
    CHECK(foc_virtual_current_publish_iq(&position, 0.0f, 0.25f) == FOC_RUNTIME_OK);
    CHECK(foc_virtual_current_run_until(&position, 200600U) == 0);
    CHECK(foc_virtual_current_queue_action(&position, FOC_RUNTIME_ACTION_ARM) == 0);
    CHECK(foc_virtual_current_run_until(&position, 200800U) == 0);
    CHECK(foc_virtual_current_queue_action(&position, FOC_RUNTIME_ACTION_START) == 0);
    CHECK(foc_virtual_current_run_until(&position, 201000U) == 0);
    CHECK(foc_virtual_current_observe(&position, &observation) == 0);
    CHECK(
        observation.mode == FOC_RUNTIME_MODE_CURRENT &&
        observation.motion_session != position_session &&
        observation.safety_state == FOC_SAFE_STATE_RUNNING && observation.accepted_updates > 0U
    );
    return 0;
}

static int test_command_timeout_and_sensor_fault_isolation(void)
{
    struct foc_virtual_current context;
    struct foc_virtual_current sensor_context;
    struct foc_virtual_current_observation observation;
    struct foc_virtual_current_observation sensor_observation;
    const struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
    CHECK(foc_virtual_current_init(&context, &profile) == 0);
    CHECK(start_motion(&context, FOC_RUNTIME_MODE_VELOCITY, 3.0f) == 0);
    CHECK(foc_virtual_current_run_until(&context, 1200U) == 0);
    CHECK(foc_virtual_current_run_until(&context, 5000U) == 0);
    CHECK(foc_virtual_current_observe(&context, &observation) == 0);
    CHECK(observation.safety_state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(observation.faults != 0U && observation.accepted_updates > 0U);

    CHECK(foc_virtual_current_init(&sensor_context, &profile) == 0);
    CHECK(start_motion(&sensor_context, FOC_RUNTIME_MODE_VELOCITY, 3.0f) == 0);
    CHECK(run_motion_with_refresh(&sensor_context, FOC_RUNTIME_MODE_VELOCITY, 3.0f, 3400U) == 0);
    const uint32_t updates_before_sensor_fault = sensor_context.output.accepted_updates;
    CHECK(foc_virtual_current_fail_next_sensor(&sensor_context) == 0);
    CHECK(foc_virtual_current_run_until(&sensor_context, 3800U) == 0);
    CHECK(foc_virtual_current_observe(&sensor_context, &sensor_observation) == 0);
    CHECK(sensor_observation.safety_state != FOC_SAFE_STATE_RUNNING);
    CHECK(sensor_observation.accepted_updates <= updates_before_sensor_fault);
    return 0;
}

static int test_multiturn_trajectory(void)
{
    for (int direction = -1; direction <= 1; direction += 2) {
        struct foc_virtual_current context;
        struct foc_virtual_current_profile profile = foc_virtual_current_default_profile();
        profile.runtime.motion.trajectory = (struct foc_trajectory_config){
            .enabled = true,
            .max_velocity_rad_s = 8.0f,
            .max_accel_rad_s2 = 20.0f,
            .max_dt_s = .002f,
            .position_tolerance_rad = .01f,
        };
        const float target = (float)direction * 12.566370614359172f;
        CHECK(foc_virtual_current_init(&context, &profile) == 0);
        CHECK(start_motion(&context, FOC_RUNTIME_MODE_POSITION, target) == 0);
        CHECK(run_motion_with_refresh(&context, FOC_RUNTIME_MODE_POSITION, target, 6000000U) == 0);
        struct foc_runtime_snapshot view;
        CHECK(foc_runtime_read_snapshot(&context.runtime, &view) == 0);
        printf(
            "multiturn trajectory target=%.9g measured=%.9g plant=%.12g counts=%lld updates=%u\n",
            (double)target,
            (double)view.mechanical_rad,
            context.motor.state.mechanical_position_rad,
            (long long)context.encoder.position_counts,
            view.updates
        );
        CHECK(view.state == FOC_SAFE_STATE_RUNNING && view.faults == 0U);
        CHECK(
            fabsf(view.mechanical_rad - target) < .1f &&
            (float)direction * view.mechanical_rad > 6.283185307179586f
        );
        CHECK(fabs(context.motor.state.mechanical_position_rad - view.mechanical_rad) < .01);
        CHECK(context.runtime.config.motion.trajectory.enabled);
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    return test_current_feedback() || test_rejected_update_and_stop() || test_replay() ||
        test_velocity_position_and_mode_session() ||
        test_command_timeout_and_sensor_fault_isolation() || test_multiturn_trajectory();
}

/* ---------------------------------------- end of file ----------------------------------------- */
