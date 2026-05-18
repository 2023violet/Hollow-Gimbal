/**
 * @file    test_tm_task_perf.c
 * @brief   Host-side tests for task performance snapshot publication
 * @version v1.0.0
 * @date    2026.04.30
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint32_t g_fake_dwt_cyccnt = 0U;
#define CORE_TIME_DWT_CYCCNT g_fake_dwt_cyccnt

#include "tm_task_perf.h"

#define TEST_ASSERT(expr)                                                  \
    do {                                                                   \
        if (!(expr)) {                                                     \
            fprintf(stderr,                                                \
                    "[test_tm_task_perf] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                 \
                    __FILE__,                                              \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

static uint32_t g_irq_enter_count = 0U;
static uint32_t g_irq_exit_count  = 0U;

float core_time_perf_get_us(const struct core_time_perf *ctx)
{
    TEST_ASSERT(ctx != NULL);
    return (float)ctx->delta_cycles;
}

uint32_t tm_irq_critical_enter(void)
{
    g_irq_enter_count++;
    return 0U;
}

void tm_irq_critical_exit(uint32_t level)
{
    TEST_ASSERT(level == 0U);
    g_irq_exit_count++;
}

static void test_tm_task_perf_end_publishes_snapshot_inside_irq_critical_section(void)
{
    struct tm_task_perf_region region = TM_TASK_PERF_REGION_INIT;

    g_irq_enter_count = 0U;
    g_irq_exit_count  = 0U;

    g_fake_dwt_cyccnt = 100U;
    tm_task_perf_begin(&region);
    g_fake_dwt_cyccnt = 132U;
    tm_task_perf_end(&region);

    TEST_ASSERT(g_irq_enter_count == 1U);
    TEST_ASSERT(g_irq_exit_count == 1U);
    TEST_ASSERT(region.snapshot.last_time_us == 32U);
    TEST_ASSERT(region.snapshot.min_time_us == 32U);
    TEST_ASSERT(region.snapshot.max_time_us == 32U);
    TEST_ASSERT(region.snapshot.sample_count == 1U);

    g_fake_dwt_cyccnt = 200U;
    tm_task_perf_begin(&region);
    g_fake_dwt_cyccnt = 212U;
    tm_task_perf_end(&region);

    TEST_ASSERT(g_irq_enter_count == 2U);
    TEST_ASSERT(g_irq_exit_count == 2U);
    TEST_ASSERT(region.snapshot.last_time_us == 12U);
    TEST_ASSERT(region.snapshot.min_time_us == 12U);
    TEST_ASSERT(region.snapshot.max_time_us == 32U);
    TEST_ASSERT(region.snapshot.sample_count == 2U);
}

int main(void)
{
    test_tm_task_perf_end_publishes_snapshot_inside_irq_critical_section();

    puts("[test_tm_task_perf] passed");
    return 0;
}
