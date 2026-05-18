/**
 * @file    test_core_time_dwt.c
 * @brief   Host-side DWT-path tests for Themis core_time
 * @version v1.0.0
 * @date    2026.04.04
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_rtos.h"

#define TEST_ASSERT(expr)                                                  \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr,                                                \
                    "[test_core_time_dwt] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                 \
                    __FILE__,                                              \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static uint32_t g_fake_demcr            = 0U;
static uint32_t g_fake_dwt_ctrl         = 0U;
static uint32_t g_fake_icsr             = 0U;
static uint32_t g_fake_dwt_cyccnt_value = 0U;
static uint32_t g_fake_dwt_step         = 1U;

static uint32_t test_core_time_dwt_cyccnt_read(void)
{
    uint32_t current = g_fake_dwt_cyccnt_value;

    g_fake_dwt_cyccnt_value += g_fake_dwt_step;
    return current;
}

#define CORE_TIME_DEMCR      g_fake_demcr
#define CORE_TIME_DWT_CTRL   g_fake_dwt_ctrl
#define CORE_TIME_ICSR       g_fake_icsr
#define CORE_TIME_DWT_CYCCNT test_core_time_dwt_cyccnt_read()

#include "../../../subsys/time/core_time.c"

static void core_time_test_reset(uint32_t step)
{
    host_rtos_reset();
    g_fake_demcr            = 0U;
    g_fake_dwt_ctrl         = 0U;
    g_fake_icsr             = 0U;
    g_fake_dwt_cyccnt_value = 0U;
    g_fake_dwt_step         = step;
}

static void test_core_time_init_enables_dwt_backend_once(void)
{
    core_time_test_reset(1U);

    TEST_ASSERT(core_time_init(12000000U) == 0);
    TEST_ASSERT((g_fake_demcr & CORE_TIME_TRCENA) != 0U);
    TEST_ASSERT((g_fake_dwt_ctrl & CORE_TIME_CYCCNTENA) != 0U);
    TEST_ASSERT(core_time_init(12000000U) == 0);
    TEST_ASSERT(core_time_init(0U) == -EINVAL);
    TEST_ASSERT(core_time_init(999999U) == -ERANGE);
}

static void test_core_time_delay_paths_choose_task_delay_or_spin_wait(void)
{
    core_time_test_reset(7U);
    TEST_ASSERT(core_time_init(1000000U) == 0);

    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    host_rtos_set_ipsr(0U);
    core_time_delay_ms(5U);
    TEST_ASSERT(host_rtos_get_vtask_delay_count() == 1U);
    TEST_ASSERT(host_rtos_get_vtask_delay_total_ticks() == 5U);

    core_time_test_reset(8U);
    TEST_ASSERT(core_time_init(2000000U) == 0);
    host_rtos_set_scheduler_state(taskSCHEDULER_NOT_STARTED);
    host_rtos_set_ipsr(0U);
    core_time_delay_us(3U);
    TEST_ASSERT(host_rtos_get_vtask_delay_count() == 0U);
    TEST_ASSERT(g_fake_dwt_cyccnt_value > 0U);

    core_time_test_reset(9U);
    TEST_ASSERT(core_time_init(2000000U) == 0);
    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    g_fake_icsr = CORE_TIME_VECTACTIVE_MASK;
    TEST_ASSERT(_core_time_is_in_isr());
    core_time_delay_ms(2U);
    TEST_ASSERT(host_rtos_get_vtask_delay_count() == 0U);
    TEST_ASSERT(g_fake_dwt_cyccnt_value > 0U);
}

static void test_core_time_perf_probe_reports_elapsed_microseconds(void)
{
    struct core_time_perf perf = {0};
    float                 elapsed_us;

    core_time_test_reset(4U);
    TEST_ASSERT(core_time_init(4000000U) == 0);

    core_time_perf_start(&perf);
    core_time_perf_stop(&perf);
    elapsed_us = core_time_perf_get_us(&perf);

    TEST_ASSERT(perf.delta_cycles == 4U);
    TEST_ASSERT(elapsed_us > 0.9f);
    TEST_ASSERT(elapsed_us < 1.1f);
}

int main(void)
{
    test_core_time_init_enables_dwt_backend_once();
    test_core_time_delay_paths_choose_task_delay_or_spin_wait();
    test_core_time_perf_probe_reports_elapsed_microseconds();

    puts("[test_core_time_dwt] passed");
    return 0;
}
