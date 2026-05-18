/**
 * @file    bitops.h
 * @brief   Fixed-width bit manipulation helpers for caller-owned data
 * @version v1.0.0
 * @date    2026.04.04
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 *
 * ============================================================================
 * Design philosophy: pure word helpers with no hidden state
 * ============================================================================
 *
 * These helpers operate on a single machine word and return the transformed
 * value directly.  They allocate nothing, keep no state, and introduce no
 * platform dependency.  This keeps them usable by containers, codecs, and
 * tests without dragging in RTOS or BSP semantics.
 *
 * The contract is intentionally narrow:
 *   - The word width follows `bitops_word_t`.
 *   - Invalid bit indices do not trap; helpers return a safe no-op style
 *     result instead.
 *   - Thread safety is the caller's responsibility because these functions
 *     are ordinary pure computations on caller-owned values.
 *
 * ============================================================================
 * Minimal usage example
 * ============================================================================
 *
 * @code
 * bitops_word_t flags = 0U;
 *
 * flags = bitops_set32(flags, 3U);
 * if (bitops_test32(flags, 3U)) {
 *     flags = bitops_clear32(flags, 3U);
 * }
 * @endcode
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Physical word type operated on by this header.
 */
typedef uint32_t bitops_word_t;

/* Public macros -------------------------------------------------------------------------------- */

/**
 * @brief  Number of bits in the word type operated on by this header.
 */
#define BITOPS_WORD_BITS ((uint32_t)(sizeof(bitops_word_t) * 8U))

/* Public inline functions ---------------------------------------------------------------------- */

/**
 * @brief  Return a single-bit mask for @p bit.
 *
 * @param  bit  Zero-based bit index.
 *
 * @return `1U << bit` when @p bit is in `[0, 31]`, or `0U` when @p bit is
 *         outside the supported range.
 */
static inline bitops_word_t bitops_mask32(uint32_t bit)
{
    return (bit < BITOPS_WORD_BITS) ? ((bitops_word_t)1U << bit) : 0U;
}

/**
 * @brief  Test whether a bit is set in @p word.
 *
 * @param  word  Source word to inspect.
 * @param  bit   Zero-based bit index.
 *
 * @return `true` when the bit is set; otherwise `false`.
 */
static inline bool bitops_test32(bitops_word_t word, uint32_t bit)
{
    return (word & bitops_mask32(bit)) != 0U;
}

/**
 * @brief  Return @p word with bit @p bit set.
 *
 * @param  word  Source word.
 * @param  bit   Zero-based bit index.
 *
 * @return Updated word.  If @p bit is out of range, the original word is
 *         returned unchanged.
 */
static inline bitops_word_t bitops_set32(bitops_word_t word, uint32_t bit)
{
    return word | bitops_mask32(bit);
}

/**
 * @brief  Return @p word with bit @p bit cleared.
 *
 * @param  word  Source word.
 * @param  bit   Zero-based bit index.
 *
 * @return Updated word.  If @p bit is out of range, the original word is
 *         returned unchanged.
 */
static inline bitops_word_t bitops_clear32(bitops_word_t word, uint32_t bit)
{
    return word & ~bitops_mask32(bit);
}

/**
 * @brief  Return @p word with bit @p bit written to @p value.
 *
 * @param  word   Source word.
 * @param  bit    Zero-based bit index.
 * @param  value  true sets the bit, false clears it.
 *
 * @return Updated word.  If @p bit is out of range, the original word is
 *         returned unchanged.
 */
static inline bitops_word_t bitops_write32(bitops_word_t word, uint32_t bit, bool value)
{
    return value ? bitops_set32(word, bit) : bitops_clear32(word, bit);
}

/**
 * @brief  Count the number of set bits in @p word.
 *
 * @param  word  Source word.
 *
 * @return Population count in the range [0, 32].
 */
static inline uint32_t bitops_popcount32(bitops_word_t word)
{
#if defined(__clang__) || defined(__GNUC__)
    if (sizeof(bitops_word_t) == sizeof(unsigned int)) {
        return (uint32_t)__builtin_popcount((unsigned int)word);
    }
    if (sizeof(bitops_word_t) == sizeof(unsigned long)) {
        return (uint32_t)__builtin_popcountl((unsigned long)word);
    }
    return (uint32_t)__builtin_popcountll((unsigned long long)word);
#else
    uint32_t count = 0U;

    /*
     * Kernighan's algorithm clears one set bit per iteration, so sparse words
     * finish faster than the naive "shift through all 32 positions" fallback.
     */
    while (word != 0U) {
        word &= (word - 1U);
        count++;
    }

    return count;
#endif
}

/**
 * @brief  Count trailing zero bits in @p word.
 *
 * @param  word  Source word.
 *
 * @return Index of the first set bit when @p word is non-zero, or
 *         `BITOPS_WORD_BITS` when @p word is zero.
 */
static inline uint32_t bitops_ctz32(bitops_word_t word)
{
    if (word == 0U) {
        return BITOPS_WORD_BITS;
    }

#if defined(__clang__) || defined(__GNUC__)
    if (sizeof(bitops_word_t) == sizeof(unsigned int)) {
        return (uint32_t)__builtin_ctz((unsigned int)word);
    }
    if (sizeof(bitops_word_t) == sizeof(unsigned long)) {
        return (uint32_t)__builtin_ctzl((unsigned long)word);
    }
    return (uint32_t)__builtin_ctzll((unsigned long long)word);
#else
    uint32_t count = 0U;

    while ((word & 1U) == 0U) {
        word >>= 1U;
        count++;
    }

    return count;
#endif
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
