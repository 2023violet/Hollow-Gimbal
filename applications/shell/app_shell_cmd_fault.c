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
#include "motor_fsm.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

static void _app_shell_fault_status(void)
{
    uint32_t mask;
    uint32_t bit;

    mask = _motor_ins.fault_code;
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
    int rc;

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "status") == 0))) {
        _app_shell_fault_status();
        return SHELL_OK;
    }

    if ((argc == 2) && (strcmp(argv[1], "clear") == 0)) {
        rc = motor_fsm_dispatch(&_motor_ins, EV_CMD_CLR_FAULT);
        if (rc < 0) {
            TM_SHELL_SUMMARY("fault",
                             "cmd=clear result=error axis=%s rc=%d",
                             app_shell_axis_state_name(_motor_ins.axis_state),
                             rc);
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("fault", "cmd=clear result=ok mask=0x%08x", (unsigned)_motor_ins.fault_code);
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(fault, app_shell_fault_command, 40U, "[status|clear]", "Fault");

/* ---------------------------------------- end of file ----------------------------------------- */
