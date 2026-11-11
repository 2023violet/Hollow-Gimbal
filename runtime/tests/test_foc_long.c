/**
 * @file    test_foc_long.c
 * @brief   Deterministic long Runtime event stream and stop transaction.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Replay a bounded 100 us current-loop schedule and retain a compact trace
 * fingerprint, counters and final controller state.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                                        \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Types ---------------------------------------------------------------- */

struct replay_result {
    uint32_t hash;
    uint32_t updates;
    uint32_t due;
    uint32_t missed;
    uint32_t late;
    uint32_t budget;
    struct foc_vector_dq integral;
};

/* Private Constants ------------------------------------------------------------ */

static const struct foc_pipeline_config pipeline_config = {
    .current = {{1.0f, 2.0f, 0.0f, 1.0f, .001f}, {1.0f, 1.5f}, {500.0f, 500.0f}, 5.0f},
    .adc = {.01f, .001f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
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

static uint32_t fold(uint32_t hash, uint32_t value)
{
    hash ^= value;
    return hash * UINT32_C(16777619);
}

static int run_replay(struct replay_result *result)
{
    struct foc_runtime runtime = {0};
    struct foc_runtime_foreground_result foreground;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &runtime_config, 0U) == FOC_RUNTIME_OK);
    struct foc_pipeline_command command = {{0.2f, 1.0f}, 0U, 0U};
    CHECK(foc_runtime_publish_command(&runtime, &command, 0U) == FOC_RUNTIME_OK);
    const struct foc_runtime_sensor initial_sensor = {.theta_elec_rad = .37f, .valid = true};
    const struct foc_runtime_fast_input initial_input = {
        .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, 0U, 0U},
        .now_us = 1U,
    };
    CHECK(foc_runtime_h1_step(&runtime, &initial_sensor, 0U, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_h0_step(&runtime, &initial_input) == FOC_RUNTIME_IDLE);
    CHECK(foc_runtime_foreground_poll(&runtime, 2U, 0U, 0U, &foreground) == FOC_RUNTIME_OK);
    for (uint32_t cycle = 1U; cycle <= 100000U; ++cycle) {
        const uint32_t now_us = cycle * 100U;
        if (cycle % 10U == 0U) {
            command.seq++;
            command.produced_us = now_us;
            CHECK(foc_runtime_publish_command(&runtime, &command, now_us) == FOC_RUNTIME_OK);
        }
        const struct foc_runtime_sensor sensor = {
            .theta_elec_rad = .37f,
            .captured_us = now_us,
            .seq = cycle,
            .valid = true,
        };
        const struct foc_runtime_fast_input input = {
            .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, cycle, now_us},
            .now_us = now_us + 1U,
            .action = cycle == 1U ? FOC_RUNTIME_ACTION_ARM
                : cycle == 2U     ? FOC_RUNTIME_ACTION_START
                                  : FOC_RUNTIME_ACTION_NONE,
        };
        CHECK(foc_runtime_h1_step(&runtime, &sensor, now_us, 0U) == FOC_RUNTIME_OK);
        const int h0_status = foc_runtime_h0_step(&runtime, &input);
        if (h0_status != FOC_RUNTIME_OK && h0_status != FOC_RUNTIME_IDLE) {
            fprintf(
                stderr,
                "long h0 cycle=%u status=%d pipeline=%d state=%d faults=%u\n",
                cycle,
                h0_status,
                runtime.pipeline_status,
                runtime.pipeline.safety.state,
                runtime.pipeline.safety.latched_faults
            );
            return 1;
        }
        CHECK(
            foc_runtime_foreground_poll(&runtime, now_us + 2U, 0U, 0U, &foreground) ==
            FOC_RUNTIME_OK
        );
        struct foc_runtime_snapshot snapshot;
        CHECK(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
        result->hash = fold(result->hash, snapshot.updates);
        result->hash = fold(result->hash, snapshot.compare[0]);
        result->hash = fold(result->hash, snapshot.compare[1]);
        result->hash = fold(result->hash, snapshot.compare[2]);
    }
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    CHECK(runtime.h0.missed == 0U && runtime.h0.late_count == 0U && runtime.h0.budget_count == 0U);
    CHECK(runtime.supervision.late_count == 0U && runtime.supervision.budget_count == 0U);
    result->updates = runtime.pipeline.mock.updates;
    result->due = runtime.h0.due_count;
    result->missed = runtime.h0.missed;
    result->late = runtime.h0.late_count;
    result->budget = runtime.h0.budget_count;
    result->integral = runtime.pipeline.current.integral_v;
    const struct foc_runtime_fast_input stop = {.now_us = 10000001U};
    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_WDT);
    CHECK(foc_runtime_h0_step(&runtime, &stop) == FOC_RUNTIME_ERR_STOP);
    CHECK(runtime.pipeline.mock.updates == result->updates);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(runtime.pipeline.current.integral_v.d == result->integral.d);
    CHECK(runtime.pipeline.current.integral_v.q == result->integral.q);
    printf(
        "long replay hash=%08x updates=%u due=%u missed=%u late=%u budget=%u\n",
        result->hash,
        result->updates,
        result->due,
        result->missed,
        result->late,
        result->budget
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    struct replay_result first = {0};
    struct replay_result second = {0};
    CHECK(run_replay(&first) == 0);
    CHECK(run_replay(&second) == 0);
    CHECK(memcmp(&first, &second, sizeof first) == 0);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
