/**
 * @file    foc_motion_runtime.h
 * @brief   FalconFoc motion-command runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 *
 * Design overview
 * ---------------
 * This header owns the runtime-side motion command glue. It consumes product
 * targets, updates the active input mode, and keeps trajectory planning in the
 * runtime owner instead of shell or periph.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime Step -------------------------------------------------------------------------- */

/**
 * @brief  Advance one RTOS-task motion step for the active input mode.
 */
void foc_process_input_task(struct motor_dev *motor);

/* Motion Request Entry Points ------------------------------------------------------------ */

/**
 * @brief  Apply one direct passthrough target in the selected control mode.
 */
void foc_cmd_passthrough(struct motor_dev *motor,
                         enum foc_ctrl_mode mode,
                         float              target,
                         float              limit);

/**
 * @brief  Apply one velocity-ramp target owned by the runtime layer.
 */
void foc_cmd_vel_ramp(struct motor_dev *motor, float new_vel, float max_acc);

/**
 * @brief  Apply one torque-ramp target owned by the runtime layer.
 */
void foc_cmd_torque_ramp(struct motor_dev *motor, float new_torque, float max_acc);

/**
 * @brief  Plan or update one trapezoidal position trajectory.
 */
void foc_cmd_pos_traj(struct motor_dev *motor,
                      float             new_pos_rad,
                      float             max_vel,
                      float             max_acc,
                      float             max_decel);

/**
 * @brief  Apply one experimental direct voltage injection owned by runtime.
 */
void foc_cmd_voltage_set(struct motor_dev *motor, float vd_set, float vq_set, float we_ramp);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
