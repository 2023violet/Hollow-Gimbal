/**
 * @file    test_foc_remote.c
 * @brief   Command revocation and restricted H0 fault-recovery tests.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Prove that Foreground cannot clear live sampling faults or restore old commands.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
            return 1;                                       \
        }                                                   \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const struct foc_pipeline_config pipeline = {
    .current = {{1, 2, 0, 1, .001f}, {1, 1}, {100, 100}, 2},
    .adc = {.01f, .001f, 1, 2048, 2048, 2048, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {1000, 500, 500, 500, 3},
};

static const struct foc_runtime_config config = {
    .h0 = {100, 20, 10},
    .h1 = {100, 20, 10},
    .supervision = {100, 20, 10},
    .telemetry = {1000, 20, 10},
    .foreground_budget_us = 15,
    .angle_timeout_us = 100,
};

/* Private Helpers -------------------------------------------------------------- */

static void tick(struct foc_runtime *runtime, uint32_t time)
{
    struct foc_runtime_foreground_result result;
    const struct foc_runtime_sensor sensor = {.seq = time, .captured_us = time, .valid = true};
    const struct foc_runtime_fast_input input = {
        .adc = {1200, 2048, 2048, 2048, FOC_DEVICE_SAMPLE_ALL_VALID, time, time},
        .now_us = time,
    };
    (void)foc_runtime_foreground_poll(runtime, time, 0, 0, &result);
    (void)foc_runtime_h1_step(runtime, &sensor, time, 0);
    (void)foc_runtime_h0_step(runtime, &input);
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    struct foc_runtime runtime;
    CHECK(foc_runtime_init(&runtime, &pipeline, &config, 0) == 0);
    const struct foc_pipeline_command command = {{0, .2f}, 1, 0};
    CHECK(foc_runtime_publish_command(&runtime, &command, 0) == 0);
    tick(&runtime, 0);
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_COMMAND_TIMEOUT);
    tick(&runtime, 100);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(foc_runtime_request_timeout_clear(&runtime, 101) == 0);
    tick(&runtime, 200);
    tick(&runtime, 300);
    struct foc_runtime_snapshot view;
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == 0);
    CHECK(view.state == FOC_SAFE_STATE_DISABLED && view.clear_result == FOC_RUNTIME_CLEAR_APPLIED);
    CHECK(!view.command_seen && atomic_load(&runtime.command_slot) >= 2);
    CHECK(runtime.pipeline.rearm_boundary_seen && runtime.pipeline.mock.updates == 0);
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
    tick(&runtime, 400);
    CHECK(foc_runtime_request_timeout_clear(&runtime, 401) == FOC_RUNTIME_ERR_CONTROL);
    tick(&runtime, 500);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    foc_runtime_revoke_command(&runtime);
    CHECK(atomic_load(&runtime.stop_requests) != 0);
    CHECK(foc_runtime_init(&runtime, &pipeline, &config, 0) == 0);
    tick(&runtime, 0);
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_COMMAND_TIMEOUT);
    tick(&runtime, 100);
    CHECK(foc_runtime_request_timeout_clear(&runtime, 101) == 0);
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_SAMPLE_INVALID);
    tick(&runtime, 200);
    tick(&runtime, 300);
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == 0);
    CHECK(
        view.state == FOC_SAFE_STATE_FAULT_LATCHED &&
        view.clear_result == FOC_RUNTIME_CLEAR_REJECTED
    );
    CHECK((view.faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
