/**
 * @file    app_shell_cmd_diag.c
 * @brief   FalconFoc product shell `diag` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_profile.h"
#include <errno.h>
#include <math.h>
#include <string.h>
#include "foc_calib.h"
#include "foc_ctrl.h"
#include "foc_param.h"
#include "motor_fsm.h"
#include "motor_task.h"
#include "pid.h"
#include "tm_shell_output.h"

#if APP_SHELL_HAS_DIAG_CMDS

/* Private Helpers ------------------------------------------------------------------------------ */

static int _app_shell_diag_emit_scalar_summary(const char *cmd,
                                               const char *field,
                                               float       value,
                                               uint8_t     frac_digits,
                                               const char *unit)
{
    char buffer[16];

    TM_SHELL_SUMMARY("diag",
                     "cmd=%s result=ok %s=%s unit=%s",
                     cmd,
                     field,
                     app_shell_format_float_or_dash(buffer, sizeof(buffer), value, frac_digits),
                     unit);
    return SHELL_OK;
}

static bool _app_shell_diag_reject_if_not_idle(const char *cmd_name)
{
    if (_motor_ins.axis_state == AXIS_STATE_IDLE) {
        return false;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=%s result=error axis=%s policy=idle-only",
                     cmd_name,
                     app_shell_axis_state_name(_motor_ins.axis_state));
    return true;
}

static int _app_shell_diag_param_set(const char *name, float value)
{
    if (strcmp(name, "rs") == 0) {
        _motor_ins.hw_config.rs = value;
        return 0;
    }
    if (strcmp(name, "ls") == 0) {
        _motor_ins.hw_config.ls = value;
        return 0;
    }
    if (strcmp(name, "flux") == 0) {
        _motor_ins.hw_config.flux = value;
        return 0;
    }
    if (strcmp(name, "poles") == 0) {
        _motor_ins.hw_config.poles = (float)(int)value;
        return 0;
    }
    if (strcmp(name, "js") == 0) {
        _motor_ins.hw_config.js = value;
        return 0;
    }
    if (strcmp(name, "gear") == 0) {
        _motor_ins.hw_config.gear_ratio = value;
        return 0;
    }
    if (strcmp(name, "vov") == 0) {
        _motor_ins.prot_config.v_bus_ov_thr = value;
        return 0;
    }
    if (strcmp(name, "vuv") == 0) {
        _motor_ins.prot_config.v_bus_uv_thr = value;
        return 0;
    }
    if (strcmp(name, "imax") == 0) {
        _motor_ins.prot_config.i_peak = value;
        return 0;
    }
    if (strcmp(name, "velmax") == 0) {
        _motor_ins.prot_config.vel_over_thr = FOC_RPM_TO_RADS(value);
        return 0;
    }
    if (strcmp(name, "currbw") == 0) {
        if (value <= 0.0f) {
            return -EINVAL;
        }
        _motor_ins.ctrl_config.curr_bandwidth = value;
        foc_curr_gain_calc(&_motor_ins);
        return 0;
    }
    if (strcmp(name, "velbw") == 0) {
        if (value <= 0.0f) {
            return -EINVAL;
        }
        _motor_ins.ctrl_config.vel_bandwidth = value;
        foc_vel_gain_calc(&_motor_ins);
        return 0;
    }
    if (strcmp(name, "posbw") == 0) {
        if (value <= 0.0f) {
            return -EINVAL;
        }
        _motor_ins.ctrl_config.pos_bandwidth = value;
        foc_pos_gain_calc(&_motor_ins);
        return 0;
    }

    return -ENOENT;
}

static void _app_shell_diag_param_show(void)
{
    char rs[16];
    char ls[16];
    char flux[16];
    char js[16];
    char gear[16];
    char vov[16];
    char vuv[16];
    char imax[16];
    char velmax[16];
    char currbw[16];
    char velbw[16];
    char posbw[16];

    TM_SHELL_SUMMARY("diag", "cmd=param result=ok view=show");
    TM_SHELL_ROW("motor",
                 "poles=%u rs=%s ls=%s flux=%s js=%s",
                 (unsigned)_motor_ins.hw_config.poles,
                 app_shell_format_float_or_dash(rs, sizeof(rs), _motor_ins.hw_config.rs, 4U),
                 app_shell_format_float_or_dash(ls, sizeof(ls), _motor_ins.hw_config.ls, 4U),
                 app_shell_format_float_or_dash(flux, sizeof(flux), _motor_ins.hw_config.flux, 4U),
                 app_shell_format_float_or_dash(js, sizeof(js), _motor_ins.hw_config.js, 4U));
    TM_SHELL_ROW("gear",
                 "ratio=%s",
                 app_shell_format_float_or_dash(gear, sizeof(gear), _motor_ins.hw_config.gear_ratio, 3U));
    TM_SHELL_ROW("prot",
                 "vov=%s vuv=%s imax=%s velmax_rpm=%s",
                 app_shell_format_float_or_dash(vov, sizeof(vov), _motor_ins.prot_config.v_bus_ov_thr, 2U),
                 app_shell_format_float_or_dash(vuv, sizeof(vuv), _motor_ins.prot_config.v_bus_uv_thr, 2U),
                 app_shell_format_float_or_dash(imax, sizeof(imax), _motor_ins.prot_config.i_peak, 2U),
                 app_shell_format_float_or_dash(velmax, sizeof(velmax), FOC_RADS_TO_RPM(_motor_ins.prot_config.vel_over_thr), 2U));
    TM_SHELL_ROW("band",
                 "curr=%s vel=%s pos=%s",
                 app_shell_format_float_or_dash(currbw, sizeof(currbw), _motor_ins.ctrl_config.curr_bandwidth, 1U),
                 app_shell_format_float_or_dash(velbw, sizeof(velbw), _motor_ins.ctrl_config.vel_bandwidth, 1U),
                 app_shell_format_float_or_dash(posbw, sizeof(posbw), _motor_ins.ctrl_config.pos_bandwidth, 1U));
}

static int _app_shell_diag_pid_find(const char *loop_name, struct pid_dev **pid_out)
{
    if ((loop_name == NULL) || (pid_out == NULL)) {
        return -EINVAL;
    }

    if (strcmp(loop_name, "id") == 0) {
        *pid_out = &_motor_ins.ctrl_config.id_pid;
        return 0;
    }
    if (strcmp(loop_name, "iq") == 0) {
        *pid_out = &_motor_ins.ctrl_config.iq_pid;
        return 0;
    }
    if (strcmp(loop_name, "vel") == 0) {
        *pid_out = &_motor_ins.ctrl_config.vel_pid;
        return 0;
    }
    if (strcmp(loop_name, "pos") == 0) {
        *pid_out = &_motor_ins.ctrl_config.pos_pid;
        return 0;
    }

    return -ENOENT;
}

static void _app_shell_diag_pid_row(const char *name, const struct pid_dev *pid)
{
    char kp[16];
    char ki[16];
    char kd[16];
    char lim[16];

    TM_SHELL_ROW("pid",
                 "loop=%s kp=%s ki=%s kd=%s lim=%s",
                 name,
                 app_shell_format_float_or_dash(kp, sizeof(kp), pid->config.kp, 4U),
                 app_shell_format_float_or_dash(ki, sizeof(ki), pid->config.ki, 4U),
                 app_shell_format_float_or_dash(kd, sizeof(kd), pid->config.kd, 4U),
                 app_shell_format_float_or_dash(lim, sizeof(lim), pid->config.max_output, 4U));
}

static int _app_shell_diag_handle_set(int argc, char *argv[])
{
    float value;
    float limit;

    if (argc < 4) {
        return SHELL_ERR_ARGC;
    }
    if (app_shell_parse_float_arg(argv[3], &value) != 0) {
        return SHELL_ERR_ARG;
    }

    limit = 0.0f;
    if ((argc >= 5) && (app_shell_parse_float_arg(argv[4], &limit) != 0)) {
        return SHELL_ERR_ARG;
    }

    if (strcmp(argv[2], "vel") == 0) {
        foc_cmd_passthrough(&_motor_ins, CTRL_MODE_VEL, FOC_RPM_TO_RADS(value), limit);
        return _app_shell_diag_emit_scalar_summary("set", "vel_rpm", value, 2U, "rpm");
    }
    if (strcmp(argv[2], "pos") == 0) {
        foc_cmd_passthrough(&_motor_ins,
                            CTRL_MODE_POS,
                            value * FOC_2PI,
                            (limit > 0.0f) ? FOC_RPM_TO_RADS(limit) : 0.0f);
        return _app_shell_diag_emit_scalar_summary("set", "pos_turn", value, 3U, "turn");
    }
    if (strcmp(argv[2], "iq") == 0) {
        foc_cmd_passthrough(&_motor_ins, CTRL_MODE_CURR, value, 0.0f);
        return _app_shell_diag_emit_scalar_summary("set", "iq", value, 3U, "A");
    }
    if (strcmp(argv[2], "vd") == 0) {
        _motor_ins.ctrl_mode         = CTRL_MODE_VOLT;
        _motor_ins.ctrl_state.vd_set = value;
        return _app_shell_diag_emit_scalar_summary("set", "vd", value, 3U, "V");
    }
    if (strcmp(argv[2], "vq") == 0) {
        _motor_ins.ctrl_mode         = CTRL_MODE_VOLT;
        _motor_ins.ctrl_state.vq_set = value;
        return _app_shell_diag_emit_scalar_summary("set", "vq", value, 3U, "V");
    }

    return SHELL_ERR_ARG;
}

static int _app_shell_diag_handle_ramp(int argc, char *argv[])
{
    float value;
    float arg1;
    float arg2;
    float arg3;

    if (argc < 4) {
        return SHELL_ERR_ARGC;
    }
    if (app_shell_parse_float_arg(argv[3], &value) != 0) {
        return SHELL_ERR_ARG;
    }

    arg1 = 0.0f;
    arg2 = 0.0f;
    arg3 = 0.0f;
    if ((argc >= 5) && (app_shell_parse_float_arg(argv[4], &arg1) != 0)) {
        return SHELL_ERR_ARG;
    }
    if ((argc >= 6) && (app_shell_parse_float_arg(argv[5], &arg2) != 0)) {
        return SHELL_ERR_ARG;
    }
    if ((argc >= 7) && (app_shell_parse_float_arg(argv[6], &arg3) != 0)) {
        return SHELL_ERR_ARG;
    }

    if (strcmp(argv[2], "vel") == 0) {
        foc_cmd_vel_ramp(&_motor_ins, FOC_RPM_TO_RADS(value), (arg1 > 0.0f) ? FOC_RPM_TO_RADS(arg1) : 0.0f);
        return _app_shell_diag_emit_scalar_summary("ramp", "vel_rpm", value, 2U, "rpm");
    }
    if (strcmp(argv[2], "iq") == 0) {
        foc_cmd_torque_ramp(&_motor_ins, value, arg1);
        return _app_shell_diag_emit_scalar_summary("ramp", "iq", value, 3U, "A");
    }
    if (strcmp(argv[2], "pos") == 0) {
        foc_cmd_pos_traj(&_motor_ins,
                         value * FOC_2PI,
                         (arg1 > 0.0f) ? FOC_RPM_TO_RADS(arg1) : 0.0f,
                         (arg2 > 0.0f) ? FOC_RPM_TO_RADS(arg2) : 0.0f,
                         (arg3 > 0.0f) ? FOC_RPM_TO_RADS(arg3) : 0.0f);
        return _app_shell_diag_emit_scalar_summary("ramp", "pos_turn", value, 3U, "turn");
    }

    return SHELL_ERR_ARG;
}

static int _app_shell_diag_handle_pid(int argc, char *argv[])
{
    struct pid_dev *pid;
    float           value;
    if (argc < 4) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[2], "all") == 0) {
        if ((argc != 4) || (strcmp(argv[3], "show") != 0)) {
            return SHELL_ERR_ARG;
        }

        TM_SHELL_SUMMARY("diag", "cmd=pid result=ok loop=all");
        _app_shell_diag_pid_row("id", &_motor_ins.ctrl_config.id_pid);
        _app_shell_diag_pid_row("iq", &_motor_ins.ctrl_config.iq_pid);
        _app_shell_diag_pid_row("vel", &_motor_ins.ctrl_config.vel_pid);
        _app_shell_diag_pid_row("pos", &_motor_ins.ctrl_config.pos_pid);
        return SHELL_OK;
    }

    if (_app_shell_diag_pid_find(argv[2], &pid) != 0) {
        return SHELL_ERR_ARG;
    }

    if (strcmp(argv[3], "show") == 0) {
        TM_SHELL_SUMMARY("diag", "cmd=pid result=ok loop=%s", argv[2]);
        _app_shell_diag_pid_row(argv[2], pid);
        return SHELL_OK;
    }

    if (_app_shell_diag_reject_if_not_idle("pid")) {
        return SHELL_OK;
    }

    if (strcmp(argv[3], "reset") == 0) {
        pid_reset(pid);
        TM_SHELL_SUMMARY("diag", "cmd=pid result=ok loop=%s action=reset", argv[2]);
        return SHELL_OK;
    }

    if ((argc != 5) || (app_shell_parse_float_arg(argv[4], &value) != 0)) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[3], "kp") == 0) {
        pid->config.kp = value;
    } else if (strcmp(argv[3], "ki") == 0) {
        pid->config.ki = value;
    } else if (strcmp(argv[3], "kd") == 0) {
        pid->config.kd = value;
    } else if (strcmp(argv[3], "lim") == 0) {
        pid->config.max_output     = fabsf(value);
        pid->config.integral_limit = fabsf(value);
    } else {
        return SHELL_ERR_ARG;
    }

    return _app_shell_diag_emit_scalar_summary("pid", argv[3], value, 4U, "raw");
}

static int _app_shell_diag_handle_param(int argc, char *argv[])
{
    float value;
    int   rc;

    if ((argc == 3) && (strcmp(argv[2], "show") == 0)) {
        _app_shell_diag_param_show();
        return SHELL_OK;
    }

    if (argc != 4) {
        return SHELL_ERR_ARGC;
    }
    if (app_shell_parse_float_arg(argv[3], &value) != 0) {
        return SHELL_ERR_ARG;
    }

    if (_app_shell_diag_reject_if_not_idle("param")) {
        return SHELL_OK;
    }

    rc = _app_shell_diag_param_set(argv[2], value);
    if (rc == -ENOENT) {
        return SHELL_ERR_ARG;
    }
    if (rc != 0) {
        TM_SHELL_SUMMARY("diag", "cmd=param result=error name=%s", argv[2]);
        return SHELL_OK;
    }

    return _app_shell_diag_emit_scalar_summary("param", argv[2], value, 4U, "raw");
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_diag_command(int argc, char *argv[])
{
    if (argc < 2) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[1], "set") == 0) {
        return _app_shell_diag_handle_set(argc, argv);
    }
    if (strcmp(argv[1], "ramp") == 0) {
        return _app_shell_diag_handle_ramp(argc, argv);
    }
    if (strcmp(argv[1], "pid") == 0) {
        return _app_shell_diag_handle_pid(argc, argv);
    }
    if (strcmp(argv[1], "param") == 0) {
        return _app_shell_diag_handle_param(argc, argv);
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(diag, app_shell_diag_command, 80U, "set|ramp|pid|param", "Diag");

#endif /* APP_SHELL_HAS_DIAG_CMDS */

/* ---------------------------------------- end of file ----------------------------------------- */
