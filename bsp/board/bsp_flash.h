/**
 * @file    bsp_flash.h
 * @brief   FalconFoc board-owned on-chip Flash backend contract
 * @version v1.1.0
 * @date    2026.06.02
 *
 * Design overview
 * ---------------
 * This module exposes the minimum Flash backend that FalconFoc keeps inside
 * `bsp/board`. It is intentionally a thin board-owned contract used by the
 * application-private `tiny_param` storage path and is not a generic device,
 * HAL, or reusable Flash utility layer.
 *
 * Usage example
 * -------------
 * struct tp_flash_ops flash_ops = {
 *     .read  = bsp_flash_read,
 *     .write = bsp_flash_write,
 *     .erase = bsp_flash_erase,
 * };
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - Board-visible Flash geometry remains platform-owned and is consumed here
 *   through compile definitions.
 * - `bsp_flash_read()` is a readonly memory-mapped access helper and may be
 *   used in any CPU context when the caller only needs a bounded copy.
 * - `bsp_flash_write()` and `bsp_flash_erase()` are task-context storage
 *   operations. ISR callers and pre-scheduler callers are rejected.
 *
 * Failure / recovery
 * ------------------
 * - Success returns `0`.
 * - Contract violations and backend failures return negative `-errno`.
 * - The caller owns higher-level retry, slot fallback, and recovery policy.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Constants ----------------------------------------------------------------------------- */

#ifndef THEMIS_PLATFORM_FLASH_BASE_ADDR
#error "bsp_flash.h requires THEMIS_PLATFORM_FLASH_BASE_ADDR from platform compile definitions."
#endif

#ifndef THEMIS_PLATFORM_FLASH_SIZE_BYTES
#error "bsp_flash.h requires THEMIS_PLATFORM_FLASH_SIZE_BYTES from platform compile definitions."
#endif

#ifndef THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES
#error "bsp_flash.h requires THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES from platform compile definitions."
#endif

#ifndef THEMIS_PLATFORM_FLASH_SECTOR_COUNT
#error "bsp_flash.h requires THEMIS_PLATFORM_FLASH_SECTOR_COUNT from platform compile definitions."
#endif

#define BSP_FLASH_BASE_ADDR    THEMIS_PLATFORM_FLASH_BASE_ADDR
#define BSP_FLASH_SIZE         THEMIS_PLATFORM_FLASH_SIZE_BYTES
#define BSP_FLASH_SECTOR_SIZE  THEMIS_PLATFORM_FLASH_SECTOR_SIZE_BYTES
#define BSP_FLASH_SECTOR_COUNT THEMIS_PLATFORM_FLASH_SECTOR_COUNT

/* Public Functions ----------------------------------------------------------------------------- */

/* Readonly access ------------------------------------------------------------------------------ */

/**
 * @brief  Copy one Flash window into caller memory.
 *
 * @param  addr  Flash start address.
 * @param  buf   Destination buffer.
 * @param  size  Requested byte count.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int bsp_flash_read(uint32_t addr, uint8_t *buf, uint32_t size);

/* Task-context storage operations -------------------------------------------------------------- */

/**
 * @brief  Program one aligned Flash window from caller memory.
 *
 * Contract:
 * - task context only
 * - scheduler must already be running
 * - `addr` and `size` must both be 4-byte aligned
 *
 * @param  addr  Flash start address.
 * @param  buf   Source buffer.
 * @param  size  Requested byte count.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int bsp_flash_write(uint32_t addr, const uint8_t *buf, uint32_t size);

/**
 * @brief  Erase the sectors that cover one Flash window.
 *
 * Contract:
 * - task context only
 * - scheduler must already be running
 * - hardware sector expansion is handled inside this backend
 *
 * @param  addr  Flash start address.
 * @param  size  Requested byte count.
 *
 * @return `0` on success, or negative `-errno` on failure.
 */
int bsp_flash_erase(uint32_t addr, uint32_t size);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
