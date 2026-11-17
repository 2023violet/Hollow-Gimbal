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

int main(void)
{
    return test_normal_apply_and_stop_cancel() || test_busy_duplicate_and_stale_session() ||
        test_expired_request_and_set_mode_contract();
}

/* ---------------------------------------- end of file ----------------------------------------- */
