/**
 * @file    app_shell_task.h
 * @brief   Application-owned interactive shell session task
 * @version v1.0.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * `app_shell_task` is the product-owned session owner for the interactive
 * shell. It decouples transport-side line ingestion from synchronous
 * `tm_shell` command execution and keeps one bounded static queue so command
 * pressure stays explicit under tight RAM budgets.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "shell_config.h"

#if TM_SHELL_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*tm_shell_task_banner_fn_t)(void);

struct tm_shell_task_diag_snapshot {
    int      status;
    bool     initialized;
    bool     transport_ready;
    bool     ready;
    uint16_t queue_depth;
    uint16_t queue_used;
    uint16_t queue_high_water;
    uint32_t submit_ok_count;
    uint32_t submit_fail_count;
};

int  tm_shell_task_init(void);
int  tm_shell_task_submit_line(const char *line, uint16_t len);
bool tm_shell_task_diag_snapshot(struct tm_shell_task_diag_snapshot *snapshot);
void tm_shell_task_set_banner_hook(tm_shell_task_banner_fn_t fn);
void tm_shell_task_notify_transport_ready(void);

#if defined(THEMIS_HOST_TEST)
int tm_shell_task_host_process_once(void);
#endif

#ifdef __cplusplus
}
#endif

#endif

/* ---------------------------------------- end of file ----------------------------------------- */
