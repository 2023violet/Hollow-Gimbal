/**
 * @file    test_foc_system.c
 * @brief   Deterministic full-chain Runtime stability and fault-injection test.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Test role
 * ---------
 * Exercise the enabled Host composition of trajectory planning, velocity PI,
 * electrical-speed feedforward, current PI, SVPWM, Device, Safety, and Mock.
 * The test keeps the output adapter at the Host Mock boundary and verifies
 * that rejected updates cannot commit staged control state.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include <math.h>
#include <stdint.h>
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

struct system_result {
    uint32_t hash;
    uint32_t updates;
    uint32_t h0_due;
    uint32_t h1_due;
    uint32_t h0_missed;
    uint32_t h0_late;
    uint32_t h0_budget;
    uint32_t h1_late;
    uint32_t h1_budget;
    uint32_t accepted_outer_seq;
    uint32_t motion_session;
    enum foc_safe_state final_state;
    uint32_t final_faults;
    struct foc_vector_dq current_integral;
    float outer_integral;
    float planned_velocity;
};

/* Private Constants ------------------------------------------------------------ */

static const struct foc_current_feedforward_model system_motor = {
    .ld_h = 0.001f,
    .lq_h = 0.0015f,
    .psi_f_wb = 0.05f,
};

static const struct foc_pipeline_config system_pipeline = {
    .current =
        {
            .pwm = {1.0f, 12.0f, 0.0f, 1.0f, 0.001f},
            .kp_v_per_a = {1.08f, 1.35f},
            .ki_v_per_a_s = {540.0f, 540.0f},
            .current_limit_a = 2.5f,
        },
    .adc = {0.01f, 0.001f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {2500U, 800U, 700U, 700U, 3U},
    .feedforward = {.enabled = true, .model = system_motor, .max_age_us = 700U},
};

static const struct foc_runtime_config system_runtime = {
    .h0 = {.period_us = 200U, .max_delay_us = 150U, .budget_us = 100U},
    .h1 = {.period_us = 500U, .max_delay_us = 150U, .budget_us = 100U},
    .supervision = {.period_us = 200U, .max_delay_us = 150U, .budget_us = 100U},
    .telemetry = {.period_us = 10000U, .max_delay_us = 500U, .budget_us = 100U},
    .foreground_budget_us = 150U,
    .angle_timeout_us = 650U,
    .motion = {
        .enabled = true,
        .rotor = {.pole_pairs = 7U, .electrical_offset_rad = 0.25f},
        .prediction =
            {
                .max_prediction_us = 650U,
                .max_accel_rad_s2 = 2000.0f,
                .max_error_rad = 0.002f,
            },
        .velocity = {.kp_a_per_rad_s = 0.18f, .ki_a_per_rad = 1.4f, .max_dt_s = 0.002f},
        .position = {.kp_per_s = 4.0f, .velocity_limit_rad_s = 12.0f},
        .iq_limit_a = 2.5f,
        .trajectory = {
            .enabled = true,
            .max_velocity_rad_s = 12.0f,
            .max_accel_rad_s2 = 20.0f,
            .max_dt_s = 0.001f,
            .position_tolerance_rad = 0.002f,
        },
    },
};

/* Private Helpers -------------------------------------------------------------- */

static uint32_t fold(uint32_t hash, uint32_t value)
{
    hash ^= value;
    return hash * UINT32_C(16777619);
}

static struct foc_runtime_sensor system_sensor(uint32_t captured_us, uint32_t seq, float speed)
{
    const double mechanical = (double)captured_us * 1.0e-5;
    const double tau = 6.283185307179586476925286766559005768;
    const float phase = (float)fmod(
        fmod(mechanical, tau) * system_runtime.motion.rotor.pole_pairs +
            system_runtime.motion.rotor.electrical_offset_rad,
        tau
    );
    return (struct foc_runtime_sensor){
        .theta_elec_rad = phase < 6.28318530717958647692f ? phase : 0.0f,
        .captured_us = captured_us,
        .seq = seq,
        .valid = true,
        .mechanical_rad = (float)mechanical,
        .mechanical_rad_s = speed,
        .electrical_phase_valid = true,
    };
}

static struct foc_runtime_fast_input system_input(uint32_t tick, uint32_t seq)
{
    return (struct foc_runtime_fast_input){
        .adc =
            {
                .vbus_raw = 2400U,
                .rank2_raw = 2048U,
                .rank3_raw = 2048U,
                .rank4_raw = 2048U,
                .valid_mask = FOC_DEVICE_SAMPLE_ALL_VALID,
                .sample_seq = seq,
                .sample_tick = tick,
            },
        .now_us = tick + 1U,
    };
}

static int foreground(struct foc_runtime *runtime, uint32_t now_us)
{
    struct foc_runtime_foreground_result result;
    const int status = foc_runtime_foreground_poll(runtime, now_us + 2U, 0U, 0U, &result);
    return status == FOC_RUNTIME_OK || status == FOC_RUNTIME_IDLE ? FOC_RUNTIME_OK : status;
}

static int publish_h1(struct foc_runtime *runtime, uint32_t source_us, uint32_t seq)
{
    const struct foc_runtime_sensor sensor = system_sensor(source_us, seq, 5.0f);
    return foc_runtime_h1_step(runtime, &sensor, source_us, 0U);
}

static int
service_h0(struct foc_runtime *runtime, uint32_t tick, uint32_t seq, enum foc_runtime_action action)
{
    struct foc_runtime_fast_input input = system_input(tick, seq);
    if (action == FOC_RUNTIME_ACTION_SET_MODE) {
        input.requested_mode = FOC_RUNTIME_MODE_VELOCITY;
    }
    input.action = action;
    return foc_runtime_h0_step(runtime, &input);
}

static int start_system(struct foc_runtime *runtime)
{
    const struct foc_runtime_command command = {
        .mode = FOC_RUNTIME_MODE_VELOCITY,
        .seq = 0U,
        .produced_us = 0U,
        .target.motion_ref = 8.0f,
    };
    CHECK(foc_runtime_init(runtime, &system_pipeline, &system_runtime, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_runtime_publish_motion_command(runtime, &command, 0U) == FOC_RUNTIME_OK);
    CHECK(publish_h1(runtime, 0U, 0U) == FOC_RUNTIME_OK);
    CHECK(service_h0(runtime, 0U, 0U, FOC_RUNTIME_ACTION_SET_MODE) == FOC_RUNTIME_IDLE);
    CHECK(foreground(runtime, 0U) == FOC_RUNTIME_OK);
    CHECK(service_h0(runtime, 200U, 1U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_IDLE);
    CHECK(foreground(runtime, 200U) == FOC_RUNTIME_OK);
    CHECK(service_h0(runtime, 400U, 2U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_IDLE);
    CHECK(foreground(runtime, 400U) == FOC_RUNTIME_OK);
    const struct foc_runtime_command fresh_command = {
        .mode = FOC_RUNTIME_MODE_VELOCITY,
        .seq = 1U,
        .produced_us = 500U,
        .target.motion_ref = 8.0f,
    };
    CHECK(foc_runtime_publish_motion_command(runtime, &fresh_command, 500U) == FOC_RUNTIME_OK);
    CHECK(publish_h1(runtime, 500U, 1U) == FOC_RUNTIME_OK);
    CHECK(service_h0(runtime, 600U, 3U, FOC_RUNTIME_ACTION_ARM) == FOC_RUNTIME_IDLE);
    CHECK(foreground(runtime, 600U) == FOC_RUNTIME_OK);
    CHECK(service_h0(runtime, 800U, 4U, FOC_RUNTIME_ACTION_START) == FOC_RUNTIME_IDLE);
    CHECK(foreground(runtime, 800U) == FOC_RUNTIME_OK);
    CHECK(runtime->mode == FOC_RUNTIME_MODE_VELOCITY);
    CHECK(runtime->pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    return 0;
}

static int run_sequence(struct system_result *result)
{
    struct foc_runtime runtime = {0};
    CHECK(start_system(&runtime) == 0);

    uint32_t h1_seq = 2U;
    uint32_t command_seq = 2U;
    for (uint32_t tick = 1000U; tick <= 20000U; tick += 100U) {
        if (tick % 500U == 0U) {
            if (tick == 1500U || (tick >= 3000U && tick % 1000U == 0U)) {
                const struct foc_runtime_command refresh = {
                    .mode = FOC_RUNTIME_MODE_VELOCITY,
                    .seq = command_seq++,
                    .produced_us = tick,
                    .target.motion_ref = tick >= 3000U ? -8.0f : 8.0f,
                };
                CHECK(
                    foc_runtime_publish_motion_command(&runtime, &refresh, tick) == FOC_RUNTIME_OK
                );
            }
            CHECK(publish_h1(&runtime, tick, h1_seq++) == FOC_RUNTIME_OK);
        }
        const int status = service_h0(&runtime, tick, tick / 200U + 4U, FOC_RUNTIME_ACTION_NONE);
        CHECK(status == FOC_RUNTIME_OK || status == FOC_RUNTIME_IDLE);
        CHECK(foreground(&runtime, tick) == FOC_RUNTIME_OK);
        struct foc_runtime_snapshot snapshot;
        CHECK(foc_runtime_read_snapshot(&runtime, &snapshot) == FOC_RUNTIME_OK);
        result->hash = fold(result->hash, snapshot.updates);
        result->hash = fold(result->hash, snapshot.compare[0]);
        result->hash = fold(result->hash, snapshot.compare[1]);
        result->hash = fold(result->hash, snapshot.compare[2]);
        result->hash = fold(result->hash, snapshot.accepted_outer_seq);
        CHECK(
            snapshot.action == FOC_DEVICE_OUTPUT_UPDATE ||
            snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT
        );
    }

    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_RUNNING);
    CHECK(runtime.pipeline.mock.updates > 10U);
    CHECK(runtime.accepted_outer_seen);
    CHECK(runtime.h0.missed == 0U && runtime.h0.late_count == 0U && runtime.h0.budget_count == 0U);
    CHECK(runtime.h1.missed == 0U && runtime.h1.late_count == 0U && runtime.h1.budget_count == 0U);
    CHECK(runtime.supervision.missed == 0U && runtime.supervision.late_count == 0U);
    CHECK(isfinite(runtime.outer_velocity.integral_a));
    CHECK(isfinite(runtime.trajectory.planned_velocity_rad_s));

    const struct foc_vector_dq current_before = runtime.pipeline.current.integral_v;
    const float outer_before = runtime.outer_velocity.integral_a;
    const struct foc_trajectory_context trajectory_before = runtime.trajectory;
    runtime.pipeline.mock.reject_update = true;
    const int rejected = service_h0(&runtime, 20200U, 105U, FOC_RUNTIME_ACTION_NONE);
    CHECK(rejected == FOC_RUNTIME_ERR_CONTROL);
    CHECK(runtime.pipeline.last_reason == FOC_PIPELINE_ERR_UPDATE);
    CHECK(runtime.pipeline.current.integral_v.d == current_before.d);
    CHECK(runtime.pipeline.current.integral_v.q == current_before.q);
    CHECK(runtime.outer_velocity.integral_a == outer_before);
    CHECK(memcmp(&runtime.trajectory, &trajectory_before, sizeof trajectory_before) == 0);
    runtime.pipeline.mock.reject_update = false;

    foc_runtime_request_inhibit(&runtime, FOC_SAFE_FAULT_WDT);
    CHECK(service_h0(&runtime, 20400U, 106U, FOC_RUNTIME_ACTION_NONE) == FOC_RUNTIME_ERR_STOP);
    CHECK(runtime.pipeline.safety.state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(runtime.pipeline.safety.latched_faults == FOC_SAFE_FAULT_WDT);
    CHECK(runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
    result->updates = runtime.pipeline.mock.updates;
    result->h0_due = runtime.h0.due_count;
    result->h1_due = runtime.h1.due_count;
    result->h0_missed = runtime.h0.missed;
    result->h0_late = runtime.h0.late_count;
    result->h0_budget = runtime.h0.budget_count;
    result->h1_late = runtime.h1.late_count;
    result->h1_budget = runtime.h1.budget_count;
    result->accepted_outer_seq = runtime.accepted_outer_seq;
    result->motion_session = runtime.motion_session;
    result->final_state = runtime.pipeline.safety.state;
    result->final_faults = runtime.pipeline.safety.latched_faults;
    result->current_integral = runtime.pipeline.current.integral_v;
    result->outer_integral = runtime.outer_velocity.integral_a;
    result->planned_velocity = runtime.trajectory.planned_velocity_rad_s;
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    struct system_result first = {0};
    struct system_result second = {0};
    CHECK(run_sequence(&first) == 0);
    CHECK(run_sequence(&second) == 0);
    CHECK(memcmp(&first, &second, sizeof first) == 0);
    CHECK(first.final_state == FOC_SAFE_STATE_FAULT_LATCHED);
    CHECK(first.final_faults == FOC_SAFE_FAULT_WDT);
    printf(
        "system replay hash=%08x updates=%u h0_due=%u h1_due=%u outer_seq=%u session=%u\n",
        first.hash,
        first.updates,
        first.h0_due,
        first.h1_due,
        first.accepted_outer_seq,
        first.motion_session
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
