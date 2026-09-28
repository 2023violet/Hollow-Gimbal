/**
 * @file    test_motor_bench_watchdog_contract.c
 * @brief   Host contract for the bench serial keepalive watchdog
 */

#include <stdio.h>
#include <stdlib.h>

#include "motor_bench_watchdog.h"
#include "motor_task_config.h"

#define TEST_ASSERT(expr)                                                            \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr, "[test_motor_bench_watchdog_contract] failed: %s\\n", #expr); \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

static void test_running_axis_times_out_without_keepalive(void)
{
    struct motor_bench_watchdog_status status;

    motor_bench_watchdog_reset();
    for (uint32_t tick = 0U; tick < TM_MOTOR_BENCH_HEARTBEAT_TIMEOUT_TICKS - 1U; ++tick) {
        TEST_ASSERT(motor_bench_watchdog_tick(true) == false);
    }
    TEST_ASSERT(motor_bench_watchdog_tick(true) == true);
    motor_bench_watchdog_status_read(&status);
    TEST_ASSERT(status.expired == true);
    TEST_ASSERT(status.timeout_count == 1U);
}

static void test_keepalive_resets_age_before_timeout(void)
{
    struct motor_bench_watchdog_status status;

    motor_bench_watchdog_reset();
    for (uint32_t tick = 0U; tick < TM_MOTOR_BENCH_HEARTBEAT_TIMEOUT_TICKS / 2U; ++tick) {
        TEST_ASSERT(motor_bench_watchdog_tick(true) == false);
    }
    TEST_ASSERT(motor_bench_watchdog_keepalive() == true);
    motor_bench_watchdog_status_read(&status);
    TEST_ASSERT(status.armed == true);
    TEST_ASSERT(status.expired == false);
    TEST_ASSERT(status.heartbeat_age_ticks == 0U);
}

static void test_keepalive_arms_a_new_running_session(void)
{
    struct motor_bench_watchdog_status status;

    motor_bench_watchdog_reset();
    TEST_ASSERT(motor_bench_watchdog_keepalive() == true);
    motor_bench_watchdog_status_read(&status);
    TEST_ASSERT(status.armed == true);
    TEST_ASSERT(status.expired == false);
    TEST_ASSERT(status.heartbeat_age_ticks == 0U);
}

static void test_idle_axis_disarms_without_erasing_timeout_evidence(void)
{
    struct motor_bench_watchdog_status status;

    motor_bench_watchdog_reset();
    for (uint32_t tick = 0U; tick < TM_MOTOR_BENCH_HEARTBEAT_TIMEOUT_TICKS; ++tick) {
        (void)motor_bench_watchdog_tick(true);
    }
    TEST_ASSERT(motor_bench_watchdog_tick(false) == false);
    motor_bench_watchdog_status_read(&status);
    TEST_ASSERT(status.armed == false);
    TEST_ASSERT(status.expired == true);
    TEST_ASSERT(status.timeout_count == 1U);
}

int main(void)
{
    test_running_axis_times_out_without_keepalive();
    test_keepalive_resets_age_before_timeout();
    test_keepalive_arms_a_new_running_session();
    test_idle_axis_disarms_without_erasing_timeout_evidence();
    puts("[test_motor_bench_watchdog_contract] passed");
    return 0;
}
