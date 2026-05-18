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
#include "foc_param.h"
#include "motor_fsm.h"
#include "tm_shell_output.h"

#if APP_SHELL_HAS_FACTORY_CMDS

/* Private Helpers ------------------------------------------------------------------------------ */

static const char *_app_shell_cal_stage_name(void)
{
    if (_motor_ins.axis_state != AXIS_STATE_CALIB) {
        return "idle";
    }

    if (_motor_ins.calib_state.enc_state != ENC_IDLE) {
        return "enc";
    }
    if (_motor_ins.calib_state.js_state != JS_IDLE) {
        return "js";
    }
    if (_motor_ins.calib_state.flux_state != FX_IDLE) {
        return "flux";
    }
    if (_motor_ins.calib_state.ls_state != LS_IDLE) {
        return "ls";
    }
    if (_motor_ins.calib_state.rs_state != RS_IDLE) {
        return "rs";
    }

    return "idle";
}

static void _app_shell_cal_status(void)
{
    char offset[16];

    TM_SHELL_SUMMARY("cal",
                     "cmd=status result=ok axis=%s stage=%s enc_valid=%u save_req=0x%02x",
                     app_shell_axis_state_name(_motor_ins.axis_state),
                     _app_shell_cal_stage_name(),
                     _motor_ins.enc_pri.state.calib_valid ? 1U : 0U,
                     (unsigned)_motor_ins.calib_state.param_save_req);
    TM_SHELL_ROW("step",
                 "rs=%u ls=%u flux=%u js=%u enc=%u",
                 (unsigned)_motor_ins.calib_state.rs_state,
                 (unsigned)_motor_ins.calib_state.ls_state,
                 (unsigned)_motor_ins.calib_state.flux_state,
                 (unsigned)_motor_ins.calib_state.js_state,
                 (unsigned)_motor_ins.calib_state.enc_state);
    TM_SHELL_ROW("enc",
                 "offset=%s pole_pairs=%u",
                 app_shell_format_float_or_dash(offset, sizeof(offset), _motor_ins.enc_pri.config->mech_offset, 3U),
                 (unsigned)_motor_ins.enc_pri.config->pole_pairs);
}

static int _app_shell_cal_start(enum foc_calib_state calib_type, const char *name)
{
    if (_motor_ins.axis_state != AXIS_STATE_IDLE) {
        TM_SHELL_SUMMARY("cal",
                         "cmd=start result=error axis=%s stage=%s",
                         app_shell_axis_state_name(_motor_ins.axis_state),
                         name);
        return SHELL_OK;
    }

    motor_fsm_dispatch_calib(&_motor_ins, calib_type);

    if (_motor_ins.axis_state != AXIS_STATE_CALIB) {
        TM_SHELL_SUMMARY("cal",
                         "cmd=start result=error axis=%s stage=%s",
                         app_shell_axis_state_name(_motor_ins.axis_state),
                         name);
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("cal", "cmd=start result=ok stage=%s", name);
    return SHELL_OK;
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_cal_command(int argc, char *argv[])
{
    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "status") == 0))) {
        _app_shell_cal_status();
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "abort") == 0)) {
        if (_motor_ins.axis_state != AXIS_STATE_CALIB) {
            TM_SHELL_SUMMARY("cal",
                             "cmd=abort result=error axis=%s",
                             app_shell_axis_state_name(_motor_ins.axis_state));
            return SHELL_OK;
        }

        (void)motor_fsm_dispatch(&_motor_ins, EV_CMD_CALIB_ABORT);
        TM_SHELL_SUMMARY("cal", "cmd=abort result=ok");
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "enc") == 0)) {
        return _app_shell_cal_start(CALIB_ENC, "enc");
    }

    if ((argc == 3) && (strcmp(argv[1], "motor") == 0)) {
        if (strcmp(argv[2], "all") == 0 || strcmp(argv[2], "rs") == 0) {
            return _app_shell_cal_start(CALIB_RS, argv[2]);
        }
        if (strcmp(argv[2], "ls") == 0) {
            return _app_shell_cal_start(CALIB_LS, "ls");
        }
        if (strcmp(argv[2], "flux") == 0) {
            return _app_shell_cal_start(CALIB_FLUX, "flux");
        }
        if (strcmp(argv[2], "js") == 0) {
            return _app_shell_cal_start(CALIB_JS, "js");
        }
        return SHELL_ERR_ARG;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(cal, app_shell_cal_command, 60U, "[status|enc|motor <all|rs|ls|flux|js>|abort]", "Calib");

#endif /* APP_SHELL_HAS_FACTORY_CMDS */

/* ---------------------------------------- end of file ----------------------------------------- */
