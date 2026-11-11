/**
 * @file    test_foc_motion_runtime.c
 * @brief   Sensed alignment, mode boundaries and outer target acknowledgement tests.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Inject adverse events into the real H0/H1/Safety chain and check recovery.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

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

/* Private Constants ------------------------------------------------------------ */

static const struct foc_pipeline_config pipeline_config = {
    .current = {{1.0f, 2.4f, 0.0f, 1.0f, 0.0003f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f},
    .adc = {0.01f, 0.002f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {2500U, 500U, 350U, 350U, 3U},
};

static const struct foc_runtime_config config = {
    .h0 = {100U, 20U, 10U},
    .h1 = {100U, 600U, 10U},
    .supervision = {100U, 20U, 10U},
    .telemetry = {10000U, 20U, 10U},
    .foreground_budget_us = 15U,
    .angle_timeout_us = 600U,
    .motion =
        {true, {4U, 0.37f}, {400U, 2000.0f, 0.0003f}, {0.18f, 1.4f, 0.002f}, {4.0f, 12.0f}, 2.5f},
};

/* Private Helpers -------------------------------------------------------------- */

static int sense(
    struct foc_runtime *runtime,
    uint32_t captured,
    uint32_t now_us,
    uint32_t seq,
    float position,
    float velocity,
    bool valid
)
{
    const struct foc_runtime_sensor sensor = {
        .captured_us = captured,
        .seq = seq,
        .valid = valid,
        .mechanical_rad = position,
        .mechanical_rad_s = velocity,
    };
    return foc_runtime_h1_step(runtime, &sensor, now_us, 0U);
}

static int h0(
    struct foc_runtime *runtime,
    uint32_t at,
    uint32_t seq,
    enum foc_runtime_action action,
    enum foc_runtime_mode mode
)
{
    const struct foc_runtime_fast_input input = {
        .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, seq, at},
        .now_us = at + 1U,
        .action = action,
        .requested_mode = mode,
    };
    return foc_runtime_h0_step(runtime, &input);
}

static int foreground(struct foc_runtime *runtime, uint32_t time)
{
    struct foc_runtime_foreground_result result;
    return foc_runtime_foreground_poll(runtime, time + 2U, 0U, 0U, &result);
}

static int drive(
    struct foc_runtime *runtime,
    uint32_t at,
    uint32_t seq,
    enum foc_runtime_action action,
    enum foc_runtime_mode mode,
    int expected
)
{
    TEST_ASSERT(sense(runtime, at, at, seq, 0.0f, 0.0f, true) == FOC_RUNTIME_OK);
    TEST_ASSERT(h0(runtime, at, seq, action, mode) == expected);
    TEST_ASSERT(foreground(runtime, at) == FOC_RUNTIME_OK);
    return 0;
}

static int motion_command(
    struct foc_runtime *runtime,
    enum foc_runtime_mode mode,
    uint32_t now_us,
    uint32_t seq,
    float ref
)
{
    const struct foc_runtime_command command = {mode, seq, now_us, {.motion_ref = ref}};
    return foc_runtime_publish_motion_command(runtime, &command, now_us);
}

static int current_command(struct foc_runtime *runtime, uint32_t now_us, uint32_t seq)
{
    const struct foc_pipeline_command command = {{0.10f, 0.50f}, seq, now_us};
    return foc_runtime_publish_command(runtime, &command, now_us);
}

static int
start_current(struct foc_runtime *runtime, const struct foc_runtime_config *policy, uint32_t base)
{
    TEST_ASSERT(foc_runtime_init(runtime, &pipeline_config, policy, base) == FOC_RUNTIME_OK);
    TEST_ASSERT(current_command(runtime, base, 1U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        drive(
            runtime,
            base,
            0U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            runtime,
            base + 100U,
            1U,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            runtime,
            base + 200U,
            2U,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            runtime,
            base + 300U,
            3U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_OK
        ) == 0
    );
    return 0;
}

static int test_mode_and_ack(void)
{
    struct foc_runtime runtime = {0};
    TEST_ASSERT(start_current(&runtime, &config, 0U) == 0);
    const float current_integral = runtime.pipeline.current.integral_v.q;
    TEST_ASSERT(current_integral > 0.0f);
    TEST_ASSERT(
        drive(
            &runtime,
            400U,
            4U,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_ERR_CONTROL
        ) == 0
    );
    TEST_ASSERT(
        runtime.mode == FOC_RUNTIME_MODE_CURRENT &&
        runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING
    );
    TEST_ASSERT(
        runtime.pipeline.mock.updates == 1U &&
        runtime.pipeline.current.integral_v.q == current_integral
    );
    TEST_ASSERT(
        motion_command(&runtime, FOC_RUNTIME_MODE_VELOCITY, 500U, 2U, 10.0f) == FOC_RUNTIME_OK
    );
    foc_runtime_request_inhibit(&runtime, 0U);
    TEST_ASSERT(
        drive(
            &runtime,
            500U,
            5U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_ERR_STOP
        ) == 0
    );
    const uint32_t stopped_session = runtime.motion_session;
    TEST_ASSERT(
        drive(
            &runtime,
            600U,
            6U,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        runtime.mode == FOC_RUNTIME_MODE_VELOCITY && runtime.motion_session != stopped_session
    );
    TEST_ASSERT(
        drive(
            &runtime,
            700U,
            7U,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_ERR_CONTROL
        ) == 0
    );
    TEST_ASSERT(
        runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED &&
        runtime.pipeline.current.integral_v.q == current_integral
    );
    TEST_ASSERT(
        motion_command(&runtime, FOC_RUNTIME_MODE_VELOCITY, 800U, 3U, 10.0f) == FOC_RUNTIME_OK
    );
    TEST_ASSERT(
        drive(
            &runtime,
            800U,
            8U,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            &runtime,
            900U,
            9U,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(runtime.pipeline.current.integral_v.q == 0.0f);
    TEST_ASSERT(
        drive(
            &runtime,
            1000U,
            10U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_OK
        ) == 0
    );
    TEST_ASSERT(
        runtime.outer_velocity.integral_a == 0.0f && runtime.outer_trial_seen &&
        runtime.accepted_outer_seq == 10U
    );
    TEST_ASSERT(
        h0(&runtime, 1100U, 11U, FOC_RUNTIME_ACTION_NONE, FOC_RUNTIME_MODE_VELOCITY) ==
        FOC_RUNTIME_OK
    );
    TEST_ASSERT(foreground(&runtime, 1100U) == FOC_RUNTIME_OK);
    TEST_ASSERT(sense(&runtime, 1200U, 1200U, 12U, 0.0f, 0.0f, true) == FOC_RUNTIME_OK);
    TEST_ASSERT(fabsf(runtime.outer_velocity.integral_a - 0.0014f) < 1.0e-7f);
    TEST_ASSERT(fabsf(runtime.outer_trial_a - 0.0042f) < 1.0e-7f);
    const float accepted = runtime.outer_velocity.integral_a;
    const float inner = runtime.pipeline.current.integral_v.q;
    runtime.pipeline.mock.reject_update = true;
    TEST_ASSERT(
        h0(&runtime, 1200U, 12U, FOC_RUNTIME_ACTION_NONE, FOC_RUNTIME_MODE_VELOCITY) ==
        FOC_RUNTIME_ERR_CONTROL
    );
    TEST_ASSERT(foreground(&runtime, 1200U) == FOC_RUNTIME_OK);
    TEST_ASSERT(runtime.pipeline_status == FOC_PIPELINE_ERR_UPDATE && !runtime.accepted_outer_seen);
    TEST_ASSERT(
        runtime.pipeline.current.integral_v.q == inner &&
        runtime.outer_velocity.integral_a == accepted
    );
    runtime.pipeline.mock.reject_update = false;
    TEST_ASSERT(
        drive(
            &runtime,
            1300U,
            13U,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_POSITION,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(runtime.outer_velocity.integral_a == 0.0f && !runtime.outer_trial_seen);
    TEST_ASSERT(
        drive(
            &runtime,
            1400U,
            14U,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_POSITION,
            FOC_RUNTIME_ERR_CONTROL
        ) == 0
    );
    TEST_ASSERT(
        motion_command(&runtime, FOC_RUNTIME_MODE_POSITION, 1500U, 4U, -1.0f) == FOC_RUNTIME_OK
    );
    TEST_ASSERT(
        drive(
            &runtime,
            1500U,
            15U,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_POSITION,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            &runtime,
            1600U,
            16U,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_POSITION,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            &runtime,
            1700U,
            17U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_POSITION,
            FOC_RUNTIME_OK
        ) == 0
    );
    TEST_ASSERT(runtime.pipeline.current.integral_v.q < 0.0f);
    foc_runtime_request_inhibit(&runtime, 0U);
    TEST_ASSERT(
        drive(
            &runtime,
            1800U,
            18U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_POSITION,
            FOC_RUNTIME_ERR_STOP
        ) == 0
    );
    TEST_ASSERT(
        drive(
            &runtime,
            1900U,
            19U,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        runtime.mode == FOC_RUNTIME_MODE_CURRENT && runtime.outer_velocity.integral_a == 0.0f
    );
    TEST_ASSERT(current_command(&runtime, 2000U, 5U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        h0(&runtime, 2000U, 20U, FOC_RUNTIME_ACTION_ARM, FOC_RUNTIME_MODE_CURRENT) ==
        FOC_RUNTIME_ERR_CONTROL
    );
    TEST_ASSERT(runtime.pipeline_status == FOC_PIPELINE_ERR_SAMPLE_ORDER);
    TEST_ASSERT(foreground(&runtime, 2000U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        drive(
            &runtime,
            2100U,
            21U,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            &runtime,
            2200U,
            22U,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            &runtime,
            2300U,
            23U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_OK
        ) == 0
    );
    TEST_ASSERT(runtime.pipeline.target_a.d == 0.1f && runtime.pipeline.target_a.q == 0.5f);
    return 0;
}

static int test_sensed_failures(void)
{
    enum {
        BAD_ANGLE,
        BAD_SPEED,
        BAD_QUALITY,
        FUTURE,
        AGE,
        HORIZON,
        ERROR_BOUND
    };
    for (unsigned int fault = BAD_ANGLE; fault <= ERROR_BOUND; ++fault) {
        struct foc_runtime runtime = {0};
        struct foc_runtime_config policy = config;
        if (fault == AGE) {
            policy.angle_timeout_us = 50U;
        } else if (fault == HORIZON) {
            policy.motion.prediction.max_prediction_us = 50U;
        } else if (fault == ERROR_BOUND) {
            policy.motion.prediction.max_error_rad = 5.0e-6f;
        }
        TEST_ASSERT(start_current(&runtime, &policy, 0U) == 0);
        if (fault <= FUTURE) {
            const uint32_t stamp = fault == FUTURE ? 401U : 400U;
            TEST_ASSERT(
                sense(
                    &runtime,
                    stamp,
                    stamp,
                    4U,
                    fault == BAD_ANGLE ? NAN : 0.0f,
                    fault == BAD_SPEED ? INFINITY : 0.0f,
                    fault != BAD_QUALITY
                ) == (fault == FUTURE ? FOC_RUNTIME_OK : FOC_RUNTIME_ERR_INPUT)
            );
        }
        const float integral = runtime.pipeline.current.integral_v.q;
        TEST_ASSERT(
            h0(&runtime, 400U, 4U, FOC_RUNTIME_ACTION_NONE, FOC_RUNTIME_MODE_CURRENT) ==
            FOC_RUNTIME_ERR_ALIGNMENT
        );
        TEST_ASSERT(
            runtime.pipeline_status == FOC_PIPELINE_ERR_SAMPLE &&
            runtime.pipeline.mock.updates == 1U
        );
        TEST_ASSERT(
            runtime.pipeline.current.integral_v.q == integral &&
            runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED
        );
        TEST_ASSERT(foreground(&runtime, 400U) == FOC_RUNTIME_OK);
        TEST_ASSERT(
            drive(
                &runtime,
                500U,
                5U,
                FOC_RUNTIME_ACTION_NONE,
                FOC_RUNTIME_MODE_CURRENT,
                FOC_RUNTIME_IDLE
            ) == 0
        );
        TEST_ASSERT(runtime.pipeline.mock.updates == 1U);
    }

    struct foc_runtime runtime = {0};
    const uint32_t base = UINT32_MAX - 349U;
    TEST_ASSERT(start_current(&runtime, &config, base) == 0);
    for (uint32_t seq = 4U; seq <= 7U; ++seq) {
        const uint32_t at = base + 100U * seq;
        TEST_ASSERT(
            h0(&runtime, at, seq, FOC_RUNTIME_ACTION_NONE, FOC_RUNTIME_MODE_CURRENT) ==
            FOC_RUNTIME_OK
        );
        TEST_ASSERT(runtime.angle_prediction_us == (seq - 3U) * 100U);
        TEST_ASSERT(foreground(&runtime, at) == FOC_RUNTIME_OK);
    }
    TEST_ASSERT(
        h0(&runtime, base + 800U, 8U, FOC_RUNTIME_ACTION_NONE, FOC_RUNTIME_MODE_CURRENT) ==
        FOC_RUNTIME_ERR_ALIGNMENT
    );
    TEST_ASSERT(foreground(&runtime, base + 800U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        drive(
            &runtime,
            base + 900U,
            9U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_CURRENT,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        runtime.pipeline.mock.updates == 5U &&
        runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED
    );
    return 0;
}

static int test_command_expiry(void)
{
    struct foc_runtime runtime = {0};
    struct foc_pipeline_config policy = pipeline_config;
    policy.safety.command_timeout_us = 900U;
    TEST_ASSERT(foc_runtime_init(&runtime, &policy, &config, 0U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        drive(
            &runtime,
            0U,
            0U,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        motion_command(&runtime, FOC_RUNTIME_MODE_VELOCITY, 100U, 1U, 100.0f) == FOC_RUNTIME_OK
    );
    TEST_ASSERT(
        drive(
            &runtime,
            100U,
            1U,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    TEST_ASSERT(
        drive(
            &runtime,
            200U,
            2U,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_IDLE
        ) == 0
    );
    for (uint32_t seq = 3U; seq < 10U; ++seq) {
        TEST_ASSERT(
            drive(
                &runtime,
                seq * 100U,
                seq,
                FOC_RUNTIME_ACTION_NONE,
                FOC_RUNTIME_MODE_VELOCITY,
                FOC_RUNTIME_OK
            ) == 0
        );
        TEST_ASSERT(
            runtime.pipeline.safety.last_command_us == 100U && runtime.accepted_outer_seq == seq
        );
        if (seq == 3U) {
            const unsigned int slot = atomic_load(&runtime.sensor_slot);
            TEST_ASSERT(fabsf(runtime.sensor_slots[slot].target.target_a.q - 2.16168f) < 1.0e-5f);
        }
    }
    TEST_ASSERT(
        drive(
            &runtime,
            1000U,
            10U,
            FOC_RUNTIME_ACTION_NONE,
            FOC_RUNTIME_MODE_VELOCITY,
            FOC_RUNTIME_ERR_CONTROL
        ) == 0
    );
    TEST_ASSERT(runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_COMMAND_TIMEOUT);
    TEST_ASSERT(
        runtime.pipeline.mock.updates == 7U &&
        runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    return test_mode_and_ack() || test_sensed_failures() || test_command_expiry();
}

/* ---------------------------------------- end of file ----------------------------------------- */
