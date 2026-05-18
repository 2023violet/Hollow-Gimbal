/**
 * @file    app_shell_banner.c
 * @brief   FalconFoc shell banner hook
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_banner.h"
#include "config/tm_core_config.h"
#include "config/console_session_config.h"
#include "tm_shell_output.h"

/* Public Functions ----------------------------------------------------------------------------- */

void app_shell_banner_print(void)
{
    TM_SHELL_SUMMARY("shell", "name=FalconFoc version=%s", TM_APP_FW_VERSION_STRING);
    TM_SHELL_ROW("image", "build=%s %s profile=%s flavor=%s", __DATE__, __TIME__, TM_APP_SHELL_PROFILE_NAME, TM_PRODUCT_FLAVOR_NAME);
    TM_SHELL_ROW("console", "transport=uart1-dma");
    TM_SHELL_ROW("hint", "help=type 'help'");
    TM_SHELL_PROMPT();
}

/* ---------------------------------------- end of file ----------------------------------------- */
