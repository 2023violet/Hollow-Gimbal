/**
 * @file    bsp_flash.h
 * @brief   Host shim for board flash backend constants and hooks
 * @version v1.0.0
 * @date    2026.05.14
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_FLASH_BASE_ADDR    0x08000000U
#define BSP_FLASH_SIZE         (1024U * 1024U)
#define BSP_FLASH_SECTOR_SIZE  (2U * 1024U)
#define BSP_FLASH_SECTOR_COUNT 512U

int  bsp_flash_read(uint32_t addr, uint8_t *buf, uint32_t size);
int  bsp_flash_write(uint32_t addr, const uint8_t *buf, uint32_t size);
int  bsp_flash_erase(uint32_t addr, uint32_t size);
bool bsp_flash_is_empty(uint32_t addr, uint32_t size);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
