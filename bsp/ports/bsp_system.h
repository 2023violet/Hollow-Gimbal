/**
 * @file    bsp_system.h
 * @brief   FalconFoc thin system-control BSP port contract
 * @version v1.1.0
 * @date    2026.06.02
 *
 * Design overview
 * ---------------
 * This port exposes only the minimum board-level reset action that FalconFoc
 * control-plane code is allowed to request. It is intentionally a thin BSP
 * port and not a generic power-management or platform-control layer.
 *
 * Usage example
 * -------------
 * if (bsp_system_reset() != 0) {
 *     // higher-level policy stays in applications/
 * }
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Platform reset truth remains southbound in `bsp_system.c`.
 * - This port owns no dynamic state and requires no explicit init.
 * - Calls are allowed from thread mode before or after the scheduler starts.
 * - ISR callers are rejected so reboot policy does not execute from interrupt
 *   paths by accident.
 *
 * Failure / recovery
 * ------------------
 * - Success returns `0`.
 * - Contract violations return negative `-errno`.
 * - Higher-level retry, operator feedback, and product policy remain owned by
 *   `applications/`.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Request an immediate MCU reset from thread mode.
 *
 * Contract:
 * - thread mode only
 * - allowed before or after scheduler start
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int bsp_system_reset(void);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
