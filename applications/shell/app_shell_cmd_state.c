/**
 * @file    app_shell_cmd_state.c
 * @brief   FalconFoc product shell `state` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include <string.h>
#include "motor_fsm.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

static void _app_shell_state_show(void)
{
    char vbus[16];
    char iq[16];
    char id[16];
    char vel[16];
    char pos[16];
    char motor_temp[16];
    char driver_temp[16];

    TM_SHELL_SUMMARY("state",
                     "cmd=status result=ok axis=%s ctrl=%s input=%s fault=0x%08x",
                     app_shell_axis_state_name(_motor_ins.axis_state),
                     app_shell_ctrl_mode_name(_motor_ins.ctrl_mode),
                     app_shell_input_mode_name(_motor_ins.input_mode),
                     (unsigned)_motor_ins.fault_code);
    TM_SHELL_ROW("motor",
                 "sensor=%s enc_valid=%u",
                 app_shell_sensor_name(_motor_ins.sensor_type),
                 _motor_ins.enc_pri.state.calib_valid ? 1U : 0U);
    TM_SHELL_ROW("meas",
                 "vbus=%s iq=%s id=%s",
                 app_shell_format_float_or_dash(vbus, sizeof(vbus), _motor_ins.meas_data.v_bus, 2U),
                 app_shell_format_float_or_dash(iq, sizeof(iq), _motor_ins.foc_calc.i_q, 3U),
                 app_shell_format_float_or_dash(id, sizeof(id), _motor_ins.foc_calc.i_d, 3U));
    TM_SHELL_ROW("out",
                 "vel_rpm=%s pos_turn=%s",
                 app_shell_format_float_or_dash(vel, sizeof(vel), _motor_ins.meas_data.out_vel_rpm, 2U),
                 app_shell_format_float_or_dash(pos, sizeof(pos), _motor_ins.meas_data.out_pos_turns, 3U));
    TM_SHELL_ROW("temp",
                 "motor=%s driver=%s",
                 app_shell_format_float_or_dash(motor_temp, sizeof(motor_temp), _motor_ins.meas_data.motor_temp, 1U),
                 app_shell_format_float_or_dash(driver_temp, sizeof(driver_temp), _motor_ins.meas_data.driver_temp, 1U));
}

static int _app_shell_state_dispatch(const char *cmd_name, enum motor_fsm_event event)
{
    int rc;

    rc = motor_fsm_dispatch(&_motor_ins, event);
    if (rc < 0) {
        TM_SHELL_SUMMARY("state",
                         "cmd=%s result=error axis=%s rc=%d",
                         cmd_name,
                         app_shell_axis_state_name(_motor_ins.axis_state),
                         rc);
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("state",
                     "cmd=%s result=ok axis=%s",
                     cmd_name,
                     app_shell_axis_state_name(_motor_ins.axis_state));
    return SHELL_OK;
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_state_command(int argc, char *argv[])
{
    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "show") == 0))) {
        _app_shell_state_show();
        return SHELL_OK;
    }

    if (argc != 2 && argc != 3) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[1], "start") == 0) {
        return _app_shell_state_dispatch("start", EV_CMD_START);
    }
    if (strcmp(argv[1], "stop") == 0) {
        return _app_shell_state_dispatch("stop", EV_CMD_STOP);
    }
    if (strcmp(argv[1], "disable") == 0) {
        return _app_shell_state_dispatch("disable", EV_CMD_DISABLE);
    }
    if ((argc == 3) && (strcmp(argv[1], "mode") == 0)) {
        TM_SHELL_SUMMARY("state", "cmd=mode result=error owner=cfg");
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(state, app_shell_state_command, 20U, "[show|start|stop|disable]", "Runtime");

/* ---------------------------------------- end of file ----------------------------------------- */
