/**
 * @file    test_motor_fsm_fault_clear_contract.c
 * @brief   Host contract tests for FalconFoc fault-clear FSM behavior
 * @version v1.0.0
 * @date    2026.05.15
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foc_param.h"
#include "motor_fsm.h"

#define TEST_ASSERT(expr)                                                                      \
    do {                                                                                       \
        if (!(expr)) {                                                                         \
            fprintf(stderr,                                                                    \
                    "[test_motor_fsm_fault_clear_contract] assertion failed: %s (%s:%d)\n",   \
                    #expr,                                                                     \
                    __FILE__,                                                                  \
                    __LINE__);                                                                 \
            exit(1);                                                                           \
        }                                                                                      \
    } while (0)

static uint32_t g_pwm_enable_count = 0U;
static uint32_t g_pwm_disable_count = 0U;

void shell_printf(const char *fmt, ...)
{
    (void)fmt;
}

void pwm_out_enable(void)
{
    g_pwm_enable_count++;
}

void pwm_out_disable(void)
{
    g_pwm_disable_count++;
}

void motor_calib_prepare_entry(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_state.target_vel = 0.0f;
    motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
    motor->input_mode            = INPUT_MODE_INACTIVE;
}

void motor_restore_after_param_load(struct motor_dev *motor)
{
    if (motor == NULL) {
        return;
    }

    motor->ctrl_state.target_vel = 0.0f;
    motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
}

void foc_calib_enc_init(struct motor_dev *motor)
{
    (void)motor;
}

int foc_param_load_enc(struct motor_dev *motor)
{
    (void)motor;
    return PARAM_EMPTY;
}

const char *foc_param_status_str(int st)
{
    (void)st;
    return "EMPTY";
}

static void test_fault_clear_clears_fault_latch_and_returns_idle(void)
{
    struct motor_dev      motor = {0};
    struct encoder_config cfg   = {0};

    g_pwm_enable_count  = 0U;
    g_pwm_disable_count = 0U;

    motor.enc_pri.config = &cfg;

    motor_fsm_init(&motor);

    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(g_pwm_enable_count >= 1U);

    motor.fault_code = 0x000000A5U;
    TEST_ASSERT(motor_fsm_dispatch(&motor, EV_CMD_DISABLE) == 0);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_FAULT);
    TEST_ASSERT(g_pwm_disable_count >= 1U);
    TEST_ASSERT(motor.fault_code == 0x000000A5U);

    TEST_ASSERT(motor_fsm_dispatch(&motor, EV_CMD_CLR_FAULT) == 0);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(motor.fault_code == 0U);
    TEST_ASSERT(g_pwm_enable_count >= 1U);
}

int main(void)
{
    test_fault_clear_clears_fault_latch_and_returns_idle();

    puts("[test_motor_fsm_fault_clear_contract] passed");
    return 0;
}
