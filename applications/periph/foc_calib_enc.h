/**
 * @file    foc_calib_enc.h
 * @brief   FOC hardware-coupled encoder calibration entry points
 * @version v1.0.0
 * @date    2026.05.22
 *
 * Design overview
 * ---------------
 * This header owns the encoder-calibration engine entry points used by the
 * hardware-coupled calibration path. Runtime save policy stays outside this
 * module.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the encoder calibration state machine.
 */
void foc_calib_enc_init(struct motor_dev *motor);

/**
 * @brief  Advance the encoder calibration state machine.
 */
void foc_calib_enc(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
