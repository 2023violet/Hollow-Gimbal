/**
 * @file    test_app_shell_cfg_cal_contract.c
 * @brief   Host contract tests for FalconFoc `cfg` and `cal` command ownership
 * @version v1.0.0
 * @date    2026.05.15
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_shell_commands.h"
#include "foc_common.h"
#include "motor_runtime.h"
#include "tm_shell.h"

#define TEST_ASSERT(expr)                                                                  \
    do {                                                                                   \
        if (!(expr)) {                                                                     \
            fprintf(stderr,                                                                \
                    "[test_app_shell_cfg_cal_contract] assertion failed: %s (%s:%d)\n",   \
                    #expr,                                                                 \
                    __FILE__,                                                              \
                    __LINE__);                                                             \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

struct motor_dev _motor_ins;

static char   g_output[1024];
static size_t g_output_len;
static int    g_request_ctrl_calls;
static int    g_request_input_calls;
static int    g_request_calib_calls;
static int    g_request_calib_abort_calls;
static int    g_last_ctrl_mode;
static int    g_last_input_mode;
static int    g_last_calib_type;
static int    g_abort_rc;
static int    g_calib_enter_axis_state;

static void test_env_reset(void)
{
    memset(&_motor_ins, 0, sizeof(_motor_ins));
    motor_runtime_attach(&_motor_ins);
    memset(g_output, 0, sizeof(g_output));
    g_output_len           = 0U;
    g_request_ctrl_calls   = 0;
    g_request_input_calls  = 0;
    g_request_calib_calls  = 0;
    g_request_calib_abort_calls = 0;
    g_last_ctrl_mode       = -1;
    g_last_input_mode      = -1;
    g_last_calib_type      = -1;
    g_abort_rc             = 0;
    g_calib_enter_axis_state = 0;
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

const char *app_shell_fault_bit_name(uint32_t bit)
{
    (void)bit;
    return "none";
}

int app_shell_parse_ctrl_mode(const char *text, enum foc_ctrl_mode *mode_out)
{
    if ((text == NULL) || (mode_out == NULL)) {
        return -1;
    }

    if (strcmp(text, "volt") == 0) {
        *mode_out = CTRL_MODE_VOLT;
        return 0;
    }
    if (strcmp(text, "curr") == 0) {
        *mode_out = CTRL_MODE_CURR;
        return 0;
    }
    if (strcmp(text, "vel") == 0) {
        *mode_out = CTRL_MODE_VEL;
        return 0;
    }
    if (strcmp(text, "pos") == 0) {
        *mode_out = CTRL_MODE_POS;
        return 0;
    }

    return -1;
}

int app_shell_parse_input_mode(const char *text, enum foc_input_mode *mode_out)
{
    if ((text == NULL) || (mode_out == NULL)) {
        return -1;
    }

    if (strcmp(text, "inactive") == 0) {
        *mode_out = INPUT_MODE_INACTIVE;
        return 0;
    }
    if (strcmp(text, "pass") == 0) {
        *mode_out = INPUT_MODE_PASSTHROUGH;
        return 0;
    }
    if (strcmp(text, "traj") == 0) {
        *mode_out = INPUT_MODE_TRAP_TRAJ;
        return 0;
    }

    return -1;
}

const char *app_shell_format_float_or_dash(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    (void)frac_digits;
    TEST_ASSERT(buffer != NULL);
    TEST_ASSERT(buffer_size > 0U);
    (void)snprintf(buffer, buffer_size, "%.2f", (double)value);
    return buffer;
}

bool motor_runtime_request_ctrl_mode(enum foc_ctrl_mode mode)
{
    g_request_ctrl_calls++;
    g_last_ctrl_mode    = (int)mode;
    _motor_ins.ctrl_mode = mode;
    return true;
}

bool motor_runtime_request_input_mode(enum foc_input_mode mode)
{
    g_request_input_calls++;
    g_last_input_mode     = (int)mode;
    _motor_ins.input_mode = mode;
    return true;
}

bool motor_runtime_request_calibration(enum foc_calib_state calib_type)
{
    g_request_calib_calls++;
    g_last_calib_type = (int)calib_type;

    if (g_calib_enter_axis_state != 0) {
        _motor_ins.axis_state = (enum foc_axis_state)g_calib_enter_axis_state;
    }

    return true;
}

int motor_runtime_request_calibration_abort(void)
{
    g_request_calib_abort_calls++;
    return g_abort_rc;
}

struct motor_dev *motor_runtime_current(void)
{
    return NULL;
}

void motor_runtime_attach(struct motor_dev *motor)
{
    (void)motor;
}

bool motor_runtime_snapshot(struct motor_runtime_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->axis_state  = (uint8_t)_motor_ins.axis_state;
    snapshot->ctrl_mode   = (uint8_t)_motor_ins.ctrl_mode;
    snapshot->input_mode  = (uint8_t)_motor_ins.input_mode;
    snapshot->sensor_type = (uint8_t)_motor_ins.sensor_type;
    snapshot->fault_code  = _motor_ins.fault_code;
    snapshot->calib_state = (uint8_t)_motor_ins.calib_state.state;
    snapshot->rs_state    = (uint8_t)_motor_ins.calib_state.rs_state;
    snapshot->ls_state    = (uint8_t)_motor_ins.calib_state.ls_state;
    snapshot->flux_state  = (uint8_t)_motor_ins.calib_state.flux_state;
    snapshot->js_state    = (uint8_t)_motor_ins.calib_state.js_state;
    snapshot->enc_state   = (uint8_t)_motor_ins.calib_state.enc_state;
    snapshot->enc_valid   = _motor_ins.enc_pri.state.calib_valid;
    snapshot->mech_offset = (_motor_ins.enc_pri.config != NULL) ? _motor_ins.enc_pri.config->mech_offset : 0.0f;
    snapshot->pole_pairs  = (_motor_ins.enc_pri.config != NULL) ? _motor_ins.enc_pri.config->pole_pairs : 0U;
    snapshot->save_req    = _motor_ins.calib_state.param_save_req;
    return true;
}

static void test_cfg_ctrl_is_rejected_while_running(void)
{
    char *argv[] = {"cfg", "ctrl", "vel"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    _motor_ins.ctrl_mode  = CTRL_MODE_CURR;

    TEST_ASSERT(app_shell_cfg_command(3, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_CURR);
    TEST_ASSERT(g_request_ctrl_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=ctrl result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=running") != NULL);
}

static void test_cfg_input_writes_formal_config_in_idle(void)
{
    char *argv[] = {"cfg", "input", "traj"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.input_mode = INPUT_MODE_INACTIVE;

    TEST_ASSERT(app_shell_cfg_command(3, argv) == SHELL_OK);
    TEST_ASSERT(g_request_input_calls == 1);
    TEST_ASSERT(g_last_input_mode == INPUT_MODE_TRAP_TRAJ);
    TEST_ASSERT(_motor_ins.input_mode == INPUT_MODE_TRAP_TRAJ);
    TEST_ASSERT(strstr(g_output, "cmd=input result=ok input=traj") != NULL);
}

static void test_cal_start_is_idle_only_event_path(void)
{
    char *argv[] = {"cal", "enc"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_cal_command(2, argv) == SHELL_OK);
    TEST_ASSERT(g_request_calib_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=enc action=start result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=running") != NULL);
}

static void test_cal_start_reports_error_when_dispatch_does_not_enter_calib(void)
{
    char *argv[] = {"cal", "enc"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    g_calib_enter_axis_state = 0;

    TEST_ASSERT(app_shell_cal_command(2, argv) == SHELL_OK);
    TEST_ASSERT(g_request_calib_calls == 1);
    TEST_ASSERT(_motor_ins.axis_state == AXIS_STATE_IDLE);
    TEST_ASSERT(strstr(g_output, "cmd=enc action=start result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "stage=enc") != NULL);
}

static void test_cal_start_reports_ok_after_entering_calib(void)
{
    char *argv[] = {"cal", "enc"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    g_calib_enter_axis_state = AXIS_STATE_CALIB;

    TEST_ASSERT(app_shell_cal_command(2, argv) == SHELL_OK);
    TEST_ASSERT(g_request_calib_calls == 1);
    TEST_ASSERT(g_last_calib_type == CALIB_ENC);
    TEST_ASSERT(_motor_ins.axis_state == AXIS_STATE_CALIB);
    TEST_ASSERT(strstr(g_output, "cmd=enc action=start result=ok stage=enc") != NULL);
}

static void test_cal_abort_dispatches_event_only(void)
{
    char *argv[] = {"cal", "abort"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_CALIB;
    g_abort_rc = 0;

    TEST_ASSERT(app_shell_cal_command(2, argv) == SHELL_OK);
    TEST_ASSERT(g_request_calib_abort_calls == 1);
    TEST_ASSERT(strstr(g_output, "cmd=abort result=ok") != NULL);
}

int main(void)
{
    test_cfg_ctrl_is_rejected_while_running();
    test_cfg_input_writes_formal_config_in_idle();
    test_cal_start_is_idle_only_event_path();
    test_cal_start_reports_error_when_dispatch_does_not_enter_calib();
    test_cal_start_reports_ok_after_entering_calib();
    test_cal_abort_dispatches_event_only();

    puts("[test_app_shell_cfg_cal_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
