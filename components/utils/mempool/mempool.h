/**
 * @file    mempool.h
 * @brief   Themis caller-owned fixed-size memory pool
 * @version v1.1.0
 * @date    2026.04.04
 *
 * Design overview
 * ---------------
 * `mempool` is a caller-owned fixed-size allocator built on an in-place free
 * list. The pool stores no external metadata beyond its control block; each
 * idle block carries the next free pointer in its first machine word.
 *
 * Supports arbitrary block sizes (will be aligned up to pointer size).
 * Division is used internally for universal compatibility.
 *
 * Usage example
 * -------------
 * MEMPOOL_DEFINE(can_rx_pool, sizeof(struct can_frame), 32);
 *
 * mempool_init(&can_rx_pool, _can_rx_pool_buf, sizeof(_can_rx_pool_buf),
 *              sizeof(struct can_frame));
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - The caller owns both `struct mempool` and the backing raw buffer.
 * - The buffer must remain naturally aligned to `sizeof(void *)`.
 * - One execution context must own alloc/free unless callers serialize access.
 * - @note Not thread-safe. Caller must ensure external serialization. Not
 *   ISR-safe.
 *
 * Blocking / allocation
 * ---------------------
 * - No dynamic allocation and no OS dependency.
 * - Allocation and free are O(1) list-head operations.
 *
 * Failure / recovery
 * ------------------
 * - Invalid init contracts fail with `-EINVAL` or `-ENOMEM`.
 * - `mempool_alloc()` returns `NULL` when exhausted.
 * - `mempool_free()` returns `-EINVAL` on obvious misuse instead of silently
 *   accepting an invalid block.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief  Align one size up to the nearest multiple of `align`.
 *
 * @param[in] x      Source size.
 * @param[in] align  Required alignment value.
 */
#define MEMPOOL_ALIGN_UP(x, align) (((x) + (align) - 1U) & ~((align) - 1U))

/**
 * @brief  Declare one static memory pool and its backing buffer.
 *
 * The pool still requires `mempool_init()` before first use; this macro only
 * allocates the static storage.
 *
 * @param[in] name       C identifier for the `struct mempool` variable.
 * @param[in] block_sz   Requested block size in bytes.
 * @param[in] n_blocks   Number of blocks to preallocate.
 *
 * @note   The runtime block stride is
 *         `MEMPOOL_ALIGN_UP(block_sz, sizeof(void *))`. The backing array is
 *         sized conservatively using that aligned value.
 */
#define MEMPOOL_DEFINE(name, block_sz, n_blocks)                                                     \
    static uint8_t        _##name##_buf[MEMPOOL_ALIGN_UP((block_sz), sizeof(void *)) * (n_blocks)]; \
    static struct mempool name = {NULL, 0U, 0U, 0U, NULL, NULL}

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief  Memory pool control block.
 *
 * @note   All fields are managed exclusively by the `mempool_*()` API.
 *
 * Internal free-list model:
 * - Each free block stores the next free-block pointer in its first
 *   `sizeof(void *)` bytes.
 * - Allocation pops the list head.
 * - Free pushes the returned block back to the list head.
 * - Both operations are O(1).
 */
struct mempool {
    /** Head of the free-block linked list. */
    void    *free_list;
    /** Per-block stride after alignment, in bytes. */
    size_t   block_size;
    /** Total number of blocks carved from the backing buffer. */
    size_t   total_blocks;
    /** Number of blocks currently available. */
    size_t   free_blocks;
    /** Inclusive lower bound of the managed buffer. */
    uint8_t *buffer_start;
    /** Exclusive upper bound of the managed buffer. */
    uint8_t *buffer_end;
};

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Initialize one memory pool over a caller-supplied raw buffer.
 *
 * The function validates arguments, aligns the requested block size,
 * calculates how many complete blocks fit in the buffer, and builds the
 * initial free list in place.
 *
 * @param[out] pool         Pool control block to initialize.
 * @param[in]  buffer       Raw backing buffer managed by the pool.
 * @param[in]  buffer_size  Size of `buffer` in bytes.
 * @param[in]  block_size   Requested allocation size in bytes.
 *
 * @retval 0          Initialization succeeded.
 * @retval -EINVAL    Invalid pointer, zero block size, or bad buffer
 *                    alignment.
 * @retval -ENOMEM    Buffer is too small to hold even one aligned block.
 *
 * @note   Supports arbitrary block sizes (will be aligned up to pointer
 *         size). Division is used internally for universal compatibility.
 */
int mempool_init(struct mempool *pool, void *buffer, size_t buffer_size, size_t block_size);

/**
 * @brief  Allocate one block from the pool.
 *
 * @param[in,out] pool  Initialized pool control block.
 *
 * @return Pointer to one block on success, or `NULL` when the pool is
 *         exhausted or `pool` is `NULL`.
 */
void *mempool_alloc(struct mempool *pool);

/**
 * @brief  Return one previously allocated block back to the pool.
 *
 * @warning Double-free remains undefined behavior. Obvious misuse such as
 *          out-of-range or misaligned pointers is rejected with `-EINVAL`.
 *
 * @param[in,out] pool   Initialized pool control block.
 * @param[in]     block  Pointer previously returned by `mempool_alloc()` on
 *                       the same pool.
 *
 * @retval 0        Free succeeded.
 * @retval -EINVAL  @p pool is NULL, @p block is NULL, the pointer does not
 *                  belong to the pool, or the pool is already fully free.
 */
int mempool_free(struct mempool *pool, void *block);

/**
 * @brief  Query how many blocks are currently free.
 *
 * @param[in] pool  Initialized pool control block.
 *
 * @return Number of free blocks, or `0` when `pool` is `NULL`.
 */
size_t mempool_get_free_count(const struct mempool *pool);

/**
 * @brief  Query the total block capacity of the pool.
 *
 * @param[in] pool  Initialized pool control block.
 *
 * @return Total number of blocks, or `0` when `pool` is `NULL`.
 */
size_t mempool_get_total_count(const struct mempool *pool);

/**
 * @brief  Check whether the pool is completely exhausted.
 *
 * @param[in] pool  Pool control block.
 *
 * @return `true` when no free blocks remain or when `pool` is `NULL`;
 *         otherwise `false`.
 */
static inline bool mempool_is_empty(const struct mempool *pool)
{
    if (pool == NULL) {
        return true;
    }

    return pool->free_blocks == 0U;
}

/**
 * @brief  Check whether every block is currently free.
 *
 * @param[in] pool  Pool control block.
 *
 * @return `true` when the pool is at full capacity; otherwise `false`.
 */
static inline bool mempool_is_full(const struct mempool *pool)
{
    if (pool == NULL) {
        return false;
    }

    return pool->free_blocks == pool->total_blocks;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
