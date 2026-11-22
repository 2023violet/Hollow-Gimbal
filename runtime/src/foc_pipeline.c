/**
 * @file    foc_pipeline.c
 * @brief   Host sample-to-current-control-to-Mock transaction.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Implementation role
 * -------------------
 * Apply caller-supplied snapshots in one execution domain and commit the PI
 * candidate only after the complete Device action passes Safety and submission.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_pipeline.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

/* Private Helpers -------------------------------------------------------------- */

static bool foc_pipeline_accept_time(struct foc_pipeline *pipeline, uint32_t now_us)
{
    if (pipeline->time_seen && !foc_safe_timestamp_fresh(now_us, pipeline->now_us, UINT32_MAX)) {
        return false;
    }
    pipeline->now_us = now_us;
    pipeline->time_seen = true;
    return true;
}

static bool foc_pipeline_after_recovery(const struct foc_pipeline *pipeline, uint32_t produced_us)
{
    return foc_safe_sequence_newer(
        produced_us,
        pipeline->rearm_after_us,
        pipeline->rearm_boundary_seen
    );
}

static bool foc_pipeline_input_newer(
    const struct foc_pipeline_watermark *watermark,
    uint32_t seq,
    uint32_t produced_us
)
{
    return foc_safe_sequence_newer(seq, watermark->seq, watermark->seen) &&
        (!watermark->seen ||
         foc_safe_timestamp_fresh(produced_us, watermark->produced_us, UINT32_MAX));
}

static bool
foc_pipeline_submit(struct foc_pipeline *pipeline, const struct foc_device_pwm_request *request)
{
    struct foc_pipeline_mock *mock = &pipeline->mock;
    const bool update = request->action == FOC_DEVICE_OUTPUT_UPDATE;
    const bool forced_rejection = update ? mock->reject_update : mock->reject_inhibit;
    struct foc_device_sim_registers candidate;

    mock->attempts++;
    mock->last_request = *request;
    if (request->action == FOC_DEVICE_OUTPUT_INHIBIT) {
        mock->last_inhibit_attempt = mock->attempts;
    }
    mock->last_accepted = !forced_rejection &&
        foc_device_sim_submit(&candidate, request, pipeline->pwm_config.period_counts) ==
            FOC_DEVICE_STATUS_OK &&
        (pipeline->output_submit == NULL || pipeline->output_submit(pipeline->output_arg, request));
    mock->output_confirmed = mock->last_accepted;
    if (mock->last_accepted) {
        mock->registers = candidate;
    }
    if (update) {
        if (mock->last_accepted) {
            mock->updates++;
            mock->last_update = *request;
        } else {
            mock->last_rejected_update = *request;
        }
    }
    return mock->last_accepted;
}

static void foc_pipeline_require_rearm(struct foc_pipeline *pipeline)
{
    pipeline->rearm_after_us = pipeline->now_us;
    pipeline->rearm_boundary_seen = true;
    if (pipeline->safety.state == FOC_SAFE_STATE_READY ||
        pipeline->safety.state == FOC_SAFE_STATE_RUNNING) {
        (void)foc_safe_disable(&pipeline->safety);
    }
}

static int foc_pipeline_emit_inhibit(struct foc_pipeline *pipeline, enum foc_pipeline_status reason)
{
    struct foc_device_pwm_request request;

    pipeline->last_reason = reason;
    pipeline->safety_reasons |= pipeline->safety.latched_faults;
    pipeline->measured_dq_a = (struct foc_vector_dq){0};
    pipeline->target_dq_a = (struct foc_vector_dq){0};
    pipeline->voltage_dq_v = (struct foc_vector_dq){0};
    /* The mapper always builds canonical INHIBIT, even with invalid mapping settings. */
    (void)foc_device_output_inhibit(&pipeline->pwm_config, &request);
    if (!foc_pipeline_submit(pipeline, &request)) {
        return FOC_PIPELINE_ERR_INHIBIT;
    }
    return reason;
}

static int foc_pipeline_stop(struct foc_pipeline *pipeline, enum foc_pipeline_status reason)
{
    if (pipeline->safety.state == FOC_SAFE_STATE_READY ||
        pipeline->safety.state == FOC_SAFE_STATE_RUNNING) {
        foc_pipeline_require_rearm(pipeline);
    }
    return foc_pipeline_emit_inhibit(pipeline, reason);
}

static bool foc_pipeline_output_inhibited(const struct foc_pipeline *pipeline)
{
    return pipeline->mock.output_confirmed &&
        pipeline->mock.last_request.action == FOC_DEVICE_OUTPUT_INHIBIT;
}

static bool foc_pipeline_ready_inputs(const struct foc_pipeline *pipeline)
{
    const struct foc_safe_context *safety = &pipeline->safety;

    return foc_pipeline_output_inhibited(pipeline) && safety->command_seen && safety->sample_seen &&
        safety->sample_current_valid &&
        foc_pipeline_after_recovery(pipeline, safety->last_command_us) &&
        foc_pipeline_after_recovery(pipeline, safety->last_sample_us) &&
        foc_safe_timestamp_fresh(
               pipeline->now_us,
               safety->last_command_us,
               safety->config.command_timeout_us
        ) &&
        foc_safe_timestamp_fresh(
               pipeline->now_us,
               safety->last_sample_us,
               safety->config.sample_timeout_us
        );
}

static enum foc_pipeline_status foc_pipeline_observe_sample(
    struct foc_pipeline *pipeline,
    const struct foc_pipeline_input *input,
    struct foc_device_measurement *measurement
)
{
    const struct foc_device_adc_raw *raw = &input->adc;

    if (!foc_pipeline_input_newer(&pipeline->sample, raw->sample_seq, raw->sample_tick) ||
        !foc_pipeline_after_recovery(pipeline, raw->sample_tick) ||
        !foc_safe_timestamp_fresh(
            pipeline->now_us,
            raw->sample_tick,
            pipeline->safety.config.sample_timeout_us
        )) {
        return FOC_PIPELINE_ERR_SAMPLE_ORDER;
    }
    pipeline->sample = (struct foc_pipeline_watermark){raw->sample_seq, raw->sample_tick, true};
    const bool valid =
        foc_device_decode_sample(raw, &pipeline->adc_config, measurement) == FOC_DEVICE_STATUS_OK &&
        input->angle_sample_us == raw->sample_tick && isfinite(input->theta_elec_rad);

    if (foc_safe_observe_sample(&pipeline->safety, raw->sample_tick, raw->sample_seq, valid) !=
        FOC_SAFE_STATUS_OK) {
        return FOC_PIPELINE_ERR_SAMPLE;
    }
    return FOC_PIPELINE_OK;
}

static bool foc_pipeline_feedforward_feedback_valid(
    const struct foc_pipeline *pipeline,
    const struct foc_pipeline_input *input
)
{
    const struct foc_pipeline_electrical_feedback *feedback = &input->electrical_feedback;
    return feedback->valid && isfinite(feedback->state.angle_rad) &&
        isfinite(feedback->state.velocity_rad_s) &&
        foc_safe_timestamp_fresh(
               input->adc.sample_tick,
               feedback->source_us,
               pipeline->feedforward_max_age_us
        );
}

/* Public Functions ------------------------------------------------------------- */

int foc_pipeline_init(struct foc_pipeline *pipeline, const struct foc_pipeline_config *config)
{
    struct foc_pipeline trial = {0};

    if (pipeline == NULL || config == NULL) {
        return FOC_PIPELINE_ERR_NULL;
    }
    if (foc_current_init(&trial.current, &config->current) != FOC_STATUS_OK ||
        foc_device_adc_config_validate(&config->adc) != FOC_DEVICE_STATUS_OK ||
        foc_device_pwm_config_validate(&config->pwm) != FOC_DEVICE_STATUS_OK ||
        foc_safe_init(&trial.safety, &config->safety) != FOC_SAFE_STATUS_OK ||
        (config->feedforward.enabled &&
         (config->feedforward.max_age_us == 0U ||
          config->feedforward.max_age_us >= UINT32_C(0x80000000))) ||
        foc_current_feedforward_init(
            &trial.feedforward,
            &config->feedforward.model,
            config->feedforward.enabled
        ) != FOC_STATUS_OK) {
        return FOC_PIPELINE_ERR_CONFIG;
    }
    trial.adc_config = config->adc;
    trial.pwm_config = config->pwm;
    trial.feedforward_max_age_us = config->feedforward.max_age_us;
    trial.output_submit = config->output_submit;
    trial.output_arg = config->output_arg;
    (void)foc_safe_boot_complete(&trial.safety);
    const int status = foc_pipeline_stop(&trial, FOC_PIPELINE_OK);
    if (status != FOC_PIPELINE_OK) {
        return status;
    }
    trial.initialized = true;
    *pipeline = trial;
    return FOC_PIPELINE_OK;
}

int foc_pipeline_set_command(
    struct foc_pipeline *pipeline,
    const struct foc_pipeline_command *command,
    uint32_t now_us
)
{
    if (pipeline == NULL || command == NULL) {
        return FOC_PIPELINE_ERR_NULL;
    }
    if (!pipeline->initialized || pipeline->safety.state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return FOC_PIPELINE_ERR_STATE;
    }
    if (!foc_pipeline_accept_time(pipeline, now_us)) {
        return FOC_PIPELINE_ERR_TIME;
    }
    if (!isfinite(command->target_a.d) || !isfinite(command->target_a.q) ||
        !foc_pipeline_after_recovery(pipeline, command->produced_us) ||
        !foc_pipeline_input_newer(&pipeline->command, command->seq, command->produced_us) ||
        !foc_safe_timestamp_fresh(
            now_us,
            command->produced_us,
            pipeline->safety.config.command_timeout_us
        ) ||
        foc_safe_observe_command(&pipeline->safety, command->produced_us, command->seq) !=
            FOC_SAFE_STATUS_OK) {
        return FOC_PIPELINE_ERR_COMMAND;
    }
    pipeline->target_a = command->target_a;
    pipeline->command = (struct foc_pipeline_watermark){command->seq, command->produced_us, true};
    return FOC_PIPELINE_OK;
}

int foc_pipeline_arm(struct foc_pipeline *pipeline, uint32_t now_us)
{
    if (pipeline == NULL) {
        return FOC_PIPELINE_ERR_NULL;
    }
    if (!pipeline->initialized || pipeline->safety.state != FOC_SAFE_STATE_DISABLED) {
        return FOC_PIPELINE_ERR_STATE;
    }
    if (!foc_pipeline_accept_time(pipeline, now_us)) {
        return FOC_PIPELINE_ERR_TIME;
    }
    if (!foc_pipeline_ready_inputs(pipeline) ||
        foc_safe_arm(&pipeline->safety) != FOC_SAFE_STATUS_OK) {
        return FOC_PIPELINE_ERR_STATE;
    }
    return FOC_PIPELINE_OK;
}

int foc_pipeline_start(struct foc_pipeline *pipeline, uint32_t now_us)
{
    if (pipeline == NULL) {
        return FOC_PIPELINE_ERR_NULL;
    }
    if (!pipeline->initialized || pipeline->safety.state != FOC_SAFE_STATE_READY) {
        return FOC_PIPELINE_ERR_STATE;
    }
    if (!foc_pipeline_accept_time(pipeline, now_us)) {
        return FOC_PIPELINE_ERR_TIME;
    }
    if (!foc_pipeline_ready_inputs(pipeline) ||
        foc_safe_watchdog_permitted(&pipeline->safety, now_us) != FOC_SAFE_STATUS_OK) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_STATE);
    }
    struct foc_current_context trial = pipeline->current;
    if (foc_current_reset(&trial) != FOC_STATUS_OK ||
        foc_safe_start(&pipeline->safety) != FOC_SAFE_STATUS_OK) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_STATE);
    }
    pipeline->current = trial;
    pipeline->rearm_boundary_seen = false;
    return FOC_PIPELINE_OK;
}

int foc_pipeline_request_inhibit(
    struct foc_pipeline *pipeline,
    uint32_t now_us,
    uint32_t fault_bits
)
{
    if (pipeline == NULL) {
        return FOC_PIPELINE_ERR_NULL;
    }
    if (!pipeline->initialized) {
        return FOC_PIPELINE_ERR_STATE;
    }
    const bool time_valid = foc_pipeline_accept_time(pipeline, now_us);

    /* An invalid timestamp must never suppress an explicit stop. */
    foc_pipeline_require_rearm(pipeline);
    if (fault_bits != FOC_SAFE_FAULT_NONE) {
        (void)foc_safe_latch_fault(&pipeline->safety, fault_bits);
    }
    pipeline->safety_reasons = pipeline->safety.latched_faults;
    return foc_pipeline_stop(pipeline, time_valid ? FOC_PIPELINE_OK : FOC_PIPELINE_ERR_TIME);
}

int foc_pipeline_clear_fault(struct foc_pipeline *pipeline, uint32_t now_us)
{
    if (pipeline == NULL) {
        return FOC_PIPELINE_ERR_NULL;
    }
    if (!pipeline->initialized || pipeline->safety.state != FOC_SAFE_STATE_FAULT_LATCHED ||
        !foc_pipeline_output_inhibited(pipeline)) {
        return FOC_PIPELINE_ERR_STATE;
    }
    if (!foc_pipeline_accept_time(pipeline, now_us)) {
        return FOC_PIPELINE_ERR_TIME;
    }
    if (foc_safe_clear_fault(&pipeline->safety) != FOC_SAFE_STATUS_OK) {
        return FOC_PIPELINE_ERR_STATE;
    }
    foc_pipeline_require_rearm(pipeline);
    pipeline->safety_reasons = FOC_SAFE_FAULT_NONE;
    return FOC_PIPELINE_OK;
}

int foc_pipeline_step(struct foc_pipeline *pipeline, const struct foc_pipeline_input *input)
{
    return foc_pipeline_step_guarded(pipeline, input, NULL, NULL);
}

int foc_pipeline_step_guarded(
    struct foc_pipeline *pipeline,
    const struct foc_pipeline_input *input,
    foc_pipeline_update_guard_t guard,
    void *argument
)
{
    if (pipeline == NULL) {
        return FOC_PIPELINE_ERR_NULL;
    }
    if (!pipeline->initialized) {
        return FOC_PIPELINE_ERR_STATE;
    }
    pipeline->safety_reasons = FOC_SAFE_FAULT_NONE;
    if (input == NULL) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_NULL);
    }
    if (!foc_pipeline_accept_time(pipeline, input->now_us)) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_TIME);
    }
    if (pipeline->safety.state == FOC_SAFE_STATE_FAULT_LATCHED) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_GATE);
    }

    struct foc_device_measurement measurement;
    const enum foc_pipeline_status sample_status =
        foc_pipeline_observe_sample(pipeline, input, &measurement);
    const int supervisor_status = pipeline->safety.state == FOC_SAFE_STATE_RUNNING
        ? foc_safe_supervisor_tick(&pipeline->safety, pipeline->now_us)
        : FOC_SAFE_STATUS_OK;
    if (sample_status != FOC_PIPELINE_OK) {
        return foc_pipeline_stop(pipeline, sample_status);
    }
    if (supervisor_status != FOC_SAFE_STATUS_OK) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_GATE);
    }
    if (pipeline->safety.state != FOC_SAFE_STATE_RUNNING) {
        const int idle_status = foc_pipeline_emit_inhibit(pipeline, FOC_PIPELINE_ERR_STATE);
        if (idle_status == FOC_PIPELINE_ERR_INHIBIT) {
            foc_pipeline_require_rearm(pipeline);
        }
        return idle_status;
    }

    struct foc_vector_dq target = pipeline->target_a;
    if (input->require_cycle_target || input->cycle_target != NULL) {
        const struct foc_pipeline_command *derived = input->cycle_target;
        if (derived == NULL || !pipeline->safety.command_seen ||
            derived->seq != pipeline->safety.command_seq ||
            derived->produced_us != pipeline->safety.last_command_us ||
            !isfinite(derived->target_a.d) || !isfinite(derived->target_a.q)) {
            return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_COMMAND);
        }
        target = derived->target_a;
    }
    struct foc_current_context trial = pipeline->current;
    struct foc_current_output result;
    if (input->voltage_drive) {
        /* Calibration drive: no PI state is used or kept. */
        struct foc_vector_alpha_beta measured_ab;
        struct foc_vector_alpha_beta voltage_ab;
        const float limit = trial.config.current_limit_a;
        result = (struct foc_current_output){.output_enable = 1U, .voltage_dq_v = target};
        if (foc_current_reset(&trial) != FOC_STATUS_OK ||
            foc_clarke(&measurement.phase_current_a, &measured_ab) != FOC_STATUS_OK ||
            foc_park(&measured_ab, input->theta_elec_rad, &result.measured_dq_a) != FOC_STATUS_OK ||
            !(result.measured_dq_a.d * result.measured_dq_a.d +
                  result.measured_dq_a.q * result.measured_dq_a.q <=
              limit * limit) ||
            foc_inverse_park(&target, input->theta_elec_rad, &voltage_ab) != FOC_STATUS_OK ||
            foc_svpwm(&voltage_ab, measurement.core.vbus_v, &trial.config.pwm, &result.svpwm) !=
                FOC_STATUS_OK) {
            return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_CORE);
        }
    } else {
        const struct foc_current_input current_input = {
            .phase_current_a = measurement.phase_current_a,
            .theta_elec_rad = input->theta_elec_rad,
            .target_a = target,
            .vbus_v = measurement.core.vbus_v,
            .dt_s = input->dt_s,
        };
        if (pipeline->feedforward.enabled &&
            !foc_pipeline_feedforward_feedback_valid(pipeline, input)) {
            return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_FEEDBACK);
        }
        const int current_status = pipeline->feedforward.enabled
            ? foc_current_step_with_feedforward(
                  &trial,
                  &current_input,
                  &pipeline->feedforward,
                  input->electrical_feedback.state.velocity_rad_s,
                  &result
              )
            : foc_current_step(&trial, &current_input, &result);
        if (current_status != FOC_STATUS_OK || result.output_enable == 0U) {
            return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_CORE);
        }
    }
    const struct foc_output core_output = {
        .output_enable = result.output_enable,
        .svpwm = result.svpwm
    };
    struct foc_device_pwm_request request;
    if (foc_device_map_output(&core_output, &pipeline->pwm_config, &request) !=
        FOC_DEVICE_STATUS_OK) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_MAP);
    }

    const struct foc_pipeline_gate_injection *event = &input->before_gate;
    const bool time_valid = foc_pipeline_accept_time(pipeline, input->now_us + event->delay_us);
    if (event->inhibit) {
        foc_pipeline_require_rearm(pipeline);
    }
    if (event->fault_bits != FOC_SAFE_FAULT_NONE) {
        (void)foc_safe_latch_fault(&pipeline->safety, event->fault_bits);
    }
    if (!time_valid) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_TIME);
    }
    struct foc_safe_output_decision decision;
    if (foc_safe_gate_output(
            &pipeline->safety,
            pipeline->now_us,
            &pipeline->pwm_config,
            &request,
            &decision
        ) != FOC_SAFE_STATUS_OK) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_GATE);
    }
    pipeline->safety_reasons = decision.inhibit_reasons;
    if (!decision.permitted) {
        return foc_pipeline_stop(
            pipeline,
            event->inhibit ? FOC_PIPELINE_ERR_STOP : FOC_PIPELINE_ERR_GATE
        );
    }
    if (guard != NULL && !guard(argument)) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_STOP);
    }
    if (!foc_pipeline_submit(pipeline, &decision.request)) {
        return foc_pipeline_stop(pipeline, FOC_PIPELINE_ERR_UPDATE);
    }

    /* From here only infallible assignments remain, under the same serial owner. */
    pipeline->current = trial;
    pipeline->measured_dq_a = result.measured_dq_a;
    pipeline->target_dq_a = target;
    pipeline->voltage_dq_v = result.voltage_dq_v;
    pipeline->last_reason = FOC_PIPELINE_OK;
    return FOC_PIPELINE_OK;
}

/* ---------------------------------------- end of file ----------------------------------------- */
