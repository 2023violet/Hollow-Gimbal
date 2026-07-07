/**
 * @file    console_session_config.h
 * @brief   Application-owned console session configuration
 * @version v1.0.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * This header bridges the shell engine's legacy `SHELL_*` macros to the
 * FalconFoc application policy. It is intentionally macro-only: no runtime
 * state, no transport ownership, and no hidden init path lives here.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "config/tm_core_config.h"
#include "tmdef.h"
#include "tm_shell.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

#define TM_SHELL_ENABLE                 (TM_FEATURE_SHELL)
#define SHELL_ENABLE                    (TM_SHELL_ENABLE)

#define SHELL_PRIO_MOTOR                (10U)
#define SHELL_PRIO_MONITOR              (20U)
#define SHELL_PRIO_CONFIG               (30U)
#define SHELL_PRIO_CALIB                (40U)
#define SHELL_PRIO_STORE                (50U)
#define SHELL_PRIO_SYSTEM               (90U)

#define SHELL_LINE_BUF_SIZE             SHELL_LINE_MAX
#define SHELL_TX_WAIT_VOFA_LOOPS        (10000U)
#define SHELL_TX_CHAR_TIMEOUT           (0xFFFFU)
#define SHELL_TX_TDC_TIMEOUT            (0xFFFFFU)
#define SHELL_UART_INTERACTIVE_QUIET_MS (1500U)

#define SHELL_FW_VERSION                TM_APP_FW_VERSION_STRING
#define SHELL_FW_DATE                   __DATE__ " " __TIME__

#define SHELL_RX_BUF_SIZE               (256U)
#define SHELL_TX_RING_SIZE              (768U)
#define FLAG_SHELL_RX                   TM_BIT(0U)
#define FLAG_SHELL_TX                   TM_BIT(1U)

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
