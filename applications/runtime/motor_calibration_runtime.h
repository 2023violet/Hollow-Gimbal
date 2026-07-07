/**
 * @file    motor_calibration_runtime.h
 * @brief   FalconFoc calibration runtime owner
 * @version v1.0.0
 * @date    2026.05.21
 *
 * Design overview
 * ---------------
 * This header owns the runtime-side glue that follows calibration steps:
 * gain refresh, post-calibration rebinding, deferred save requests, and
 * abort-time restore helpers.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "motor_runtime.h"
#include "motor_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Gain / Derived-State Refresh ---------------------------------------------------------- */

/**
 * @brief  Refresh derived runtime quantities after calibration or restore changes.
 */
void foc_calib_runtime_refresh(struct motor_dev *motor);

/**
 * @brief  Recompute current-loop gains from the current motor configuration.
 */
void foc_curr_gain_calc(struct motor_dev *motor);

/**
 * @brief  Recompute velocity-loop gains from the current motor configuration.
 */
void foc_vel_gain_calc(struct motor_dev *motor);

/**
 * @brief  Recompute position-loop gains from the current motor configuration.
 */
void foc_pos_gain_calc(struct motor_dev *motor);

/* Calibration Lifecycle Glue ------------------------------------------------------------ */

/**
 * @brief  Rebind runtime state after current-offset calibration completes.
 */
void motor_calibration_runtime_after_current_offsets(struct motor_dev *motor);

/**
 * @brief  Rebind runtime state after flux calibration completes.
 */
void motor_calibration_runtime_after_flux_complete(struct motor_dev *motor);

/**
 * @brief  Rebind runtime state after inertia calibration completes.
 */
void motor_calibration_runtime_after_js_complete(struct motor_dev *motor);

/**
 * @brief  Queue one deferred encoder-save request after a successful calibration.
 */
void motor_calibration_runtime_request_encoder_save(struct motor_dev *motor);

/**
 * @brief  Restore encoder runtime state after a calibration abort path.
 *
 * @return `0` on success, or negative `-errno` style status on failure.
 */
int  motor_calibration_runtime_restore_encoder_after_abort(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
