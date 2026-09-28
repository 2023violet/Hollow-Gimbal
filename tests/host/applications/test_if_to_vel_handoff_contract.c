/**
 * @file    test_if_to_vel_handoff_contract.c
 * @brief   Host contract tests for the I-f -> closed-loop velocity handoff
 *
 * The handoff must only arm while the axis is RUNNING in voltage mode with
 * the I-f path enabled, and it must transfer the shaft torque bumplessly by
 * seeding the velocity PID integral with the I-f q-current amplitude.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foc_motion_runtime.h"
#include "foc_math.h"

#define TEST_ASSERT(expr)                                                              \
    do {                                                                               \
        if (!(expr)) {                                                                 \
            fprintf(stderr,                                                            \
                    "[test_if_to_vel_handoff_contract] assertion failed: %s\n",        \
                    #expr);                                                            \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

static struct motor_dev *test_motor(void)
{
    static struct motor_dev motor;

    memset(&motor, 0, sizeof(motor));
    motor.ctrl_state.iq_max  = 1.20f;
    motor.ctrl_state.vel_max = FOC_RPM_TO_RADS(120.0f);
    return &motor;
}

static void test_handoff_rejected_when_idle(void)
{
    struct motor_dev *m = test_motor();

    m->axis_state = AXIS_STATE_IDLE;
    m->ctrl_mode  = CTRL_MODE_VOLT;
    m->ctrl_state.if_enable    = 1U;
    m->ctrl_state.if_iq_target = 0.5f;

    TEST_ASSERT(foc_cmd_if_to_vel_handoff(m, -30.0f) == false);
    TEST_ASSERT(m->ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(m->ctrl_state.if_enable == 1U);
}

static void test_handoff_rejected_without_if(void)
{
    struct motor_dev *m = test_motor();

    m->axis_state = AXIS_STATE_RUNNING;
    m->ctrl_mode  = CTRL_MODE_VOLT;
    m->ctrl_state.if_enable    = 0U;
    m->ctrl_state.if_iq_target = 0.0f;

    TEST_ASSERT(foc_cmd_if_to_vel_handoff(m, -30.0f) == false);
    TEST_ASSERT(m->ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(m->axis_state == AXIS_STATE_RUNNING);
}

static void test_handoff_bumpless_transfer(void)
{
    struct motor_dev *m = test_motor();

    m->axis_state = AXIS_STATE_RUNNING;
    m->ctrl_mode  = CTRL_MODE_VOLT;
    m->ctrl_state.if_enable    = 1U;
    m->ctrl_state.if_iq_target = 0.5f;
    m->ctrl_state.iq_max       = 1.20f;
    m->meas_data.out_vel_rads  = -2.0f;

    TEST_ASSERT(foc_cmd_if_to_vel_handoff(m, -30.0f) == true);
    TEST_ASSERT(m->ctrl_mode == CTRL_MODE_VEL);
    TEST_ASSERT(m->input_mode == INPUT_MODE_PASSTHROUGH);
    TEST_ASSERT(m->axis_state == AXIS_STATE_RUNNING);
    TEST_ASSERT(m->ctrl_state.if_enable == 0U);
    TEST_ASSERT(fabsf(m->ctrl_state.vel_set - FOC_RPM_TO_RADS(-30.0f)) < 0.001f);
    /* The passthrough input task copies target_vel into vel_set every tick,
     * so the handoff must own target_vel or the stale zero target pins the
     * motor at standstill (bench finding 2026-08-31). */
    TEST_ASSERT(fabsf(m->ctrl_state.target_vel - FOC_RPM_TO_RADS(-30.0f)) < 0.001f);
    TEST_ASSERT(fabsf(m->ctrl_state.dyn_iq_limit - 1.20f) < 0.001f);
    /* Bumpless: the velocity PID starts at the dragging torque. */
    TEST_ASSERT(fabsf(m->ctrl_config.vel_pid.i_out - 0.5f) < 0.001f);
    TEST_ASSERT(fabsf(m->ctrl_config.vel_pid.output - 0.5f) < 0.001f);
    TEST_ASSERT(m->ctrl_state.we_volt_now == 0.0f);
}

static void test_handoff_rejected_in_wrong_ctrl_mode(void)
{
    struct motor_dev *m = test_motor();

    m->axis_state = AXIS_STATE_RUNNING;
    m->ctrl_mode  = CTRL_MODE_CURR;
    m->ctrl_state.if_enable    = 1U;
    m->ctrl_state.if_iq_target = 0.5f;

    TEST_ASSERT(foc_cmd_if_to_vel_handoff(m, -30.0f) == false);
    TEST_ASSERT(m->ctrl_mode == CTRL_MODE_CURR);
}

static void test_bench_velocity_step_requires_handoff_and_preserves_loop_state(void)
{
    struct motor_dev *m = test_motor();
    float prior_target;
    float prior_vel_set;
    float prior_i_out;
    float prior_output;
    float prior_dyn_iq_limit;

    m->axis_state = AXIS_STATE_RUNNING;
    m->ctrl_mode = CTRL_MODE_VEL;
    m->input_mode = INPUT_MODE_PASSTHROUGH;
    m->ctrl_state.target_vel = FOC_RPM_TO_RADS(-30.0f);
    prior_target = m->ctrl_state.target_vel;
    TEST_ASSERT(foc_cmd_bench_velocity_step(m, 60.0f) == false);
    TEST_ASSERT(m->ctrl_state.target_vel == prior_target);

    m->ctrl_mode = CTRL_MODE_VOLT;
    m->ctrl_state.if_enable = 1U;
    m->ctrl_state.if_iq_target = 0.50f;
    TEST_ASSERT(foc_cmd_if_to_vel_handoff(m, -30.0f) == true);

    prior_vel_set = m->ctrl_state.vel_set;
    prior_i_out = m->ctrl_config.vel_pid.i_out;
    prior_output = m->ctrl_config.vel_pid.output;
    prior_dyn_iq_limit = m->ctrl_state.dyn_iq_limit;

    TEST_ASSERT(foc_cmd_bench_velocity_step(m, -90.0f) == true);
    TEST_ASSERT(fabsf(m->ctrl_state.target_vel - FOC_RPM_TO_RADS(-90.0f)) < 0.001f);
    TEST_ASSERT(m->ctrl_state.vel_set == prior_vel_set);
    TEST_ASSERT(m->ctrl_config.vel_pid.i_out == prior_i_out);
    TEST_ASSERT(m->ctrl_config.vel_pid.output == prior_output);
    TEST_ASSERT(m->ctrl_state.dyn_iq_limit == prior_dyn_iq_limit);

    prior_target = m->ctrl_state.target_vel;
    TEST_ASSERT(foc_cmd_bench_velocity_step(m, 90.1f) == false);
    TEST_ASSERT(foc_cmd_bench_velocity_step(m, NAN) == false);
    TEST_ASSERT(m->ctrl_state.target_vel == prior_target);

    m->axis_state = AXIS_STATE_IDLE;
    TEST_ASSERT(foc_cmd_bench_velocity_step(m, 30.0f) == false);
    TEST_ASSERT(m->ctrl_state.target_vel == prior_target);
}

int main(void)
{
    test_handoff_rejected_when_idle();
    test_handoff_rejected_without_if();
    test_handoff_rejected_in_wrong_ctrl_mode();
    test_handoff_bumpless_transfer();
    test_bench_velocity_step_requires_handoff_and_preserves_loop_state();
    puts("[test_if_to_vel_handoff_contract] PASS");
    return 0;
}
