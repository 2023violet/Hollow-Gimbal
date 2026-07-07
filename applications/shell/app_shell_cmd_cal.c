/**
 * @file    app_shell_cmd_cal.c
 * @brief   FalconFoc product shell `cal` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_profile.h"
#include <string.h>
#include "tm_shell_output.h"

#if APP_SHELL_ENABLED && APP_SHELL_HAS_CAL_CMDS

/* Private Helpers ------------------------------------------------------------------------------ */

static const char *_app_shell_cal_stage_name(void)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot) || (snapshot.axis_state != AXIS_STATE_CALIB)) {
        return "idle";
    }

    if (snapshot.enc_state != ENC_IDLE) {
        return "enc";
    }
    if (snapshot.js_state != JS_IDLE) {
        return "js";
    }
    if (snapshot.flux_state != FX_IDLE) {
        return "flux";
    }
    if (snapshot.ls_state != LS_IDLE) {
        return "ls";
    }
    if (snapshot.rs_state != RS_IDLE) {
        return "rs";
    }

    return "idle";
}

static void _app_shell_cal_status(void)
{
    struct motor_runtime_snapshot snapshot;
    char offset[16];

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("cal", "cmd=status result=error reason=snapshot_unavailable");
        return;
    }

    TM_SHELL_SUMMARY("cal",
                     "cmd=status result=ok axis=%s stage=%s enc_valid=%u save_req=0x%02x",
                     app_shell_axis_state_name(snapshot.axis_state),
                     _app_shell_cal_stage_name(),
                     snapshot.enc_valid ? 1U : 0U,
                     (unsigned)snapshot.save_req);
    TM_SHELL_ROW("step",
                 "rs=%u ls=%u flux=%u js=%u enc=%u",
                 (unsigned)snapshot.rs_state,
                 (unsigned)snapshot.ls_state,
                 (unsigned)snapshot.flux_state,
                 (unsigned)snapshot.js_state,
                 (unsigned)snapshot.enc_state);
    TM_SHELL_ROW("enc",
                 "offset=%s pole_pairs=%u",
                 app_shell_format_float_or_dash(offset, sizeof(offset), snapshot.mech_offset, 3U),
                 (unsigned)snapshot.pole_pairs);
}

static int _app_shell_cal_start(enum foc_calib_state calib_type, const char *cmd_name, const char *stage_name)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("cal",
                         "cmd=%s action=start result=error reason=snapshot_unavailable stage=%s",
                         cmd_name,
                         stage_name);
        return SHELL_OK;
    }

    if (snapshot.axis_state != AXIS_STATE_IDLE) {
        TM_SHELL_SUMMARY("cal",
                         "cmd=%s action=start result=error axis=%s stage=%s",
                         cmd_name,
                         app_shell_axis_state_name(snapshot.axis_state),
                         stage_name);
        return SHELL_OK;
    }

    if (!motor_runtime_request_calibration(calib_type)) {
        TM_SHELL_SUMMARY("cal",
                         "cmd=%s action=start result=error reason=snapshot_unavailable stage=%s",
                         cmd_name,
                         stage_name);
        return SHELL_OK;
    }

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("cal",
                         "cmd=%s action=start result=error reason=snapshot_unavailable stage=%s",
                         cmd_name,
                         stage_name);
        return SHELL_OK;
    }

    if (snapshot.axis_state != AXIS_STATE_CALIB) {
        TM_SHELL_SUMMARY("cal",
                         "cmd=%s action=start result=error axis=%s stage=%s",
                         cmd_name,
                         app_shell_axis_state_name(snapshot.axis_state),
                         stage_name);
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("cal", "cmd=%s action=start result=ok stage=%s", cmd_name, stage_name);
    return SHELL_OK;
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_cal_command(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    int                           rc;

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("cal", "cmd=status result=error reason=snapshot_unavailable");
        return SHELL_OK;
    }

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "status") == 0))) {
        _app_shell_cal_status();
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "abort") == 0)) {
        if (snapshot.axis_state != AXIS_STATE_CALIB) {
            TM_SHELL_SUMMARY("cal",
                             "cmd=abort result=error axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }

        rc = motor_runtime_request_calibration_abort();
        if (rc < 0) {
            TM_SHELL_SUMMARY("cal", "cmd=abort result=error axis=%s rc=%d", app_shell_axis_state_name(snapshot.axis_state), rc);
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("cal", "cmd=abort result=ok");
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "enc") == 0)) {
        return _app_shell_cal_start(CALIB_ENC, "enc", "enc");
    }

    if ((argc == 3) && (strcmp(argv[1], "motor") == 0)) {
        if (strcmp(argv[2], "all") == 0 || strcmp(argv[2], "rs") == 0) {
            return _app_shell_cal_start(CALIB_RS, "motor", argv[2]);
        }
        if (strcmp(argv[2], "ls") == 0) {
            return _app_shell_cal_start(CALIB_LS, "motor", "ls");
        }
        if (strcmp(argv[2], "flux") == 0) {
            return _app_shell_cal_start(CALIB_FLUX, "motor", "flux");
        }
        if (strcmp(argv[2], "js") == 0) {
            return _app_shell_cal_start(CALIB_JS, "motor", "js");
        }
        return SHELL_ERR_ARG;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(cal, app_shell_cal_command, 60U, "[status|enc|motor <all|rs|ls|flux|js>|abort]", "Calib");

#endif /* APP_SHELL_HAS_CAL_CMDS */

/* ---------------------------------------- end of file ----------------------------------------- */
