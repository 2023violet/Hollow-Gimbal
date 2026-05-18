/**
 * @file    foc_ctrl.h
 * @brief   FOC Control Utilities & Constants - Control (Decoupled)
 * @version v1.3.0
 * @date    2026.01.17
 *
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Check motor faults
 */
void foc_fault_check(struct motor_dev *motor);

/**
 * @brief  Calculate FOC parameters
 */
void foc_para_calc(struct motor_dev *motor);

/**
 * @brief  FOC Voltage Control Mode
 */
void foc_ctrl_volt(struct motor_dev *motor, float vd_set, float vq_set, float pos);

/**
 * @brief  FOC Current Control Mode
 */
void foc_ctrl_curr(struct motor_dev *motor, float id_set, float iq_set, float pos);

/**
 * @brief  FOC Velocity Control Mode
 */
void foc_ctrl_vel(struct motor_dev *motor, float vel_set, float iq_set, float pos);

/**
 * @brief  FOC Position Control Mode
 */
void foc_ctrl_pos(struct motor_dev *motor, float pos_set, float vel_set, float iq_set, float pos);

/**
 * @brief  High-frequency current loop step (ADC ISR, 20kHz)
 */
void foc_current_loop_step(struct motor_dev *motor);

/**
 * @brief  Outer loop step (TIM ISR, 5khz)
 */
void foc_outer_loop_step(struct motor_dev *motor);

/**
 * @brief  Input processing and trajectory task (RTOS Task, 1kHz)
 */
void foc_process_input_task(struct motor_dev *motor);

/**
 * @brief  Command position move with full trajectory
 */
void foc_cmd_pos_traj(struct motor_dev *motor,
                      float             new_pos,
                      float             max_vel,
                      float             max_acc,
                      float             max_decel);

/**
 * @brief  Command velocity move with ramp
 */
void foc_cmd_vel_ramp(struct motor_dev *motor, float new_vel, float max_acc);

void foc_cmd_torque_ramp(struct motor_dev *motor, float new_torque, float max_acc);

/**
 * @brief  Command pass through mode
 */
void foc_cmd_passthrough(struct motor_dev  *motor,
                         enum foc_ctrl_mode mode,
                         float              target,
                         float              limit);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
