/**
 * @file    test_motor_calibration_runtime_contract.c
 * @brief   Host contract tests for calibration runtime owner seams
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "motor_calibration_runtime.h"

#define TEST_ASSERT(expr)                                                          \
    do {                                                                           \
        if (!(expr)) {                                                             \
            fprintf(stderr,                                                        \
                    "[test_motor_calibration_runtime_contract] assertion failed: %s\n", \
                    #expr);                                                        \
            exit(1);                                                               \
        }                                                                          \
    } while (0)

static void test_refresh_and_gain_helpers_accept_motor(void)
{
    struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.hw_config.poles          = 7.0f;
    motor.hw_config.flux           = 0.01f;
    motor.hw_config.gear_ratio     = 2.0f;
    motor.hw_config.gear_eff       = 0.9f;
    motor.hw_config.rs             = 0.2f;
    motor.hw_config.ls             = 0.001f;
    motor.prot_config.tor_peak     = 2.0f;
    motor.prot_config.i_peak       = 3.0f;
    motor.prot_config.i_rated      = 1.5f;
    motor.prot_config.vel_peak     = 20.0f;
    motor.ctrl_config.curr_bandwidth = 1000.0f;
    motor.ctrl_config.vel_bandwidth  = 100.0f;
    motor.ctrl_config.pos_bandwidth  = 10.0f;
    motor.hw_config.js             = 0.002f;

    foc_calib_runtime_refresh(&motor);
    foc_curr_gain_calc(&motor);
    foc_vel_gain_calc(&motor);
    foc_pos_gain_calc(&motor);

    TEST_ASSERT(motor.calc_params.kt > 0.0f);
    TEST_ASSERT(motor.ctrl_state.iq_max > 0.0f);
    TEST_ASSERT(motor.ctrl_config.pos_pid.config.kp > 0.0f);
}

int main(void)
{
    test_refresh_and_gain_helpers_accept_motor();
    puts("[test_motor_calibration_runtime_contract] passed");
    return 0;
}

