/**
 * @file    foc_restore.c
 * @brief   FalconFoc parameter-restore finalization helpers
 * @version v1.0.0
 * @date    2026.05.21
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_restore.h"
#include "motor_calibration_runtime.h"
#include "traj_trap.h"

#ifdef __cplusplus
extern "C" {
#endif

void foc_restore_finalize(struct motor_dev *motor)
{
    struct traj_trap_config traj_cfg;

    if (motor == NULL) {
        return;
    }

    foc_calib_runtime_refresh(motor);

    traj_cfg.max_vel = motor->ctrl_config.traj_vel;
    traj_cfg.max_acc = motor->ctrl_config.traj_accel;
    traj_cfg.max_dec = motor->ctrl_config.traj_decel;
    (void)traj_trap_config_set(&motor->traj, &traj_cfg);

    motor->ctrl_state.dyn_iq_limit  = motor->ctrl_state.iq_max;
    motor->ctrl_state.dyn_vel_limit = motor->ctrl_state.vel_max;
}

void foc_restore_clear_transient_setpoints(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_state.target_vel = 0.0f;
    motor->ctrl_state.target_iq  = 0.0f;
    motor->ctrl_state.target_id  = 0.0f;
    motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
    motor->ctrl_state.target_tor = 0.0f;

    motor->ctrl_state.vel_set = 0.0f;
    motor->ctrl_state.iq_set  = 0.0f;
    motor->ctrl_state.id_set  = 0.0f;
    motor->ctrl_state.pos_set = motor->meas_data.out_pos_rad;
    motor->ctrl_state.vel_ff  = 0.0f;
    motor->ctrl_state.iq_ff   = 0.0f;

    motor->ctrl_state.dyn_iq_limit  = motor->ctrl_state.iq_max;
    motor->ctrl_state.dyn_vel_limit = motor->ctrl_state.vel_max;

    motor->ctrl_config.pos_pid.config.max_output     = motor->ctrl_state.vel_max;
    motor->ctrl_config.pos_pid.config.integral_limit = motor->ctrl_state.vel_max;
    motor->ctrl_config.vel_pid.config.max_output     = motor->ctrl_state.iq_max;
    motor->ctrl_config.vel_pid.config.integral_limit = motor->ctrl_state.iq_max;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
