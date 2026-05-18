/**
 * @file    test_tm_task_budget_probe_failfast.c
 * @brief   Host fail-fast tests for budget probe registration overflow
 * @version v1.0.0
 * @date    2026.04.29
 */

#include <stdint.h>
#include <stdio.h>

#include "host_rtos.h"
#include "tm_task_budget_probe.h"

static void test_thread_entry(void *argument)
{
    (void)argument;
}

int main(void)
{
    host_rtos_reset();

    for (uint32_t i = 0U; i < TM_TASK_BUDGET_PROBE_MAX_TASKS; i++) {
        char         *name;
        osThreadId_t  thread;
        static char   names[TM_TASK_BUDGET_PROBE_MAX_TASKS + 1U][8];

        (void)snprintf(names[i], sizeof(names[i]), "t%lu", (unsigned long)i);
        name   = names[i];
        thread = osThreadNew(test_thread_entry, NULL, &(const osThreadAttr_t){.name = name});
        if (thread == NULL) {
            return 2;
        }

        tm_task_budget_probe_register(name, thread, 256U + i);
    }

    puts("[test_tm_task_budget_probe_failfast] overflow registration should assert");
    tm_task_budget_probe_register("overflow",
                                  osThreadNew(test_thread_entry,
                                              NULL,
                                              &(const osThreadAttr_t){.name = "overflow"}),
                                  1024U);
    return 0;
}
