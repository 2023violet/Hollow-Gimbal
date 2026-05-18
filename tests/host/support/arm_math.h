/**
 * @file    arm_math.h
 * @brief   Minimal host shim for CMSIS-DSP trigonometric helpers
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <math.h>

static inline float arm_sin_f32(float x)
{
    return sinf(x);
}

static inline float arm_cos_f32(float x)
{
    return cosf(x);
}

/* ---------------------------------------- end of file ----------------------------------------- */
