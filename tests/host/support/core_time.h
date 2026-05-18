/**
 * @file    core_time.h
 * @brief   Minimal host shim for PID timing dependency
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

struct core_time_perf {
    unsigned dummy;
};

static inline void core_time_perf_start(struct core_time_perf *perf)
{
    (void)perf;
}

static inline void core_time_perf_stop(struct core_time_perf *perf)
{
    (void)perf;
}

static inline float core_time_perf_get_us(const struct core_time_perf *perf)
{
    (void)perf;
    return 1000.0f;
}

/* ---------------------------------------- end of file ----------------------------------------- */
