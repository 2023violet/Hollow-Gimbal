/**
 * @file    pid.h
 * @brief   Minimal host shim for PID persistence coverage
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>
#include <string.h>
#include "core_time.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PID_INTEGRAL_LIMIT (0x01U)
#define PID_POSITION       (0x00U)

struct pid_status {
    float    max_error;
    uint16_t block_cnt;
    uint8_t  err_type;
    uint8_t  reserved;
};

struct pid_config {
    float   kp;
    float   ki;
    float   kd;
    float   max_output;
    float   integral_limit;
    float   dead_band;
    float   output_lpf_hz;
    float   deriv_lpf_hz;
    float   var_int_coef_a;
    float   var_int_coef_b;
    uint8_t improve;
    uint8_t mode;
    uint8_t reserved[2];
};

struct pid_dev {
    struct pid_config    config;
    struct pid_status    status;
    float                ref;
    float                measure;
    float                output;
    float                tau_output;
    float                tau_deriv;
    float                err[3];
    float                last_measure;
    float                last_output;
    float                last_d_out;
    float                p_out;
    float                i_out;
    float                d_out;
    float                f_out;
    struct core_time_perf timer;
    float                 dt;
};

static inline int pid_init(struct pid_dev *pid, const struct pid_config *config)
{
    if ((pid == NULL) || (config == NULL)) {
        return -1;
    }

    memset(pid, 0, sizeof(*pid));
    pid->config = *config;
    return 0;
}

static inline int pid_reset(struct pid_dev *pid)
{
    if (pid == NULL) {
        return -1;
    }

    memset(pid->err, 0, sizeof(pid->err));
    pid->ref = pid->measure = pid->output = 0.0f;
    pid->p_out = pid->i_out = pid->d_out = pid->f_out = 0.0f;
    pid->last_measure = pid->last_output = pid->last_d_out = 0.0f;
    return 0;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
