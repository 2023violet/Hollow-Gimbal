/**
 * @file    app_shell_cmd_cfg.c
 * @brief   FalconFoc product shell `cfg` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_profile.h"
#include <string.h>
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

#if APP_SHELL_ENABLED

static void _app_shell_cfg_show(void)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("cfg", "cmd=show result=error reason=snapshot_unavailable");
        return;
    }

    TM_SHELL_SUMMARY("cfg",
                     "cmd=show result=ok shell=%s ctrl=%s input=%s",
                     tm_shell_onoff_name(TM_SHELL_ENABLE != 0U),
                     app_shell_ctrl_mode_name(snapshot.ctrl_mode),
                     app_shell_input_mode_name(snapshot.input_mode));
    TM_SHELL_ROW("hint", "next='store save sys'");
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_cfg_command(int argc, char *argv[])
{
    enum foc_ctrl_mode  ctrl_mode;
    enum foc_input_mode input_mode;
    struct motor_runtime_snapshot snapshot;

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "show") == 0))) {
        _app_shell_cfg_show();
        return SHELL_OK;
    }

    if (argc != 3) {
        return SHELL_ERR_ARGC;
    }

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("cfg", "cmd=%s result=error reason=snapshot_unavailable", argv[1]);
        return SHELL_OK;
    }

    if (snapshot.axis_state != AXIS_STATE_IDLE) {
        TM_SHELL_SUMMARY("cfg",
                         "cmd=%s result=error axis=%s",
                         argv[1],
                         app_shell_axis_state_name(snapshot.axis_state));
        return SHELL_OK;
    }

    if (strcmp(argv[1], "ctrl") == 0) {
        if (app_shell_parse_ctrl_mode(argv[2], &ctrl_mode) != 0) {
            return SHELL_ERR_ARG;
        }

        if (!motor_runtime_request_ctrl_mode(ctrl_mode)) {
            TM_SHELL_SUMMARY("cfg", "cmd=ctrl result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("cfg", "cmd=ctrl result=ok ctrl=%s", app_shell_ctrl_mode_name((uint8_t)ctrl_mode));
        return SHELL_OK;
    }

    if (strcmp(argv[1], "input") == 0) {
        if (app_shell_parse_input_mode(argv[2], &input_mode) != 0) {
            return SHELL_ERR_ARG;
        }

        if (!motor_runtime_request_input_mode(input_mode)) {
            TM_SHELL_SUMMARY("cfg", "cmd=input result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("cfg", "cmd=input result=ok input=%s", app_shell_input_mode_name((uint8_t)input_mode));
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(cfg, app_shell_cfg_command, 70U, "[show|ctrl <mode>|input <mode>]", "Config");

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
