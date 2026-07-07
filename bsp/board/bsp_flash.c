/**
 * @file    bsp_flash.c
 * @brief   FalconFoc board-owned on-chip Flash backend
 * @version v1.1.0
 * @date    2026.06.02
 *
 * Implementation role
 * -------------------
 * This file owns the southbound AT32 on-chip Flash read/write/erase path that
 * backs FalconFoc persistence. It keeps vendor Flash operations local to
 * `bsp/board` while exposing only the small northbound contract declared in
 * `bsp_flash.h`.
 *
 * Key collaborators
 * -----------------
 * - `bsp_flash.h`: public board-owned Flash contract
 * - `tm_runtime.h`: shared task/ISR context and critical-section helpers
 * - vendor Flash driver header selected by platform compile definitions
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "bsp_flash.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include "tm_runtime.h"

#ifndef THEMIS_PLATFORM_FLASH_DRIVER_HEADER
#error "bsp_flash.c requires THEMIS_PLATFORM_FLASH_DRIVER_HEADER from platform compile definitions."
#endif

#include THEMIS_PLATFORM_FLASH_DRIVER_HEADER

#ifdef __cplusplus
extern "C" {
#endif

/* Private Function Prototypes ------------------------------------------------------------------ */

static bool     _bsp_flash_span_is_valid(uint32_t addr, uint32_t size, uint32_t *end_addr_exclusive);
static bool     _bsp_flash_is_word_aligned(uint32_t addr, uint32_t size);
static bool     _bsp_flash_mutation_context_is_valid(void);
static int      _bsp_flash_sector_window(uint32_t addr,
                                         uint32_t size,
                                         uint32_t *start_sector_addr,
                                         uint32_t *end_sector_addr);
static int      _bsp_flash_status_to_errno(flash_status_type status);
static uint32_t _bsp_flash_u32_from_le_bytes(const uint8_t *src);

/* Public Functions ----------------------------------------------------------------------------- */

int bsp_flash_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
    if ((buf == NULL) || !_bsp_flash_span_is_valid(addr, size, NULL)) {
        return -EINVAL;
    }

    memcpy(buf, (const void *)(uintptr_t)addr, size);
    return 0;
}

int bsp_flash_write(uint32_t addr, const uint8_t *buf, uint32_t size)
{
    uint32_t critical_level = 0U;
    bool     flash_unlocked = false;
    int      rc             = 0;

    if ((buf == NULL) || !_bsp_flash_span_is_valid(addr, size, NULL)) {
        return -EINVAL;
    }
    if (!_bsp_flash_mutation_context_is_valid()) {
        return -EPERM;
    }
    if (!_bsp_flash_is_word_aligned(addr, size)) {
        return -EINVAL;
    }

    critical_level = tm_irq_critical_enter();
    flash_unlock();
    flash_unlocked = true;

    for (uint32_t offset = 0U; offset < size; offset += 4U) {
        flash_status_type status;
        uint32_t          word;

        word   = _bsp_flash_u32_from_le_bytes(&buf[offset]);
        status = flash_word_program(addr + offset, word);
        rc     = _bsp_flash_status_to_errno(status);
        if (rc != 0) {
            goto exit;
        }
    }

exit:
    if (flash_unlocked) {
        flash_lock();
    }
    tm_irq_critical_exit(critical_level);
    return rc;
}

int bsp_flash_erase(uint32_t addr, uint32_t size)
{
    uint32_t start_sector_addr;
    uint32_t end_sector_addr;
    uint32_t critical_level = 0U;
    uint32_t sector_addr;
    bool     flash_unlocked = false;
    int      rc             = 0;

    rc = _bsp_flash_sector_window(addr, size, &start_sector_addr, &end_sector_addr);
    if (rc != 0) {
        return rc;
    }
    if (!_bsp_flash_mutation_context_is_valid()) {
        return -EPERM;
    }

    critical_level = tm_irq_critical_enter();
    flash_unlock();
    flash_unlocked = true;

    sector_addr = start_sector_addr;
    while (sector_addr <= end_sector_addr) {
        flash_status_type status;

        status = flash_sector_erase(sector_addr);
        rc     = _bsp_flash_status_to_errno(status);
        if (rc != 0) {
            goto exit;
        }

        sector_addr += BSP_FLASH_SECTOR_SIZE;
    }

exit:
    if (flash_unlocked) {
        flash_lock();
    }
    tm_irq_critical_exit(critical_level);
    return rc;
}

/* Private Functions ---------------------------------------------------------------------------- */

static bool _bsp_flash_span_is_valid(uint32_t addr, uint32_t size, uint32_t *end_addr_exclusive)
{
    uint32_t flash_limit;
    uint32_t end_addr;

    if (size == 0U) {
        return false;
    }

    flash_limit = BSP_FLASH_BASE_ADDR + BSP_FLASH_SIZE;
    if ((flash_limit < BSP_FLASH_BASE_ADDR) || (addr < BSP_FLASH_BASE_ADDR) || (addr >= flash_limit)) {
        return false;
    }

    end_addr = addr + size;
    if ((end_addr < addr) || (end_addr > flash_limit)) {
        return false;
    }

    if (end_addr_exclusive != NULL) {
        *end_addr_exclusive = end_addr;
    }

    return true;
}

static bool _bsp_flash_is_word_aligned(uint32_t addr, uint32_t size)
{
    return (((addr & 0x3U) == 0U) && ((size & 0x3U) == 0U));
}

static bool _bsp_flash_mutation_context_is_valid(void)
{
    return (!tm_is_in_isr() && tm_scheduler_is_running());
}

static int _bsp_flash_sector_window(uint32_t addr,
                                    uint32_t size,
                                    uint32_t *start_sector_addr,
                                    uint32_t *end_sector_addr)
{
    uint32_t end_addr_exclusive;
    uint32_t start_offset;
    uint32_t end_offset;

    if ((start_sector_addr == NULL) || (end_sector_addr == NULL)) {
        return -EINVAL;
    }
    if (!_bsp_flash_span_is_valid(addr, size, &end_addr_exclusive)) {
        return -EINVAL;
    }

    start_offset = addr - BSP_FLASH_BASE_ADDR;
    end_offset   = (end_addr_exclusive - 1U) - BSP_FLASH_BASE_ADDR;

    *start_sector_addr =
        BSP_FLASH_BASE_ADDR + ((start_offset / BSP_FLASH_SECTOR_SIZE) * BSP_FLASH_SECTOR_SIZE);
    *end_sector_addr =
        BSP_FLASH_BASE_ADDR + ((end_offset / BSP_FLASH_SECTOR_SIZE) * BSP_FLASH_SECTOR_SIZE);

    return 0;
}

static int _bsp_flash_status_to_errno(flash_status_type status)
{
    switch (status) {
        case FLASH_OPERATE_DONE:
            return 0;

        case FLASH_OPERATE_TIMEOUT:
            return -ETIMEDOUT;

        case FLASH_EPP_ERROR:
            return -EACCES;

        case FLASH_PROGRAM_ERROR:
            return -EIO;

        case FLASH_OPERATE_BUSY:
        default:
            return -EBUSY;
    }
}

static uint32_t _bsp_flash_u32_from_le_bytes(const uint8_t *src)
{
    return (((uint32_t)src[0]) | ((uint32_t)src[1] << 8U) | ((uint32_t)src[2] << 16U) |
            ((uint32_t)src[3] << 24U));
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
