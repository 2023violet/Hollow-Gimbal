/**
 * @file    test_motor_fault_context_contract.c
 * @brief   Host contract tests for latched motor fault diagnostics
 * @version v1.0.0
 * @date    2026.08.28
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foc_common.h"
#include "foc_config.h"
#include "motor_fault_runtime.h"

#define TEST_ASSERT(expr)                                                               \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            fprintf(stderr,                                                             \
                    "[test_motor_fault_context_contract] assertion failed: %s (%s:%d)\n", \
                    #expr,                                                              \
                    __FILE__,                                                           \
                    __LINE__);                                                          \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

static uint32_t              g_pwm_disable_calls;
static bool                  g_context_visible_before_pwm_disable;
static struct motor_fault_context g_context_at_pwm_disable;

void pwm_out_disable(void)
{
    g_pwm_disable_calls++;
    g_context_visible_before_pwm_disable =
        motor_fault_context_read(&g_context_at_pwm_disable);
}

static void test_env_reset(struct motor_dev *motor)
{
    memset(motor, 0, sizeof(*motor));
    motor->axis_state                 = AXIS_STATE_RUNNING;
    motor->ctrl_mode                  = CTRL_MODE_VOLT;
    motor->prot_config.v_bus_ov_thr   = DEFAULT_V_BUS_OV;
    motor->prot_config.v_bus_uv_thr   = DEFAULT_V_BUS_UV;
    motor->prot_config.vel_over_thr   = DEFAULT_VEL_OVER;
    motor->prot_config.motor_temp_thr = DEFAULT_MOTOR_TEMP;
    motor->prot_config.driver_temp_thr = DEFAULT_DRIVER_TEMP;
    motor->prot_config.i_peak         = DEFAULT_CURR_PEAK;
    motor->meas_data.v_bus            = DEFAULT_VOLT_RATED;
    motor->foc_calc.i_d              = 0.0f;
    motor->foc_calc.i_q              = 0.0f;
    g_pwm_disable_calls               = 0U;
    g_context_visible_before_pwm_disable = false;
    memset(&g_context_at_pwm_disable, 0, sizeof(g_context_at_pwm_disable));
}

static void test_fault_context_latches_before_pwm_disable(void)
{
    struct motor_dev           motor;
    struct motor_fault_context context;

    motor_fault_context_clear();
    TEST_ASSERT(motor_fault_context_read(&context) == false);

    test_env_reset(&motor);
    motor.foc_calc.sector       = 4U;
    motor.foc_calc.v_d          = 0.11f;
    motor.foc_calc.v_q          = 0.22f;
    motor.foc_calc.v_alpha      = 0.33f;
    motor.foc_calc.v_beta       = -0.44f;
    motor.foc_calc.dtc_a        = 0.45f;
    motor.foc_calc.dtc_b        = 0.55f;
    motor.foc_calc.dtc_c        = 0.65f;
    motor.foc_calc.i_d          = -0.12f;
    motor.foc_calc.i_q          = 0.18f;
    motor.meas_data.ia_raw     = 2100.0f;
    motor.meas_data.ib_raw     = 2200.0f;
    motor.meas_data.ic_raw     = 2300.0f;
    motor.meas_data.a          = FOC_FAST_CURRENT_TRIP_A + 0.01f;
    motor.meas_data.b          = -0.03f;
    motor.meas_data.c          = 0.02f;
    motor.meas_data.v_bus_raw  = 13500.0f;
    motor.meas_data.theta_elec = 1.75f;
    motor.ctrl_state.theta_sim = 2.25f;
    motor.ctrl_state.we_volt_now = 1.25f;
    motor.ctrl_state.we_volt_ramp = 3.14f;

    foc_fault_check_fast(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_OVER_CURR) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1U);
    TEST_ASSERT(g_context_visible_before_pwm_disable == true);
    TEST_ASSERT(motor_fault_context_read(&context) == true);
    TEST_ASSERT(context.sequence == 1U);
    TEST_ASSERT(context.fault_code == FAULT_OVER_CURR);
    TEST_ASSERT(context.axis_state == (uint8_t)AXIS_STATE_RUNNING);
    TEST_ASSERT(context.ctrl_mode == (uint8_t)CTRL_MODE_VOLT);
    TEST_ASSERT(context.sector == 4U);
    TEST_ASSERT(context.ia_raw == 2100.0f);
    TEST_ASSERT(context.ib_raw == 2200.0f);
    TEST_ASSERT(context.ic_raw == 2300.0f);
    TEST_ASSERT(context.ia == motor.meas_data.a);
    TEST_ASSERT(context.ib == motor.meas_data.b);
    TEST_ASSERT(context.ic == motor.meas_data.c);
    TEST_ASSERT(context.v_bus_raw == 13500.0f);
    TEST_ASSERT(context.v_bus == DEFAULT_VOLT_RATED);
    TEST_ASSERT(context.id == -0.12f);
    TEST_ASSERT(context.iq == 0.18f);
    TEST_ASSERT(context.theta_sim == 2.25f);
    TEST_ASSERT(context.theta_elec == 1.75f);
    TEST_ASSERT(context.we_volt_now == 1.25f);
    TEST_ASSERT(context.we_volt_ramp == 3.14f);
    TEST_ASSERT(context.vd == 0.11f);
    TEST_ASSERT(context.vq == 0.22f);
    TEST_ASSERT(context.v_alpha == 0.33f);
    TEST_ASSERT(context.v_beta == -0.44f);
    TEST_ASSERT(context.dtc_a == 0.45f);
    TEST_ASSERT(context.dtc_b == 0.55f);
    TEST_ASSERT(context.dtc_c == 0.65f);
}

static void test_fault_context_is_not_overwritten_by_repeated_fault_checks(void)
{
    struct motor_dev           motor;
    struct motor_fault_context first;
    struct motor_fault_context second;

    motor_fault_context_clear();
    test_env_reset(&motor);
    motor.meas_data.a = FOC_FAST_CURRENT_TRIP_A + 0.01f;
    foc_fault_check_fast(&motor);
    TEST_ASSERT(motor_fault_context_read(&first) == true);

    motor.meas_data.a = 0.0f;
    motor.meas_data.b = FOC_FAST_CURRENT_TRIP_A + 0.02f;
    foc_fault_check_fast(&motor);

    TEST_ASSERT(motor_fault_context_read(&second) == true);
    TEST_ASSERT(second.sequence == first.sequence);
    TEST_ASSERT(second.fault_code == first.fault_code);
    TEST_ASSERT(second.ia == first.ia);
    TEST_ASSERT(second.ib == first.ib);
    TEST_ASSERT(g_pwm_disable_calls == 1U);
}

static void test_stale_context_does_not_suppress_new_fault_latch(void)
{
    struct motor_dev            first_motor;
    struct motor_dev            second_motor;
    struct motor_fault_context first;
    struct motor_fault_context second;

    motor_fault_context_clear();
    test_env_reset(&first_motor);
    first_motor.meas_data.a = FOC_FAST_CURRENT_TRIP_A + 0.01f;
    foc_fault_check_fast(&first_motor);
    TEST_ASSERT(motor_fault_context_read(&first) == true);

    test_env_reset(&second_motor);
    second_motor.meas_data.b = FOC_FAST_CURRENT_TRIP_A + 0.02f;
    foc_fault_check_fast(&second_motor);

    TEST_ASSERT((second_motor.fault_code & FAULT_OVER_CURR) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1U);
    TEST_ASSERT(motor_fault_context_read(&second) == true);
    TEST_ASSERT(second.sequence == first.sequence);
    TEST_ASSERT(second.ia == first.ia);
    TEST_ASSERT(second.ib == first.ib);
}

static void test_fault_context_clear_removes_stale_context(void)
{
    struct motor_dev           motor;
    struct motor_fault_context context;

    motor_fault_context_clear();
    test_env_reset(&motor);
    motor.meas_data.a = FOC_FAST_CURRENT_TRIP_A + 0.01f;
    foc_fault_check_fast(&motor);
    TEST_ASSERT(motor_fault_context_read(&context) == true);

    motor_fault_context_clear();
    memset(&context, 0xA5, sizeof(context));
    TEST_ASSERT(motor_fault_context_read(&context) == false);
    TEST_ASSERT(context.sequence == 0U);
    TEST_ASSERT(context.fault_code == FAULT_NONE);
}

int main(void)
{
    test_fault_context_latches_before_pwm_disable();
    test_fault_context_is_not_overwritten_by_repeated_fault_checks();
    test_stale_context_does_not_suppress_new_fault_latch();
    test_fault_context_clear_removes_stale_context();

    puts("[test_motor_fault_context_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
