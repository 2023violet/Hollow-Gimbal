/**
 * @file    test_foc_control01b.c
 * @brief   Verify H1 electrical-speed publication through H0 and the Pipeline.
 * @version v0.1.0
 * @date    2026.11.13
 *
 * Test role
 * ---------
 * Exercise the real H1 slot, H0 alignment, Pipeline feedforward gate, and
 * Core entry together. The tests deliberately keep Board1 power output
 * closed and use only the Host Mock submission journal.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_runtime.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>

/* Private Macros --------------------------------------------------------------- */

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Types ---------------------------------------------------------------- */

struct run_observation {
    struct foc_device_pwm_request request;
    struct foc_device_pwm_request last_request;
    struct foc_vector_dq integral_v;
    struct foc_runtime_sensor observed_sensor;
    int status;
};

struct interleave_plan {
    bool armed;
    uint32_t source_us;
    uint32_t now_us;
    uint32_t seq;
    float speed_rad_s;
    int h0_status;
};

/* Private Constants ------------------------------------------------------------ */

static const struct foc_current_feedforward_model test_motor = {
    .ld_h = 0.001f,
    .lq_h = 0.0015f,
    .psi_f_wb = 0.05f,
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_pipeline_config pipeline_config(bool enabled, uint32_t max_age_us)
{
    return (struct foc_pipeline_config){
        .current =
            {
                .pwm =
                    {.vbus_min_v = 6.0f,
                     .voltage_limit_v = 12.0f,
                     .duty_min = 0.05f,
                     .duty_max = 0.95f,
                     .max_dt_s = 0.001f},
                .kp_v_per_a = {1.0f, 1.5f},
                .ki_v_per_a_s = {500.0f, 500.0f},
                .current_limit_a = 5.0f,
            },
        .adc =
            {.vbus_v_per_count = 0.01f,
             .current_a_per_count = 0.001f,
             .vbus_min_v = 1.0f,
             .phase_a_offset_count = 2048.0f,
             .phase_b_offset_count = 2048.0f,
             .phase_c_offset_count = 2048.0f,
             .phase_order = FOC_DEVICE_PHASE_ORDER_ABC},
        .pwm = {.period_counts = 6000U, .phase_order = FOC_DEVICE_PHASE_ORDER_ABC},
        .safety =
            {.command_timeout_us = 1500U,
             .sample_timeout_us = 800U,
             .fast_heartbeat_timeout_us = 350U,
             .foreground_heartbeat_timeout_us = 350U,
             .invalid_sample_latch_after = 3U},
        .feedforward = {.enabled = enabled, .model = test_motor, .max_age_us = max_age_us},
    };
}

static struct foc_runtime_config runtime_config(void)
{
    return (struct foc_runtime_config){
        .h0 = {.period_us = 100U, .max_delay_us = 20U, .budget_us = 10U},
        .h1 = {.period_us = 100U, .max_delay_us = 20U, .budget_us = 10U},
        .supervision = {.period_us = 100U, .max_delay_us = 20U, .budget_us = 10U},
        .telemetry = {.period_us = 10000U, .max_delay_us = 20U, .budget_us = 10U},
        .foreground_budget_us = 15U,
        .angle_timeout_us = 500U,
        .motion = {
            .enabled = true,
            .rotor = {.pole_pairs = 4U, .electrical_offset_rad = 0.0f},
            .prediction =
                {
                    .max_prediction_us = 400U,
                    .max_accel_rad_s2 = 2000.0f,
                    .max_error_rad = 0.0005f,
                },
            .velocity = {.kp_a_per_rad_s = 0.18f, .ki_a_per_rad = 1.4f, .max_dt_s = 0.002f},
            .position = {.kp_per_s = 4.0f, .velocity_limit_rad_s = 12.0f},
            .iq_limit_a = 2.5f,
        },
    };
}

static struct foc_runtime_sensor
sensor_at(uint32_t source_us, uint32_t seq, float speed_rad_s, bool valid)
{
    return (struct foc_runtime_sensor){
        .theta_elec_rad = 0.0f,
        .captured_us = source_us,
        .seq = seq,
        .valid = valid,
        .mechanical_rad = 0.0f,
        .mechanical_rad_s = speed_rad_s,
        /* Analytic fixed zero position; this fixture's rotor offset is zero. */
        .electrical_phase_valid = valid,
    };
}

static struct foc_runtime_fast_input
h0_input(uint32_t sample_tick, uint32_t seq, enum foc_runtime_action action)
{
    return (struct foc_runtime_fast_input){
        .adc =
            {.vbus_raw = 2400U,
             .rank2_raw = 2048U,
             .rank3_raw = 2048U,
             .rank4_raw = 2048U,
             .valid_mask = FOC_DEVICE_SAMPLE_ALL_VALID,
             .sample_seq = seq,
             .sample_tick = sample_tick},
        .now_us = sample_tick + 1U,
        .action = action,
        .requested_mode = FOC_RUNTIME_MODE_CURRENT,
    };
}

static int publish_sensor(
    struct foc_runtime *runtime,
    uint32_t source_us,
    uint32_t now_us,
    uint32_t seq,
    float speed_rad_s,
    bool valid
)
{
    const struct foc_runtime_sensor sensor = sensor_at(source_us, seq, speed_rad_s, valid);
    return foc_runtime_h1_step(runtime, &sensor, now_us, 0U);
}

static int poll_foreground(struct foc_runtime *runtime, uint32_t now_us)
{
    struct foc_runtime_foreground_result result;
    return foc_runtime_foreground_poll(runtime, now_us, 0U, 0U, &result);
}

static int service(
    struct foc_runtime *runtime,
    uint32_t at,
    uint32_t seq,
    enum foc_runtime_action action,
    float speed_rad_s,
    uint32_t source_us,
    bool valid
)
{
    TEST_ASSERT(publish_sensor(runtime, source_us, at, seq, speed_rad_s, valid) == FOC_RUNTIME_OK);
    const struct foc_runtime_fast_input input = h0_input(at, seq, action);
    return foc_runtime_h0_step(runtime, &input);
}

static int start_runtime(
    struct foc_runtime *runtime,
    bool enabled,
    uint32_t max_age_us,
    uint32_t base,
    float speed_rad_s
)
{
    const struct foc_pipeline_config pipeline = pipeline_config(enabled, max_age_us);
    const struct foc_runtime_config policy = runtime_config();
    const struct foc_pipeline_command command = {{0.1f, 1.5f}, 0U, base};

    TEST_ASSERT(foc_runtime_init(runtime, &pipeline, &policy, base) == FOC_RUNTIME_OK);
    TEST_ASSERT(foc_runtime_publish_command(runtime, &command, base) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        service(runtime, base, 0U, FOC_RUNTIME_ACTION_NONE, speed_rad_s, base, true) ==
        FOC_RUNTIME_IDLE
    );
    TEST_ASSERT(poll_foreground(runtime, base + 2U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        service(runtime, base + 100U, 1U, FOC_RUNTIME_ACTION_ARM, speed_rad_s, base + 100U, true) ==
        FOC_RUNTIME_IDLE
    );
    TEST_ASSERT(poll_foreground(runtime, base + 102U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        service(
            runtime,
            base + 200U,
            2U,
            FOC_RUNTIME_ACTION_START,
            speed_rad_s,
            base + 200U,
            true
        ) == FOC_RUNTIME_IDLE
    );
    TEST_ASSERT(poll_foreground(runtime, base + 202U) == FOC_RUNTIME_OK);
    return 0;
}

static int run_once(
    bool enabled,
    float speed_rad_s,
    uint32_t base,
    uint32_t final_age_us,
    struct run_observation *observation
)
{
    struct foc_runtime runtime = {0};
    const uint32_t final_tick = base + 300U;
    const uint32_t final_source = final_tick - final_age_us;

    TEST_ASSERT(start_runtime(&runtime, enabled, 50U, base, speed_rad_s) == 0);
    TEST_ASSERT(
        service(
            &runtime,
            final_tick,
            3U,
            FOC_RUNTIME_ACTION_NONE,
            speed_rad_s,
            final_source,
            true
        ) == (final_age_us > 50U ? FOC_RUNTIME_ERR_CONTROL : FOC_RUNTIME_OK)
    );
    TEST_ASSERT(poll_foreground(&runtime, final_tick + 2U) == FOC_RUNTIME_OK);
    *observation = (struct run_observation){
        .request = runtime.pipeline.mock.last_update,
        .last_request = runtime.pipeline.mock.last_request,
        .integral_v = runtime.pipeline.current.integral_v,
        .observed_sensor = runtime.observed_sensor,
        .status = runtime.pipeline_status,
    };
    return 0;
}

static void
interleave_hook(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *argument)
{
    struct interleave_plan *plan = argument;
    if (!plan->armed || point != FOC_RUNTIME_HOOK_SENSOR_WRITING) {
        return;
    }
    plan->armed = false;
    const struct foc_runtime_fast_input input =
        h0_input(plan->source_us, plan->seq, FOC_RUNTIME_ACTION_NONE);
    plan->h0_status = foc_runtime_h0_step(runtime, &input);
}

/* Tests ----------------------------------------------------------------------- */

static int test_enabled_and_direction(void)
{
    struct run_observation off;
    struct run_observation zero_off;
    struct run_observation zero_on;
    struct run_observation forward;
    struct run_observation reverse;

    TEST_ASSERT(run_once(false, 25.0f, 0U, 0U, &off) == 0);
    TEST_ASSERT(run_once(false, 0.0f, 30000U, 0U, &zero_off) == 0);
    TEST_ASSERT(run_once(true, 0.0f, 40000U, 0U, &zero_on) == 0);
    TEST_ASSERT(run_once(true, 25.0f, 10000U, 0U, &forward) == 0);
    TEST_ASSERT(run_once(true, -25.0f, 20000U, 0U, &reverse) == 0);
    TEST_ASSERT(off.request.action == FOC_DEVICE_OUTPUT_UPDATE);
    TEST_ASSERT(forward.request.action == FOC_DEVICE_OUTPUT_UPDATE);
    TEST_ASSERT(reverse.request.action == FOC_DEVICE_OUTPUT_UPDATE);
    TEST_ASSERT(
        zero_on.request.duty_a == zero_off.request.duty_a &&
        zero_on.request.duty_b == zero_off.request.duty_b &&
        zero_on.request.duty_c == zero_off.request.duty_c
    );
    TEST_ASSERT(
        forward.request.duty_a != off.request.duty_a ||
        forward.request.duty_b != off.request.duty_b || forward.request.duty_c != off.request.duty_c
    );
    TEST_ASSERT(
        reverse.request.duty_a != forward.request.duty_a ||
        reverse.request.duty_b != forward.request.duty_b ||
        reverse.request.duty_c != forward.request.duty_c
    );
    TEST_ASSERT(forward.observed_sensor.captured_us == 10300U);
    TEST_ASSERT(forward.observed_sensor.mechanical_rad_s == 25.0f);
    return 0;
}

static int test_feedforward_configuration_bounds(void)
{
    struct foc_pipeline pipeline = {0};
    struct foc_pipeline_config invalid = pipeline_config(true, 0U);
    TEST_ASSERT(foc_pipeline_init(&pipeline, &invalid) == FOC_PIPELINE_ERR_CONFIG);
    invalid.feedforward.max_age_us = UINT32_C(0x80000000);
    TEST_ASSERT(foc_pipeline_init(&pipeline, &invalid) == FOC_PIPELINE_ERR_CONFIG);
    return 0;
}

static int test_legal_delay_and_wrap(void)
{
    struct run_observation delayed;
    struct run_observation wrapped;

    TEST_ASSERT(run_once(true, 25.0f, 30000U, 20U, &delayed) == 0);
    TEST_ASSERT(delayed.request.action == FOC_DEVICE_OUTPUT_UPDATE);
    TEST_ASSERT(run_once(true, 25.0f, UINT32_MAX - 150U, 20U, &wrapped) == 0);
    TEST_ASSERT(wrapped.request.action == FOC_DEVICE_OUTPUT_UPDATE);
    return 0;
}

static int test_stale_and_invalid_feedback(void)
{
    struct run_observation stale;
    TEST_ASSERT(run_once(true, 25.0f, 40000U, 80U, &stale) == 0);
    TEST_ASSERT(stale.status == FOC_PIPELINE_ERR_FEEDBACK);
    TEST_ASSERT(stale.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
    TEST_ASSERT(stale.integral_v.d == 0.0f && stale.integral_v.q == 0.0f);

    struct foc_runtime runtime = {0};
    const uint32_t base = 50000U;
    TEST_ASSERT(start_runtime(&runtime, true, 50U, base, 25.0f) == 0);
    const struct foc_runtime_sensor invalid = sensor_at(base + 300U, 3U, NAN, false);
    TEST_ASSERT(foc_runtime_h1_step(&runtime, &invalid, base + 300U, 0U) == FOC_RUNTIME_ERR_INPUT);
    const struct foc_runtime_fast_input invalid_input =
        h0_input(base + 300U, 3U, FOC_RUNTIME_ACTION_NONE);
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &invalid_input) == FOC_RUNTIME_ERR_ALIGNMENT);
    TEST_ASSERT(runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
    TEST_ASSERT(
        runtime.pipeline.current.integral_v.d == 0.0f &&
        runtime.pipeline.current.integral_v.q == 0.0f
    );
    return 0;
}

static int test_future_source_rejected(void)
{
    struct foc_runtime runtime = {0};
    const uint32_t base = 60000U;
    TEST_ASSERT(start_runtime(&runtime, true, 50U, base, 25.0f) == 0);
    const struct foc_runtime_sensor future = sensor_at(base + 320U, 3U, 25.0f, true);
    TEST_ASSERT(foc_runtime_h1_step(&runtime, &future, base + 320U, 0U) == FOC_RUNTIME_OK);
    const struct foc_runtime_fast_input input = h0_input(base + 300U, 3U, FOC_RUNTIME_ACTION_NONE);
    TEST_ASSERT(foc_runtime_h0_step(&runtime, &input) == FOC_RUNTIME_ERR_ALIGNMENT);
    TEST_ASSERT(runtime.pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT);
    TEST_ASSERT(
        runtime.pipeline.current.integral_v.d == 0.0f &&
        runtime.pipeline.current.integral_v.q == 0.0f
    );
    return 0;
}

static int test_h1_h0_slot_atomicity(void)
{
    struct foc_runtime runtime = {0};
    struct interleave_plan plan = {
        .armed = false,
        .source_us = 70300U,
        .now_us = 70301U,
        .seq = 3U,
        .speed_rad_s = -25.0f,
        .h0_status = FOC_RUNTIME_ERR_INPUT,
    };
    struct foc_runtime_config policy = runtime_config();
    const struct foc_pipeline_config pipeline = pipeline_config(true, 200U);
    const struct foc_pipeline_command command = {{0.1f, 1.5f}, 0U, 70000U};

    policy.hook = interleave_hook;
    policy.hook_arg = &plan;
    TEST_ASSERT(foc_runtime_init(&runtime, &pipeline, &policy, 70000U) == FOC_RUNTIME_OK);
    TEST_ASSERT(foc_runtime_publish_command(&runtime, &command, 70000U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        service(&runtime, 70000U, 0U, FOC_RUNTIME_ACTION_NONE, 25.0f, 70000U, true) ==
        FOC_RUNTIME_IDLE
    );
    TEST_ASSERT(poll_foreground(&runtime, 70002U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        service(&runtime, 70100U, 1U, FOC_RUNTIME_ACTION_ARM, 25.0f, 70100U, true) ==
        FOC_RUNTIME_IDLE
    );
    TEST_ASSERT(poll_foreground(&runtime, 70102U) == FOC_RUNTIME_OK);
    TEST_ASSERT(
        service(&runtime, 70200U, 2U, FOC_RUNTIME_ACTION_START, 25.0f, 70200U, true) ==
        FOC_RUNTIME_IDLE
    );
    TEST_ASSERT(poll_foreground(&runtime, 70202U) == FOC_RUNTIME_OK);

    plan.armed = true;
    const struct foc_runtime_sensor replacement = sensor_at(70300U, 3U, -25.0f, true);
    TEST_ASSERT(foc_runtime_h1_step(&runtime, &replacement, 70300U, 0U) == FOC_RUNTIME_OK);
    TEST_ASSERT(plan.h0_status == FOC_RUNTIME_OK);
    TEST_ASSERT(runtime.observed_sensor.captured_us == 70200U);
    TEST_ASSERT(runtime.observed_sensor.mechanical_rad_s == 25.0f);
    TEST_ASSERT(runtime.pipeline.last_reason == FOC_PIPELINE_OK);
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

int main(void)
{
    return test_feedforward_configuration_bounds() || test_enabled_and_direction() ||
        test_legal_delay_and_wrap() || test_stale_and_invalid_feedback() ||
        test_future_source_rejected() || test_h1_h0_slot_atomicity();
}

/* ---------------------------------------- end of file ----------------------------------------- */
