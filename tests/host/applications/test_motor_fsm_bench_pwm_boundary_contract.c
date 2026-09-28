/**
 * @file    test_motor_fsm_bench_pwm_boundary_contract.c
 * @brief   Host contract tests for the bench PWM enable boundary
 * @version v1.0.0
 * @date    2026.08.28
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "foc_param.h"
#include "motor_fsm.h"

#define TEST_ASSERT(expr)                                                                    \
    do {                                                                                     \
        if (!(expr)) {                                                                       \
            fprintf(stderr,                                                                 \
                    "[test_motor_fsm_bench_pwm_boundary_contract] assertion failed: %s\n", \
                    #expr);                                                                  \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

static uint32_t g_pwm_enable_count  = 0U;
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
    motor->ctrl_state.theta_sim     = 0.0f;
    motor->ctrl_state.we_volt_now  = 0.0f;
    motor->ctrl_state.we_volt_ramp = 0.0f;
}

void motor_restore_after_param_load(struct motor_dev *motor)
{
    (void)motor;
}

void foc_calib_enc_init(struct motor_dev *motor)
{
    (void)motor;
}

int motor_calibration_runtime_restore_encoder_after_abort(struct motor_dev *motor)
{
    (void)motor;
    return PARAM_EMPTY;
}

const char *foc_param_status_str(int status)
{
    (void)status;
    return "EMPTY";
}

static void test_bench_init_keeps_pwm_disabled(void)
{
    struct motor_dev motor = {0};

    g_pwm_enable_count  = 0U;
    g_pwm_disable_count = 0U;

    motor_fsm_init(&motor);

    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(g_pwm_enable_count == 0U);
    TEST_ASSERT(g_pwm_disable_count == 1U);
}

static void test_bench_start_enables_pwm_after_transition(void)
{
    struct motor_dev motor = {0};

    g_pwm_enable_count  = 0U;
    g_pwm_disable_count = 0U;

    motor_fsm_init(&motor);
    TEST_ASSERT(motor_fsm_dispatch(&motor, EV_CMD_START) == 0);

    TEST_ASSERT(motor.axis_state == AXIS_STATE_RUNNING);
    TEST_ASSERT(g_pwm_enable_count == 1U);
    TEST_ASSERT(g_pwm_disable_count == 1U);
}

static void test_bench_disable_from_running_reasserts_pwm_off(void)
{
    struct motor_dev motor = {0};

    g_pwm_enable_count  = 0U;
    g_pwm_disable_count = 0U;

    motor_fsm_init(&motor);
    TEST_ASSERT(motor_fsm_dispatch(&motor, EV_CMD_START) == 0);
    TEST_ASSERT(motor_fsm_dispatch(&motor, EV_CMD_DISABLE) == 0);

    TEST_ASSERT(motor.axis_state == AXIS_STATE_FAULT);
    TEST_ASSERT(g_pwm_enable_count == 1U);
    TEST_ASSERT(g_pwm_disable_count == 2U);
}

static void test_bench_disable_preserves_last_foc_snapshot_for_post_stop_diagnostics(void)
{
    struct motor_dev motor = {0};

    g_pwm_enable_count  = 0U;
    g_pwm_disable_count = 0U;

    motor_fsm_init(&motor);
    TEST_ASSERT(motor_fsm_dispatch(&motor, EV_CMD_START) == 0);
    motor.foc_calc.v_d   = 0.12f;
    motor.foc_calc.v_q   = 0.34f;
    motor.foc_calc.dtc_a = 0.45f;
    motor.foc_calc.dtc_b = 0.56f;
    motor.foc_calc.dtc_c = 0.67f;
    motor.foc_calc.sector = 3U;
    motor.ctrl_state.theta_sim     = 1.23f;
    motor.ctrl_state.we_volt_now  = 2.34f;
    motor.ctrl_state.we_volt_ramp = 3.45f;

    TEST_ASSERT(motor_fsm_dispatch(&motor, EV_CMD_DISABLE) == 0);

    TEST_ASSERT(motor.axis_state == AXIS_STATE_FAULT);
    TEST_ASSERT(motor.foc_calc.v_d == 0.12f);
    TEST_ASSERT(motor.foc_calc.v_q == 0.34f);
    TEST_ASSERT(motor.foc_calc.dtc_a == 0.45f);
    TEST_ASSERT(motor.foc_calc.dtc_b == 0.56f);
    TEST_ASSERT(motor.foc_calc.dtc_c == 0.67f);
    TEST_ASSERT(motor.foc_calc.sector == 3U);
    TEST_ASSERT(motor.ctrl_state.theta_sim == 1.23f);
    TEST_ASSERT(motor.ctrl_state.we_volt_now == 2.34f);
    TEST_ASSERT(motor.ctrl_state.we_volt_ramp == 3.45f);
    TEST_ASSERT(g_pwm_disable_count == 2U);
}

int main(void)
{
    test_bench_init_keeps_pwm_disabled();
    test_bench_start_enables_pwm_after_transition();
    test_bench_disable_from_running_reasserts_pwm_off();
    test_bench_disable_preserves_last_foc_snapshot_for_post_stop_diagnostics();

    puts("[test_motor_fsm_bench_pwm_boundary_contract] passed");
    return 0;
}
