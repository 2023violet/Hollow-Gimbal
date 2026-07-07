/**
 * @file    test_motor_runtime_contract.c
 * @brief   Host contract tests for motor runtime owner seams
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "motor_runtime.h"
#include "motor_store.h"

void     host_rtos_reset(void);
void     host_rtos_set_ipsr(uint32_t ipsr);
uint32_t host_rtos_get_isr_critical_enter_count(void);
uint32_t host_rtos_get_isr_critical_exit_count(void);

#define TEST_ASSERT(expr)                                                     \
    do {                                                                      \
        if (!(expr)) {                                                        \
            fprintf(stderr,                                                   \
                    "[test_motor_runtime_contract] assertion failed: %s\n",  \
                    #expr);                                                   \
            exit(1);                                                          \
    }                                                                     \
    } while (0)

static void test_runtime_init_and_clear_hooks(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));

    motor_runtime_init(&motor);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_INACTIVE);

    motor.ctrl_state.target_vel = 9.0f;
    motor.ctrl_state.target_pos = 7.0f;
    motor_runtime_reset_transient_setpoints(&motor);
    TEST_ASSERT(motor.ctrl_state.target_vel == 0.0f);
    TEST_ASSERT(motor.ctrl_state.target_pos == motor.meas_data.out_pos_rad);
}

static void test_runtime_attach_and_current_binding(void)
{
    struct motor_dev motor_a;
    struct motor_dev motor_b;

    memset(&motor_a, 0, sizeof(motor_a));
    memset(&motor_b, 0, sizeof(motor_b));

    motor_runtime_attach(NULL);
    TEST_ASSERT(motor_runtime_current() == NULL);

    motor_runtime_attach(&motor_a);
    TEST_ASSERT(motor_runtime_current() == &motor_a);

    motor_runtime_attach(&motor_b);
    TEST_ASSERT(motor_runtime_current() == &motor_b);
}

static void test_runtime_snapshot_returns_false_and_zeros_when_unattached(void)
{
    struct motor_runtime_snapshot snapshot;

    memset(&snapshot, 0xA5, sizeof(snapshot));

    motor_runtime_attach(NULL);

    TEST_ASSERT(motor_runtime_snapshot(&snapshot) == false);
    TEST_ASSERT(snapshot.axis_state == 0U);
    TEST_ASSERT(snapshot.ctrl_mode == 0U);
    TEST_ASSERT(snapshot.input_mode == 0U);
    TEST_ASSERT(snapshot.sensor_type == 0U);
    TEST_ASSERT(snapshot.fault_code == 0U);
    TEST_ASSERT(snapshot.enc_valid == false);
    TEST_ASSERT(snapshot.v_bus == 0.0f);
    TEST_ASSERT(snapshot.i_q == 0.0f);
    TEST_ASSERT(snapshot.i_d == 0.0f);
    TEST_ASSERT(snapshot.out_vel_rpm == 0.0f);
    TEST_ASSERT(snapshot.out_pos_turns == 0.0f);
    TEST_ASSERT(snapshot.motor_temp == 0.0f);
    TEST_ASSERT(snapshot.driver_temp == 0.0f);
    TEST_ASSERT(snapshot.angle_mech_rad == 0.0f);
    TEST_ASSERT(snapshot.angle_elec_rad == 0.0f);
    TEST_ASSERT(snapshot.encoder_vel_rads == 0.0f);
    TEST_ASSERT(snapshot.save_req == 0U);
    TEST_ASSERT(snapshot.calib_state == 0U);
    TEST_ASSERT(snapshot.rs_state == 0U);
    TEST_ASSERT(snapshot.ls_state == 0U);
    TEST_ASSERT(snapshot.flux_state == 0U);
    TEST_ASSERT(snapshot.js_state == 0U);
    TEST_ASSERT(snapshot.enc_state == 0U);
    TEST_ASSERT(snapshot.mech_offset == 0.0f);
    TEST_ASSERT(snapshot.pole_pairs == 0U);
}

static void test_runtime_snapshot_returns_false_for_null_output(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor_runtime_attach(&motor);

    TEST_ASSERT(motor_runtime_snapshot(NULL) == false);
}

static void test_runtime_snapshot_captures_live_shell_fields(void)
{
    struct motor_dev              motor;
    struct motor_runtime_snapshot snapshot;

    memset(&motor, 0, sizeof(motor));
    memset(&snapshot, 0, sizeof(snapshot));

    motor_runtime_init(&motor);
    motor.axis_state                   = AXIS_STATE_RUNNING;
    motor.ctrl_mode                    = CTRL_MODE_POS;
    motor.input_mode                   = INPUT_MODE_TRAP_TRAJ;
    motor.sensor_type                  = SENSOR_TYPE_INC_ENC;
    motor.fault_code                   = 0x1234ABCDU;
    motor.calib_state.state            = RS_CALC;
    motor.calib_state.rs_state         = RS_END;
    motor.calib_state.ls_state         = LS_END;
    motor.calib_state.flux_state       = FX_END;
    motor.calib_state.js_state         = JS_END;
    motor.calib_state.enc_state        = ENC_LUT_END;
    motor.enc_pri.state.calib_valid    = true;
    motor.enc_pri.config->mech_offset  = 1.5f;
    motor.enc_pri.config->pole_pairs   = 7U;
    motor.meas_data.v_bus              = 51.25f;
    motor.foc_calc.i_q                 = 7.5f;
    motor.foc_calc.i_d                 = -1.25f;
    motor.meas_data.out_vel_rpm        = 345.5f;
    motor.meas_data.out_pos_turns      = 12.75f;
    motor.meas_data.motor_temp         = 48.0f;
    motor.meas_data.driver_temp        = 52.5f;
    motor.enc_pri.state.angle_mech_rad = 1.125f;
    motor.enc_pri.state.angle_elec_rad = 2.25f;
    motor.enc_pri.state.velocity_rads  = 9.5f;
    motor.calib_state.param_save_req   = PARAM_SAVE_MOTOR | PARAM_SAVE_SYS;

    motor_runtime_attach(&motor);
    host_rtos_reset();
    host_rtos_set_ipsr(1U);

    TEST_ASSERT(motor_runtime_snapshot(&snapshot) == true);
    TEST_ASSERT(host_rtos_get_isr_critical_enter_count() == 1U);
    TEST_ASSERT(host_rtos_get_isr_critical_exit_count() == 1U);
    TEST_ASSERT(snapshot.axis_state == (uint8_t)AXIS_STATE_RUNNING);
    TEST_ASSERT(snapshot.ctrl_mode == (uint8_t)CTRL_MODE_POS);
    TEST_ASSERT(snapshot.input_mode == (uint8_t)INPUT_MODE_TRAP_TRAJ);
    TEST_ASSERT(snapshot.sensor_type == (uint8_t)SENSOR_TYPE_INC_ENC);
    TEST_ASSERT(snapshot.fault_code == 0x1234ABCDU);
    TEST_ASSERT(snapshot.calib_state == (uint8_t)RS_CALC);
    TEST_ASSERT(snapshot.rs_state == (uint8_t)RS_END);
    TEST_ASSERT(snapshot.ls_state == (uint8_t)LS_END);
    TEST_ASSERT(snapshot.flux_state == (uint8_t)FX_END);
    TEST_ASSERT(snapshot.js_state == (uint8_t)JS_END);
    TEST_ASSERT(snapshot.enc_state == (uint8_t)ENC_LUT_END);
    TEST_ASSERT(snapshot.enc_valid == true);
    TEST_ASSERT(snapshot.mech_offset == 1.5f);
    TEST_ASSERT(snapshot.pole_pairs == 7U);
    TEST_ASSERT(snapshot.v_bus == 51.25f);
    TEST_ASSERT(snapshot.i_q == 7.5f);
    TEST_ASSERT(snapshot.i_d == -1.25f);
    TEST_ASSERT(snapshot.out_vel_rpm == 345.5f);
    TEST_ASSERT(snapshot.out_pos_turns == 12.75f);
    TEST_ASSERT(snapshot.motor_temp == 48.0f);
    TEST_ASSERT(snapshot.driver_temp == 52.5f);
    TEST_ASSERT(snapshot.angle_mech_rad == 1.125f);
    TEST_ASSERT(snapshot.angle_elec_rad == 2.25f);
    TEST_ASSERT(snapshot.encoder_vel_rads == 9.5f);
    TEST_ASSERT(snapshot.save_req == (uint8_t)(PARAM_SAVE_MOTOR | PARAM_SAVE_SYS));
}

int main(void)
{
    test_runtime_init_and_clear_hooks();
    test_runtime_attach_and_current_binding();
    test_runtime_snapshot_returns_false_and_zeros_when_unattached();
    test_runtime_snapshot_returns_false_for_null_output();
    test_runtime_snapshot_captures_live_shell_fields();
    puts("[test_motor_runtime_contract] passed");
    return 0;
}
