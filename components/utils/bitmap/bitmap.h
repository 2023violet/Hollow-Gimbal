/**
 * @file    bitmap.h
 * @brief   Caller-owned fixed-size bitmap helpers
 * @version v1.0.0
 * @date    2026.04.04
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 *
 * ============================================================================
 * Design philosophy: caller-owned storage, explicit bit-count contract
 * ============================================================================
 *
 * This component provides a minimal bitmap abstraction for modules that need a
 * compact set of flags or allocation marks without introducing heap usage or
 * hidden ownership.  The caller always provides the backing storage and the
 * logical bit count.
 *
 * Consequences:
 *   - No dynamic memory allocation.
 *   - No RTOS, BSP, or board dependency.
 *   - No built-in thread safety; callers must serialize access if multiple
 *     writers or readers race on the same bitmap.
 *   - Range helpers optimize whole-word spans internally, but still preserve
 *     the same logical contract over [0, bit_count) without exposing padding
 *     bits above the caller-visible tail.
 *
 * The bitmap may occupy more physical bits than the logical @p bit_count due
 * to word rounding.  Helpers mask the unused tail bits so callers can reason
 * purely in terms of the logical bit range [0, bit_count).
 *
 * ============================================================================
 * Storage and ownership contract
 * ============================================================================
 *
 * Storage is a plain caller-owned array of bitmap_word_t.  It may live in:
 *   - stack storage for short-lived scratch use;
 *   - static storage for global resource maps;
 *   - embedded members inside another caller-owned structure.
 *
 * The library never retains extra ownership beyond the call itself.
 * - @note Not thread-safe. Caller must ensure external serialization. Not
 *   ISR-safe.
 *
 * ============================================================================
 * Minimal usage example
 * ============================================================================
 *
 * @code
 * BITMAP_DEFINE(slot_map, 96U) = {0U};
 * size_t free_slot = 0U;
 *
 * (void)bitmap_set(slot_map, 96U, 3U);
 * if (bitmap_find_first_zero(slot_map, 96U, &free_slot)) {
 *     (void)bitmap_set(slot_map, 96U, free_slot);
 * }
 * @endcode
 *
 * Failure / recovery
 * ------------------
 * - Invalid arguments fail with `-EINVAL` where the API returns a status code.
 * - Out-of-range bit indices fail with `-ERANGE`.
 * - Snapshot helpers return conservative `0`/`false` results on invalid input.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "bitops.h"
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief  Physical storage word used by bitmap arrays.
 */
typedef bitops_word_t bitmap_word_t;

/* Public macros -------------------------------------------------------------------------------- */

/**
 * @brief  Number of logical bits stored in one bitmap_word_t.
 */
#define BITMAP_WORD_BITS          (BITOPS_WORD_BITS)

/**
 * @brief  Number of words required to store @p bits logical bits.
 */
#define BITMAP_WORD_COUNT(bits)   (((size_t)(bits) + BITMAP_WORD_BITS - 1U) / BITMAP_WORD_BITS)

/**
 * @brief  Declare a caller-owned bitmap array with storage sized for @p bits.
 *
 * This macro only allocates storage.  Callers still choose initial content,
 * usually `{0U}` for an empty bitmap.
 */
#define BITMAP_DEFINE(name, bits) bitmap_word_t name[BITMAP_WORD_COUNT(bits)]

/* Public function declarations ----------------------------------------------------------------- */

/**
 * @brief  Clear every logical bit in the bitmap.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 *
 * @retval 0        Clear succeeded.
 * @retval -EINVAL  @p bitmap is NULL or @p bit_count is zero.
 */
int bitmap_clear_all(bitmap_word_t *bitmap, size_t bit_count);

/**
 * @brief  Set every logical bit in the bitmap.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 *
 * @note   Tail bits above @p bit_count are masked back to zero.
 */
void bitmap_fill(bitmap_word_t *bitmap, size_t bit_count);

/**
 * @brief  Set one logical bit.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 * @param  bit        Zero-based logical bit index.
 *
 * @retval 0        Set succeeded.
 * @retval -EINVAL  @p bitmap is NULL or @p bit_count is zero.
 * @retval -ERANGE  @p bit is outside [0, bit_count).
 */
int bitmap_set(bitmap_word_t *bitmap, size_t bit_count, size_t bit);

/**
 * @brief  Clear one logical bit.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 * @param  bit        Zero-based logical bit index.
 *
 * @retval 0        Clear succeeded.
 * @retval -EINVAL  @p bitmap is NULL or @p bit_count is zero.
 * @retval -ERANGE  @p bit is outside [0, bit_count).
 */
int bitmap_clear(bitmap_word_t *bitmap, size_t bit_count, size_t bit);

/**
 * @brief  Write one logical bit to a specific boolean value.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 * @param  bit        Zero-based logical bit index.
 * @param  value      true sets the bit, false clears it.
 *
 * @retval 0        Write succeeded.
 * @retval -EINVAL  @p bitmap is NULL or @p bit_count is zero.
 * @retval -ERANGE  @p bit is outside [0, bit_count).
 */
int bitmap_write(bitmap_word_t *bitmap, size_t bit_count, size_t bit, bool value);

/**
 * @brief  Test one logical bit.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 * @param  bit        Zero-based logical bit index.
 *
 * @return `true` when the bit is set; otherwise `false`.
 */
bool bitmap_test(const bitmap_word_t *bitmap, size_t bit_count, size_t bit);

/**
 * @brief  Set a contiguous logical bit range.
 *
 * @param  bitmap      Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count   Number of logical bits represented by @p bitmap.
 * @param  start_bit   First logical bit in the range.
 * @param  bit_len     Number of bits to set.  Zero is accepted as a no-op.
 *
 * @retval 0        Range set succeeded.
 * @retval -EINVAL  @p bitmap is NULL or @p bit_count is zero.
 * @retval -ERANGE  The requested range exceeds the logical bitmap length.
 */
int bitmap_set_range(bitmap_word_t *bitmap, size_t bit_count, size_t start_bit, size_t bit_len);

/**
 * @brief  Clear a contiguous logical bit range.
 *
 * @param  bitmap      Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count   Number of logical bits represented by @p bitmap.
 * @param  start_bit   First logical bit in the range.
 * @param  bit_len     Number of bits to clear.  Zero is accepted as a no-op.
 *
 * @retval 0        Range clear succeeded.
 * @retval -EINVAL  @p bitmap is NULL or @p bit_count is zero.
 * @retval -ERANGE  The requested range exceeds the logical bitmap length.
 */
int bitmap_clear_range(bitmap_word_t *bitmap, size_t bit_count, size_t start_bit, size_t bit_len);

/**
 * @brief  Count the number of set bits across the logical bitmap range.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 *
 * @return Number of set logical bits.  Returns 0 on invalid input.
 */
size_t bitmap_count_set(const bitmap_word_t *bitmap, size_t bit_count);

/**
 * @brief  Find the first set logical bit.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 * @param  index      Output index location.  Must not be NULL.
 *
 * @return `true` when a set bit was found and written to @p *index;
 *         otherwise `false`.
 */
bool bitmap_find_first_set(const bitmap_word_t *bitmap, size_t bit_count, size_t *index);

/**
 * @brief  Find the first clear logical bit.
 *
 * @param  bitmap     Caller-owned bitmap storage.  Must not be NULL.
 * @param  bit_count  Number of logical bits represented by @p bitmap.
 * @param  index      Output index location.  Must not be NULL.
 *
 * @return `true` when a clear bit was found and written to @p *index;
 *         otherwise `false`.
 */
bool bitmap_find_first_zero(const bitmap_word_t *bitmap, size_t bit_count, size_t *index);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
