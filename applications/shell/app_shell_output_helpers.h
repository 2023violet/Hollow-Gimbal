/**
 * @file    app_shell_output_helpers.h
 * @brief   FalconFoc shell output helper primitives
 * @version v1.0.0
 * @date    2026.05.20
 *
 * Design overview
 * ---------------
 * This header owns shared shell-format helpers used by multiple command owners.
 * It keeps repeated rows and common summary shapes centralized without changing
 * command-level runtime semantics.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

void app_shell_emit_motion_row(const char *label, float vel_rpm, float pos_turns);
void app_shell_emit_temp_row(const char *label, float motor_temp, float driver_temp);
void app_shell_emit_encoder_row(const char *label, float mech_rad, float elec_rad, float vel_rads);
int  app_shell_emit_scalar_summary(const char *topic,
                                   const char *cmd,
                                   const char *action,
                                   const char *field,
                                   float       value,
                                   uint8_t     frac_digits,
                                   const char *unit);
void app_shell_emit_snapshot_unavailable(const char *topic, const char *cmd, const char *action);
void app_shell_emit_snapshot_unavailable_named(const char *topic,
                                               const char *cmd,
                                               const char *action,
                                               const char *name_key,
                                               const char *name_value);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
