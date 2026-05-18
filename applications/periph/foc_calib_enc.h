/**
 * @file foc_calib.h
 * @brief  FOC Control Utilities & Constants - Calibration
 * @version v1.0.0
 * @date 2025.12.23
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Execute Encoder Calibration Logic
 * 
 * @param  motor  Pointer to the motor instance
 */
void foc_calib_enc(struct motor_dev *motor);

void foc_calib_enc_init(struct motor_dev *motor);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
