/**
 * @file    test_foc_device_sim.c
 * @brief   Host contract tests for the closed-gate G3-NP offline chain.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Test role
 * ---------
 * This executable is the host acceptance owner for the closed-gate simulator.
 *
 * The test verifies fault priority, freshness, timing, recovery, mapping, and
 * the guarantee that the mock bridge never reports physical enable.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_device_sim.h"

#include <math.h>
#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */

static int test_assert(int condition, const char *expression, int line)
{
    if (!condition) {
        fprintf(stderr, "assertion failed: %s at line %d\n", expression, line);
        return 0;
    }
    return 1;
}

#define TEST_ASSERT(condition) test_assert((condition), #condition, __LINE__)

static struct foc_device_sim_config make_config(enum foc_device_phase_order phase_order)
{
    struct foc_device_sim_config config = {
        .core_config =
            {
                .vbus_min_v = 6.0f,
                .voltage_limit_v = 4.0f,
                .duty_min = 0.28f,
                .duty_max = 0.72f,
                .max_dt_s = 0.01f,
            },
        .adc_config =
            {
                .vbus_v_per_count = (3.3f / 4095.0f) * 11.0f,
                .current_a_per_count = 3.3f / (4095.0f * 0.5f),
                .vbus_min_v = 6.0f,
                .phase_a_offset_count = 2048.0f,
                .phase_b_offset_count = 2048.0f,
                .phase_c_offset_count = 2048.0f,
                .phase_order = phase_order,
            },
        .pwm_config =
            {
                .period_counts = 3749U,
                .phase_order = phase_order,
            },
        .max_sample_age_ticks = 10U,
    };

    return config;
}

static struct foc_device_sim_input make_input(uint32_t sample_seq, uint32_t sample_tick)
{
    struct foc_device_sim_input input = {
        .command =
            {
                .mode = FOC_CONTROL_VOLTAGE_OPEN_LOOP,
                .enable = 1U,
                .vd_v = 0.0f,
                .vq_v = 1.0f,
                .electrical_speed_rad_s = 100.0f,
            },
        .sample =
            {
                .vbus_raw = 1354U,
                .rank2_raw = 2058U,
                .rank3_raw = 2043U,
                .rank4_raw = 2043U,
                .valid_mask = FOC_DEVICE_SAMPLE_ALL_VALID,
                .sample_seq = sample_seq,
                .sample_tick = sample_tick,
            },
        .now_tick = sample_tick,
        .timing = {.dt_s = 0.001f},
    };

    return input;
}

static int result_is_inhibited(const struct foc_device_sim_result *result)
{
    return result->pwm_request.action == FOC_DEVICE_OUTPUT_INHIBIT &&
        result->pwm_request.output_enable_request == 0U &&
        result->registers.bridge_gate_closed != 0U && result->registers.bridge_enabled == 0U &&
        result->registers.channel1_compare == 0U && result->registers.channel2_compare == 0U &&
        result->registers.channel3_compare == 0U;
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    struct foc_device_sim sim = {0};
    struct foc_device_sim_result result = {0};
    struct foc_device_sim_input input = make_input(1U, 100U);
    const struct foc_device_sim_config config = make_config(FOC_DEVICE_PHASE_ORDER_ABC);
    float previous_theta;

    if (!TEST_ASSERT(foc_device_sim_init(&sim, &config) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(foc_device_sim_step(&sim, NULL, &result) == FOC_DEVICE_STATUS_ERR_NULL))
        return 1;
    if (!TEST_ASSERT(result_is_inhibited(&result)))
        return 1;
    if (!TEST_ASSERT(
            foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_ERR_NOT_RUNNING
        ))
        return 1;
    if (!TEST_ASSERT((result.fault_bits & FOC_DEVICE_SIM_FAULT_NOT_RUNNING) != 0U))
        return 1;
    if (!TEST_ASSERT(result_is_inhibited(&result)))
        return 1;
    if (!TEST_ASSERT(foc_device_sim_set_running(&sim) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(result.core_status == FOC_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(result.core_output.output_enable != 0U))
        return 1;
    if (!TEST_ASSERT(result.pwm_request.action == FOC_DEVICE_OUTPUT_UPDATE))
        return 1;
    if (!TEST_ASSERT(result.pwm_request.output_enable_request != 0U))
        return 1;
    if (!TEST_ASSERT(result.registers.bridge_gate_closed != 0U))
        return 1;
    if (!TEST_ASSERT(result.registers.bridge_enabled == 0U))
        return 1;
    if (!TEST_ASSERT(
            result.registers.channel3_compare == (uint32_t)(result.pwm_request.duty_a * 3749.0f)
        ))
        return 1;
    if (!TEST_ASSERT(
            result.registers.channel2_compare == (uint32_t)(result.pwm_request.duty_b * 3749.0f)
        ))
        return 1;
    if (!TEST_ASSERT(
            result.registers.channel1_compare == (uint32_t)(result.pwm_request.duty_c * 3749.0f)
        ))
        return 1;

    {
        struct foc_device_sim acb_sim = {0};
        struct foc_device_sim_result acb_result = {0};
        struct foc_device_sim_input acb_input = input;
        const struct foc_device_sim_config acb_config = make_config(FOC_DEVICE_PHASE_ORDER_ACB);

        if (!TEST_ASSERT(foc_device_sim_init(&acb_sim, &acb_config) == FOC_DEVICE_STATUS_OK))
            return 1;
        if (!TEST_ASSERT(foc_device_sim_set_running(&acb_sim) == FOC_DEVICE_STATUS_OK))
            return 1;
        if (!TEST_ASSERT(
                foc_device_sim_step(&acb_sim, &acb_input, &acb_result) == FOC_DEVICE_STATUS_OK
            ))
            return 1;
        if (!TEST_ASSERT(
                acb_result.registers.channel3_compare ==
                (uint32_t)(acb_result.pwm_request.duty_a * 3749.0f)
            ))
            return 1;
        if (!TEST_ASSERT(
                acb_result.registers.channel2_compare ==
                (uint32_t)(acb_result.pwm_request.duty_c * 3749.0f)
            ))
            return 1;
        if (!TEST_ASSERT(
                acb_result.registers.channel1_compare ==
                (uint32_t)(acb_result.pwm_request.duty_b * 3749.0f)
            ))
            return 1;
    }

    {
        struct foc_device_pwm_request invalid = result.pwm_request;
        struct foc_device_sim_registers registers = result.registers;
        const struct foc_device_sim_input zero_seq = make_input(0U, 101U);

        invalid.channel3_compare = config.pwm_config.period_counts + 1U;
        if (!TEST_ASSERT(
                foc_device_sim_submit(&registers, &invalid, config.pwm_config.period_counts) ==
                FOC_DEVICE_STATUS_ERR_INVALID_OUTPUT
            ))
            return 1;
        if (!TEST_ASSERT(
                registers.channel1_compare == result.registers.channel1_compare &&
                registers.channel2_compare == result.registers.channel2_compare &&
                registers.channel3_compare == result.registers.channel3_compare
            ))
            return 1;
        if (!TEST_ASSERT(
                foc_device_sim_step(&sim, &zero_seq, &result) ==
                    FOC_DEVICE_STATUS_ERR_INVALID_SAMPLE &&
                result_is_inhibited(&result)
            ))
            return 1;
    }

    previous_theta = sim.core.theta_elec_rad;
    input.sample.sample_seq = 1U;
    input.now_tick = 101U;
    input.sample.sample_tick = 101U;
    if (!TEST_ASSERT(
            foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_ERR_STALE_SAMPLE
        ))
        return 1;
    if (!TEST_ASSERT((result.fault_bits & FOC_DEVICE_SIM_FAULT_SAMPLE_STALE) != 0U))
        return 1;
    if (!TEST_ASSERT(result_is_inhibited(&result)))
        return 1;
    if (!TEST_ASSERT(sim.core.theta_elec_rad == previous_theta))
        return 1;

    input.sample.sample_seq = 2U;
    input.sample.valid_mask = FOC_DEVICE_SAMPLE_VBUS_VALID | FOC_DEVICE_SAMPLE_RANK2_VALID |
        FOC_DEVICE_SAMPLE_RANK3_VALID;
    if (!TEST_ASSERT(
            foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_ERR_INVALID_SAMPLE
        ))
        return 1;
    if (!TEST_ASSERT((result.fault_bits & FOC_DEVICE_SIM_FAULT_SAMPLE_INVALID) != 0U))
        return 1;
    if (!TEST_ASSERT(result_is_inhibited(&result)))
        return 1;

    input.sample.sample_seq = 3U;
    input.sample.valid_mask = FOC_DEVICE_SAMPLE_ALL_VALID;
    input.sample.sample_tick = 200U;
    input.now_tick = 200U;
    if (!TEST_ASSERT(foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_OK))
        return 1;

    input.sample.sample_seq = 4U;
    input.sample.sample_tick = 201U;
    input.now_tick = 201U;
    input.timing.dt_s = 0.0f;
    previous_theta = sim.core.theta_elec_rad;
    if (!TEST_ASSERT(
            foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_ERR_INVALID_TIME
        ))
        return 1;
    if (!TEST_ASSERT((result.fault_bits & FOC_DEVICE_SIM_FAULT_TIME_INVALID) != 0U))
        return 1;
    if (!TEST_ASSERT(result_is_inhibited(&result)))
        return 1;
    if (!TEST_ASSERT(sim.core.theta_elec_rad == previous_theta))
        return 1;

    input.timing.dt_s = 0.001f;
    input.sample.sample_seq = 5U;
    input.sample.sample_tick = 202U;
    input.now_tick = 220U;
    if (!TEST_ASSERT(
            foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_ERR_STALE_SAMPLE
        ))
        return 1;
    if (!TEST_ASSERT(result_is_inhibited(&result)))
        return 1;

    input.sample.sample_seq = 6U;
    input.sample.sample_tick = 220U;
    input.now_tick = 220U;
    input.command.enable = 0U;
    if (!TEST_ASSERT(foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(result.core_output.output_enable == 0U))
        return 1;
    if (!TEST_ASSERT(result_is_inhibited(&result)))
        return 1;

    if (!TEST_ASSERT(
            foc_device_sim_latch_fault(&sim, FOC_DEVICE_SIM_FAULT_CORE) == FOC_DEVICE_STATUS_OK
        ))
        return 1;
    if (!TEST_ASSERT(foc_device_sim_set_disabled(&sim) == FOC_DEVICE_STATUS_ERR_FAULT_LATCHED))
        return 1;
    input.command.enable = 1U;
    input.sample.sample_seq = 7U;
    input.sample.sample_tick = 221U;
    input.now_tick = 221U;
    input.sample.valid_mask = 0U;
    if (!TEST_ASSERT(
            foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_ERR_FAULT_LATCHED
        ))
        return 1;
    if (!TEST_ASSERT(result.fault_bits == FOC_DEVICE_SIM_FAULT_CORE))
        return 1;
    if (!TEST_ASSERT(result_is_inhibited(&result)))
        return 1;

    if (!TEST_ASSERT(foc_device_sim_clear_fault(&sim) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(foc_device_sim_set_running(&sim) == FOC_DEVICE_STATUS_OK))
        return 1;
    input.sample.valid_mask = FOC_DEVICE_SAMPLE_ALL_VALID;
    input.sample.sample_seq = 8U;
    input.sample.sample_tick = 222U;
    input.now_tick = 222U;
    if (!TEST_ASSERT(foc_device_sim_step(&sim, &input, &result) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(isfinite(result.core_output.theta_elec_rad) != 0))
        return 1;

    puts("test_foc_device_sim passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
