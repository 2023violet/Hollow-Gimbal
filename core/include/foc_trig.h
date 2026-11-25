/**
 * @file    foc_trig.h
 * @brief   Bounded Float32 sine/cosine pair without the libm large-argument reduction.
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Design overview
 * ---------------
 * foc_sincosf() reduces the angle to r in about [-pi/4, pi/4] with a three-part
 * pi/2 (Cody-Waite, 12 + 12 + 24 bits) and evaluates degree-9 sine and degree-8
 * cosine minimax polynomials. For |angle| <= 6400 rad the absolute error against
 * a double reference is below 1.5e-7 (unit test test-foc-trig). Larger angles
 * are first wrapped with fmodf() by the Float32 value of 2*pi, which adds about
 * |angle| * 2.8e-8 rad of phase error; at that size the Float32 input itself is
 * coarser than 4e-4 rad. It replaces sinf()/cosf(), whose Payne-Hanek reduction
 * costs about 4.6 KB of flash on the soft-float AT32F415.
 *
 * Usage example
 * -------------
 * float s;
 * float c;
 * foc_sincosf(theta_rad, &s, &c);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Stateless and reentrant; safe in ISR, H0, H1 and Foreground. Defined once in
 * core/src/foc_trig.c, which Core, Calibration and the Board1 image compile.
 *
 * Failure / recovery
 * ------------------
 * A NaN or infinite angle yields NaN for both outputs, as sinf()/cosf() do;
 * callers keep their existing isfinite() checks.
 */
#pragma once

/* Includes --------------------------------------------------------------------- */

/* None. */

/* Public Types ---------------------------------------------------------------- */

/* None: the pair is returned through two caller-owned floats. */

/* Public Functions ------------------------------------------------------------ */

/**
 * @brief Compute sin(angle_rad) and cos(angle_rad) in Float32.
 * @param angle_rad Angle in rad; any value (non-finite gives NaN).
 * @param sine      Receives the sine; must not be NULL.
 * @param cosine    Receives the cosine; must not be NULL.
 */
void foc_sincosf(float angle_rad, float *sine, float *cosine);

/* ---------------------------------------- end of file ----------------------------------------- */
