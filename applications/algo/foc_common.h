/**
 * @file foc_common.h
 * @brief   FOC common contract types and shared live state
 * @version v1.0.0
 * @date 2025.12.23
 *
 * Design overview
 * ---------------
 * This header groups the shared FOC enums, configuration structs, derived
 * parameters, and runtime state that are reused across algo, periph, runtime,
 * tasks, and shell owners.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - config and derived values are written during bring-up / restore / calib
 * - runtime state is mutated by the product owners, not by shell helpers
 * - fast-path code may read these types directly when latency requires it
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "encoder.h"
#include "foc_config.h"
#include "foc_core.h"
#include "fsm.h"
#include "pid.h"
#include "traj_trap.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/* Fault flag bitmask definitions (for motor_fault_state.flags) */
#define FAULT_NONE               (0U)
#define FAULT_OVER_CURR          (1U << 0)
#define FAULT_OVER_VOLT          (1U << 1)
#define FAULT_UNDER_VOLT         (1U << 2)
#define FAULT_OVER_VEL           (1U << 3)
#define FAULT_MOTOR_OVER_TEMP    (1U << 4)
#define FAULT_DRIVER_OVER_TEMP   (1U << 5)
#define FAULT_SENSOR_ERROR       (1U << 6)
#define FAULT_CALIBRATION_FAILED (1U << 7)
#define FAULT_COMM_ERROR         (1U << 8)

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Control mode
 */
enum foc_ctrl_mode {
    CTRL_MODE_VOLT = 0, /* Open loop control */
    CTRL_MODE_CURR = 1, /* Torque control */
    CTRL_MODE_VEL = 2, /* Velocity control */
    CTRL_MODE_POS = 3, /* Position control */
};

/**
 * @brief Input mode
 */
enum foc_input_mode {
    INPUT_MODE_INACTIVE = 0, /* Inactive (Output 0) */
    INPUT_MODE_PASSTHROUGH = 1, /* Passthrough (Directly apply input) */
    INPUT_MODE_VEL_RAMP = 2, /* Velocity Ramp (Linear acceleration) */
    INPUT_MODE_TORQUE_RAMP = 3, /* Torque Ramp (Soft start/stop) */
    INPUT_MODE_POS_FILTER = 4, /* Position Filter (2nd order input filter) */
    INPUT_MODE_VEL_FILTER = 5, /* Velocity Filter (1st order lpf for noisy input) */
    INPUT_MODE_TRAP_TRAJ = 6, /* Trapezoidal Trajectory (Pos -> Vel -> Accel limit) */
    INPUT_MODE_TUNING = 7, /* Tuning (Sine/Step/Square wave injection) */
};

/**
 * @brief Axis state machine
 */
enum foc_axis_state {
    AXIS_STATE_IDLE = 0, /* No operation, safe state */
    AXIS_STATE_CALIB = 1, /* Current offset calibration */
    AXIS_STATE_RUNNING = 2, /* Closed-loop control (FOC) */
    AXIS_STATE_FAULT = 3, /* Fault state */
};

/**
 * @brief Calibration state machine
 */
enum foc_calib_state {
    CALIB_CURR = 0, /* Current calibration */
    CALIB_RS = 1, /* Rs calibration */
    CALIB_LS = 2, /* Ls calibration */
    CALIB_FLUX = 3, /* Flux calibration */
    CALIB_JS = 4, /* Js calibration */
    CALIB_RL = 5, /* Rl calibration */
    CALIB_JB = 6, /* JB calibration */
    CALIB_ENC = 7, /* Encoder calibration */
};

/**
 * @brief Rs calibration states
 */
enum foc_rs_calib_state {
    RS_IDLE = 0, /* Rs initialization */
    RS_ALIGN = 1, /* Rs alignment */
    RS_VOLT = 2, /* Rs voltage calibration */
    RS_WAIT = 3, /* Rs wait */
    RS_SAMP = 4, /* Rs sampling */
    RS_CALC = 5, /* Rs calculation */
    RS_END = 6, /* Rs done */
};

/**
 * @brief Ls calibration states
 */
enum foc_ls_calib_state {
    LS_IDLE = 0, /* Ls initialization */
    LS_LD_LOOP = 1, /* Ls LD loop calibration */
    LS_LQ_LOOP = 2, /* Ls LQ loop calibration */
    LS_CALC = 4, /* Ls calculation */
    LS_END = 5, /* Ls done */
};

/**
 * @brief Flux calibration states
 */
enum foc_flux_calib_state {
    FX_IDLE = 0, /* Flux initialization */
    FX_SPD_L = 1, /* Flux speed low */
    FX_SAMP_L = 2, /* Flux sampling low */
    FX_SPD_H = 3, /* Flux speed high */
    FX_SAMP_H = 4, /* Flux sampling high */
    FX_CALC = 5, /* Flux calculation */
    FX_END = 6, /* Flux done */
};

/**
 * @brief Js calibration states
 */
enum foc_js_calib_state {
    JS_IDLE = 0, /* Js initialization */
    JS_LOOP = 1, /* Js loop calibration */
    JS_CALC = 2, /* Js calculation */
    JS_END = 3, /* Js done */
};

/**
 * @brief Rl calibration states
 */
enum foc_rl_calib_state {
    RL_IDLE = 0, /* Rl initialization */
    RL_ALIGN = 1, /* Rl alignment */
    RL_VOLT = 2, /* Rl voltage calibration */
    RL_CALC = 3, /* Rl calculation */
    RL_END = 4, /* Rl done */
};

/**
 * @brief JB calibration states
 */
enum foc_jb_calib_state {
    JB_IDLE = 0, /* JB initialization */
    JB_LOOP = 1, /* JB loop calibration */
    JB_CALC = 2, /* JB calculation */
    JB_END = 3, /* JB done */
};

/**
 * @brief encoder Calibration Type
 */
enum foc_enc_calib_state {
    ENC_IDLE = 0, /* Encoder initialization */
    ENC_PP_START = 1, /* Position Probing Stationary */
    ENC_PP_CALC = 2, /* Position Probing Calculation */
    ENC_LUT_START = 3, /* Lookup Table Initialization */
    ENC_LUT_CW = 4, /* Lookup Table CW */
    ENC_LUT_CCW = 5, /* Lookup Table CCW */
    ENC_LUT_CALC = 6, /* Lookup Table Calculation */
    ENC_LUT_FLUSH = 7, /* Lookup Table Flush */
    ENC_LUT_END = 8, /* Lookup Table End */
};

/**
 * @brief sensor types
 */
enum sensor_type {
    SENSOR_TYPE_NONE = 0, /* None */
    SENSOR_TYPE_MAGNETIC = 1, /* Magnetic sensors like MT6816, AS5047 */
    SENSOR_TYPE_HALL = 2, /* Hall sensor */
    SENSOR_TYPE_INC_ENC = 3, /* Incremental encoder ABZ */
};

/**
 * @brief sensorless estimator types
 */
enum sensorless_type {
    SENSORLESS_TYPE_NONE = 0, /* None */
    SENSORLESS_TYPE_NLOB = 1, /* Nested loop observer */
    SENSORLESS_TYPE_ALOB = 2, /* Adaptive loop observer */
    SENSORLESS_TYPE_SCVM = 3, /* Sliding mode observer */
    SENSORLESS_TYPE_ESMO = 4, /* Extended sliding mode observer */
    SENSORLESS_TYPE_HSFI = 5, /* High speed flux injection */
    SENSORLESS_TYPE_EKF = 6, /* Extended Kalman filter */
};

/* Configuration Data ---------------------------------------------------------------------------- */

/**
 * @brief Motor hardware configuration - Physical characteristics determined at compile time
 */
struct motor_hw_config { /* Electrical parameters */
    float poles; /* Pole pairs */
    float rs; /* Phase resistance (Ω) */
    float ls; /* Phase inductance (H) */

    /* Magnetic circuit parameters */
    float flux; /* Permanent magnet flux linkage (Wb) */
    float js; /* Moment of inertia of the rotor (kg·m²) */
    float l_d; /* D-axis inductance (H) */
    float l_q; /* Q-axis inductance (H) */
    float l_d_diff; /* D-axis inductance difference (H) */

    /* Mechanical parameters */
    float inertia; /* Moment of inertia (kg·m²) */
    float gear_ratio; /* Gear ratio */
    float gear_eff; /* Gear efficiency */

    uint8_t phase_order;
};

/**
 * @brief Protection configuration - System safety thresholds
 */
struct motor_prot_config {
    /* Electrical protection */
    float v_rated; /* Rated voltage (V) */
    float i_rated; /* Rated current (A) */
    float i_peak; /* Peak current (A) */

    /* Mechanical protection */
    float vel_rated; /* Rated output speed (rad/s, output shaft) */
    float vel_peak; /* Peak output speed (rad/s, output shaft) */
    float tor_rated; /* Rated torque (N·m) */
    float tor_peak; /* Peak torque (N·m) */

    /* Temperature protection */
    float motor_temp_thr; /* Maximum motor temperature (°C) */
    float driver_temp_thr; /* Maximum driver temperature (°C) */

    /* Power supply protection */
    float v_bus_ov_thr; /* Bus overvoltage threshold (V) */
    float v_bus_uv_thr; /* Bus undervoltage threshold (V) */
    float vel_over_thr; /* Overspeed threshold (rad/s, output shaft) */
};

/**
 * @brief Control algorithm configuration - Controller parameters
 */
struct motor_ctrl_config {
    /* Current loop PI parameters */
    struct pid_dev iq_pid;
    struct pid_dev id_pid;
    struct pid_dev vel_pid;
    struct pid_dev pos_pid;

    /* Ramp parameters */
    float ramp_vel; /* Maximum ramp velocity (rad/s, output shaft) */
    float ramp_accel; /* Ramp acceleration (rad/s², output shaft) */
    float ramp_decel; /* Ramp deceleration (rad/s², output shaft) */

    /* Trajectory parameters — traj_trap now uses rad / rad·s⁻¹ / rad·s⁻² */
    float traj_vel; /* Maximum trajectory velocity (rad/s, traj_trap unit) */
    float traj_accel; /* Trajectory acceleration (rad/s², traj_trap unit) */
    float traj_decel; /* Trajectory deceleration (rad/s², traj_trap unit) */
    float traj_jerk; /* Trajectory jerk (rad/s³, traj_trap unit) */

    /* Current loop bandwidth */
    float curr_bandwidth;
    float vel_bandwidth;
    float pos_bandwidth;
};

/* Derived Parameters --------------------------------------------------------------------------- */

/**
 * @brief Motor calculation parameters - Calculated from configuration at runtime
 */
struct motor_calc_params {
    /* Calculated from basic parameters */
    float kt; /* Torque constant = 1.5 * poles * psi (N·m/A) */
    float ke; /* Back EMF constant (V/(rad/s)) */
    float inv_kt; /* Inverse of torque constant */
    float inv_poles; /* Inverse of pole pairs */
    float inv_gear; /* Inverse of gear ratio */
    float poles_over_2pi; /* poles / (2π) */
};

/* Runtime State --------------------------------------------------------------------------------- */

/**
 * @brief Motor control state - FOC core runtime state
 */
struct motor_ctrl_state {
    /* User Targets (setpoints) */
    float target_id;
    float target_iq;
    float target_vel; /* Output target velocity (rad/s, output shaft) */
    float target_pos; /* Output target position (rad, output shaft, multi-turn) */
    float target_tor;

    /* Setpoints (control outputs) */
    float id_set, iq_set;
    float vd_set, vq_set;
    float vel_set; /* Velocity loop setpoint (rad/s, output shaft) */
    float pos_set; /* Position loop setpoint (rad, output shaft, multi-turn) */

    /* Feedforward */
    float iq_ff; /* Torque Current Feedforward (A) */
    float vel_ff; /* Velocity Feedforward (rad/s) */

    /* Voltage open-loop state */
    float theta_sim; /* Simulated electrical angle for CTRL_MODE_VOLT (rad) */
    float we_volt_ramp; /* Electrical angular velocity for voltage open-loop (rad/s) */

    // float omega_elec_set; /* Electrical angular velocity setpoint (rad/s) */
    // float omega_mech_set; /* Mechanical speed setpoint (rad/s) */
    // float theta_mech_set; /* Mechanical position setpoint (rad) */

    float dyn_vel_limit; /* Dynamic velocity limit for position passthrough (rad/s) */
    float dyn_iq_limit; /* Dynamic current limit for velocity passthrough (A) */

    float iq_max, id_max;
    float vel_max, tor_max; /* Maximum output velocity (rad/s, output shaft) */

    /* Filters */
    struct lpf iq_filter;
    struct lpf id_filter;
    // struct lpf vq_filter;
    // struct lpf vd_filter;
    // struct lpf v_bus_filter;
    // struct lpf vel_filter;
    // struct lpf pos_filter;
};

/**
 * @brief Motor measurement data - ADC sampling and sensor data
 */
struct motor_meas_data {
    /* Current measurements */
    float a; /* Phase A current (A) */
    float b; /* Phase B current (A) */
    float c; /* Phase C current (A) */
    float ia_raw;
    float ib_raw;
    float ic_raw;
    float ia_offset; /* Phase A raw ADC value */
    float ib_offset; /* Phase B raw ADC value */
    float ic_offset; /* Phase C raw ADC value */

    /* Voltage measurements */
    float v_bus; /* Bus voltage (V) */
    float v_bus_raw; /* Bus voltage raw ADC value */

    float v_bat; /* Battery voltage (V) */
    float v_bat_raw; /* Battery voltage raw ADC value */

    /* Temperature measurements */
    float motor_temp; /* Motor temperature (°C) */
    float driver_temp; /* Driver temperature (°C) */
    float motor_raw; /* Motor temperature raw value */
    float driver_raw; /* Driver temperature raw value */

    /* Position/velocity measurements */
    float theta_elec; /* Electrical angle (rad) */
    float theta_mech; /* Mechanical angle (rad) */
    float omega_elec; /* Electrical angular velocity (rad/s) */
    float omega_mech; /* Mechanical angular velocity (rad/s) */
    float total_enc; /* Total encoder count (turns) */

    float out_pos_rad; /* Output-shaft position (rad, multi-turn accumulator) */
    float out_vel_rads; /* Output-shaft velocity (rad/s, after gearbox) */

    float out_vel_rpm; /* Output-shaft velocity (RPM) */
    float out_pos_turns; /* Output-shaft position (turns, multi-turn) */
};

/**
 * @brief Encoder Calibration Runtime Context
 */
struct foc_enc_calib_ctx {
    /* State Variables */
    float pos; /* Current mechanical position (rad) */
    float pos_start; /* Position at start of probing */
    float pos_dir_end; /* Position at direction check point */
    float pos_err; /* Instantaneous position error */
    float pe_set; /* Target electrical angle */
    float pe_acc; /* Scanning acceleration/step */
    float pr_set; /* Target rotor angle */
    float vd_ref; /* D-axis voltage reference */

    /* Processing Data */
    int32_t raw; /* Raw encoder reading */
    uint32_t raw_start_idx; /* Starting index in LUT (for calculation) */
    uint16_t raw_off; /* Raw offset used by the final output path. */
    float mean; /* Mean error */
    float r_off; /* Rotor offset result */
    float e_off; /* Electrical offset result */
    float offset; /* Temp offset accumulator */

    /* Configuration & Counters */
    uint8_t pn; /* Pole pairs */
    uint8_t bit; /* Encoder resolution bits */
    uint16_t lut_num; /* LUT size */
    uint16_t window; /* Filter window size */
    uint32_t n1; /* Total electrical cycles steps */
    uint32_t n2; /* Steps per electrical cycle */

    /* Loop Counters */
    uint32_t cnt_i;
    uint32_t cnt_j;
    uint32_t cnt_k;
    int ind; /* LUT index */

    /* Data Buffers - Stream Mode (Only ~1KB RAM needed) */
    float lut[CALIB_LUT_SIZE];
    float p_raw[2];
    /* Stream-mode scratch buffers were kept minimal to match the live RAM budget. */
};

/**
 * @brief Motor calibration state - Calibration process tracking
 */
struct motor_calib_state {
    struct foc_enc_calib_ctx enc_ctx;

    enum foc_calib_state state; /* Calibration state */
    enum foc_rs_calib_state rs_state; /* Rs calibration state */
    enum foc_ls_calib_state ls_state; /* Ls calibration state */
    enum foc_flux_calib_state flux_state; /* Flux calibration state */
    enum foc_js_calib_state js_state; /* Js calibration state */
    enum foc_enc_calib_state enc_state; /* Encoder calibration state */

    bool calib_valid_flags; /* Calibration valid flags */

    /* --- NEW in v2.0 --- */
    uint8_t param_save_req; /* Deferred Flash save request (PARAM_SAVE_xxx bitmask) */
    /* Set by ISR/calib; consumed by motor_task in RTOS ctx  */
};

/**
 * @brief Complete motor instance structure
 */
struct motor_dev {
    /* Configuration references (ROM) */
    struct motor_hw_config hw_config;
    struct motor_prot_config prot_config;
    struct motor_ctrl_config ctrl_config;

    /* Calculated parameters (RAM, unchanged after initialization) */
    struct motor_calc_params calc_params;

    /* Runtime state (RAM, dynamically changing) */
    struct motor_ctrl_state ctrl_state;
    struct motor_calib_state calib_state;
    struct motor_meas_data meas_data;

    /* FOC calculation variables (RAM, updated in FOC loop) */
    struct foc_vars foc_calc;

    struct encoder_dev enc_pri; /* Primary encoder */
    struct encoder_dev enc_sec; /* Secondary encoder */

    struct traj_trap traj;

    struct fsm axis_fsm;

    /* Mode states */
    enum foc_ctrl_mode ctrl_mode;
    enum foc_input_mode input_mode;
    enum foc_axis_state axis_state;
    enum sensor_type sensor_type;

    uint32_t fault_code;
};

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
