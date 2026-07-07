/**
 * @file    app_shell_banner.h
 * @brief   FalconFoc shell banner hook
 * @version v1.0.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * This header exposes the product-owned shell banner hook. Transport/session
 * owners may call it, but banner vocabulary stays in the product shell layer.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_profile.h"

#if APP_SHELL_ENABLED

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

void app_shell_banner_print(void);

#ifdef __cplusplus
}
#endif

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
