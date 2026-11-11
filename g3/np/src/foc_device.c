/**
 * @file    foc_device.c
 * @brief   Host-only Board1 ADC and PWM mapping implementation for G3-NP.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_device.h` and owns no hardware registers
 * - preserves the source-derived Board1 rank, phase, and timer mappings
 * - emits deterministic inhibit metadata before publishing any update request
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_device.h"

#include <math.h>

/* Private Helpers -------------------------------------------------------------- */

static int foc_device_float_is_finite(float value)
{
    return isfinite(value) != 0;
}

static int foc_device_phase_order_valid(enum foc_device_phase_order phase_order)
{
    return phase_order == FOC_DEVICE_PHASE_ORDER_ABC || phase_order == FOC_DEVICE_PHASE_ORDER_ACB;
}

static uint32_t foc_device_duty_to_compare(float duty, uint32_t period_counts)
{
    const float period = (float)period_counts;
    const float compare = duty * period;

    /* At full duty, a float-rounded uint32 period may be outside uint32 range. */
    return compare >= period ? period_counts : (uint32_t)compare;
}

static void foc_device_set_inhibit_fields(struct foc_device_pwm_request *request)
{
    request->action = FOC_DEVICE_OUTPUT_INHIBIT;
    request->output_enable_request = 0U;
    request->duty_a = 0.0f;
    request->duty_b = 0.0f;
    request->duty_c = 0.0f;
    request->channel1_compare = 0U;
    request->channel2_compare = 0U;
    request->channel3_compare = 0U;
}

/* Public Functions ------------------------------------------------------------- */

int foc_device_adc_config_validate(const struct foc_device_adc_config *config)
{
    if (config == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }

    if (!foc_device_float_is_finite(config->vbus_v_per_count) ||
        !foc_device_float_is_finite(config->current_a_per_count) ||
        !foc_device_float_is_finite(config->vbus_min_v) ||
        !foc_device_float_is_finite(config->phase_a_offset_count) ||
        !foc_device_float_is_finite(config->phase_b_offset_count) ||
        !foc_device_float_is_finite(config->phase_c_offset_count)) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }

    if (config->vbus_v_per_count <= 0.0f || config->current_a_per_count <= 0.0f ||
        config->vbus_min_v <= 0.0f || !foc_device_phase_order_valid(config->phase_order)) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }

    return FOC_DEVICE_STATUS_OK;
}

int foc_device_pwm_config_validate(const struct foc_device_pwm_config *config)
{
    if (config == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }

    if (config->period_counts == 0U || !foc_device_phase_order_valid(config->phase_order)) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }

    return FOC_DEVICE_STATUS_OK;
}

int foc_device_decode_sample(
    const struct foc_device_adc_raw *raw,
    const struct foc_device_adc_config *config,
    struct foc_device_measurement *measurement
)
{
    float phase_a_raw;
    float phase_b_raw;
    float phase_c_raw;
    struct foc_device_measurement decoded;

    if (measurement == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    *measurement = (struct foc_device_measurement){0};
    if (raw == NULL || config == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }

    if (foc_device_adc_config_validate(config) != FOC_DEVICE_STATUS_OK) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }

    if ((raw->valid_mask & FOC_DEVICE_SAMPLE_ALL_VALID) != FOC_DEVICE_SAMPLE_ALL_VALID) {
        return FOC_DEVICE_STATUS_ERR_INVALID_SAMPLE;
    }

    decoded.core.vbus_v = (float)raw->vbus_raw * config->vbus_v_per_count;
    phase_a_raw = (float)raw->rank2_raw;
    phase_b_raw = (float)raw->rank3_raw;
    phase_c_raw = (float)raw->rank4_raw;

    /* ACB is a physical label swap, not a change to the ADC rank sequence. */
    if (config->phase_order == FOC_DEVICE_PHASE_ORDER_ACB) {
        const float phase_b_raw_saved = phase_b_raw;

        phase_b_raw = phase_c_raw;
        phase_c_raw = phase_b_raw_saved;
    }

    decoded.phase_current_a.a =
        (phase_a_raw - config->phase_a_offset_count) * config->current_a_per_count;
    decoded.phase_current_a.b =
        (phase_b_raw - config->phase_b_offset_count) * config->current_a_per_count;
    decoded.phase_current_a.c =
        (phase_c_raw - config->phase_c_offset_count) * config->current_a_per_count;
    decoded.sample_seq = raw->sample_seq;
    decoded.sample_tick = raw->sample_tick;

    if (!foc_device_float_is_finite(decoded.core.vbus_v) ||
        !foc_device_float_is_finite(decoded.phase_current_a.a) ||
        !foc_device_float_is_finite(decoded.phase_current_a.b) ||
        !foc_device_float_is_finite(decoded.phase_current_a.c) ||
        decoded.core.vbus_v < config->vbus_min_v) {
        return FOC_DEVICE_STATUS_ERR_INVALID_SAMPLE;
    }

    *measurement = decoded;
    return FOC_DEVICE_STATUS_OK;
}

int foc_device_output_inhibit(
    const struct foc_device_pwm_config *config,
    struct foc_device_pwm_request *request
)
{
    if (request == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    foc_device_set_inhibit_fields(request);

    if (foc_device_pwm_config_validate(config) != FOC_DEVICE_STATUS_OK) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }

    return FOC_DEVICE_STATUS_OK;
}

static int foc_device_output_non_inhibit_action(
    const struct foc_device_pwm_config *config,
    struct foc_device_pwm_request *request,
    enum foc_device_output_action action
)
{
    if (request == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    foc_device_set_inhibit_fields(request);
    if (foc_device_pwm_config_validate(config) != FOC_DEVICE_STATUS_OK) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }
    request->action = action;
    request->output_enable_request = 1U;
    return FOC_DEVICE_STATUS_OK;
}

int foc_device_output_zero_duty(
    const struct foc_device_pwm_config *config,
    struct foc_device_pwm_request *request
)
{
    return foc_device_output_non_inhibit_action(config, request, FOC_DEVICE_OUTPUT_ZERO_DUTY);
}

int foc_device_output_active_brake(
    const struct foc_device_pwm_config *config,
    struct foc_device_pwm_request *request
)
{
    return foc_device_output_non_inhibit_action(config, request, FOC_DEVICE_OUTPUT_ACTIVE_BRAKE);
}

int foc_device_map_output(
    const struct foc_output *core_output,
    const struct foc_device_pwm_config *config,
    struct foc_device_pwm_request *request
)
{
    uint32_t compare_a;
    uint32_t compare_b;
    uint32_t compare_c;

    if (request == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    foc_device_set_inhibit_fields(request);
    if (core_output == NULL || config == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }

    if (foc_device_pwm_config_validate(config) != FOC_DEVICE_STATUS_OK) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }

    if (core_output->output_enable == 0U) {
        return foc_device_output_inhibit(config, request);
    }

    if (!foc_device_float_is_finite(core_output->svpwm.duty_a) ||
        !foc_device_float_is_finite(core_output->svpwm.duty_b) ||
        !foc_device_float_is_finite(core_output->svpwm.duty_c) ||
        core_output->svpwm.duty_a < 0.0f || core_output->svpwm.duty_a > 1.0f ||
        core_output->svpwm.duty_b < 0.0f || core_output->svpwm.duty_b > 1.0f ||
        core_output->svpwm.duty_c < 0.0f || core_output->svpwm.duty_c > 1.0f) {
        return FOC_DEVICE_STATUS_ERR_INVALID_OUTPUT;
    }

    compare_a = foc_device_duty_to_compare(core_output->svpwm.duty_a, config->period_counts);
    compare_b = foc_device_duty_to_compare(core_output->svpwm.duty_b, config->period_counts);
    compare_c = foc_device_duty_to_compare(core_output->svpwm.duty_c, config->period_counts);

    request->action = FOC_DEVICE_OUTPUT_UPDATE;
    request->output_enable_request = 1U;
    request->duty_a = core_output->svpwm.duty_a;
    request->duty_b = core_output->svpwm.duty_b;
    request->duty_c = core_output->svpwm.duty_c;
    /* Board1 keeps phase A on CH3 and uses CH1/CH2 for the B/C pair. */
    request->channel3_compare = compare_a;
    if (config->phase_order == FOC_DEVICE_PHASE_ORDER_ABC) {
        request->channel2_compare = compare_b;
        request->channel1_compare = compare_c;
    } else {
        request->channel2_compare = compare_c;
        request->channel1_compare = compare_b;
    }
    return FOC_DEVICE_STATUS_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
