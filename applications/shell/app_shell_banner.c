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

#if APP_SHELL_ENABLED

void app_shell_banner_print(void)
{
    TM_SHELL_SUMMARY("shell", "name=FalconFoc version=%s", TM_APP_FW_VERSION_STRING);
    TM_SHELL_ROW("image",
                 "build_date=%s build_time=%s shell=%s flavor=%s",
                 __DATE__,
                 __TIME__,
                 tm_shell_onoff_name(TM_SHELL_ENABLE != 0U),
                 TM_PRODUCT_FLAVOR_NAME);
    TM_SHELL_ROW("console", "uart=USART1 baud=1000000 transport=dma");
    TM_SHELL_ROW("hint", "next='help'");
    TM_SHELL_PROMPT();
}

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
