/**
 * @file    foc_calibration_math.c
 * @brief   Double helpers that avoid the libm double sqrt/hypot/fmax/fmin.
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Implementation role
 * -------------------
 * - the single out-of-line definitions declared in foc_calibration_math.h
 * - shared by the motor, flux and mechanical identification fits
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_calibration_math.h"

#include <math.h>

/* Private Helpers -------------------------------------------------------------- */

/* None. */

/* Public Functions ------------------------------------------------------------- */

double foc_calibration_sqrt(double value)
{
    if (isnan(value)) {
        return value;
    }
    if (value < 0.0) {
        return (double)NAN;
    }
    if (value == 0.0 || isinf(value)) {
        return value;
    }
    /* Keep the Float32 seed in range; scaling by an even power of two is exact. */
    if (value > 1.0e30) {
        return 0x1p32 * foc_calibration_sqrt(value * 0x1p-64);
    }
    if (value < 1.0e-30) {
        return 0x1p-32 * foc_calibration_sqrt(value * 0x1p64);
    }
    double root = (double)sqrtf((float)value);
    root = 0.5 * (root + value / root);
    root = 0.5 * (root + value / root);
    return root;
}

double foc_calibration_fmax(double a, double b)
{
    if (isnan(a)) {
        return b;
    }
    if (isnan(b)) {
        return a;
    }
    return a > b ? a : b;
}

double foc_calibration_fmin(double a, double b)
{
    if (isnan(a)) {
        return b;
    }
    if (isnan(b)) {
        return a;
    }
    return a < b ? a : b;
}

/* ---------------------------------------- end of file ----------------------------------------- */
