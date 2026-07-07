/**
 * @file    test_app_shell_state_contract.c
 * @brief   Host contract tests for FalconFoc `state` shell behavior
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

#define TEST_ASSERT(expr)                                                                \
    do {                                                                                 \
        if (!(expr)) {                                                                   \
            fprintf(stderr,                                                              \
                    "[test_app_shell_state_contract] assertion failed: %s (%s:%d)\n",   \
                    #expr,                                                               \
                    __FILE__,                                                            \
                    __LINE__);                                                           \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

struct motor_dev _motor_ins;
static struct motor_dev g_runtime_motor;

static char   g_output[1024];
static size_t g_output_len;
static int    g_request_axis_calls;
static int    g_last_axis_action;

static void test_env_reset(void)
{
    memset(&_motor_ins, 0, sizeof(_motor_ins));
    memset(&g_runtime_motor, 0, sizeof(g_runtime_motor));
    memset(g_output, 0, sizeof(g_output));
    g_output_len      = 0U;
    g_request_axis_calls = 0;
    g_last_axis_action = -1;
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

const char *app_shell_sensor_name(uint8_t sensor)
{
    switch (sensor) {
    case SENSOR_TYPE_NONE:
        return "none";
    case SENSOR_TYPE_MAGNETIC:
        return "magnetic";
    case SENSOR_TYPE_HALL:
        return "hall";
    case SENSOR_TYPE_INC_ENC:
        return "inc-enc";
    default:
        return "unknown";
    }
}

const char *app_shell_fault_bit_name(uint32_t bit)
{
    (void)bit;
    return "unknown";
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

const char *app_shell_format_float_or_dash(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    (void)frac_digits;
    TEST_ASSERT(buffer != NULL);
    TEST_ASSERT(buffer_size > 0U);
    (void)snprintf(buffer, buffer_size, "%.2f", (double)value);
    return buffer;
}

void app_shell_emit_motion_row(const char *label, float vel_rpm, float pos_turns)
{
    char vel[16];
    char pos[16];

    TEST_ASSERT(label != NULL);
    shell_printf("  %-7s: vel_rpm=%s pos_turn=%s\r\n",
                 label,
                 app_shell_format_float_or_dash(vel, sizeof(vel), vel_rpm, 2U),
                 app_shell_format_float_or_dash(pos, sizeof(pos), pos_turns, 3U));
}

void app_shell_emit_temp_row(const char *label, float motor_temp, float driver_temp)
{
    char motor[16];
    char driver[16];

    TEST_ASSERT(label != NULL);
    shell_printf("  %-7s: motor=%s driver=%s\r\n",
                 label,
                 app_shell_format_float_or_dash(motor, sizeof(motor), motor_temp, 1U),
                 app_shell_format_float_or_dash(driver, sizeof(driver), driver_temp, 1U));
}

bool motor_runtime_request_axis_action(enum motor_runtime_axis_action action)
{
    g_request_axis_calls++;
    g_last_axis_action = (int)action;
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

    if (g_runtime_motor.axis_state == 0U &&
        g_runtime_motor.ctrl_mode == 0U &&
        g_runtime_motor.input_mode == 0U &&
        g_runtime_motor.sensor_type == 0U &&
        g_runtime_motor.fault_code == 0U &&
        g_runtime_motor.meas_data.v_bus == 0.0f &&
        g_runtime_motor.enc_pri.config == NULL) {
        memset(snapshot, 0, sizeof(*snapshot));
        return false;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->axis_state    = (uint8_t)g_runtime_motor.axis_state;
    snapshot->ctrl_mode     = (uint8_t)g_runtime_motor.ctrl_mode;
    snapshot->input_mode    = (uint8_t)g_runtime_motor.input_mode;
    snapshot->sensor_type   = (uint8_t)g_runtime_motor.sensor_type;
    snapshot->fault_code    = g_runtime_motor.fault_code;
    snapshot->enc_valid     = g_runtime_motor.enc_pri.state.calib_valid;
    snapshot->v_bus         = g_runtime_motor.meas_data.v_bus;
    snapshot->i_q           = g_runtime_motor.foc_calc.i_q;
    snapshot->i_d           = g_runtime_motor.foc_calc.i_d;
    snapshot->out_vel_rpm   = g_runtime_motor.meas_data.out_vel_rpm;
    snapshot->out_pos_turns = g_runtime_motor.meas_data.out_pos_turns;
    snapshot->motor_temp    = g_runtime_motor.meas_data.motor_temp;
    snapshot->driver_temp   = g_runtime_motor.meas_data.driver_temp;
    return true;
}

static void test_state_start_dispatches_fsm_event_only(void)
{
    char *argv[] = {"state", "start"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_mode  = CTRL_MODE_VEL;
    _motor_ins.sensor_type = SENSOR_TYPE_MAGNETIC;
    g_runtime_motor.axis_state = AXIS_STATE_IDLE;
    g_runtime_motor.ctrl_mode  = CTRL_MODE_VEL;
    g_runtime_motor.sensor_type = SENSOR_TYPE_MAGNETIC;

    TEST_ASSERT(app_shell_state_command(2, argv) == SHELL_OK);
    TEST_ASSERT(g_request_axis_calls == 1);
    TEST_ASSERT(g_last_axis_action == TM_MOTOR_RUNTIME_AXIS_START);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_VEL);
    TEST_ASSERT(strstr(g_output, "cmd=start result=ok") != NULL);
}

static void test_state_show_reads_runtime_snapshot(void)
{
    static struct encoder_config enc_cfg = {
        .mech_offset = 0.0f,
        .pole_pairs  = 7U,
    };
    char *argv[] = {"state", "show"};

    test_env_reset();

    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_mode  = CTRL_MODE_VOLT;
    _motor_ins.input_mode = INPUT_MODE_INACTIVE;
    _motor_ins.sensor_type = SENSOR_TYPE_MAGNETIC;
    _motor_ins.fault_code = 0U;

    g_runtime_motor.axis_state                   = AXIS_STATE_RUNNING;
    g_runtime_motor.ctrl_mode                    = CTRL_MODE_POS;
    g_runtime_motor.input_mode                   = INPUT_MODE_TRAP_TRAJ;
    g_runtime_motor.sensor_type                  = SENSOR_TYPE_MAGNETIC;
    g_runtime_motor.fault_code                   = 0x000055AAU;
    g_runtime_motor.enc_pri.config               = &enc_cfg;
    g_runtime_motor.enc_pri.state.calib_valid    = true;
    g_runtime_motor.meas_data.v_bus              = 24.5f;
    g_runtime_motor.foc_calc.i_q                 = 1.25f;
    g_runtime_motor.foc_calc.i_d                 = -0.50f;
    g_runtime_motor.meas_data.out_vel_rpm        = 345.0f;
    g_runtime_motor.meas_data.out_pos_turns      = 6.75f;
    g_runtime_motor.meas_data.motor_temp         = 36.5f;
    g_runtime_motor.meas_data.driver_temp        = 33.0f;
    TEST_ASSERT(app_shell_state_command(2, argv) == SHELL_OK);
    TEST_ASSERT(g_request_axis_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=show result=ok axis=running ctrl=pos input=traj fault=0x000055aa") != NULL);
    TEST_ASSERT(strstr(g_output, "sensor=magnetic enc_valid=1") != NULL);
    TEST_ASSERT(strstr(g_output, "vbus=24.50 iq=1.25 id=-0.50") != NULL);
    TEST_ASSERT(strstr(g_output, "vel_rpm=345.00 pos_turn=6.75") != NULL);
    TEST_ASSERT(strstr(g_output, "motor=36.50 driver=33.00") != NULL);
}

static void test_state_show_reports_snapshot_unavailable(void)
{
    char *argv[] = {"state", "show"};

    test_env_reset();
    TEST_ASSERT(app_shell_state_command(2, argv) == SHELL_OK);
    TEST_ASSERT(g_request_axis_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=show result=error reason=snapshot_unavailable") != NULL);
}

static void test_state_mode_is_not_a_live_write_path_anymore(void)
{
    char *argv[] = {"state", "mode", "vel"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_mode  = CTRL_MODE_VOLT;
    _motor_ins.sensor_type = SENSOR_TYPE_MAGNETIC;
    g_runtime_motor.axis_state = AXIS_STATE_IDLE;
    g_runtime_motor.ctrl_mode  = CTRL_MODE_VOLT;
    g_runtime_motor.sensor_type = SENSOR_TYPE_MAGNETIC;

    TEST_ASSERT(app_shell_state_command(3, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(g_request_axis_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=mode result=error reason=redirect target=cfg") != NULL);
    TEST_ASSERT(strstr(g_output, "next='cfg ctrl <mode>'") != NULL);
}

int main(void)
{
    test_state_start_dispatches_fsm_event_only();
    test_state_show_reads_runtime_snapshot();
    test_state_show_reports_snapshot_unavailable();
    test_state_mode_is_not_a_live_write_path_anymore();

    puts("[test_app_shell_state_contract] passed");
    return 0;
}
