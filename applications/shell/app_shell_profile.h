/**
 * @file    app_shell_profile.h
 * @brief   FalconFoc shell profile gates
 * @version v1.0.0
 * @date    2026.05.12
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "config/console_session_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

#define APP_SHELL_PROFILE_PRODUCT (TM_APP_SHELL_PROFILE_PRODUCT != 0U)
#define APP_SHELL_PROFILE_FACTORY (TM_APP_SHELL_PROFILE_FACTORY != 0U)
#define APP_SHELL_PROFILE_DIAG    (TM_APP_SHELL_PROFILE_DIAG != 0U)

#define APP_SHELL_HAS_FACTORY_CMDS \
    ((TM_APP_SHELL_PROFILE_FACTORY != 0U) || (TM_APP_SHELL_PROFILE_DIAG != 0U))
#define APP_SHELL_HAS_DIAG_CMDS (TM_APP_SHELL_PROFILE_DIAG != 0U)

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
