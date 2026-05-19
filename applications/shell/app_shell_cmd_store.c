/**
 * @file    app_shell_cmd_store.c
 * @brief   FalconFoc product shell `store` command
 * @version v1.0.0
 * @date    2026.05.12
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "app_shell_commands.h"
#include "app_shell_common.h"
#include "app_shell_profile.h"
#include <string.h>
#include "foc_param.h"
#include "motor_task.h"
#include "tm_format.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

static void _app_shell_store_slot_row(const char *name, const struct param_slot_status *slot)
{
    char version[12];
    char size[16];

    if ((slot == NULL) || (slot->status == PARAM_EMPTY)) {
        TM_SHELL_ROW("slot",
                     "name=%s status=%s ver=- size=- addr=0x%08x",
                     name,
                     foc_param_status_str((slot != NULL) ? slot->status : PARAM_ERR_FLASH),
                     (unsigned)((slot != NULL) ? slot->addr : 0U));
        return;
    }

    (void)tm_format_snprintf(version, sizeof(version), "%u", (unsigned)slot->version);
    (void)tm_format_snprintf(size, sizeof(size), "%uB", (unsigned)slot->data_len);
    TM_SHELL_ROW("slot",
                 "name=%s status=%s ver=%s size=%s addr=0x%08x",
                 name,
                 foc_param_status_str(slot->status),
                 version,
                 size,
                 (unsigned)slot->addr);
}

static void _app_shell_store_status(void)
{
    struct param_status_all status;

    foc_param_check_all(&status);
    TM_SHELL_SUMMARY("store", "cmd=status result=ok");
    _app_shell_store_slot_row("motor", &status.motor);
    _app_shell_store_slot_row("enc", &status.enc);
    _app_shell_store_slot_row("sys", &status.sys);
}

static int _app_shell_store_resolve_target(const char *name, uint8_t *mask_out)
{
    if ((name == NULL) || (mask_out == NULL)) {
        return -1;
    }

    if (strcmp(name, "motor") == 0) {
        *mask_out = PARAM_SAVE_MOTOR;
        return 0;
    }
    if (strcmp(name, "enc") == 0) {
        *mask_out = PARAM_SAVE_ENC;
        return 0;
    }
    if (strcmp(name, "sys") == 0) {
        *mask_out = PARAM_SAVE_SYS;
        return 0;
    }
    if (strcmp(name, "all") == 0) {
        *mask_out = PARAM_SAVE_ALL;
        return 0;
    }

    return -1;
}

static void _app_shell_store_result_row(const char *name, int status)
{
    TM_SHELL_ROW("slot", "name=%s status=%s", name, foc_param_status_str(status));
}

static bool _app_shell_store_requires_idle(const char *verb, uint8_t mask)
{
    if (verb == NULL) {
        return false;
    }

    if (strcmp(verb, "erase") == 0) {
        return true;
    }

    if (strcmp(verb, "save") == 0) {
        return true;
    }

    if ((strcmp(verb, "load") == 0) && ((mask & (PARAM_SAVE_MOTOR | PARAM_SAVE_SYS)) != 0U)) {
        return true;
    }

    return false;
}

/* Public Functions ----------------------------------------------------------------------------- */

int app_shell_store_command(int argc, char *argv[])
{
    uint8_t mask;
    bool    ok;
    bool    needs_runtime_apply;
    int     status;

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "status") == 0))) {
        _app_shell_store_status();
        return SHELL_OK;
    }

    if ((argc < 2) || (argc > 3)) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[1], "erase") == 0) {
        if (_app_shell_store_requires_idle("erase", PARAM_SAVE_ALL) &&
            (_motor_ins.axis_state != AXIS_STATE_IDLE)) {
            TM_SHELL_SUMMARY("store",
                             "cmd=erase result=error axis=%s",
                             app_shell_axis_state_name(_motor_ins.axis_state));
            return SHELL_OK;
        }

        foc_param_erase_all();
        TM_SHELL_SUMMARY("store", "cmd=erase result=ok target=all");
        return SHELL_OK;
    }

    if (argc != 3) {
        return SHELL_ERR_ARGC;
    }

    if (_app_shell_store_resolve_target(argv[2], &mask) != 0) {
        return SHELL_ERR_ARG;
    }

    if (_app_shell_store_requires_idle(argv[1], mask) && (_motor_ins.axis_state != AXIS_STATE_IDLE)) {
        TM_SHELL_SUMMARY("store",
                         "cmd=%s result=error axis=%s target=%s",
                         argv[1],
                         app_shell_axis_state_name(_motor_ins.axis_state),
                         argv[2]);
        return SHELL_OK;
    }

    if (strcmp(argv[1], "save") == 0) {
        _motor_ins.calib_state.param_save_req |= mask;
        TM_SHELL_SUMMARY("store", "cmd=save result=ok target=%s mode=deferred", argv[2]);

        if ((mask & PARAM_SAVE_MOTOR) != 0U) {
            TM_SHELL_ROW("slot", "name=motor status=pending");
        }
        if ((mask & PARAM_SAVE_ENC) != 0U) {
            TM_SHELL_ROW("slot", "name=enc status=pending");
        }
        if ((mask & PARAM_SAVE_SYS) != 0U) {
            TM_SHELL_ROW("slot", "name=sys status=pending");
        }
        TM_SHELL_ROW("note", "writer=safe-task");
        return SHELL_OK;
    }

    if (strcmp(argv[1], "load") == 0) {
        ok                  = true;
        needs_runtime_apply = false;
        TM_SHELL_SUMMARY("store", "cmd=load result=ok target=%s", argv[2]);

        if ((mask & PARAM_SAVE_MOTOR) != 0U) {
            status = foc_param_load_motor(&_motor_ins);
            _app_shell_store_result_row("motor", status);
            if (status == PARAM_OK) {
                needs_runtime_apply = true;
            }
            ok = ok && (status == PARAM_OK);
        }
        if ((mask & PARAM_SAVE_ENC) != 0U) {
            status = foc_param_load_enc(&_motor_ins);
            _app_shell_store_result_row("enc", status);
            ok = ok && (status == PARAM_OK);
        }
        if ((mask & PARAM_SAVE_SYS) != 0U) {
            status = foc_param_load_sys(&_motor_ins);
            _app_shell_store_result_row("sys", status);
            if (status == PARAM_OK) {
                needs_runtime_apply = true;
            }
            ok = ok && (status == PARAM_OK);
        }

        if (ok && needs_runtime_apply) {
            motor_restore_after_param_load(&_motor_ins);
        }

        if (!ok) {
            TM_SHELL_ROW("note", "result=error partial=1");
        }
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(store, app_shell_store_command, 50U, "[status|save <slot>|load <slot>|erase]", "Store");

/* ---------------------------------------- end of file ----------------------------------------- */
