/**
 * @file    motor_task_config.h
 * @brief   Application-owned motor task budget configuration
 * @version v1.0.0
 * @date    2026.05.22
 *
 * Design overview
 * ---------------
 * This header keeps the FalconFoc motor-task stack budgets explicit at the
 * application config layer. It is intentionally macro-only: no runtime state
 * and no task behavior live here.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "config/tm_core_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/* Increased stack budgets for FOC ISR-coupled tasks.  The safe task runs
 * adc_read_ordinary_dma() plus deferred flash save bookkeeping; the motor
 * task dispatches the FSM and calibration completion callbacks. */
#define TM_MOTOR_TASK_STACK_BYTES      (512U * 4U)
#define TM_MOTOR_SAFE_TASK_STACK_BYTES (256U * 4U)

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
