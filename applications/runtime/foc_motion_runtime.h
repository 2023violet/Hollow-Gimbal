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
#include <stdbool.h>

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

/**
 * @brief  Apply one I-f open-loop spin command: closed-loop current amplitude
 *         following the ramped simulated electrical angle.
 */
void foc_cmd_voltage_if(struct motor_dev *motor, float iq_amp, float we);

/**
 * @brief  Bumpless handoff from the running I-f open loop to closed-loop
 *         velocity control.  Valid only while the axis is RUNNING in
 *         CTRL_MODE_VOLT with the I-f path enabled; the velocity PID
 *         integral is seeded with the I-f torque so the shaft torque is
 *         continuous across the switch.
 *
 * @param  motor   Motor instance.
 * @param  vel_rpm Closed-loop velocity target (rpm, output shaft).
 *
 * @return true when the handoff was performed, false when the preconditions
 *         (running + I-f) were not met.
 */
bool foc_cmd_if_to_vel_handoff(struct motor_dev *motor, float vel_rpm);

#if defined(TM_BENCH_DIAG) && (TM_BENCH_DIAG != 0U)
/**
 * @brief  Update the closed-loop velocity target after a live I-f handoff.
 *
 * This diagnostic-only entry point accepts only a running velocity loop that
 * was entered through foc_cmd_if_to_vel_handoff().  It changes target_vel
 * only, preserving the handoff's PID state and current limit.
 */
bool foc_cmd_bench_velocity_step(struct motor_dev *motor, float vel_rpm);
#endif

/**
 * @brief  Apply a static voltage vector pinned to stator phase axis
 *         `phase` (0/1/2), independent of any encoder information.
 *         Diagnostic anchor for electrical-zero verification.
 */
void foc_cmd_phase_volt(struct motor_dev *motor, int phase, float volt);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
