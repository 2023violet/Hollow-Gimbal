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
#include "motor_fsm.h"
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

static char   g_output[1024];
static size_t g_output_len;
static int    g_dispatch_calls;
static int    g_last_event;

static void test_env_reset(void)
{
    memset(&_motor_ins, 0, sizeof(_motor_ins));
    memset(g_output, 0, sizeof(g_output));
    g_output_len    = 0U;
    g_dispatch_calls = 0;
    g_last_event     = -1;
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
    (void)mode;
    return "inactive";
}

const char *app_shell_sensor_name(uint8_t sensor)
{
    (void)sensor;
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

const char *app_shell_format_float_or_dash(char *buffer, size_t buffer_size, float value, uint8_t frac_digits)
{
    (void)frac_digits;
    TEST_ASSERT(buffer != NULL);
    TEST_ASSERT(buffer_size > 0U);
    (void)snprintf(buffer, buffer_size, "%.2f", (double)value);
    return buffer;
}

int motor_fsm_dispatch(struct motor_dev *motor, enum motor_fsm_event event)
{
    (void)motor;
    g_dispatch_calls++;
    g_last_event = (int)event;
    return 0;
}

static void test_state_start_dispatches_fsm_event_only(void)
{
    char *argv[] = {"state", "start"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_mode  = CTRL_MODE_VEL;
    _motor_ins.sensor_type = SENSOR_TYPE_MAGNETIC;

    TEST_ASSERT(app_shell_state_command(2, argv) == SHELL_OK);
    TEST_ASSERT(g_dispatch_calls == 1);
    TEST_ASSERT(g_last_event == EV_CMD_START);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_VEL);
    TEST_ASSERT(strstr(g_output, "cmd=start result=ok") != NULL);
}

static void test_state_mode_is_not_a_live_write_path_anymore(void)
{
    char *argv[] = {"state", "mode", "vel"};

    test_env_reset();
    _motor_ins.axis_state = AXIS_STATE_IDLE;
    _motor_ins.ctrl_mode  = CTRL_MODE_VOLT;
    _motor_ins.sensor_type = SENSOR_TYPE_MAGNETIC;

    TEST_ASSERT(app_shell_state_command(3, argv) == SHELL_OK);
    TEST_ASSERT(_motor_ins.ctrl_mode == CTRL_MODE_VOLT);
    TEST_ASSERT(g_dispatch_calls == 0);
    TEST_ASSERT(strstr(g_output, "cmd=mode result=error") != NULL);
    TEST_ASSERT(strstr(g_output, "owner=cfg") != NULL);
}

int main(void)
{
    test_state_start_dispatches_fsm_event_only();
    test_state_mode_is_not_a_live_write_path_anymore();

    puts("[test_app_shell_state_contract] passed");
    return 0;
}
