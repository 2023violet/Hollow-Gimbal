/**
 * @file    app_shell_cmd_obs.c
 * @brief   FalconFoc product shell `obs` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_output_helpers.h"
#include <string.h>
#include "motor_runtime.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

#if APP_SHELL_ENABLED

static void _app_shell_obs_show_all(void)
{
    struct motor_runtime_snapshot snapshot;
    char iq[16];
    char id[16];
    char vbus[16];

    if (!motor_runtime_snapshot(&snapshot)) {
        app_shell_emit_snapshot_unavailable("obs", "all", NULL);
        return;
    }

    TM_SHELL_SUMMARY("obs", "cmd=all result=ok view=all");
    TM_SHELL_ROW("curr",
                 "iq=%s id=%s",
                 app_shell_format_float_or_dash(iq, sizeof(iq), snapshot.i_q, 3U),
                 app_shell_format_float_or_dash(id, sizeof(id), snapshot.i_d, 3U));
    app_shell_emit_motion_row("out", snapshot.out_vel_rpm, snapshot.out_pos_turns);
    TM_SHELL_ROW("bus", "vbus=%s", app_shell_format_float_or_dash(vbus, sizeof(vbus), snapshot.v_bus, 2U));
    app_shell_emit_temp_row("temp", snapshot.motor_temp, snapshot.driver_temp);
    app_shell_emit_encoder_row("enc",
                               snapshot.angle_mech_rad,
                               snapshot.angle_elec_rad,
                               snapshot.encoder_vel_rads);
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_obs_command(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "all") == 0))) {
        _app_shell_obs_show_all();
        return SHELL_OK;
    }

    if (argc != 2) {
        return SHELL_ERR_ARGC;
    }

    if (!motor_runtime_snapshot(&snapshot)) {
        app_shell_emit_snapshot_unavailable("obs", argv[1], NULL);
        return SHELL_OK;
    }

    if (strcmp(argv[1], "iq") == 0) {
        return app_shell_emit_scalar_summary("obs", "iq", NULL, "value", snapshot.i_q, 3U, "A");
    }
    if (strcmp(argv[1], "id") == 0) {
        return app_shell_emit_scalar_summary("obs", "id", NULL, "value", snapshot.i_d, 3U, "A");
    }
    if (strcmp(argv[1], "vel") == 0) {
        return app_shell_emit_scalar_summary("obs", "vel", NULL, "value", snapshot.out_vel_rpm, 2U, "rpm");
    }
    if (strcmp(argv[1], "pos") == 0) {
        return app_shell_emit_scalar_summary("obs", "pos", NULL, "value", snapshot.out_pos_turns, 3U, "turn");
    }
    if (strcmp(argv[1], "vbus") == 0) {
        return app_shell_emit_scalar_summary("obs", "vbus", NULL, "value", snapshot.v_bus, 2U, "V");
    }
    if (strcmp(argv[1], "temp") == 0) {
        TM_SHELL_SUMMARY("obs", "cmd=temp result=ok");
        app_shell_emit_temp_row("temp", snapshot.motor_temp, snapshot.driver_temp);
        return SHELL_OK;
    }
    if (strcmp(argv[1], "enc") == 0) {
        TM_SHELL_SUMMARY("obs", "cmd=enc result=ok");
        app_shell_emit_encoder_row("enc",
                                   snapshot.angle_mech_rad,
                                   snapshot.angle_elec_rad,
                                   snapshot.encoder_vel_rads);
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(obs, app_shell_obs_command, 30U, "[all|iq|id|vel|pos|vbus|temp|enc]", "Observe");

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
