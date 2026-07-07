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
#include "tm_format.h"
#include "tm_shell_output.h"

/* Private Helpers ------------------------------------------------------------------------------ */

#if APP_SHELL_ENABLED

static void _app_shell_store_slot_row(const char *name, const struct param_slot_status *slot)
{
    char version[12];
    char size[16];

    if ((slot == NULL) || (slot->status == PARAM_EMPTY)) {
        TM_SHELL_ROW("slot",
                     "name=%s status=%s version=- data=- addr=0x%08x",
                     name,
                     foc_param_status_str((slot != NULL) ? slot->status : PARAM_ERR_FLASH),
                     (unsigned)((slot != NULL) ? slot->addr : 0U));
        return;
    }

    (void)tm_format_snprintf(version, sizeof(version), "%u", (unsigned)slot->version);
    (void)tm_format_snprintf(size, sizeof(size), "%uB", (unsigned)slot->data_len);
    TM_SHELL_ROW("slot",
                 "name=%s status=%s version=%s data=%s addr=0x%08x",
                 name,
                 foc_param_status_str(slot->status),
                 version,
                 size,
                 (unsigned)slot->addr);
}

static void _app_shell_store_status(void)
{
    struct param_status_all status;

    if (!motor_runtime_store_status(&status)) {
        TM_SHELL_SUMMARY("store", "cmd=status result=error reason=snapshot_unavailable");
        return;
    }

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

static bool _app_shell_store_layout_total(uint8_t mask, struct param_save_layout *total_out)
{
    struct param_save_layout layout;

    if (total_out == NULL) {
        return false;
    }

    memset(total_out, 0, sizeof(*total_out));

    if ((mask & PARAM_SAVE_MOTOR) != 0U) {
        if (!motor_runtime_store_layout(PARAM_SAVE_MOTOR, &layout)) {
            return false;
        }
        total_out->payload_size += layout.payload_size;
        total_out->flash_total_size += layout.flash_total_size;
    }
    if ((mask & PARAM_SAVE_ENC) != 0U) {
        if (!motor_runtime_store_layout(PARAM_SAVE_ENC, &layout)) {
            return false;
        }
        total_out->payload_size += layout.payload_size;
        total_out->flash_total_size += layout.flash_total_size;
    }
    if ((mask & PARAM_SAVE_SYS) != 0U) {
        if (!motor_runtime_store_layout(PARAM_SAVE_SYS, &layout)) {
            return false;
        }
        total_out->payload_size += layout.payload_size;
        total_out->flash_total_size += layout.flash_total_size;
    }

    return true;
}

static void _app_shell_store_pending_row(const char *name, uint8_t mask)
{
    struct param_save_layout layout;

    if (motor_runtime_store_layout(mask, &layout)) {
        TM_SHELL_ROW("slot",
                     "name=%s status=pending data=%uB flash=%uB",
                     name,
                     (unsigned)layout.payload_size,
                     (unsigned)layout.flash_total_size);
        return;
    }

    TM_SHELL_ROW("slot", "name=%s status=pending data=- flash=-", name);
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
    struct motor_runtime_snapshot snapshot;
    struct param_status_all       status_all;
    struct param_save_layout      total_layout;
    uint8_t                       mask;
    bool                          partial;
    bool                          have_total_layout;
    int                           status;

    if ((argc == 1) || ((argc == 2) && (strcmp(argv[1], "status") == 0))) {
        _app_shell_store_status();
        return SHELL_OK;
    }

    if (!motor_runtime_snapshot(&snapshot)) {
        TM_SHELL_SUMMARY("store", "cmd=%s result=error reason=snapshot_unavailable", (argc >= 2) ? argv[1] : "status");
        return SHELL_OK;
    }

    if ((argc < 2) || (argc > 3)) {
        return SHELL_ERR_ARGC;
    }

    if (strcmp(argv[1], "erase") == 0) {
        if (_app_shell_store_requires_idle("erase", PARAM_SAVE_ALL) && (snapshot.axis_state != AXIS_STATE_IDLE)) {
            TM_SHELL_SUMMARY("store",
                             "cmd=erase result=error axis=%s",
                             app_shell_axis_state_name(snapshot.axis_state));
            return SHELL_OK;
        }

        if (!motor_runtime_request_store_erase()) {
            TM_SHELL_SUMMARY("store", "cmd=erase result=error reason=snapshot_unavailable");
            return SHELL_OK;
        }

        TM_SHELL_SUMMARY("store", "cmd=erase result=ok target=all");
        return SHELL_OK;
    }

    if (argc != 3) {
        return SHELL_ERR_ARGC;
    }

    if (_app_shell_store_resolve_target(argv[2], &mask) != 0) {
        return SHELL_ERR_ARG;
    }

    if (_app_shell_store_requires_idle(argv[1], mask) && (snapshot.axis_state != AXIS_STATE_IDLE)) {
        TM_SHELL_SUMMARY("store",
                         "cmd=%s result=error axis=%s target=%s",
                         argv[1],
                         app_shell_axis_state_name(snapshot.axis_state),
                         argv[2]);
        return SHELL_OK;
    }

    if (strcmp(argv[1], "save") == 0) {
        if (!motor_runtime_request_store_save(mask)) {
            TM_SHELL_SUMMARY("store", "cmd=save result=error reason=snapshot_unavailable target=%s", argv[2]);
            return SHELL_OK;
        }

        have_total_layout = _app_shell_store_layout_total(mask, &total_layout);
        if (have_total_layout) {
            TM_SHELL_SUMMARY("store",
                             "cmd=save result=ok target=%s mode=deferred total_data=%uB total_flash=%uB",
                             argv[2],
                             (unsigned)total_layout.payload_size,
                             (unsigned)total_layout.flash_total_size);
        } else {
            TM_SHELL_SUMMARY("store", "cmd=save result=ok target=%s mode=deferred", argv[2]);
        }

        if ((mask & PARAM_SAVE_MOTOR) != 0U) {
            _app_shell_store_pending_row("motor", PARAM_SAVE_MOTOR);
        }
        if ((mask & PARAM_SAVE_ENC) != 0U) {
            _app_shell_store_pending_row("enc", PARAM_SAVE_ENC);
        }
        if ((mask & PARAM_SAVE_SYS) != 0U) {
            _app_shell_store_pending_row("sys", PARAM_SAVE_SYS);
        }
        TM_SHELL_ROW("note", "write='queued; safe-task will commit flash later'");
        return SHELL_OK;
    }

    if (strcmp(argv[1], "load") == 0) {
        TM_SHELL_SUMMARY("store", "cmd=load result=ok target=%s", argv[2]);

        if (!motor_runtime_request_store_load(mask, &status_all, &partial)) {
            TM_SHELL_ROW("note", "result=error partial=1");
            return SHELL_OK;
        }

        if ((mask & PARAM_SAVE_MOTOR) != 0U) {
            status = status_all.motor.status;
            _app_shell_store_result_row("motor", status);
        }
        if ((mask & PARAM_SAVE_ENC) != 0U) {
            status = status_all.enc.status;
            _app_shell_store_result_row("enc", status);
        }
        if ((mask & PARAM_SAVE_SYS) != 0U) {
            status = status_all.sys.status;
            _app_shell_store_result_row("sys", status);
        }

        if (partial) {
            TM_SHELL_ROW("note", "result=error partial=1");
        }
        return SHELL_OK;
    }

    return SHELL_ERR_ARG;
}

SHELL_CMD_EXPORT(store, app_shell_store_command, 50U, "[status|save <slot>|load <slot>|erase]", "Store");

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
