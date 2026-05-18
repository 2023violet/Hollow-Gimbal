/**
 * @file    app_shell_cmd_obs.c
 * @brief   FalconFoc product shell `obs` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include <string.h>
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

static void _app_shell_obs_show_all(void)
{
    char iq[16];
    char id[16];
    char vel[16];
    char pos[16];
    char vbus[16];
    char motor_temp[16];
    char driver_temp[16];
    char mech[16];
    char elec[16];
    char omega[16];

    TM_SHELL_SUMMARY("obs", "cmd=show result=ok view=all");
    TM_SHELL_ROW("curr",
                 "iq=%s id=%s",
                 app_shell_format_float_or_dash(iq, sizeof(iq), _motor_ins.foc_calc.i_q, 3U),
                 app_shell_format_float_or_dash(id, sizeof(id), _motor_ins.foc_calc.i_d, 3U));
    TM_SHELL_ROW("out",
                 "vel_rpm=%s pos_turn=%s",
                 app_shell_format_float_or_dash(vel, sizeof(vel), _motor_ins.meas_data.out_vel_rpm, 2U),
                 app_shell_format_float_or_dash(pos, sizeof(pos), _motor_ins.meas_data.out_pos_turns, 3U));
    TM_SHELL_ROW("bus", "vbus=%s", app_shell_format_float_or_dash(vbus, sizeof(vbus), _motor_ins.meas_data.v_bus, 2U));
    TM_SHELL_ROW("temp",
                 "motor=%s driver=%s",
                 app_shell_format_float_or_dash(motor_temp, sizeof(motor_temp), _motor_ins.meas_data.motor_temp, 1U),
                 app_shell_format_float_or_dash(driver_temp, sizeof(driver_temp), _motor_ins.meas_data.driver_temp, 1U));
    TM_SHELL_ROW("enc",
                 "mech_rad=%s elec_rad=%s vel_rads=%s",
                 app_shell_format_float_or_dash(mech, sizeof(mech), _motor_ins.enc_pri.state.angle_mech_rad, 3U),
                 app_shell_format_float_or_dash(elec, sizeof(elec), _motor_ins.enc_pri.state.angle_elec_rad, 3U),
                 app_shell_format_float_or_dash(omega, sizeof(omega), _motor_ins.enc_pri.state.velocity_rads, 3U));
}

static int _app_shell_obs_show_scalar(const char *key, float value, uint8_t frac_digits, const char *unit)
{
    char buffer[16];

    TM_SHELL_SUMMARY("obs",
                     "key=%s result=ok val=%s unit=%s",
                     key,
                     app_shell_format_float_or_dash(buffer, sizeof(buffer), value, frac_digits),
                     unit);
    return SHELL_OK;
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_obs_command(int argc, char *argv[])
{
    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "all") == 0))) {
        _app_shell_obs_show_all();
        return SHELL_OK;
    }

    if (argc != 2) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[1], "iq") == 0) {
        return _app_shell_obs_show_scalar("iq", _motor_ins.foc_calc.i_q, 3U, "A");
    }
    if (strcmp(argv[1], "id") == 0) {
        return _app_shell_obs_show_scalar("id", _motor_ins.foc_calc.i_d, 3U, "A");
    }
    if (strcmp(argv[1], "vel") == 0) {
        return _app_shell_obs_show_scalar("vel", _motor_ins.meas_data.out_vel_rpm, 2U, "rpm");
    }
    if (strcmp(argv[1], "pos") == 0) {
        return _app_shell_obs_show_scalar("pos", _motor_ins.meas_data.out_pos_turns, 3U, "turn");
    }
    if (strcmp(argv[1], "vbus") == 0) {
        return _app_shell_obs_show_scalar("vbus", _motor_ins.meas_data.v_bus, 2U, "V");
    }
    if (strcmp(argv[1], "temp") == 0) {
        char motor_temp[16];
        char driver_temp[16];

        TM_SHELL_SUMMARY("obs", "key=temp result=ok");
        TM_SHELL_ROW("temp",
                     "motor=%s driver=%s",
                     app_shell_format_float_or_dash(motor_temp, sizeof(motor_temp), _motor_ins.meas_data.motor_temp, 1U),
                     app_shell_format_float_or_dash(driver_temp, sizeof(driver_temp), _motor_ins.meas_data.driver_temp, 1U));
        return SHELL_OK;
    }
    if (strcmp(argv[1], "enc") == 0) {
        char mech[16];
        char elec[16];
        char omega[16];

        TM_SHELL_SUMMARY("obs", "key=enc result=ok");
        TM_SHELL_ROW("enc",
                     "mech_rad=%s elec_rad=%s vel_rads=%s",
                     app_shell_format_float_or_dash(mech, sizeof(mech), _motor_ins.enc_pri.state.angle_mech_rad, 3U),
                     app_shell_format_float_or_dash(elec, sizeof(elec), _motor_ins.enc_pri.state.angle_elec_rad, 3U),
                     app_shell_format_float_or_dash(omega, sizeof(omega), _motor_ins.enc_pri.state.velocity_rads, 3U));
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(obs, app_shell_obs_command, 30U, "[all|iq|id|vel|pos|vbus|temp|enc]", "Observe");

/* ---------------------------------------- end of file ----------------------------------------- */
