/**
 * @file    tm_common.h
 * @brief   Legacy compatibility shim for historical Themis common include users
 * @version v1.0.0
 * @date    2026.05.11
 *
 * Design overview
 * ---------------
 * Older Falcon FOC modules still include `tm_common.h` as an aggregate runtime
 * header. The current Themis ownership has already split that surface into:
 *
 * - `tmdef.h` for low-level macros and core types
 * - `tmconfig.h` for aggregate build / board configuration
 * - `tm_runtime.h` for tick / heap / critical-section APIs
 *
 * This shim exists only to keep the in-flight refactor buildable while the
 * remaining includes are migrated to their narrower owners.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "tmconfig.h"
#include "tmdef.h"
#include "tm_runtime.h"

/* ---------------------------------------- end of file ----------------------------------------- */
