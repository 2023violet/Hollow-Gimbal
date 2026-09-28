/**
 * @file    test_motor_fault_runtime_contract.c
 * @brief   Host contract tests for runtime fault owner boundaries
 * @version v1.0.0
 * @date    2026.05.24
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "foc_common.h"
#include "foc_config.h"
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
    motor_fault_context_clear();
    memset(motor, 0, sizeof(*motor));
    motor->prot_config.v_bus_ov_thr    = DEFAULT_V_BUS_OV;
    motor->prot_config.v_bus_uv_thr    = DEFAULT_V_BUS_UV;
    motor->prot_config.vel_over_thr    = DEFAULT_VEL_OVER;
    motor->prot_config.motor_temp_thr  = DEFAULT_MOTOR_TEMP;
    motor->prot_config.driver_temp_thr = DEFAULT_DRIVER_TEMP;
    motor->prot_config.i_peak          = DEFAULT_CURR_PEAK;
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

static void test_fast_fault_latches_over_current_and_is_idempotent(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.axis_state          = AXIS_STATE_RUNNING;
    motor.meas_data.v_bus     = DEFAULT_VOLT_RATED;
    motor.meas_data.a         = FOC_FAST_CURRENT_TRIP_A + 0.01f;
    motor.meas_data.b         = 0.0f;
    motor.meas_data.c         = 0.0f;

    foc_fault_check_fast(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_OVER_CURR) != 0U);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_RUNNING);
    TEST_ASSERT(g_pwm_disable_calls == 1);

    foc_fault_check_fast(&motor);
    TEST_ASSERT(g_pwm_disable_calls == 1);
}

static void test_bench_warning_does_not_trip_fast_path_below_absolute_limit(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.axis_state      = AXIS_STATE_RUNNING;
    motor.meas_data.v_bus = DEFAULT_VOLT_RATED;
    motor.meas_data.a     = FOC_WARN_CURRENT_A + 0.01f;

    foc_fault_check_fast(&motor);
    foc_fault_check_fast(&motor);
    foc_fault_check_fast(&motor);
    foc_fault_check_fast(&motor);
    TEST_ASSERT(motor.fault_code == FAULT_NONE);
    TEST_ASSERT(g_pwm_disable_calls == 0);
}

static void test_open_loop_does_not_use_closed_loop_stall_trip(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    foc_fault_runtime_reset();
    motor.axis_state            = AXIS_STATE_RUNNING;
    motor.ctrl_mode             = CTRL_MODE_VOLT;
    motor.prot_config.i_rated   = DEFAULT_CURR_RATED;
    motor.meas_data.v_bus       = DEFAULT_VOLT_RATED;
    motor.meas_data.out_vel_rads = 0.0f;
    motor.ctrl_state.vq_set     = 0.5f;
    motor.foc_calc.i_q          = FOC_STALL_CURRENT_A + 0.01f;

    for (uint32_t i = 0U; i < (FOC_STALL_TRIP_MS + 10U); i++) {
        foc_fault_check(&motor);
    }

    TEST_ASSERT(motor.fault_code == FAULT_NONE);
    TEST_ASSERT(g_pwm_disable_calls == 0);
}

static void test_protection_envelope_matches_motor_bringup_spec(void)
{
    /* 2026-08-31 bench: the 2804 detent/stiction needs the datasheet 2 A
     * peak headroom; the trip family is 1.6 A fast / 1.8 A task-level. */
    TEST_ASSERT(fabsf(FOC_FAST_CURRENT_TRIP_A - 1.6f) < 0.0001f);
    TEST_ASSERT(fabsf(FOC_WARN_CURRENT_A - 1.5f) < 0.0001f);
    TEST_ASSERT(fabsf(FOC_STALL_CURRENT_A - 1.5f) < 0.0001f);
    TEST_ASSERT(FOC_FAST_CURRENT_TRIP_A > FOC_WARN_CURRENT_A);
    TEST_ASSERT(FOC_WARN_CURRENT_A > DEFAULT_CURR_RATED);
    TEST_ASSERT(FOC_FAST_CURRENT_TRIP_A < DEFAULT_CURR_PEAK);
}

static void test_fast_fault_latches_bus_voltage_limits(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.axis_state      = AXIS_STATE_RUNNING;
    motor.meas_data.v_bus = DEFAULT_V_BUS_UV - 0.1f;
    foc_fault_check_fast(&motor);
    TEST_ASSERT((motor.fault_code & FAULT_UNDER_VOLT) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1);

    test_env_reset(&motor);
    motor.axis_state      = AXIS_STATE_RUNNING;
    motor.meas_data.v_bus = DEFAULT_V_BUS_OV + 0.1f;
    foc_fault_check_fast(&motor);
    TEST_ASSERT((motor.fault_code & FAULT_OVER_VOLT) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1);
}

static void test_fast_fault_latches_nonfinite_measurement(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    motor.axis_state      = AXIS_STATE_RUNNING;
    motor.meas_data.v_bus = 12.0f;
    motor.meas_data.b     = NAN;

    foc_fault_check_fast(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_SENSOR_ERROR) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1);

    test_env_reset(&motor);
    motor.axis_state      = AXIS_STATE_RUNNING;
    motor.meas_data.v_bus = INFINITY;

    foc_fault_check_fast(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_SENSOR_ERROR) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1);
}

static void test_bench_timeout_latches_fault_and_preserves_context(void)
{
    struct motor_dev motor;
    struct motor_fault_context context;

    test_env_reset(&motor);
    motor.axis_state       = AXIS_STATE_RUNNING;
    motor.ctrl_mode        = CTRL_MODE_VOLT;
    motor.meas_data.v_bus  = 12.0f;
    motor.foc_calc.v_d     = 0.05f;
    motor.foc_calc.dtc_a   = 0.49f;
    motor.foc_calc.dtc_b   = 0.50f;
    motor.foc_calc.dtc_c   = 0.51f;
    motor.foc_calc.sector  = 2U;

    foc_fault_bench_timeout(&motor);

    TEST_ASSERT((motor.fault_code & FAULT_COMM_ERROR) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1);
    TEST_ASSERT(motor_fault_context_read(&context));
    TEST_ASSERT(context.fault_code == FAULT_COMM_ERROR);
    TEST_ASSERT(context.vd == 0.05f);
    TEST_ASSERT(context.sector == 2U);

    foc_fault_bench_timeout(&motor);
    TEST_ASSERT(g_pwm_disable_calls == 1);
}

static void test_task_fault_check_trips_after_warning_persists(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    foc_fault_runtime_reset();
    motor.axis_state      = AXIS_STATE_RUNNING;
    motor.prot_config.i_rated = DEFAULT_CURR_RATED;
    motor.meas_data.v_bus = DEFAULT_VOLT_RATED;
    motor.foc_calc.i_q    = FOC_WARN_CURRENT_A + 0.01f;

    for (uint32_t i = 0U; i < (FOC_WARNING_TRIP_MS - 1U); i++) {
        foc_fault_check(&motor);
    }
    TEST_ASSERT(motor.fault_code == FAULT_NONE);
    TEST_ASSERT(g_pwm_disable_calls == 0);

    foc_fault_check(&motor);
    TEST_ASSERT((motor.fault_code & FAULT_OVER_CURR) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1);
}

static void test_task_fault_check_trips_stall_before_warning_timeout(void)
{
    struct motor_dev motor;

    test_env_reset(&motor);
    foc_fault_runtime_reset();
    motor.axis_state          = AXIS_STATE_RUNNING;
    motor.ctrl_mode           = CTRL_MODE_CURR;
    motor.prot_config.i_rated = DEFAULT_CURR_RATED;
    motor.meas_data.v_bus     = DEFAULT_VOLT_RATED;
    motor.meas_data.out_vel_rads = 0.0f;
    motor.ctrl_state.iq_set   = FOC_STALL_CURRENT_A + 0.01f;
    motor.foc_calc.i_q        = FOC_STALL_CURRENT_A + 0.01f;

    for (uint32_t i = 0U; i < (FOC_STALL_TRIP_MS - 1U); i++) {
        foc_fault_check(&motor);
    }
    TEST_ASSERT(motor.fault_code == FAULT_NONE);

    foc_fault_check(&motor);
    TEST_ASSERT((motor.fault_code & FAULT_OVER_CURR) != 0U);
    TEST_ASSERT(g_pwm_disable_calls == 1);
}

int main(void)
{
    test_fault_check_latches_fault_and_keeps_axis_owner_with_fsm();
    test_fault_check_ignores_null_motor();
    test_fast_fault_latches_over_current_and_is_idempotent();
    test_bench_warning_does_not_trip_fast_path_below_absolute_limit();
    test_open_loop_does_not_use_closed_loop_stall_trip();
    test_protection_envelope_matches_motor_bringup_spec();
    test_fast_fault_latches_bus_voltage_limits();
    test_fast_fault_latches_nonfinite_measurement();
    test_bench_timeout_latches_fault_and_preserves_context();
    test_task_fault_check_trips_after_warning_persists();
    test_task_fault_check_trips_stall_before_warning_timeout();

    puts("[test_motor_fault_runtime_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
