/**
 * @file    bsp_flash.h
 * @brief   Falcon FOC board-owned on-chip Flash access contract
 * @version v1.0.0
 * @date    2026.05.13
 *
 * Design overview
 * ---------------
 * This header exposes the minimum Flash service surface that FalconFoc keeps
 * inside the BSP layer. It is intentionally a thin board-owned contract for
 * the application-private `tiny_param` backend and is not a generic device or
 * driver abstraction.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>
#include "at32f415_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Constants ----------------------------------------------------------------------------- */

#define BSP_FLASH_BASE_ADDR    0x08000000U    /* Main Flash start address */
#define BSP_FLASH_SIZE         (128U * 1024U) /* 128 KB */
#define BSP_FLASH_SECTOR_SIZE  (1U * 1024U)   /* 2 KB per sector */
#define BSP_FLASH_SECTOR_COUNT (128U)         /* Total sectors */

/* Public Error Codes --------------------------------------------------------------------------- */

#define BSP_FLASH_OK           (0)
#define BSP_FLASH_ERR_RANGE    (-1)
#define BSP_FLASH_ERR_ALIGN    (-2)
#define BSP_FLASH_ERR_OP       (-3)

/* Public Functions ----------------------------------------------------------------------------- */

int      bsp_flash_read(uint32_t addr, uint8_t *buf, uint32_t size);
int      bsp_flash_write(uint32_t addr, const uint8_t *buf, uint32_t size);
int      bsp_flash_erase(uint32_t addr, uint32_t size);
uint32_t bsp_flash_get_sector(uint32_t addr);
bool     bsp_flash_is_empty(uint32_t addr, uint32_t size);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
