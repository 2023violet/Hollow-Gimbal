/**
 * @file    foc_core.h
 * @brief   FOC Core Data Structures (Runtime)
 * @version v1.0.0
 * @date    2025.12.23
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>
#include "foc_math.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Constants ------------------------------------------------------------------------------------ */
/* SVPWM Sector Definitions */
#define FOC_SECTOR_1 (1U) /* Sextant v1-v2 */
#define FOC_SECTOR_2 (2U) /* Sextant v2-v3 */
#define FOC_SECTOR_3 (3U) /* Sextant v3-v4 */
#define FOC_SECTOR_4 (4U) /* Sextant v4-v5 */
#define FOC_SECTOR_5 (5U) /* Sextant v5-v6 */
#define FOC_SECTOR_6 (6U) /* Sextant v6-v1 */

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief FOC Runtime Variables
 */
struct foc_vars {
    /* 1. Inputs (Sampled & Normalized) */
    float v_bus;    /* Bus Voltage (V) */
    float inv_vbus; /* 1.0 / v_bus (Cached) */

    float i_a; /* Phase A Current (A) */
    float i_b; /* Phase B Current (A) */
    float i_c; /* Phase C Current (A) */

    float v_a; /* Phase A Voltage (V) */
    float v_b; /* Phase B Voltage (V) */
    float v_c; /* Phase C Voltage (V) */

    /* 2. Angle State */
    float theta;   /* Electrical Angle (rad) */
    float sin_val; /* sin(theta) */
    float cos_val; /* cos(theta) */

    /* 3. Clarke Transform (3ph -> 2ph Stationary) */
    float i_alpha;
    float i_beta;

    /* 4. Park Transform (Stationary -> Rotating) */
    float i_d; /* Flux Current (Feedback) */
    float i_q; /* Torque Current (Feedback) */

    float i_d_filter; /* Flux Current (Filtered) */
    float i_q_filter; /* Torque Current (Filtered) */

    /* 5. Control Inputs (From PID/OpenLoop) */
    float v_d; /* D-Axis Voltage Command (V) */
    float v_q; /* Q-Axis Voltage Command (V) */

    /* 6. Inverse Park (Rotating -> Stationary) */
    float v_alpha;
    float v_beta;

    /* 7. SVPWM Outputs (0.0f - 1.0f) */
    float   dtc_a;  /* Duty Cycle A */
    float   dtc_b;  /* Duty Cycle B */
    float   dtc_c;  /* Duty Cycle C */
    uint8_t sector; /* Current SVPWM Sector (1-6) */
};

/* Public function prototypes ------------------------------------------------------------------- */

/**
 * @brief  Calculate sine and cosine values for given theta
 * 
 * @param  foc  Pointer to FOC variables structure
 */
static inline void sin_cos_val(struct foc_vars *foc)
{
    // foc->sin_val = FOC_SIN(foc->theta);
    // foc->cos_val = FOC_COS(foc->theta);
    foc_fast_sincos(foc->theta, &foc->sin_val, &foc->cos_val);
}

/**
 * @brief  Clarke Transform (Ia, Ib, Ic -> Ialpha, Ibeta)
 * 
 * @param  foc  Pointer to FOC variables structure
 */
static inline void foc_clarke(struct foc_vars *foc)
{
    foc->i_alpha = foc->i_a;
    foc->i_beta  = (foc->i_b - foc->i_c) * FOC_INV_SQRT3;
}

/**
 * @brief  Inverse Clarke Transform (Ialpha, Ibeta -> Ia, Ib, Ic)
 * 
 * @param  foc  Pointer to FOC variables structure
 */
static inline void foc_inv_clarke(struct foc_vars *foc)
{
    foc->v_a = foc->v_alpha;
    foc->v_b = -0.5f * foc->v_alpha + FOC_SQRT3_OVER2 * foc->v_beta;
    foc->v_c = -0.5f * foc->v_alpha - FOC_SQRT3_OVER2 * foc->v_beta;
}

/**
 * @brief  Park Transform (Stationary -> Rotating)
 * 
 * @param  foc  Pointer to FOC variables structure
 */
static inline void foc_park(struct foc_vars *foc)
{
    foc->i_d = foc->i_alpha * foc->cos_val + foc->i_beta * foc->sin_val;
    foc->i_q = foc->i_beta * foc->cos_val - foc->i_alpha * foc->sin_val;
}

/**
 * @brief  Inverse Park Transform (Rotating -> Stationary)
 * 
 * @param  foc  Pointer to FOC variables structure
 */
static inline void foc_inv_park(struct foc_vars *foc)
{
    foc->v_alpha = (foc->v_d * foc->cos_val - foc->v_q * foc->sin_val);
    foc->v_beta  = (foc->v_d * foc->sin_val + foc->v_q * foc->cos_val);
}

/**
 * @brief  Space Vector PWM Calculation
 * 
 * @param  foc  Pointer to FOC variables structure
 * 
 * @return 0 on success (valid duty cycles), -1 on error (saturation/NaN)
 */
int foc_svpwm(struct foc_vars *foc);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
