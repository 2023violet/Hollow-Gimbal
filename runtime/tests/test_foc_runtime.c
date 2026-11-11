/**
 * @file    test_foc_runtime.c
 * @brief   Deterministic H0, H1, and Foreground runtime acceptance.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Inject single-core preemption at publication and read boundaries and
 * verify the complete closed-gate control and supervision path.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"
#include "foc_test_motor.h"

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

/* Private State ---------------------------------------------------------------- */

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

static struct foc_runtime_fast_input fast_input(uint32_t capture_us, uint32_t seq)
{
    return (struct foc_runtime_fast_input){
        .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, seq, capture_us},
        .now_us = capture_us + 1U,
    };
}

static int publish_angle(struct foc_runtime *runtime, uint32_t capture_us, uint32_t seq)
{
    const struct foc_runtime_sensor sensor = {0.37f, capture_us, seq, true};
    return foc_runtime_h1_step(runtime, &sensor, capture_us, 0U);
}

static int poll(struct foc_runtime *runtime, uint32_t now_us)
{
    struct foc_runtime_foreground_result result;
    return foc_runtime_foreground_poll(runtime, now_us, 0U, 0U, &result);
}

static int
service(struct foc_runtime *runtime, uint32_t time, uint32_t seq, enum foc_runtime_action action)
{
    struct foc_runtime_fast_input input = fast_input(time, seq);
    input.action = action;
    if (publish_angle(runtime, time, seq) != FOC_RUNTIME_OK) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    return foc_runtime_h0_step(runtime, &input);
}

static int start_run(
    struct foc_runtime *runtime,
    const struct foc_runtime_config *config,
    uint32_t base,
    uint32_t seq
)
{
    const struct foc_pipeline_command command = {{0.2f, 1.0f}, seq, base};

    TEST_ASSERT(foc_runtime_init(runtime, &pipeline_config, config, base) == FOC_RUNTIME_OK);
    TEST_ASSERT(foc_runtime_publish_command(runtime, &command, base) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(runtime, base, seq, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_IDLE);
    TEST_ASSERT(poll(runtime, base + 2U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        service(runtime, base + 100U, seq + 1U, FOC_RUNTIME_ACTION_ARM) == FOC_RUNTIME_IDLE
    );
    TEST_ASSERT(poll(runtime, base + 102U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        service(runtime, base + 200U, seq + 2U, FOC_RUNTIME_ACTION_START) == FOC_RUNTIME_IDLE
    );
    TEST_ASSERT(poll(runtime, base + 202U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(runtime, base + 300U, seq + 3U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
    TEST_ASSERT(poll(runtime, base + 302U) == FOC_RUNTIME_OK);
    return 0;
}

struct preemption {
    enum foc_runtime_hook_point point;
    bool armed;
    unsigned int calls;
    int h0_status;
};

static void preempt(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *arg)
{
    struct preemption *plan = arg;
    if (!plan->armed || point != plan->point) {
        return;
    }
    plan->armed = false;
    plan->calls++;
    if (point == FOC_RUNTIME_HOOK_TELEMETRY_READING) {
        plan->h0_status = service(runtime, 400U, 4U, FOC_RUNTIME_ACTION_NONE);
        if (plan->h0_status == FOC_RUNTIME_OK) {
            plan->h0_status = service(runtime, 500U, 5U, FOC_RUNTIME_ACTION_NONE);
        }
    } else {
        const struct foc_runtime_fast_input input = fast_input(400U, 4U);
        plan->h0_status = foc_runtime_h0_step(runtime, &input);
    }
}

static int test_preemption(void)
{
    const enum foc_runtime_hook_point points[] = {
        FOC_RUNTIME_HOOK_SENSOR_WRITING,
        FOC_RUNTIME_HOOK_COMMAND_WRITING,
        FOC_RUNTIME_HOOK_TELEMETRY_READING,
    };
    for (unsigned int index = 0U; index < sizeof(points) / sizeof(points[0]); ++index) {
        struct foc_runtime runtime = {0};
        struct preemption plan = {.point = points[index]};
        struct foc_runtime_config config = runtime_config;
        config.hook = preempt;
        config.hook_arg = &plan;
        TEST_ASSERT(start_run(&runtime, &config, 0U, 0U) == 0);
        plan.armed = true;

        if (plan.point == FOC_RUNTIME_HOOK_SENSOR_WRITING) {
            TEST_ASSERT(publish_angle(&runtime, 400U, 4U) == FOC_RUNTIME_OK);
            TEST_ASSERT(plan.h0_status == FOC_RUNTIME_ERR_CONTROL);
            TEST_ASSERT(runtime.pipeline.mock.updates == 1U);
            TEST_ASSERT(runtime.pipeline.last_reason == FOC_PIPELINE_ERR_SAMPLE);
        } else if (plan.point == FOC_RUNTIME_HOOK_COMMAND_WRITING) {
            const struct foc_pipeline_command command = {{0.7f, -1.0f}, 1U, 400U};
            TEST_ASSERT(publish_angle(&runtime, 400U, 4U) == FOC_RUNTIME_OK);
            TEST_ASSERT(foc_runtime_publish_command(&runtime, &command, 400U) == FOC_RUNTIME_OK);
            TEST_ASSERT(plan.h0_status == FOC_RUNTIME_OK);
            TEST_ASSERT(runtime.pipeline.target_a.d == 0.2f && runtime.pipeline.target_a.q == 1.0f);
            TEST_ASSERT(service(&runtime, 500U, 5U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
            TEST_ASSERT(
                runtime.pipeline.target_a.d == 0.7f && runtime.pipeline.target_a.q == -1.0f
            );
        } else {
            struct foc_runtime_snapshot snapshot = {.updates = UINT32_MAX};
            TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_ERR_BUSY);
            TEST_ASSERT(snapshot.updates == UINT32_MAX && plan.h0_status == FOC_RUNTIME_OK);
            TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
            TEST_ASSERT(snapshot.updates == 3U);
            TEST_ASSERT(snapshot.compare[0] == runtime.pipeline.mock.registers.channel1_compare);
            TEST_ASSERT(snapshot.compare[1] == runtime.pipeline.mock.registers.channel2_compare);
            TEST_ASSERT(snapshot.compare[2] == runtime.pipeline.mock.registers.channel3_compare);
        }
        TEST_ASSERT(plan.calls == 1U);
    }
    return 0;
}

static int test_stalls(void)
{
    struct foc_runtime fast_stall = {0};
    struct foc_runtime foreground_stall = {0};
    struct foc_runtime_snapshot snapshot;

    TEST_ASSERT(start_run(&fast_stall, &runtime_config, 0U, 0U) == 0);
    TEST_ASSERT(start_run(&foreground_stall, &runtime_config, 0U, 0U) == 0);
    for (uint32_t tick = 4U; tick <= 7U; ++tick) {
        TEST_ASSERT(poll(&fast_stall, tick * 100U + 2U) == FOC_RUNTIME_OK);
        const int expected = tick == 7U ? FOC_RUNTIME_ERR_CONTROL : FOC_RUNTIME_OK;
        TEST_ASSERT(
            service(&foreground_stall, tick * 100U, tick, FOC_RUNTIME_ACTION_NONE) == expected
        );
    }
    TEST_ASSERT(foc_runtime_read_snapshot(&fast_stall, &snapshot) == FOC_RUNTIME_OK);
    TEST_ASSERT(snapshot.inhibit_pending && !snapshot.output_confirmed);
    TEST_ASSERT(snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT);
    TEST_ASSERT((snapshot.faults & FOC_SAFE_FAULT_FAST_HEARTBEAT) != 0U);
    TEST_ASSERT(fast_stall.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    TEST_ASSERT(
        foreground_stall.pipeline.safety.latched_faults == FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT
    );
    TEST_ASSERT(foreground_stall.pipeline.mock.updates == 4U);
    TEST_ASSERT(foreground_stall.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
    return 0;
}

static void
fatal_during_read(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *arg)
{
    bool *armed = arg;
    if (point == FOC_RUNTIME_HOOK_TELEMETRY_READING && *armed) {
        *armed = false;
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_FATAL_RESET_REQUIRED);
        const struct foc_runtime_fast_input input = {.now_us = 503U};
        (void)foc_runtime_h0_step(runtime, &input);
    }
}

static int test_watchdog(void)
{
    struct foc_runtime runtime = {0};
    struct foc_runtime_foreground_result result;
    TEST_ASSERT(start_run(&runtime, &runtime_config, 0U, 0U) == 0);
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_INVALID_STATE);
    TEST_ASSERT(service(&runtime, 400U, 4U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_ERR_STOP);
    TEST_ASSERT(foc_runtime_foreground_poll(&runtime, 402U, 0U, 0U, &result) == FOC_RUNTIME_OK);
    TEST_ASSERT(result.supervised && result.watchdog_permitted);
    TEST_ASSERT(service(&runtime, 500U, 5U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_ERR_CONTROL);
    TEST_ASSERT(foc_runtime_foreground_poll(&runtime, 502U, 0U, 0U, &result) == FOC_RUNTIME_OK);
    TEST_ASSERT(result.watchdog_permitted);
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_FATAL_RESET_REQUIRED);
    TEST_ASSERT(service(&runtime, 600U, 6U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_ERR_STOP);
    TEST_ASSERT(foc_runtime_foreground_poll(&runtime, 602U, 0U, 0U, &result) == FOC_RUNTIME_OK);
    TEST_ASSERT(result.supervised && !result.watchdog_permitted);

    bool armed = false;
    struct foc_runtime_config config = runtime_config;
    config.hook = fatal_during_read;
    config.hook_arg = &armed;
    TEST_ASSERT(start_run(&runtime, &config, 0U, 0U) == 0);
    TEST_ASSERT(service(&runtime, 400U, 4U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
    TEST_ASSERT(poll(&runtime, 402U) == FOC_RUNTIME_OK);
    armed = true;
    TEST_ASSERT(foc_runtime_foreground_poll(&runtime, 502U, 0U, 0U, &result) == FOC_RUNTIME_OK);
    TEST_ASSERT(!armed && !result.telemetry_ready);
    TEST_ASSERT(
        (runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_FATAL_RESET_REQUIRED) != 0U
    );
    TEST_ASSERT(result.supervised && !result.watchdog_permitted);
    return 0;
}

static void
stop_at_boundary(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *arg)
{
    struct preemption *plan = arg;
    if (plan->armed && point == plan->point) {
        plan->armed = false;
        plan->calls++;
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_INVALID_STATE);
        foc_runtime_request_inhibit(runtime, FOC_SAFE_FAULT_WDT);
    }
}

static int test_stop_and_recovery(void)
{
    const enum foc_runtime_hook_point points[] = {
        FOC_RUNTIME_HOOK_SENSOR_WRITING,
        FOC_RUNTIME_HOOK_COMMAND_WRITING,
        FOC_RUNTIME_HOOK_H0_INPUTS,
        FOC_RUNTIME_HOOK_BEFORE_SUBMIT,
        FOC_RUNTIME_HOOK_TELEMETRY_READING,
    };
    const uint32_t faults = FOC_SAFE_FAULT_INVALID_STATE | FOC_SAFE_FAULT_WDT;
    for (unsigned int index = 0U; index < sizeof(points) / sizeof(points[0]); ++index) {
        struct foc_runtime runtime = {0};
        struct preemption plan = {.point = points[index]};
        struct foc_runtime_config config = runtime_config;
        struct foc_runtime_snapshot snapshot;
        config.hook = stop_at_boundary;
        config.hook_arg = &plan;
        TEST_ASSERT(start_run(&runtime, &config, 0U, 0U) == 0);
        const struct foc_vector_dq integral = runtime.pipeline.current.integral_v;
        plan.armed = true;
        TEST_ASSERT(publish_angle(&runtime, 400U, 4U) == FOC_RUNTIME_OK);
        const struct foc_pipeline_command command = {{0.0f, -1.0f}, 1U, 400U};
        TEST_ASSERT(foc_runtime_publish_command(&runtime, &command, 400U) == FOC_RUNTIME_OK);
        if (plan.point == FOC_RUNTIME_HOOK_TELEMETRY_READING) {
            TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
            TEST_ASSERT(snapshot.inhibit_pending && !snapshot.output_confirmed);
            TEST_ASSERT(snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.faults == faults);
        }
        const struct foc_runtime_fast_input input = fast_input(400U, 4U);
        TEST_ASSERT(foc_runtime_h0_step(&runtime, &input) == FOC_RUNTIME_ERR_STOP);
        TEST_ASSERT(plan.calls == 1U && runtime.pipeline.mock.updates == 1U);
        TEST_ASSERT(runtime.pipeline.current.integral_v.d == integral.d);
        TEST_ASSERT(runtime.pipeline.current.integral_v.q == integral.q);
        TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
        TEST_ASSERT(snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED && snapshot.faults == faults);
        TEST_ASSERT(snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && snapshot.output_confirmed);
        TEST_ASSERT(
            !snapshot.inhibit_pending && snapshot.compare[0] == 0U && snapshot.compare[1] == 0U &&
            snapshot.compare[2] == 0U
        );
        TEST_ASSERT(poll(&runtime, 402U) == FOC_RUNTIME_OK);
        TEST_ASSERT(
            service(&runtime, 500U, 5U, FOC_RUNTIME_ACTION_CLEAR_FAULT) == FOC_RUNTIME_IDLE
        );
        TEST_ASSERT(runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
        TEST_ASSERT(runtime.pipeline.current.integral_v.q == integral.q);
        TEST_ASSERT(poll(&runtime, 502U) == FOC_RUNTIME_OK);
        TEST_ASSERT(
            service(&runtime, 600U, 6U, FOC_RUNTIME_ACTION_START) == FOC_RUNTIME_ERR_CONTROL
        );
        TEST_ASSERT(foc_runtime_publish_command(&runtime, &command, 600U) == FOC_RUNTIME_ERR_ORDER);
        TEST_ASSERT(poll(&runtime, 602U) == FOC_RUNTIME_OK);
        const struct foc_pipeline_command fresh = {{0.0f, -1.0f}, 2U, 700U};
        TEST_ASSERT(foc_runtime_publish_command(&runtime, &fresh, 700U) == FOC_RUNTIME_OK);
        TEST_ASSERT(service(&runtime, 700U, 7U, FOC_RUNTIME_ACTION_ARM) == FOC_RUNTIME_IDLE);
        TEST_ASSERT(poll(&runtime, 702U) == FOC_RUNTIME_OK);
        TEST_ASSERT(service(&runtime, 800U, 8U, FOC_RUNTIME_ACTION_START) == FOC_RUNTIME_IDLE);
        TEST_ASSERT(
            runtime.pipeline.current.integral_v.d == 0.0f &&
            runtime.pipeline.current.integral_v.q == 0.0f
        );
        TEST_ASSERT(service(&runtime, 900U, 9U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
        TEST_ASSERT(
            runtime.pipeline.mock.updates == 2U && runtime.pipeline.current.integral_v.q < 0.0f
        );
    }
    return 0;
}

static int test_stop_after_admission(void)
{
    struct foc_runtime runtime = {0};
    struct preemption plan = {.point = FOC_RUNTIME_HOOK_AFTER_CONTROL};
    struct foc_runtime_config config = runtime_config;
    struct foc_runtime_snapshot snapshot;
    config.hook = stop_at_boundary;
    config.hook_arg = &plan;

    TEST_ASSERT(start_run(&runtime, &config, 0U, 0U) == 0);
    const float previous = runtime.pipeline.current.integral_v.q;
    plan.armed = true;
    TEST_ASSERT(service(&runtime, 400U, 4U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
    TEST_ASSERT(plan.calls == 1U && runtime.pipeline.current.integral_v.q > previous);
    TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    TEST_ASSERT(snapshot.updates == 2U && snapshot.output_confirmed);
    TEST_ASSERT(
        snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED
    );
    TEST_ASSERT(
        snapshot.compare[0] == 0U && snapshot.compare[1] == 0U && snapshot.compare[2] == 0U
    );
    return 0;
}

static int test_timing(void)
{
    struct foc_runtime runtime = {0};
    struct foc_runtime_config config = runtime_config;
    struct foc_runtime_snapshot snapshot;
    struct foc_runtime_foreground_result foreground;
    config.h0.max_delay_us = 160U;
    config.h1.max_delay_us = 160U;
    TEST_ASSERT(start_run(&runtime, &config, 0U, 0U) == 0);
    const float previous_iq = runtime.pipeline.current.integral_v.q;
    TEST_ASSERT(poll(&runtime, 402U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(&runtime, 500U, 5U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
    TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    TEST_ASSERT(snapshot.interval_us == 200U && snapshot.missed == 1U && snapshot.delay_us == 101U);
    TEST_ASSERT(runtime.h0.deadline_us == 400U && snapshot.trigger_us == 501U);
    TEST_ASSERT(runtime.h0.due_count == 5U && snapshot.updates == 2U);
    TEST_ASSERT(fabsf(runtime.pipeline.current.integral_v.q - previous_iq - 0.1f) < 1.0e-6f);
    const struct foc_runtime_fast_input duplicate_trigger = fast_input(500U, 5U);
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &duplicate_trigger) == FOC_RUNTIME_IDLE);
    TEST_ASSERT(runtime.h0.due_count == 5U && runtime.pipeline.mock.updates == 2U);
    TEST_ASSERT(foc_runtime_foreground_poll(&runtime, 502U, 8U, 8U, &foreground) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        foreground.supervised && foreground.watchdog_permitted && !foreground.telemetry_ready
    );
    TEST_ASSERT(runtime.telemetry_dropped == 1U && runtime.telemetry.budget_count == 0U);
    TEST_ASSERT(publish_angle(&runtime, 600U, 6U) == FOC_RUNTIME_OK);
    struct foc_runtime_fast_input over_budget = fast_input(600U, 6U);
    over_budget.cost_us = 11U;
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &over_budget) == FOC_RUNTIME_ERR_TIMING);
    TEST_ASSERT(runtime.h0.budget_count == 1U && runtime.pipeline.mock.updates == 2U);
    TEST_ASSERT(runtime.pipeline.safety.last_fast_heartbeat_us == 501U);
    TEST_ASSERT(runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_FAST_HEARTBEAT);
    TEST_ASSERT(
        foc_runtime_foreground_poll(&runtime, 725U, 0U, 0U, &foreground) == FOC_RUNTIME_ERR_TIMING
    );
    TEST_ASSERT(!foreground.supervised && !foreground.watchdog_permitted);
    TEST_ASSERT(runtime.supervision.deadline_us == 600U && runtime.supervision.trigger_us == 725U);
    TEST_ASSERT(runtime.supervision.missed == 1U && runtime.supervision.late_count == 1U);
    TEST_ASSERT(atomic_load(&runtime.foreground_us) == 502U);
    TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        snapshot.inhibit_pending && (snapshot.faults & FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT) != 0U
    );
    return 0;
}

static int test_angle_stall_and_time(void)
{
    struct foc_runtime runtime = {0};
    struct foc_runtime_config config = runtime_config;
    config.h1.max_delay_us = 250U;
    TEST_ASSERT(start_run(&runtime, &config, 0U, 0U) == 0);
    for (uint32_t seq = 4U; seq <= 5U; ++seq) {
        const struct foc_runtime_fast_input input = fast_input(seq * 100U, seq);
        TEST_ASSERT(foc_runtime_h0_step(&runtime, &input) == FOC_RUNTIME_ERR_CONTROL);
        TEST_ASSERT(runtime.pipeline.last_reason == FOC_PIPELINE_ERR_SAMPLE);
        TEST_ASSERT(poll(&runtime, seq * 100U + 2U) == FOC_RUNTIME_OK);
    }
    TEST_ASSERT(runtime.pipeline.safety.invalid_sample_streak == 2U);
    TEST_ASSERT(service(&runtime, 600U, 6U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_IDLE);
    TEST_ASSERT(runtime.h1.missed == 2U && runtime.pipeline.safety.invalid_sample_streak == 0U);
    TEST_ASSERT(
        runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED &&
        runtime.pipeline.mock.updates == 1U
    );

    const uint32_t base = UINT32_MAX - 299U;
    TEST_ASSERT(start_run(&runtime, &runtime_config, base, UINT32_MAX - 2U) == 0);
    TEST_ASSERT(
        runtime.adc.seq == 0U && runtime.adc.produced_us == 0U && runtime.interval_us == 100U
    );
    const struct foc_pipeline_command wrapped = {{0.0f, -1.0f}, 0U, 100U};
    TEST_ASSERT(foc_runtime_publish_command(&runtime, &wrapped, 100U) == FOC_RUNTIME_OK);
    TEST_ASSERT(foc_runtime_publish_command(&runtime, &wrapped, 100U) == FOC_RUNTIME_ERR_ORDER);
    TEST_ASSERT(service(&runtime, 100U, 1U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
    TEST_ASSERT(runtime.pipeline.safety.command_seq == 0U);
    TEST_ASSERT(poll(&runtime, 102U) == FOC_RUNTIME_OK);
    const struct foc_runtime_sensor future_angle = {0.37f, 201U, 2U, true};
    TEST_ASSERT(foc_runtime_h1_step(&runtime, &future_angle, 200U, 0U) == FOC_RUNTIME_ERR_ORDER);
    struct foc_runtime_fast_input input = fast_input(200U, 2U);
    input.adc.sample_tick = 100U;
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &input) == FOC_RUNTIME_ERR_ORDER);
    TEST_ASSERT(runtime.pipeline.mock.updates == 2U && runtime.adc.seq == 1U);
    input = fast_input(300U, 3U);
    input.adc.sample_tick = 302U;
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &input) == FOC_RUNTIME_ERR_ORDER);
    input.now_us += UINT32_C(0x80000000);
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &input) == FOC_RUNTIME_ERR_ORDER);
    TEST_ASSERT(runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_FAST_HEARTBEAT);
    TEST_ASSERT(runtime.pipeline.mock.updates == 2U);
    return 0;
}

struct model_result {
    struct foc_runtime_snapshot final;
    struct foc_vector_dq integral;
    uint32_t trace;
    double final_q[2];
    double error[2];
};

static int run_closed_loop(struct model_result *result)
{
    struct foc_runtime runtime = {0};
    struct foc_runtime_config config = runtime_config;
    struct foc_pipeline_config settings = pipeline_config;
    struct foc_test_motor motor = foc_test_motor_init();
    struct foc_runtime_snapshot snapshot;
    const struct foc_pipeline_command initial = {{0.0f, 1.0f}, 0U, 0U};
    uint32_t tick = 2U;
    uint32_t command_seq = 0U;

    config.supervision.period_us = 1000U;
    config.telemetry.period_us = 10000U;
    settings.safety.foreground_heartbeat_timeout_us = 1200U;
    settings.adc.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
    settings.pwm.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
    TEST_ASSERT(foc_runtime_init(&runtime, &settings, &config, 0U) == FOC_RUNTIME_OK);
    TEST_ASSERT(foc_runtime_publish_command(&runtime, &initial, 0U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(&runtime, 0U, 0U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_IDLE);
    TEST_ASSERT(poll(&runtime, 2U) == FOC_RUNTIME_OK);
    TEST_ASSERT(service(&runtime, 100U, 1U, FOC_RUNTIME_ACTION_ARM) == FOC_RUNTIME_IDLE);
    TEST_ASSERT(service(&runtime, 200U, 2U, FOC_RUNTIME_ACTION_START) == FOC_RUNTIME_IDLE);
    *result = (struct model_result){.trace = UINT32_C(2166136261)};
    for (unsigned int stage = 0U; stage < 2U; ++stage) {
        const float iq_target = stage == 0U ? 1.0f : -1.0f;
        for (unsigned int index = 0U; index < 600U; ++index) {
            tick++;
            const uint32_t captured_us = tick * 100U;
            struct foc_runtime_fast_input input = fast_input(captured_us, tick);
            TEST_ASSERT(publish_angle(&runtime, captured_us, tick) == FOC_RUNTIME_OK);
            if (index % 5U == 0U) {
                const struct foc_pipeline_command command =
                    {{0.0f, iq_target}, ++command_seq, captured_us};
                TEST_ASSERT(
                    foc_runtime_publish_command(&runtime, &command, captured_us) == FOC_RUNTIME_OK
                );
            }
            foc_test_motor_encode(&motor, &input.adc);
            TEST_ASSERT(foc_runtime_h0_step(&runtime, &input) == FOC_RUNTIME_OK);
            TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
            TEST_ASSERT(snapshot.output_confirmed && snapshot.action == FOC_DEVICE_OUTPUT_UPDATE);
            TEST_ASSERT(snapshot.state == FOC_SAFE_STATE_RUNNING && snapshot.faults == 0U);
            TEST_ASSERT(snapshot.updates == tick - 2U && snapshot.interval_us == 100U);
            TEST_ASSERT(runtime.pipeline.mock.registers.bridge_enabled == 0U);
            TEST_ASSERT(fabs(runtime.pipeline.measured_dq_a.q - motor.iq) <= 0.002);
            TEST_ASSERT(foc_test_motor_apply(&motor, snapshot.compare) <= 2.003);
            for (unsigned int channel = 0U; channel < 3U; ++channel) {
                result->trace = (result->trace ^ snapshot.compare[channel]) * UINT32_C(16777619);
            }
            if (index >= 500U) {
                result->error[stage] =
                    fmax(result->error[stage], fmax(fabs(motor.id), fabs(motor.iq - iq_target)));
            }
            struct foc_runtime_foreground_result foreground;
            const int fg_status =
                foc_runtime_foreground_poll(&runtime, captured_us + 2U, 0U, 0U, &foreground);
            TEST_ASSERT(fg_status == FOC_RUNTIME_OK || fg_status == FOC_RUNTIME_IDLE);
            if (foreground.supervised) {
                TEST_ASSERT(foreground.watchdog_permitted);
            }
        }
        TEST_ASSERT(result->error[stage] <= 0.01);
        result->final_q[stage] = motor.iq;
    }
    result->integral = runtime.pipeline.current.integral_v;
    /* A late trigger consumes no model step and never replays the skipped deadline. */
    const struct foc_runtime_fast_input late = fast_input((tick + 2U) * 100U, tick + 2U);
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &late) == FOC_RUNTIME_ERR_TIMING);
    TEST_ASSERT(foc_runtime_read_snapshot(&runtime, &result->final) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        result->final.output_confirmed && result->final.action == FOC_DEVICE_OUTPUT_INHIBIT
    );
    TEST_ASSERT(result->final.updates == 1200U && result->final.missed == 1U);
    TEST_ASSERT(result->final.faults == FOC_SAFE_FAULT_FAST_HEARTBEAT);
    TEST_ASSERT(runtime.pipeline.current.integral_v.d == result->integral.d);
    TEST_ASSERT(runtime.pipeline.current.integral_v.q == result->integral.q);
    return 0;
}

static int test_closed_loop_and_replay(void)
{
    struct model_result first;
    struct model_result replay;
    TEST_ASSERT(run_closed_loop(&first) == 0);
    TEST_ASSERT(run_closed_loop(&replay) == 0);
    TEST_ASSERT(first.trace == replay.trace && first.final.updates == replay.final.updates);
    TEST_ASSERT(
        first.final.state == replay.final.state && first.final.faults == replay.final.faults
    );
    TEST_ASSERT(
        first.final.action == replay.final.action && first.final.missed == replay.final.missed
    );
    TEST_ASSERT(first.final.compare[0] == replay.final.compare[0]);
    TEST_ASSERT(first.final.compare[1] == replay.final.compare[1]);
    TEST_ASSERT(first.final.compare[2] == replay.final.compare[2]);
    TEST_ASSERT(first.integral.d == replay.integral.d && first.integral.q == replay.integral.q);
    for (unsigned int stage = 0U; stage < 2U; ++stage) {
        TEST_ASSERT(
            first.final_q[stage] == replay.final_q[stage] &&
            first.error[stage] == replay.error[stage]
        );
        printf(
            "runtime stage=%u iq=%.6f A window_error=%.6f A\n",
            stage,
            first.final_q[stage],
            first.error[stage]
        );
    }
    printf(
        "runtime final=INHIBIT updates=%u missed=%u replay=%08x\n",
        (unsigned int)first.final.updates,
        (unsigned int)first.final.missed,
        (unsigned int)first.trace
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    return test_preemption() || test_stalls() || test_watchdog() || test_stop_and_recovery() ||
        test_stop_after_admission() || test_timing() || test_angle_stall_and_time() ||
        test_closed_loop_and_replay();
}

/* ---------------------------------------- end of file ----------------------------------------- */
