/**
 * @file    foc_motion_runtime.c
 * @brief   FalconFoc motion-command runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_motion_runtime.h"

#include "foc_math.h"
#include "traj_trap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Private constants ---------------------------------------------------------------------------- */

/* Trajectory-task sample time (matches RTOS motor task period). */
#define DT_TRAJ_TASK (TRAJ_TASK_TS)

/* Private helpers ------------------------------------------------------------------------------ */

static void _motion_runtime_reset_soft_limits(struct motor_dev *motor)
{
    motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.vel_max;
    motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.vel_max;
    motor->ctrl_config.vel_pid.config.max_output     = motor->ctrl_state.iq_max;
    motor->ctrl_config.vel_pid.config.integral_limit = motor->ctrl_state.iq_max;
}

/* Public functions ---------------------------------------------------------------------------- */

void foc_process_input_task(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_state.iq_ff  = 0.0f;
    motor->ctrl_state.vel_ff = 0.0f;

    _motion_runtime_reset_soft_limits(motor);

    switch (motor->input_mode) {
    case INPUT_MODE_INACTIVE:
        motor->ctrl_state.vel_set = 0.0f;
        motor->ctrl_state.iq_set  = 0.0f;
        motor->ctrl_state.pos_set  = motor->meas_data.out_pos_rad;
        break;

    case INPUT_MODE_PASSTHROUGH:
        if (motor->ctrl_mode == CTRL_MODE_CURR) {
            motor->ctrl_state.iq_set = motor->ctrl_state.target_iq;
        } else if (motor->ctrl_mode == CTRL_MODE_VEL) {
            motor->ctrl_state.vel_set = motor->ctrl_state.target_vel;
            motor->ctrl_config.vel_pid.config.max_output     = motor->ctrl_state.dyn_iq_limit;
            motor->ctrl_config.vel_pid.config.integral_limit = motor->ctrl_state.dyn_iq_limit;
        } else if (motor->ctrl_mode == CTRL_MODE_POS) {
            motor->ctrl_state.pos_set = motor->ctrl_state.target_pos;
            motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.dyn_vel_limit;
            motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.dyn_vel_limit;
        }
        break;

    case INPUT_MODE_VEL_RAMP: {
        float accel = (motor->ctrl_state.target_vel > motor->ctrl_state.vel_set)
                          ? motor->ctrl_config.ramp_accel
                          : motor->ctrl_config.ramp_decel;
        motor->ctrl_state.vel_set =
            foc_ramp_linear(motor->ctrl_state.vel_set, motor->ctrl_state.target_vel, accel, DT_TRAJ_TASK);
        break;
    }

    case INPUT_MODE_TORQUE_RAMP:
        motor->ctrl_state.iq_set = foc_ramp_linear(motor->ctrl_state.iq_set,
                                                   motor->ctrl_state.target_iq,
                                                   motor->ctrl_config.ramp_accel,
                                                   DT_TRAJ_TASK);
        break;

    case INPUT_MODE_TRAP_TRAJ:
        if (traj_trap_step_update(&motor->traj, DT_TRAJ_TASK) == 0) {
            motor->ctrl_state.pos_set = motor->traj.step.pos;
            motor->ctrl_state.vel_ff  = motor->traj.step.vel;
        }
        break;

    case INPUT_MODE_POS_FILTER:
    case INPUT_MODE_VEL_FILTER:
    case INPUT_MODE_TUNING:
        break;

    default:
        motor->ctrl_state.vel_set = 0.0f;
        motor->ctrl_state.iq_set  = 0.0f;
        break;
    }
}

void foc_cmd_passthrough(struct motor_dev  *motor,
                         enum foc_ctrl_mode mode,
                         float              target,
                         float              limit)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode  = mode;
    motor->input_mode = INPUT_MODE_PASSTHROUGH;

    switch (mode) {
    case CTRL_MODE_CURR:
        motor->ctrl_state.target_iq = FOC_SATURATE(target, motor->ctrl_state.iq_max);
        motor->ctrl_state.target_id = 0.0f;
        break;

    case CTRL_MODE_VEL:
        motor->ctrl_state.target_vel = FOC_SATURATE(target, motor->ctrl_state.vel_max);
        motor->ctrl_state.dyn_iq_limit =
            (limit > 0.0f) ? FOC_MIN(limit, motor->ctrl_state.iq_max) : motor->ctrl_state.iq_max;
        break;

    case CTRL_MODE_POS:
        motor->ctrl_state.target_pos = target;
        motor->ctrl_state.dyn_vel_limit =
            (limit > 0.0f) ? FOC_MIN(limit, motor->ctrl_state.vel_max) : motor->ctrl_state.vel_max;
        break;

    default:
        break;
    }
}

void foc_cmd_vel_ramp(struct motor_dev *motor, float new_vel, float max_acc)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode             = CTRL_MODE_VEL;
    motor->input_mode            = INPUT_MODE_VEL_RAMP;
    motor->ctrl_state.target_vel = FOC_SATURATE(new_vel, motor->ctrl_state.vel_max);

    if (max_acc > 0.0f) {
        motor->ctrl_config.ramp_accel = max_acc;
    }
}

void foc_cmd_torque_ramp(struct motor_dev *motor, float new_torque, float max_acc)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_mode  = CTRL_MODE_CURR;
    motor->input_mode = INPUT_MODE_TORQUE_RAMP;

    float torque_motor = new_torque / (motor->hw_config.gear_ratio * motor->hw_config.gear_eff);
    float target_iq    = torque_motor * motor->calc_params.inv_kt;
    motor->ctrl_state.target_iq = FOC_SATURATE(target_iq, motor->ctrl_state.iq_max);

    if (max_acc > 0.0f) {
        motor->ctrl_config.ramp_accel = max_acc * motor->calc_params.inv_kt;
    }
}

void foc_cmd_pos_traj(struct motor_dev *motor,
                      float             new_pos_rad,
                      float             max_vel,
                      float             max_acc,
                      float             max_decel)
{
    if (motor == NULL) {
        return;
    }

    if ((motor->ctrl_mode == CTRL_MODE_POS) && (motor->input_mode == INPUT_MODE_TRAP_TRAJ) &&
        (motor->ctrl_state.target_pos == new_pos_rad)) {
        return;
    }

    motor->ctrl_mode             = CTRL_MODE_POS;
    motor->input_mode            = INPUT_MODE_TRAP_TRAJ;
    motor->ctrl_state.target_pos = new_pos_rad;

    struct traj_trap_config cfg = motor->traj.config;
    bool                    changed = false;

    if (max_vel > 0.0f) {
        cfg.max_vel = max_vel;
        changed     = true;
    }
    if (max_acc > 0.0f) {
        cfg.max_acc = max_acc;
        changed     = true;
    }
    if (max_decel > 0.0f) {
        cfg.max_dec = max_decel;
        changed     = true;
    }

    if (changed) {
        traj_trap_config_set(&motor->traj, &cfg);
    }

    float start_pos = motor->traj.is_active ? motor->traj.step.pos : motor->meas_data.out_pos_rad;
    float start_vel = motor->traj.is_active ? motor->traj.step.vel : motor->meas_data.out_vel_rads;

    traj_trap_plan(&motor->traj, new_pos_rad, start_pos, start_vel);
}

void foc_cmd_voltage_set(struct motor_dev *motor, float vd_set, float vq_set, float we_ramp)
{
    if (motor == NULL) {
        return;
    }

    /* Snapshot the real electrical angle only when entering voltage mode,
     * so a running open-loop ramp does not jump if the user only changes
     * voltage amplitude later. */
    if (motor->ctrl_mode != CTRL_MODE_VOLT) {
        motor->ctrl_state.theta_sim = motor->meas_data.theta_elec;
    }

    motor->ctrl_mode              = CTRL_MODE_VOLT;
    motor->input_mode             = INPUT_MODE_INACTIVE;
    motor->ctrl_state.vd_set      = vd_set;
    motor->ctrl_state.vq_set      = vq_set;
    motor->ctrl_state.we_volt_ramp = we_ramp;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
