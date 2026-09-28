/**
 * @file    motor_bench_watchdog.h
 * @brief   Bench-only serial keepalive watchdog
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct motor_bench_watchdog_status {
    uint32_t heartbeat_age_ticks;
    uint32_t timeout_count;
    bool     armed;
    bool     expired;
};

void motor_bench_watchdog_reset(void);
bool motor_bench_watchdog_tick(bool running);
bool motor_bench_watchdog_keepalive(void);
void motor_bench_watchdog_status_read(struct motor_bench_watchdog_status *out);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
