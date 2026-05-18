/**
 * @file    foc_config.h
 * @brief   Host shim for FalconFoc config constants
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

#define PHASE_ORDER_ABC       (0U)
#define PHASE_ORDER_ACB       (1U)
#define DIR_CW                (0U)
#define DIR_CCW               (1U)
#define ENCODER_LUT_BITS      (7U)
#define ENCODER_LUT_SIZE      (1U << ENCODER_LUT_BITS)
#define CALIB_LUT_SIZE        ENCODER_LUT_SIZE
#define DEFAULT_D_AXIS_BW_RATIO (0.2f)
#define DEFAULT_POS_VEL_BW_RATIO (0.5f)
#define CALIB_K_DELTA         (4.0f)

/* ---------------------------------------- end of file ----------------------------------------- */
