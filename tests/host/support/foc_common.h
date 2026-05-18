/**
 * @file    foc_common.h
 * @brief   Host shim for motor_fsm and shell-store unit coverage
 * @version v1.2.0
 * @date    2026.05.14
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "fsm.h"
#include "pid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIR_CW            (0U)
#define DIR_CCW           (1U)
#define PHASE_ORDER_ABC   (0U)
#define PHASE_ORDER_ACB   (1U)
#define ENCODER_LUT_BITS  (7U)
#define ENCODER_LUT_SIZE  (1U << ENCODER_LUT_BITS)
#define FOC_2PI           (6.28318530717958647692f)
#define FOC_INV_2PI       (0.15915494309189533577f)
#define FOC_RPM_TO_RADS(x) ((x) * (FOC_2PI / 60.0f))
#define FOC_RADS_TO_RPM(x) ((x) * (60.0f * FOC_INV_2PI))
#define FAULT_NONE        (0U)
#define FAULT_OVER_CURR   (1U << 0)
#define FAULT_OVER_VOLT   (1U << 1)
#define FAULT_UNDER_VOLT  (1U << 2)
#define FAULT_OVER_VEL    (1U << 3)
#define FAULT_MOTOR_OVER_TEMP    (1U << 4)
#define FAULT_DRIVER_OVER_TEMP   (1U << 5)
#define FAULT_SENSOR_ERROR       (1U << 6)
#define FAULT_CALIBRATION_FAILED (1U << 7)
#define FAULT_COMM_ERROR         (1U << 8)

enum foc_ctrl_mode {
    CTRL_MODE_VOLT = 0,
    CTRL_MODE_CURR = 1,
    CTRL_MODE_VEL  = 2,
    CTRL_MODE_POS  = 3,
};

enum foc_input_mode {
    INPUT_MODE_INACTIVE    = 0,
    INPUT_MODE_PASSTHROUGH = 1,
    INPUT_MODE_VEL_RAMP    = 2,
    INPUT_MODE_TORQUE_RAMP = 3,
    INPUT_MODE_POS_FILTER  = 4,
    INPUT_MODE_VEL_FILTER  = 5,
    INPUT_MODE_TRAP_TRAJ   = 6,
    INPUT_MODE_TUNING      = 7,
};

enum foc_axis_state {
    AXIS_STATE_IDLE    = 0,
    AXIS_STATE_CALIB   = 1,
    AXIS_STATE_RUNNING = 2,
    AXIS_STATE_FAULT   = 3,
};

enum sensor_type {
    SENSOR_TYPE_NONE     = 0,
    SENSOR_TYPE_MAGNETIC = 1,
    SENSOR_TYPE_HALL     = 2,
    SENSOR_TYPE_INC_ENC  = 3,
};

enum foc_calib_state {
    CALIB_CURR = 0,
    CALIB_RS   = 1,
    CALIB_LS   = 2,
    CALIB_FLUX = 3,
    CALIB_JS   = 4,
    CALIB_RL   = 5,
    CALIB_JB   = 6,
    CALIB_ENC  = 7,
};

enum foc_rs_calib_state {
    RS_IDLE = 0,
};

enum foc_ls_calib_state {
    LS_IDLE = 0,
};

enum foc_flux_calib_state {
    FX_IDLE = 0,
};

enum foc_js_calib_state {
    JS_IDLE = 0,
};

enum foc_enc_calib_state {
    ENC_IDLE = 0,
};

struct encoder_config {
    uint8_t dir;
    float   mech_offset;
    float   pole_pairs;
};

struct encoder_pll_state {
    uint16_t raw_value;
    bool     calib_valid;
    float    enc_lut;
    int32_t  enc_table[ENCODER_LUT_SIZE];
};

struct encoder_dev {
    struct encoder_config   *config;
    struct encoder_pll_state state;
};

struct foc_enc_calib_ctx {
    uint32_t reserved;
};

struct motor_calc_params {
    float kt;
    float inv_kt;
};

struct motor_ctrl_state {
    float target_id;
    float target_iq;
    float target_vel;
    float target_pos;
    float target_tor;
    float id_set;
    float iq_set;
    float vd_set;
    float vq_set;
    float vel_set;
    float pos_set;
    float iq_ff;
    float vel_ff;
    float dyn_iq_limit;
    float dyn_vel_limit;
    float iq_max;
    float id_max;
    float vel_max;
    float tor_max;
};

struct motor_prot_config {
    float v_bus_ov_thr;
    float v_bus_uv_thr;
    float i_peak;
    float vel_over_thr;
};

struct motor_ctrl_config {
    struct pid_dev id_pid;
    struct pid_dev iq_pid;
    struct pid_dev vel_pid;
    struct pid_dev pos_pid;
    float          ramp_accel;
    float          curr_bandwidth;
    float          vel_bandwidth;
    float          pos_bandwidth;
};

struct motor_meas_data {
    float v_bus;
    float out_pos_rad;
    float out_vel_rads;
    float out_vel_rpm;
    float out_pos_turns;
    float motor_temp;
    float driver_temp;
};

struct foc_vars {
    float i_q;
    float i_d;
    uint32_t sentinel;
};

struct motor_hw_config {
    float   poles;
    float   rs;
    float   ls;
    float   flux;
    float   js;
    float   gear_ratio;
    uint8_t phase_order;
};

struct motor_calib_state {
    struct foc_enc_calib_ctx enc_ctx;

    enum foc_calib_state      state;
    enum foc_rs_calib_state   rs_state;
    enum foc_ls_calib_state   ls_state;
    enum foc_flux_calib_state flux_state;
    enum foc_js_calib_state   js_state;
    enum foc_enc_calib_state  enc_state;

    bool    calib_valid_flags;
    uint8_t param_save_req;
};

struct motor_dev {
    struct motor_hw_config   hw_config;
    struct motor_prot_config prot_config;
    struct motor_ctrl_config ctrl_config;
    struct motor_calc_params calc_params;
    struct motor_ctrl_state  ctrl_state;
    struct motor_calib_state calib_state;
    struct motor_meas_data   meas_data;
    struct foc_vars          foc_calc;
    struct encoder_dev       enc_pri;
    struct encoder_dev       enc_sec;
    struct fsm               axis_fsm;
    enum foc_ctrl_mode       ctrl_mode;
    enum foc_input_mode      input_mode;
    enum foc_axis_state      axis_state;
    enum sensor_type         sensor_type;
    uint32_t                 fault_code;
};

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
