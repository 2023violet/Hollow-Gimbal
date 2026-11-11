/**
 * @file    foc_safe.c
 * @brief   No-power G3-SAFE state, freshness, heartbeat, and guard implementation.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_safe.h` and owns only host safety-contract state
 * - makes freshness and heartbeat decisions from explicit monotonic time
 * - never accesses WDT, Flash, ADC, PWM, ISR, or board registers
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_safe.h"

#include <math.h>
#include <stddef.h>

/* Private Helpers -------------------------------------------------------------- */

static int foc_safe_config_valid(const struct foc_safe_config *config)
{
    return config != NULL && config->command_timeout_us > 0U && config->sample_timeout_us > 0U &&
        config->fast_heartbeat_timeout_us > 0U && config->foreground_heartbeat_timeout_us > 0U &&
        config->invalid_sample_latch_after > 0U;
}

static int foc_safe_timestamp_not_before(uint32_t now_us, uint32_t previous_us, bool seen)
{
    const uint32_t elapsed_us = now_us - previous_us;

    return !seen || elapsed_us < 0x80000000U;
}

static int foc_safe_elapsed_exceeded(uint32_t now_us, uint32_t previous_us, uint32_t timeout_us)
{
    const uint32_t elapsed_us = now_us - previous_us;

    return elapsed_us >= 0x80000000U || elapsed_us > timeout_us;
}

static void foc_safe_decision_reset(struct foc_safe_output_decision *decision)
{
    decision->permitted = 0U;
    decision->inhibit_reasons = FOC_SAFE_FAULT_NONE;
    decision->request.action = FOC_DEVICE_OUTPUT_INHIBIT;
    decision->request.output_enable_request = 0U;
    decision->request.duty_a = 0.0f;
    decision->request.duty_b = 0.0f;
    decision->request.duty_c = 0.0f;
    decision->request.channel1_compare = 0U;
    decision->request.channel2_compare = 0U;
    decision->request.channel3_compare = 0U;
}

static int foc_safe_heartbeat_healthy(const struct foc_safe_context *context, uint32_t now_us)
{
    if (!context->fast_heartbeat_seen ||
        foc_safe_elapsed_exceeded(
            now_us,
            context->last_fast_heartbeat_us,
            context->config.fast_heartbeat_timeout_us
        )) {
        return 0;
    }
    if (!context->foreground_heartbeat_seen ||
        foc_safe_elapsed_exceeded(
            now_us,
            context->last_foreground_heartbeat_us,
            context->config.foreground_heartbeat_timeout_us
        )) {
        return 0;
    }
    return 1;
}

static int foc_safe_running_inputs_healthy(const struct foc_safe_context *context, uint32_t now_us)
{
    if (!context->command_seen ||
        foc_safe_elapsed_exceeded(
            now_us,
            context->last_command_us,
            context->config.command_timeout_us
        )) {
        return 0;
    }
    if (!context->sample_seen ||
        foc_safe_elapsed_exceeded(
            now_us,
            context->last_sample_us,
            context->config.sample_timeout_us
        )) {
        return 0;
    }
    return 1;
}

static uint32_t
foc_safe_current_health_faults(const struct foc_safe_context *context, uint32_t now_us)
{
    uint32_t faults = FOC_SAFE_FAULT_NONE;

    if (!foc_safe_heartbeat_healthy(context, now_us)) {
        if (!context->fast_heartbeat_seen ||
            foc_safe_elapsed_exceeded(
                now_us,
                context->last_fast_heartbeat_us,
                context->config.fast_heartbeat_timeout_us
            )) {
            faults |= FOC_SAFE_FAULT_FAST_HEARTBEAT;
        }
        if (!context->foreground_heartbeat_seen ||
            foc_safe_elapsed_exceeded(
                now_us,
                context->last_foreground_heartbeat_us,
                context->config.foreground_heartbeat_timeout_us
            )) {
            faults |= FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT;
        }
    }

    if (context->state == FOC_SAFE_STATE_RUNNING &&
        !foc_safe_running_inputs_healthy(context, now_us)) {
        if (!context->command_seen ||
            foc_safe_elapsed_exceeded(
                now_us,
                context->last_command_us,
                context->config.command_timeout_us
            )) {
            faults |= FOC_SAFE_FAULT_COMMAND_TIMEOUT;
        }
        if (!context->sample_seen ||
            foc_safe_elapsed_exceeded(
                now_us,
                context->last_sample_us,
                context->config.sample_timeout_us
            )) {
            faults |= FOC_SAFE_FAULT_SAMPLE_TIMEOUT;
        }
    }
    return faults;
}

static int foc_safe_update_request_valid(const struct foc_device_pwm_request *request)
{
    return request->action == FOC_DEVICE_OUTPUT_UPDATE && request->output_enable_request != 0U &&
        isfinite(request->duty_a) != 0 && isfinite(request->duty_b) != 0 &&
        isfinite(request->duty_c) != 0 && request->duty_a >= 0.0f && request->duty_a <= 1.0f &&
        request->duty_b >= 0.0f && request->duty_b <= 1.0f && request->duty_c >= 0.0f &&
        request->duty_c <= 1.0f;
}

/* Public Functions ------------------------------------------------------------- */

int foc_safe_init(struct foc_safe_context *context, const struct foc_safe_config *config)
{
    if (context == NULL || config == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (!foc_safe_config_valid(config)) {
        return FOC_SAFE_STATUS_ERR_CONFIG;
    }

    *context = (struct foc_safe_context){0};
    context->config = *config;
    context->state = FOC_SAFE_STATE_BOOT;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_boot_complete(struct foc_safe_context *context)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (context->state != FOC_SAFE_STATE_BOOT) {
        return FOC_SAFE_STATUS_ERR_STATE;
    }
    context->state = FOC_SAFE_STATE_DISABLED;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_arm(struct foc_safe_context *context)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (context->state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_SAFE_STATUS_ERR_FAULT_LATCHED;
    }
    if (context->state != FOC_SAFE_STATE_DISABLED) {
        return FOC_SAFE_STATUS_ERR_STATE;
    }
    context->state = FOC_SAFE_STATE_READY;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_start(struct foc_safe_context *context)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (context->state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_SAFE_STATUS_ERR_FAULT_LATCHED;
    }
    if (context->state != FOC_SAFE_STATE_READY) {
        return FOC_SAFE_STATUS_ERR_STATE;
    }
    context->state = FOC_SAFE_STATE_RUNNING;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_disable(struct foc_safe_context *context)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (context->state == FOC_SAFE_STATE_BOOT || context->state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_SAFE_STATUS_ERR_STATE;
    }
    context->state = FOC_SAFE_STATE_DISABLED;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_latch_fault(struct foc_safe_context *context, uint32_t fault_bits)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (fault_bits == FOC_SAFE_FAULT_NONE) {
        return FOC_SAFE_STATUS_ERR_INVALID_INPUT;
    }
    context->latched_faults |= fault_bits;
    context->state = FOC_SAFE_STATE_FAULT_LATCHED;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_clear_fault(struct foc_safe_context *context)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (context->state != FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_SAFE_STATUS_ERR_STATE;
    }
    context->latched_faults = FOC_SAFE_FAULT_NONE;
    context->command_seen = false;
    context->sample_seen = false;
    context->sample_current_valid = false;
    context->fast_heartbeat_seen = false;
    context->foreground_heartbeat_seen = false;
    context->invalid_sample_streak = 0U;
    context->state = FOC_SAFE_STATE_DISABLED;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_sequence_newer(uint32_t sequence, uint32_t previous_sequence, bool seen)
{
    const uint32_t delta = sequence - previous_sequence;

    return !seen || (delta != 0U && delta < 0x80000000U);
}

int foc_safe_timestamp_fresh(uint32_t now_us, uint32_t observed_us, uint32_t timeout_us)
{
    return !foc_safe_elapsed_exceeded(now_us, observed_us, timeout_us);
}

int foc_safe_observe_command(
    struct foc_safe_context *context,
    uint32_t now_us,
    uint32_t command_seq
)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (!foc_safe_timestamp_not_before(now_us, context->last_command_us, context->command_seen) ||
        !foc_safe_sequence_newer(command_seq, context->command_seq, context->command_seen)) {
        return FOC_SAFE_STATUS_ERR_INVALID_INPUT;
    }
    context->command_seq = command_seq;
    context->last_command_us = now_us;
    context->command_seen = true;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_observe_sample(
    struct foc_safe_context *context,
    uint32_t now_us,
    uint32_t sample_seq,
    bool sample_valid
)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }

    if (!sample_valid) {
        context->sample_current_valid = false;
        context->invalid_sample_streak++;
        if (context->invalid_sample_streak >= context->config.invalid_sample_latch_after) {
            (void)foc_safe_latch_fault(context, FOC_SAFE_FAULT_SAMPLE_INVALID);
        }
        return FOC_SAFE_STATUS_ERR_INVALID_INPUT;
    }

    if (!foc_safe_timestamp_not_before(now_us, context->last_sample_us, context->sample_seen) ||
        !foc_safe_sequence_newer(sample_seq, context->sample_seq, context->sample_seen)) {
        return FOC_SAFE_STATUS_ERR_INVALID_INPUT;
    }
    context->sample_seq = sample_seq;
    context->last_sample_us = now_us;
    context->sample_seen = true;
    context->sample_current_valid = true;
    context->invalid_sample_streak = 0U;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_heartbeat_fast(struct foc_safe_context *context, uint32_t now_us)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (!foc_safe_timestamp_not_before(
            now_us,
            context->last_fast_heartbeat_us,
            context->fast_heartbeat_seen
        )) {
        return FOC_SAFE_STATUS_ERR_INVALID_INPUT;
    }
    context->last_fast_heartbeat_us = now_us;
    context->fast_heartbeat_seen = true;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_heartbeat_foreground(struct foc_safe_context *context, uint32_t now_us)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (!foc_safe_timestamp_not_before(
            now_us,
            context->last_foreground_heartbeat_us,
            context->foreground_heartbeat_seen
        )) {
        return FOC_SAFE_STATUS_ERR_INVALID_INPUT;
    }
    context->last_foreground_heartbeat_us = now_us;
    context->foreground_heartbeat_seen = true;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_supervisor_tick(struct foc_safe_context *context, uint32_t now_us)
{
    uint32_t faults;

    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (context->state == FOC_SAFE_STATE_BOOT) {
        return FOC_SAFE_STATUS_ERR_STATE;
    }
    if (context->state == FOC_SAFE_STATE_FAULT_LATCHED) {
        if (!foc_safe_heartbeat_healthy(context, now_us)) {
            uint32_t heartbeat_faults = foc_safe_current_health_faults(context, now_us) &
                (FOC_SAFE_FAULT_FAST_HEARTBEAT | FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT);

            if (heartbeat_faults != FOC_SAFE_FAULT_NONE) {
                context->latched_faults |= heartbeat_faults;
                return FOC_SAFE_STATUS_ERR_TIMEOUT;
            }
        }
        return FOC_SAFE_STATUS_OK;
    }

    /* Idle states may boot before either heartbeat exists. */
    if (context->state != FOC_SAFE_STATE_RUNNING && !context->fast_heartbeat_seen &&
        !context->foreground_heartbeat_seen) {
        return FOC_SAFE_STATUS_OK;
    }

    faults = foc_safe_current_health_faults(context, now_us);
    if (context->state != FOC_SAFE_STATE_RUNNING) {
        faults &= FOC_SAFE_FAULT_FAST_HEARTBEAT | FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT;
    }
    if (faults != FOC_SAFE_FAULT_NONE) {
        (void)foc_safe_latch_fault(context, faults);
        return FOC_SAFE_STATUS_ERR_TIMEOUT;
    }
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_watchdog_permitted(struct foc_safe_context *context, uint32_t now_us)
{
    int status;

    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    status = foc_safe_supervisor_tick(context, now_us);
    if (status != FOC_SAFE_STATUS_OK) {
        if (context->state == FOC_SAFE_STATE_FAULT_LATCHED &&
            (context->latched_faults & FOC_SAFE_FAULT_FATAL_RESET_REQUIRED) == 0U &&
            foc_safe_heartbeat_healthy(context, now_us)) {
            return FOC_SAFE_STATUS_OK;
        }
        return FOC_SAFE_STATUS_ERR_NOT_PERMITTED;
    }
    if (context->state == FOC_SAFE_STATE_BOOT ||
        (context->latched_faults & FOC_SAFE_FAULT_FATAL_RESET_REQUIRED) != 0U ||
        !foc_safe_heartbeat_healthy(context, now_us)) {
        return FOC_SAFE_STATUS_ERR_NOT_PERMITTED;
    }
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_gate_output(
    struct foc_safe_context *context,
    uint32_t now_us,
    const struct foc_device_pwm_config *pwm_config,
    const struct foc_device_pwm_request *request,
    struct foc_safe_output_decision *decision
)
{
    uint32_t health_faults;
    int status;

    if (context == NULL || pwm_config == NULL || request == NULL || decision == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    foc_safe_decision_reset(decision);

    if (context->state == FOC_SAFE_STATE_FAULT_LATCHED) {
        decision->inhibit_reasons = context->latched_faults;
        (void)foc_device_output_inhibit(pwm_config, &decision->request);
        return FOC_SAFE_STATUS_OK;
    }

    status = foc_safe_supervisor_tick(context, now_us);
    if (status != FOC_SAFE_STATUS_OK) {
        decision->inhibit_reasons = context->latched_faults;
        (void)foc_device_output_inhibit(pwm_config, &decision->request);
        return FOC_SAFE_STATUS_OK;
    }

    health_faults = foc_safe_current_health_faults(context, now_us);
    if (context->state != FOC_SAFE_STATE_RUNNING || health_faults != FOC_SAFE_FAULT_NONE ||
        !context->sample_current_valid || !foc_safe_update_request_valid(request)) {
        decision->inhibit_reasons = health_faults;
        if (context->state != FOC_SAFE_STATE_RUNNING || !context->sample_current_valid ||
            !foc_safe_update_request_valid(request)) {
            if (!context->sample_current_valid) {
                decision->inhibit_reasons |= FOC_SAFE_FAULT_SAMPLE_INVALID;
            }
            decision->inhibit_reasons |= FOC_SAFE_FAULT_INVALID_STATE;
        }
        (void)foc_device_output_inhibit(pwm_config, &decision->request);
        return FOC_SAFE_STATUS_OK;
    }

    decision->permitted = 1U;
    decision->request = *request;
    return FOC_SAFE_STATUS_OK;
}

int foc_safe_flash_permitted(
    const struct foc_safe_context *context,
    enum foc_device_output_action output_action
)
{
    if (context == NULL) {
        return FOC_SAFE_STATUS_ERR_NULL;
    }
    if (context->state != FOC_SAFE_STATE_DISABLED ||
        context->latched_faults != FOC_SAFE_FAULT_NONE ||
        output_action != FOC_DEVICE_OUTPUT_INHIBIT) {
        return FOC_SAFE_STATUS_ERR_NOT_PERMITTED;
    }
    return FOC_SAFE_STATUS_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
