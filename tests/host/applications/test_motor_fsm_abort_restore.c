/**
 * @file    test_motor_fsm_abort_restore.c
 * @brief   Host regression test for encoder-calibration abort recovery
 * @version v1.0.0
 * @date    2026.05.12
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foc_param.h"
#include "motor_fsm.h"

#define TEST_ASSERT(expr)                                                                \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            fprintf(stderr,                                                              \
                    "[test_motor_fsm_abort_restore] assertion failed: %s (%s:%d)\n",    \
                    #expr,                                                               \
                    __FILE__,                                                            \
                    __LINE__);                                                           \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

static uint32_t g_pwm_enable_count = 0U;
static uint32_t g_pwm_disable_count = 0U;
static uint32_t g_prepare_count = 0U;
static uint32_t g_restore_count = 0U;
static uint32_t g_restore_after_param_load_count = 0U;

static struct encoder_config g_restore_cfg;
static bool                  g_restore_valid = false;
static int32_t               g_restore_table[ENCODER_LUT_SIZE];

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
    g_prepare_count++;
    if (motor == NULL) {
        return;
    }

    motor->ctrl_state.target_vel = 0.0f;
    motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
    motor->input_mode            = INPUT_MODE_INACTIVE;
}

void motor_restore_after_param_load(struct motor_dev *motor)
{
    g_restore_after_param_load_count++;
    if (motor == NULL) {
        return;
    }

    motor->ctrl_state.target_vel = 0.0f;
    motor->ctrl_state.target_pos = motor->meas_data.out_pos_rad;
}

void foc_calib_enc_init(struct motor_dev *motor)
{
    if ((motor == NULL) || (motor->enc_pri.config == NULL)) {
        return;
    }

    memset(motor->enc_pri.state.enc_table, 0, sizeof(motor->enc_pri.state.enc_table));
    motor->enc_pri.state.calib_valid   = false;
    motor->enc_pri.config->mech_offset = 0.0f;
}

int foc_param_load_enc(struct motor_dev *motor)
{
    if ((motor == NULL) || (motor->enc_pri.config == NULL)) {
        return PARAM_ERR_FLASH;
    }

    g_restore_count++;
    motor->enc_pri.state.calib_valid   = g_restore_valid;
    motor->enc_pri.config->mech_offset = g_restore_cfg.mech_offset;
    motor->enc_pri.config->dir         = g_restore_cfg.dir;
    memcpy(motor->enc_pri.state.enc_table, g_restore_table, sizeof(g_restore_table));
    return PARAM_OK;
}

int motor_calibration_runtime_restore_encoder_after_abort(struct motor_dev *motor)
{
    return foc_param_load_enc(motor);
}

const char *foc_param_status_str(int st)
{
    switch (st) {
    case PARAM_OK:
        return "OK";
    case PARAM_EMPTY:
        return "EMPTY";
    case PARAM_ERR_FLASH:
        return "FLASH";
    default:
        return "ERR";
    }
}

static void test_encoder_calibration_abort_restores_last_committed_payload(void)
{
    struct motor_dev      motor = {0};
    struct encoder_config cfg   = {0};

    g_pwm_enable_count = 0U;
    g_pwm_disable_count = 0U;
    g_prepare_count = 0U;
    g_restore_count = 0U;
    g_restore_after_param_load_count = 0U;

    cfg.dir         = DIR_CCW;
    cfg.mech_offset = 1.234f;

    g_restore_cfg   = cfg;
    g_restore_valid = true;
    for (uint32_t i = 0U; i < ENCODER_LUT_SIZE; ++i) {
        g_restore_table[i] = (int32_t)(1000 + i);
    }

    motor.enc_pri.config            = &cfg;
    motor.enc_pri.state.calib_valid = true;
    memcpy(motor.enc_pri.state.enc_table, g_restore_table, sizeof(g_restore_table));

    motor_fsm_init(&motor);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(g_pwm_enable_count == 1U);

    motor_fsm_dispatch_calib(&motor, CALIB_ENC);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_CALIB);
    TEST_ASSERT(motor.calib_state.state == CALIB_ENC);
    TEST_ASSERT(g_restore_count == 0U);
    TEST_ASSERT(motor.enc_pri.state.calib_valid == false);
    TEST_ASSERT(motor.enc_pri.config->mech_offset == 0.0f);

    TEST_ASSERT(motor_fsm_dispatch(&motor, EV_CMD_CALIB_ABORT) == 0);
    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(g_restore_count == 1U);
    TEST_ASSERT(motor.calib_state.enc_state == ENC_IDLE);
    TEST_ASSERT(motor.enc_pri.state.calib_valid == true);
    TEST_ASSERT(motor.enc_pri.config->mech_offset == g_restore_cfg.mech_offset);
    TEST_ASSERT(motor.enc_pri.config->dir == g_restore_cfg.dir);
    TEST_ASSERT(motor.enc_pri.state.enc_table[0] == g_restore_table[0]);
    TEST_ASSERT(motor.enc_pri.state.enc_table[ENCODER_LUT_SIZE - 1] ==
                g_restore_table[ENCODER_LUT_SIZE - 1]);
}

static void test_initial_idle_enter_preserves_restored_input_mode(void)
{
    struct motor_dev      motor = {0};
    struct encoder_config cfg   = {0};

    g_pwm_enable_count = 0U;
    g_pwm_disable_count = 0U;
    g_prepare_count = 0U;
    g_restore_count = 0U;
    g_restore_after_param_load_count = 0U;

    motor.enc_pri.config         = &cfg;
    motor.axis_state             = AXIS_STATE_IDLE;
    motor.ctrl_mode              = CTRL_MODE_VEL;
    motor.input_mode             = INPUT_MODE_TRAP_TRAJ;
    motor.meas_data.out_pos_rad  = 4.0f;
    motor.ctrl_state.target_vel  = 22.0f;
    motor.ctrl_state.target_pos  = 9.0f;

    motor_fsm_init(&motor);

    TEST_ASSERT(motor.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(motor.ctrl_mode == CTRL_MODE_VEL);
    TEST_ASSERT(motor.input_mode == INPUT_MODE_TRAP_TRAJ);
    TEST_ASSERT(g_restore_after_param_load_count == 1U);
    TEST_ASSERT(g_prepare_count == 0U);
    TEST_ASSERT(g_pwm_enable_count == 1U);
    TEST_ASSERT(motor.ctrl_state.target_vel == 0.0f);
    TEST_ASSERT(motor.ctrl_state.target_pos == 4.0f);
}

int main(void)
{
    test_encoder_calibration_abort_restores_last_committed_payload();
    test_initial_idle_enter_preserves_restored_input_mode();

    puts("[test_motor_fsm_abort_restore] passed");
    return 0;
}
