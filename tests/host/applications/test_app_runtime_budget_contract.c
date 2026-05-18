/**
 * @file    test_app_runtime_budget_contract.c
 * @brief   Host tests for application runtime budget contracts
 * @version v1.0.0
 * @date    2026.05.11
 */

#include <stdio.h>
#include <stdlib.h>

#include "config/tm_core_config.h"

#define TEST_ASSERT(expr)                                                             \
    do {                                                                              \
        if (!(expr)) {                                                                \
            fprintf(stderr,                                                           \
                    "[test_app_runtime_budget_contract] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                            \
                    __FILE__,                                                         \
                    __LINE__);                                                        \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

static void test_shell_stack_budget_matches_profile_contract(void)
{
    TEST_ASSERT(TM_APP_TASK_STACK_SHELL_BYTES == TM_SHELL_TASK_STACK_BYTES);

#if TM_DEVDRV_PROFILE_TINY
    TEST_ASSERT(TM_SHELL_TASK_STACK_BYTES == (384U * 4U));
#else
    /*
     * Standard/full-diag now drive settings persistence and repeated long-line
     * shell formatting through the shared shell owner on real hardware. Keep a
     * wider stack budget than tiny so those maintenance paths do not run on a
     * near-overflow envelope.
     */
    TEST_ASSERT(TM_SHELL_TASK_STACK_BYTES >= (512U * 4U));
#endif
}

int main(void)
{
    test_shell_stack_budget_matches_profile_contract();

    puts("[test_app_runtime_budget_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
