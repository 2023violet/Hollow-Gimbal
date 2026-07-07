/**
 * @file    foc_ctrl.h
 * @brief   FOC hardware-coupled control engine entry points
 * @version v1.3.0
 * @date    2026.01.17
 *
 * Design overview
 * ---------------
 * This header owns the hardware-coupled FOC control engine: measurement
 * preparation, direct-drive helpers, and ISR-owned inner/outer loop steps.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Update derived measurements and dynamic per-tick control limits.
 */
void foc_para_calc(struct motor_dev *motor);

/**
 * @brief  Drive the motor in direct voltage mode at one electrical angle.
 */
void foc_ctrl_volt(struct motor_dev *motor, float vd_set, float vq_set, float pos);

/**
 * @brief  Drive the motor with direct d/q current setpoints.
 */
void foc_ctrl_curr(struct motor_dev *motor, float id_set, float iq_set, float pos);

/**
 * @brief  Execute one velocity-mode direct-drive update.
 */
void foc_ctrl_vel(struct motor_dev *motor, float vel_set, float iq_set, float pos);

/**
 * @brief  Execute one position-mode direct-drive update.
 */
void foc_ctrl_pos(struct motor_dev *motor, float pos_set, float vel_set, float iq_set, float pos);

/**
 * @brief  Run the high-frequency current loop from ADC ISR context.
 */
void foc_current_loop_step(struct motor_dev *motor);

/**
 * @brief  Run the outer velocity/position loop from timer ISR context.
 */
void foc_outer_loop_step(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
