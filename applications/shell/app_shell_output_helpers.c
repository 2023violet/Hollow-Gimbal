/**
 * @file    app_shell_output_helpers.c
 * @brief   FalconFoc shell output helper primitives
 * @version v1.0.0
 * @date    2026.05.20
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_output_helpers.h"
#include "app_shell_common.h"
#include "tm_shell_output.h"

/* Public Functions ----------------------------------------------------------------------------- */

void app_shell_emit_motion_row(const char *label, float vel_rpm, float pos_turns)
{
    char vel[16];
    char pos[16];

    TM_SHELL_ROW(label,
                 "vel_rpm=%s pos_turn=%s",
                 app_shell_format_float_or_dash(vel, sizeof(vel), vel_rpm, 2U),
                 app_shell_format_float_or_dash(pos, sizeof(pos), pos_turns, 3U));
}

void app_shell_emit_temp_row(const char *label, float motor_temp, float driver_temp)
{
    char motor[16];
    char driver[16];

    TM_SHELL_ROW(label,
                 "motor=%s driver=%s",
                 app_shell_format_float_or_dash(motor, sizeof(motor), motor_temp, 1U),
                 app_shell_format_float_or_dash(driver, sizeof(driver), driver_temp, 1U));
}

void app_shell_emit_encoder_row(const char *label, float mech_rad, float elec_rad, float vel_rads)
{
    char mech[16];
    char elec[16];
    char omega[16];

    TM_SHELL_ROW(label,
                 "mech_rad=%s elec_rad=%s vel_rads=%s",
                 app_shell_format_float_or_dash(mech, sizeof(mech), mech_rad, 3U),
                 app_shell_format_float_or_dash(elec, sizeof(elec), elec_rad, 3U),
                 app_shell_format_float_or_dash(omega, sizeof(omega), vel_rads, 3U));
}

int app_shell_emit_scalar_summary(const char *topic,
                                  const char *cmd,
                                  const char *action,
                                  const char *field,
                                  float       value,
                                  uint8_t     frac_digits,
                                  const char *unit)
{
    char buffer[16];

    if ((topic == NULL) || (cmd == NULL) || (field == NULL) || (unit == NULL)) {
        return SHELL_ERR_ARG;
    }

    if ((action != NULL) && (action[0] != '\0')) {
        TM_SHELL_SUMMARY(topic,
                         "cmd=%s action=%s result=ok %s=%s unit=%s",
                         cmd,
                         action,
                         field,
                         app_shell_format_float_or_dash(buffer, sizeof(buffer), value, frac_digits),
                         unit);
    } else {
        TM_SHELL_SUMMARY(topic,
                         "cmd=%s result=ok %s=%s unit=%s",
                         cmd,
                         field,
                         app_shell_format_float_or_dash(buffer, sizeof(buffer), value, frac_digits),
                         unit);
    }

    return SHELL_OK;
}

void app_shell_emit_snapshot_unavailable(const char *topic, const char *cmd, const char *action)
{
    if ((topic == NULL) || (cmd == NULL)) {
        return;
    }

    if ((action != NULL) && (action[0] != '\0')) {
        TM_SHELL_SUMMARY(topic,
                         "cmd=%s action=%s result=error reason=snapshot_unavailable",
                         cmd,
                         action);
    } else {
        TM_SHELL_SUMMARY(topic, "cmd=%s result=error reason=snapshot_unavailable", cmd);
    }
}

void app_shell_emit_snapshot_unavailable_named(const char *topic,
                                               const char *cmd,
                                               const char *action,
                                               const char *name_key,
                                               const char *name_value)
{
    if ((topic == NULL) || (cmd == NULL) || (name_key == NULL) || (name_value == NULL)) {
        return;
    }

    if ((action != NULL) && (action[0] != '\0')) {
        TM_SHELL_SUMMARY(topic,
                         "cmd=%s action=%s result=error reason=snapshot_unavailable %s=%s",
                         cmd,
                         action,
                         name_key,
                         name_value);
    } else {
        TM_SHELL_SUMMARY(topic,
                         "cmd=%s result=error reason=snapshot_unavailable %s=%s",
                         cmd,
                         name_key,
                         name_value);
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
