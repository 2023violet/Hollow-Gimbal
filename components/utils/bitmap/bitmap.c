/**
 * @file    bitmap.c
 * @brief   Caller-owned fixed-size bitmap helpers
 * @version v1.0.0
 * @date    2026.04.04
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 *
 * The implementation stays intentionally small and explicit:
 *   - range helpers fast-path whole-word spans while keeping boundary masks explicit;
 *   - tail masking keeps unused storage bits outside the logical bitmap range;
 *   - no hidden allocation or synchronization is introduced here.
 *
 * This favors auditable behaviour for the small and medium bitmaps typically
 * used by embedded resource maps.
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "bitmap.h"
#include <string.h>

static bool _bitmap_args_ok(const bitmap_word_t *bitmap, size_t bit_count)
{
    return (bitmap != NULL) && (bit_count != 0U);
}

enum {
    _BITMAP_WORD_SHIFT = (BITMAP_WORD_BITS == 8U)  ? 3U
                       : (BITMAP_WORD_BITS == 16U) ? 4U
                       : (BITMAP_WORD_BITS == 32U) ? 5U
                       : (BITMAP_WORD_BITS == 64U) ? 6U
                                                   : 0U,
    _BITMAP_WORD_MASK  = BITMAP_WORD_BITS - 1U,
};

typedef char _bitmap_word_bits_must_be_power_of_two[((BITMAP_WORD_BITS == 8U) ||
                                                      (BITMAP_WORD_BITS == 16U) ||
                                                      (BITMAP_WORD_BITS == 32U) ||
                                                      (BITMAP_WORD_BITS == 64U))
                                                         ? 1
                                                         : -1];

static bool _bitmap_index_ok(size_t bit_count, size_t bit)
{
    return bit < bit_count;
}

static size_t _bitmap_word_index(size_t bit)
{
    return bit >> _BITMAP_WORD_SHIFT;
}

static uint32_t _bitmap_word_bit(size_t bit)
{
    return (uint32_t)(bit & _BITMAP_WORD_MASK);
}

static bitmap_word_t _bitmap_tail_mask(size_t bit_count)
{
    uint32_t valid_bits = (uint32_t)(bit_count & _BITMAP_WORD_MASK);

    if (valid_bits == 0U) {
        return (bitmap_word_t)~(bitmap_word_t)0;
    }

    /* Keep only logical tail bits; storage bits above bit_count stay ignored. */
    return ((bitmap_word_t)1U << valid_bits) - (bitmap_word_t)1U;
}

static bitmap_word_t _bitmap_mask_from_bit(uint32_t word_bit)
{
    if (word_bit == 0U) {
        return (bitmap_word_t)~(bitmap_word_t)0;
    }

    return ((bitmap_word_t)~(bitmap_word_t)0) << word_bit;
}

static bitmap_word_t _bitmap_mask_until_bit(uint32_t word_bit)
{
    if (word_bit >= (BITMAP_WORD_BITS - 1U)) {
        return (bitmap_word_t)~(bitmap_word_t)0;
    }

    return ((bitmap_word_t)1U << (word_bit + 1U)) - (bitmap_word_t)1U;
}

static bitmap_word_t _bitmap_range_mask(uint32_t first_word_bit, uint32_t last_word_bit)
{
    return _bitmap_mask_from_bit(first_word_bit) & _bitmap_mask_until_bit(last_word_bit);
}

static int _bitmap_range_args_ok(const bitmap_word_t *bitmap,
                                 size_t               bit_count,
                                 size_t               start_bit,
                                 size_t               bit_len)
{
    if (!_bitmap_args_ok(bitmap, bit_count)) {
        return -EINVAL;
    }
    if ((bit_len != 0U) && ((start_bit >= bit_count) || (bit_len > (bit_count - start_bit)))) {
        return -ERANGE;
    }

    return 0;
}

static void _bitmap_apply_range(bitmap_word_t *bitmap,
                                size_t         start_bit,
                                size_t         bit_len,
                                bool           set_bits)
{
    size_t   first_word;
    size_t   last_word;
    uint32_t first_word_bit;
    uint32_t last_word_bit;

    if (bit_len == 0U) {
        return;
    }

    first_word     = _bitmap_word_index(start_bit);
    last_word      = _bitmap_word_index(start_bit + bit_len - 1U);
    first_word_bit = _bitmap_word_bit(start_bit);
    last_word_bit  = _bitmap_word_bit(start_bit + bit_len - 1U);

    if (first_word == last_word) {
        bitmap_word_t mask = _bitmap_range_mask(first_word_bit, last_word_bit);

        bitmap[first_word] = set_bits ? (bitmap[first_word] | mask) : (bitmap[first_word] & ~mask);
        return;
    }

    bitmap[first_word] = set_bits ? (bitmap[first_word] | _bitmap_mask_from_bit(first_word_bit))
                                  : (bitmap[first_word] & ~_bitmap_mask_from_bit(first_word_bit));

    for (size_t word = first_word + 1U; word < last_word; ++word) {
        bitmap[word] = set_bits ? (bitmap_word_t)~(bitmap_word_t)0 : 0U;
    }

    bitmap[last_word] = set_bits ? (bitmap[last_word] | _bitmap_mask_until_bit(last_word_bit))
                                 : (bitmap[last_word] & ~_bitmap_mask_until_bit(last_word_bit));
}

int bitmap_clear_all(bitmap_word_t *bitmap, size_t bit_count)
{
    if (!_bitmap_args_ok(bitmap, bit_count)) {
        return -EINVAL;
    }

    memset(bitmap, 0, BITMAP_WORD_COUNT(bit_count) * sizeof(bitmap[0]));
    return 0;
}

void bitmap_fill(bitmap_word_t *bitmap, size_t bit_count)
{
    size_t word_count;

    if (!_bitmap_args_ok(bitmap, bit_count)) {
        return;
    }

    word_count = BITMAP_WORD_COUNT(bit_count);
    for (size_t i = 0U; i < word_count; i++) {
        bitmap[i] = (bitmap_word_t)~(bitmap_word_t)0;
    }

    /* The last storage word may contain padding bits beyond the logical range. */
    bitmap[word_count - 1U] &= _bitmap_tail_mask(bit_count);
}

int bitmap_set(bitmap_word_t *bitmap, size_t bit_count, size_t bit)
{
    size_t   word_index;
    uint32_t word_bit;

    if (!_bitmap_args_ok(bitmap, bit_count)) {
        return -EINVAL;
    }
    if (!_bitmap_index_ok(bit_count, bit)) {
        return -ERANGE;
    }

    word_index         = _bitmap_word_index(bit);
    word_bit           = _bitmap_word_bit(bit);
    bitmap[word_index] = bitops_set32(bitmap[word_index], word_bit);
    return 0;
}

int bitmap_clear(bitmap_word_t *bitmap, size_t bit_count, size_t bit)
{
    size_t   word_index;
    uint32_t word_bit;

    if (!_bitmap_args_ok(bitmap, bit_count)) {
        return -EINVAL;
    }
    if (!_bitmap_index_ok(bit_count, bit)) {
        return -ERANGE;
    }

    word_index         = _bitmap_word_index(bit);
    word_bit           = _bitmap_word_bit(bit);
    bitmap[word_index] = bitops_clear32(bitmap[word_index], word_bit);
    return 0;
}

int bitmap_write(bitmap_word_t *bitmap, size_t bit_count, size_t bit, bool value)
{
    return value ? bitmap_set(bitmap, bit_count, bit) : bitmap_clear(bitmap, bit_count, bit);
}

bool bitmap_test(const bitmap_word_t *bitmap, size_t bit_count, size_t bit)
{
    size_t   word_index;
    uint32_t word_bit;

    if (!_bitmap_args_ok(bitmap, bit_count)) {
        return false;
    }
    if (!_bitmap_index_ok(bit_count, bit)) {
        return false;
    }

    word_index = _bitmap_word_index(bit);
    word_bit   = _bitmap_word_bit(bit);
    return bitops_test32(bitmap[word_index], word_bit);
}

int bitmap_set_range(bitmap_word_t *bitmap, size_t bit_count, size_t start_bit, size_t bit_len)
{
    int rc = _bitmap_range_args_ok(bitmap, bit_count, start_bit, bit_len);

    if (rc != 0) {
        return rc;
    }

    _bitmap_apply_range(bitmap, start_bit, bit_len, true);
    return 0;
}

int bitmap_clear_range(bitmap_word_t *bitmap, size_t bit_count, size_t start_bit, size_t bit_len)
{
    int rc = _bitmap_range_args_ok(bitmap, bit_count, start_bit, bit_len);

    if (rc != 0) {
        return rc;
    }

    _bitmap_apply_range(bitmap, start_bit, bit_len, false);
    return 0;
}

size_t bitmap_count_set(const bitmap_word_t *bitmap, size_t bit_count)
{
    size_t word_count;
    size_t total = 0U;

    if (!_bitmap_args_ok(bitmap, bit_count)) {
        return 0U;
    }

    word_count = BITMAP_WORD_COUNT(bit_count);
    for (size_t i = 0U; i < word_count; i++) {
        bitmap_word_t word = bitmap[i];

        if (i == (word_count - 1U)) {
            /* Ignore padding bits that do not belong to the logical bitmap. */
            word &= _bitmap_tail_mask(bit_count);
        }
        total += bitops_popcount32(word);
    }

    return total;
}

bool bitmap_find_first_set(const bitmap_word_t *bitmap, size_t bit_count, size_t *index)
{
    size_t word_count;

    if ((index == NULL) || !_bitmap_args_ok(bitmap, bit_count)) {
        return false;
    }

    word_count = BITMAP_WORD_COUNT(bit_count);
    for (size_t i = 0U; i < word_count; i++) {
        bitmap_word_t word = bitmap[i];

        if (i == (word_count - 1U)) {
            word &= _bitmap_tail_mask(bit_count);
        }

        if (word != 0U) {
            /* ctz32 gives the first set bit within the current physical word. */
            *index = (i * BITMAP_WORD_BITS) + bitops_ctz32(word);
            return true;
        }
    }

    return false;
}

bool bitmap_find_first_zero(const bitmap_word_t *bitmap, size_t bit_count, size_t *index)
{
    size_t word_count;

    if ((index == NULL) || !_bitmap_args_ok(bitmap, bit_count)) {
        return false;
    }

    word_count = BITMAP_WORD_COUNT(bit_count);
    for (size_t i = 0U; i < word_count; i++) {
        bitmap_word_t word = ~bitmap[i];

        if (i == (word_count - 1U)) {
            /* Padding bits are forced to zero so they never look available. */
            word &= _bitmap_tail_mask(bit_count);
        }

        if (word != 0U) {
            *index = (i * BITMAP_WORD_BITS) + bitops_ctz32(word);
            return true;
        }
    }

    return false;
}

/* ---------------------------------------- end of file ----------------------------------------- */
