/**
 * @file    tmconfig.h
 * @brief   Themis Aggregate Configuration Entry
 * @version v3.0.0
 * @date    2026.03.22
 *
 * Design overview
 * ---------------
 * This header stays as the single aggregate configuration entry for project /
 * BSP integration code, while the real ownership is split into:
 * - core config   : include/config/tm_core_config.h
 * - driver config : include/config/tm_driver_config.h
 * - board config  : bsp/bsp_config.h
 *
 * Include guidance
 * ----------------
 * - Framework headers should prefer their narrow direct dependencies instead
 *   of including this aggregate entry transitively.
 * - Product/bootstrap code that genuinely needs the combined build profile,
 *   driver profile, and board defaults may include this header as the single
 *   aggregate entry.
 *
 * Non-ownership
 * -------------
 * `tmconfig.h` does not become the canonical owner of any individual macro or
 * typedef it re-exports. Changes still belong in the narrow owner header.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "cmsis_os2.h"
#include "bsp_config.h"
#include "config/tm_core_config.h"
#include "config/tm_driver_config.h"

/* ---------------------------------------- end of file ----------------------------------------- */
