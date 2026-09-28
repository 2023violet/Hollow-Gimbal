/**
 * @file    motor_runtime.h
 * @brief   FalconFoc motor product runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 *
 * Design overview
 * ---------------
 * This header owns the product-runtime seam for the motor instance: defaults,
 * restore, deferred save, calibration entry preparation, and read-only
 * snapshot access for shell/diag consumers.
 *
 * Usage example
 * -------------
 * - call `motor_runtime_init()` during product bring-up before tasks start
 * - call `motor_runtime_attach()` once the live motor instance is ready
 * - runtime/shell readers may call `motor_runtime_current()` or
 *   `motor_runtime_snapshot()` after attach
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - caller owns the `struct motor_dev` storage passed into this API
 * - this module borrows that instance and never allocates a second motor owner
 * - `motor_runtime_snapshot()` is read-only and protects its copy window with
 *   the shared runtime critical-section helper
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"
#include "motor_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize one caller-owned motor instance with FalconFoc defaults.
 *
 * @param  motor  Motor instance storage owned by the caller.
 */
void motor_runtime_init(struct motor_dev *motor);

/**
 * @brief  Consume deferred save requests in task context.
 *
 * @param  motor  Live motor instance that owns the save-request bitmask.
 */
void motor_runtime_handle_deferred_save(struct motor_dev *motor);

/* Restore / Calibration Glue ------------------------------------------------------------- */

/**
 * @brief  Reset transient control state before entering a calibration phase.
 *
 * @param  motor  Live motor instance to sanitize.
 */
void motor_calib_prepare_entry(struct motor_dev *motor);

/**
 * @brief  Re-apply runtime-side restore glue after parameter load succeeds.
 *
 * @param  motor  Live motor instance whose persisted configuration was loaded.
 */
void motor_restore_after_param_load(struct motor_dev *motor);

/**
 * @brief  Clear transient command state without re-running full initialization.
 *
 * @param  motor  Live motor instance to sanitize.
 */
void motor_runtime_reset_transient_setpoints(struct motor_dev *motor);

/* Live Binding --------------------------------------------------------------------------- */

/**
 * @brief  Return the current live motor owner bound into the runtime seam.
 *
 * @return Borrowed motor pointer, or `NULL` when no live instance is attached.
 */
struct motor_dev *motor_runtime_current(void);

/**
 * @brief  Bind or clear the current live motor instance.
 *
 * @param  motor  Borrowed motor pointer, or `NULL` to detach the seam.
 */
void              motor_runtime_attach(struct motor_dev *motor);

/* Request / Control --------------------------------------------------------------------- */

enum motor_runtime_axis_action {
    TM_MOTOR_RUNTIME_AXIS_START   = 0,
    TM_MOTOR_RUNTIME_AXIS_STOP    = 1,
    TM_MOTOR_RUNTIME_AXIS_DISABLE = 2,
    TM_MOTOR_RUNTIME_AXIS_CLEAR_FAULT = 3,
};

bool motor_runtime_request_axis_action(enum motor_runtime_axis_action action);
bool motor_runtime_request_ctrl_mode(enum foc_ctrl_mode mode);
bool motor_runtime_request_input_mode(enum foc_input_mode mode);
bool motor_runtime_request_calibration(enum foc_calib_state calib_type);
int  motor_runtime_request_calibration_abort(void);

bool motor_runtime_store_status(struct param_status_all *out);
bool motor_runtime_store_layout(uint8_t mask, struct param_save_layout *out);
bool motor_runtime_request_store_save(uint8_t mask);
bool motor_runtime_request_store_load(uint8_t                mask,
                                      struct param_status_all *result_out,
                                      bool                   *partial_out);
bool motor_runtime_request_store_erase(void);

bool motor_runtime_diag_set_param(const char *name, float value);
bool motor_runtime_diag_set_pid(const char *loop_name, const char *field, float value);
bool motor_runtime_diag_reset_pid(const char *loop_name);
bool motor_runtime_diag_set_voltage(float vd, float vq, float we_ramp);
bool motor_runtime_diag_set_if(float iq_amp, float we);
/**
 * @brief  Enter pure voltage-mode open-loop spin at an output-shaft speed.
 *
 * Converts the commanded rpm into an electrical rad/s target using the
 * configured pole pairs and applies it through the CTRL_MODE_VOLT path with
 * if_enable cleared, so no current loop wraps the open-loop angle.  Valid
 * from IDLE only, exactly like the other diag motion writes.
 *
 * @param  vel_rpm      Open-loop speed target (output shaft, rpm).
 * @param  vq           Q-axis voltage amplitude (V, >= 0).
 * @param  we_elec_out  Receives the derived electrical velocity (rad/s).
 *
 * @return true when the command was accepted.
 */
bool motor_runtime_diag_set_voltloop(float vel_rpm, float vq, float *we_elec_out);
bool motor_runtime_request_if_to_vel_handoff(float vel_rpm);
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
bool motor_runtime_request_bench_velocity_step(float vel_rpm);
struct motor_runtime_adc_view {
    float ia_raw;
    float ib_raw;
    float ic_raw;
    float ia_offset;
    float ib_offset;
    float ic_offset;
    float ia_residual;
    float ib_residual;
    float ic_residual;
    uint32_t sample_seq;
    uint32_t sample_count;
    uint32_t fresh_count;
    uint8_t pwm_map;
    uint8_t adc_map;
    uint16_t adc_trigger;
    bool calib_valid;
    bool clamp_used;
};
bool motor_runtime_diag_read_adc(struct motor_runtime_adc_view *view);
bool motor_runtime_diag_zero_current_offsets(void);
struct motor_runtime_bench_status {
    uint32_t heartbeat_age_ms;
    uint32_t timeout_count;
    bool     armed;
    bool     expired;
};
bool motor_runtime_diag_bench_keepalive(void);
bool motor_runtime_diag_bench_status(struct motor_runtime_bench_status *out);
bool motor_runtime_bench_watchdog_tick(const struct motor_dev *motor);
#endif
bool motor_runtime_diag_set_phase_volt(int phase, float volt);
bool motor_runtime_diag_set_phase_seed_offset(float offset_rad);
#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
bool motor_runtime_diag_set_encoder_dir(uint8_t dir);
bool motor_runtime_diag_set_encoder_abs(uint8_t dir, float mech_offset);
bool motor_runtime_diag_set_pwm_map(uint8_t map_id);
bool motor_runtime_diag_set_adc_map(uint8_t map_id);
void motor_runtime_diag_reset_maps(void);
bool motor_runtime_diag_set_adc_trigger(uint16_t compare);
bool motor_runtime_diag_read_bench_maps(uint8_t *pwm_map,
                                        uint8_t *adc_map,
                                        uint16_t *adc_trigger);
bool motor_runtime_diag_encoder_trace_clear(void);
bool motor_runtime_diag_encoder_trace_read(uint32_t index,
                                           struct encoder_bench_sample *out);
uint32_t motor_runtime_diag_encoder_trace_count(void);
#endif
bool motor_runtime_request_set_passthrough(enum foc_ctrl_mode mode, float target, float limit);
bool motor_runtime_request_vel_ramp(float target_rads, float max_acc_rads);
bool motor_runtime_request_torque_ramp(float target_torque, float max_acc);
bool motor_runtime_request_pos_traj(float target_pos_rad,
                                    float max_vel_rads,
                                    float max_acc_rads,
                                    float max_decel_rads);

struct motor_runtime_diag_param_view {
    float poles;
    float rs;
    float ls;
    float flux;
    float js;
    float gear_ratio;
    float v_bus_ov_thr;
    float v_bus_uv_thr;
    float i_peak;
    float vel_over_rpm;
    float curr_bandwidth;
    float vel_bandwidth;
    float pos_bandwidth;
};

struct motor_runtime_pid_view {
    float kp;
    float ki;
    float kd;
    float lim;
};

bool motor_runtime_diag_read_params(struct motor_runtime_diag_param_view *view);
bool motor_runtime_diag_read_pid(const char *loop_name, struct motor_runtime_pid_view *view);

/* Snapshot / Diag ------------------------------------------------------------------------ */

/**
 * @brief  Read-only shell/diag snapshot of the current live motor instance.
 *
 * The snapshot is intentionally a copy so shell and diag readers do not become
 * a second mutable owner of the live motor state.
 */
struct motor_runtime_snapshot {
    /** Live axis state copied from the bound motor instance. */
    uint8_t  axis_state;
    /** Active control mode copied from the bound motor instance. */
    uint8_t  ctrl_mode;
    /** Active input mode copied from the bound motor instance. */
    uint8_t  input_mode;
    /** Active sensor type copied from the bound motor instance. */
    uint8_t  sensor_type;
    /** Latched fault bitmap at the time of the snapshot. */
    uint32_t fault_code;
    /** Top-level calibration stage. */
    uint8_t  calib_state;
    /** Rs calibration sub-state. */
    uint8_t  rs_state;
    /** Ls calibration sub-state. */
    uint8_t  ls_state;
    /** Flux calibration sub-state. */
    uint8_t  flux_state;
    /** Js calibration sub-state. */
    uint8_t  js_state;
    /** Encoder calibration sub-state. */
    uint8_t  enc_state;
    /** Non-zero when encoder calibration is currently valid. */
    bool     enc_valid;
    /** Bus voltage snapshot in volts. */
    float    v_bus;
    /** Q-axis current snapshot in amperes. */
    float    i_q;
    /** D-axis current snapshot in amperes. */
    float    i_d;
    /** Raw ADC samples for phase A/B/C. */
    float    ia_raw;
    float    ib_raw;
    float    ic_raw;
    /** Raw ADC offsets used for phase-current subtraction. */
    float    ia_offset;
    float    ib_offset;
    float    ic_offset;
    /** Reconstructed phase currents in amperes. */
    float    ia;
    float    ib;
    float    ic;
    /** Raw bus-voltage ADC sample. */
    float    v_bus_raw;
    /** Voltage-open-loop and measured electrical-angle diagnostics. */
    float    theta_sim;
    float    theta_elec;
    float    we_volt_now;
    float    we_volt_ramp;
    /** Last FOC voltage-vector values. */
    float    vd;
    float    vq;
    float    v_alpha;
    float    v_beta;
    /** Last SVPWM duty values and sector. */
    float    dtc_a;
    float    dtc_b;
    float    dtc_c;
    uint8_t  sector;
    /** Output-shaft velocity snapshot in RPM. */
    float    out_vel_rpm;
    /** Output-shaft multi-turn position snapshot in turns. */
    float    out_pos_turns;
    /** Motor temperature snapshot in degrees Celsius. */
    float    motor_temp;
    /** Driver temperature snapshot in degrees Celsius. */
    float    driver_temp;
    /** Mechanical encoder angle snapshot in radians. */
    float    angle_mech_rad;
    /** Electrical encoder angle snapshot in radians. */
    float    angle_elec_rad;
    /** Encoder velocity snapshot in radians per second. */
    float    encoder_vel_rads;
    /** Borrowed encoder mechanical offset at snapshot time. */
    float    mech_offset;
    /** Borrowed encoder pole-pair configuration at snapshot time. */
    uint8_t  pole_pairs;
    /** Deferred save-request bitmask. */
    uint8_t  save_req;
};

/**
 * @brief  Copy a read-only runtime snapshot of the bound live motor instance.
 *
 * @param[out] snapshot  Caller-owned output snapshot buffer.
 *
 * @retval true   Snapshot copied successfully.
 * @retval false  No live motor is attached, or `snapshot == NULL`.
 */
bool motor_runtime_snapshot(struct motor_runtime_snapshot *snapshot);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
