/**
 * @file    test_foc_pipeline.c
 * @brief   End-to-end current pipeline acceptance tests.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Test role
 * ---------
 * Exercise actual Device, Current Core, Safety, and closed-gate Mock behavior.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_pipeline.h"
#include "foc_test_motor.h"

#include <math.h>
#include <stdio.h>

#define TEST_ASSERT(condition)                                                                  \
    do {                                                                                        \
        if (!(condition)) {                                                                     \
            fprintf(stderr, "assertion failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                           \
        }                                                                                       \
    } while (0)

/* Private Helpers -------------------------------------------------------------- */

static const struct foc_pipeline_config test_config = {
    .current =
        {
            .pwm = {1.0f, 2.0f, 0.0f, 1.0f, 0.001f},
            .kp_v_per_a = {1.0f, 1.5f},
            .ki_v_per_a_s = {500.0f, 500.0f},
            .current_limit_a = 5.0f,
        },
    .adc = {0.01f, 0.001f, 1.0f, 2048.0f, 2048.0f, 2048.0f, FOC_DEVICE_PHASE_ORDER_ABC},
    .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
    .safety = {1000U, 500U, 1000U, 1000U, 3U},
};

static struct foc_pipeline_input sample(uint32_t now_us, uint32_t seq)
{
    return (struct foc_pipeline_input){
        .adc = {1200U, 2048U, 2048U, 2048U, FOC_DEVICE_SAMPLE_ALL_VALID, seq, now_us},
        .angle_sample_us = now_us,
        .theta_elec_rad = 0.37f,
        .now_us = now_us,
        .dt_s = 0.0001f,
    };
}

static int publish_command(struct foc_pipeline *pipeline, uint32_t now_us, uint32_t seq)
{
    const struct foc_pipeline_command command = {{0.0f, 1.0f}, seq, now_us};
    return foc_pipeline_set_command(pipeline, &command, now_us);
}

static int publish_heartbeats(struct foc_pipeline *pipeline, uint32_t now_us)
{
    TEST_ASSERT(foc_safe_heartbeat_fast(&pipeline->safety, now_us) == FOC_SAFE_STATUS_OK);
    TEST_ASSERT(foc_safe_heartbeat_foreground(&pipeline->safety, now_us) == FOC_SAFE_STATUS_OK);
    return 0;
}

static int inhibited(const struct foc_pipeline *pipeline)
{
    return pipeline->mock.output_confirmed && pipeline->mock.last_accepted &&
        pipeline->mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        pipeline->mock.registers.bridge_gate_closed == 1U &&
        pipeline->mock.registers.bridge_enabled == 0U &&
        pipeline->mock.registers.channel1_compare == 0U &&
        pipeline->mock.registers.channel2_compare == 0U &&
        pipeline->mock.registers.channel3_compare == 0U;
}

static int begin_run(
    struct foc_pipeline *pipeline,
    const struct foc_pipeline_config *config,
    const struct foc_pipeline_input *input,
    struct foc_vector_dq target
)
{
    const struct foc_pipeline_command command = {target, input->adc.sample_seq, input->now_us};

    TEST_ASSERT(foc_pipeline_init(pipeline, config) == FOC_PIPELINE_OK);
    TEST_ASSERT(publish_heartbeats(pipeline, input->now_us) == 0);
    TEST_ASSERT(foc_pipeline_set_command(pipeline, &command, input->now_us) == FOC_PIPELINE_OK);
    TEST_ASSERT(foc_pipeline_step(pipeline, input) == FOC_PIPELINE_ERR_STATE);
    TEST_ASSERT(foc_pipeline_arm(pipeline, input->now_us) == FOC_PIPELINE_OK);
    TEST_ASSERT(foc_pipeline_start(pipeline, input->now_us) == FOC_PIPELINE_OK);
    return 0;
}

static int test_startup_and_recovery(void)
{
    struct foc_pipeline pipeline = {0};
    struct foc_pipeline_input input = sample(0U, 0U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_STATE);
    TEST_ASSERT(foc_pipeline_init(&pipeline, &test_config) == FOC_PIPELINE_OK);
    TEST_ASSERT(inhibited(&pipeline));
    TEST_ASSERT(foc_pipeline_arm(&pipeline, 0U) == FOC_PIPELINE_ERR_STATE);
    TEST_ASSERT(publish_heartbeats(&pipeline, 0U) == 0);
    TEST_ASSERT(publish_command(&pipeline, 0U, 0U) == FOC_PIPELINE_OK);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_STATE);
    TEST_ASSERT(inhibited(&pipeline));
    TEST_ASSERT(foc_pipeline_arm(&pipeline, 0U) == FOC_PIPELINE_OK);
    input = sample(100U, 1U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_STATE);
    TEST_ASSERT(inhibited(&pipeline));
    TEST_ASSERT(foc_pipeline_start(&pipeline, 100U) == FOC_PIPELINE_OK);
    input = sample(200U, 2U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_OK);
    TEST_ASSERT(pipeline.current.integral_v.q > 0.0f && pipeline.mock.updates == 1U);
    const float integral = pipeline.current.integral_v.q;

    input = sample(300U, 3U);
    input.adc.valid_mask = 0U;
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_SAMPLE);
    TEST_ASSERT(inhibited(&pipeline) && pipeline.current.integral_v.q == integral);
    TEST_ASSERT(pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    TEST_ASSERT(pipeline.safety.latched_faults == 0U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_SAMPLE_ORDER);
    TEST_ASSERT(pipeline.safety.invalid_sample_streak == 1U);
    input = sample(400U, 4U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_STATE);
    TEST_ASSERT(inhibited(&pipeline));
    TEST_ASSERT(foc_pipeline_start(&pipeline, 400U) == FOC_PIPELINE_ERR_STATE);

    for (uint32_t index = 5U; index <= 7U; ++index) {
        input = sample(index * 100U, index);
        input.adc.valid_mask = 0U;
        TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_SAMPLE);
        TEST_ASSERT(inhibited(&pipeline));
        TEST_ASSERT((pipeline.safety.latched_faults != 0U) == (index == 7U));
    }
    TEST_ASSERT(pipeline.safety.latched_faults == FOC_SAFE_FAULT_SAMPLE_INVALID);
    TEST_ASSERT(publish_command(&pipeline, 700U, 1U) == FOC_PIPELINE_ERR_STATE);
    TEST_ASSERT(foc_pipeline_clear_fault(&pipeline, 700U) == FOC_PIPELINE_OK);
    TEST_ASSERT(pipeline.safety.state == FOC_SAFE_STATE_DISABLED);
    TEST_ASSERT(foc_pipeline_start(&pipeline, 700U) == FOC_PIPELINE_ERR_STATE);
    const struct foc_pipeline_command delayed = {{0.0f, 1.0f}, 1U, 650U};
    TEST_ASSERT(foc_pipeline_set_command(&pipeline, &delayed, 800U) == FOC_PIPELINE_ERR_COMMAND);
    TEST_ASSERT(publish_command(&pipeline, 800U, 0U) == FOC_PIPELINE_ERR_COMMAND);
    input = sample(800U, 8U);
    input.angle_sample_us = input.adc.sample_tick = 700U;
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_SAMPLE_ORDER);
    input = sample(800U, 7U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_SAMPLE_ORDER);
    TEST_ASSERT(publish_command(&pipeline, 800U, 1U) == FOC_PIPELINE_OK);
    TEST_ASSERT(publish_heartbeats(&pipeline, 800U) == 0);
    input = sample(800U, 8U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_STATE);
    TEST_ASSERT(foc_pipeline_arm(&pipeline, 800U) == FOC_PIPELINE_OK);
    TEST_ASSERT(foc_pipeline_start(&pipeline, 800U) == FOC_PIPELINE_OK);
    TEST_ASSERT(pipeline.current.integral_v.q == 0.0f);
    input = sample(900U, 9U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_OK);
    return 0;
}

static int test_final_gate(void)
{
    struct foc_pipeline pipeline = {0};
    struct foc_pipeline_input input = sample(0U, 0U);
    TEST_ASSERT(
        begin_run(&pipeline, &test_config, &input, (struct foc_vector_dq){0.0f, 1.0f}) == 0
    );
    input = sample(100U, 1U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_OK);
    const struct foc_vector_dq committed = pipeline.current.integral_v;

    input = sample(200U, 2U);
    input.before_gate.inhibit = true;
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_STOP);
    TEST_ASSERT(inhibited(&pipeline) && pipeline.mock.updates == 1U);
    TEST_ASSERT(pipeline.current.integral_v.d == committed.d);
    TEST_ASSERT(pipeline.current.integral_v.q == committed.q);
    return 0;
}

static int test_rejections(void)
{
    const struct {
        int expected;
        int reason;
        uint32_t fault;
    } cases[] = {
        {FOC_PIPELINE_ERR_GATE, FOC_PIPELINE_ERR_GATE, FOC_SAFE_FAULT_COMMAND_TIMEOUT},
        {FOC_PIPELINE_ERR_GATE, FOC_PIPELINE_ERR_GATE, FOC_SAFE_FAULT_SAMPLE_TIMEOUT},
        {FOC_PIPELINE_ERR_GATE, FOC_PIPELINE_ERR_GATE, FOC_SAFE_FAULT_WDT},
        {FOC_PIPELINE_ERR_MAP, FOC_PIPELINE_ERR_MAP, 0U},
        {FOC_PIPELINE_ERR_CORE, FOC_PIPELINE_ERR_CORE, 0U},
        {FOC_PIPELINE_ERR_SAMPLE, FOC_PIPELINE_ERR_SAMPLE, 0U},
        {FOC_PIPELINE_ERR_UPDATE, FOC_PIPELINE_ERR_UPDATE, 0U},
        {FOC_PIPELINE_ERR_INHIBIT, FOC_PIPELINE_ERR_UPDATE, 0U},
    };
    for (unsigned int index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        struct foc_pipeline pipeline = {0};
        struct foc_pipeline_input input = sample(0U, 0U);
        TEST_ASSERT(
            begin_run(&pipeline, &test_config, &input, (struct foc_vector_dq){0.1f, 1.0f}) == 0
        );
        input = sample(100U, 1U);
        TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_OK);
        const struct foc_vector_dq committed = pipeline.current.integral_v;
        const struct foc_device_sim_registers old = pipeline.mock.registers;
        const uint32_t previous_attempts = pipeline.mock.attempts;

        input = sample(200U, 2U);
        switch (cases[index].fault) {
        case FOC_SAFE_FAULT_COMMAND_TIMEOUT:
            input = sample(900U, 2U);
            TEST_ASSERT(publish_heartbeats(&pipeline, 900U) == 0);
            input.before_gate.delay_us = 101U;
            break;
        case FOC_SAFE_FAULT_SAMPLE_TIMEOUT:
            input.before_gate.delay_us = 501U;
            break;
        case FOC_SAFE_FAULT_WDT:
            input.before_gate.fault_bits = FOC_SAFE_FAULT_WDT;
            break;
        default:
            break;
        }
        if (cases[index].reason == FOC_PIPELINE_ERR_MAP) {
            /* Corrupt only the test fixture to exercise the real Device mapper rejection. */
            pipeline.pwm_config.period_counts = 0U;
        }
        if (cases[index].reason == FOC_PIPELINE_ERR_CORE) {
            input.dt_s = NAN;
        }
        if (cases[index].reason == FOC_PIPELINE_ERR_SAMPLE) {
            input.angle_sample_us--;
        }
        pipeline.mock.reject_update = cases[index].reason == FOC_PIPELINE_ERR_UPDATE;
        pipeline.mock.reject_inhibit = cases[index].expected == FOC_PIPELINE_ERR_INHIBIT;

        TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == cases[index].expected);
        TEST_ASSERT((int)pipeline.last_reason == cases[index].reason);
        TEST_ASSERT(pipeline.safety.latched_faults == cases[index].fault);
        TEST_ASSERT((pipeline.safety_reasons & cases[index].fault) == cases[index].fault);
        TEST_ASSERT(
            pipeline.mock.updates == 1U &&
            pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT
        );
        TEST_ASSERT(
            pipeline.current.integral_v.d == committed.d &&
            pipeline.current.integral_v.q == committed.q
        );
        TEST_ASSERT(
            pipeline.mock.attempts - previous_attempts == (pipeline.mock.reject_update ? 2U : 1U)
        );
        if (pipeline.mock.reject_update) {
            TEST_ASSERT(pipeline.mock.last_rejected_update.action == FOC_DEVICE_OUTPUT_UPDATE);
        }
        if (pipeline.mock.reject_inhibit) {
            TEST_ASSERT(!pipeline.mock.output_confirmed && !pipeline.mock.last_accepted);
            TEST_ASSERT(pipeline.mock.registers.channel1_compare == old.channel1_compare);
            TEST_ASSERT(pipeline.mock.registers.channel2_compare == old.channel2_compare);
            TEST_ASSERT(pipeline.mock.registers.channel3_compare == old.channel3_compare);
            TEST_ASSERT(foc_pipeline_arm(&pipeline, 200U) == FOC_PIPELINE_ERR_STATE);
            pipeline.mock.reject_inhibit = false;
            TEST_ASSERT(foc_pipeline_request_inhibit(&pipeline, 200U, 0U) == FOC_PIPELINE_OK);
        }
        TEST_ASSERT(inhibited(&pipeline));
    }
    return 0;
}

static int test_time_and_replay(void)
{
    struct foc_pipeline pipeline = {0};
    struct foc_pipeline_input input = sample(UINT32_MAX - 99U, UINT32_MAX - 1U);
    TEST_ASSERT(
        begin_run(&pipeline, &test_config, &input, (struct foc_vector_dq){0.0f, 1.0f}) == 0
    );
    input = sample(UINT32_MAX - 1U, UINT32_MAX);
    TEST_ASSERT(publish_command(&pipeline, input.now_us, UINT32_MAX) == FOC_PIPELINE_OK);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_OK);
    input = sample(0U, 0U);
    TEST_ASSERT(publish_command(&pipeline, 0U, 0U) == FOC_PIPELINE_OK);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_OK);
    TEST_ASSERT(pipeline.safety.sample_seen && pipeline.safety.sample_seq == 0U);
    TEST_ASSERT(pipeline.safety.command_seen && pipeline.safety.command_seq == 0U);
    TEST_ASSERT(publish_command(&pipeline, 999U, 0U) == FOC_PIPELINE_ERR_COMMAND);
    TEST_ASSERT(publish_command(&pipeline, 999U, UINT32_MAX) == FOC_PIPELINE_ERR_COMMAND);
    struct foc_pipeline_command future = {{0.0f, 1.0f}, 1U, 1000U};
    TEST_ASSERT(foc_pipeline_set_command(&pipeline, &future, 999U) == FOC_PIPELINE_ERR_COMMAND);
    future.produced_us = 999U - 0x80000000U;
    TEST_ASSERT(foc_pipeline_set_command(&pipeline, &future, 999U) == FOC_PIPELINE_ERR_COMMAND);
    TEST_ASSERT(pipeline.safety.last_command_us == 0U);
    TEST_ASSERT(publish_heartbeats(&pipeline, 1001U) == 0);
    input = sample(1001U, 1U);
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_GATE);
    TEST_ASSERT(inhibited(&pipeline));
    TEST_ASSERT(pipeline.safety.latched_faults == FOC_SAFE_FAULT_COMMAND_TIMEOUT);

    const struct {
        uint32_t now_us;
        uint32_t sample_us;
        uint32_t seq;
    } invalid_samples[] = {
        {200U, 200U, 1U}, /* Replay. */
        {200U, 200U, 0U}, /* Older sequence. */
        {200U, 201U, 2U}, /* Future capture. */
        {200U, 200U - 0x80000000U, 2U}, /* Ambiguous half range. */
        {200U, 200U, 1U + 0x80000000U}, /* Ambiguous sequence half range. */
        {701U, 200U, 2U}, /* Expired sample. */
    };
    for (unsigned int index = 0U; index < sizeof(invalid_samples) / sizeof(invalid_samples[0]);
         ++index) {
        input = sample(0U, 0U);
        TEST_ASSERT(
            begin_run(&pipeline, &test_config, &input, (struct foc_vector_dq){0.0f, 1.0f}) == 0
        );
        input = sample(100U, 1U);
        TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_OK);
        const float integral = pipeline.current.integral_v.q;
        input = sample(invalid_samples[index].now_us, invalid_samples[index].seq);
        input.adc.sample_tick = input.angle_sample_us = invalid_samples[index].sample_us;
        TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_SAMPLE_ORDER);
        TEST_ASSERT(inhibited(&pipeline) && pipeline.mock.updates == 1U);
        TEST_ASSERT(pipeline.current.integral_v.q == integral);
        TEST_ASSERT(pipeline.safety.invalid_sample_streak == 0U);
    }
    return 0;
}

static int test_instances_and_determinism(void)
{
    struct foc_pipeline first = {0};
    struct foc_pipeline replay = {0};
    struct foc_pipeline second = {0};
    struct foc_pipeline_input input = sample(0U, 0U);
    const struct foc_vector_dq forward = {0.2f, 1.0f};
    const struct foc_vector_dq reverse = {0.0f, -0.5f};

    TEST_ASSERT(begin_run(&first, &test_config, &input, forward) == 0);
    TEST_ASSERT(begin_run(&replay, &test_config, &input, forward) == 0);
    TEST_ASSERT(begin_run(&second, &test_config, &input, reverse) == 0);
    for (uint32_t step = 1U; step <= 4U; ++step) {
        input = sample(100U * step, step);
        if (step == 4U) {
            input.before_gate.fault_bits = FOC_SAFE_FAULT_INVALID_STATE;
        }
        const int result = foc_pipeline_step(&first, &input);
        TEST_ASSERT(foc_pipeline_step(&replay, &input) == result);
        TEST_ASSERT(first.current.integral_v.d == replay.current.integral_v.d);
        TEST_ASSERT(first.current.integral_v.q == replay.current.integral_v.q);
        TEST_ASSERT(first.safety.latched_faults == replay.safety.latched_faults);
        TEST_ASSERT(
            first.safety.state == replay.safety.state && first.last_reason == replay.last_reason
        );
        TEST_ASSERT(first.mock.last_request.action == replay.mock.last_request.action);
        TEST_ASSERT(
            first.mock.attempts == replay.mock.attempts && first.mock.updates == replay.mock.updates
        );
        TEST_ASSERT(
            first.mock.registers.channel1_compare == replay.mock.registers.channel1_compare
        );
        TEST_ASSERT(
            first.mock.registers.channel2_compare == replay.mock.registers.channel2_compare
        );
        TEST_ASSERT(
            first.mock.registers.channel3_compare == replay.mock.registers.channel3_compare
        );
        input.before_gate.fault_bits = 0U;
        TEST_ASSERT(foc_pipeline_step(&second, &input) == FOC_PIPELINE_OK);
    }
    TEST_ASSERT(first.current.integral_v.q > 0.0f && second.current.integral_v.q < 0.0f);
    TEST_ASSERT(first.safety.state == FOC_SAFE_STATE_FAULT_LATCHED && inhibited(&first));
    TEST_ASSERT(second.safety.state == FOC_SAFE_STATE_RUNNING);
    TEST_ASSERT(
        first.mock.last_update.channel1_compare != second.mock.last_update.channel1_compare
    );
    TEST_ASSERT(foc_pipeline_request_inhibit(&second, 400U, 0U) == FOC_PIPELINE_OK);
    TEST_ASSERT(inhibited(&second) && second.mock.last_inhibit_attempt == second.mock.attempts);
    TEST_ASSERT(second.mock.last_update.action == FOC_DEVICE_OUTPUT_UPDATE);
    TEST_ASSERT(second.safety.state == FOC_SAFE_STATE_DISABLED);
    return 0;
}

static int test_closed_loop(void)
{
    const struct foc_vector_dq targets[] = {{0.0f, 1.0f}, {0.0f, -1.0f}, {0.5f, -0.75f}};
    struct foc_pipeline_config config = test_config;
    struct foc_pipeline pipeline = {0};
    struct foc_pipeline_input input = sample(0U, 0U);
    struct foc_test_motor motor = foc_test_motor_init();
    uint32_t tick = 0U;
    uint32_t command_seq = 0U;

    config.adc.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
    config.pwm.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
    TEST_ASSERT(begin_run(&pipeline, &config, &input, targets[0]) == 0);
    for (unsigned int stage = 0U; stage < sizeof(targets) / sizeof(targets[0]); ++stage) {
        double max_error = 0.0;
        for (unsigned int step = 0U; step < 600U; ++step) {
            tick++;
            input = sample(tick * 100U, tick);
            if (step % 5U == 0U) {
                const struct foc_pipeline_command command =
                    {targets[stage], ++command_seq, input.now_us};
                TEST_ASSERT(
                    foc_pipeline_set_command(&pipeline, &command, input.now_us) == FOC_PIPELINE_OK
                );
            }
            if (tick % 4U == 0U) {
                TEST_ASSERT(
                    foc_safe_heartbeat_fast(&pipeline.safety, input.now_us) == FOC_SAFE_STATUS_OK
                );
            }
            if (tick % 7U == 0U) {
                TEST_ASSERT(
                    foc_safe_heartbeat_foreground(&pipeline.safety, input.now_us) ==
                    FOC_SAFE_STATUS_OK
                );
            }
            foc_test_motor_encode(&motor, &input.adc);
            TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_OK);
            TEST_ASSERT(pipeline.mock.output_confirmed && pipeline.mock.last_accepted);
            TEST_ASSERT(
                pipeline.mock.updates == tick &&
                pipeline.mock.last_request.action == FOC_DEVICE_OUTPUT_UPDATE
            );
            TEST_ASSERT(pipeline.mock.registers.bridge_enabled == 0U);
            TEST_ASSERT(fabs(pipeline.measured_dq_a.d - motor.id) <= 0.002);
            TEST_ASSERT(fabs(pipeline.measured_dq_a.q - motor.iq) <= 0.002);
            const struct foc_device_pwm_request *request = &pipeline.mock.last_update;
            const struct foc_device_sim_registers *registers = &pipeline.mock.registers;
            TEST_ASSERT(registers->channel3_compare == (uint32_t)(request->duty_a * 6000.0f));
            TEST_ASSERT(registers->channel1_compare == (uint32_t)(request->duty_b * 6000.0f));
            TEST_ASSERT(registers->channel2_compare == (uint32_t)(request->duty_c * 6000.0f));

            /* The model consumes accepted compares, including Device quantization. */
            const uint32_t compare[] = {
                registers->channel1_compare,
                registers->channel2_compare,
                registers->channel3_compare
            };
            TEST_ASSERT(foc_test_motor_apply(&motor, compare) <= 2.003);
            if (step >= 500U) {
                max_error = fmax(
                    max_error,
                    fmax(fabs(motor.id - targets[stage].d), fabs(motor.iq - targets[stage].q))
                );
            }
        }
        TEST_ASSERT(max_error <= 0.01);
        TEST_ASSERT(fabs(pipeline.current.integral_v.q) >= 0.30);
        printf(
            "stage=%u final=(%.6f, %.6f) A window_error=%.6f A accepted=%u\n",
            stage,
            motor.id,
            motor.iq,
            max_error,
            (unsigned int)pipeline.mock.updates
        );
    }
    const struct foc_vector_dq committed = pipeline.current.integral_v;
    input = sample((tick + 1U) * 100U, tick + 1U);
    input.before_gate.inhibit = true;
    TEST_ASSERT(foc_pipeline_step(&pipeline, &input) == FOC_PIPELINE_ERR_STOP);
    TEST_ASSERT(inhibited(&pipeline) && pipeline.mock.updates == tick);
    TEST_ASSERT(
        pipeline.current.integral_v.d == committed.d && pipeline.current.integral_v.q == committed.q
    );
    /* Stop the model at INHIBIT. No bridge-off electrical response is modeled. */
    printf(
        "final=INHIBIT accepted=%u model_steps=%u\n",
        (unsigned int)pipeline.mock.updates,
        (unsigned int)tick
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    return test_startup_and_recovery() || test_final_gate() || test_rejections() ||
        test_time_and_replay() || test_instances_and_determinism() || test_closed_loop();
}

/* ---------------------------------------- end of file ----------------------------------------- */
