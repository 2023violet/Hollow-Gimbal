/**
 * @file foc_calib_enc.h
 * @brief  FOC Control Utilities & Constants - Calibration
 * @version v1.0.0
 * @date 2025.12.23
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
 * @brief  Initialize calibration state
 * 
 * @param  motor  Motor instance pointer
 */
void foc_calib_init(struct motor_dev *motor);

/**
 * @brief  Update current controller gains
 *
 * @param  motor  Motor instance pointer
 */
void foc_curr_gain_calc(struct motor_dev *motor);

/**
 * @brief  Refresh derived constants and runtime limits from the current config.
 *
 * This updates calc_params and runtime ceilings only. It does not rewrite the
 * caller-owned PID Kp/Ki/Kd tuning fields.
 *
 * @param  motor  Motor instance pointer.
 */
void foc_calib_runtime_refresh(struct motor_dev *motor);

/**
 * @brief  Calculate velocity controller gains
 *
 * @param  motor  Motor instance pointer
 */
void foc_vel_gain_calc(struct motor_dev *motor);

/**
 * @brief  Calculate position controller gains
 */
void foc_pos_gain_calc(struct motor_dev *motor);

/**
 * @brief  Calibrate current offsets (IA/IB/IC)
 *
 * @param  motor  Motor instance pointer
 */
void foc_calib_curr(struct motor_dev *motor);

/**
 * @brief  Calibration Loop
 *
 * @param  motor  Motor instance pointer
 */
void foc_calib_loop(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
