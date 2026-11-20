/**
 * @file    foc_board_pwm_model.c
 * @brief   Host-only C1/C2/C3 register and qualified-load event model.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * Run the Board1 map and timing rules against explicit transaction witnesses.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_board_pwm_model.h"

#include <string.h>

/* Private Helpers -------------------------------------------------------------- */
static bool newer(uint32_t value, uint32_t previous)
{
    const uint32_t delta = value - previous;
    return delta != 0U && delta < UINT32_C(0x80000000);
}

static bool same_id(const struct foc_board_pwm_id *a, const struct foc_board_pwm_id *b)
{
    return a->session == b->session && a->submission == b->submission && a->slot == b->slot &&
        a->generation == b->generation && a->command == b->command;
}

static bool
equal_or_later(struct foc_board_carrier_stamp value, struct foc_board_carrier_stamp previous)
{
    return value.cycle == previous.cycle ? value.phase_ns >= previous.phase_ns
                                         : newer(value.cycle, previous.cycle);
}

static enum foc_board_pwm_stage reject(struct foc_board_pwm_model *port)
{
    foc_board_pwm_inhibit(port);
    return FOC_BOARD_PWM_INHIBITED;
}

static bool mapped_request(
    const struct foc_device_pwm_config *config,
    const struct foc_device_pwm_request *request
)
{
    if (request->action != FOC_DEVICE_OUTPUT_UPDATE || request->output_enable_request != 1U ||
        config->period_counts > UINT16_MAX) {
        return false;
    }
    const struct foc_output core = {
        .output_enable = 1U,
        .svpwm = {.duty_a = request->duty_a, .duty_b = request->duty_b, .duty_c = request->duty_c},
    };
    struct foc_device_pwm_request mapped;
    return foc_device_map_output(&core, config, &mapped) == FOC_DEVICE_STATUS_OK &&
        request->channel1_compare == mapped.channel1_compare &&
        request->channel2_compare == mapped.channel2_compare &&
        request->channel3_compare == mapped.channel3_compare;
}

/* Public Functions ------------------------------------------------------------- */
bool foc_board_pwm_rearm(struct foc_board_pwm_model *port, uint32_t session, bool recovery_allowed)
{
    if (!recovery_allowed || port->session_open || session == 0U ||
        (port->latest_session != 0U && !newer(session, port->latest_session))) {
        return false;
    }
    port->latest_session = session;
    port->session_open = true;
    port->identity_seen = false;
    return true;
}

enum foc_board_pwm_stage foc_board_pwm_accept(
    struct foc_board_pwm_model *port,
    const struct foc_board_pwm_id *id,
    const struct foc_board_adc_transaction *token,
    const struct foc_board_load_plan *plan,
    const struct foc_board_shadow_attempt *timing,
    const struct foc_device_pwm_config *config,
    const struct foc_device_pwm_request *request,
    bool guard_allowed
)
{
    if (!guard_allowed || !port->session_open || id->session != port->latest_session ||
        port->stage == FOC_BOARD_PWM_UPDATE_ACCEPTED ||
        port->stage == FOC_BOARD_PWM_SHADOW_WRITTEN || token->state != FOC_BOARD_ADC_COMPLETED ||
        !token->adc_valid || token->physical_output_applied || token->slot_id != id->slot ||
        token->generation != id->generation || timing->sample_cycle != id->slot ||
        timing->generation != id->generation || plan->load.cycle - id->slot > 1U ||
        (port->identity_seen &&
         (!newer(id->submission, port->latest_id.submission) ||
          !newer(id->slot, port->latest_id.slot) ||
          !newer(id->generation, port->latest_id.generation) ||
          !equal_or_later(plan->load, port->plan.load) ||
          (plan->load.cycle == port->plan.load.cycle &&
           plan->load.phase_ns == port->plan.load.phase_ns))) ||
        !mapped_request(config, request) ||
        foc_board_shadow_check_timing(plan, timing) != FOC_BOARD_SHADOW_STAGED) {
        return reject(port);
    }
    port->latest_id = *id;
    port->identity_seen = true;
    port->plan = *plan;
    port->timing = *timing;
    port->requested[0] = request->channel1_compare;
    port->requested[1] = request->channel2_compare;
    port->requested[2] = request->channel3_compare;
    port->written = 0U;
    memset(port->shadow, 0, sizeof port->shadow);
    port->stage = FOC_BOARD_PWM_UPDATE_ACCEPTED;
    return port->stage;
}

enum foc_board_pwm_stage foc_board_pwm_write(
    struct foc_board_pwm_model *port,
    const struct foc_board_pwm_id *id,
    unsigned int channel,
    uint32_t compare,
    struct foc_board_carrier_stamp observed
)
{
    if (!port->session_open || port->stage != FOC_BOARD_PWM_UPDATE_ACCEPTED ||
        !same_id(id, &port->latest_id) || channel < 1U || channel > 3U ||
        channel != port->written + 1U || compare != port->requested[channel - 1U] ||
        (port->written != 0U && !equal_or_later(observed, port->timing.shadow_write))) {
        return reject(port);
    }
    port->timing.shadow_write = observed;
    if (foc_board_shadow_check_timing(&port->plan, &port->timing) != FOC_BOARD_SHADOW_STAGED) {
        return reject(port);
    }
    port->shadow[port->written++] = compare;
    if (port->written == 3U) {
        port->stage = FOC_BOARD_PWM_SHADOW_WRITTEN;
    }
    return port->stage;
}

enum foc_board_pwm_stage foc_board_pwm_load(
    struct foc_board_pwm_model *port,
    const struct foc_board_pwm_id *id,
    struct foc_board_carrier_stamp observed,
    bool timer_witness_valid,
    bool guard_allowed
)
{
    if (!port->session_open || !timer_witness_valid || !guard_allowed ||
        port->stage != FOC_BOARD_PWM_SHADOW_WRITTEN || !same_id(id, &port->latest_id) ||
        observed.cycle != port->plan.load.cycle || observed.phase_ns != port->plan.load.phase_ns) {
        return reject(port);
    }
    memcpy(port->compare, port->shadow, sizeof port->compare);
    port->active_id = port->latest_id;
    port->active = true;
    port->stage = FOC_BOARD_PWM_MODEL_ACTIVE;
    return port->stage;
}

void foc_board_pwm_inhibit(struct foc_board_pwm_model *port)
{
    const uint32_t session = port->latest_session;
    *port = (struct foc_board_pwm_model){.latest_session = session};
}

/* ---------------------------------------- end of file ----------------------------------------- */
