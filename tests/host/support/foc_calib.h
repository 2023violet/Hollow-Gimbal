/**
 * @file    foc_calib.h
 * @brief   Host shim for calibration runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_calibration_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

void foc_calib_init(struct motor_dev *motor);
void foc_pid_init(struct motor_dev *motor);
void foc_lpf_init(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
