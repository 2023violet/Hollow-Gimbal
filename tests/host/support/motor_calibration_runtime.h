/**
 * @file    motor_calibration_runtime.h
 * @brief   Host shim for calibration runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void foc_calib_runtime_refresh(struct motor_dev *motor);
void foc_curr_gain_calc(struct motor_dev *motor);
void foc_vel_gain_calc(struct motor_dev *motor);
void foc_pos_gain_calc(struct motor_dev *motor);

void motor_calibration_runtime_after_current_offsets(struct motor_dev *motor);
void motor_calibration_runtime_after_flux_complete(struct motor_dev *motor);
void motor_calibration_runtime_after_js_complete(struct motor_dev *motor);
void motor_calibration_runtime_request_encoder_save(struct motor_dev *motor);
int  motor_calibration_runtime_restore_encoder_after_abort(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

