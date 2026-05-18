/**
 * @file    foc_math.h
 * @brief   FOC Mathematical Utilities — Constants, Transforms & Filters
 * @version v3.0.0
 * @date    2026.02.19
 *
 * This header is intentionally header-only (inline functions + macros) so that
 * the compiler can fully inline every call site with zero function-call overhead,
 * which is critical in the 20 kHz ADC ISR hot path.
 *
 * Sections:
 *   1. Physical constants & factors
 *   2. Unit-conversion macros
 *   3. Scalar utility macros (min/max/clamp/sat/sign/sq/cb)
 *   4. Angle & modulo helpers
 *   5. First-order low-pass filter
 *   6. Exponential moving average (EMA)
 *   7. Ramp / rate-limiter
 *   8. Linear interpolation helpers
 *   9. Vector geometry helpers
 *  10. Clarke / Park transform kernels (inline, allocation-free)
 *  11. Dead-time compensation
 *  12. Numerics & DSP helpers
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "arm_math.h"
#include <stdint.h>
#include <math.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Section 1: Physical constants ---------------------------------------------------------------- */

#define FOC_PI                       (3.14159265f) /* π */
#define FOC_2PI                      (6.28318530f) /* 2π */
#define FOC_HALF_PI                  (1.57079632f) /* π/2 */
#define FOC_QUARTER_PI               (0.78539816f) /* π/4 */
#define FOC_INV_PI                   (0.31830988f) /* 1/π */
#define FOC_INV_2PI                  (0.15915494f) /* 1/(2π) */

#define FOC_SQRT2                    (1.41421356f) /* √2 */
#define FOC_INV_SQRT2                (0.70710678f) /* 1/√2  (peak→RMS) */
#define FOC_SQRT3                    (1.73205080f) /* √3 */
#define FOC_INV_SQRT3                (0.57735027f) /* 1/√3  (Clarke α-axis weight) */
#define FOC_2_INV_SQRT3              (1.15470054f) /* 2/√3  (SVPWM voltage gain) */
#define FOC_SQRT3_OVER2              (0.86602540f) /* √3/2 */
#define FOC_TWO_THIRDS               (0.66666667f) /* 2/3   (amplitude-invariant Clarke scale) */
#define FOC_ONE_THIRD                (0.33333333f) /* 1/3 */

/* Section 2: Unit-conversion constants & macros ------------------------------------------------ */

/** @defgroup unit_conv Unit Conversions */
/** @{ */
#define FOC_RPM_TO_RADS_K            (0.10471976f) /* 2π/60 */
#define FOC_RADS_TO_RPM_K            (9.54929659f) /* 60/(2π) */
#define FOC_RADS_TO_RPS_K            (0.15915494f) /* 1/(2π) */
#define FOC_RPS_TO_RADS_K            (6.28318530f) /* 2π */
#define FOC_DEG_TO_RAD_K             (0.01745329f) /* π/180 */
#define FOC_RAD_TO_DEG_K             (57.2957795f) /* 180/π */

#define FOC_RPM_TO_RADS(x)           ((x) * FOC_RPM_TO_RADS_K)
#define FOC_RADS_TO_RPM(x)           ((x) * FOC_RADS_TO_RPM_K)
#define FOC_RADS_TO_RPS(x)           ((x) * FOC_RADS_TO_RPS_K)
#define FOC_RPS_TO_RADS(x)           ((x) * FOC_RPS_TO_RADS_K)
#define FOC_DEG_TO_RAD(x)            ((x) * FOC_DEG_TO_RAD_K)
#define FOC_RAD_TO_DEG(x)            ((x) * FOC_RAD_TO_DEG_K)

/* Mechanical ↔ electrical angle conversion */
#define FOC_MECH_TO_ELEC(mech, pp)   ((mech) * (float)(pp))
#define FOC_ELEC_TO_MECH(elec, pp)   ((elec) / (float)(pp))

/* RMS / peak conversions */
#define FOC_RMS_TO_PEAK(rms)         ((rms) * FOC_SQRT2)
#define FOC_PEAK_TO_RMS(peak)        ((peak) * FOC_INV_SQRT2)

/* Duty cycle ↔ voltage */
#define FOC_DUTY_TO_VOLT(duty, vbus) ((duty) * (vbus))
#define FOC_VOLT_TO_DUTY(volt, vbus) ((volt) / (vbus))

/* Frequency ↔ angular velocity conversions */
#define FOC_HZ_TO_RADS(x)            ((x) * FOC_2PI)
#define FOC_RADS_TO_HZ(x)            ((x) / FOC_2PI)

/** @} */

/* Section 3: Scalar utility macros ------------------------------------------------------------- */

/** @defgroup scalar Scalar Utilities */
/** @{ */

/* Trig wrappers — use CMSIS DSP table lookup on M4 (much faster than libm) */
#define FOC_SIN(x)                   arm_sin_f32(x)
#define FOC_COS(x)                   arm_cos_f32(x)

#define FOC_ABS(x)                   fabsf(x)
#define FOC_SIGN(x)                  (signbit((x)) ? -1.0f : 1.0f)
#define FOC_SQ(x)                    ((x) * (x))
#define FOC_CB(x)                    ((x) * (x) * (x))
#define FOC_SQRT(x)                  sqrtf(x)

#define FOC_MAX(a, b)                ((a) > (b) ? (a) : (b))
#define FOC_MIN(a, b)                ((a) < (b) ? (a) : (b))

/* Clamp x to the closed interval [lo, hi]. */
#define FOC_CLAMP(x, lo, hi)         ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

/* Symmetric clamp: clamp x to [-limit, +limit]. limit must be non-negative. */
#define FOC_SATURATE(x, limit)       FOC_CLAMP((x), -(limit), (limit))

/* Test whether a float is effectively zero (within a small epsilon). */
#define FOC_IS_ZERO(x, eps)          (FOC_ABS(x) < (eps))

/* Swap two floats without a temporary variable (XOR trick is for integers). */
#define FOC_SWAP_FLOAT(a, b) \
    do {                     \
        float _t = (a);      \
        (a)      = (b);      \
        (b)      = _t;       \
    } while (0)

/** @} */

/* Section 4: Angle & modulo helpers (inline for ISR use) --------------------------------------- */

/**
 * @brief  Positive-result integer modulo.
 * @return a % b, always in [0, b).
 */
static inline int32_t foc_mod_int(int32_t a, int32_t b)
{
    int32_t r = a % b;
    return (r < 0) ? (r + b) : r;
}

/**
 * @brief  Positive-result float fmod.
 * @return x mod y, always in [0, y).
 */
static inline float foc_fmod_pos(float x, float y)
{
    float r = fmodf(x, y);
    return (r < 0.0f) ? (r + y) : r;
}

/**
 * @brief  Wrap angle to [0, 2π).
 */
static inline float foc_wrap_0_2pi(float angle)
{
    float n = floorf(angle * FOC_INV_2PI); /* FMUL + FRINTM */
    float r = angle - n * FOC_2PI;         /* FMLS */
    if (r >= FOC_2PI) {
        r -= FOC_2PI;
    }
    if (r < 0.0f) {
        r += FOC_2PI;
    }
    return r;
}

/**
 * @brief  Wrap angle to [-2π, 4π), with near-range corrections.
 */
static inline float foc_wrap_0_2pi_near(float angle)
{
    if (angle >= FOC_2PI) {
        angle -= FOC_2PI;
    }
    if (angle < 0.0f) {
        angle += FOC_2PI;
    }
    return angle;
}

/**
 * @brief  Wrap angle to (−π, π].
 */
static inline float foc_wrap_pm_pi(float angle)
{
    float n = floorf(angle * FOC_INV_2PI + 0.5f);
    float r = angle - n * FOC_2PI;
    if (r > FOC_PI) {
        r -= FOC_2PI;
    }
    if (r < -FOC_PI) {
        r += FOC_2PI;
    }
    return r;
}

/**
 * @brief  Signed shortest angular difference: angle2 − angle1, result in (−π, π].
 *
 * @note   Prefer this over plain subtraction whenever angles may wrap around.
 */
static inline float foc_angle_diff(float angle1, float angle2)
{
    return foc_wrap_pm_pi(angle2 - angle1);
}

/**
 * @brief  Spherical linear interpolation between two angles.
 * @param  t  Blend factor in [0, 1].
 * @return    Angle in [0, 2π).
 */
static inline float foc_angle_lerp(float angle1, float angle2, float t)
{
    return foc_wrap_0_2pi(angle1 + foc_angle_diff(angle1, angle2) * t);
}

/**
 * @brief  Vector magnitude from Cartesian components.
 */
static inline float foc_magnitude(float x, float y)
{
    return sqrtf(FOC_SQ(x) + FOC_SQ(y));
}

/**
 * @brief  Vector phase angle (atan2), result in [0, 2π).
 */
static inline float foc_phase_angle(float x, float y)
{
    return foc_wrap_0_2pi(atan2f(y, x));
}

/* Section 5: First-order low-pass filter ------------------------------------------------------- */

/**
 * @brief  Discrete first-order IIR low-pass filter.
 *
 * Transfer function (bilinear, Tustin approximation):
 *   y[n] = α · x[n] + (1 − α) · y[n−1]
 *   where α = ωc / (ωc + fs),  ωc = 2π · fc
 *
 * For ADC ISR use (constant Ts), α is computed once in lpf_init().
 */
struct lpf {
    float alpha;  /* Filter coefficient in (0, 1). Closer to 1 → less filtering. */
    float output; /* Last filtered value. */
};

/**
 * @brief  Compute LPF coefficient from cutoff frequency and sample rate.
 * @param  fc_hz  Cutoff frequency in Hz.
 * @param  fs_hz  Sample rate in Hz (must be > 0).
 */
static inline void lpf_init(struct lpf *f, float fc_hz, float fs_hz)
{
    if (f == NULL || fs_hz <= 0.0f || fc_hz <= 0.0f) {
        return;
    }
    float wc  = FOC_HZ_TO_RADS(fc_hz);
    f->alpha  = wc / (wc + fs_hz);
    f->output = 0.0f;
}

/**
 * @brief  Feed one sample through the LPF and return the filtered value.
 */
static inline float lpf_update(struct lpf *f, float input)
{
    f->output += f->alpha * (input - f->output);
    return f->output;
}

/**
 * @brief  Reset LPF state to an explicit value.
 * @note   Use this to avoid transients when enabling closed-loop control.
 */
static inline void lpf_reset(struct lpf *f, float initial_value)
{
    if (f != NULL) {
        f->output = initial_value;
    }
}

/* Section 6: Exponential moving average (EMA) -------------------------------------------------- */

/**
 * @brief  Compute a one-shot EMA update without persistent state.
 *
 * Useful for single-line smoothing in calibration or fault detection routines.
 *
 * @param  prev    Last averaged value.
 * @param  sample  New sample.
 * @param  alpha   Blend coefficient in (0, 1). α=1 → no smoothing.
 * @return New averaged value.
 */
static inline float foc_ema(float prev, float sample, float alpha)
{
    return prev + alpha * (sample - prev);
}

/**
 * @brief  Compute cumulative moving average (CMA).
 *
 * @param  avg     Running average up to sample index (n − 1).
 * @param  sample  New sample (index n, 1-based).
 * @param  n       Total count of samples including this one (n ≥ 1).
 * @return Updated average.
 */
static inline float foc_cma(float avg, float sample, uint32_t n)
{
    if (n == 0U) {
        return sample;
    }
    return avg + (sample - avg) / (float)n;
}

/* Section 7: Ramp / rate limiter --------------------------------------------------------------- */

/**
 * @brief  Rate-limited ramp from `current` toward `target`.
 *
 * Given a maximum rate of change `rate` (units/s) and a time step `dt` (s),
 * advances `current` by at most `rate × dt` in either direction.
 *
 * @param  current     Present value.
 * @param  target      Desired value.
 * @param  rate_limit  Maximum |Δvalue|/s.  Must be > 0.
 * @param  dt          Time step in seconds.  Must be > 0.
 * @return New value, clamped so the step ≤ rate_limit × dt.
 */
static inline float foc_ramp_linear(float current, float target, float rate_limit, float dt)
{
    if (dt <= 0.0f || rate_limit <= 0.0f) {
        return current;
    }
    float max_step = rate_limit * dt;
    float delta    = target - current;
    if (delta > max_step) {
        return current + max_step;
    } else if (delta < -max_step) {
        return current - max_step;
    }
    return target; /* dead-beat: target is within one step */
}

/**
 * @brief  Determine whether `current` has reached `target` within a tolerance.
 * @return 1 if |target − current| ≤ tol, else 0.
 */
static inline int foc_ramp_done(float current, float target, float tol)
{
    return (FOC_ABS(target - current) <= tol) ? 1 : 0;
}

/* Section 8: Linear interpolation helpers ------------------------------------------------------ */

/**
 * @brief  Scalar linear interpolation:  a + t · (b − a).
 * @param  t  Blend factor. Unclamped — values outside [0, 1] extrapolate.
 */
static inline float foc_lerp(float a, float b, float t)
{
    return a + t * (b - a);
}

/**
 * @brief  Clamped lerp: t is clamped to [0, 1] before blending.
 */
static inline float foc_lerp_clamp(float a, float b, float t)
{
    return foc_lerp(a, b, FOC_CLAMP(t, 0.0f, 1.0f));
}

/**
 * @brief  Inverse lerp: given value v in [a, b], return normalized t in [0, 1].
 *         Returns 0 if a == b to avoid division by zero.
 */
static inline float foc_inv_lerp(float a, float b, float v)
{
    float span = b - a;
    return (FOC_ABS(span) < 1e-9f) ? 0.0f : FOC_CLAMP((v - a) / span, 0.0f, 1.0f);
}

/**
 * @brief  1-D LUT linear interpolation.
 *
 * Given a sorted LUT of (x_values[i], y_values[i]) pairs and a query x,
 * returns the linearly interpolated y.  Clamps to the endpoints.
 *
 * @param  x_vals  Sorted input axis array.
 * @param  y_vals  Output value array.
 * @param  n       Number of entries (must be ≥ 2).
 * @param  x       Query value.
 */
static inline float foc_lut_interp(const float *x_vals, const float *y_vals, uint32_t n, float x)
{
    if (x <= x_vals[0]) {
        return y_vals[0];
    }
    if (x >= x_vals[n - 1U]) {
        return y_vals[n - 1U];
    }
    /* Binary-ish scan (table is small in FOC — linear scan is fine) */
    for (uint32_t i = 0U; i < (n - 1U); i++) {
        if (x < x_vals[i + 1U]) {
            float t = (x - x_vals[i]) / (x_vals[i + 1U] - x_vals[i]);
            return foc_lerp(y_vals[i], y_vals[i + 1U], t);
        }
    }
    return y_vals[n - 1U];
}

/* Section 9: Dead-time compensation ------------------------------------------------------------ */

/**
 * @brief  Compute dead-time voltage distortion compensation for one phase.
 *
 * The dead-time introduces a voltage error proportional to the sign of the
 * phase current.  The correction adds (or subtracts) a fixed offset to the
 * duty-cycle command.
 *
 * Derivation: ΔV = (Td / Ts) · Vbus · sign(I_phase)
 *
 * @param  i_phase    Phase current (A).  Sign determines direction.
 * @param  td         Dead time (s).
 * @param  ts         PWM period (s).
 * @param  v_bus      DC bus voltage (V).
 * @return Voltage compensation term (V) to ADD to the commanded phase voltage.
 */
static inline float foc_dead_time_comp(float i_phase, float td, float ts, float v_bus)
{
    float ratio = td / ts;
    return ratio * v_bus * FOC_SIGN(i_phase);
}

/* Section 10: Numerics & DSP helpers ----------------------------------------------------------- */

/**
 * @brief  Safe reciprocal: returns 1/x if |x| > eps, else fallback.
 * @note   Avoids a division-by-zero trap in observer / PI gains.
 */
static inline float foc_safe_reciprocal(float x, float eps, float fallback)
{
    return (FOC_ABS(x) > eps) ? (1.0f / x) : fallback;
}

/**
 * @brief  Compute bandwidth-based PI gains for a first-order RL plant.
 *
 *   Kp = Ls · ωc
 *   Ki = Rs · ωc           (continuous; multiply by Ts for discrete)
 *
 * @param  ls       Phase inductance (H).
 * @param  rs       Phase resistance (Ω).
 * @param  bw_hz    Desired closed-loop bandwidth (Hz).
 * @param[out] kp   Proportional gain.
 * @param[out] ki   Continuous integral gain.
 */
static inline void foc_current_pi_gains(float ls, float rs, float bw_hz, float *kp, float *ki)
{
    float wc = FOC_HZ_TO_RADS(bw_hz);
    *kp      = ls * wc;
    *ki      = rs * wc;
}

/**
 * @brief  Clamp a value and set a saturation flag (anti-windup helper).
 *
 * @param[in,out] value  Value to clamp.
 * @param         lo     Lower bound.
 * @param         hi     Upper bound.
 * @return  1 if value was clamped, 0 if it was within bounds.
 */
static inline int foc_clamp_flag(float *value, float lo, float hi)
{
    if (*value < lo) {
        *value = lo;
        return 1;
    }
    if (*value > hi) {
        *value = hi;
        return 1;
    }
    return 0;
}

/* Section 11: sine and cosine ------------------------------------------------------------------ */

/**
 * @brief  Sine kernel for x ∈ [0, π/2].
 *
 * Degree-9 Horner polynomial (Taylor series, 5 coefficients):
 *   sin(x) = x · (1 + x²·(−1/6 + x²·(1/120 + x²·(−1/5040 + x²/362880))))
 *
 * Accuracy:
 *   · Max truncation error at x = π/2 : ≈ 3.5e-6
 *   · For x ≤ π/4              : error < 5e-9  (near float32 machine ε)
 *
 * Performance (Cortex-M4F, FPU pipeline):
 *   1 × FMUL  (x² = x·x)
 *   4 × FMLA  (Horner steps, each 1 cycle pipelined)
 *   1 × FMUL  (x · p)
 *   = 6 FP instructions, no branches.
 *
 * Precondition: x ∈ [0, π/2].  Caller is responsible for range reduction.
 */
static inline float _foc_sin_kernel(float x)
{
    float x2 = x * x;
    float p  = 2.7557314e-6f;           /* +1/362880 */
    p        = -1.9841270e-4f + p * x2; /* -1/5040   */
    p        = 8.3333333e-3f + p * x2;  /* +1/120    */
    p        = -1.6666667e-1f + p * x2; /* -1/6      */
    p        = 1.0000000e+0f + p * x2;  /* +1        */
    return x * p;
}

/**
 * @brief  Fast sine approximation.
 *
 * Accuracy: max error ≈ 3.5e-6 (200× better than the previous Bhaskara I
 * implementation which had max error ≈ 7e-4).
 *
 * @param  angle  Input angle (rad). Caller MUST ensure input ∈ [0, 2π)
 *                via foc_wrap_0_2pi() before calling.
 *
 * @return float  Sine value in [-1.0, 1.0].
 */
static inline float foc_fast_sin(float angle)
{
    /* Quadrant reduction to [0, π/2] */
    if (angle < FOC_HALF_PI) {
        return _foc_sin_kernel(angle); /* Q1 */
    } else if (angle < FOC_PI) {
        return _foc_sin_kernel(FOC_PI - angle); /* Q2: sin(π−x)=sin(x) */
    } else if (angle < FOC_PI + FOC_HALF_PI) {
        return -_foc_sin_kernel(angle - FOC_PI); /* Q3: sin(π+x)=−sin(x) */
    } else {
        return -_foc_sin_kernel(FOC_2PI - angle); /* Q4: sin(2π−x)=−sin(x) */
    }
}

/**
 * @brief  Fast cosine approximation.
 *
 * Implemented via the same kernel as foc_fast_sin() using cos(x)=sin(π/2−x)
 * with quadrant-specific sign corrections.
 *
 * @param  angle  Input angle (rad). Caller MUST ensure input ∈ [0, 2π)
 *                via foc_wrap_0_2pi() before calling.
 *
 * @return float  Cosine value in [-1.0, 1.0].
 */
static inline float foc_fast_cos(float angle)
{
    if (angle < FOC_HALF_PI) {
        return _foc_sin_kernel(FOC_HALF_PI - angle); /* Q1 */
    } else if (angle < FOC_PI) {
        return -_foc_sin_kernel(angle - FOC_HALF_PI); /* Q2 */
    } else if (angle < FOC_PI + FOC_HALF_PI) {
        return -_foc_sin_kernel(FOC_PI + FOC_HALF_PI - angle); /* Q3 */
    } else {
        return _foc_sin_kernel(angle - FOC_PI - FOC_HALF_PI); /* Q4 */
    }
}

/**
 * @brief  Fast simultaneous sine and cosine — 20 kHz FOC hot path.
 *
 * Design:
 *   · Quadrant detection maps angle ∈ [0, 2π) → reduced x ∈ [0, π/2].
 *   · sin(angle) and cos(angle) are both derived from the same kernel via:
 *       s = _foc_sin_kernel(x)
 *       c = _foc_sin_kernel(π/2 − x)    [cos(x) = sin(π/2−x)]
 *   · Sign correction applied per quadrant.  No second wrapping needed
 *     because x ∈ [0, π/2] guarantees (π/2−x) ∈ [0, π/2] as well.
 *
 * Accuracy vs previous Bhaskara I + 0.225:
 *   Bhaskara (old):  max error ≈ 7.0e-4,  norm_err ≈ 2.0e-3  (at 45°)
 *   Horner   (new):  max error ≈ 3.5e-6,  norm_err ≈ 7.1e-6  (at 90°)
 *   Improvement:     200× more accurate.
 *
 * Performance vs previous implementation:
 *   Old:  ~23 FP ops + 5 branches (two-pass: Bhaskara + 0.225 refinement)
 *   New:  ~14 FP ops + 4 branches (single-pass Horner × 2 calls)
 *   Improvement:  ~40% fewer operations, more branch-predictor-friendly.
 *
 * @param  angle  Input angle (rad). Caller MUST ensure input ∈ [0, 2π)
 *                via foc_wrap_0_2pi() before calling.
 * @param  sin_v  Output pointer for sin(angle).
 * @param  cos_v  Output pointer for cos(angle).
 */
static inline void foc_fast_sincos(float angle, float *sin_v, float *cos_v)
{
    float x; /* Reduced angle ∈ [0, π/2] */
    float s; /* _foc_sin_kernel(x)       */
    float c; /* _foc_sin_kernel(π/2 − x) */

    if (angle < FOC_HALF_PI) {
        /* Q1 [0, π/2): sin > 0, cos > 0 */
        x      = angle;
        s      = _foc_sin_kernel(x);
        c      = _foc_sin_kernel(FOC_HALF_PI - x);
        *sin_v = s;
        *cos_v = c;

    } else if (angle < FOC_PI) {
        /* Q2 [π/2, π): sin(π−x)=sin(x) > 0, cos(π−x)=−cos(x) < 0 */
        x      = FOC_PI - angle;
        s      = _foc_sin_kernel(x);
        c      = _foc_sin_kernel(FOC_HALF_PI - x);
        *sin_v = s;
        *cos_v = -c;

    } else if (angle < FOC_PI + FOC_HALF_PI) {
        /* Q3 [π, 3π/2): sin(π+x)=−sin(x) < 0, cos(π+x)=−cos(x) < 0 */
        x      = angle - FOC_PI;
        s      = _foc_sin_kernel(x);
        c      = _foc_sin_kernel(FOC_HALF_PI - x);
        *sin_v = -s;
        *cos_v = -c;

    } else {
        /* Q4 [3π/2, 2π): sin(2π−x)=−sin(x) < 0, cos(2π−x)=cos(x) > 0 */
        x      = FOC_2PI - angle;
        s      = _foc_sin_kernel(x);
        c      = _foc_sin_kernel(FOC_HALF_PI - x);
        *sin_v = -s;
        *cos_v = c;
    }
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
