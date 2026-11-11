/**
 * @file    test_foc_device_contract.c
 * @brief   Host contract tests for G3-NP ADC and PWM mapping.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Test role
 * ---------
 * This executable is the host acceptance owner for the Device mapping contract.
 *
 * The test uses source-derived Board1 facts as data and verifies conversion,
 * phase order, action semantics, and deterministic invalid-output handling.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_device.h"

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

static uint32_t duty_to_compare(float duty, uint32_t period_counts)
{
    return (uint32_t)(duty * (float)period_counts);
}

/* Test Entry ------------------------------------------------------------------ */

int main(void)
{
    const struct foc_device_adc_config adc_config = {
        .vbus_v_per_count = (3.3f / 4095.0f) * 11.0f,
        .current_a_per_count = 3.3f / (4095.0f * 0.5f),
        .vbus_min_v = 6.0f,
        .phase_a_offset_count = 2048.0f,
        .phase_b_offset_count = 2048.0f,
        .phase_c_offset_count = 2048.0f,
        .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
    };
    const struct foc_device_pwm_config pwm_config = {
        .period_counts = 3749U,
        .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
    };
    const struct foc_device_adc_raw raw = {
        .vbus_raw = 1354U,
        .rank2_raw = 2058U,
        .rank3_raw = 2043U,
        .rank4_raw = 2043U,
        .valid_mask = FOC_DEVICE_SAMPLE_ALL_VALID,
        .sample_seq = 1U,
        .sample_tick = 100U,
    };
    struct foc_device_measurement measurement = {0};
    struct foc_output core_output = {0};
    struct foc_device_pwm_request request = {0};

    if (!TEST_ASSERT(foc_device_adc_config_validate(&adc_config) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(foc_device_pwm_config_validate(&pwm_config) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(
            foc_device_decode_sample(&raw, &adc_config, &measurement) == FOC_DEVICE_STATUS_OK
        ))
        return 1;
    if (!TEST_ASSERT(measurement.core.vbus_v > 11.0f))
        return 1;
    if (!TEST_ASSERT(measurement.phase_current_a.a > 0.0f))
        return 1;
    if (!TEST_ASSERT(measurement.phase_current_a.b < 0.0f))
        return 1;
    if (!TEST_ASSERT(measurement.phase_current_a.c < 0.0f))
        return 1;
    if (!TEST_ASSERT(measurement.sample_seq == raw.sample_seq))
        return 1;
    if (!TEST_ASSERT(measurement.sample_tick == raw.sample_tick))
        return 1;

    {
        struct foc_device_adc_raw wrapped = raw;

        wrapped.sample_seq = 0U;
        wrapped.sample_tick = 0U;
        if (!TEST_ASSERT(
                foc_device_decode_sample(&wrapped, &adc_config, &measurement) ==
                    FOC_DEVICE_STATUS_OK &&
                measurement.sample_seq == 0U && measurement.sample_tick == 0U
            ))
            return 1;
    }

    {
        struct foc_device_adc_config acb_config = adc_config;
        struct foc_device_adc_raw acb_raw = raw;

        acb_config.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
        acb_raw.rank3_raw = 2058U;
        acb_raw.rank4_raw = 2043U;
        if (!TEST_ASSERT(
                foc_device_decode_sample(&acb_raw, &acb_config, &measurement) ==
                FOC_DEVICE_STATUS_OK
            ))
            return 1;
        if (!TEST_ASSERT(
                measurement.phase_current_a.b < 0.0f && measurement.phase_current_a.c > 0.0f
            ))
            return 1;
    }

    core_output.output_enable = 1U;
    core_output.svpwm.duty_a = 0.60f;
    core_output.svpwm.duty_b = 0.40f;
    core_output.svpwm.duty_c = 0.50f;
    if (!TEST_ASSERT(
            foc_device_map_output(&core_output, &pwm_config, &request) == FOC_DEVICE_STATUS_OK
        ))
        return 1;
    if (!TEST_ASSERT(request.action == FOC_DEVICE_OUTPUT_UPDATE))
        return 1;
    if (!TEST_ASSERT(request.output_enable_request != 0U))
        return 1;
    if (!TEST_ASSERT(request.channel3_compare == duty_to_compare(0.60f, 3749U)))
        return 1;
    if (!TEST_ASSERT(request.channel2_compare == duty_to_compare(0.40f, 3749U)))
        return 1;
    if (!TEST_ASSERT(request.channel1_compare == duty_to_compare(0.50f, 3749U)))
        return 1;

    {
        struct foc_device_pwm_config acb_config = pwm_config;

        acb_config.phase_order = FOC_DEVICE_PHASE_ORDER_ACB;
        if (!TEST_ASSERT(
                foc_device_map_output(&core_output, &acb_config, &request) == FOC_DEVICE_STATUS_OK
            ))
            return 1;
        if (!TEST_ASSERT(request.channel3_compare == duty_to_compare(0.60f, 3749U)))
            return 1;
        if (!TEST_ASSERT(request.channel2_compare == duty_to_compare(0.50f, 3749U)))
            return 1;
        if (!TEST_ASSERT(request.channel1_compare == duty_to_compare(0.40f, 3749U)))
            return 1;
    }

    core_output.output_enable = 0U;
    if (!TEST_ASSERT(
            foc_device_map_output(&core_output, &pwm_config, &request) == FOC_DEVICE_STATUS_OK
        ))
        return 1;
    if (!TEST_ASSERT(request.action == FOC_DEVICE_OUTPUT_INHIBIT))
        return 1;
    if (!TEST_ASSERT(request.output_enable_request == 0U))
        return 1;
    if (!TEST_ASSERT(
            request.channel1_compare == 0U && request.channel2_compare == 0U &&
            request.channel3_compare == 0U
        ))
        return 1;

    if (!TEST_ASSERT(foc_device_output_zero_duty(&pwm_config, &request) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(
            request.action == FOC_DEVICE_OUTPUT_ZERO_DUTY && request.output_enable_request != 0U
        ))
        return 1;
    if (!TEST_ASSERT(foc_device_output_active_brake(&pwm_config, &request) == FOC_DEVICE_STATUS_OK))
        return 1;
    if (!TEST_ASSERT(
            request.action == FOC_DEVICE_OUTPUT_ACTIVE_BRAKE && request.output_enable_request != 0U
        ))
        return 1;

    core_output.output_enable = 1U;
    core_output.svpwm.duty_a = NAN;
    request.channel1_compare = 123U;
    if (!TEST_ASSERT(
            foc_device_map_output(&core_output, &pwm_config, &request) ==
            FOC_DEVICE_STATUS_ERR_INVALID_OUTPUT
        ))
        return 1;
    if (!TEST_ASSERT(
            request.action == FOC_DEVICE_OUTPUT_INHIBIT && request.channel1_compare == 0U &&
            request.channel2_compare == 0U && request.channel3_compare == 0U
        ))
        return 1;

    {
        struct foc_device_adc_raw invalid_raw = raw;

        invalid_raw.valid_mask = FOC_DEVICE_SAMPLE_VBUS_VALID | FOC_DEVICE_SAMPLE_RANK2_VALID |
            FOC_DEVICE_SAMPLE_RANK3_VALID;
        if (!TEST_ASSERT(
                foc_device_decode_sample(&invalid_raw, &adc_config, &measurement) ==
                FOC_DEVICE_STATUS_ERR_INVALID_SAMPLE
            ))
            return 1;
    }

    if (!TEST_ASSERT(isfinite(measurement.core.vbus_v) != 0))
        return 1;
    {
        const struct foc_device_pwm_config wide_pwm = {UINT32_MAX, FOC_DEVICE_PHASE_ORDER_ABC};

        core_output.svpwm.duty_a = 1.0f;
        if (!TEST_ASSERT(
                foc_device_map_output(&core_output, &wide_pwm, &request) == FOC_DEVICE_STATUS_OK &&
                request.channel3_compare == UINT32_MAX
            ))
            return 1;
    }
    puts("test_foc_device_contract passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
