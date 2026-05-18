/**
 * @file    foc_calib.h
 * @brief   Host shim for motor_fsm unit coverage
 * @version v1.0.0
 * @date    2026.05.12
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

void foc_curr_gain_calc(struct motor_dev *motor);
void foc_vel_gain_calc(struct motor_dev *motor);
void foc_pos_gain_calc(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
