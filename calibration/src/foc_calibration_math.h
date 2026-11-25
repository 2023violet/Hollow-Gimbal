/**
 * @file    foc_calibration_math.h
 * @brief   Private double helpers that avoid the libm double sqrt/hypot/fmax/fmin.
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Design overview
 * ---------------
 * The identification fits keep their double accumulators. Only the final
 * square roots and clamps used libm double functions, which cost about 1.3 KB
 * of flash on the soft-float AT32F415. foc_calibration_sqrt() starts from the
 * Float32 sqrtf() and applies two Newton steps in double, which restores
 * double precision (relative error near 1e-16). The min/max helpers follow
 * C fmin()/fmax(): a NaN operand yields the other operand.
 *
 * Usage example
 * -------------
 * const double rms = foc_calibration_sqrt(sum_square / count);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Stateless, private to calibration/src; defined in foc_calibration_math.c.
 *
 * Failure / recovery
 * ------------------
 * A negative input or NaN yields NaN, +inf yields +inf and +/-0 is returned
 * unchanged, as sqrt() does; callers keep their isfinite() checks.
 */
#pragma once

/* Includes --------------------------------------------------------------------- */

/* None. */

/* Public Types ---------------------------------------------------------------- */

/* None: plain double values. */

/* Public Functions ------------------------------------------------------------ */

/** @brief Double square root through sqrtf() plus two Newton steps. */
double foc_calibration_sqrt(double value);

/** @brief fmax() semantics without libm: a NaN operand yields the other operand. */
double foc_calibration_fmax(double a, double b);

/** @brief fmin() semantics without libm: a NaN operand yields the other operand. */
double foc_calibration_fmin(double a, double b);

/* ---------------------------------------- end of file ----------------------------------------- */
