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
