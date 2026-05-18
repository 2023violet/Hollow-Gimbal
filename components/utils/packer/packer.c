/**
 * @file    packer.c
 * @brief   Boundary-bound binary packing and unpacking helpers
 * @version v2.0.0
 * @date    2026.05.01
 *
 * Implementation role
 * -------------------
 * - pairs with `packer.h`
 * - owns checked cursor validation and failure semantics
 * - keeps unsafe helpers inline in the header for zero-overhead call sites
 *
 * Key collaborators
 * -----------------
 * - `packer.h`: caller-facing cursor contract and unsafe helpers
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "packer.h"
#include <errno.h>

/* Private Helpers ------------------------------------------------------------------------------ */

static int _packer_write_check(const struct packer_write_cursor *cursor, size_t width)
{
    /*
     * Checked helpers validate the cursor span before any byte moves. The
     * unsafe layer is reserved for callers that already proved capacity.
     */
    if (cursor == NULL) {
        return -EINVAL;
    }

    if ((cursor->begin == NULL) || (cursor->cursor == NULL) || (cursor->end == NULL)) {
        return -EINVAL;
    }

    if ((cursor->begin > cursor->cursor) || (cursor->cursor > cursor->end)) {
        return -EINVAL;
    }

    if ((size_t)(cursor->end - cursor->cursor) < width) {
        return -ENOSPC;
    }

    return 0;
}

static int _packer_read_check(const struct packer_read_cursor *cursor, size_t width)
{
    /*
     * Reads use the same contract as writes, but report underrun with
     * -ENODATA so callers can distinguish "not enough source bytes" from a
     * malformed cursor object.
     */
    if (cursor == NULL) {
        return -EINVAL;
    }

    if ((cursor->begin == NULL) || (cursor->cursor == NULL) || (cursor->end == NULL)) {
        return -EINVAL;
    }

    if ((cursor->begin > cursor->cursor) || (cursor->cursor > cursor->end)) {
        return -EINVAL;
    }

    if ((size_t)(cursor->end - cursor->cursor) < width) {
        return -ENODATA;
    }

    return 0;
}

#define _PACKER_WRITE_IMPL(name, width, value_type, unsafe_fn)                  \
    int name(struct packer_write_cursor *cursor, value_type value)              \
    {                                                                           \
        int rc;                                                                 \
                                                                                \
        rc = _packer_write_check(cursor, (width));                              \
        if (rc != 0) {                                                          \
            return rc;                                                          \
        }                                                                       \
                                                                                \
        unsafe_fn(&cursor->cursor, value);                                      \
        return 0;                                                               \
    }

#define _PACKER_READ_IMPL(name, width, value_type, unsafe_fn)                   \
    int name(struct packer_read_cursor *cursor, value_type *value)              \
    {                                                                           \
        int rc;                                                                 \
                                                                                \
        if (value == NULL) {                                                    \
            return -EINVAL;                                                     \
        }                                                                       \
                                                                                \
        rc = _packer_read_check(cursor, (width));                               \
        if (rc != 0) {                                                          \
            return rc;                                                          \
        }                                                                       \
                                                                                \
        *value = unsafe_fn(&cursor->cursor);                                    \
        return 0;                                                               \
    }

/* Public Functions ----------------------------------------------------------------------------- */

int packer_write_cursor_init(struct packer_write_cursor *cursor, uint8_t *buffer, size_t capacity)
{
    if (cursor == NULL) {
        return -EINVAL;
    }

    if ((buffer == NULL) && (capacity != 0U)) {
        return -EINVAL;
    }

    cursor->begin  = buffer;
    cursor->cursor = buffer;
    cursor->end    = (buffer == NULL) ? NULL : (buffer + capacity);
    return 0;
}

int packer_read_cursor_init(struct packer_read_cursor *cursor, const uint8_t *buffer, size_t size)
{
    if (cursor == NULL) {
        return -EINVAL;
    }

    if ((buffer == NULL) && (size != 0U)) {
        return -EINVAL;
    }

    cursor->begin  = buffer;
    cursor->cursor = buffer;
    cursor->end    = (buffer == NULL) ? NULL : (buffer + size);
    return 0;
}

size_t packer_write_offset(const struct packer_write_cursor *cursor)
{
    if ((cursor == NULL) || (cursor->begin == NULL) || (cursor->cursor == NULL) ||
        (cursor->cursor < cursor->begin)) {
        return 0U;
    }

    return (size_t)(cursor->cursor - cursor->begin);
}

size_t packer_write_remaining(const struct packer_write_cursor *cursor)
{
    if ((cursor == NULL) || (cursor->cursor == NULL) || (cursor->end == NULL) ||
        (cursor->end < cursor->cursor)) {
        return 0U;
    }

    return (size_t)(cursor->end - cursor->cursor);
}

size_t packer_read_offset(const struct packer_read_cursor *cursor)
{
    if ((cursor == NULL) || (cursor->begin == NULL) || (cursor->cursor == NULL) ||
        (cursor->cursor < cursor->begin)) {
        return 0U;
    }

    return (size_t)(cursor->cursor - cursor->begin);
}

size_t packer_read_remaining(const struct packer_read_cursor *cursor)
{
    if ((cursor == NULL) || (cursor->cursor == NULL) || (cursor->end == NULL) ||
        (cursor->end < cursor->cursor)) {
        return 0U;
    }

    return (size_t)(cursor->end - cursor->cursor);
}

_PACKER_WRITE_IMPL(packer_write_u8, 1U, uint8_t, packer_unsafe_write_u8)
_PACKER_WRITE_IMPL(packer_write_u16_le, 2U, uint16_t, packer_unsafe_write_u16_le)
_PACKER_WRITE_IMPL(packer_write_u16_be, 2U, uint16_t, packer_unsafe_write_u16_be)
_PACKER_WRITE_IMPL(packer_write_u32_le, 4U, uint32_t, packer_unsafe_write_u32_le)
_PACKER_WRITE_IMPL(packer_write_u32_be, 4U, uint32_t, packer_unsafe_write_u32_be)
_PACKER_WRITE_IMPL(packer_write_u64_le, 8U, uint64_t, packer_unsafe_write_u64_le)
_PACKER_WRITE_IMPL(packer_write_u64_be, 8U, uint64_t, packer_unsafe_write_u64_be)
_PACKER_WRITE_IMPL(packer_write_i8, 1U, int8_t, packer_unsafe_write_i8)
_PACKER_WRITE_IMPL(packer_write_i16_le, 2U, int16_t, packer_unsafe_write_i16_le)
_PACKER_WRITE_IMPL(packer_write_i16_be, 2U, int16_t, packer_unsafe_write_i16_be)
_PACKER_WRITE_IMPL(packer_write_i32_le, 4U, int32_t, packer_unsafe_write_i32_le)
_PACKER_WRITE_IMPL(packer_write_i32_be, 4U, int32_t, packer_unsafe_write_i32_be)
_PACKER_WRITE_IMPL(packer_write_i64_le, 8U, int64_t, packer_unsafe_write_i64_le)
_PACKER_WRITE_IMPL(packer_write_i64_be, 8U, int64_t, packer_unsafe_write_i64_be)
_PACKER_WRITE_IMPL(packer_write_f32_le, 4U, float, packer_unsafe_write_f32_le)
_PACKER_WRITE_IMPL(packer_write_f32_be, 4U, float, packer_unsafe_write_f32_be)
_PACKER_WRITE_IMPL(packer_write_f64_le, 8U, double, packer_unsafe_write_f64_le)
_PACKER_WRITE_IMPL(packer_write_f64_be, 8U, double, packer_unsafe_write_f64_be)

int packer_write_bytes(struct packer_write_cursor *cursor, const uint8_t *data, size_t len)
{
    int rc;

    if ((data == NULL) && (len != 0U)) {
        return -EINVAL;
    }

    rc = _packer_write_check(cursor, len);
    if (rc != 0) {
        return rc;
    }

    packer_unsafe_write_bytes(&cursor->cursor, data, len);
    return 0;
}

_PACKER_READ_IMPL(packer_read_u8, 1U, uint8_t, packer_unsafe_read_u8)
_PACKER_READ_IMPL(packer_read_u16_le, 2U, uint16_t, packer_unsafe_read_u16_le)
_PACKER_READ_IMPL(packer_read_u16_be, 2U, uint16_t, packer_unsafe_read_u16_be)
_PACKER_READ_IMPL(packer_read_u32_le, 4U, uint32_t, packer_unsafe_read_u32_le)
_PACKER_READ_IMPL(packer_read_u32_be, 4U, uint32_t, packer_unsafe_read_u32_be)
_PACKER_READ_IMPL(packer_read_u64_le, 8U, uint64_t, packer_unsafe_read_u64_le)
_PACKER_READ_IMPL(packer_read_u64_be, 8U, uint64_t, packer_unsafe_read_u64_be)
_PACKER_READ_IMPL(packer_read_i8, 1U, int8_t, packer_unsafe_read_i8)
_PACKER_READ_IMPL(packer_read_i16_le, 2U, int16_t, packer_unsafe_read_i16_le)
_PACKER_READ_IMPL(packer_read_i16_be, 2U, int16_t, packer_unsafe_read_i16_be)
_PACKER_READ_IMPL(packer_read_i32_le, 4U, int32_t, packer_unsafe_read_i32_le)
_PACKER_READ_IMPL(packer_read_i32_be, 4U, int32_t, packer_unsafe_read_i32_be)
_PACKER_READ_IMPL(packer_read_i64_le, 8U, int64_t, packer_unsafe_read_i64_le)
_PACKER_READ_IMPL(packer_read_i64_be, 8U, int64_t, packer_unsafe_read_i64_be)
_PACKER_READ_IMPL(packer_read_f32_le, 4U, float, packer_unsafe_read_f32_le)
_PACKER_READ_IMPL(packer_read_f32_be, 4U, float, packer_unsafe_read_f32_be)
_PACKER_READ_IMPL(packer_read_f64_le, 8U, double, packer_unsafe_read_f64_le)
_PACKER_READ_IMPL(packer_read_f64_be, 8U, double, packer_unsafe_read_f64_be)

int packer_read_bytes(struct packer_read_cursor *cursor, uint8_t *out, size_t len)
{
    int rc;

    if ((out == NULL) && (len != 0U)) {
        return -EINVAL;
    }

    rc = _packer_read_check(cursor, len);
    if (rc != 0) {
        return rc;
    }

    packer_unsafe_read_bytes(&cursor->cursor, out, len);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
