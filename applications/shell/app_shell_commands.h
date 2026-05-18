/**
 * @file    app_shell_commands.h
 * @brief   FalconFoc product shell command entry points
 * @version v1.0.0
 * @date    2026.05.12
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

int app_shell_sys_command(int argc, char *argv[]);
int app_shell_state_command(int argc, char *argv[]);
int app_shell_obs_command(int argc, char *argv[]);
int app_shell_fault_command(int argc, char *argv[]);
int app_shell_store_command(int argc, char *argv[]);
int app_shell_cfg_command(int argc, char *argv[]);

#if APP_SHELL_HAS_FACTORY_CMDS
int app_shell_cal_command(int argc, char *argv[]);
#endif

#if APP_SHELL_HAS_DIAG_CMDS
int app_shell_diag_command(int argc, char *argv[]);
#endif

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
