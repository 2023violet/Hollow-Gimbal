/**
 * @file    test_foc_runtime_thermal.c
 * @brief   Runtime I²t fold-back, overload latch, board derating and temperature faults.
 * @version v0.1.0
 * @date    2026.11.25
 *
 * Test role
 * ---------
 * Drive CURRENT mode with a fixed 1 A phase-current reading against a 0.5 A rated
 * current and check the I²t timeline, the fold-back ceiling, the OVERLOAD latch and
 * its cool-down before FAULT_CLEAR, the sustained-current basis, the tuning range,
 * the board derating ceiling and the latched board temperature faults.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include <math.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Constants ------------------------------------------------------------ */

static const struct foc_pipeline_config pipeline_config = {
    .current = {{1.0f, 2.0f, 0.0f, 1.0f, 0.001f}, {1.0f, 1.5f}, {500.0f, 500.0f}, 5.0f},
    .adc = {0.01f, 0.001f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {1500U, 800U, 350U, 350U, 3U},
};

static const struct foc_runtime_config base_config = {
    .h0 = {100U, 20U, 10U},
    .h1 = {100U, 20U, 10U},
    .supervision = {100U, 20U, 10U},
    .telemetry = {500U, 20U, 10U},
    .foreground_budget_us = 15U,
    .angle_timeout_us = 200U,
    /* The trips derive over-speed from the speed limit, even without motion. */
    .motion = {.position = {4.0f, 12.0f}},
    .trip_enabled = true,
    .trips = {8.0f, 30.0f, 6.0f, 0.0f, 1000U, 0U, 0U},
    .profile = {.rated_current_a = 0.5f, .thermal_tau_s = 1.0f},
};

/* Private Helpers -------------------------------------------------------------- */

struct board {
    float derate;
    uint32_t faults;
    bool command;
};

/* One H1 + H0 + Foreground period with a 1 A phase-current reading. */
static int tick(
    struct foc_runtime *runtime,
    const struct board *board,
    uint32_t t,
    enum foc_runtime_action action
)
{
    const uint32_t seq = t / 100U + 1U;
    if (board->command) {
        const struct foc_pipeline_command command = {{0.0f, 1.0f}, seq, t};
        (void)foc_runtime_publish_command(runtime, &command, t);
    }
    const struct foc_runtime_sensor sensor = {0.37f, t, seq, true};
    (void)foc_runtime_h1_step(runtime, &sensor, t, 0U);
    const struct foc_runtime_fast_input input = {
        .adc = {1200U, 3048U, 1548U, 1548U, FOC_DEVICE_SAMPLE_ALL_VALID, seq, t},
        .now_us = t + 1U,
        .action = action,
        .thermal_derate = board->derate,
        .thermal_faults = board->faults,
    };
    const int status = foc_runtime_h0_step(runtime, &input);
    struct foc_runtime_foreground_result result;
    (void)foc_runtime_foreground_poll(runtime, t + 2U, 0U, 0U, &result);
    return status;
}

static int start(struct foc_runtime *runtime, const struct board *board, uint32_t *t)
{
    (void)tick(runtime, board, *t, FOC_RUNTIME_ACTION_NONE);
    *t += 100U;
    (void)tick(runtime, board, *t, FOC_RUNTIME_ACTION_ARM);
    *t += 100U;
    (void)tick(runtime, board, *t, FOC_RUNTIME_ACTION_START);
    *t += 100U;
    CHECK(tick(runtime, board, *t, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_OK);
    CHECK(runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    *t += 100U;
    return 0;
}

static void thermal(struct foc_runtime *runtime, float *theta, float *limit)
{
    (void)foc_runtime_read_thermal(runtime, theta, limit);
}

/* Test Cases ------------------------------------------------------------------- */

static int test_overload(void)
{
    struct foc_runtime runtime;
    struct board board = {0.0f, 0U, true};
    float theta;
    float limit;
    uint32_t t = 100U;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &base_config, 100U) == FOC_RUNTIME_OK);
    /* With I²t the sustained rule moves off the rated current. */
    CHECK(fabsf(runtime.protect.sustained_current_a - 4.75f) < 1e-6f);
    thermal(&runtime, &theta, &limit);
    CHECK(theta == 0.0f && limit == 5.0f);
    CHECK(start(&runtime, &board, &t) == 0);

    /* Twice the rated current: θ = 4 (1 − e^(−t/τ)) reaches 1 near 288 ms ... */
    const uint32_t started = t;
    while (theta < 1.0f && t - started < 1000000U) {
        (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
        thermal(&runtime, &theta, &limit);
        t += 100U;
    }
    CHECK(fabsf((float)(t - started) - 287700.0f) < 3000.0f);
    CHECK(limit == 5.0f);
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    thermal(&runtime, &theta, &limit);
    CHECK(limit == 0.5f && runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    t += 100U;

    /* ... and, as the reading ignores the fold-back, 1.25 near 375 ms latches OVERLOAD. */
    while (runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING && t - started < 1000000U) {
        (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
        t += 100U;
    }
    CHECK(fabsf((float)(t - started) - 375500.0f) < 3000.0f);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_OVERLOAD);

    /* Inhibited, θ decays; FAULT_CLEAR waits until it is below 1. */
    board.command = false;
    foc_runtime_revoke_command(&runtime);
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    CHECK(foc_runtime_request_timeout_clear(&runtime, t - 1U) == FOC_RUNTIME_OK);
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    thermal(&runtime, &theta, &limit);
    CHECK(theta > 1.0f && limit == 0.5f);
    while (theta >= 1.0f && t - started < 2000000U) {
        (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
        thermal(&runtime, &theta, &limit);
        t += 100U;
    }
    /* 1.25 e^(−t/τ) = 1 after τ ln 1.25 ≈ 223 ms. */
    CHECK(theta < 1.0f);
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    thermal(&runtime, &theta, &limit);
    CHECK(limit == 5.0f);
    CHECK(foc_runtime_request_timeout_clear(&runtime, t - 1U) == FOC_RUNTIME_OK);
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    struct foc_runtime_snapshot view;
    CHECK(foc_runtime_read_snapshot(&runtime, &view) == FOC_RUNTIME_OK);
    CHECK(view.state == FOC_SAFE_STATE_DISABLED && view.clear_result == FOC_RUNTIME_CLEAR_APPLIED);
    return 0;
}

static int test_configuration(void)
{
    struct foc_runtime runtime;
    struct foc_runtime_config config = base_config;
    struct board board = {0.0f, 0U, true};
    float theta;
    float limit;
    uint32_t t = 100U;

    /* Without τ the rated current keeps the timed sustained rule and θ stays 0. */
    config.profile.thermal_tau_s = 0.0f;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &config, 100U) == FOC_RUNTIME_OK);
    CHECK(runtime.protect.sustained_current_a == 0.5f);
    CHECK(start(&runtime, &board, &t) == 0);
    for (int i = 0; i < 50; ++i) {
        (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
        t += 100U;
    }
    thermal(&runtime, &theta, &limit);
    CHECK(theta == 0.0f && limit == 5.0f);

    /* Protection disabled turns I²t off as well. */
    config = base_config;
    config.trip_enabled = false;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &config, 100U) == FOC_RUNTIME_OK);
    CHECK(!(runtime.i2t.rate_per_us > 0.0f));

    /* τ outside 1..3600 s is refused at init and as a tuning. */
    config = base_config;
    config.profile.thermal_tau_s = 0.5f;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &config, 100U) == FOC_RUNTIME_ERR_CONFIG);
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &base_config, 100U) == FOC_RUNTIME_OK);
    struct foc_runtime_tuning tuning;
    CHECK(foc_runtime_read_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    tuning.command_timeout_us = FOC_RUNTIME_LEASE_MIN_US;
    tuning.profile.thermal_tau_s = 4000.0f;
    CHECK(foc_runtime_publish_tuning(&runtime, &tuning, 1U) == FOC_RUNTIME_ERR_CONFIG);
    tuning.profile.thermal_tau_s = 2.0f;
    CHECK(foc_runtime_apply_tuning(&runtime, &tuning) == FOC_RUNTIME_OK);
    CHECK(fabsf(runtime.i2t.rate_per_us - 0.5e-6f) < 1e-12f);
    return 0;
}

static int test_board_inputs(void)
{
    struct foc_runtime runtime;
    struct foc_runtime_config config = base_config;
    struct board board = {0.5f, 0U, true};
    float theta;
    float limit;
    uint32_t t = 100U;
    config.profile.thermal_tau_s = 0.0f;
    CHECK(foc_runtime_init(&runtime, &pipeline_config, &config, 100U) == FOC_RUNTIME_OK);
    CHECK(start(&runtime, &board, &t) == 0);
    thermal(&runtime, &theta, &limit);
    CHECK(limit == 2.5f);
    board.derate = NAN;
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    thermal(&runtime, &theta, &limit);
    CHECK(limit == 0.0f && runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    board.derate = 0.0f;
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    thermal(&runtime, &theta, &limit);
    CHECK(limit == 5.0f);

    /* A board temperature trip latches once and blocks FAULT_CLEAR while it holds. */
    board.faults = FOC_SAFE_FAULT_OVER_TEMPERATURE_FET;
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_OVER_TEMPERATURE_FET);
    board.command = false;
    foc_runtime_revoke_command(&runtime);
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    CHECK(foc_runtime_request_timeout_clear(&runtime, t - 1U) == FOC_RUNTIME_OK);
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    board.faults = 0U;
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    CHECK(foc_runtime_request_timeout_clear(&runtime, t - 1U) == FOC_RUNTIME_OK);
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    t += 100U;
    (void)tick(&runtime, &board, t, FOC_RUNTIME_ACTION_NONE);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    if (test_overload() != 0 || test_configuration() != 0 || test_board_inputs() != 0) {
        return 1;
    }
    puts("test-foc-runtime-thermal: PASS");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
