/**
 * @file    bsp_flash.c
 * @brief   Falcon FOC board-owned on-chip Flash access
 * @version v1.0.0
 * @date    2026.05.13
 *
 * Implementation role
 * -------------------
 * This file keeps the AT32 on-chip Flash read/write/erase path inside the BSP
 * layer so FalconFoc can reuse it from the application-owned `tiny_param`
 * backend without reintroducing the full Themis device/driver stack.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "bsp_flash.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Functions ----------------------------------------------------------------------------- */

int bsp_flash_read(uint32_t addr, uint8_t *buf, uint32_t size)
{
    if ((addr < BSP_FLASH_BASE_ADDR) || (addr >= (BSP_FLASH_BASE_ADDR + BSP_FLASH_SIZE)) ||
        (buf == NULL) || (size == 0U)) {
        return BSP_FLASH_ERR_RANGE;
    }

    memcpy(buf, (const uint8_t *)addr, size);
    return (int)size;
}

int bsp_flash_write(uint32_t addr, const uint8_t *buf, uint32_t size)
{
    uint32_t          words;
    const uint32_t   *src;
    uint32_t         *dst;
    flash_status_type status = FLASH_OPERATE_DONE;

    if ((addr < BSP_FLASH_BASE_ADDR) || (addr >= (BSP_FLASH_BASE_ADDR + BSP_FLASH_SIZE)) ||
        (buf == NULL) || (size == 0U)) {
        return BSP_FLASH_ERR_RANGE;
    }

    if (((addr % 4U) != 0U) || ((size % 4U) != 0U)) {
        return BSP_FLASH_ERR_ALIGN;
    }

    __disable_irq();
    flash_unlock();

    words = size / 4U;
    src   = (const uint32_t *)buf;
    dst   = (uint32_t *)addr;

    for (uint32_t i = 0U; i < words; i++) {
        status = flash_word_program((uint32_t)dst, *src++);
        if (status != FLASH_OPERATE_DONE) {
            flash_lock();
            __enable_irq();
            return BSP_FLASH_ERR_OP;
        }
        dst++;
    }

    flash_lock();
    __enable_irq();

    return (int)size;
}

int bsp_flash_erase(uint32_t addr, uint32_t size)
{
    uint32_t          start_sector_addr;
    uint32_t          end_sector_addr;
    uint32_t          erased_bytes = 0U;
    uint32_t          sector_addr;
    flash_status_type status;

    if ((addr < BSP_FLASH_BASE_ADDR) || (addr >= (BSP_FLASH_BASE_ADDR + BSP_FLASH_SIZE)) ||
        (size == 0U)) {
        return BSP_FLASH_ERR_RANGE;
    }

    start_sector_addr = (addr / BSP_FLASH_SECTOR_SIZE) * BSP_FLASH_SECTOR_SIZE;
    end_sector_addr = ((addr + size - 1U) / BSP_FLASH_SECTOR_SIZE) * BSP_FLASH_SECTOR_SIZE;

    __disable_irq();
    flash_unlock();

    sector_addr = start_sector_addr;
    while (sector_addr <= end_sector_addr) {
        status = flash_sector_erase(sector_addr);
        if (status != FLASH_OPERATE_DONE) {
            flash_lock();
            __enable_irq();
            return BSP_FLASH_ERR_OP;
        }

        erased_bytes += BSP_FLASH_SECTOR_SIZE;
        sector_addr += BSP_FLASH_SECTOR_SIZE;
    }

    flash_lock();
    __enable_irq();

    return (int)erased_bytes;
}

uint32_t bsp_flash_get_sector(uint32_t addr)
{
    if ((addr < BSP_FLASH_BASE_ADDR) || (addr >= (BSP_FLASH_BASE_ADDR + BSP_FLASH_SIZE))) {
        return 0xFFFFFFFFU;
    }

    return (addr - BSP_FLASH_BASE_ADDR) / BSP_FLASH_SECTOR_SIZE;
}

bool bsp_flash_is_empty(uint32_t addr, uint32_t size)
{
    uint32_t remaining_size;
    uint32_t current_addr;
    uint32_t addr_offset;

    if (size == 0U) {
        return false;
    }

    if ((addr < BSP_FLASH_BASE_ADDR) || ((addr + size) > (BSP_FLASH_BASE_ADDR + BSP_FLASH_SIZE))) {
        return false;
    }

    remaining_size = size;
    current_addr   = addr;
    addr_offset    = current_addr % 4U;

    if (addr_offset != 0U) {
        const uint32_t *first_word_ptr = (const uint32_t *)(current_addr - addr_offset);
        uint32_t        first_word     = *first_word_ptr;

        for (; (addr_offset < 4U) && (remaining_size > 0U);
             addr_offset++, current_addr++, remaining_size--) {
            uint8_t byte_val = (uint8_t)((first_word >> (addr_offset * 8U)) & 0xFFU);
            if (byte_val != 0xFFU) {
                return false;
            }
        }
    }

    {
        const uint32_t *word_ptr   = (const uint32_t *)current_addr;
        uint32_t        word_count = remaining_size / 4U;

        for (uint32_t i = 0U; i < word_count; i++) {
            if (word_ptr[i] != 0xFFFFFFFFU) {
                return false;
            }
        }

        current_addr += word_count * 4U;
        remaining_size %= 4U;
    }

    {
        const uint8_t *byte_ptr = (const uint8_t *)current_addr;
        for (uint32_t i = 0U; i < remaining_size; i++) {
            if (byte_ptr[i] != 0xFFU) {
                return false;
            }
        }
    }

    return true;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
