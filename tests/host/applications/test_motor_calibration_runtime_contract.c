/**
 * @file    test_motor_calibration_runtime_contract.c
 * @brief   Host contract tests for calibration runtime owner seams
 */

#include <math.h>
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

static void test_current_gain_helper_accepts_null(void)
{
    foc_curr_gain_calc(NULL);
}

static void test_current_gain_helper_falls_back_for_invalid_bandwidth(void)
{
    const float invalid_values[] = {0.0f, -1.0f, NAN, INFINITY};
    const float expected_iq_kp = DEFAULT_LS * FOC_HZ_TO_RADS(DEFAULT_CURR_BW_HZ);
    const float expected_id_kp = expected_iq_kp * DEFAULT_D_AXIS_BW_RATIO;

    for (size_t i = 0U; i < sizeof(invalid_values) / sizeof(invalid_values[0]); ++i) {
        struct motor_dev motor;

        memset(&motor, 0, sizeof(motor));
        motor.hw_config.ls = DEFAULT_LS;
        motor.hw_config.rs = DEFAULT_RS;
        motor.ctrl_config.curr_bandwidth = invalid_values[i];

        foc_curr_gain_calc(&motor);

        TEST_ASSERT(fabsf(motor.ctrl_config.iq_pid.config.kp - expected_iq_kp) < 1.0e-6f);
        TEST_ASSERT(fabsf(motor.ctrl_config.id_pid.config.kp - expected_id_kp) < 1.0e-6f);
        TEST_ASSERT(isfinite(motor.ctrl_config.iq_pid.config.ki));
        TEST_ASSERT(isfinite(motor.ctrl_config.id_pid.config.ki));
    }
}

int main(void)
{
    test_refresh_and_gain_helpers_accept_motor();
    test_current_gain_helper_accepts_null();
    test_current_gain_helper_falls_back_for_invalid_bandwidth();
    puts("[test_motor_calibration_runtime_contract] passed");
    return 0;
}

