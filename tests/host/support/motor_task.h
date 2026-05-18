/**
 * @file    motor_task.h
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

void motor_calib_prepare_entry(struct motor_dev *motor);
void motor_restore_after_param_load(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
