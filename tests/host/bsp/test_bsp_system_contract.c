/**
 * @file    test_bsp_system_contract.c
 * @brief   Host-side contract tests for the FalconFoc bsp system port
 * @version v1.0.0
 * @date    2026.06.02
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "host_rtos.h"
#include "bsp_system.h"
#include "task.h"

#define TEST_ASSERT(expr)                                                   \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr,                                                 \
                    "[test_bsp_system_contract] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                  \
                    __FILE__,                                               \
                    __LINE__);                                              \
            exit(1);                                                        \
        }                                                                   \
    } while (0)

static void test_bsp_system_reset_accepts_thread_mode_callers(void)
{
    host_rtos_reset();
    host_rtos_set_ipsr(0U);
    host_rtos_set_scheduler_state(taskSCHEDULER_NOT_STARTED);

    TEST_ASSERT(bsp_system_reset() == 0);

    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);
    TEST_ASSERT(bsp_system_reset() == 0);
}

static void test_bsp_system_reset_rejects_isr_callers(void)
{
    host_rtos_reset();
    host_rtos_set_ipsr(9U);
    host_rtos_set_scheduler_state(taskSCHEDULER_RUNNING);

    TEST_ASSERT(bsp_system_reset() == -EPERM);
}

int main(void)
{
    test_bsp_system_reset_accepts_thread_mode_callers();
    test_bsp_system_reset_rejects_isr_callers();

    puts("[test_bsp_system_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
