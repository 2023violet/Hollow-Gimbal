/**
 * @file    foc_device_sim.c
 * @brief   Closed-gate host simulator for the G3-NP control chain.
 * @version v0.1.0
 * @date    2026.11.09
 *
 * Implementation role
 * -------------------
 * - pairs with `foc_device_sim.h` and owns simulator-only state
 * - enforces state, freshness, timing, and fault priority before foc_step()
 * - records requests in mock registers while permanently keeping the bridge
 *   gate closed
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_device_sim.h"

#include <math.h>

/* Private Helpers -------------------------------------------------------------- */

static int foc_device_sim_float_is_finite(float value)
{
    return isfinite(value) != 0;
}

static void foc_device_sim_set_safe_core_output(struct foc_output *output)
{
    output->output_enable = 0U;
    output->theta_elec_rad = 0.0f;
    output->voltage_alpha_beta_v.alpha = 0.0f;
    output->voltage_alpha_beta_v.beta = 0.0f;
    output->svpwm.duty_a = 0.5f;
    output->svpwm.duty_b = 0.5f;
    output->svpwm.duty_c = 0.5f;
    output->svpwm.sector = 0U;
    output->svpwm.saturated = 1U;
}

static void foc_device_sim_set_safe_registers(struct foc_device_sim_registers *registers)
{
    registers->bridge_gate_closed = 1U;
    registers->bridge_enabled = 0U;
    registers->channel1_compare = 0U;
    registers->channel2_compare = 0U;
    registers->channel3_compare = 0U;
}

static int foc_device_sim_config_validate(const struct foc_device_sim_config *config)
{
    if (config == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    if (foc_config_validate(&config->core_config) != FOC_STATUS_OK ||
        foc_device_adc_config_validate(&config->adc_config) != FOC_DEVICE_STATUS_OK ||
        foc_device_pwm_config_validate(&config->pwm_config) != FOC_DEVICE_STATUS_OK ||
        config->max_sample_age_ticks == 0U) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }
    return FOC_DEVICE_STATUS_OK;
}

/* Public Functions ------------------------------------------------------------- */

int foc_device_sim_init(struct foc_device_sim *sim, const struct foc_device_sim_config *config)
{
    int status;

    if (sim == NULL || config == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }

    status = foc_device_sim_config_validate(config);
    if (status != FOC_DEVICE_STATUS_OK) {
        return status;
    }

    status = foc_init(&sim->core, &config->core_config);
    if (status != FOC_STATUS_OK) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }

    sim->config = *config;
    sim->state = FOC_DEVICE_SIM_DISABLED;
    sim->latched_faults = FOC_DEVICE_SIM_FAULT_NONE;
    sim->last_sample_seq = 0U;
    sim->last_sample_tick = 0U;
    foc_device_sim_set_safe_registers(&sim->registers);
    return FOC_DEVICE_STATUS_OK;
}

int foc_device_sim_set_running(struct foc_device_sim *sim)
{
    if (sim == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    /* A fault latch cannot be bypassed by cycling through disabled state. */
    if (sim->state == FOC_DEVICE_SIM_FAULT_LATCHED) {
        return FOC_DEVICE_STATUS_ERR_FAULT_LATCHED;
    }
    sim->state = FOC_DEVICE_SIM_RUNNING;
    return FOC_DEVICE_STATUS_OK;
}

int foc_device_sim_set_disabled(struct foc_device_sim *sim)
{
    if (sim == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    /* Latched faults require the explicit clear path before recovery. */
    if (sim->state == FOC_DEVICE_SIM_FAULT_LATCHED) {
        return FOC_DEVICE_STATUS_ERR_FAULT_LATCHED;
    }
    sim->state = FOC_DEVICE_SIM_DISABLED;
    foc_device_sim_set_safe_registers(&sim->registers);
    return FOC_DEVICE_STATUS_OK;
}

int foc_device_sim_latch_fault(struct foc_device_sim *sim, uint32_t fault_bits)
{
    if (sim == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    if (fault_bits == FOC_DEVICE_SIM_FAULT_NONE) {
        return FOC_DEVICE_STATUS_ERR_INVALID_CONFIG;
    }
    sim->latched_faults |= fault_bits;
    sim->state = FOC_DEVICE_SIM_FAULT_LATCHED;
    foc_device_sim_set_safe_registers(&sim->registers);
    return FOC_DEVICE_STATUS_OK;
}

int foc_device_sim_clear_fault(struct foc_device_sim *sim)
{
    if (sim == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    sim->latched_faults = FOC_DEVICE_SIM_FAULT_NONE;
    sim->state = FOC_DEVICE_SIM_DISABLED;
    sim->last_sample_seq = 0U;
    sim->last_sample_tick = 0U;
    (void)foc_reset(&sim->core);
    foc_device_sim_set_safe_registers(&sim->registers);
    return FOC_DEVICE_STATUS_OK;
}

static void
foc_device_sim_write_inhibit(struct foc_device_sim *sim, struct foc_device_sim_result *result)
{
    (void)foc_device_output_inhibit(&sim->config.pwm_config, &result->pwm_request);
    (void)foc_device_sim_submit(
        &sim->registers,
        &result->pwm_request,
        sim->config.pwm_config.period_counts
    );
    result->registers = sim->registers;
}

int foc_device_sim_submit(
    struct foc_device_sim_registers *registers,
    const struct foc_device_pwm_request *request,
    uint32_t period_counts
)
{
    if (registers == NULL || request == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    if (request->action == FOC_DEVICE_OUTPUT_INHIBIT && request->output_enable_request == 0U) {
        foc_device_sim_set_safe_registers(registers);
        return FOC_DEVICE_STATUS_OK;
    }
    /* Inclusive comparisons also reject NaN and infinities. */
    if (request->action != FOC_DEVICE_OUTPUT_UPDATE || request->output_enable_request == 0U ||
        !(request->duty_a >= 0.0f && request->duty_a <= 1.0f) ||
        !(request->duty_b >= 0.0f && request->duty_b <= 1.0f) ||
        !(request->duty_c >= 0.0f && request->duty_c <= 1.0f) || period_counts == 0U ||
        request->channel1_compare > period_counts || request->channel2_compare > period_counts ||
        request->channel3_compare > period_counts) {
        return FOC_DEVICE_STATUS_ERR_INVALID_OUTPUT;
    }

    *registers = (struct foc_device_sim_registers){
        .bridge_gate_closed = 1U,
        .channel1_compare = request->channel1_compare,
        .channel2_compare = request->channel2_compare,
        .channel3_compare = request->channel3_compare,
    };
    return FOC_DEVICE_STATUS_OK;
}

int foc_device_sim_step(
    struct foc_device_sim *sim,
    const struct foc_device_sim_input *input,
    struct foc_device_sim_result *result
)
{
    struct foc_device_measurement measurement;
    int device_status;
    int core_status;
    uint32_t fault_bits = FOC_DEVICE_SIM_FAULT_NONE;

    if (result == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    foc_device_sim_set_safe_core_output(&result->core_output);
    result->status = FOC_DEVICE_STATUS_ERR_NULL;
    result->core_status = FOC_STATUS_OK;
    result->fault_bits = FOC_DEVICE_SIM_FAULT_NONE;
    result->pwm_request.action = FOC_DEVICE_OUTPUT_INHIBIT;
    result->pwm_request.output_enable_request = 0U;
    result->pwm_request.duty_a = 0.0f;
    result->pwm_request.duty_b = 0.0f;
    result->pwm_request.duty_c = 0.0f;
    result->pwm_request.channel1_compare = 0U;
    result->pwm_request.channel2_compare = 0U;
    result->pwm_request.channel3_compare = 0U;
    if (sim != NULL) {
        result->registers = sim->registers;
    } else {
        foc_device_sim_set_safe_registers(&result->registers);
    }

    if (sim == NULL) {
        return FOC_DEVICE_STATUS_ERR_NULL;
    }
    if (input == NULL) {
        result->status = FOC_DEVICE_STATUS_ERR_NULL;
        result->fault_bits = FOC_DEVICE_SIM_FAULT_SAMPLE_INVALID;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    /* Fault priority is evaluated before sample freshness or Core execution. */
    if (sim->state == FOC_DEVICE_SIM_FAULT_LATCHED) {
        result->status = FOC_DEVICE_STATUS_ERR_FAULT_LATCHED;
        result->fault_bits = sim->latched_faults;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    if (sim->state != FOC_DEVICE_SIM_RUNNING) {
        fault_bits = FOC_DEVICE_SIM_FAULT_NOT_RUNNING;
        result->status = FOC_DEVICE_STATUS_ERR_NOT_RUNNING;
        result->fault_bits = fault_bits;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    if (input->sample.sample_seq == 0U || input->sample.sample_tick > input->now_tick) {
        fault_bits = FOC_DEVICE_SIM_FAULT_SAMPLE_INVALID;
        result->status = FOC_DEVICE_STATUS_ERR_INVALID_SAMPLE;
        result->fault_bits = fault_bits;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    if (input->sample.sample_seq <= sim->last_sample_seq ||
        input->now_tick - input->sample.sample_tick > sim->config.max_sample_age_ticks) {
        fault_bits = FOC_DEVICE_SIM_FAULT_SAMPLE_STALE;
        result->status = FOC_DEVICE_STATUS_ERR_STALE_SAMPLE;
        result->fault_bits = fault_bits;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    device_status = foc_device_decode_sample(&input->sample, &sim->config.adc_config, &measurement);
    if (device_status != FOC_DEVICE_STATUS_OK) {
        fault_bits = FOC_DEVICE_SIM_FAULT_SAMPLE_INVALID;
        result->status = device_status;
        result->fault_bits = fault_bits;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    sim->last_sample_seq = input->sample.sample_seq;
    sim->last_sample_tick = input->sample.sample_tick;
    if (!foc_device_sim_float_is_finite(input->timing.dt_s) || input->timing.dt_s <= 0.0f) {
        fault_bits = FOC_DEVICE_SIM_FAULT_TIME_INVALID;
        result->status = FOC_DEVICE_STATUS_ERR_INVALID_TIME;
        result->fault_bits = fault_bits;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    core_status = foc_step(
        &sim->core,
        &input->command,
        &measurement.core,
        &input->timing,
        &result->core_output
    );
    result->core_status = core_status;
    if (core_status != FOC_STATUS_OK) {
        if (core_status == FOC_STATUS_ERR_INVALID_DT) {
            fault_bits = FOC_DEVICE_SIM_FAULT_TIME_INVALID;
            result->status = FOC_DEVICE_STATUS_ERR_INVALID_TIME;
        } else {
            fault_bits = FOC_DEVICE_SIM_FAULT_CORE;
            result->status = FOC_DEVICE_STATUS_ERR_CORE;
        }
        result->fault_bits = fault_bits;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    device_status =
        foc_device_map_output(&result->core_output, &sim->config.pwm_config, &result->pwm_request);
    if (device_status != FOC_DEVICE_STATUS_OK) {
        fault_bits = FOC_DEVICE_SIM_FAULT_CORE;
        result->status = FOC_DEVICE_STATUS_ERR_INVALID_OUTPUT;
        result->fault_bits = fault_bits;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }

    device_status = foc_device_sim_submit(
        &sim->registers,
        &result->pwm_request,
        sim->config.pwm_config.period_counts
    );
    if (device_status != FOC_DEVICE_STATUS_OK) {
        result->fault_bits = FOC_DEVICE_SIM_FAULT_CORE;
        result->status = device_status;
        foc_device_sim_write_inhibit(sim, result);
        return result->status;
    }
    result->status = FOC_DEVICE_STATUS_OK;
    result->fault_bits = FOC_DEVICE_SIM_FAULT_NONE;
    result->registers = sim->registers;
    return result->status;
}

/* ---------------------------------------- end of file ----------------------------------------- */
