/**
 * @file    test_foc_motion_runtime_contract.c
 * @brief   Host contract tests for motion runtime owner seams
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foc_motion_runtime.h"

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

int main(void)
{
    test_motion_commands_update_runtime_owned_targets();
    puts("[test_foc_motion_runtime_contract] passed");
    return 0;
}

