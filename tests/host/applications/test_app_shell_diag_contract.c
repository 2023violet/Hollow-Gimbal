/**
 * @file    test_app_shell_diag_contract.c
 * @brief   Host contract tests for FalconFoc `diag` shell ownership boundaries
 * @version v1.0.0
 * @date    2026.05.15
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell_commands.h"
#include "foc_calib.h"
#include "foc_common.h"
#include "foc_ctrl.h"
#include "tm_shell.h"

#define TEST_ASSERT(expr)                                                               \
    do {                                                                                \
        if (!(expr)) {                                                                  \
            fprintf(stderr,                                                             \
                    "[test_app_shell_diag_contract] assertion failed: %s (%s:%d)\n",   \
                    #expr,                                                              \
                    __FILE__,                                                           \
                    __LINE__);                                                          \
            exit(1);                                                                    \
        }                                                                               \
    } while (0)

struct motor_dev _motor_ins;

static char   g_output[1024];
static size_t g_output_len;
static int    g_passthrough_calls;
static int    g_vel_ramp_calls;
static int    g_torque_ramp_calls;
static int    g_pos_traj_calls;
static int    g_curr_gain_calls;
static int    g_vel_gain_calls;
static int    g_pos_gain_calls;

static void test_env_reset(void)
{
    memset(&_motor_ins, 0, sizeof(_motor_ins));
    memset(g_output, 0, sizeof(g_output));
    g_output_len        = 0U;
    g_passthrough_calls = 0;
    g_vel_ramp_calls    = 0;
    g_torque_ramp_calls = 0;
    g_pos_traj_calls    = 0;
    g_curr_gain_calls   = 0;
    g_vel_gain_calls    = 0;
    g_pos_gain_calls    = 0;

    _motor_ins.ctrl_state.iq_max    = 10.0f;
    _motor_ins.ctrl_state.vel_max   = 100.0f;
    _motor_ins.ctrl_state.dyn_iq_limit  = 10.0f;
    _motor_ins.ctrl_state.dyn_vel_limit = 100.0f;
    _motor_ins.hw_config.gear_ratio = 1.0f;
    _motor_ins.calc_params.inv_kt   = 1.0f;
    _motor_ins.ctrl_mode            = CTRL_MODE_VEL;
    _motor_ins.input_mode           = INPUT_MODE_PASSTHROUGH;
}

void shell_printf(const char *fmt, ...)
{
    va_list args;
    int     written;

    TEST_ASSERT(fmt != NULL);
    TEST_ASSERT(g_output_len < sizeof(g_output));

    va_start(args, fmt);
    written = vsnprintf(&g_output[g_output_len], sizeof(g_output) - g_output_len, fmt, args);
    va_end(args);

    TEST_ASSERT(written >= 0);
    TEST_ASSERT((size_t)written < (sizeof(g_output) - g_output_len));
    g_output_len += (size_t)written;
}

const char *app_shell_axis_state_name(uint8_t state)
{
    switch (state) {
    case AXIS_STATE_IDLE:
        return "idle";
    case AXIS_STATE_CALIB:
        return "calib";
    case AXIS_STATE_RUNNING:
        return "running";
    case AXIS_STATE_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

const char *app_shell_ctrl_mode_name(uint8_t mode)
{
    switch (mode) {
    case CTRL_MODE_VOLT:
        return "volt";
    case CTRL_MODE_CURR:
        return "curr";
    case CTRL_MODE_VEL:
        return "vel";
    case CTRL_MODE_POS:
        return "pos";
    default:
        return "unknown";
    }
}

const char *app_shell_input_mode_name(uint8_t mode)
{
    switch (mode) {
    case INPUT_MODE_INACTIVE:
        return "inactive";
    case INPUT_MODE_PASSTHROUGH:
        return "pass";
    case INPUT_MODE_VEL_RAMP:
        return "vel-ramp";
    case INPUT_MODE_TORQUE_RAMP:
        return "torque-ramp";
    case INPUT_MODE_POS_FILTER:
        return "pos-filter";
    case INPUT_MODE_VEL_FILTER:
        return "vel-filter";
    case INPUT_MODE_TRAP_TRAJ:
        return "traj";
    case INPUT_MODE_TUNING:
        return "tuning";
    default:
        return "unknown";
    }
}

int app_shell_parse_float_arg(const char *text, float *out)
{
    char *end = NULL;

    TEST_ASSERT(text != NULL);
    TEST_ASSERT(out != NULL);

    *out = strtof(text, &end);
    if (end == text) {
        return -1;
    }

    return (*end == '\0') ? 0 : -1;
}

const char *app_shell_format_float_or_dash(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    (void)frac_digits;
    TEST_ASSERT(buffer != NULL);
    TEST_ASSERT(buffer_size > 0U);
    (void)snprintf(buffer, buffer_size, "%.2f", (double)value);
    return buffer;
}

void foc_cmd_passthrough(struct motor_dev *motor, enum foc_ctrl_mode mode, float target, float limit)
{
    TEST_ASSERT(motor != NULL);
    g_passthrough_calls++;
    motor->ctrl_mode  = mode;
    motor->input_mode = INPUT_MODE_PASSTHROUGH;

    if (mode == CTRL_MODE_VEL) {
        motor->ctrl_state.target_vel = target;
        motor->ctrl_state.dyn_iq_limit = limit;
    } else if (mode == CTRL_MODE_CURR) {
        motor->ctrl_state.target_iq = target;
    } else if (mode == CTRL_MODE_POS) {
        motor->ctrl_state.target_pos = target;
        motor->ctrl_state.dyn_vel_limit = limit;
    }
}

void foc_cmd_vel_ramp(struct motor_dev *motor, float new_vel, float max_acc)
{
    TEST_ASSERT(motor != NULL);
    g_vel_ramp_calls++;
    motor->ctrl_mode             = CTRL_MODE_VEL;
    motor->input_mode            = INPUT_MODE_VEL_RAMP;
    motor->ctrl_state.target_vel = new_vel;
    motor->ctrl_config.ramp_accel = max_acc;
}

void foc_cmd_torque_ramp(struct motor_dev *motor, float new_torque, float max_acc)
{
    TEST_ASSERT(motor != NULL);
    g_torque_ramp_calls++;
    motor->ctrl_mode             = CTRL_MODE_CURR;
    motor->input_mode            = INPUT_MODE_TORQUE_RAMP;
    motor->ctrl_state.target_tor = new_torque;
    motor->ctrl_config.ramp_accel = max_acc;
}

void foc_cmd_pos_traj(struct motor_dev *motor,
                      float             new_pos_rad,
                      float             max_vel,
                      float             max_acc,
                      float             max_decel)
{
    TEST_ASSERT(motor != NULL);
    g_pos_traj_calls++;
    motor->ctrl_mode              = CTRL_MODE_POS;
    motor->input_mode             = INPUT_MODE_TRAP_TRAJ;
    motor->ctrl_state.target_pos  = new_pos_rad;
    motor->ctrl_state.dyn_vel_limit = max_vel;
    (void)max_acc;
    (void)max_decel;
}

void foc_curr_gain_calc(struct motor_dev *motor)
{
    TEST_ASSERT(motor != NULL);
    g_curr_gain_calls++;
}

void foc_vel_gain_calc(struct motor_dev *motor)
{
    TEST_ASSERT(motor != NULL);
    g_vel_gain_calls++;
}

void foc_pos_gain_calc(struct motor_dev *motor)
{
    TEST_ASSERT(motor != NULL);
    g_pos_gain_calls++;
}

static void test_diag_set_vel_is_allowed_while_running(void)
{
    char *argv[] = {"diag", "set", "vel", "50"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    _motor_ins.ctrl_mode  = CTRL_MODE_CURR;
    _motor_ins.input_mode = INPUT_MODE_INACTIVE;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_passthrough_calls == 1);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_VEL);
    TEST_ASSERT(_motor_ins.input_mode == INPUT_MODE_PASSTHROUGH);
    TEST_ASSERT(strstr(g_output, "cmd=set result=ok") != NULL);
}

static void test_diag_pid_write_is_rejected_while_running(void)
{
    char *argv[] = {"diag", "pid", "vel", "kp", "0.1000"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    _motor_ins.ctrl_config.vel_pid.config.kp = 1.0f;

    TEST_ASSERT(app_shell_diag_command(5, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.ctrl_config.vel_pid.config.kp == 1.0f);
    TEST_ASSERT(strstr(g_output, "cmd=pid result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=running") != NULL);
}

static void test_diag_param_write_is_rejected_while_calib(void)
{
    char *argv[] = {"diag", "param", "rs", "0.2300"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_CALIB;
    _motor_ins.hw_config.rs = 0.5f;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.hw_config.rs == 0.5f);
    TEST_ASSERT(g_curr_gain_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=param result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=calib") != NULL);
}

static void test_diag_pid_show_remains_available_while_running(void)
{
    char *argv[] = {"diag", "pid", "all", "show"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=pid result=ok loop=all") != NULL);
}

int main(void)
{
    test_diag_set_vel_is_allowed_while_running();
    test_diag_pid_write_is_rejected_while_running();
    test_diag_param_write_is_rejected_while_calib();
    test_diag_pid_show_remains_available_while_running();

    puts("[test_app_shell_diag_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
