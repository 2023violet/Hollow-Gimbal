/**
 * @file    shell_task.h
 * @brief   Shell UART transport task
 * @version v1.0.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * This module owns only the shell transport path:
 * - USART1 DMA RX buffering
 * - bounded async TX staging
 * - line submission into `app_shell_task`
 *
 * Command vocabulary, banner policy, help text, and synchronous shell
 * execution are owned by `applications/shell/` and `app_shell_task`.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>
#include "shell_config.h"

#if TM_SHELL_ENABLE

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize the shell UART transport owner.
 *
 * Called explicitly from `motor_task_init()` during APP export bring-up.
 * This binds the DMA RX ring, installs the shell print backend, brings up the
 * application-owned shell session task, and starts the transport thread.
 */
void shell_task_init(void);

#ifdef __cplusplus
}
#endif

#endif /* TM_SHELL_ENABLE */

/* ---------------------------------------- end of file ----------------------------------------- */
