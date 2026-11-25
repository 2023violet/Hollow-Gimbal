/**
 * @file    foc_trig.c
 * @brief   Bounded Float32 sine/cosine pair.
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Implementation role
 * -------------------
 * - the single out-of-line definition of foc_sincosf() declared in foc_trig.h
 * - compiled into Core, Calibration and the Board1 image; one copy per image
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_trig.h"

#include <math.h>
#include <stdint.h>

/* Private Helpers -------------------------------------------------------------- */

/* None: the reduction and polynomials are inline in foc_sincosf(). */

/* Public Functions ------------------------------------------------------------- */

void foc_sincosf(float angle_rad, float *sine, float *cosine)
{
    /* pi/2 = PIO2_HI + PIO2_MID + PIO2_LO. HI and MID carry 12 significant bits, so k * HI and
     * k * MID are exact for |k| <= 4096, which |x| <= 6400 rad guarantees. */
    const float two_over_pi = 0.636619772f;
    const float pio2_hi = 1.5703125f;
    const float pio2_mid = 4.83751297e-04f;
    const float pio2_lo = 7.54979013e-08f;
    float x = angle_rad;

    if (!isfinite(x)) {
        *sine = x - x;
        *cosine = x - x;
        return;
    }
    if (!(fabsf(x) <= 6400.0f)) {
        x = fmodf(x, 6.28318530717958647692f);
    }
    const int32_t k = (int32_t)(x * two_over_pi + (x >= 0.0f ? 0.5f : -0.5f));
    const float kf = (float)k;
    const float r = ((x - kf * pio2_hi) - kf * pio2_mid) - kf * pio2_lo;
    const float z = r * r;
    /* Minimax on |r| <= pi/4: sin(r) = r + r^3 * P(r^2), cos(r) = 1 - r^2/2 + r^4 * Q(r^2). */
    const float p =
        -1.66666672e-01f + z * (8.33332818e-03f + z * (-1.98390437e-04f + z * 2.71601402e-06f));
    const float q = 4.16666456e-02f + z * (-1.38873677e-03f + z * 2.44384519e-05f);
    const float s = r + (r * z) * p;
    const float c = (1.0f - 0.5f * z) + (z * z) * q;

    switch ((uint32_t)k & 3U) {
    case 0U:
        *sine = s;
        *cosine = c;
        break;
    case 1U:
        *sine = c;
        *cosine = -s;
        break;
    case 2U:
        *sine = -s;
        *cosine = -c;
        break;
    default:
        *sine = -c;
        *cosine = s;
        break;
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
