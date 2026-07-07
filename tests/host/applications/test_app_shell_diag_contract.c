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
#include "foc_common.h"
#include "foc_motion_runtime.h"
#include "motor_calibration_runtime.h"
#include "motor_runtime.h"
#include "tm_shell.h"

#define TEST_ASSERT(expr)                                                        \
    do {                                                                         \
        if (!(expr)) {                                                           \
            fprintf(                                                             \
                stderr,                                                          \
                "[test_app_shell_diag_contract] assertion failed: %s (%s:%d)\n", \
                #expr,                                                           \
                __FILE__,                                                        \
                __LINE__                                                         \
            );                                                                   \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

struct motor_dev _motor_ins;

static char g_output[1024];
static size_t g_output_len;
static int g_passthrough_calls;
static int g_vel_ramp_calls;
static int g_torque_ramp_calls;
static int g_pos_traj_calls;
static int g_request_param_calls;
static int g_request_pid_calls;
static int g_request_voltage_calls;
static int g_request_set_calls;
static int g_request_ramp_calls;
static int g_last_pid_loop;
static int g_last_pid_field;
static float g_last_pid_value;
static int g_last_param_field;
static float g_last_param_value;
static float g_last_voltage_vd;
static float g_last_voltage_vq;
static float g_last_voltage_we;
static int g_last_set_mode;
static float g_last_set_target;
static float g_last_set_limit;
static int g_last_ramp_kind;
static float g_last_ramp_value0;
static float g_last_ramp_value1;
static float g_last_ramp_value2;
static float g_last_ramp_value3;

static void test_env_reset(void)
{
    memset(&_motor_ins, 0, sizeof(_motor_ins));
    motor_runtime_attach(&_motor_ins);
    memset(g_output, 0, sizeof(g_output));
    g_output_len = 0U;
    g_passthrough_calls = 0;
    g_vel_ramp_calls = 0;
    g_torque_ramp_calls = 0;
    g_pos_traj_calls = 0;
    g_request_param_calls = 0;
    g_request_pid_calls = 0;
    g_request_voltage_calls = 0;
    g_request_set_calls = 0;
    g_request_ramp_calls = 0;
    g_last_pid_loop = -1;
    g_last_pid_field = -1;
    g_last_pid_value = 0.0f;
    g_last_param_field = -1;
    g_last_param_value = 0.0f;
    g_last_voltage_vd = 0.0f;
    g_last_voltage_vq = 0.0f;
    g_last_voltage_we = 0.0f;
    g_last_set_mode = -1;
    g_last_set_target = 0.0f;
    g_last_set_limit = 0.0f;
    g_last_ramp_kind = -1;
    g_last_ramp_value0 = 0.0f;
    g_last_ramp_value1 = 0.0f;
    g_last_ramp_value2 = 0.0f;
    g_last_ramp_value3 = 0.0f;

    _motor_ins.ctrl_state.iq_max = 10.0f;
    _motor_ins.ctrl_state.vel_max = 100.0f;
    _motor_ins.ctrl_state.dyn_iq_limit = 10.0f;
    _motor_ins.ctrl_state.dyn_vel_limit = 100.0f;
    _motor_ins.hw_config.gear_ratio = 1.0f;
    _motor_ins.calc_params.inv_kt = 1.0f;
    _motor_ins.ctrl_mode = CTRL_MODE_VEL;
    _motor_ins.input_mode = INPUT_MODE_PASSTHROUGH;
}

void shell_printf(const char *fmt, ...)
{
    va_list args;
    int written;

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

const char *
app_shell_format_float_or_dash(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    (void)frac_digits;
    TEST_ASSERT(buffer != NULL);
    TEST_ASSERT(buffer_size > 0U);
    (void)snprintf(buffer, buffer_size, "%.2f", (double)value);
    return buffer;
}

bool motor_runtime_request_set_passthrough(enum foc_ctrl_mode mode, float target, float limit)
{
    g_request_set_calls++;
    g_passthrough_calls++;
    g_last_set_mode = (int)mode;
    g_last_set_target = target;
    g_last_set_limit = limit;
    _motor_ins.ctrl_mode = mode;
    _motor_ins.input_mode = INPUT_MODE_PASSTHROUGH;

    if (mode == CTRL_MODE_VEL) {
        _motor_ins.ctrl_state.target_vel = target;
        _motor_ins.ctrl_state.dyn_iq_limit = limit;
    } else if (mode == CTRL_MODE_CURR) {
        _motor_ins.ctrl_state.target_iq = target;
    } else if (mode == CTRL_MODE_POS) {
        _motor_ins.ctrl_state.target_pos = target;
        _motor_ins.ctrl_state.dyn_vel_limit = limit;
    }

    return true;
}

bool motor_runtime_request_vel_ramp(float new_vel, float max_acc)
{
    g_request_ramp_calls++;
    g_vel_ramp_calls++;
    g_last_ramp_kind = 0;
    g_last_ramp_value0 = new_vel;
    g_last_ramp_value1 = max_acc;
    _motor_ins.ctrl_mode = CTRL_MODE_VEL;
    _motor_ins.input_mode = INPUT_MODE_VEL_RAMP;
    _motor_ins.ctrl_state.target_vel = new_vel;
    _motor_ins.ctrl_config.ramp_accel = max_acc;
    return true;
}

bool motor_runtime_request_torque_ramp(float new_torque, float max_acc)
{
    g_request_ramp_calls++;
    g_torque_ramp_calls++;
    g_last_ramp_kind = 1;
    g_last_ramp_value0 = new_torque;
    g_last_ramp_value1 = max_acc;
    _motor_ins.ctrl_mode = CTRL_MODE_CURR;
    _motor_ins.input_mode = INPUT_MODE_TORQUE_RAMP;
    _motor_ins.ctrl_state.target_tor = new_torque;
    _motor_ins.ctrl_config.ramp_accel = max_acc;
    return true;
}

bool motor_runtime_request_pos_traj(
    float new_pos_rad,
    float max_vel,
    float max_acc,
    float max_decel
)
{
    g_request_ramp_calls++;
    g_pos_traj_calls++;
    g_last_ramp_kind = 2;
    g_last_ramp_value0 = new_pos_rad;
    g_last_ramp_value1 = max_vel;
    g_last_ramp_value2 = max_acc;
    g_last_ramp_value3 = max_decel;
    _motor_ins.ctrl_mode = CTRL_MODE_POS;
    _motor_ins.input_mode = INPUT_MODE_TRAP_TRAJ;
    _motor_ins.ctrl_state.target_pos = new_pos_rad;
    _motor_ins.ctrl_state.dyn_vel_limit = max_vel;
    return true;
}

bool motor_runtime_diag_set_param(const char *name, float value)
{
    TEST_ASSERT(name != NULL);
    g_request_param_calls++;

    if (strcmp(name, "rs") == 0) {
        g_last_param_field = 0;
        _motor_ins.hw_config.rs = value;
    } else if (strcmp(name, "ls") == 0) {
        g_last_param_field = 1;
        _motor_ins.hw_config.ls = value;
    } else if (strcmp(name, "flux") == 0) {
        g_last_param_field = 2;
        _motor_ins.hw_config.flux = value;
    } else if (strcmp(name, "poles") == 0) {
        g_last_param_field = 3;
        _motor_ins.hw_config.poles = (float)(int)value;
    } else if (strcmp(name, "js") == 0) {
        g_last_param_field = 4;
        _motor_ins.hw_config.js = value;
    } else if (strcmp(name, "gear") == 0) {
        g_last_param_field = 5;
        _motor_ins.hw_config.gear_ratio = value;
    } else if (strcmp(name, "vov") == 0) {
        g_last_param_field = 6;
        _motor_ins.prot_config.v_bus_ov_thr = value;
    } else if (strcmp(name, "vuv") == 0) {
        g_last_param_field = 7;
        _motor_ins.prot_config.v_bus_uv_thr = value;
    } else if (strcmp(name, "imax") == 0) {
        g_last_param_field = 8;
        _motor_ins.prot_config.i_peak = value;
    } else if (strcmp(name, "velmax") == 0) {
        g_last_param_field = 9;
        _motor_ins.prot_config.vel_over_thr = FOC_RPM_TO_RADS(value);
    } else if (strcmp(name, "currbw") == 0) {
        if (value <= 0.0f) {
            return false;
        }
        g_last_param_field = 10;
        _motor_ins.ctrl_config.curr_bandwidth = value;
    } else if (strcmp(name, "velbw") == 0) {
        if (value <= 0.0f) {
            return false;
        }
        g_last_param_field = 11;
        _motor_ins.ctrl_config.vel_bandwidth = value;
    } else if (strcmp(name, "posbw") == 0) {
        if (value <= 0.0f) {
            return false;
        }
        g_last_param_field = 12;
        _motor_ins.ctrl_config.pos_bandwidth = value;
    } else {
        return false;
    }

    g_last_param_value = value;
    return true;
}

bool motor_runtime_diag_set_pid(const char *loop_name, const char *field, float value)
{
    struct pid_dev *pid = NULL;

    TEST_ASSERT(loop_name != NULL);
    TEST_ASSERT(field != NULL);
    g_request_pid_calls++;

    if (strcmp(loop_name, "id") == 0) {
        pid = &_motor_ins.ctrl_config.id_pid;
        g_last_pid_loop = 0;
    } else if (strcmp(loop_name, "iq") == 0) {
        pid = &_motor_ins.ctrl_config.iq_pid;
        g_last_pid_loop = 1;
    } else if (strcmp(loop_name, "vel") == 0) {
        pid = &_motor_ins.ctrl_config.vel_pid;
        g_last_pid_loop = 2;
    } else if (strcmp(loop_name, "pos") == 0) {
        pid = &_motor_ins.ctrl_config.pos_pid;
        g_last_pid_loop = 3;
    } else {
        return false;
    }

    if (strcmp(field, "kp") == 0) {
        pid->config.kp = value;
        g_last_pid_field = 0;
    } else if (strcmp(field, "ki") == 0) {
        pid->config.ki = value;
        g_last_pid_field = 1;
    } else if (strcmp(field, "kd") == 0) {
        pid->config.kd = value;
        g_last_pid_field = 2;
    } else if (strcmp(field, "lim") == 0) {
        pid->config.max_output = value;
        pid->config.integral_limit = value;
        g_last_pid_field = 3;
    } else {
        return false;
    }

    g_last_pid_value = value;
    return true;
}

bool motor_runtime_diag_reset_pid(const char *loop_name)
{
    struct pid_dev *pid = NULL;

    TEST_ASSERT(loop_name != NULL);
    g_request_pid_calls++;

    if (strcmp(loop_name, "id") == 0) {
        pid = &_motor_ins.ctrl_config.id_pid;
        g_last_pid_loop = 0;
    } else if (strcmp(loop_name, "iq") == 0) {
        pid = &_motor_ins.ctrl_config.iq_pid;
        g_last_pid_loop = 1;
    } else if (strcmp(loop_name, "vel") == 0) {
        pid = &_motor_ins.ctrl_config.vel_pid;
        g_last_pid_loop = 2;
    } else if (strcmp(loop_name, "pos") == 0) {
        pid = &_motor_ins.ctrl_config.pos_pid;
        g_last_pid_loop = 3;
    } else {
        return false;
    }

    g_last_pid_field = 4;
    pid->config.kp = 0.0f;
    pid->config.ki = 0.0f;
    pid->config.kd = 0.0f;
    return true;
}

bool motor_runtime_diag_set_voltage(float vd, float vq, float we_ramp)
{
    g_request_voltage_calls++;
    g_last_voltage_vd = vd;
    g_last_voltage_vq = vq;
    g_last_voltage_we = we_ramp;
    _motor_ins.ctrl_mode = CTRL_MODE_VOLT;
    return true;
}

bool motor_runtime_diag_read_params(struct motor_runtime_diag_param_view *view)
{
    TEST_ASSERT(view != NULL);
    view->poles = _motor_ins.hw_config.poles;
    view->rs = _motor_ins.hw_config.rs;
    view->ls = _motor_ins.hw_config.ls;
    view->flux = _motor_ins.hw_config.flux;
    view->js = _motor_ins.hw_config.js;
    view->gear_ratio = _motor_ins.hw_config.gear_ratio;
    view->v_bus_ov_thr = _motor_ins.prot_config.v_bus_ov_thr;
    view->v_bus_uv_thr = _motor_ins.prot_config.v_bus_uv_thr;
    view->i_peak = _motor_ins.prot_config.i_peak;
    view->vel_over_rpm = FOC_RADS_TO_RPM(_motor_ins.prot_config.vel_over_thr);
    view->curr_bandwidth = _motor_ins.ctrl_config.curr_bandwidth;
    view->vel_bandwidth = _motor_ins.ctrl_config.vel_bandwidth;
    view->pos_bandwidth = _motor_ins.ctrl_config.pos_bandwidth;
    return true;
}

bool motor_runtime_diag_read_pid(const char *loop_name, struct motor_runtime_pid_view *view)
{
    const struct pid_dev *pid = NULL;

    TEST_ASSERT(loop_name != NULL);
    TEST_ASSERT(view != NULL);

    if (strcmp(loop_name, "id") == 0) {
        pid = &_motor_ins.ctrl_config.id_pid;
    } else if (strcmp(loop_name, "iq") == 0) {
        pid = &_motor_ins.ctrl_config.iq_pid;
    } else if (strcmp(loop_name, "vel") == 0) {
        pid = &_motor_ins.ctrl_config.vel_pid;
    } else if (strcmp(loop_name, "pos") == 0) {
        pid = &_motor_ins.ctrl_config.pos_pid;
    } else {
        return false;
    }

    view->kp = pid->config.kp;
    view->ki = pid->config.ki;
    view->kd = pid->config.kd;
    view->lim = pid->config.max_output;
    return true;
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
    snapshot->axis_state = (uint8_t)_motor_ins.axis_state;
    return true;
}

static void test_diag_set_vel_is_allowed_while_running(void)
{
    char *argv[] = {"diag", "set", "vel", "50"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    _motor_ins.ctrl_mode = CTRL_MODE_CURR;
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
    TEST_ASSERT(g_request_pid_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=running") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=vel") != NULL);
    TEST_ASSERT(strstr(g_output, "field=kp") != NULL);
}

static void test_diag_param_write_is_rejected_while_calib(void)
{
    char *argv[] = {"diag", "param", "rs", "0.2300"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_CALIB;
    _motor_ins.hw_config.rs = 0.5f;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.hw_config.rs == 0.5f);
    TEST_ASSERT(g_request_param_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=param action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "axis=calib") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=idle_only") != NULL);
    TEST_ASSERT(strstr(g_output, "name=rs") != NULL);
}

static void test_diag_pid_show_remains_available_while_running(void)
{
    char *argv[] = {"diag", "pid", "all", "show"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=show result=ok loop=all") != NULL);
}

static void test_diag_param_write_uses_runtime_request_in_idle(void)
{
    char *argv[] = {"diag", "param", "currbw", "2500"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_config.curr_bandwidth = 1000.0f;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_param_calls == 1);
    TEST_ASSERT(g_last_param_field == 10);
    TEST_ASSERT(_motor_ins.ctrl_config.curr_bandwidth == 2500.0f);
    TEST_ASSERT(strstr(g_output, "cmd=param action=set result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "name=currbw") != NULL);
    TEST_ASSERT(strstr(g_output, "value=2500.00 unit=raw") != NULL);
}

static void test_diag_pid_write_uses_runtime_request_in_idle(void)
{
    char *argv[] = {"diag", "pid", "vel", "kp", "0.1250"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(5, argv) == SHELL_OK);
    TEST_ASSERT(g_request_pid_calls == 1);
    TEST_ASSERT(g_last_pid_loop == 2);
    TEST_ASSERT(g_last_pid_field == 0);
    TEST_ASSERT(_motor_ins.ctrl_config.vel_pid.config.kp == 0.1250f);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=set result=ok") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=vel") != NULL);
    TEST_ASSERT(strstr(g_output, "field=kp") != NULL);
    TEST_ASSERT(strstr(g_output, "value=0.13 unit=raw") != NULL);
}

static void test_diag_pid_reset_uses_runtime_request_in_idle(void)
{
    char *argv[] = {"diag", "pid", "vel", "reset"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_config.vel_pid.config.kp = 2.0f;
    _motor_ins.ctrl_config.vel_pid.config.ki = 3.0f;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_pid_calls == 1);
    TEST_ASSERT(g_last_pid_loop == 2);
    TEST_ASSERT(g_last_pid_field == 4);
    TEST_ASSERT(_motor_ins.ctrl_config.vel_pid.config.kp == 0.0f);
    TEST_ASSERT(_motor_ins.ctrl_config.vel_pid.config.ki == 0.0f);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=reset result=ok loop=vel") != NULL);
}

static void test_diag_pid_show_bad_loop_reports_reason(void)
{
    char *argv[] = {"diag", "pid", "flux", "show"};

    test_env_reset();

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=show result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_loop") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=flux") != NULL);
}

static void test_diag_pid_reset_bad_loop_reports_reason(void)
{
    char *argv[] = {"diag", "pid", "flux", "reset"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_pid_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=reset result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_loop") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=flux") != NULL);
}

static void test_diag_pid_set_bad_field_reports_reason(void)
{
    char *argv[] = {"diag", "pid", "vel", "foo", "0.5000"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(5, argv) == SHELL_OK);
    TEST_ASSERT(g_request_pid_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=pid action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_field") != NULL);
    TEST_ASSERT(strstr(g_output, "loop=vel") != NULL);
    TEST_ASSERT(strstr(g_output, "field=foo") != NULL);
}

static void test_diag_param_bad_name_reports_reason(void)
{
    char *argv[] = {"diag", "param", "foo", "1.0000"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_param_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=param action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_name") != NULL);
    TEST_ASSERT(strstr(g_output, "name=foo") != NULL);
}

static void test_diag_param_bad_value_reports_reason(void)
{
    char *argv[] = {"diag", "param", "currbw", "0.0000"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_param_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=param action=set result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "reason=bad_value") != NULL);
    TEST_ASSERT(strstr(g_output, "name=currbw") != NULL);
    TEST_ASSERT(strstr(g_output, "value=0.00 unit=raw") != NULL);
}

static void test_diag_set_vd_uses_runtime_voltage_request(void)
{
    char *argv[] = {"diag", "set", "vd", "1.250"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_RUNNING;
    _motor_ins.ctrl_mode = CTRL_MODE_CURR;

    TEST_ASSERT(app_shell_diag_command(4, argv) == SHELL_OK);
    TEST_ASSERT(g_request_voltage_calls == 1);
    TEST_ASSERT(g_last_voltage_vd == 1.250f);
    TEST_ASSERT(g_last_voltage_vq == 0.0f);
    TEST_ASSERT(g_last_voltage_we == 0.0f);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(strstr(g_output, "cmd=set result=ok vd=1.25 unit=V") != NULL);
}

int main(void)
{
    test_diag_set_vel_is_allowed_while_running();
    test_diag_pid_write_is_rejected_while_running();
    test_diag_param_write_is_rejected_while_calib();
    test_diag_pid_show_remains_available_while_running();
    test_diag_param_write_uses_runtime_request_in_idle();
    test_diag_pid_write_uses_runtime_request_in_idle();
    test_diag_pid_reset_uses_runtime_request_in_idle();
    test_diag_pid_show_bad_loop_reports_reason();
    test_diag_pid_reset_bad_loop_reports_reason();
    test_diag_pid_set_bad_field_reports_reason();
    test_diag_param_bad_name_reports_reason();
    test_diag_param_bad_value_reports_reason();
    test_diag_set_vd_uses_runtime_voltage_request();

    puts("[test_app_shell_diag_contract] passed");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
