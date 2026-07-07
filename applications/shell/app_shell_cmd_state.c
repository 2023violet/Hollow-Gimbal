/**
 * @file    app_shell_cmd_state.c
 * @brief   FalconFoc product shell `state` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_output_helpers.h"
#include <string.h>
#include "motor_fsm.h"
#include "motor_runtime.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

#if APP_SHELL_ENABLED

static void _app_shell_state_show(void)
{
    struct motor_runtime_snapshot snapshot;
    char vbus[16];
    char iq[16];
    char id[16];

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("state", "cmd=show result=error reason=snapshot_unavailable");
        return;
    }

    TM_SHELL_SUMMARY("state",
                     "cmd=show result=ok axis=%s ctrl=%s input=%s fault=0x%08x",
                     app_shell_axis_state_name(snapshot.axis_state),
                     app_shell_ctrl_mode_name(snapshot.ctrl_mode),
                     app_shell_input_mode_name(snapshot.input_mode),
                     (unsigned)snapshot.fault_code);
    TM_SHELL_ROW("motor",
                 "sensor=%s enc_valid=%u",
                 app_shell_sensor_name(snapshot.sensor_type),
                 snapshot.enc_valid ? 1U : 0U);
    TM_SHELL_ROW("meas",
                 "vbus=%s iq=%s id=%s",
                 app_shell_format_float_or_dash(vbus, sizeof(vbus), snapshot.v_bus, 2U),
                 app_shell_format_float_or_dash(iq, sizeof(iq), snapshot.i_q, 3U),
                 app_shell_format_float_or_dash(id, sizeof(id), snapshot.i_d, 3U));
    app_shell_emit_motion_row("out", snapshot.out_vel_rpm, snapshot.out_pos_turns);
    app_shell_emit_temp_row("temp", snapshot.motor_temp, snapshot.driver_temp);
}

static int _app_shell_state_dispatch(const char *cmd_name, enum motor_fsm_event event)
{
    enum motor_runtime_axis_action action;
    struct motor_runtime_snapshot  snapshot;
    bool                           ok;

    switch (event) {
    case EV_CMD_START:
        action = TM_MOTOR_RUNTIME_AXIS_START;
        break;
    case EV_CMD_STOP:
        action = TM_MOTOR_RUNTIME_AXIS_STOP;
        break;
    case EV_CMD_DISABLE:
        action = TM_MOTOR_RUNTIME_AXIS_DISABLE;
        break;
    default:
        return SHELL_ERR_ARG;
    }

    ok = motor_runtime_request_axis_action(action);
    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("state", "cmd=%s result=error reason=snapshot_unavailable", cmd_name);
        return SHELL_OK;
    }

    if (!ok) {
        TM_SHELL_SUMMARY("state", "cmd=%s result=error axis=%s rc=-1", cmd_name, app_shell_axis_state_name(snapshot.axis_state));
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("state", "cmd=%s result=ok axis=%s", cmd_name, app_shell_axis_state_name(snapshot.axis_state));
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
        TM_SHELL_SUMMARY("state", "cmd=mode result=error reason=redirect target=cfg");
        TM_SHELL_ROW("hint", "next='cfg ctrl <mode>'");
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(state, app_shell_state_command, 20U, "[show|start|stop|disable]", "Runtime");

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
