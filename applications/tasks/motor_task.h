/**
 * @file    motor_task.h
 * @brief   Motor task public hooks
 * @version v1.2.0
 * @date    2026.05.12
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"
#include "tm_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief Prepare motor hardware and software state before entering CALIB.
 *
 * Called by the FSM enter-callback for `AXIS_STATE_CALIB`.
 *
 * @param  motor  Motor instance pointer.
 */
void motor_calib_prepare_entry(struct motor_dev *motor);

/**
 * @brief Apply the formal post-restore normalization path after motor/sys load.
 *
 * Shared by startup bring-up and shell-triggered restore so both paths refresh
 * derived runtime state and reset transient setpoints in the same way.
 *
 * @param  motor  Motor instance pointer.
 */
void motor_restore_after_param_load(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
