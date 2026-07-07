/**
 * @file    app_shell_profile.h
 * @brief   FalconFoc shell profile gates
 * @version v1.0.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * This header maps the product shell policy to simple compile-time gates used
 * by command owners.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "config/console_session_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

#define APP_SHELL_ENABLED      (TM_SHELL_ENABLE != 0U)
#define APP_SHELL_HAS_CAL_CMDS APP_SHELL_ENABLED
#define APP_SHELL_HAS_DIAG_CMDS APP_SHELL_ENABLED

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
