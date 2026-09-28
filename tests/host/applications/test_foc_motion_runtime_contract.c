/**
 * @file    test_foc_motion_runtime_contract.c
 * @brief   Host contract tests for motion runtime owner seams
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "foc_motion_runtime.h"
#include "foc_math.h"

#define TEST_ASSERT(expr)                                                        \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(stderr,                                                      \
                    "[test_foc_motion_runtime_contract] assertion failed: %s\n", \
                    #expr);                                                      \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

static void test_motion_commands_update_runtime_owned_targets(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.ctrl_state.iq_max   = 5.0f;
    motor.ctrl_state.vel_max  = 10.0f;
    motor.ctrl_state.dyn_iq_limit  = 5.0f;
    motor.ctrl_state.dyn_vel_limit = 10.0f;
    motor.hw_config.gear_ratio = 1.0f;
    motor.hw_config.gear_eff   = 1.0f;
    motor.calc_params.inv_kt   = 1.0f;

    foc_cmd_passthrough(&motor, CTRL_MODE_VEL, 8.0f, 4.0f);
    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_VEL);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_PASSTHROUGH);
    TEST_ASSERT(motor.ctrl_state.target_vel == 8.0f);

    foc_cmd_vel_ramp(&motor, 6.0f, 2.0f);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_VEL_RAMP);

    foc_cmd_torque_ramp(&motor, 3.0f, 1.0f);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_TORQUE_RAMP);

    foc_cmd_pos_traj(&motor, 12.0f, 5.0f, 4.0f, 3.0f);
    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_POS);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_TRAP_TRAJ);
    TEST_ASSERT(motor.ctrl_state.target_pos == 12.0f);
}

static void test_voltage_entry_seeds_simulated_angle_from_encoder_in_idle(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.axis_state                = AXIS_STATE_IDLE;
    motor.ctrl_mode                 = CTRL_MODE_VOLT;
    motor.ctrl_state.theta_sim      = 0.0f;
    motor.ctrl_state.we_volt_now    = 7.0f;
    motor.meas_data.theta_elec      = 0.0f;
    motor.enc_pri.state.angle_elec_rad = 3.35f;

    foc_cmd_voltage_set(&motor, 0.0f, 0.10f, 3.14f);

    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - 3.35f) < 0.0001f);
    TEST_ASSERT(motor.ctrl_state.we_volt_now == 0.0f);
    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_INACTIVE);
}

static void test_voltage_entry_wraps_encoder_angle(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.axis_state           = AXIS_STATE_IDLE;
    motor.ctrl_mode            = CTRL_MODE_VOLT;
    motor.meas_data.theta_elec = 0.0f;
    motor.enc_pri.state.angle_elec_rad = FOC_2PI + 0.25f;

    foc_cmd_voltage_set(&motor, 0.0f, 0.10f, 3.14f);

    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - 0.25f) < 0.0001f);
    TEST_ASSERT(motor.ctrl_state.theta_sim >= 0.0f);
    TEST_ASSERT(motor.ctrl_state.theta_sim < FOC_2PI);
}

static void test_voltage_if_entry_uses_same_angle_boundary(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.axis_state             = AXIS_STATE_IDLE;
    motor.ctrl_mode              = CTRL_MODE_VOLT;
    motor.ctrl_state.we_volt_now = 5.0f;
    motor.meas_data.theta_elec   = 0.0f;
    motor.enc_pri.state.angle_elec_rad = 5.75f;

    foc_cmd_voltage_if(&motor, 0.05f, -3.14f);

    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - 5.75f) < 0.0001f);
    TEST_ASSERT(motor.ctrl_state.we_volt_now == 0.0f);
    TEST_ASSERT(motor.ctrl_state.if_enable == 1U);
}

static void test_voltage_entry_applies_bench_phase_seed_offset(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.axis_state = AXIS_STATE_IDLE;
    motor.ctrl_mode = CTRL_MODE_VOLT;
    motor.enc_pri.state.angle_elec_rad = 1.0f;
    motor.ctrl_state.theta_seed_offset_rad = FOC_PI * 0.5f;

    foc_cmd_voltage_set(&motor, 0.0f, 0.10f, 0.0f);

    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - (1.0f + FOC_PI * 0.5f)) < 0.0001f);
    TEST_ASSERT(motor.ctrl_state.we_volt_now == 0.0f);
}

static void test_voltage_entry_wraps_positive_and_negative_seed_offsets(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.axis_state = AXIS_STATE_IDLE;
    motor.ctrl_mode = CTRL_MODE_VOLT;
    motor.enc_pri.state.angle_elec_rad = 5.5f;

    motor.ctrl_state.theta_seed_offset_rad = FOC_PI;
    foc_cmd_voltage_set(&motor, 0.0f, 0.10f, 0.0f);
    TEST_ASSERT(motor.ctrl_state.theta_sim >= 0.0f);
    TEST_ASSERT(motor.ctrl_state.theta_sim < FOC_2PI);
    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - (5.5f + FOC_PI - FOC_2PI)) < 0.0001f);

    motor.ctrl_state.theta_seed_offset_rad = -FOC_PI;
    foc_cmd_voltage_set(&motor, 0.0f, 0.10f, 0.0f);
    TEST_ASSERT(motor.ctrl_state.theta_sim >= 0.0f);
    TEST_ASSERT(motor.ctrl_state.theta_sim < FOC_2PI);
    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - (5.5f - FOC_PI)) < 0.0001f);
}

static void test_phase_voltage_does_not_use_encoder_or_seed_offset(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.axis_state = AXIS_STATE_IDLE;
    motor.ctrl_mode = CTRL_MODE_VOLT;
    motor.enc_pri.state.angle_elec_rad = 2.0f;
    motor.ctrl_state.theta_seed_offset_rad = FOC_PI * 0.5f;

    foc_cmd_phase_volt(&motor, 1, 0.10f);

    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - (FOC_2PI / 3.0f)) < 0.0001f);
}

static void test_running_voltage_command_does_not_jump_simulated_angle(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.axis_state             = AXIS_STATE_RUNNING;
    motor.ctrl_mode              = CTRL_MODE_VOLT;
    motor.ctrl_state.theta_sim   = 1.25f;
    motor.ctrl_state.we_volt_now = 1.0f;
    motor.meas_data.theta_elec   = 5.75f;

    foc_cmd_voltage_set(&motor, 0.0f, 0.20f, 3.14f);

    TEST_ASSERT(fabsf(motor.ctrl_state.theta_sim - 1.25f) < 0.0001f);
    TEST_ASSERT(motor.ctrl_state.we_volt_now == 1.0f);
}

int main(void)
{
    test_motion_commands_update_runtime_owned_targets();
    test_voltage_entry_seeds_simulated_angle_from_encoder_in_idle();
    test_voltage_entry_wraps_encoder_angle();
    test_voltage_if_entry_uses_same_angle_boundary();
    test_voltage_entry_applies_bench_phase_seed_offset();
    test_voltage_entry_wraps_positive_and_negative_seed_offsets();
    test_phase_voltage_does_not_use_encoder_or_seed_offset();
    test_running_voltage_command_does_not_jump_simulated_angle();
    puts("[test_foc_motion_runtime_contract] passed");
    return 0;
}

