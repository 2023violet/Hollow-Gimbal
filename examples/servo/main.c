/**
 * @file main.c
 * @brief Standalone Host integration of FalconFOC Runtime IMPEDANCE.
 * @version 1.0
 * @date 2026-11-18
 *
 * Implementation role: application-owned scheduling, reference, and Host I/O.
 */

/* Includes ----------------------------------------------------------------- */

#include "foc_device_sim.h"
#include "foc_impedance.h"
#include "foc_impedance_feasibility.h"
#include "foc_impedance_feedforward.h"
#include "foc_impedance_tuning.h"
#include "foc_virtual_current.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Definitions ------------------------------------------------------ */

#define REQUIRE(condition)                                                  \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

enum {
    FRAME_ID = 81,
    MODEL_ID = 2,
    LOAD_ID = 3,
    GRID_US = 100,
    FIRST_UPDATE_US = 1000,
    END_US = 2400000,
    REVERSE_START_US = 900000,
    REVERSE_END_US = 1300000,
    GAIN_CHANGE_US = 1450000,
    LOAD_ON_US = 1550000,
    LOAD_OFF_US = 1900000,
};

static const float load_nm = 0.039f;

struct reference {
    float position;
    float velocity;
    float acceleration;
};

struct host_adapter {
    struct foc_device_sim_registers registers;
    struct foc_device_pwm_request held;
    uint32_t period_counts;
    unsigned updates;
    unsigned rejected;
    bool reject_next_update;
};

struct demo {
    struct foc_virtual_current_profile profile;
    struct foc_impedance_tuning_candidate gains;
    struct foc_runtime runtime;
    struct foc_encoder_context encoder;
    struct foc_virtual_motor motor;
    struct host_adapter adapter;
    struct foc_runtime_snapshot snapshot;
    struct foc_runtime_sensor first_frame;
    struct foc_runtime_command published;
    uint32_t time_us;
    uint32_t command_seq;
    uint32_t sensor_seq;
    uint32_t adc_seq;
    uint32_t request_seq;
    int encoder_status;
    int h1_status;
    int h0_status;
};

enum sensor_event {
    CAPTURE_FRAME,
    REPLAY_FIRST_FRAME,
    FAIL_FRAME,
};

struct metrics {
    double reverse_position_squared;
    double reverse_velocity_squared;
    unsigned reverse_samples;
    double peak_ref_iq;
    double peak_actual_iq;
    double load_peak_error;
    unsigned limited_updates;
    unsigned updates;
};

/* Private Helpers ---------------------------------------------------------- */

static bool submit_output(void *arg, const struct foc_device_pwm_request *request)
{
    struct host_adapter *adapter = arg;
    if (request->action == FOC_DEVICE_OUTPUT_UPDATE && adapter->reject_next_update) {
        adapter->reject_next_update = false;
        adapter->rejected++;
        return false;
    }
    if (foc_device_sim_submit(&adapter->registers, request, adapter->period_counts) != 0) {
        return false;
    }
    adapter->held = *request;
    if (request->action == FOC_DEVICE_OUTPUT_UPDATE) {
        adapter->updates++;
    }
    return true;
}

static int init_demo(struct demo *demo, float kt)
{
    *demo = (struct demo){0};
    demo->profile = foc_virtual_current_default_profile();
    demo->profile.runtime.motion.iq_limit_a = 2.2f;
    demo->profile.runtime.motion.impedance = (struct foc_runtime_impedance_config){
        .enabled = true,
        .mechanical_frame_id = FRAME_ID,
        .torque_constant_nm_per_a = kt,
        .torque_limit_nm = 0.315f,
        .max_stiffness_nm_per_rad = 1.0f,
        .max_damping_nm_s_per_rad = 0.1f,
        .max_feedforward_nm = 0.1f,
    };
    demo->adapter.period_counts = demo->profile.pwm.period_counts;
    const struct foc_pipeline_config pipeline = {
        .current = demo->profile.current,
        .adc = demo->profile.adc,
        .pwm = demo->profile.pwm,
        .safety = demo->profile.safety,
        .output_submit = submit_output,
        .output_arg = &demo->adapter,
    };
    const int status = foc_runtime_init(&demo->runtime, &pipeline, &demo->profile.runtime, 0U);
    if (status != FOC_RUNTIME_OK) {
        return status;
    }
    REQUIRE(foc_encoder_init(&demo->encoder, &demo->profile.encoder) == FOC_ENCODER_STATUS_OK);
    REQUIRE(
        foc_virtual_motor_init(
            &demo->motor,
            &demo->profile.motor,
            &(const struct foc_virtual_motor_state){0}
        ) == 0
    );
    const struct foc_impedance_tuning_input input = {
        .model_id = MODEL_ID,
        .damping_source = FOC_IMPEDANCE_TUNING_DAMPING_PROVIDED,
        .inertia_kg_m2 = (float)demo->profile.motor.inertia_kg_m2,
        .viscous_damping_nm_s_per_rad = (float)demo->profile.motor.damping_nm_s,
        .target_natural_frequency_rad_s = 18.0f,
        .target_damping_ratio = 0.75f,
        .torque_constant_nm_per_a = kt,
        .max_stiffness_nm_per_rad = demo->profile.runtime.motion.impedance.max_stiffness_nm_per_rad,
        .max_damping_nm_s_per_rad = demo->profile.runtime.motion.impedance.max_damping_nm_s_per_rad,
        .iq_limit_a = demo->profile.runtime.motion.iq_limit_a,
        .sample_period_s = (float)demo->profile.runtime.h1.period_us * 1e-6f,
        .extra_delay_s = (float)demo->profile.runtime.motion.prediction.max_prediction_us * 1e-6f,
    };
    REQUIRE(foc_impedance_tuning_design(&input, &demo->gains) == FOC_IMPEDANCE_TUNING_OK);
    REQUIRE(demo->gains.valid);
    return 0;
}

static struct reference transition(uint32_t now_us, uint32_t start_us, float from, float to)
{
    /* Quintic reference with zero velocity and acceleration at both ends. */
    const float duration_s = 0.4f;
    const float u = fminf((float)(now_us - start_us) * 1e-6f / duration_s, 1.0f);
    const float delta = to - from;
    const float tail = 1.0f - u;
    return (struct reference){
        .position = from + delta * u * u * u * (10.0f + u * (-15.0f + 6.0f * u)),
        .velocity = delta * 30.0f * u * u * tail * tail / duration_s,
        .acceleration = delta * 60.0f * u * tail * (1.0f - 2.0f * u) / (duration_s * duration_s),
    };
}

static struct reference reference_at(uint32_t now_us)
{
    if (now_us >= REVERSE_START_US) {
        return transition(now_us, REVERSE_START_US, 0.22f, -0.10f);
    }
    if (now_us >= 300000U) {
        return transition(now_us, 300000U, 0.0f, 0.22f);
    }
    return (struct reference){0};
}

static struct foc_runtime_command
command_at(const struct demo *demo, uint32_t now_us, const struct reference *reference)
{
    const bool changed = now_us >= GAIN_CHANGE_US;
    return (struct foc_runtime_command){
        .mode = FOC_RUNTIME_MODE_IMPEDANCE,
        .seq = demo->command_seq + 1U,
        .produced_us = now_us,
        .target.impedance = {
            .position_rad = reference->position,
            .velocity_rad_s = reference->velocity,
            .stiffness_nm_per_rad = demo->gains.stiffness_nm_per_rad * (changed ? 0.85f : 1.0f),
            .damping_nm_s_per_rad = demo->gains.damping_nm_s_per_rad * (changed ? 0.90f : 1.0f),
            .mechanical_frame_id = FRAME_ID,
            .source_session = demo->snapshot.motion_session,
        },
    };
}

static int publish_reference(struct demo *demo, uint32_t now_us, bool feedforward)
{
    const struct reference reference = reference_at(now_us);
    struct foc_runtime_command command = command_at(demo, now_us, &reference);
    if (feedforward) {
        const struct foc_impedance_feedforward_input input = {
            .inertia_kg_m2 = (float)demo->profile.motor.inertia_kg_m2,
            .viscous_damping_nm_s_per_rad = (float)demo->profile.motor.damping_nm_s,
            .reference_velocity_rad_s = reference.velocity,
            .reference_acceleration_rad_s2 = reference.acceleration,
            .limit_nm = demo->profile.runtime.motion.impedance.max_feedforward_nm,
            .model_id = MODEL_ID,
            .model_frame_id = FRAME_ID,
            .reference_frame_id = FRAME_ID,
            .reference_qualified = true,
        };
        struct foc_impedance_feedforward_result result;
        REQUIRE(
            foc_impedance_feedforward_evaluate(&input, &result) == FOC_IMPEDANCE_FEEDFORWARD_OK
        );
        REQUIRE(result.valid && result.mechanical_frame_id == FRAME_ID);
        command.target.impedance.torque_feedforward_nm = result.torque_nm;
    }
    REQUIRE(
        foc_runtime_publish_impedance_command(&demo->runtime, &command, now_us) == FOC_RUNTIME_OK
    );
    demo->published = command;
    demo->command_seq = command.seq;
    return 0;
}

static int capture_sensor(struct demo *demo, uint32_t now_us, enum sensor_event event)
{
    if (event == REPLAY_FIRST_FRAME) {
        demo->h1_status = foc_runtime_h1_step(&demo->runtime, &demo->first_frame, now_us, 0U);
        return 0;
    }

    uint16_t raw_code;
    REQUIRE(
        foc_virtual_motor_make_encoder_code(&demo->motor, &demo->profile.encoder, &raw_code) == 0
    );
    const enum foc_encoder_transport transport =
        event == FAIL_FRAME ? FOC_ENCODER_TRANSPORT_FRAME_ERROR : FOC_ENCODER_TRANSPORT_OK;
    struct foc_encoder_feedback feedback;
    demo->encoder_status =
        foc_encoder_update(&demo->encoder, now_us, raw_code, transport, &feedback);
    REQUIRE(
        demo->encoder_status ==
        (event == FAIL_FRAME ? FOC_ENCODER_STATUS_ERR_TRANSPORT : FOC_ENCODER_STATUS_OK)
    );
    struct foc_runtime_sensor sample = {
        .captured_us = feedback.captured_us,
        .seq = ++demo->sensor_seq,
        .valid = demo->encoder_status == FOC_ENCODER_STATUS_OK,
        .mechanical_rad = feedback.mechanical_rad,
        .mechanical_rad_s = feedback.speed_valid ? feedback.filtered_velocity_rad_s : 0.0f,
        .mechanical_frame_id = feedback.valid ? FRAME_ID : 0U,
    };
    if (sample.valid) {
        REQUIRE(
            foc_rotor_phase_from_counts(
                &demo->profile.runtime.motion.rotor,
                feedback.mechanical_phase_count,
                demo->profile.encoder.counts_per_revolution,
                &sample.theta_elec_rad
            ) == FOC_STATUS_OK
        );
        sample.electrical_phase_valid = true;
    }
    if (now_us == 500U) {
        demo->first_frame = sample;
    }
    demo->h1_status = foc_runtime_h1_step(&demo->runtime, &sample, now_us, 0U);
    return 0;
}

static int
tick(struct demo *demo, uint32_t now_us, bool publish, bool feedforward, enum sensor_event event)
{
    /* Advance using the previous accepted output. Events at this time run in
     * producer, H1, Foreground supervision, H0 order. */
    const struct foc_runtime_config *schedule = &demo->profile.runtime;
    if (now_us != demo->time_us) {
        const double dt = (double)(now_us - demo->time_us) * 1e-6;
        if (demo->adapter.held.action == FOC_DEVICE_OUTPUT_UPDATE) {
            REQUIRE(
                foc_virtual_motor_apply_duty(
                    &demo->motor,
                    &demo->adapter.held,
                    demo->profile.vbus_v,
                    dt
                ) == 0
            );
        } else {
            REQUIRE(foc_virtual_motor_step(&demo->motor, 0.0, 0.0, dt) == 0);
        }
    }
    demo->time_us = now_us;
    demo->h1_status = FOC_RUNTIME_IDLE;
    demo->h0_status = FOC_RUNTIME_IDLE;
    if (now_us % schedule->h1.period_us == 0U) {
        if (publish) {
            REQUIRE(publish_reference(demo, now_us, feedforward) == 0);
        }
        REQUIRE(capture_sensor(demo, now_us, event) == 0);
    }
    if (now_us % schedule->supervision.period_us == 0U) {
        struct foc_runtime_foreground_result foreground;
        REQUIRE(foc_runtime_foreground_poll(&demo->runtime, now_us, 0U, 0U, &foreground) >= 0);
    }
    if (now_us % schedule->h0.period_us == 0U) {
        struct foc_runtime_fast_input input = {.now_us = now_us};
        REQUIRE(
            foc_virtual_motor_make_adc(
                &demo->motor,
                &demo->profile.adc,
                demo->profile.vbus_v,
                ++demo->adc_seq,
                now_us,
                &input.adc
            ) == 0
        );
        demo->h0_status = foc_runtime_h0_step(&demo->runtime, &input);
        REQUIRE(foc_runtime_read_snapshot(&demo->runtime, &demo->snapshot) == FOC_RUNTIME_OK);
        REQUIRE(demo->snapshot.updates == demo->adapter.updates);
    }
    return 0;
}

static int advance_idle(struct demo *demo, uint32_t until_us, bool lifecycle_boundary)
{
    for (uint32_t t = demo->time_us + GRID_US; t <= until_us; t += GRID_US) {
        REQUIRE(tick(demo, t, false, false, CAPTURE_FRAME) == 0);
        REQUIRE(demo->h1_status >= 0);
        if (!lifecycle_boundary || t != until_us) {
            REQUIRE(demo->h0_status >= 0);
        }
    }
    return 0;
}

static int lifecycle(
    struct demo *demo,
    enum foc_runtime_action action,
    enum foc_runtime_mode mode,
    uint32_t applied_us,
    enum foc_runtime_request_result outcome,
    enum foc_safe_state state
)
{
    const struct foc_runtime_lifecycle_request request = {
        .action = action,
        .requested_mode = mode,
        .request_sequence = ++demo->request_seq,
        .source_session = demo->snapshot.motion_session,
        .produced_us = demo->time_us,
    };
    REQUIRE(
        foc_runtime_publish_lifecycle_request(&demo->runtime, &request, demo->time_us) ==
        FOC_RUNTIME_OK
    );
    REQUIRE(advance_idle(demo, applied_us, true) == 0);
    struct foc_runtime_lifecycle_result result;
    REQUIRE(foc_runtime_read_lifecycle_result(&demo->runtime, &result) == FOC_RUNTIME_OK);
    REQUIRE(
        demo->h0_status >= 0 ||
        (outcome == FOC_RUNTIME_REQUEST_REJECTED && demo->h0_status == FOC_RUNTIME_ERR_CONTROL)
    );
    REQUIRE(result.result == outcome && result.action == action && result.requested_mode == mode);
    REQUIRE(result.state == state && result.request_sequence == request.request_sequence);
    REQUIRE(result.h0_us == applied_us && result.applied_session == demo->snapshot.motion_session);
    const char *name = action == FOC_RUNTIME_ACTION_SET_MODE ? "SET_MODE"
        : action == FOC_RUNTIME_ACTION_ARM                   ? "ARM"
                                                             : "START";
    printf(
        "lifecycle %s %s H0_us=%u mode=%u state=%u session=%u\n",
        name,
        outcome == FOC_RUNTIME_REQUEST_APPLIED ? "APPLIED" : "REJECTED",
        (unsigned)result.h0_us,
        (unsigned)demo->snapshot.mode,
        (unsigned)result.state,
        (unsigned)result.applied_session
    );
    return 0;
}

static int start_demo(struct demo *demo)
{
    REQUIRE(tick(demo, 0U, false, false, CAPTURE_FRAME) == 0);
    REQUIRE(demo->h1_status >= 0 && demo->h0_status >= 0);
    REQUIRE(advance_idle(demo, 100U, false) == 0);
    REQUIRE(
        lifecycle(
            demo,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_IMPEDANCE,
            200U,
            FOC_RUNTIME_REQUEST_APPLIED,
            FOC_SAFE_STATE_DISABLED
        ) == 0
    );
    REQUIRE(advance_idle(demo, 400U, false) == 0);
    REQUIRE(tick(demo, 500U, true, false, CAPTURE_FRAME) == 0);
    REQUIRE(demo->h1_status == FOC_RUNTIME_OK);
    REQUIRE(demo->snapshot.mode == FOC_RUNTIME_MODE_IMPEDANCE);
    REQUIRE(
        lifecycle(
            demo,
            FOC_RUNTIME_ACTION_ARM,
            FOC_RUNTIME_MODE_IMPEDANCE,
            600U,
            FOC_RUNTIME_REQUEST_APPLIED,
            FOC_SAFE_STATE_READY
        ) == 0
    );
    REQUIRE(
        lifecycle(
            demo,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_IMPEDANCE,
            800U,
            FOC_RUNTIME_REQUEST_APPLIED,
            FOC_SAFE_STATE_RUNNING
        ) == 0
    );
    REQUIRE(demo->snapshot.updates == 0U);
    REQUIRE(advance_idle(demo, 900U, false) == 0);
    return 0;
}

static struct foc_impedance_config
impedance(const struct demo *demo, float stiffness, float damping)
{
    const struct foc_runtime_impedance_config *policy = &demo->profile.runtime.motion.impedance;
    return (struct foc_impedance_config){
        .stiffness_nm_per_rad = stiffness,
        .damping_nm_s_per_rad = damping,
        .torque_constant_nm_per_a = policy->torque_constant_nm_per_a,
        .torque_limit_nm = policy->torque_limit_nm,
    };
}

static int feasibility(const struct demo *demo)
{
    const struct foc_impedance_config config =
        impedance(demo, demo->gains.stiffness_nm_per_rad, demo->gains.damping_nm_s_per_rad);
    struct foc_impedance_feasibility_input input = {
        .live_iq_limit_a = demo->profile.runtime.motion.iq_limit_a,
        .external_load_nm = load_nm,
        .position_tolerance_rad = 0.08f,
        .torque_model_id = MODEL_ID,
        .load_model_id = LOAD_ID,
        .stationary_target = true,
        .support_torque_accounted = true,
        .current_tracking_qualified = true,
    };
    struct foc_impedance_feasibility_result result;
    REQUIRE(foc_impedance_feasibility_evaluate(&config, &input, &result) == FOC_STATUS_OK);
    REQUIRE(result.verdict == FOC_IMPEDANCE_FEASIBILITY_REACHABLE && result.has_equilibrium);
    printf(
        "static REACHABLE Iq_A=%.2f torque_capacity_Nm=%.6f margin_Nm=%.6f error_rad=%.6f\n",
        (double)input.live_iq_limit_a,
        result.torque_capacity_nm,
        result.torque_margin_nm,
        (double)result.equilibrium_error_rad
    );
    input.live_iq_limit_a = 0.25f;
    REQUIRE(foc_impedance_feasibility_evaluate(&config, &input, &result) == FOC_STATUS_OK);
    REQUIRE(result.verdict == FOC_IMPEDANCE_FEASIBILITY_TORQUE_INSUFFICIENT);
    printf(
        "static TORQUE_INSUFFICIENT Iq_A=%.2f torque_capacity_Nm=%.6f margin_Nm=%.6f\n",
        (double)input.live_iq_limit_a,
        result.torque_capacity_nm,
        result.torque_margin_nm
    );
    input.load_model_id = 0U;
    REQUIRE(foc_impedance_feasibility_evaluate(&config, &input, &result) == FOC_STATUS_OK);
    REQUIRE(result.verdict == FOC_IMPEDANCE_FEASIBILITY_UNKNOWN);
    puts("static UNKNOWN load_model_id=0");
    return 0;
}

static int observe_update(const struct demo *demo, const char *label, struct metrics *metrics)
{
    const struct foc_runtime_snapshot *status = &demo->snapshot;
    const struct foc_runtime_command *command = &demo->published;
    const struct foc_runtime_impedance_command *target = &command->target.impedance;
    const uint32_t now_us = demo->time_us;
    REQUIRE(status->state == FOC_SAFE_STATE_RUNNING && status->mode == FOC_RUNTIME_MODE_IMPEDANCE);
    REQUIRE(status->accepted_outer_seen && status->sensor_valid && status->output_confirmed);
    REQUIRE(
        status->command_seq == command->seq && status->command_us == command->produced_us &&
        status->motion_session == target->source_session &&
        status->accepted_outer_seq == demo->sensor_seq
    );
    REQUIRE(
        status->sample_us == now_us && status->sensor_us == command->produced_us &&
        status->angle_prediction_us == now_us - status->sensor_us
    );

    /* Re-evaluate the admitted reference and sensed feedback using the public Core law. */
    const struct foc_impedance_input input = {
        .position_error_rad = target->position_rad - status->mechanical_rad,
        .reference_velocity_rad_s = target->velocity_rad_s,
        .measured_velocity_rad_s = status->mechanical_rad_s,
        .torque_feedforward_nm = target->torque_feedforward_nm,
        .live_iq_limit_a = demo->profile.runtime.motion.iq_limit_a,
    };
    const struct foc_impedance_config config =
        impedance(demo, target->stiffness_nm_per_rad, target->damping_nm_s_per_rad);
    struct foc_impedance_output output;
    REQUIRE(foc_impedance_step(&config, &input, &output) == FOC_STATUS_OK && output.valid);
    const double actual_iq = demo->motor.state.iq_a;
    const double position_error =
        (double)target->position_rad - demo->motor.state.mechanical_position_rad;
    const double velocity_error =
        (double)target->velocity_rad_s - demo->motor.state.mechanical_velocity_rad_s;
    metrics->updates++;
    if (output.limited) {
        metrics->limited_updates++;
    }
    metrics->peak_ref_iq = fmax(metrics->peak_ref_iq, fabs((double)output.iq_ref_a));
    metrics->peak_actual_iq = fmax(metrics->peak_actual_iq, fabs(actual_iq));
    if (now_us >= REVERSE_START_US && now_us <= REVERSE_END_US) {
        metrics->reverse_samples++;
        metrics->reverse_position_squared += position_error * position_error;
        metrics->reverse_velocity_squared += velocity_error * velocity_error;
    }
    if (now_us >= LOAD_ON_US && now_us <= LOAD_OFF_US) {
        metrics->load_peak_error = fmax(metrics->load_peak_error, fabs(position_error));
    }
    if (now_us % 200000U == 0U || now_us == 1100200U || now_us == GAIN_CHANGE_US ||
        now_us == 1700200U) {
        printf(
            "%s t_us=%u ref_rad=%.5f pos_rad=%.5f ref_rad_s=%.5f vel_rad_s=%.5f "
            "K_Nm_per_rad=%.5f D_Nms_per_rad=%.5f FF_Nm=%.5f Iq_request_audit_A=%.5f "
            "Iq_plant_A=%.5f "
            "load_Nm=%.3f limited=%u mode=IMPEDANCE state=%u faults=0x%x action=%u session=%u "
            "FG_seq=%u FG_us=%u "
            "H1_seq=%u sensor_us=%u sensor_age_us=%u ADC_us=%u confirmed=%u updates=%u "
            "physical_applied=false\n",
            label,
            (unsigned)now_us,
            (double)target->position_rad,
            demo->motor.state.mechanical_position_rad,
            (double)target->velocity_rad_s,
            demo->motor.state.mechanical_velocity_rad_s,
            (double)target->stiffness_nm_per_rad,
            (double)target->damping_nm_s_per_rad,
            (double)target->torque_feedforward_nm,
            (double)output.iq_ref_a,
            actual_iq,
            demo->motor.load_torque_nm,
            (unsigned)output.limited,
            (unsigned)status->state,
            (unsigned)status->faults,
            (unsigned)status->action,
            (unsigned)status->motion_session,
            (unsigned)status->command_seq,
            (unsigned)status->command_us,
            (unsigned)status->accepted_outer_seq,
            (unsigned)status->sensor_us,
            (unsigned)status->angle_prediction_us,
            (unsigned)status->sample_us,
            (unsigned)status->output_confirmed,
            (unsigned)status->updates
        );
    }
    return 0;
}

static int verify_inhibited(
    struct demo *demo,
    const char *label,
    uint32_t old_session,
    unsigned updates,
    int expected_republish_status
)
{
    REQUIRE(demo->snapshot.action == FOC_DEVICE_OUTPUT_INHIBIT && demo->snapshot.output_confirmed);
    REQUIRE(demo->adapter.registers.bridge_enabled == 0U && demo->snapshot.updates == updates);
    REQUIRE(demo->snapshot.motion_session != old_session);

    struct foc_runtime_command stale =
        command_at(demo, demo->time_us, &(const struct reference){0});
    stale.target.impedance.source_session = old_session;
    const int publication =
        foc_runtime_publish_impedance_command(&demo->runtime, &stale, demo->time_us);
    printf(
        "%s action=INHIBIT state=%u faults=0x%x session=%u->%u updates=%u->%u "
        "old_session_publish=%d physical_applied=false\n",
        label,
        (unsigned)demo->snapshot.state,
        (unsigned)demo->snapshot.faults,
        (unsigned)old_session,
        (unsigned)demo->snapshot.motion_session,
        updates,
        (unsigned)demo->snapshot.updates,
        publication
    );
    REQUIRE(publication == expected_republish_status);
    return 0;
}

static int run_scenario(bool feedforward, struct metrics *metrics)
{
    struct demo demo;
    REQUIRE(init_demo(&demo, 0.15f) == 0);
    if (!feedforward) {
        printf(
            "HOST EXAMPLE PARAMETERS - NOT BOARD1 DEFAULTS model=%u frame=%u pole_pairs=%u "
            "flux_Wb=%.3f Kt_Nm_per_A=%.3f J_kg_m2=%.4f B_Nms_per_rad=%.3f CPR=%u direction=%d "
            "Iq_limit_A=%.2f H0_us=%u H1_us=%u foreground_us=%u angle_timeout_us=%u\n",
            MODEL_ID,
            FRAME_ID,
            demo.profile.motor.pole_pairs,
            demo.profile.motor.psi_f_wb,
            (double)demo.profile.runtime.motion.impedance.torque_constant_nm_per_a,
            demo.profile.motor.inertia_kg_m2,
            demo.profile.motor.damping_nm_s,
            (unsigned)demo.profile.encoder.counts_per_revolution,
            (int)demo.profile.encoder.direction,
            (double)demo.profile.runtime.motion.iq_limit_a,
            (unsigned)demo.profile.runtime.h0.period_us,
            (unsigned)demo.profile.runtime.h1.period_us,
            (unsigned)demo.profile.runtime.supervision.period_us,
            (unsigned)demo.profile.runtime.angle_timeout_us
        );
        printf(
            "tuning model=%u K_Nm_per_rad=%.6f D_Nms_per_rad=%.6f omega_rad_s=%.2f zeta=%.2f\n",
            (unsigned)demo.gains.model_id,
            (double)demo.gains.stiffness_nm_per_rad,
            (double)demo.gains.damping_nm_s_per_rad,
            (double)demo.gains.ideal_natural_frequency_rad_s,
            (double)demo.gains.ideal_damping_ratio
        );
        REQUIRE(feasibility(&demo) == 0);
    }
    const char *label = feedforward ? "MODEL_FF" : "ZERO_FF";
    printf("scenario %s hold/forward/reverse/KD_change/load/recovery\n", label);
    REQUIRE(start_demo(&demo) == 0);
    for (uint32_t t = FIRST_UPDATE_US; t <= END_US; t += GRID_US) {
        REQUIRE(tick(&demo, t, true, feedforward, CAPTURE_FRAME) == 0);
        REQUIRE(demo.h0_status >= 0 && demo.h1_status >= 0);
        if (t == LOAD_ON_US || t == LOAD_OFF_US) {
            REQUIRE(
                foc_virtual_motor_set_load(&demo.motor, t == LOAD_ON_US ? (double)load_nm : 0.0) ==
                0
            );
        }
        if (demo.h0_status == FOC_RUNTIME_OK) {
            REQUIRE(observe_update(&demo, label, metrics) == 0);
        }
    }
    REQUIRE(metrics->updates == demo.adapter.updates && metrics->updates == 11996U);
    REQUIRE(metrics->reverse_samples == 2001U);
    const uint32_t session = demo.snapshot.motion_session;
    foc_runtime_revoke_command(&demo.runtime);
    REQUIRE(tick(&demo, END_US + GRID_US, false, false, CAPTURE_FRAME) == 0);
    REQUIRE(tick(&demo, END_US + 2U * GRID_US, false, false, CAPTURE_FRAME) == 0);
    REQUIRE(demo.h0_status == FOC_RUNTIME_ERR_STOP);
    REQUIRE(advance_idle(&demo, END_US + 2000U, false) == 0);
    REQUIRE(verify_inhibited(&demo, "STOP", session, metrics->updates, FOC_RUNTIME_ERR_ORDER) == 0);
    printf(
        "%s reverse_position_RMS_rad=%.7f reverse_velocity_RMS_rad_s=%.7f "
        "peak_Iq_request_A=%.6f peak_Iq_plant_A=%.6f loaded_peak_error_rad=%.7f limited_updates=%u "
        "updates=%u\n",
        label,
        sqrt(metrics->reverse_position_squared / metrics->reverse_samples),
        sqrt(metrics->reverse_velocity_squared / metrics->reverse_samples),
        metrics->peak_ref_iq,
        metrics->peak_actual_iq,
        metrics->load_peak_error,
        metrics->limited_updates,
        metrics->updates
    );
    return 0;
}

static int publication_faults(void)
{
    struct demo demo;
    REQUIRE(init_demo(&demo, 0.0f) == FOC_RUNTIME_ERR_CONFIG);
    puts("invalid Kt: ERR_CONFIG");

    REQUIRE(init_demo(&demo, 0.15f) == 0);
    REQUIRE(tick(&demo, 0U, false, false, CAPTURE_FRAME) == 0);
    REQUIRE(advance_idle(&demo, 100U, false) == 0);
    struct foc_runtime_command command = command_at(&demo, 100U, &(const struct reference){0});
    REQUIRE(
        foc_runtime_publish_impedance_command(&demo.runtime, &command, 100U) ==
        FOC_RUNTIME_ERR_CONTROL
    );
    puts("publish in CURRENT: ERR_CONTROL");
    REQUIRE(
        lifecycle(
            &demo,
            FOC_RUNTIME_ACTION_START,
            FOC_RUNTIME_MODE_IMPEDANCE,
            200U,
            FOC_RUNTIME_REQUEST_REJECTED,
            FOC_SAFE_STATE_DISABLED
        ) == 0
    );

    REQUIRE(init_demo(&demo, 0.15f) == 0);
    REQUIRE(start_demo(&demo) == 0);
    REQUIRE(tick(&demo, 1000U, true, false, CAPTURE_FRAME) == 0);
    REQUIRE(demo.h0_status == FOC_RUNTIME_OK && demo.snapshot.updates == 1U);

    command = command_at(&demo, 1000U, &(const struct reference){0});
    command.target.impedance.stiffness_nm_per_rad = 1.01f;
    REQUIRE(
        foc_runtime_publish_impedance_command(&demo.runtime, &command, 1000U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    command = command_at(&demo, 1000U, &(const struct reference){0});
    command.target.impedance.damping_nm_s_per_rad = -0.01f;
    REQUIRE(
        foc_runtime_publish_impedance_command(&demo.runtime, &command, 1000U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    command = demo.published;
    REQUIRE(
        foc_runtime_publish_impedance_command(&demo.runtime, &command, 1000U) ==
        FOC_RUNTIME_ERR_ORDER
    );
    puts("invalid K and D: ERR_INPUT; duplicate sequence: ERR_ORDER");
    REQUIRE(
        lifecycle(
            &demo,
            FOC_RUNTIME_ACTION_SET_MODE,
            FOC_RUNTIME_MODE_CURRENT,
            1200U,
            FOC_RUNTIME_REQUEST_REJECTED,
            FOC_SAFE_STATE_RUNNING
        ) == 0
    );
    REQUIRE(demo.snapshot.mode == FOC_RUNTIME_MODE_IMPEDANCE && demo.snapshot.updates == 2U);
    return 0;
}

static int output_faults(enum sensor_event event, bool reject_update)
{
    struct demo demo;
    REQUIRE(init_demo(&demo, 0.15f) == 0 && start_demo(&demo) == 0);
    REQUIRE(tick(&demo, 1000U, true, false, CAPTURE_FRAME) == 0);
    REQUIRE(demo.h0_status == FOC_RUNTIME_OK && demo.snapshot.updates == 1U);
    const uint32_t session = demo.snapshot.motion_session;

    if (reject_update) {
        demo.adapter.reject_next_update = true;
        REQUIRE(tick(&demo, 1200U, false, false, CAPTURE_FRAME) == 0);
        REQUIRE(demo.h0_status == FOC_RUNTIME_ERR_CONTROL && demo.adapter.rejected == 1U);
        REQUIRE(
            verify_inhibited(
                &demo,
                "adapter rejected UPDATE",
                session,
                1U,
                FOC_RUNTIME_ERR_ORDER
            ) == 0
        );
    } else {
        REQUIRE(advance_idle(&demo, 1400U, false) == 0);
        REQUIRE(demo.snapshot.updates == 3U);
        REQUIRE(tick(&demo, 1500U, true, false, event) == 0);
        const int expected = event == FAIL_FRAME ? FOC_RUNTIME_ERR_INPUT : FOC_RUNTIME_ERR_ORDER;
        REQUIRE(demo.h1_status == expected);
        printf(
            "%s H1_status=%d capture_age_us=%u\n",
            event == FAIL_FRAME ? "invalid Sensor" : "stale Sensor",
            demo.h1_status,
            event == FAIL_FRAME ? 0U : (unsigned)(demo.time_us - demo.first_frame.captured_us)
        );
        REQUIRE(tick(&demo, 1600U, false, false, CAPTURE_FRAME) == 0);
        REQUIRE(demo.h0_status == FOC_RUNTIME_ERR_ALIGNMENT);
        REQUIRE(
            verify_inhibited(&demo, "Sensor stopped control", session, 3U, FOC_RUNTIME_ERR_ORDER) ==
            0
        );
    }

    const unsigned updates = demo.adapter.updates;
    for (uint32_t t = demo.time_us + GRID_US; t <= 2400U; t += GRID_US) {
        const enum sensor_event capture =
            event == FAIL_FRAME && t == 2000U ? FAIL_FRAME : CAPTURE_FRAME;
        REQUIRE(tick(&demo, t, false, false, capture) == 0);
        if (capture == FAIL_FRAME) {
            REQUIRE(demo.h1_status == FOC_RUNTIME_ERR_INPUT);
            REQUIRE(demo.h0_status == FOC_RUNTIME_ERR_ALIGNMENT);
        }
        REQUIRE(demo.snapshot.updates == updates && demo.adapter.registers.bridge_enabled == 0U);
    }
    if (event == FAIL_FRAME) {
        REQUIRE(demo.snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED);
        REQUIRE((demo.snapshot.faults & FOC_SAFE_FAULT_SAMPLE_INVALID) != 0U);
        REQUIRE(
            verify_inhibited(
                &demo,
                "SAMPLE_INVALID FAULT_LATCHED",
                session,
                updates,
                FOC_RUNTIME_ERR_CONTROL
            ) == 0
        );
    } else {
        REQUIRE(demo.snapshot.state == FOC_SAFE_STATE_DISABLED && demo.snapshot.faults == 0U);
    }
    return 0;
}

static int faults(void)
{
    REQUIRE(publication_faults() == 0);
    REQUIRE(output_faults(REPLAY_FIRST_FRAME, false) == 0);
    REQUIRE(output_faults(FAIL_FRAME, false) == 0);
    REQUIRE(output_faults(CAPTURE_FRAME, true) == 0);

    struct demo demo;
    REQUIRE(init_demo(&demo, 0.15f) == 0 && start_demo(&demo) == 0);
    REQUIRE(tick(&demo, 1000U, true, false, CAPTURE_FRAME) == 0);
    REQUIRE(demo.h0_status == FOC_RUNTIME_OK && demo.snapshot.updates == 1U);
    const uint32_t session = demo.snapshot.motion_session;
    const struct foc_runtime_command old = demo.published;
    unsigned stopped_updates = 0U;
    for (uint32_t t = 1100U; t <= 4000U; t += GRID_US) {
        REQUIRE(tick(&demo, t, false, false, CAPTURE_FRAME) == 0);
        REQUIRE(demo.h1_status >= 0);
        REQUIRE(
            demo.h0_status >= 0 || demo.h0_status == FOC_RUNTIME_ERR_CONTROL ||
            demo.h0_status == FOC_RUNTIME_ERR_STOP
        );
        if ((demo.snapshot.faults & FOC_SAFE_FAULT_COMMAND_TIMEOUT) != 0U) {
            if (stopped_updates == 0U) {
                stopped_updates = demo.adapter.updates;
            }
            REQUIRE(demo.adapter.updates == stopped_updates);
        }
    }
    REQUIRE(stopped_updates > 0U);
    REQUIRE(demo.snapshot.state == FOC_SAFE_STATE_FAULT_LATCHED);
    REQUIRE((demo.snapshot.faults & FOC_SAFE_FAULT_COMMAND_TIMEOUT) != 0U);
    REQUIRE(
        foc_runtime_publish_impedance_command(&demo.runtime, &old, demo.time_us) ==
        FOC_RUNTIME_ERR_CONTROL
    );
    REQUIRE(
        verify_inhibited(
            &demo,
            "COMMAND_TIMEOUT FAULT_LATCHED",
            session,
            stopped_updates,
            FOC_RUNTIME_ERR_CONTROL
        ) == 0
    );
    puts("fault checks PASS physical_applied=false");
    return 0;
}

/* Public Functions --------------------------------------------------------- */

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--faults") == 0) {
        return faults();
    }
    if (argc != 1) {
        fputs("Usage: falconfoc-servo-demo [--faults]\n", stderr);
        return 2;
    }
    struct metrics without = {0};
    struct metrics with = {0};
    REQUIRE(run_scenario(false, &without) == 0);
    REQUIRE(run_scenario(true, &with) == 0);
    REQUIRE(with.updates == without.updates && with.reverse_samples == without.reverse_samples);
    REQUIRE(with.reverse_position_squared < without.reverse_position_squared);
    puts("same-reference comparison PASS physical_applied=false");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
