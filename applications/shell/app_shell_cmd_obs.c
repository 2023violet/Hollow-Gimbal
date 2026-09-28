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

static void _app_shell_obs_emit_curr(const struct motor_runtime_snapshot *snapshot)
{
    char iq[16];
    char id[16];

    TM_SHELL_ROW("curr",
                 "iq=%s id=%s",
                 app_shell_format_float_or_dash(iq, sizeof(iq), snapshot->i_q, 3U),
                 app_shell_format_float_or_dash(id, sizeof(id), snapshot->i_d, 3U));
}

static void _app_shell_obs_emit_bus(const struct motor_runtime_snapshot *snapshot)
{
    char vbus[16];

    TM_SHELL_ROW("bus", "vbus=%s", app_shell_format_float_or_dash(vbus, sizeof(vbus), snapshot->v_bus, 2U));
}

static void _app_shell_obs_emit_phase(const struct motor_runtime_snapshot *snapshot)
{
    char ia_raw[16];
    char ib_raw[16];
    char ic_raw[16];
    char ia_offset[16];
    char ib_offset[16];
    char ic_offset[16];

    TM_SHELL_ROW("phase",
                 "raw=%s/%s/%s off=%s/%s/%s",
                 app_shell_format_float_or_dash(ia_raw, sizeof(ia_raw), snapshot->ia_raw, 1U),
                 app_shell_format_float_or_dash(ib_raw, sizeof(ib_raw), snapshot->ib_raw, 1U),
                 app_shell_format_float_or_dash(ic_raw, sizeof(ic_raw), snapshot->ic_raw, 1U),
                 app_shell_format_float_or_dash(ia_offset, sizeof(ia_offset), snapshot->ia_offset, 1U),
                 app_shell_format_float_or_dash(ib_offset, sizeof(ib_offset), snapshot->ib_offset, 1U),
                 app_shell_format_float_or_dash(ic_offset, sizeof(ic_offset), snapshot->ic_offset, 1U));
}

static void _app_shell_obs_emit_amps(const struct motor_runtime_snapshot *snapshot)
{
    char ia[16];
    char ib[16];
    char ic[16];
    char vbus_raw[16];

    TM_SHELL_ROW("amps",
                 "i=%s/%s/%s vraw=%s",
                 app_shell_format_float_or_dash(ia, sizeof(ia), snapshot->ia, 3U),
                 app_shell_format_float_or_dash(ib, sizeof(ib), snapshot->ib, 3U),
                 app_shell_format_float_or_dash(ic, sizeof(ic), snapshot->ic, 3U),
                 app_shell_format_float_or_dash(vbus_raw, sizeof(vbus_raw), snapshot->v_bus_raw, 1U));
}

static void _app_shell_obs_emit_current_residual(const struct motor_runtime_snapshot *snapshot)
{
    char ia[16];
    char ib[16];
    char ic[16];
    char sum[16];
    float kcl_sum = snapshot->ia + snapshot->ib + snapshot->ic;

    TM_SHELL_ROW("residual",
                 "i=%s/%s/%s sum=%s unit=A",
                 app_shell_format_float_or_dash(ia, sizeof(ia), snapshot->ia, 3U),
                 app_shell_format_float_or_dash(ib, sizeof(ib), snapshot->ib, 3U),
                 app_shell_format_float_or_dash(ic, sizeof(ic), snapshot->ic, 3U),
                 app_shell_format_float_or_dash(sum, sizeof(sum), kcl_sum, 3U));
}

static void _app_shell_obs_emit_ctrl(const struct motor_runtime_snapshot *snapshot)
{
    char theta_sim[16];
    char theta_elec[16];
    char we_now[16];
    char we_ramp[16];
    char vd[16];
    char vq[16];

    TM_SHELL_ROW("ctrl",
                 "theta=%s/%s we=%s/%s vdq=%s/%s",
                 app_shell_format_float_or_dash(theta_sim, sizeof(theta_sim), snapshot->theta_sim, 2U),
                 app_shell_format_float_or_dash(theta_elec, sizeof(theta_elec), snapshot->theta_elec, 2U),
                 app_shell_format_float_or_dash(we_now, sizeof(we_now), snapshot->we_volt_now, 2U),
                 app_shell_format_float_or_dash(we_ramp, sizeof(we_ramp), snapshot->we_volt_ramp, 2U),
                 app_shell_format_float_or_dash(vd, sizeof(vd), snapshot->vd, 2U),
                 app_shell_format_float_or_dash(vq, sizeof(vq), snapshot->vq, 2U));
}

static void _app_shell_obs_emit_svpwm(const struct motor_runtime_snapshot *snapshot)
{
    char v_alpha[16];
    char v_beta[16];
    char dtc_a[16];
    char dtc_b[16];
    char dtc_c[16];

    TM_SHELL_ROW("svpwm",
                 "ab=%s/%s duty=%s/%s/%s sec=%u",
                 app_shell_format_float_or_dash(v_alpha, sizeof(v_alpha), snapshot->v_alpha, 2U),
                 app_shell_format_float_or_dash(v_beta, sizeof(v_beta), snapshot->v_beta, 2U),
                 app_shell_format_float_or_dash(dtc_a, sizeof(dtc_a), snapshot->dtc_a, 2U),
                 app_shell_format_float_or_dash(dtc_b, sizeof(dtc_b), snapshot->dtc_b, 2U),
                 app_shell_format_float_or_dash(dtc_c, sizeof(dtc_c), snapshot->dtc_c, 2U),
                 (unsigned)snapshot->sector);
}

static void _app_shell_obs_show_all(void)
{
    struct motor_runtime_snapshot snapshot;

    if (!motor_runtime_snapshot(&snapshot)) {
        app_shell_emit_snapshot_unavailable("obs", "all", NULL);
        return;
    }

    TM_SHELL_SUMMARY("obs", "cmd=all result=ok view=all");
    _app_shell_obs_emit_curr(&snapshot);
    app_shell_emit_motion_row("out", snapshot.out_vel_rpm, snapshot.out_pos_turns);
    _app_shell_obs_emit_bus(&snapshot);
    _app_shell_obs_emit_phase(&snapshot);
    _app_shell_obs_emit_amps(&snapshot);
    _app_shell_obs_emit_current_residual(&snapshot);
    _app_shell_obs_emit_ctrl(&snapshot);
    _app_shell_obs_emit_svpwm(&snapshot);
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
