/**
 * @file    foc_virtual_current.h
 * @brief   Deterministic Host Runtime harness for a virtual PMSM current loop.
 * @version v0.1.0
 * @date    2026.11.15
 *
 * Design overview
 * ---------------
 * Drive one virtual PMSM through the production Runtime and Current PI using
 * a deterministic Host clock and accepted-output adapter.
 *
 * Usage example
 * -------------
 * Initialize the default profile, publish a Current target, queue ARM/START,
 * and advance the caller-owned instance to a virtual timestamp.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * One caller owns and serializes a context. The adapter and Plant are
 * Host-only; no pointers escape the context and no dynamic allocation occurs.
 *
 * Failure / recovery
 * ------------------
 * Invalid lifecycle requests or time reversal return an error. Runtime Safety
 * remains authoritative for command, sample, heartbeat, and stop failures.
 *
 * Implementation role
 * -------------------
 * Connect the production Runtime, Device, Safety, Current PI, Encoder, and a
 * Host-only accepted-output adapter to one deterministic virtual clock.
 */
#pragma once

#include "foc_runtime.h"
#include "foc_virtual_motor.h"

#include <stdbool.h>
#include <stdint.h>

/* Public Types ---------------------------------------------------------------- */

/** Generic Host-only research profile; it is not Board1 calibration data. */
struct foc_virtual_current_profile {
    struct foc_virtual_motor_params motor;
    double load_torque_nm;
    float vbus_v;
    struct foc_device_adc_config adc;
    struct foc_device_pwm_config pwm;
    struct foc_encoder_config encoder;
    struct foc_current_config current;
    struct foc_safe_config safety;
    struct foc_runtime_config runtime;
    /** Host encoder distortion in undirected raw coordinates, zero disables. */
    double encoder_fundamental_error_rad;
    double encoder_third_error_rad;
};

/** Host output disposition used by the virtual plant. */
enum foc_virtual_output_mode {
    FOC_VIRTUAL_OUTPUT_HIGH_Z,
    FOC_VIRTUAL_OUTPUT_UPDATE,
    FOC_VIRTUAL_OUTPUT_ZERO_DUTY,
    FOC_VIRTUAL_OUTPUT_ACTIVE_BRAKE,
};

/** Host-only accepted-output journal. */
struct foc_virtual_current_output {
    struct foc_device_pwm_request request;
    struct foc_device_sim_registers registers;
    enum foc_virtual_output_mode mode;
    double v_alpha_v;
    double v_beta_v;
    uint32_t accepted_updates;
    uint32_t rejected_updates;
    uint32_t saturated_updates;
    bool reject_next_update;
};

/** Compact observation emitted after a deterministic event boundary. */
struct foc_virtual_current_observation {
    uint32_t virtual_time_us;
    enum foc_runtime_mode mode;
    uint32_t motion_session;
    float motion_ref;
    float id_ref_a;
    float iq_ref_a;
    float id_a;
    float iq_a;
    float duty_a;
    float duty_b;
    float duty_c;
    float v_alpha_v;
    float v_beta_v;
    uint32_t accepted_updates;
    uint32_t rejected_updates;
    uint32_t saturated_updates;
    enum foc_safe_state safety_state;
    uint32_t faults;
    double mechanical_position_rad;
    double mechanical_velocity_rad_s;
};

/** Host-only event journal for fault timing and H0 output accounting. */
struct foc_virtual_fault_events {
    bool injection_seen;
    uint32_t injection_us;
    uint32_t updates_at_injection;
    bool sensor_failure_seen;
    uint32_t sensor_failure_us;
    uint32_t updates_at_sensor_failure;
    bool h1_invalid_seen;
    uint32_t h1_invalid_us;
    uint32_t updates_at_h1_invalid;
    bool h0_failure_seen;
    uint32_t h0_failure_us;
    uint32_t updates_at_h0_failure;
    bool inhibit_requested_seen;
    uint32_t inhibit_requested_us;
    uint32_t updates_at_inhibit_requested;
    bool inhibit_accepted_seen;
    uint32_t inhibit_accepted_us;
    uint32_t updates_at_inhibit_accepted;
    bool fault_latched_seen;
    uint32_t fault_latched_us;
    uint32_t updates_at_fault_latched;
    bool last_update_seen;
    uint32_t last_accepted_update_us;
};

/** Caller-owned deterministic current-loop instance. */
struct foc_virtual_current {
    struct foc_virtual_current_profile profile;
    struct foc_virtual_motor motor;
    struct foc_runtime runtime;
    struct foc_encoder_context encoder;
    struct foc_virtual_current_output output;
    struct foc_vector_dq target_a;
    uint32_t virtual_time_us;
    uint32_t next_h0_us;
    uint32_t next_h1_us;
    uint32_t next_foreground_us;
    uint32_t sample_seq;
    uint32_t encoder_seq;
    uint32_t command_seq;
    uint32_t lifecycle_sequence;
    enum foc_runtime_action pending_action;
    enum foc_runtime_mode pending_mode;
    float motion_ref;
    uint32_t replay_hash;
    bool fail_next_sensor;
    struct foc_virtual_fault_events fault_events;
    bool initialized;
};

/* Public Functions ------------------------------------------------------------- */

/** Return the fixed GENERIC_RESEARCH_MOTOR profile used by VM-01a. */
struct foc_virtual_current_profile foc_virtual_current_default_profile(void);

/** Initialize Runtime, Plant, Encoder, and an accepted initial INHIBIT. */
int foc_virtual_current_init(struct foc_virtual_current *context,
                             const struct foc_virtual_current_profile *profile);

/** Publish a newer d/q current command at the current virtual time. */
int foc_virtual_current_publish_iq(struct foc_virtual_current *context, float id_a, float iq_a);

/** Publish one VELOCITY or POSITION reference at the current virtual time. */
int foc_virtual_current_publish_motion(struct foc_virtual_current *context,
                                       enum foc_runtime_mode mode,
                                       float motion_ref);

/** Publish an ARM, START, or SET_MODE request through the Runtime mailbox. */
int foc_virtual_current_publish_lifecycle(struct foc_virtual_current *context,
                                          enum foc_runtime_action action,
                                          enum foc_runtime_mode requested_mode);

/** Read the current Runtime mailbox result. */
int foc_virtual_current_read_lifecycle(const struct foc_virtual_current *context,
                                       struct foc_runtime_lifecycle_result *result);

/** Queue one lifecycle action for the next due H0 event. */
int foc_virtual_current_queue_action(struct foc_virtual_current *context,
                                     enum foc_runtime_action action);

/** Queue a legal disabled-state mode change for the next H0 event. */
int foc_virtual_current_queue_set_mode(struct foc_virtual_current *context,
                                       enum foc_runtime_mode mode);

/** Request one Host output UPDATE rejection. */
int foc_virtual_current_reject_next_update(struct foc_virtual_current *context);

/** Make the next virtual encoder transport observation fail. */
int foc_virtual_current_fail_next_sensor(struct foc_virtual_current *context);

/** Mark the exact virtual timestamp at which the next encoder failure is injected. */
int foc_virtual_current_mark_sensor_fault(struct foc_virtual_current *context,
                                          uint32_t injection_us);

/** Read the event-level fault and accepted-UPDATE journal. */
int foc_virtual_current_read_fault_events(const struct foc_virtual_current *context,
                                          struct foc_virtual_fault_events *events);

/** Advance deterministic events through target_us without wall-clock sleeping. */
int foc_virtual_current_run_until(struct foc_virtual_current *context, uint32_t target_us);

/** Revoke the command; the next H0 event services the real Runtime stop path. */
int foc_virtual_current_request_stop(struct foc_virtual_current *context);

/** Read the quiescent Runtime snapshot. */
int foc_virtual_current_snapshot(const struct foc_virtual_current *context,
                                 struct foc_runtime_snapshot *snapshot);

/** Read the current Host observation and virtual Plant state. */
int foc_virtual_current_observe(const struct foc_virtual_current *context,
                                struct foc_virtual_current_observation *observation);

/** Read-only Plant state for scenario assertions. */
const struct foc_virtual_motor_state *foc_virtual_current_motor(
    const struct foc_virtual_current *context);

/** Deterministic replay fingerprint accumulated at H0 boundaries. */
uint32_t foc_virtual_current_hash(const struct foc_virtual_current *context);

/* ---------------------------------------- end of file ----------------------------------------- */
