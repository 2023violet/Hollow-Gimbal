/**
 * @file    foc_at32_store.c
 * @brief   AT32F415 internal Flash driver for the Board1 persistent settings slots.
 * @version v0.1.0
 * @date    2026.11.22
 *
 * Implementation role
 * -------------------
 * Linked only by FALCONFOC_BOARD_PERSISTENCE=ON images. Slots A and B are the
 * two 2 KiB regions at the top of the 128 KiB Flash; the linker refuses an
 * image that reaches them. Both 1 KiB and 2 KiB sector geometries keep each
 * slot's first erase unit inside its own region. The sector size, the erase
 * stall of instruction fetch while H0/H1 IRQs are enabled, and endurance are
 * target facts still to be confirmed on hardware.
 */

/* Includes --------------------------------------------------------------------- */

#include "at32f415.h"
#include "foc_board_store.h"

#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define FOC_AT32_STORE_BASE UINT32_C(0x0801F000)

/* Private Helpers -------------------------------------------------------------- */

static uint32_t foc_at32_store_address(unsigned int slot, size_t offset)
{
    return FOC_AT32_STORE_BASE + (uint32_t)slot * FOC_BOARD_STORE_SLOT_BYTES + (uint32_t)offset;
}

static bool foc_at32_store_range(unsigned int slot, size_t offset, size_t size)
{
    return slot <= 1U && offset <= FOC_BOARD_STORE_SLOT_BYTES &&
        size <= FOC_BOARD_STORE_SLOT_BYTES - offset;
}

static int
foc_at32_store_read(void *context, unsigned int slot, size_t offset, uint8_t *bytes, size_t size)
{
    (void)context;
    if (!foc_at32_store_range(slot, offset, size)) {
        return -1;
    }
    memcpy(bytes, (const void *)foc_at32_store_address(slot, offset), size);
    return 0;
}

static int foc_at32_store_erase(void *context, unsigned int slot)
{
    (void)context;
    if (slot > 1U) {
        return -1;
    }
    flash_unlock();
    const flash_status_type status = flash_sector_erase(foc_at32_store_address(slot, 0U));
    flash_lock();
    return status == FLASH_OPERATE_DONE ? 0 : -1;
}

static int foc_at32_store_program(
    void *context,
    unsigned int slot,
    size_t offset,
    const uint8_t *bytes,
    size_t size
)
{
    (void)context;
    if (!foc_at32_store_range(slot, offset, size) || offset % 4U != 0U || size % 4U != 0U) {
        return -1;
    }
    int result = 0;
    flash_unlock();
    for (size_t index = 0U; index < size && result == 0; index += 4U) {
        const uint32_t word = (uint32_t)bytes[index] | ((uint32_t)bytes[index + 1U] << 8) |
            ((uint32_t)bytes[index + 2U] << 16) | ((uint32_t)bytes[index + 3U] << 24);
        if (flash_word_program(foc_at32_store_address(slot, offset + index), word) !=
            FLASH_OPERATE_DONE) {
            result = -1;
        }
    }
    flash_lock();
    return result;
}

/* Public Functions ------------------------------------------------------------- */

struct foc_board_store_driver foc_at32_store_driver(void)
{
    return (struct foc_board_store_driver){
        .context = NULL,
        .read = foc_at32_store_read,
        .erase = foc_at32_store_erase,
        .program = foc_at32_store_program,
    };
}

/* ---------------------------------------- end of file ----------------------------------------- */
