/**
 * @file    app_shell_cmd_diag.c
 * @brief   FalconFoc product shell `diag` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_output_helpers.h"
#include "app_shell_profile.h"
#include <math.h>
#include <string.h>
#include "motor_runtime.h"
#include "tm_shell_output.h"

#if APP_SHELL_ENABLED && APP_SHELL_HAS_DIAG_CMDS

/* Private Helpers ------------------------------------------------------------------------------ */

static bool _app_shell_diag_is_pid_loop(const char *name)
{
    return (name != NULL) && ((strcmp(name, "id") == 0) || (strcmp(name, "iq") == 0) ||
                              (strcmp(name, "vel") == 0) || (strcmp(name, "pos") == 0));
}

static bool _app_shell_diag_is_pid_field(const char *name)
{
    return (name != NULL) && ((strcmp(name, "kp") == 0) || (strcmp(name, "ki") == 0) ||
                              (strcmp(name, "kd") == 0) || (strcmp(name, "lim") == 0));
}

static bool _app_shell_diag_is_param_name(const char *name)
{
    return (name != NULL) &&
           ((strcmp(name, "rs") == 0) || (strcmp(name, "ls") == 0) || (strcmp(name, "flux") == 0) ||
            (strcmp(name, "poles") == 0) || (strcmp(name, "js") == 0) ||
            (strcmp(name, "gear") == 0) || (strcmp(name, "vov") == 0) ||
            (strcmp(name, "vuv") == 0) || (strcmp(name, "imax") == 0) ||
            (strcmp(name, "velmax") == 0) || (strcmp(name, "currbw") == 0) ||
            (strcmp(name, "velbw") == 0) || (strcmp(name, "posbw") == 0));
}

static bool _app_shell_diag_param_requires_positive(const char *name)
{
    return (name != NULL) &&
           ((strcmp(name, "currbw") == 0) || (strcmp(name, "velbw") == 0) || (strcmp(name, "posbw") == 0));
}

static void _app_shell_diag_emit_pid_error(const char *action,
                                           const char *reason,
                                           const char *loop,
                                           const char *field,
                                           const char *axis)
{
    if ((field != NULL) && (axis != NULL)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=pid action=%s result=error axis=%s reason=%s loop=%s field=%s",
                         action,
                         axis,
                         reason,
                         loop,
                         field);
        return;
    }

    if (field != NULL) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=pid action=%s result=error reason=%s loop=%s field=%s",
                         action,
                         reason,
                         loop,
                         field);
        return;
    }

    if (axis != NULL) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=pid action=%s result=error axis=%s reason=%s loop=%s",
                         action,
                         axis,
                         reason,
                         loop);
        return;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=pid action=%s result=error reason=%s loop=%s",
                     action,
                     reason,
                     loop);
}

static void _app_shell_diag_emit_param_error(const char *action,
                                             const char *reason,
                                             const char *name,
                                             const char *axis,
                                             float       value,
                                             bool        include_value)
{
    char buffer[16];

    if (include_value && (axis != NULL)) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=param action=%s result=error axis=%s reason=%s name=%s value=%s unit=raw",
                         action,
                         axis,
                         reason,
                         name,
                         app_shell_format_float_or_dash(buffer, sizeof(buffer), value, 4U));
        return;
    }

    if (include_value) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=param action=%s result=error reason=%s name=%s value=%s unit=raw",
                         action,
                         reason,
                         name,
                         app_shell_format_float_or_dash(buffer, sizeof(buffer), value, 4U));
        return;
    }

    if (axis != NULL) {
        TM_SHELL_SUMMARY("diag",
                         "cmd=param action=%s result=error axis=%s reason=%s name=%s",
                         action,
                         axis,
                         reason,
                         name);
        return;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=param action=%s result=error reason=%s name=%s",
                     action,
                     reason,
                     name);
}

static bool _app_shell_diag_reject_pid_write_if_not_idle(const char *action,
                                                         const char *loop,
                                                         const char *field)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot)) {
        _app_shell_diag_emit_pid_error(action, "snapshot_unavailable", loop, field, NULL);
        return true;
    }

    if (snapshot.axis_state == AXIS_STATE_IDLE) {
        return false;
    }

    _app_shell_diag_emit_pid_error(action,
                                   "idle_only",
                                   loop,
                                   field,
                                   app_shell_axis_state_name(snapshot.axis_state));
    return true;
}

static bool _app_shell_diag_reject_param_write_if_not_idle(const char *action, const char *name, float value)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot)) {
        _app_shell_diag_emit_param_error(action, "snapshot_unavailable", name, NULL, value, false);
        return true;
    }

    if (snapshot.axis_state == AXIS_STATE_IDLE) {
        return false;
    }

    _app_shell_diag_emit_param_error(action,
                                     "idle_only",
                                     name,
                                     app_shell_axis_state_name(snapshot.axis_state),
                                     value,
                                     false);
    return true;
}

static void _app_shell_diag_param_show(void)
{
    struct motor_runtime_diag_param_view view;
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

    if (!motor_runtime_diag_read_params(&view)) {
        app_shell_emit_snapshot_unavailable("diag", "param", "show");
        return;
    }

    TM_SHELL_SUMMARY("diag", "cmd=param action=show result=ok");
    TM_SHELL_ROW("motor",
                 "poles=%u rs=%s ls=%s flux=%s js=%s",
                 (unsigned)view.poles,
                 app_shell_format_float_or_dash(rs, sizeof(rs), view.rs, 4U),
                 app_shell_format_float_or_dash(ls, sizeof(ls), view.ls, 4U),
                 app_shell_format_float_or_dash(flux, sizeof(flux), view.flux, 4U),
                 app_shell_format_float_or_dash(js, sizeof(js), view.js, 4U));
    TM_SHELL_ROW("gear",
                 "ratio=%s",
                 app_shell_format_float_or_dash(gear, sizeof(gear), view.gear_ratio, 3U));
    TM_SHELL_ROW("prot",
                 "vov=%s vuv=%s imax=%s velmax_rpm=%s",
                 app_shell_format_float_or_dash(vov, sizeof(vov), view.v_bus_ov_thr, 2U),
                 app_shell_format_float_or_dash(vuv, sizeof(vuv), view.v_bus_uv_thr, 2U),
                 app_shell_format_float_or_dash(imax, sizeof(imax), view.i_peak, 2U),
                 app_shell_format_float_or_dash(velmax, sizeof(velmax), view.vel_over_rpm, 2U));
    TM_SHELL_ROW("band",
                 "curr=%s vel=%s pos=%s",
                 app_shell_format_float_or_dash(currbw, sizeof(currbw), view.curr_bandwidth, 1U),
                 app_shell_format_float_or_dash(velbw, sizeof(velbw), view.vel_bandwidth, 1U),
                 app_shell_format_float_or_dash(posbw, sizeof(posbw), view.pos_bandwidth, 1U));
}

static void _app_shell_diag_pid_row(const char *name, const struct motor_runtime_pid_view *view)
{
    char kp[16];
    char ki[16];
    char kd[16];
    char lim[16];

    TM_SHELL_ROW("pid",
                 "loop=%s kp=%s ki=%s kd=%s lim=%s",
                 name,
                 app_shell_format_float_or_dash(kp, sizeof(kp), view->kp, 4U),
                 app_shell_format_float_or_dash(ki, sizeof(ki), view->ki, 4U),
                 app_shell_format_float_or_dash(kd, sizeof(kd), view->kd, 4U),
                 app_shell_format_float_or_dash(lim, sizeof(lim), view->lim, 4U));
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
        if (!motor_runtime_request_set_passthrough(CTRL_MODE_VEL, FOC_RPM_TO_RADS(value), limit)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "vel_rpm", value, 2U, "rpm");
    }
    if (strcmp(argv[2], "pos") == 0) {
        if (!motor_runtime_request_set_passthrough(
                CTRL_MODE_POS,
                value * FOC_2PI,
                (limit > 0.0f) ? FOC_RPM_TO_RADS(limit) : 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "pos_turn", value, 3U, "turn");
    }
    if (strcmp(argv[2], "iq") == 0) {
        if (!motor_runtime_request_set_passthrough(CTRL_MODE_CURR, value, 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "iq", value, 3U, "A");
    }
    if (strcmp(argv[2], "vd") == 0) {
        if (!motor_runtime_diag_set_voltage(value, 0.0f, limit)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "vd", value, 3U, "V");
    }
    if (strcmp(argv[2], "vq") == 0) {
        if (!motor_runtime_diag_set_voltage(0.0f, value, limit)) {
            app_shell_emit_snapshot_unavailable("diag", "set", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "set", NULL, "vq", value, 3U, "V");
    }

    return SHELL_ERR_ARG;
}

static int _app_shell_diag_handle_ramp(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    float value;
    float arg1;
    float arg2;
    float arg3;

    if (!motor_runtime_snapshot(&snapshot)) {
        app_shell_emit_snapshot_unavailable("diag", "ramp", NULL);
        return SHELL_OK;
    }

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
        if (!motor_runtime_request_vel_ramp(FOC_RPM_TO_RADS(value),
                                            (arg1 > 0.0f) ? FOC_RPM_TO_RADS(arg1) : 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "ramp", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "ramp", NULL, "vel_rpm", value, 2U, "rpm");
    }
    if (strcmp(argv[2], "iq") == 0) {
        if (!motor_runtime_request_torque_ramp(value, arg1)) {
            app_shell_emit_snapshot_unavailable("diag", "ramp", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "ramp", NULL, "iq", value, 3U, "A");
    }
    if (strcmp(argv[2], "pos") == 0) {
        if (!motor_runtime_request_pos_traj(value * FOC_2PI,
                                            (arg1 > 0.0f) ? FOC_RPM_TO_RADS(arg1) : 0.0f,
                                            (arg2 > 0.0f) ? FOC_RPM_TO_RADS(arg2) : 0.0f,
                                            (arg3 > 0.0f) ? FOC_RPM_TO_RADS(arg3) : 0.0f)) {
            app_shell_emit_snapshot_unavailable("diag", "ramp", NULL);
            return SHELL_OK;
        }
        return app_shell_emit_scalar_summary("diag", "ramp", NULL, "pos_turn", value, 3U, "turn");
    }

    return SHELL_ERR_ARG;
}

static int _app_shell_diag_handle_pid(int argc, char *argv[])
{
    struct motor_runtime_pid_view pid_id_view;
    struct motor_runtime_pid_view pid_iq_view;
    struct motor_runtime_pid_view pid_vel_view;
    struct motor_runtime_pid_view pid_pos_view;
    struct motor_runtime_pid_view pid_view;
    char                         buffer[16];
    float                        value;

    if (argc < 4) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[2], "all") == 0) {
        if ((argc != 4) || (strcmp(argv[3], "show") != 0)) {
            return SHELL_ERR_ARG;
        }

        if (!motor_runtime_diag_read_pid("id", &pid_id_view) || !motor_runtime_diag_read_pid("iq", &pid_iq_view) ||
            !motor_runtime_diag_read_pid("vel", &pid_vel_view) || !motor_runtime_diag_read_pid("pos", &pid_pos_view)) {
            app_shell_emit_snapshot_unavailable_named("diag", "pid", "show", "loop", "all");
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("diag", "cmd=pid action=show result=ok loop=all");
        _app_shell_diag_pid_row("id", &pid_id_view);
        _app_shell_diag_pid_row("iq", &pid_iq_view);
        _app_shell_diag_pid_row("vel", &pid_vel_view);
        _app_shell_diag_pid_row("pos", &pid_pos_view);
        return SHELL_OK;
    }

    if (strcmp(argv[3], "show") == 0) {
        if (!_app_shell_diag_is_pid_loop(argv[2])) {
            _app_shell_diag_emit_pid_error("show", "bad_loop", argv[2], NULL, NULL);
            return SHELL_OK;
        }

        if (!motor_runtime_diag_read_pid(argv[2], &pid_view)) {
            app_shell_emit_snapshot_unavailable_named("diag", "pid", "show", "loop", argv[2]);
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("diag", "cmd=pid action=show result=ok loop=%s", argv[2]);
        _app_shell_diag_pid_row(argv[2], &pid_view);
        return SHELL_OK;
    }

    if (strcmp(argv[3], "reset") == 0) {
        if (!_app_shell_diag_is_pid_loop(argv[2])) {
            _app_shell_diag_emit_pid_error("reset", "bad_loop", argv[2], NULL, NULL);
            return SHELL_OK;
        }

        if (_app_shell_diag_reject_pid_write_if_not_idle("reset", argv[2], NULL)) {
            return SHELL_OK;
        }

        if (!motor_runtime_diag_reset_pid(argv[2])) {
            _app_shell_diag_emit_pid_error("reset", "snapshot_unavailable", argv[2], NULL, NULL);
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("diag", "cmd=pid action=reset result=ok loop=%s", argv[2]);
        return SHELL_OK;
    }

    if ((argc != 5) || (app_shell_parse_float_arg(argv[4], &value) != 0)) {
        return SHELL_ERR_ARGC;
    }

    if (!_app_shell_diag_is_pid_loop(argv[2])) {
        _app_shell_diag_emit_pid_error("set", "bad_loop", argv[2], argv[3], NULL);
        return SHELL_OK;
    }
    if (!_app_shell_diag_is_pid_field(argv[3])) {
        _app_shell_diag_emit_pid_error("set", "bad_field", argv[2], argv[3], NULL);
        return SHELL_OK;
    }
    if (_app_shell_diag_reject_pid_write_if_not_idle("set", argv[2], argv[3])) {
        return SHELL_OK;
    }

    if (strcmp(argv[3], "lim") == 0) {
        value = fabsf(value);
    }

    if (!motor_runtime_diag_set_pid(argv[2], argv[3], value)) {
        _app_shell_diag_emit_pid_error("set", "snapshot_unavailable", argv[2], argv[3], NULL);
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=pid action=set result=ok loop=%s field=%s value=%s unit=raw",
                     argv[2],
                     argv[3],
                     app_shell_format_float_or_dash(buffer, sizeof(buffer), value, 4U));
    return SHELL_OK;
}

static int _app_shell_diag_handle_param(int argc, char *argv[])
{
    char  buffer[16];
    float value;

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

    if (!_app_shell_diag_is_param_name(argv[2])) {
        _app_shell_diag_emit_param_error("set", "bad_name", argv[2], NULL, value, false);
        return SHELL_OK;
    }
    if (_app_shell_diag_param_requires_positive(argv[2]) && (value <= 0.0f)) {
        _app_shell_diag_emit_param_error("set", "bad_value", argv[2], NULL, value, true);
        return SHELL_OK;
    }
    if (_app_shell_diag_reject_param_write_if_not_idle("set", argv[2], value)) {
        return SHELL_OK;
    }

    if (!motor_runtime_diag_set_param(argv[2], value)) {
        _app_shell_diag_emit_param_error("set", "snapshot_unavailable", argv[2], NULL, value, false);
        return SHELL_OK;
    }

    TM_SHELL_SUMMARY("diag",
                     "cmd=param action=set result=ok name=%s value=%s unit=raw",
                     argv[2],
                     app_shell_format_float_or_dash(buffer, sizeof(buffer), value, 4U));
    return SHELL_OK;
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
