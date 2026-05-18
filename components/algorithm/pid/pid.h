/**
 * @file    pid.h
 * @brief   Reusable PID controller component with optional improvement flags
 * @version v2.1.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * `pid` is a reusable control component that owns PID state, timing history,
 * and optional enhancement switches such as integral clamp, derivative-on-
 * measurement, and output filtering. Hardware sampling, task ownership, and
 * actuator transport remain owned by the caller.
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns each `struct pid_dev` instance.
 * - `pid_init()` must run before any calculation function.
 * - `pid_calc()` updates `dt` from `core_time_perf`; the `*_fast` variants use
 *   the caller-maintained `pid->dt`, skip the timing read, and still clamp the
 *   supplied value into the component's safe numeric range.
 * - No internal locking is provided; task ownership and concurrency remain the
 *   caller's responsibility.
 *
 * Typical usage
 * -------------
 * @code
 *   pid_init(&loop, &cfg);
 *   for (;;) {
 *       float out = pid_calc(&loop, ref, feedback, ff);
 *       apply_output(out);
 *   }
 * @endcode
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "arm_math.h"
#include "core_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/** Disable all optional improvement paths. */
#define PID_IMPROVE_NONE                (0x00U)
/** Clamp the integral accumulator to `integral_limit`. */
#define PID_INTEGRAL_LIMIT              (0x01U)
/** Use derivative-on-measurement to avoid setpoint kick. */
#define PID_DERIVATIVE_ON_MEASUREMENT   (0x02U)
/** Use trapezoidal integration instead of rectangular integration. */
#define PID_TRAPEZOID_INTEGRAL          (0x04U)
/** Use proportional-on-measurement instead of proportional-on-error. */
#define PID_PROPORTIONAL_ON_MEASUREMENT (0x08U)
/** Low-pass filter the final controller output. */
#define PID_OUTPUT_FILTER               (0x10U)
/** Low-pass filter the derivative term. */
#define PID_DERIVATIVE_FILTER           (0x20U)
/** Enable simple stall/block detection bookkeeping. */
#define PID_ERROR_HANDLE                (0x40U)

/** Absolute/position-mode PID output. */
#define PID_POSITION                    (0x00U)
/** Incremental/delta-mode PID output. */
#define PID_DELTA                       (0x01U)

/** No controller error is currently latched. */
#define PID_ERROR_NONE                  (0x00U)
/** Simple stall detector believes the controlled plant is blocked. */
#define PID_MOTOR_BLOCKED               (0x01U)

/* Public Types --------------------------------------------------------------------------------- */

/** Runtime diagnostics maintained by the optional PID error-monitor logic. */
struct pid_status {
    /** Maximum absolute error observed since the last reset. */
    float    max_error;
    /** Consecutive stall-suspect samples counted by the monitor. */
    uint16_t block_cnt;
    /** Current latched PID error type. */
    uint8_t  err_type;
    /** Reserved for layout stability. */
    uint8_t  reserved;
};

/** Static configuration copied into `pid_dev` during initialization. */
struct pid_config {
    /** Proportional gain. */
    float kp;
    /** Integral gain. */
    float ki;
    /** Derivative gain. */
    float kd;

    /** Absolute output saturation limit. */
    float max_output;
    /** Absolute clamp limit for the integral accumulator. */
    float integral_limit;
    /** Error dead band around zero. */
    float dead_band;

    /** Final-output low-pass cutoff frequency in hertz. */
    float output_lpf_hz;
    /** Derivative-term low-pass cutoff frequency in hertz. */
    float deriv_lpf_hz;

    /** Reserved coefficient for future variable-integration policy. */
    float var_int_coef_a;
    /** Reserved coefficient for future variable-integration policy. */
    float var_int_coef_b;

    /** Bitwise OR of `PID_*` improvement flags. */
    uint8_t improve;
    /** `PID_POSITION` or `PID_DELTA`. */
    uint8_t mode;
    /** Reserved for layout stability. */
    uint8_t reserved[2];
};

/** Runtime PID state owned by the caller. */
struct pid_dev {
    /** Runtime copy of the caller-provided configuration. */
    struct pid_config config;

    /** Optional runtime diagnostics. */
    struct pid_status status;

    /** Current reference setpoint. */
    float ref;
    /** Current measured feedback value. */
    float measure;
    /** Current controller output or delta-mode accumulator. */
    float output;

    /** Output-filter time constant derived from `output_lpf_hz`. */
    float tau_output;
    /** Derivative-filter time constant derived from `deriv_lpf_hz`. */
    float tau_deriv;

    /** Error history: current, last, previous. */
    float err[3];
    /** Previous measurement sample used by derivative-on-measurement. */
    float last_measure;
    /** Previous filtered final output. */
    float last_output;
    /** Previous filtered derivative term. */
    float last_d_out;

    /** Current proportional contribution. */
    float p_out;
    /** Current integral contribution. */
    float i_out;
    /** Current derivative contribution. */
    float d_out;
    /** Current feed-forward term. */
    float f_out;

    /** Performance timer used by the full `pid_calc()` path. */
    struct core_time_perf timer;
    /** Current delta time in seconds. */
    float                 dt;
};

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief Initialize one PID controller instance.
 *
 * Copies the caller-provided configuration and clears all dynamic runtime
 * state (integral accumulator, previous error, derivative filter).
 *
 * @param[out] pid    Caller-owned PID instance to initialize.
 * @param[in]  config Caller-owned configuration snapshot copied into `pid`.
 *                    Must remain valid only for the duration of this call.
 *
 * @return `0` on success, or negative `-errno` on validation failure.
 */
int pid_init(struct pid_dev *pid, const struct pid_config *config);

/**
 * @brief Reset dynamic PID state while keeping the copied configuration.
 *
 * Clears the integral accumulator, previous error, and derivative history
 * without altering the P/I/D gains or output limits.
 *
 * @param[in,out] pid PID instance to reset.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int pid_reset(struct pid_dev *pid);

/**
 * @brief Update PID gains in the copied runtime configuration.
 *
 * @param[in,out] pid PID instance to update.
 * @param[in]     kp  Proportional gain.
 * @param[in]     ki  Integral gain.
 * @param[in]     kd  Derivative gain.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int pid_set_gains(struct pid_dev *pid, float kp, float ki, float kd);

/**
 * @brief Run the full PID calculation path.
 *
 * This variant automatically refreshes `dt` from the system performance
 * counter before calculating the new controller output. Suitable for
 * standard task-context periodic control loops.
 *
 * @note This function is NOT ISR-safe due to the underlying time query.
 *
 * @param[in,out] pid     PID instance.
 * @param[in]     ref     Setpoint reference value.
 * @param[in]     measure Measured process variable.
 * @param[in]     ff      Feed-forward term added to the PID output.
 *
 * @return Calculated controller output, bounded by the configured output limits.
 */
float pid_calc(struct pid_dev *pid, float ref, float measure, float ff);

/**
 * @brief Run the streamlined position-mode update using the caller-maintained `dt`.
 *
 * Assumes the error is `ref - measure` (position PID). The caller is
 * responsible for providing a stable and valid `dt` (e.g., from a fixed
 * hardware timer ISR).
 *
 * @note This function is ISR-safe if `dt` is provided from an ISR context.
 *
 * @param[in,out] pid     PID instance.
 * @param[in]     ref     Setpoint reference value.
 * @param[in]     measure Measured process variable.
 * @param[in]     ff      Feed-forward term added to the PID output.
 *
 * @return Calculated controller output, bounded by the configured output limits.
 */
float pid_calc_position_fast(struct pid_dev *pid, float ref, float measure, float ff);

/**
 * @brief Run the streamlined delta-mode update using the caller-maintained `dt`.
 *
 * Assumes the error is `measure - ref` (delta/speed PID). The caller is
 * responsible for providing a stable and valid `dt`.
 *
 * @note This function is ISR-safe if `dt` is provided from an ISR context.
 *
 * @param[in,out] pid     PID instance.
 * @param[in]     ref     Setpoint reference value.
 * @param[in]     measure Measured process variable.
 * @param[in]     ff      Feed-forward term added to the PID output.
 *
 * @return Calculated controller output, bounded by the configured output limits.
 */
float pid_calc_delta_fast(struct pid_dev *pid, float ref, float measure, float ff);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
