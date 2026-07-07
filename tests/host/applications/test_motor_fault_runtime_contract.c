/**
 * @file    test_motor_fault_runtime_contract.c
 * @brief   Host contract tests for runtime fault owner boundaries
 * @version v1.0.0
 * @date    2026.05.24
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foc_common.h"
#include "motor_fault_runtime.h"

#define TEST_ASSERT(expr)                                                            \
    do {                                                                             \
        if (!(expr)) {                                                               \
            fprintf(stderr,                                                          \
                    "[test_motor_fault_runtime_contract] assertion failed: %s\n",    \
                    #expr);                                                          \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

static int g_pwm_disable_calls;

void pwm_out_disable(void)
{
    g_pwm_disable_calls++;
}

static void test_env_reset(struct motor_dev *motor)
{
    memset(motor, 0, sizeof(*motor));
    g_pwm_disable_calls = 0;
}

static void test_fault_check_latches_fault_and_keeps_axis_owner_with_fsm(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.axis_state           = AXIS_STATE_RUNNING;
    motor.prot_config.i_rated  = 2.0f;
    motor.foc_calc.i_q         = 4.0f;
    motor.foc_calc.i_d         = 0.0f;

    foc_fault_check(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_OVER_CURR) != 0U);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_RUNNING);
    TEST_ASSERT(g_pwm_disable_calls == 1);
}

static void test_fault_check_ignores_null_motor(void)
{
    g_pwm_disable_calls = 0;
    foc_fault_check(NULL);
    TEST_ASSERT(g_pwm_disable_calls == 0);
}

int main(void)
{
    test_fault_check_latches_fault_and_keeps_axis_owner_with_fsm();
    test_fault_check_ignores_null_motor();

    puts("[test_motor_fault_runtime_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
