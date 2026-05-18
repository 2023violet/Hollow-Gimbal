/**
 * @file    app_shell_cmd_sys.c
 * @brief   FalconFoc product shell `sys` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_profile.h"
#include "app_shell_task.h"
#include "config/console_session_config.h"
#include "config/tm_core_config.h"
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "cmsis_os2.h"
#include "system_port.h"
#include "task.h"
#include <string.h>
#include "tm_shell.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

static void _app_shell_sys_status(void)
{
    struct tm_shell_task_diag_snapshot snapshot = {0};
    bool                               ready;

    ready = tm_shell_task_diag_snapshot(&snapshot);

    TM_SHELL_SUMMARY("sys",
                     "cmd=status result=ok version=%s profile=%s flavor=%s",
                     TM_APP_FW_VERSION_STRING,
                     TM_APP_SHELL_PROFILE_NAME,
                     TM_PRODUCT_FLAVOR_NAME);
    TM_SHELL_ROW("runtime",
                 "tick=%lu threads=%lu heap_free=%u heap_min=%u heap_total=%u",
                 (unsigned long)osKernelGetTickCount(),
                 (unsigned long)osThreadGetCount(),
                 (unsigned)xPortGetFreeHeapSize(),
                 (unsigned)xPortGetMinimumEverFreeHeapSize(),
                 (unsigned)configTOTAL_HEAP_SIZE);

    if (ready) {
        TM_SHELL_ROW("shell",
                     "ready=%u queue=%u/%u backlog=%u/%u ok=%lu hold=%lu fail=%lu",
                     snapshot.ready ? 1U : 0U,
                     snapshot.queue_used,
                     snapshot.queue_depth,
                     snapshot.backlog_used,
                     snapshot.backlog_depth,
                     (unsigned long)snapshot.submit_ok_count,
                     (unsigned long)snapshot.submit_backlog_count,
                     (unsigned long)snapshot.submit_fail_count);
    } else {
        TM_SHELL_ROW("shell", "ready=0 status=unavailable");
    }
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_sys_command(int argc, char *argv[])
{
    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "status") == 0))) {
        _app_shell_sys_status();
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "tasks") == 0)) {
        TM_SHELL_SUMMARY("sys", "cmd=tasks result=ok count=%lu", (unsigned long)osThreadGetCount());
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "reboot") == 0)) {
        TM_SHELL_SUMMARY("sys", "cmd=reboot result=ok");
        TM_SHELL_ROW("note", "delay_ms=20");
        (void)osDelay(20U);
        system_port_reset();
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(sys, app_shell_sys_command, 10U, "[status|tasks|reboot]", "System");

/* ---------------------------------------- end of file ----------------------------------------- */
