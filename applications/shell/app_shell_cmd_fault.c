/**
 * @file    app_shell_cmd_fault.c
 * @brief   FalconFoc product shell `fault` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include <string.h>
#include "motor_runtime.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

#if APP_SHELL_ENABLED

static void _app_shell_fault_status(void)
{
    struct motor_runtime_snapshot snapshot;
    uint32_t mask;
    uint32_t bit;

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("fault", "cmd=status result=error reason=snapshot_unavailable");
        return;
    }

    mask = snapshot.fault_code;
    TM_SHELL_SUMMARY("fault",
                     "cmd=status result=ok active=%u mask=0x%08x",
                     (mask != 0U) ? 1U : 0U,
                     (unsigned)mask);

    if (mask == 0U) {
        TM_SHELL_ROW("fault", "name=none");
        return;
    }

    for (bit = 1U; bit != 0U; bit <<= 1U) {
        if ((mask & bit) != 0U) {
            TM_SHELL_ROW("fault", "name=%s bit=0x%08x", app_shell_fault_bit_name(bit), (unsigned)bit);
        }
    }
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_fault_command(int argc, char *argv[])
{
    struct motor_runtime_snapshot snapshot;
    bool                          ok;

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "status") == 0))) {
        _app_shell_fault_status();
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "clear") == 0)) {
        if (!motor_runtime_snapshot(&snapshot)) {
            TM_SHELL_SUMMARY("fault", "cmd=clear result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }

        ok = motor_runtime_request_axis_action(TM_MOTOR_RUNTIME_AXIS_CLEAR_FAULT);
        if (!motor_runtime_snapshot(&snapshot)) {
            TM_SHELL_SUMMARY("fault", "cmd=clear result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }

        if (!ok) {
            TM_SHELL_SUMMARY("fault",
                             "cmd=clear result=error axis=%s rc=-1",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("fault", "cmd=clear result=ok mask=0x%08x", (unsigned)snapshot.fault_code);
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(fault, app_shell_fault_command, 40U, "[status|clear]", "Fault");

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
