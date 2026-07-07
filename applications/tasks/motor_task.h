/**
 * @file    motor_task.h
 * @brief   Motor task public entry
 * @version v1.3.0
 * @date    2026.05.22
 *
 * Design overview
 * ---------------
 * This header exposes only the task bring-up entry for the motor owner.
 * Product logic, calibration glue, and runtime policy stay in their own
 * owners.
 *
 * Usage example
 * -------------
 * - call `motor_task_init()` once during the application bring-up chain
 * - the task owner then creates the live motor threads and binds the runtime
 *   seam to the product motor instance
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#ifdef __cplusplus
extern "C" {
#endif

/* Lifecycle ----------------------------------------------------------------------------- */

/**
 * @brief  Bring up the FalconFoc motor task owner and its child threads.
 */
void motor_task_init(void);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
