/**
 * @file    pid.c
 * @brief   Reusable PID controller component with optional improvement flags
 * @version v2.1.0
 * @date    2026.03.28
 *
 * Design overview
 * ---------------
 * This file keeps full PID policy, fast-path helpers, and optional monitor
 * logic reusable while leaving sampling cadence and actuator transport to the
 * caller.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "pid.h"
#include <string.h>

/* Private constants ---------------------------------------------------------------------------- */
#define PID_MIN_DT      (1.0e-6f)    /* 1us: Avoid division by zero */
#define PID_MAX_DT      (0.1f)       /* 100ms: Avoid integral explosion during breakpoints */
#define PID_STALL_LIMIT (500U)       /* Cycles to trigger stall error */
#define PID_2PI         (6.2831853f) /* 2 * PI */

/* Private macros ------------------------------------------------------------------------------- */
#ifndef ABS
#define ABS(x) ((x) > 0 ? (x) : -(x))
#endif

#ifndef CONSTRAIN
#define CONSTRAIN(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))
#endif

/* Private function prototypes ------------------------------------------------------------------ */
static inline float _pid_filter_update(float input, float last_output, float tau, float dt);
static float        _pid_sanitize_dt(struct pid_dev *pid);
static void         _pid_handle_stall(struct pid_dev *pid);

/* Public functions ----------------------------------------------------------------------------- */

int pid_init(struct pid_dev *pid, const struct pid_config *config)
{
    if (pid == NULL || config == NULL) {
        return -EINVAL;
    }

    /* Reset the whole runtime object so no stale state leaks across re-init. */
    memset(pid, 0, sizeof(struct pid_dev));

    /* Copy config */
    pid->config = *config;

    /*
     * Pre-calculate filter time constants once at init so the runtime path
     * only performs cheap multiply/add work inside the control loop.
     */
    if (config->output_lpf_hz > 0.0f) {
        pid->tau_output = 1.0f / (PID_2PI * config->output_lpf_hz);
    } else {
        pid->tau_output = 0.0f;
    }

    if (config->deriv_lpf_hz > 0.0f) {
        pid->tau_deriv = 1.0f / (PID_2PI * config->deriv_lpf_hz);
    } else {
        pid->tau_deriv = 0.0f;
    }

    /* Initialize timing */
    core_time_perf_start(&pid->timer);

    return 0;
}

int pid_reset(struct pid_dev *pid)
{
    if (pid == NULL) {
        return -EINVAL;
    }

    /* Reset state variables but keep config and pre-calculated constants */
    memset(pid->err, 0, sizeof(pid->err));
    pid->ref = pid->measure = pid->output = 0.0f;
    pid->p_out = pid->i_out = pid->d_out = pid->f_out = 0.0f;
    pid->last_output = pid->last_measure = pid->last_d_out = 0.0f;

    pid->status.err_type  = PID_ERROR_NONE;
    pid->status.block_cnt = 0;

    /* Reset timer reference */
    core_time_perf_start(&pid->timer);

    return 0;
}

int pid_set_gains(struct pid_dev *pid, float kp, float ki, float kd)
{
    if (pid == NULL) {
        return -EINVAL;
    }
    pid->config.kp = kp;
    pid->config.ki = ki;
    pid->config.kd = kd;

    return 0;
}

float pid_calc(struct pid_dev *pid, float ref, float measure, float ff)
{
    if (pid == NULL)
        return 0.0f;

    /* 1. Calculate DT with core_time */
    core_time_perf_stop(&pid->timer);
    pid->dt = core_time_perf_get_us(&pid->timer) * 1e-6f;
    core_time_perf_start(&pid->timer);

    /* Clamp DT so breakpoints or timer jitter cannot explode integral math. */
    (void)_pid_sanitize_dt(pid);

    /* 2. Update State and Error History */
    pid->ref     = ref;
    pid->measure = measure;
    pid->f_out   = ff;

    pid->err[2] = pid->err[1];
    pid->err[1] = pid->err[0];
    pid->err[0] = pid->ref - pid->measure;

    /* 3. Dead Band Handling */
    if (ABS(pid->err[0]) < pid->config.dead_band) {
        pid->err[0] = 0.0f;
        /* Note: We continue execution to maintain filter states */
    }

    float final_out = 0.0f;

    /* ======================= Position Mode ======================= */
    if (pid->config.mode == PID_POSITION) {
        /* --- Proportional Term --- */
        if (pid->config.improve & PID_PROPORTIONAL_ON_MEASUREMENT) {
            /* P on Measurement (Smoother for setpoint changes) */
            pid->p_out = -pid->config.kp * pid->measure;
        } else {
            /* Standard P on Error */
            pid->p_out = pid->config.kp * pid->err[0];
        }

        /* --- Integral Term --- */
        float i_inc = 0.0f;
        if (pid->config.improve & PID_TRAPEZOID_INTEGRAL) {
            /* Trapezoidal rule: Area = (a+b)*h/2 */
            i_inc = pid->config.ki * (pid->err[0] + pid->err[1]) * 0.5f * pid->dt;
        } else {
            /* Rectangular rule */
            i_inc = pid->config.ki * pid->err[0] * pid->dt;
        }

        pid->i_out += i_inc;

        /* Static anti-windup stays local to the integral accumulator only. */
        if (pid->config.improve & PID_INTEGRAL_LIMIT) {
            pid->i_out =
                CONSTRAIN(pid->i_out, -pid->config.integral_limit, pid->config.integral_limit);
        }

        /* --- Derivative Term --- */
        float d_raw = 0.0f;
        if (pid->config.improve & PID_DERIVATIVE_ON_MEASUREMENT) {
            /* D on Measurement: -Kd * d(Meas)/dt */
            d_raw = -pid->config.kd * (pid->measure - pid->last_measure) / pid->dt;
        } else {
            /* D on Error: Kd * d(Err)/dt */
            d_raw = pid->config.kd * (pid->err[0] - pid->err[1]) / pid->dt;
        }

        /* Derivative Filter (Tau-based) */
        if (pid->config.improve & PID_DERIVATIVE_FILTER) {
            pid->d_out = _pid_filter_update(d_raw, pid->last_d_out, pid->tau_deriv, pid->dt);
        } else {
            pid->d_out = d_raw;
        }

        /* Save derivative state after optional filtering for the next cycle. */
        pid->last_measure = pid->measure;
        pid->last_d_out   = pid->d_out;

        /* --- Summation --- */
        /* Result = P + I + D + FF */
        pid->output = pid->p_out + pid->i_out + pid->d_out + pid->f_out;
        final_out   = pid->output;
    }
    /* ======================= Delta Mode ======================= */
    else {
        float p_inc, i_inc, d_inc;

        /* Calculate Increments */
        p_inc = pid->config.kp * (pid->err[0] - pid->err[1]);
        i_inc = pid->config.ki * pid->err[0] * pid->dt;
        d_inc = pid->config.kd * (pid->err[0] - 2.0f * pid->err[1] + pid->err[2]) / pid->dt;

        /*
         * In delta mode `pid->output` is the accumulator state. Feed-forward is
         * added only to the final output, never to the accumulator itself.
         */
        pid->output += (p_inc + i_inc + d_inc);

        /* Accumulator Anti-Windup 
         * Preventing the internal state from drifting to infinity */
        pid->output = CONSTRAIN(pid->output, -pid->config.max_output, pid->config.max_output);

        /* Add Feed-Forward to the Final Output */
        final_out = pid->output + ff;
    }

    /* 4. Output Filter (Common for both modes) */
    if (pid->config.improve & PID_OUTPUT_FILTER) {
        final_out = _pid_filter_update(final_out, pid->last_output, pid->tau_output, pid->dt);
    }
    pid->last_output = final_out; /* Save filtered result for next loop */

    /* 5. Final Output Saturation (Static Limit) */
    final_out = CONSTRAIN(final_out, -pid->config.max_output, pid->config.max_output);

    /*
     * Position mode exposes the final clamped value directly. Delta mode keeps
     * `pid->output` as the accumulator state computed above.
     */
    if (pid->config.mode == PID_POSITION) {
        pid->output = final_out;
    }

    /* 6. Error Handling */
    if (pid->config.improve & PID_ERROR_HANDLE) {
        /* Stall monitor inspects the clamped final output, not the raw sum. */
        float temp_store = pid->output;
        pid->output      = final_out;
        _pid_handle_stall(pid);
        pid->output = temp_store; /* Restore state */
    }

    return final_out;
}

float pid_calc_position_fast(struct pid_dev *pid, float ref, float measure, float ff)
{
    float dt;

    if (pid == NULL) {
        return 0.0f;
    }

    dt = _pid_sanitize_dt(pid);

    pid->ref     = ref;
    pid->measure = measure;
    pid->f_out   = ff;

    pid->err[1] = pid->err[0];
    pid->err[0] = pid->ref - pid->measure;

    pid->p_out = pid->config.kp * pid->err[0];
    pid->i_out += pid->config.ki * pid->err[0] * dt;

    if (pid->config.improve & PID_INTEGRAL_LIMIT) {
        if (pid->i_out > pid->config.integral_limit) {
            pid->i_out = pid->config.integral_limit;
        } else if (pid->i_out < -pid->config.integral_limit) {
            pid->i_out = -pid->config.integral_limit;
        }
    }

    pid->output = pid->p_out + pid->i_out + pid->f_out;

    if (pid->output > pid->config.max_output) {
        pid->output = pid->config.max_output;
    } else if (pid->output < -pid->config.max_output) {
        pid->output = -pid->config.max_output;
    }

    return pid->output;
}
float pid_calc_delta_fast(struct pid_dev *pid, float ref, float measure, float ff)
{
    float dt;

    if (pid == NULL) {
        return 0.0f;
    }

    dt           = _pid_sanitize_dt(pid);
    pid->ref     = ref;
    pid->measure = measure;

    pid->err[2] = pid->err[1];
    pid->err[1] = pid->err[0];
    pid->err[0] = pid->ref - pid->measure;

    float delta_p = pid->config.kp * (pid->err[0] - pid->err[1]);
    float delta_i = pid->config.ki * pid->err[0] * dt;

    float delta_d = 0.0f;
    if (pid->config.kd > 0.0f) {
        float inv_dt = 1.0f / dt;
        delta_d      = pid->config.kd * (pid->err[0] - 2.0f * pid->err[1] + pid->err[2]) * inv_dt;
    }

    pid->output += (delta_p + delta_i + delta_d);

    if (pid->output > pid->config.max_output) {
        pid->output = pid->config.max_output;
    } else if (pid->output < -pid->config.max_output) {
        pid->output = -pid->config.max_output;
    }

    float final_out = pid->output + ff;

    if (final_out > pid->config.max_output) {
        final_out = pid->config.max_output;
    } else if (final_out < -pid->config.max_output) {
        final_out = -pid->config.max_output;
    }

    return final_out;
}

/* Private functions ---------------------------------------------------------------------------- */

/**
 * @brief  Bandwidth-limited Low Pass Filter
 * @note   Implements: y[n] = y[n-1] + alpha * (x[n] - y[n-1])
 * where alpha = dt / (tau + dt)
 * This ensures constant Bandwidth even if DT jitters.
 */
static inline float _pid_filter_update(float input, float last_output, float tau, float dt)
{
    if (tau <= 0.0f) {
        return input;
    }

    /* Dynamic alpha keeps cutoff behavior stable even when `dt` jitters. */
    float alpha = dt / (tau + dt);

    return last_output + alpha * (input - last_output);
}

static float _pid_sanitize_dt(struct pid_dev *pid)
{
    if (pid == NULL) {
        return PID_MIN_DT;
    }

    if (pid->dt < PID_MIN_DT) {
        pid->dt = PID_MIN_DT;
    } else if (pid->dt > PID_MAX_DT) {
        pid->dt = PID_MAX_DT;
    }

    return pid->dt;
}

/**
 * @brief  Check for motor stall/blocking
 */
static void _pid_handle_stall(struct pid_dev *pid)
{
    if (ABS(pid->err[0]) > pid->status.max_error) {
        pid->status.max_error = ABS(pid->err[0]);
    }

    float out_abs = ABS(pid->last_output); /* Use the final filtered output */

    if ((out_abs >= (pid->config.max_output * 0.95f)) &&
        (ABS(pid->err[0]) > (pid->config.max_output * 0.1f))) {
        pid->status.block_cnt++;
        if (pid->status.block_cnt > PID_STALL_LIMIT) {
            pid->status.err_type = PID_MOTOR_BLOCKED;
        }
    } else {
        pid->status.block_cnt = 0;
        pid->status.err_type  = PID_ERROR_NONE;
    }
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
