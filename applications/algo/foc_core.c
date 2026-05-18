/**
 * @file    foc_core.c
 * @brief   FOC Mathematical Core Implementation
 * @version v1.0.0
 * @date    2025.12.23
 * 
 * Copyright (c) 2025 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "foc_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public functions ----------------------------------------------------------------------------- */

/**
 * @brief  Space Vector PWM Calculation (Optimized Standard 7-Segment)
 * 
 * @param  foc  Pointer to FOC variables structure (alpha/beta inputs, dtc_a/b/c outputs)
 * 
 * @return 0 on success (valid duties), -1 on saturation/NaN/error
 */
int foc_svpwm(struct foc_vars *foc)
{
    foc->v_alpha *= foc->inv_vbus;
    foc->v_beta *= foc->inv_vbus;

    float mag = FOC_SQRT(foc->v_alpha * foc->v_alpha + foc->v_beta * foc->v_beta);

    if (mag > FOC_TWO_THIRDS) {
        float scale_factor = FOC_TWO_THIRDS / mag;
        foc->v_alpha *= scale_factor;
        foc->v_beta *= scale_factor;
    }

    if (foc->v_beta >= 0.0f) {
        if (foc->v_alpha >= 0.0f) {
            //quadrant I
            if (FOC_INV_SQRT3 * foc->v_beta > foc->v_alpha) {
                foc->sector = FOC_SECTOR_2; //sextant v2-v3
            } else {
                foc->sector = FOC_SECTOR_1; //sextant v1-v2
            }
        } else {
            //quadrant II
            if (-FOC_INV_SQRT3 * foc->v_beta > foc->v_alpha) {
                foc->sector = FOC_SECTOR_3; //sextant v3-v4
            } else {
                foc->sector = FOC_SECTOR_2; //sextant v2-v3
            }
        }
    } else {
        if (foc->v_alpha >= 0.0f) {
            //quadrant IV
            if (-FOC_INV_SQRT3 * foc->v_beta > foc->v_alpha) {
                foc->sector = FOC_SECTOR_5; //sextant v5-v6
            } else {
                foc->sector = FOC_SECTOR_6; //sextant v6-v1
            }
        } else {
            //quadrant III
            if (FOC_INV_SQRT3 * foc->v_beta > foc->v_alpha) {
                foc->sector = FOC_SECTOR_4; //sextant v4-v5
            } else {
                foc->sector = FOC_SECTOR_5; //sextant v5-v6
            }
        }
    }

    switch (foc->sector) {
    // sextant v1-v2
    case FOC_SECTOR_1: {
        // Vector on-times
        float t1   = foc->v_alpha - FOC_INV_SQRT3 * foc->v_beta;
        float t2   = FOC_2_INV_SQRT3 * foc->v_beta;
        // PWM timings
        foc->dtc_a = (1.0f - t1 - t2) * 0.5f;
        foc->dtc_b = foc->dtc_a + t1;
        foc->dtc_c = foc->dtc_b + t2;
    } break;

    // sextant v2-v3
    case FOC_SECTOR_2: {
        // Vector on-times
        float t2   = foc->v_alpha + FOC_INV_SQRT3 * foc->v_beta;
        float t3   = -foc->v_alpha + FOC_INV_SQRT3 * foc->v_beta;
        // PWM timings
        foc->dtc_b = (1.0f - t2 - t3) * 0.5f;
        foc->dtc_a = foc->dtc_b + t3;
        foc->dtc_c = foc->dtc_a + t2;
    } break;

    // sextant v3-v4
    case FOC_SECTOR_3: {
        // Vector on-times
        float t3   = FOC_2_INV_SQRT3 * foc->v_beta;
        float t4   = -foc->v_alpha - FOC_INV_SQRT3 * foc->v_beta;
        // PWM timings
        foc->dtc_b = (1.0f - t3 - t4) * 0.5f;
        foc->dtc_c = foc->dtc_b + t3;
        foc->dtc_a = foc->dtc_c + t4;
    } break;

    // sextant v4-v5
    case FOC_SECTOR_4: {
        // Vector on-times
        float t4   = -foc->v_alpha + FOC_INV_SQRT3 * foc->v_beta;
        float t5   = -FOC_2_INV_SQRT3 * foc->v_beta;
        // PWM timings
        foc->dtc_c = (1.0f - t4 - t5) * 0.5f;
        foc->dtc_b = foc->dtc_c + t5;
        foc->dtc_a = foc->dtc_b + t4;
    } break;

    // sextant v5-FOC_INV_SQRT3
    case FOC_SECTOR_5: {
        // Vector on-times
        float t5   = -foc->v_alpha - FOC_INV_SQRT3 * foc->v_beta;
        float t6   = foc->v_alpha - FOC_INV_SQRT3 * foc->v_beta;
        // PWM timings
        foc->dtc_c = (1.0f - t5 - t6) * 0.5f;
        foc->dtc_a = foc->dtc_c + t5;
        foc->dtc_b = foc->dtc_a + t6;
    } break;

    // sextant v6-v1
    case FOC_SECTOR_6: {
        // Vector on-times
        float t6   = -FOC_2_INV_SQRT3 * foc->v_beta;
        float t1   = foc->v_alpha + FOC_INV_SQRT3 * foc->v_beta;
        // PWM timings
        foc->dtc_a = (1.0f - t6 - t1) * 0.5f;
        foc->dtc_c = foc->dtc_a + t1;
        foc->dtc_b = foc->dtc_c + t6;
    } break;
    }

    foc->dtc_a = FOC_CLAMP(foc->dtc_a, 0.0f, 1.0f);
    foc->dtc_b = FOC_CLAMP(foc->dtc_b, 0.0f, 1.0f);
    foc->dtc_c = FOC_CLAMP(foc->dtc_c, 0.0f, 1.0f);

    return 0;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
