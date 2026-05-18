/**
 * @file    test_tm_runtime.c
 * @brief   Host-side tests for Themis runtime helpers
 * @version v1.0.0
 * @date    2026.03.30
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_rtos.h"
#include "task.h"
#include "tm_runtime.h"

int tm_runtime_weak_write_stub_for_test(int file, char *ptr, int len);

#define TEST_ASSERT(expr)                                               \
    do {                                                                \
        if (!(expr)) {                                                  \
            fprintf(stderr,                                             \
                    "[test_tm_runtime] assertion failed: %s (%s:%d)\n", \
                    #expr,                                              \
                    __FILE__,                                           \
                    __LINE__);                                          \
            exit(1);                                                    \
        }                                                               \
    } while (0)

static void test_tm_runtime_tick_queries_follow_scheduler_and_context_contract(void)
{
    host_rtos_reset();
    host_rtos_set_scheduler_state(taskSCHEDULER_NOT_STARTED);

    TEST_ASSERT(!tm_scheduler_is_running());
    TEST_ASSERT(tm_tick_get() == 0U);
    TEST_ASSERT(tm_tick_get_ms() == 0U);

    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    host_rtos_set_tick_count(123U);
    host_rtos_set_ipsr(0U);
    TEST_ASSERT(tm_scheduler_is_running());
    TEST_ASSERT(tm_tick_get() == 123U);
    TEST_ASSERT(tm_tick_get_ms() == 123U);

    host_rtos_set_tick_count(456U);
    host_rtos_set_ipsr(7U);
    TEST_ASSERT(tm_is_in_isr());
    TEST_ASSERT(tm_tick_get() == 456U);
    TEST_ASSERT(tm_tick_get_ms() == 456U);
}

static void test_tm_runtime_conversions_and_heap_guards_work(void)
{
    void *memory;
    void *zeroed;

    host_rtos_reset();
    TEST_ASSERT(tm_tick_hz_get() == 1000U);
    TEST_ASSERT(tm_ms_to_ticks_ceil(0U) == 0U);
    TEST_ASSERT(tm_ms_to_ticks_ceil(1U) == 1U);
    TEST_ASSERT(tm_ms_to_ticks_ceil(25U) == 25U);
    TEST_ASSERT(tm_ticks_to_ms(0U) == 0U);
    TEST_ASSERT(tm_ticks_to_ms(1U) == 1U);
    TEST_ASSERT(tm_ticks_to_ms(25U) == 25U);

    host_rtos_set_ipsr(0U);
    memory = tm_malloc(16U);
    TEST_ASSERT(memory != NULL);
    tm_free(memory);

    zeroed = tm_calloc(2U, 8U);
    TEST_ASSERT(zeroed != NULL);
    tm_free(zeroed);

    host_rtos_set_ipsr(3U);
    TEST_ASSERT(tm_malloc(16U) == NULL);
    TEST_ASSERT(tm_calloc(2U, 8U) == NULL);
}

static void test_tm_runtime_elapsed_helper_updates_baseline(void)
{
    uint32_t last_tick = 0U;

    host_rtos_reset();
    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    host_rtos_set_ipsr(0U);

    host_rtos_set_tick_count(0U);
    TEST_ASSERT(tm_tick_elapsed_ms_update(&last_tick) == 0U);
    TEST_ASSERT(last_tick == 0U);

    host_rtos_set_tick_count(15U);
    TEST_ASSERT(tm_tick_elapsed_ms_update(&last_tick) == 15U);
    TEST_ASSERT(last_tick == 15U);

    host_rtos_set_tick_count(15U);
    TEST_ASSERT(tm_tick_elapsed_ms_update(&last_tick) == 0U);
    TEST_ASSERT(last_tick == 15U);

    TEST_ASSERT(tm_tick_elapsed_ms_update(NULL) == 0U);
}

static void test_tm_runtime_critical_sections_choose_the_right_backend(void)
{
    uint32_t level;

    host_rtos_reset();
    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    host_rtos_set_ipsr(0U);

    tm_task_critical_enter();
    tm_task_critical_exit();
    TEST_ASSERT(host_rtos_get_suspend_all_count() == 1U);
    TEST_ASSERT(host_rtos_get_resume_all_count() == 1U);

    level = tm_irq_critical_enter();
    TEST_ASSERT(level == 0U);
    tm_irq_critical_exit(level);
    TEST_ASSERT(host_rtos_get_task_critical_enter_count() == 1U);
    TEST_ASSERT(host_rtos_get_task_critical_exit_count() == 1U);

    host_rtos_reset();
    host_rtos_set_scheduler_state(taskSCHEDULER_NOT_STARTED);
    tm_task_critical_enter();
    tm_task_critical_exit();
    TEST_ASSERT(host_rtos_get_suspend_all_count() == 0U);
    TEST_ASSERT(host_rtos_get_resume_all_count() == 0U);

    host_rtos_reset();
    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    host_rtos_set_ipsr(11U);
    tm_task_critical_enter();
    tm_task_critical_exit();
    TEST_ASSERT(host_rtos_get_suspend_all_count() == 0U);
    TEST_ASSERT(host_rtos_get_resume_all_count() == 0U);

    level = tm_irq_critical_enter();
    TEST_ASSERT(level == 1U);
    tm_irq_critical_exit(level);
    TEST_ASSERT(host_rtos_get_isr_critical_enter_count() == 1U);
    TEST_ASSERT(host_rtos_get_isr_critical_exit_count() == 1U);
}

static void test_tm_runtime_write_stub_reports_missing_backend(void)
{
    char buffer[] = "probe";

    TEST_ASSERT(tm_runtime_weak_write_stub_for_test(1, buffer, (int)strlen(buffer)) == -ENOSYS);
}

int main(void)
{
    test_tm_runtime_tick_queries_follow_scheduler_and_context_contract();
    test_tm_runtime_conversions_and_heap_guards_work();
    test_tm_runtime_elapsed_helper_updates_baseline();
    test_tm_runtime_critical_sections_choose_the_right_backend();
    test_tm_runtime_write_stub_reports_missing_backend();

    puts("[test_tm_runtime] passed");
    return 0;
}
