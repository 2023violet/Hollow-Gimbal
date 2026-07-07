/**
 * @file    foc_calib.h
 * @brief   FOC hardware-coupled calibration engine entry points
 * @version v1.0.0
 * @date    2025.12.23
 *
 * Design overview
 * ---------------
 * This header owns the hardware-coupled calibration engines for current
 * offsets and motor-parameter calibration steps. Runtime policy and post-step
 * product behavior stay outside this module.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize calibration state owned by the hardware-coupled engine.
 */
void foc_calib_init(struct motor_dev *motor);

/**
 * @brief  Measure and apply ADC current offsets before closed-loop control.
 */
void foc_calib_curr(struct motor_dev *motor);

/**
 * @brief  Advance the active motor calibration step machine.
 */
void foc_calib_loop(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
