/**
 * @file    test_app_runtime_budget_contract.c
 * @brief   Host tests for application runtime budget contracts
 * @version v1.0.0
 * @date    2026.05.11
 */

#include <stdio.h>
#include <stdlib.h>

#include "motor_task_config.h"
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
    TEST_ASSERT(TM_APP_TASK_STACK_SHELL_BYTES >= TM_SHELL_TASK_STACK_BYTES);
    TEST_ASSERT(TM_SHELL_TASK_STACK_BYTES == (192U * 4U));
    TEST_ASSERT(TM_APP_TASK_STACK_SHELL_BYTES == (384U * 4U));
}

static void test_motor_task_stack_budget_is_explicit(void)
{
    TEST_ASSERT(TM_MOTOR_TASK_STACK_BYTES == (512U * 4U));
    TEST_ASSERT(TM_MOTOR_SAFE_TASK_STACK_BYTES == (256U * 4U));
}

int main(void)
{
    test_shell_stack_budget_matches_profile_contract();
    test_motor_task_stack_budget_is_explicit();

    puts("[test_app_runtime_budget_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
