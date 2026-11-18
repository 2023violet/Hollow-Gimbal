/**
 * @file    foc_virtual_current.c
 * @brief   Deterministic Host Runtime harness for a virtual PMSM current loop.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Implementation role
 * -------------------
 * Keep the virtual clock, accepted-output journal, sensor publication, and
 * Runtime event ordering together without changing the production Runtime.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_virtual_current.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Private Constants ------------------------------------------------------------ */

static const double FOC_VIRTUAL_SQRT3 = 1.73205080756887729353;
static const uint32_t FOC_VIRTUAL_H0_US = 200U;
static const uint32_t FOC_VIRTUAL_H1_US = 500U;
static const uint32_t FOC_VIRTUAL_FOREGROUND_US = 200U;

/* Private Helpers -------------------------------------------------------------- */

static uint32_t foc_virtual_hash_bytes(uint32_t hash, const void *data, size_t size)
{
    const unsigned char *bytes = data;
    for (size_t index = 0U; index < size; ++index) {
        hash ^= bytes[index];
        hash *= UINT32_C(16777619);
    }
    return hash;
}

static void foc_virtual_update_hash(struct foc_virtual_current *context)
{
    struct foc_runtime_snapshot snapshot;
    if (foc_runtime_read_snapshot(&context->runtime, &snapshot) != FOC_RUNTIME_OK) {
        return;
    }
    context->replay_hash = foc_virtual_hash_bytes(context->replay_hash, &snapshot, sizeof snapshot);
    context->replay_hash = foc_virtual_hash_bytes(
        context->replay_hash,
        &context->motor.state,
        sizeof context->motor.state
    );
}

static bool foc_virtual_output_submit(void *argument, const struct foc_device_pwm_request *request)
{
    struct foc_virtual_current *context = argument;
    if (context == NULL || request == NULL) {
        return false;
    }

    struct foc_device_sim_registers registers = context->output.registers;
    if (request->action == FOC_DEVICE_OUTPUT_UPDATE) {
        if (context->output.reject_next_update) {
            context->output.reject_next_update = false;
            context->output.rejected_updates++;
            return false;
        }
        if (foc_device_sim_submit(&registers, request, context->profile.pwm.period_counts) !=
            FOC_DEVICE_STATUS_OK) {
            context->output.rejected_updates++;
            return false;
        }
        context->output.request = *request;
        context->output.registers = registers;
        context->output.mode = FOC_VIRTUAL_OUTPUT_UPDATE;
        context->output.v_alpha_v = (double)context->profile.vbus_v *
            (2.0 * (double)request->duty_a - (double)request->duty_b - (double)request->duty_c) /
            3.0;
        context->output.v_beta_v = (double)context->profile.vbus_v *
            ((double)request->duty_b - (double)request->duty_c) / FOC_VIRTUAL_SQRT3;
        if (hypot(context->output.v_alpha_v, context->output.v_beta_v) >=
            (double)context->profile.current.pwm.voltage_limit_v - 1.0e-3) {
            context->output.saturated_updates++;
        }
        context->output.accepted_updates++;
        context->fault_events.last_update_seen = true;
        context->fault_events.last_accepted_update_us = context->virtual_time_us;
        return true;
    }

    if (request->action == FOC_DEVICE_OUTPUT_INHIBIT &&
        foc_device_sim_submit(&registers, request, context->profile.pwm.period_counts) ==
            FOC_DEVICE_STATUS_OK) {
        context->output.request = *request;
        context->output.registers = registers;
        context->output.mode = FOC_VIRTUAL_OUTPUT_HIGH_Z;
        context->output.v_alpha_v = 0.0;
        context->output.v_beta_v = 0.0;
        if (context->fault_events.injection_seen && !context->fault_events.inhibit_accepted_seen) {
            context->fault_events.inhibit_accepted_seen = true;
            context->fault_events.inhibit_accepted_us = context->virtual_time_us;
            context->fault_events.updates_at_inhibit_accepted = context->output.accepted_updates;
        }
        return true;
    }

    return false;
}

static int foc_virtual_integrate_to(struct foc_virtual_current *context, uint32_t timestamp_us)
{
    if (timestamp_us < context->virtual_time_us) {
        return -1;
    }
    const uint32_t elapsed_us = timestamp_us - context->virtual_time_us;
    if (elapsed_us != 0U) {
        const double dt_s = (double)elapsed_us * 1.0e-6;
        const int status = context->output.mode == FOC_VIRTUAL_OUTPUT_UPDATE
            ? foc_virtual_motor_apply_duty(
                  &context->motor,
                  &context->output.request,
                  context->profile.vbus_v,
                  dt_s
              )
            : foc_virtual_motor_step(&context->motor, 0.0, 0.0, dt_s);
        if (status != 0) {
            return status;
        }
    }
    context->virtual_time_us = timestamp_us;
    return 0;
}

static int foc_virtual_encoder_code(const struct foc_virtual_current *context, uint16_t *code)
{
    const double fundamental = context->profile.encoder_fundamental_error_rad;
    const double third = context->profile.encoder_third_error_rad;
    const struct foc_encoder_config *encoder = &context->profile.encoder;
    if (fundamental == 0.0 && third == 0.0) {
        return foc_virtual_motor_make_encoder_code(&context->motor, encoder, code);
    }
    /* Bias the simulated transducer before its existing quantizer. The real
     * Plant angle remains available for ADC synthesis and the Host reference. */
    struct foc_virtual_motor observation = context->motor;
    const double direction = encoder->direction;
    const double phase = direction * observation.state.mechanical_position_rad;
    const double error = fundamental * sin(phase + 0.41) + third * sin(3.0 * phase - 0.23);
    observation.state.mechanical_position_rad += direction * error;
    return foc_virtual_motor_make_encoder_code(&observation, encoder, code);
}

static int foc_virtual_publish_sensor(struct foc_virtual_current *context)
{
    uint16_t raw_code = 0U;
    if (foc_virtual_encoder_code(context, &raw_code) != 0) {
        return -1;
    }
    struct foc_encoder_feedback feedback;
    const enum foc_encoder_transport transport =
        context->fail_next_sensor ? FOC_ENCODER_TRANSPORT_FRAME_ERROR : FOC_ENCODER_TRANSPORT_OK;
    context->fail_next_sensor = false;
    if (transport == FOC_ENCODER_TRANSPORT_FRAME_ERROR && context->fault_events.injection_seen &&
        !context->fault_events.sensor_failure_seen) {
        context->fault_events.sensor_failure_seen = true;
        context->fault_events.sensor_failure_us = context->virtual_time_us;
        context->fault_events.updates_at_sensor_failure = context->output.accepted_updates;
    }
    const int encoder_status = foc_encoder_update(
        &context->encoder,
        context->virtual_time_us,
        raw_code,
        transport,
        &feedback
    );
    const bool position_valid =
        encoder_status == FOC_ENCODER_STATUS_OK && isfinite(feedback.mechanical_rad);
    const float velocity = feedback.speed_valid ? feedback.filtered_velocity_rad_s : 0.0f;
    float phase = 0.0f;
    bool phase_valid = false;
    if (context->runtime.config.motion.enabled) {
        phase_valid = position_valid && feedback.captured_us == context->virtual_time_us &&
            foc_rotor_phase_from_counts(
                &context->runtime.config.motion.rotor,
                feedback.mechanical_phase_count,
                context->encoder.config.counts_per_revolution,
                &phase
            ) == FOC_STATUS_OK;
    } else {
        /* Preserve the direct-angle producer; disabled motion has no validated rotor config. */
        phase =
            (float)((double)context->profile.motor.pole_pairs * (double)feedback.mechanical_rad +
                    context->profile.motor.electrical_offset_rad);
    }
    const struct foc_runtime_sensor sample = {
        .theta_elec_rad = phase,
        .captured_us = context->virtual_time_us,
        .seq = ++context->encoder_seq,
        /* Current mode needs an angle on the first frame; speed becomes valid
         * after foc_encoder_update() has accepted its second frame. */
        .valid = position_valid,
        .mechanical_rad = feedback.mechanical_rad,
        .mechanical_rad_s = velocity,
        .electrical_phase_valid = phase_valid,
        .mechanical_frame_id = position_valid && feedback.speed_valid
            ? context->profile.runtime.motion.impedance.mechanical_frame_id
            : 0U,
    };
    const int status =
        foc_runtime_h1_step(&context->runtime, &sample, context->virtual_time_us, 0U);
    if (transport == FOC_ENCODER_TRANSPORT_FRAME_ERROR && context->fault_events.injection_seen &&
        !context->fault_events.h1_invalid_seen) {
        context->fault_events.h1_invalid_seen = true;
        context->fault_events.h1_invalid_us = context->virtual_time_us;
        context->fault_events.updates_at_h1_invalid = context->output.accepted_updates;
    }
    (void)status;
    return 0;
}

static int foc_virtual_run_h0(struct foc_virtual_current *context)
{
    struct foc_device_adc_raw raw;
    if (foc_virtual_motor_make_adc(
            &context->motor,
            &context->profile.adc,
            context->profile.vbus_v,
            ++context->sample_seq,
            context->virtual_time_us,
            &raw
        ) != 0) {
        return -1;
    }
    const struct foc_runtime_fast_input input = {
        .adc = raw,
        .now_us = context->virtual_time_us,
        .cost_us = 0U,
        .action = context->pending_action,
        .requested_mode = context->pending_mode,
    };
    context->pending_action = FOC_RUNTIME_ACTION_NONE;
    const int status = foc_runtime_h0_step(&context->runtime, &input);
    struct foc_runtime_snapshot snapshot;
    if (foc_runtime_read_snapshot(&context->runtime, &snapshot) == FOC_RUNTIME_OK &&
        context->fault_events.injection_seen) {
        if (context->fault_events.h1_invalid_seen && !context->fault_events.h0_failure_seen &&
            (status != FOC_RUNTIME_OK || snapshot.state != FOC_SAFE_STATE_RUNNING)) {
            context->fault_events.h0_failure_seen = true;
            context->fault_events.h0_failure_us = context->virtual_time_us;
            context->fault_events.updates_at_h0_failure = context->output.accepted_updates;
        }
        if (!context->fault_events.inhibit_requested_seen &&
            snapshot.state != FOC_SAFE_STATE_RUNNING) {
            context->fault_events.inhibit_requested_seen = true;
            context->fault_events.inhibit_requested_us = context->virtual_time_us;
            context->fault_events.updates_at_inhibit_requested = context->output.accepted_updates;
        }
        if (snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED &&
            !context->fault_events.fault_latched_seen) {
            context->fault_events.fault_latched_seen = true;
            context->fault_events.fault_latched_us = context->virtual_time_us;
            context->fault_events.updates_at_fault_latched = context->output.accepted_updates;
        }
    }
    foc_virtual_update_hash(context);
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

struct foc_virtual_current_profile foc_virtual_current_default_profile(void)
{
    return (struct foc_virtual_current_profile){
        .motor =
            {
                .rs_ohm = 0.6,
                .ld_h = 0.0012,
                .lq_h = 0.0015,
                .psi_f_wb = 0.025,
                .pole_pairs = 4U,
                .electrical_offset_rad = 0.37,
                .inertia_kg_m2 = 0.002,
                .damping_nm_s = 0.006,
            },
        .load_torque_nm = 0.0,
        .vbus_v = 12.0f,
        .adc =
            {
                .vbus_v_per_count = 0.01f,
                .current_a_per_count = 0.002f,
                .vbus_min_v = 1.0f,
                .phase_a_offset_count = 2048.0f,
                .phase_b_offset_count = 2048.0f,
                .phase_c_offset_count = 2048.0f,
                .phase_order = FOC_DEVICE_PHASE_ORDER_ABC,
            },
        .pwm = {6000U, FOC_DEVICE_PHASE_ORDER_ABC},
        .encoder =
            {
                .counts_per_revolution = 65536U,
                .direction = FOC_ENCODER_DIRECTION_NORMAL,
                .max_speed_rad_s = 1000.0f,
                .min_dt_us = 1U,
                .max_dt_us = 2000U,
                .speed_lpf_cutoff_hz = 80.0f,
                .tracker_alpha = 0.85f,
                .tracker_beta = 0.2f,
            },
        .current =
            {
                .pwm = {6.0f, 2.4f, 0.0f, 1.0f, 0.0006f},
                .kp_v_per_a = {1.08f, 1.35f},
                .ki_v_per_a_s = {540.0f, 540.0f},
                .current_limit_a = 2.5f,
            },
        .safety = {2500U, 500U, 700U, 1000U, 3U},
        .runtime = {
            .h0 = {FOC_VIRTUAL_H0_US, 150U, 100U},
            .h1 = {FOC_VIRTUAL_H1_US, 150U, 150U},
            .supervision = {FOC_VIRTUAL_FOREGROUND_US, 150U, 100U},
            .telemetry = {10000U, 500U, 100U},
            .foreground_budget_us = 150U,
            .angle_timeout_us = 600U,
            .motion = {
                .enabled = true,
                .rotor = {.pole_pairs = 4U, .electrical_offset_rad = 0.37f},
                .prediction =
                    {.max_prediction_us = 400U,
                     .max_accel_rad_s2 = 2000.0f,
                     .max_error_rad = 0.002f},
                .velocity = {.kp_a_per_rad_s = 0.18f, .ki_a_per_rad = 1.4f, .max_dt_s = 0.002f},
                .position = {.kp_per_s = 4.0f, .velocity_limit_rad_s = 12.0f},
                .iq_limit_a = 2.5f,
                .trajectory = {.enabled = false},
            },
        },
    };
}

int foc_virtual_current_init(
    struct foc_virtual_current *context,
    const struct foc_virtual_current_profile *profile
)
{
    if (context == NULL || profile == NULL) {
        return -1;
    }
    /* Keep the continuous raw-angle map strictly increasing. */
    const double error_slope =
        fabs(profile->encoder_fundamental_error_rad) + 3.0 * fabs(profile->encoder_third_error_rad);
    if (!isfinite(error_slope) || error_slope >= 1.0) {
        return -1;
    }
    *context = (struct foc_virtual_current){0};
    context->profile = *profile;
    if (foc_virtual_motor_init(&context->motor, &profile->motor, NULL) != 0 ||
        foc_virtual_motor_set_load(&context->motor, profile->load_torque_nm) != 0 ||
        foc_encoder_init(&context->encoder, &profile->encoder) != FOC_ENCODER_STATUS_OK) {
        return -1;
    }
    context->output.mode = FOC_VIRTUAL_OUTPUT_HIGH_Z;
    context->pending_mode = FOC_RUNTIME_MODE_CURRENT;
    context->replay_hash = UINT32_C(2166136261);
    const struct foc_pipeline_config pipeline = {
        .current = profile->current,
        .adc = profile->adc,
        .pwm = profile->pwm,
        .safety = profile->safety,
        .output_submit = foc_virtual_output_submit,
        .output_arg = context,
    };
    if (foc_runtime_init(&context->runtime, &pipeline, &profile->runtime, 0U) != FOC_RUNTIME_OK) {
        return -1;
    }
    context->initialized = true;
    return 0;
}

int foc_virtual_current_publish_iq(struct foc_virtual_current *context, float id_a, float iq_a)
{
    if (context == NULL || !context->initialized || !isfinite(id_a) || !isfinite(iq_a)) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const struct foc_pipeline_command command = {
        .target_a = {id_a, iq_a},
        .seq = ++context->command_seq,
        .produced_us = context->virtual_time_us,
    };
    const int status =
        foc_runtime_publish_command(&context->runtime, &command, context->virtual_time_us);
    if (status == FOC_RUNTIME_OK) {
        context->target_a = command.target_a;
    }
    return status;
}

int foc_virtual_current_publish_motion(
    struct foc_virtual_current *context,
    enum foc_runtime_mode mode,
    float motion_ref
)
{
    if (context == NULL || !context->initialized ||
        (mode != FOC_RUNTIME_MODE_VELOCITY && mode != FOC_RUNTIME_MODE_POSITION) ||
        !isfinite(motion_ref)) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    const struct foc_runtime_command command = {
        .mode = mode,
        .seq = ++context->command_seq,
        .produced_us = context->virtual_time_us,
        .target.motion_ref = motion_ref,
    };
    const int status =
        foc_runtime_publish_motion_command(&context->runtime, &command, context->virtual_time_us);
    if (status == FOC_RUNTIME_OK) {
        context->motion_ref = motion_ref;
    }
    return status;
}

int foc_virtual_current_publish_lifecycle(
    struct foc_virtual_current *context,
    enum foc_runtime_action action,
    enum foc_runtime_mode requested_mode
)
{
    if (context == NULL || !context->initialized ||
        (action != FOC_RUNTIME_ACTION_ARM && action != FOC_RUNTIME_ACTION_START &&
         action != FOC_RUNTIME_ACTION_SET_MODE)) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    struct foc_runtime_snapshot snapshot;
    if (foc_runtime_read_snapshot(&context->runtime, &snapshot) != FOC_RUNTIME_OK) {
        return FOC_RUNTIME_ERR_BUSY;
    }
    const struct foc_runtime_lifecycle_request request = {
        .action = action,
        .requested_mode = requested_mode,
        .request_sequence = ++context->lifecycle_sequence,
        .source_session = snapshot.motion_session,
        .produced_us = context->virtual_time_us,
    };
    return foc_runtime_publish_lifecycle_request(
        &context->runtime,
        &request,
        context->virtual_time_us
    );
}

int foc_virtual_current_read_lifecycle(
    const struct foc_virtual_current *context,
    struct foc_runtime_lifecycle_result *result
)
{
    if (context == NULL || !context->initialized || result == NULL) {
        return FOC_RUNTIME_ERR_INPUT;
    }
    return foc_runtime_read_lifecycle_result((struct foc_runtime *)&context->runtime, result);
}

int foc_virtual_current_queue_action(
    struct foc_virtual_current *context,
    enum foc_runtime_action action
)
{
    if (context == NULL || !context->initialized ||
        (action != FOC_RUNTIME_ACTION_ARM && action != FOC_RUNTIME_ACTION_START &&
         action != FOC_RUNTIME_ACTION_CLEAR_FAULT)) {
        return -1;
    }
    if (context->pending_action != FOC_RUNTIME_ACTION_NONE) {
        return -1;
    }
    context->pending_action = action;
    return 0;
}

int foc_virtual_current_queue_set_mode(
    struct foc_virtual_current *context,
    enum foc_runtime_mode mode
)
{
    if (context == NULL || !context->initialized ||
        (mode != FOC_RUNTIME_MODE_CURRENT && mode != FOC_RUNTIME_MODE_VELOCITY &&
         mode != FOC_RUNTIME_MODE_POSITION) ||
        context->pending_action != FOC_RUNTIME_ACTION_NONE) {
        return -1;
    }
    context->pending_action = FOC_RUNTIME_ACTION_SET_MODE;
    context->pending_mode = mode;
    return 0;
}

int foc_virtual_current_reject_next_update(struct foc_virtual_current *context)
{
    if (context == NULL || !context->initialized) {
        return -1;
    }
    context->output.reject_next_update = true;
    return 0;
}

int foc_virtual_current_fail_next_sensor(struct foc_virtual_current *context)
{
    if (context == NULL || !context->initialized) {
        return -1;
    }
    context->fail_next_sensor = true;
    return 0;
}

int foc_virtual_current_mark_sensor_fault(
    struct foc_virtual_current *context,
    uint32_t injection_us
)
{
    if (context == NULL || !context->initialized) {
        return -1;
    }
    context->fault_events.injection_seen = true;
    context->fault_events.injection_us = injection_us;
    context->fault_events.updates_at_injection = context->output.accepted_updates;
    return 0;
}

int foc_virtual_current_read_fault_events(
    const struct foc_virtual_current *context,
    struct foc_virtual_fault_events *events
)
{
    if (context == NULL || !context->initialized || events == NULL) {
        return -1;
    }
    *events = context->fault_events;
    return 0;
}

int foc_virtual_current_run_until(struct foc_virtual_current *context, uint32_t target_us)
{
    if (context == NULL || !context->initialized || target_us < context->virtual_time_us) {
        return -1;
    }
    while (context->virtual_time_us < target_us || context->next_h0_us <= target_us ||
           context->next_h1_us <= target_us || context->next_foreground_us <= target_us) {
        uint32_t next = target_us;
        if (context->next_h0_us < next) {
            next = context->next_h0_us;
        }
        if (context->next_h1_us < next) {
            next = context->next_h1_us;
        }
        if (context->next_foreground_us < next) {
            next = context->next_foreground_us;
        }
        if (foc_virtual_integrate_to(context, next) != 0) {
            return -1;
        }
        const bool h1_due = context->next_h1_us == next;
        const bool foreground_due = context->next_foreground_us == next;
        const bool h0_due = context->next_h0_us == next;
        if (h1_due) {
            if (foc_virtual_publish_sensor(context) != 0) {
                return -1;
            }
            context->next_h1_us += FOC_VIRTUAL_H1_US;
        }
        if (foreground_due) {
            struct foc_runtime_foreground_result result;
            (void)foc_runtime_foreground_poll(
                &context->runtime,
                context->virtual_time_us,
                0U,
                0U,
                &result
            );
            context->next_foreground_us += FOC_VIRTUAL_FOREGROUND_US;
        }
        if (h0_due) {
            if (foc_virtual_run_h0(context) != 0) {
                return -1;
            }
            context->next_h0_us += FOC_VIRTUAL_H0_US;
        }
        if (next == target_us && !h1_due && !foreground_due && !h0_due) {
            break;
        }
    }
    return foc_virtual_integrate_to(context, target_us);
}

int foc_virtual_current_request_stop(struct foc_virtual_current *context)
{
    if (context == NULL || !context->initialized) {
        return -1;
    }
    foc_runtime_revoke_command(&context->runtime);
    return 0;
}

int foc_virtual_current_snapshot(
    const struct foc_virtual_current *context,
    struct foc_runtime_snapshot *snapshot
)
{
    if (context == NULL || !context->initialized || snapshot == NULL) {
        return -1;
    }
    return foc_runtime_read_snapshot((struct foc_runtime *)&context->runtime, snapshot);
}

int foc_virtual_current_observe(
    const struct foc_virtual_current *context,
    struct foc_virtual_current_observation *observation
)
{
    struct foc_runtime_snapshot snapshot;
    if (context == NULL || !context->initialized || observation == NULL ||
        foc_virtual_current_snapshot(context, &snapshot) != FOC_RUNTIME_OK) {
        return -1;
    }
    *observation = (struct foc_virtual_current_observation){
        .virtual_time_us = context->virtual_time_us,
        .mode = snapshot.mode,
        .motion_session = snapshot.motion_session,
        .motion_ref = context->motion_ref,
        .id_ref_a = (float)context->runtime.pipeline.target_a.d,
        .iq_ref_a = (float)context->runtime.pipeline.target_a.q,
        .id_a = (float)context->motor.state.id_a,
        .iq_a = (float)context->motor.state.iq_a,
        .duty_a = context->output.request.duty_a,
        .duty_b = context->output.request.duty_b,
        .duty_c = context->output.request.duty_c,
        .v_alpha_v = (float)context->output.v_alpha_v,
        .v_beta_v = (float)context->output.v_beta_v,
        .accepted_updates = context->output.accepted_updates,
        .rejected_updates = context->output.rejected_updates,
        .saturated_updates = context->output.saturated_updates,
        .safety_state = snapshot.state,
        .faults = snapshot.faults,
        .mechanical_position_rad = context->motor.state.mechanical_position_rad,
        .mechanical_velocity_rad_s = context->motor.state.mechanical_velocity_rad_s,
    };
    return 0;
}

const struct foc_virtual_motor_state *foc_virtual_current_motor(
    const struct foc_virtual_current *context
)
{
    return context == NULL || !context->initialized ? NULL : &context->motor.state;
}

uint32_t foc_virtual_current_hash(const struct foc_virtual_current *context)
{
    return context == NULL || !context->initialized ? 0U : context->replay_hash;
}

/* ---------------------------------------- end of file ----------------------------------------- */
