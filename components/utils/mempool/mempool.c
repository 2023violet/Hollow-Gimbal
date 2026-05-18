/**
 * @file    mempool.c
 * @brief   Caller-owned fixed-size memory pool implementation
 * @version v1.1.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `mempool.h`
 * - owns in-place free-list wiring and defensive block-range checks
 * - keeps alloc and free on constant-time head operations once initialized
 *
 * Key collaborators
 * -----------------
 * - `mempool.h`: caller-facing storage and alignment contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "mempool.h"
#include <errno.h>

/* Private Macros ------------------------------------------------------------------------------- */

/**
 * @brief  Access the embedded next-pointer stored in the first word of a free block.
 */
#define _MP_NEXT(ptr) (*(void **)(ptr))

/* Private Helpers ------------------------------------------------------------------------------ */

static bool _mempool_block_belongs(const struct mempool *pool, const void *block)
{
    uintptr_t start;
    uintptr_t end;
    uintptr_t addr;
    size_t    offset;

    if ((pool == NULL) || (block == NULL) || (pool->buffer_start == NULL) ||
        (pool->buffer_end == NULL) || (pool->block_size == 0U)) {
        return false;
    }

    start = (uintptr_t)pool->buffer_start;
    end   = (uintptr_t)pool->buffer_end;
    addr  = (uintptr_t)block;

    if ((addr < start) || (addr >= end)) {
        return false;
    }

    offset = (size_t)(addr - start);

    return (offset % pool->block_size) == 0U;
}

/* Public Functions ----------------------------------------------------------------------------- */

int mempool_init(struct mempool *pool, void *buffer, size_t buffer_size, size_t block_size)
{
    size_t   aligned_size;
    size_t   n_blocks;
    uint8_t *base;

    if ((pool == NULL) || (buffer == NULL) || (block_size == 0U)) {
        return -EINVAL;
    }
    if ((((uintptr_t)buffer) & (sizeof(void *) - 1U)) != 0U) {
        return -EINVAL;
    }

    /*
     * Every free block stores its next-pointer in the first machine word, so
     * block size must be rounded up to pointer alignment.
     *
     * Example on a 32-bit target:
     * - block_size = 3 -> aligned_size = 4
     * - block_size = 5 -> aligned_size = 8
     * - block_size = 8 -> aligned_size = 8
     */
    aligned_size = MEMPOOL_ALIGN_UP(block_size, sizeof(void *));
    n_blocks = buffer_size / aligned_size;

    if (n_blocks == 0U) {
        return -ENOMEM;
    }

    /*
     * Build the initial free list in-place. Each free block stores the address
     * of the next free block in its first machine word.
     */
    base = (uint8_t *)buffer;

    for (size_t i = 0U; i < (n_blocks - 1U); i++) {
        void *current_block = (void *)(base + i * aligned_size);
        void *next_block    = (void *)(base + (i + 1U) * aligned_size);

        _MP_NEXT(current_block) = next_block;
    }

    _MP_NEXT((void *)(base + (n_blocks - 1U) * aligned_size)) = NULL;

    pool->free_list    = (void *)base;
    pool->block_size   = aligned_size;
    pool->total_blocks = n_blocks;
    pool->free_blocks  = n_blocks;
    pool->buffer_start = base;
    pool->buffer_end   = base + (n_blocks * aligned_size);

    return 0;
}

void *mempool_alloc(struct mempool *pool)
{
    void *block;

    if ((pool == NULL) || (pool->free_list == NULL)) {
        return NULL;
    }

    block           = pool->free_list;
    pool->free_list = _MP_NEXT(block);
    pool->free_blocks--;

    /*
     * The block is intentionally not zeroed here. Keeping allocation as a
     * pure O(1) pop lets the caller decide whether zero-fill cost is needed.
     */
    return block;
}

int mempool_free(struct mempool *pool, void *block)
{
    if ((pool == NULL) || (block == NULL)) {
        return -EINVAL;
    }
    if (!_mempool_block_belongs(pool, block)) {
        return -EINVAL;
    }
    if (pool->free_blocks >= pool->total_blocks) {
        return -EINVAL;
    }

    /* Classic push-front onto the free list. */
    _MP_NEXT(block) = pool->free_list;
    pool->free_list = block;
    pool->free_blocks++;
    return 0;
}

size_t mempool_get_free_count(const struct mempool *pool)
{
    if (pool == NULL) {
        return 0U;
    }

    return pool->free_blocks;
}

size_t mempool_get_total_count(const struct mempool *pool)
{
    if (pool == NULL) {
        return 0U;
    }

    return pool->total_blocks;
}

/* ---------------------------------------- end of file ----------------------------------------- */
