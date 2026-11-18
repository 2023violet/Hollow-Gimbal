/**
 * @file    test_foc_runtime_mailbox.c
 * @brief   Negative-first lifecycle mailbox contract tests.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Test role
 * ---------
 * Exercise the approved Foreground-to-H0 mailbox contract before its Runtime
 * implementation is present. The local declarations are intentionally kept
 * here during the red phase and will move to foc_runtime.h with the minimal
 * production implementation.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

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
    .current = {{1.0f, 2.0f, 0.0f, 1.0f, 0.001f}, {1.0f, 1.5f}, {500.0f, 500.0f}, 5.0f},
    .adc = {0.01f, 0.001f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {1500U, 800U, 350U, 350U, 3U},
};

static const struct foc_runtime_config runtime_config = {
    .h0 = {100U, 20U, 10U},
    .h1 = {100U, 20U, 10U},
    .supervision = {100U, 20U, 10U},
    .telemetry = {500U, 20U, 10U},
    .foreground_budget_us = 15U,
    .angle_timeout_us = 200U,
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_runtime_fast_input input_at(uint32_t time_us, uint32_t seq)
{
    return (struct foc_runtime_fast_input){
        .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, seq, time_us},
        .now_us = time_us,
    };
}

static int publish_sample(struct foc_runtime *runtime, uint32_t time_us, uint32_t seq)
{
    const struct foc_runtime_sensor sample =
        {.theta_elec_rad = 0.37f, .captured_us = time_us, .seq = seq, .valid = true};
    return foc_runtime_h1_step(runtime, &sample, time_us, 0U);
}

static int service(struct foc_runtime *runtime, uint32_t time_us, uint32_t seq)
{
    struct foc_runtime_fast_input input = input_at(time_us, seq);
    return foc_runtime_h0_step(runtime, &input);
}

static int start_ready(struct foc_runtime *runtime)
{
    const struct foc_pipeline_command command = {{0.0f, 1.0f}, 0U, 0U};
    TEST_ASSERT(foc_runtime_init(runtime, &pipeline_config, &runtime_config, 0U) == FOC_RUNTIME_OK);
    TEST_ASSERT(foc_runtime_publish_command(runtime, &command, 0U) == FOC_RUNTIME_OK);
    TEST_ASSERT(publish_sample(runtime, 0U, 0U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(runtime, 0U, 0U) == FOC_RUNTIME_IDLE);
    return 0;
}

static int read_result(
    struct foc_runtime *runtime,
    enum foc_runtime_request_result expected,
    uint32_t request_sequence
)
{
    struct foc_runtime_lifecycle_result result;
    TEST_ASSERT(foc_runtime_read_lifecycle_result(runtime, &result) == FOC_RUNTIME_OK);
    TEST_ASSERT(result.result == expected && result.request_sequence == request_sequence);
    return 0;
}

/* Test Cases ------------------------------------------------------------------- */

static int test_normal_apply_and_stop_cancel(void)
{
    struct foc_runtime runtime = {0};
    TEST_ASSERT(start_ready(&runtime) == 0);
    const struct foc_runtime_lifecycle_request arm = {
        .action = FOC_RUNTIME_ACTION_ARM,
        .requested_mode = FOC_RUNTIME_MODE_CURRENT,
        .request_sequence = 1U,
        .source_session = 0U,
        .produced_us = 100U,
    };
    TEST_ASSERT(foc_runtime_publish_lifecycle_request(&runtime, &arm, 100U) == FOC_RUNTIME_OK);
    TEST_ASSERT(read_result(&runtime, FOC_RUNTIME_REQUEST_PENDING, 1U) == 0);
    TEST_ASSERT(publish_sample(&runtime, 100U, 1U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(&runtime, 100U, 1U) == FOC_RUNTIME_IDLE);
    TEST_ASSERT(read_result(&runtime, FOC_RUNTIME_REQUEST_APPLIED, 1U) == 0);
    TEST_ASSERT(runtime.pipeline.safety.state == FOC_SAFE_STATE_READY);

    const struct foc_runtime_lifecycle_request start = {
        .action = FOC_RUNTIME_ACTION_START,
        .requested_mode = FOC_RUNTIME_MODE_CURRENT,
        .request_sequence = 2U,
        .source_session = 0U,
        .produced_us = 200U,
    };
    TEST_ASSERT(foc_runtime_publish_lifecycle_request(&runtime, &start, 200U) == FOC_RUNTIME_OK);
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_INVALID_STATE);
    TEST_ASSERT(publish_sample(&runtime, 200U, 2U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(&runtime, 200U, 2U) == FOC_RUNTIME_ERR_STOP);
    TEST_ASSERT(read_result(&runtime, FOC_RUNTIME_REQUEST_CANCELLED, 2U) == 0);
    TEST_ASSERT(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    return 0;
}

static int test_busy_duplicate_and_stale_session(void)
{
    struct foc_runtime runtime = {0};
    TEST_ASSERT(start_ready(&runtime) == 0);
    const struct foc_runtime_lifecycle_request arm = {
        .action = FOC_RUNTIME_ACTION_ARM,
        .requested_mode = FOC_RUNTIME_MODE_CURRENT,
        .request_sequence = 10U,
        .source_session = 0U,
        .produced_us = 100U,
    };
    TEST_ASSERT(foc_runtime_publish_lifecycle_request(&runtime, &arm, 100U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        foc_runtime_publish_lifecycle_request(&runtime, &arm, 100U) == FOC_RUNTIME_ERR_BUSY
    );
    TEST_ASSERT(publish_sample(&runtime, 100U, 1U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(&runtime, 100U, 1U) == FOC_RUNTIME_IDLE);
    TEST_ASSERT(read_result(&runtime, FOC_RUNTIME_REQUEST_APPLIED, 10U) == 0);
    struct foc_runtime_lifecycle_request duplicate = arm;
    duplicate.request_sequence = 9U;
    TEST_ASSERT(
        foc_runtime_publish_lifecycle_request(&runtime, &duplicate, 100U) == FOC_RUNTIME_ERR_ORDER
    );

    foc_runtime_request_inhibit(&runtime, 0U);
    TEST_ASSERT(publish_sample(&runtime, 200U, 2U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(&runtime, 200U, 2U) == FOC_RUNTIME_ERR_STOP);
    struct foc_runtime_snapshot snapshot;
    TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    const struct foc_runtime_lifecycle_request stale = {
        .action = FOC_RUNTIME_ACTION_START,
        .requested_mode = FOC_RUNTIME_MODE_CURRENT,
        .request_sequence = 11U,
        .source_session = snapshot.motion_session - 1U,
        .produced_us = 300U,
    };
    TEST_ASSERT(
        foc_runtime_publish_lifecycle_request(&runtime, &stale, 300U) == FOC_RUNTIME_ERR_ORDER
    );
    TEST_ASSERT(runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    return 0;
}

static int test_expired_request_and_set_mode_contract(void)
{
    struct foc_runtime runtime = {0};
    TEST_ASSERT(start_ready(&runtime) == 0);
    const struct foc_runtime_lifecycle_request expired = {
        .action = FOC_RUNTIME_ACTION_SET_MODE,
        .requested_mode = FOC_RUNTIME_MODE_CURRENT,
        .request_sequence = 20U,
        .source_session = 0U,
        .produced_us = 0U,
    };
    TEST_ASSERT(
        foc_runtime_publish_lifecycle_request(&runtime, &expired, 2000U) == FOC_RUNTIME_ERR_ORDER
    );
    const struct foc_runtime_lifecycle_request mode = {
        .action = FOC_RUNTIME_ACTION_SET_MODE,
        .requested_mode = FOC_RUNTIME_MODE_CURRENT,
        .request_sequence = 21U,
        .source_session = 0U,
        .produced_us = 100U,
    };
    TEST_ASSERT(foc_runtime_publish_lifecycle_request(&runtime, &mode, 100U) == FOC_RUNTIME_OK);
    TEST_ASSERT(publish_sample(&runtime, 100U, 1U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(&runtime, 100U, 1U) == FOC_RUNTIME_IDLE);
    TEST_ASSERT(read_result(&runtime, FOC_RUNTIME_REQUEST_APPLIED, 21U) == 0);
    TEST_ASSERT(runtime.mode == FOC_RUNTIME_MODE_CURRENT);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

static int start_running(struct foc_runtime *runtime)
{
    TEST_ASSERT(start_ready(runtime) == 0);
    struct foc_runtime_foreground_result initial;
    TEST_ASSERT(foc_runtime_foreground_poll(runtime, 2U, 0U, 0U, &initial) >= 0);
    for (uint32_t t = 100U; t <= 300U; t += 100U) {
        TEST_ASSERT(publish_sample(runtime, t, t / 100U) == FOC_RUNTIME_OK);
        struct foc_runtime_fast_input input = input_at(t, t / 100U);
        input.action = t == 100U ? FOC_RUNTIME_ACTION_ARM
            : t == 200U          ? FOC_RUNTIME_ACTION_START
                                 : FOC_RUNTIME_ACTION_NONE;
        TEST_ASSERT(
            foc_runtime_h0_step(runtime, &input) == (t == 300U ? FOC_RUNTIME_OK : FOC_RUNTIME_IDLE)
        );
        struct foc_runtime_foreground_result result;
        TEST_ASSERT(foc_runtime_foreground_poll(runtime, t + 2U, 0U, 0U, &result) >= 0);
    }
    return 0;
}

/* Route: 0 normal, 1 wrong ARM mailbox, 2 expired mailbox, 3 direct ARM,
 * 4 wrong SET_MODE mailbox, 5 direct SET_MODE, 6 direct CLEAR, 7 wrong START. */
static int test_rejected_cycle(unsigned route, unsigned failure)
{
    struct foc_runtime runtime = {0};
    TEST_ASSERT(start_running(&runtime) == 0);
    uint32_t t = 400U;
    if (failure == 3U) {
        for (; t <= 600U; t += 100U) {
            TEST_ASSERT(publish_sample(&runtime, t, t / 100U) == 0);
            TEST_ASSERT(service(&runtime, t, t / 100U) == FOC_RUNTIME_OK);
        }
    }
    const bool mailbox = route == 1U || route == 2U || route == 4U || route == 7U;
    const struct foc_runtime_lifecycle_request request = {
        .action = route == 4U ? FOC_RUNTIME_ACTION_SET_MODE
            : route == 7U     ? FOC_RUNTIME_ACTION_START
                              : FOC_RUNTIME_ACTION_ARM,
        .requested_mode = FOC_RUNTIME_MODE_CURRENT,
        .request_sequence = 41U,
        .source_session = runtime.motion_session,
        .produced_us = route == 2U ? t - 1500U : t - 98U,
    };
    if (mailbox) {
        TEST_ASSERT(foc_runtime_publish_lifecycle_request(&runtime, &request, t - 98U) == 0);
    }
    struct foc_runtime_sensor sensor =
        {.theta_elec_rad = .37f, .captured_us = t, .seq = t / 100U, .valid = failure != 2U};
    TEST_ASSERT(
        foc_runtime_h1_step(&runtime, &sensor, t, 0U) ==
        (failure == 2U ? FOC_RUNTIME_ERR_INPUT : FOC_RUNTIME_OK)
    );
    struct foc_runtime_fast_input input = input_at(t, t / 100U);
    input.now_us = t + 1U;
    if (failure == 1U)
        input.adc.valid_mask = 0U;
    if (route == 3U)
        input.action = FOC_RUNTIME_ACTION_ARM;
    if (route == 5U)
        input.action = FOC_RUNTIME_ACTION_SET_MODE;
    if (route == 6U)
        input.action = FOC_RUNTIME_ACTION_CLEAR_FAULT;
    const uint32_t updates = runtime.pipeline.mock.updates;
    const uint32_t session = runtime.motion_session;
    const struct foc_current_context before = runtime.pipeline.current;
    const int status = foc_runtime_h0_step(&runtime, &input);
    printf(
        "rejected_cycle route=%u failure=%u status=%d state=%d action=%d updates=%u "
        "streak=%u faults=%u session=%u\n",
        route,
        failure,
        status,
        runtime.pipeline.safety.state,
        runtime.pipeline.mock.last_request.action,
        runtime.pipeline.mock.updates,
        runtime.pipeline.safety.invalid_sample_streak,
        runtime.pipeline.safety.latched_faults,
        runtime.motion_session
    );
    TEST_ASSERT(runtime.pipeline.mock.output_confirmed);
    if (failure != 0U) {
        TEST_ASSERT(status < 0);
        TEST_ASSERT(runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
        TEST_ASSERT(runtime.pipeline.safety.state != FOC_SAFE_STATE_RUNNING);
        TEST_ASSERT(runtime.pipeline.mock.updates == updates);
        TEST_ASSERT(
            runtime.pipeline.current.integral_v.d == before.integral_v.d &&
            runtime.pipeline.current.integral_v.q == before.integral_v.q
        );
        TEST_ASSERT(runtime.motion_session == session + 1U);
        if (failure == 1U || failure == 2U) {
            TEST_ASSERT(runtime.pipeline.safety.invalid_sample_streak == 1U);
            TEST_ASSERT(runtime.pipeline.safety.latched_faults == 0U);
        }
    } else {
        TEST_ASSERT(runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
        TEST_ASSERT(runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_UPDATE);
        TEST_ASSERT(runtime.pipeline.mock.updates == updates + 1U);
        TEST_ASSERT(
            runtime.motion_session == session && runtime.pipeline.safety.latched_faults == 0U
        );
    }
    if (mailbox) {
        struct foc_runtime_lifecycle_result result;
        TEST_ASSERT(foc_runtime_read_lifecycle_result(&runtime, &result) == 0);
        TEST_ASSERT(
            result.result == FOC_RUNTIME_REQUEST_REJECTED ||
            (failure == 3U && result.result == FOC_RUNTIME_REQUEST_CANCELLED)
        );
        TEST_ASSERT(
            result.request_sequence == 41U && result.action == request.action &&
            result.h0_us == t + 1U && result.applied_session == runtime.motion_session
        );
        const struct foc_runtime_lifecycle_result terminal = result;
        TEST_ASSERT(
            foc_runtime_publish_lifecycle_request(&runtime, &request, t + 1U) ==
            FOC_RUNTIME_ERR_ORDER
        );
        TEST_ASSERT(foc_runtime_read_lifecycle_result(&runtime, &result) == 0);
        TEST_ASSERT(
            result.result == terminal.result && result.h0_us == terminal.h0_us &&
            result.request_sequence == terminal.request_sequence
        );
    }
    return 0;
}

static int test_rejection_safety_matrix(void)
{
    const unsigned failures[] = {1U, 0U, 2U, 3U};
    for (unsigned index = 0U; index < 4U; ++index) {
        const unsigned failure = failures[index];
        for (unsigned route = 0U; route < 8U; ++route) {
            TEST_ASSERT(test_rejected_cycle(route, failure) == 0);
        }
    }
    return 0;
}

static int test_stop_and_recovery(void)
{
    for (unsigned fault = 0U; fault < 2U; ++fault) {
        for (unsigned expired = 0U; expired < 2U; ++expired) {
            struct foc_runtime runtime = {0};
            TEST_ASSERT(start_running(&runtime) == 0);
            const struct foc_runtime_lifecycle_request request = {
                .action = FOC_RUNTIME_ACTION_ARM,
                .request_sequence = 51U,
                .source_session = runtime.motion_session,
                .produced_us = expired ? 400U - 1500U : 302U,
            };
            TEST_ASSERT(foc_runtime_publish_lifecycle_request(&runtime, &request, 302U) == 0);
            const uint32_t updates = runtime.pipeline.mock.updates;
            foc_runtime_request_inhibit(&runtime, fault ? FOC_SAFE_FAULT_INVALID_STATE : 0U);
            TEST_ASSERT(publish_sample(&runtime, 400U, 4U) == 0);
            TEST_ASSERT(service(&runtime, 400U, 4U) == FOC_RUNTIME_ERR_STOP);
            TEST_ASSERT(read_result(&runtime, FOC_RUNTIME_REQUEST_CANCELLED, 51U) == 0);
            TEST_ASSERT(
                runtime.pipeline.mock.updates == updates &&
                runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT
            );
            TEST_ASSERT(
                runtime.pipeline.safety.state ==
                (fault ? FOC_SAFE_STATE_FAULT_LATCHED : FOC_SAFE_STATE_DISABLED)
            );
            if (fault) {
                struct foc_runtime_lifecycle_request again = request;
                again.request_sequence = 52U;
                again.source_session = runtime.motion_session;
                again.produced_us = 500U;
                TEST_ASSERT(foc_runtime_publish_lifecycle_request(&runtime, &again, 500U) == 0);
                TEST_ASSERT(publish_sample(&runtime, 500U, 5U) == 0);
                TEST_ASSERT(service(&runtime, 500U, 5U) < 0);
                TEST_ASSERT(
                    runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED &&
                    runtime.pipeline.mock.updates == updates
                );
            }
        }
    }
    struct foc_runtime runtime = {0};
    TEST_ASSERT(start_running(&runtime) == 0);
    const struct foc_runtime_lifecycle_request request = {
        .action = FOC_RUNTIME_ACTION_ARM,
        .request_sequence = 61U,
        .source_session = runtime.motion_session,
        .produced_us = 302U
    };
    TEST_ASSERT(foc_runtime_publish_lifecycle_request(&runtime, &request, 302U) == 0);
    TEST_ASSERT(publish_sample(&runtime, 400U, 4U) == 0);
    struct foc_runtime_fast_input bad = input_at(400U, 4U);
    bad.adc.valid_mask = 0U;
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &bad) < 0);
    TEST_ASSERT(runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    /* Old command is still before rearm boundary, even with a new sample. */
    struct foc_runtime_foreground_result foreground;
    TEST_ASSERT(foc_runtime_foreground_poll(&runtime, 402U, 0U, 0U, &foreground) >= 0);
    TEST_ASSERT(publish_sample(&runtime, 500U, 5U) == 0);
    struct foc_runtime_fast_input input = input_at(500U, 5U);
    input.action = FOC_RUNTIME_ACTION_ARM;
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &input) < 0);
    TEST_ASSERT(runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    TEST_ASSERT(foc_runtime_foreground_poll(&runtime, 502U, 0U, 0U, &foreground) >= 0);
    const struct foc_pipeline_command fresh = {{0.0f, 1.0f}, 1U, 600U};
    TEST_ASSERT(foc_runtime_publish_command(&runtime, &fresh, 600U) == 0);
    for (uint32_t t = 600U; t <= 800U; t += 100U) {
        TEST_ASSERT(publish_sample(&runtime, t, t / 100U) == 0);
        input = input_at(t, t / 100U);
        input.action = t == 600U ? FOC_RUNTIME_ACTION_ARM
            : t == 700U          ? FOC_RUNTIME_ACTION_START
                                 : FOC_RUNTIME_ACTION_NONE;
        TEST_ASSERT(
            foc_runtime_h0_step(&runtime, &input) == (t == 800U ? FOC_RUNTIME_OK : FOC_RUNTIME_IDLE)
        );
        TEST_ASSERT(foc_runtime_foreground_poll(&runtime, t + 2U, 0U, 0U, &foreground) >= 0);
    }
    TEST_ASSERT(
        runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING &&
        runtime.pipeline.mock.updates == 2U
    );
    TEST_ASSERT(read_result(&runtime, FOC_RUNTIME_REQUEST_REJECTED, 61U) == 0);
    return 0;
}

int main(void)
{
    return test_rejection_safety_matrix() || test_stop_and_recovery() ||
        test_normal_apply_and_stop_cancel() || test_busy_duplicate_and_stale_session() ||
        test_expired_request_and_set_mode_contract();
}

/* ---------------------------------------- end of file ----------------------------------------- */
