/**
 * @file    shell_config.h
 * @brief   Legacy shell config compatibility wrapper
 * @version v2.0.0
 * @date    2026.05.12
 *
 * Design overview
 * ---------------
 * FalconFoc now owns shell policy under
 * `applications/config/console_session_config.h`. This wrapper stays only to
 * keep generated/platform entry points and older includes building while the
 * codebase finishes converging on the application-owned config path.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "config/console_session_config.h"

/* ---------------------------------------- end of file ----------------------------------------- */
