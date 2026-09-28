/**
 * @file    motor_bench_watchdog.c
 * @brief   Bench-only serial keepalive watchdog
 */

#include "motor_bench_watchdog.h"

#include "motor_task_config.h"

#include <string.h>

static struct motor_bench_watchdog_status _status;

void motor_bench_watchdog_reset(void)
{
    memset(&_status, 0, sizeof(_status));
}

bool motor_bench_watchdog_tick(bool running)
{
    if (!running) {
        _status.armed = false;
        _status.heartbeat_age_ticks = 0U;
        return false;
    }

    if (!_status.armed) {
        _status.armed = true;
        _status.expired = false;
        _status.heartbeat_age_ticks = 0U;
    }

    if (_status.heartbeat_age_ticks < TM_MOTOR_BENCH_HEARTBEAT_TIMEOUT_TICKS) {
        _status.heartbeat_age_ticks++;
    }
    if (_status.heartbeat_age_ticks < TM_MOTOR_BENCH_HEARTBEAT_TIMEOUT_TICKS) {
        return false;
    }

    if (!_status.expired) {
        _status.expired = true;
        _status.timeout_count++;
    }
    return true;
}

bool motor_bench_watchdog_keepalive(void)
{
    if (_status.expired) {
        return false;
    }

    _status.armed = true;
    _status.heartbeat_age_ticks = 0U;
    return true;
}

void motor_bench_watchdog_status_read(struct motor_bench_watchdog_status *out)
{
    if (out != NULL) {
        *out = _status;
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
