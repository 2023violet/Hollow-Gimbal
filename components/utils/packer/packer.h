/**
 * @file    packer.h
 * @brief   Boundary-bound binary packing and unpacking helpers
 * @version v2.0.0
 * @date    2026.05.01
 *
 * Design overview
 * ---------------
 * `packer` provides two explicit layers:
 * - a checked cursor layer for protocol boundaries and untrusted lengths;
 * - an unsafe zero-overhead layer for tightly controlled hot paths.
 *
 * The checked layer binds cursor and boundary together. Every read or write
 * either completes fully and advances the cursor by the documented byte count,
 * or fails with negative `-errno` and leaves the cursor unchanged.
 *
 * The unsafe layer keeps the classic pointer-to-pointer style for callers that
 * already proved capacity elsewhere. It performs no validation, but still uses
 * `memcpy()` for floating-point bit transport so strict aliasing and unaligned
 * access stay well-defined on low-end MCUs.
 *
 * Usage example
 * -------------
 * uint8_t                    frame[16];
 * struct packer_write_cursor writer;
 * struct packer_read_cursor  reader;
 *
 * if (packer_write_cursor_init(&writer, frame, sizeof(frame)) == 0 &&
 *     packer_write_u16_le(&writer, 0x1234U) == 0 &&
 *     packer_write_f32_be(&writer, 1.25f) == 0 &&
 *     packer_read_cursor_init(&reader, frame, packer_write_offset(&writer)) == 0) {
 *     uint16_t id = 0U;
 *     float    value = 0.0f;
 *
 *     (void)packer_read_u16_le(&reader, &id);
 *     (void)packer_read_f32_be(&reader, &value);
 * }
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns every buffer and every cursor object.
 * - Checked cursors borrow the supplied buffer span for the caller-managed
 *   cursor lifetime; no allocation or hidden global state exists.
 * - Re-entrant, thread-safe, and ISR-safe when each call operates on caller-
 *   owned cursors and buffers without unsynchronised overlap.
 * - Checked cursor failures never advance the cursor.
 * - Unsafe helpers advance exactly by the encoded or decoded byte width.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no RTOS dependency.
 * - Checked helpers add boundary validation; unsafe helpers add no checks.
 *
 * Failure / recovery
 * ------------------
 * - Checked initialisers and operations return `0` or negative `-errno`.
 * - `-EINVAL` means a required pointer or cursor contract was invalid.
 * - `-ENOSPC` means a checked write would exceed the writable span.
 * - `-ENODATA` means a checked read would exceed the readable span.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Checked write cursor bound to one writable byte span.
 */
struct packer_write_cursor {
    /**
     * @brief First writable byte in the caller-owned span.
     */
    uint8_t *begin;

    /**
     * @brief Next byte that will be written on success.
     */
    uint8_t *cursor;

    /**
     * @brief One-past-the-end boundary of the writable span.
     */
    uint8_t *end;
};

/**
 * @brief Checked read cursor bound to one readable byte span.
 */
struct packer_read_cursor {
    /**
     * @brief First readable byte in the caller-owned span.
     */
    const uint8_t *begin;

    /**
     * @brief Next byte that will be read on success.
     */
    const uint8_t *cursor;

    /**
     * @brief One-past-the-end boundary of the readable span.
     */
    const uint8_t *end;
};

/* Checked Cursor Lifecycle --------------------------------------------------------------------- */

/**
 * @brief Initialise a checked write cursor over one writable byte span.
 *
 * @param[in,out] cursor    Cursor object to initialise.
 * @param[in]     buffer    Caller-owned writable byte span. May be NULL only
 *                          when @p capacity is zero.
 * @param[in]     capacity  Writable byte count in @p buffer.
 *
 * @retval 0         Initialisation succeeded.
 * @retval -EINVAL   A required argument contract was invalid.
 */
int packer_write_cursor_init(struct packer_write_cursor *cursor, uint8_t *buffer, size_t capacity);

/**
 * @brief Initialise a checked read cursor over one readable byte span.
 *
 * @param[in,out] cursor  Cursor object to initialise.
 * @param[in]     buffer  Caller-owned readable byte span. May be NULL only
 *                        when @p size is zero.
 * @param[in]     size    Readable byte count in @p buffer.
 *
 * @retval 0         Initialisation succeeded.
 * @retval -EINVAL   A required argument contract was invalid.
 */
int packer_read_cursor_init(struct packer_read_cursor *cursor, const uint8_t *buffer, size_t size);

/* Checked Cursor Snapshot ---------------------------------------------------------------------- */

/**
 * @brief Return the number of bytes already written through a checked cursor.
 *
 * @param[in] cursor  Checked write cursor.
 *
 * @return Written byte count from the span start, or `0` when @p cursor is
 *         invalid.
 */
size_t packer_write_offset(const struct packer_write_cursor *cursor);

/**
 * @brief Return the remaining writable byte count.
 *
 * @param[in] cursor  Checked write cursor.
 *
 * @return Remaining writable byte count, or `0` when @p cursor is invalid.
 */
size_t packer_write_remaining(const struct packer_write_cursor *cursor);

/**
 * @brief Return the number of bytes already consumed through a checked cursor.
 *
 * @param[in] cursor  Checked read cursor.
 *
 * @return Consumed byte count from the span start, or `0` when @p cursor is
 *         invalid.
 */
size_t packer_read_offset(const struct packer_read_cursor *cursor);

/**
 * @brief Return the remaining readable byte count.
 *
 * @param[in] cursor  Checked read cursor.
 *
 * @return Remaining readable byte count, or `0` when @p cursor is invalid.
 */
size_t packer_read_remaining(const struct packer_read_cursor *cursor);

/* Checked Write APIs --------------------------------------------------------------------------- */

/**
 * @brief Write one `uint8_t`.
 *
 * Advances the checked write cursor by 1 byte on success.
 */
int packer_write_u8(struct packer_write_cursor *cursor, uint8_t value);

/**
 * @brief Write one `uint16_t` in little-endian order.
 *
 * Advances the checked write cursor by 2 bytes on success.
 */
int packer_write_u16_le(struct packer_write_cursor *cursor, uint16_t value);

/**
 * @brief Write one `uint16_t` in big-endian order.
 *
 * Advances the checked write cursor by 2 bytes on success.
 */
int packer_write_u16_be(struct packer_write_cursor *cursor, uint16_t value);

/**
 * @brief Write one `uint32_t` in little-endian order.
 *
 * Advances the checked write cursor by 4 bytes on success.
 */
int packer_write_u32_le(struct packer_write_cursor *cursor, uint32_t value);

/**
 * @brief Write one `uint32_t` in big-endian order.
 *
 * Advances the checked write cursor by 4 bytes on success.
 */
int packer_write_u32_be(struct packer_write_cursor *cursor, uint32_t value);

/**
 * @brief Write one `uint64_t` in little-endian order.
 *
 * Advances the checked write cursor by 8 bytes on success.
 */
int packer_write_u64_le(struct packer_write_cursor *cursor, uint64_t value);

/**
 * @brief Write one `uint64_t` in big-endian order.
 *
 * Advances the checked write cursor by 8 bytes on success.
 */
int packer_write_u64_be(struct packer_write_cursor *cursor, uint64_t value);

/**
 * @brief Write one `int8_t`.
 *
 * Advances the checked write cursor by 1 byte on success.
 */
int packer_write_i8(struct packer_write_cursor *cursor, int8_t value);

/**
 * @brief Write one `int16_t` in little-endian order.
 *
 * Advances the checked write cursor by 2 bytes on success.
 */
int packer_write_i16_le(struct packer_write_cursor *cursor, int16_t value);

/**
 * @brief Write one `int16_t` in big-endian order.
 *
 * Advances the checked write cursor by 2 bytes on success.
 */
int packer_write_i16_be(struct packer_write_cursor *cursor, int16_t value);

/**
 * @brief Write one `int32_t` in little-endian order.
 *
 * Advances the checked write cursor by 4 bytes on success.
 */
int packer_write_i32_le(struct packer_write_cursor *cursor, int32_t value);

/**
 * @brief Write one `int32_t` in big-endian order.
 *
 * Advances the checked write cursor by 4 bytes on success.
 */
int packer_write_i32_be(struct packer_write_cursor *cursor, int32_t value);

/**
 * @brief Write one `int64_t` in little-endian order.
 *
 * Advances the checked write cursor by 8 bytes on success.
 */
int packer_write_i64_le(struct packer_write_cursor *cursor, int64_t value);

/**
 * @brief Write one `int64_t` in big-endian order.
 *
 * Advances the checked write cursor by 8 bytes on success.
 */
int packer_write_i64_be(struct packer_write_cursor *cursor, int64_t value);

/**
 * @brief Write one IEEE-754 `float` in little-endian order.
 *
 * Advances the checked write cursor by 4 bytes on success.
 */
int packer_write_f32_le(struct packer_write_cursor *cursor, float value);

/**
 * @brief Write one IEEE-754 `float` in big-endian order.
 *
 * Advances the checked write cursor by 4 bytes on success.
 */
int packer_write_f32_be(struct packer_write_cursor *cursor, float value);

/**
 * @brief Write one IEEE-754 `double` in little-endian order.
 *
 * Advances the checked write cursor by 8 bytes on success.
 */
int packer_write_f64_le(struct packer_write_cursor *cursor, double value);

/**
 * @brief Write one IEEE-754 `double` in big-endian order.
 *
 * Advances the checked write cursor by 8 bytes on success.
 */
int packer_write_f64_be(struct packer_write_cursor *cursor, double value);

/**
 * @brief Copy raw bytes into the checked write span.
 *
 * On success the cursor advances by @p len bytes. On failure it does not move.
 */
int packer_write_bytes(struct packer_write_cursor *cursor, const uint8_t *data, size_t len);

/* Checked Read APIs ---------------------------------------------------------------------------- */

/**
 * @brief Read one `uint8_t`.
 *
 * Advances the checked read cursor by 1 byte on success.
 */
int packer_read_u8(struct packer_read_cursor *cursor, uint8_t *value);

/**
 * @brief Read one `uint16_t` in little-endian order.
 *
 * Advances the checked read cursor by 2 bytes on success.
 */
int packer_read_u16_le(struct packer_read_cursor *cursor, uint16_t *value);

/**
 * @brief Read one `uint16_t` in big-endian order.
 *
 * Advances the checked read cursor by 2 bytes on success.
 */
int packer_read_u16_be(struct packer_read_cursor *cursor, uint16_t *value);

/**
 * @brief Read one `uint32_t` in little-endian order.
 *
 * Advances the checked read cursor by 4 bytes on success.
 */
int packer_read_u32_le(struct packer_read_cursor *cursor, uint32_t *value);

/**
 * @brief Read one `uint32_t` in big-endian order.
 *
 * Advances the checked read cursor by 4 bytes on success.
 */
int packer_read_u32_be(struct packer_read_cursor *cursor, uint32_t *value);

/**
 * @brief Read one `uint64_t` in little-endian order.
 *
 * Advances the checked read cursor by 8 bytes on success.
 */
int packer_read_u64_le(struct packer_read_cursor *cursor, uint64_t *value);

/**
 * @brief Read one `uint64_t` in big-endian order.
 *
 * Advances the checked read cursor by 8 bytes on success.
 */
int packer_read_u64_be(struct packer_read_cursor *cursor, uint64_t *value);

/**
 * @brief Read one `int8_t`.
 *
 * Advances the checked read cursor by 1 byte on success.
 */
int packer_read_i8(struct packer_read_cursor *cursor, int8_t *value);

/**
 * @brief Read one `int16_t` in little-endian order.
 *
 * Advances the checked read cursor by 2 bytes on success.
 */
int packer_read_i16_le(struct packer_read_cursor *cursor, int16_t *value);

/**
 * @brief Read one `int16_t` in big-endian order.
 *
 * Advances the checked read cursor by 2 bytes on success.
 */
int packer_read_i16_be(struct packer_read_cursor *cursor, int16_t *value);

/**
 * @brief Read one `int32_t` in little-endian order.
 *
 * Advances the checked read cursor by 4 bytes on success.
 */
int packer_read_i32_le(struct packer_read_cursor *cursor, int32_t *value);

/**
 * @brief Read one `int32_t` in big-endian order.
 *
 * Advances the checked read cursor by 4 bytes on success.
 */
int packer_read_i32_be(struct packer_read_cursor *cursor, int32_t *value);

/**
 * @brief Read one `int64_t` in little-endian order.
 *
 * Advances the checked read cursor by 8 bytes on success.
 */
int packer_read_i64_le(struct packer_read_cursor *cursor, int64_t *value);

/**
 * @brief Read one `int64_t` in big-endian order.
 *
 * Advances the checked read cursor by 8 bytes on success.
 */
int packer_read_i64_be(struct packer_read_cursor *cursor, int64_t *value);

/**
 * @brief Read one IEEE-754 `float` in little-endian order.
 *
 * Advances the checked read cursor by 4 bytes on success.
 */
int packer_read_f32_le(struct packer_read_cursor *cursor, float *value);

/**
 * @brief Read one IEEE-754 `float` in big-endian order.
 *
 * Advances the checked read cursor by 4 bytes on success.
 */
int packer_read_f32_be(struct packer_read_cursor *cursor, float *value);

/**
 * @brief Read one IEEE-754 `double` in little-endian order.
 *
 * Advances the checked read cursor by 8 bytes on success.
 */
int packer_read_f64_le(struct packer_read_cursor *cursor, double *value);

/**
 * @brief Read one IEEE-754 `double` in big-endian order.
 *
 * Advances the checked read cursor by 8 bytes on success.
 */
int packer_read_f64_be(struct packer_read_cursor *cursor, double *value);

/**
 * @brief Copy raw bytes out of the checked read span.
 *
 * On success the cursor advances by @p len bytes. On failure it does not move.
 */
int packer_read_bytes(struct packer_read_cursor *cursor, uint8_t *out, size_t len);

/* Unsafe Helpers -------------------------------------------------------------------------------- */

/*
 * These helpers intentionally skip boundary checks. They remain in the public
 * header because the header is the implementation unit for this zero-overhead
 * path, but callers must only use them after proving the span is valid by
 * other means.
 */

/**
 * @brief Return the byte distance between two unsafe cursor positions.
 *
 * @param[in] begin   Span start pointer.
 * @param[in] cursor  Current cursor pointer within the same span.
 *
 * @return Byte distance from @p begin to @p cursor.
 */
static inline size_t packer_unsafe_advanced(const void *begin, const void *cursor)
{
    return (size_t)((const uint8_t *)cursor - (const uint8_t *)begin);
}

/**
 * @brief Write one `uint8_t` without boundary checks.
 */
static inline void packer_unsafe_write_u8(uint8_t **cursor, uint8_t value)
{
    (*cursor)[0] = value;
    *cursor += 1U;
}

/**
 * @brief Write one `uint16_t` in little-endian order without checks.
 */
static inline void packer_unsafe_write_u16_le(uint8_t **cursor, uint16_t value)
{
    (*cursor)[0] = (uint8_t)(value);
    (*cursor)[1] = (uint8_t)(value >> 8U);
    *cursor += 2U;
}

/**
 * @brief Write one `uint16_t` in big-endian order without checks.
 */
static inline void packer_unsafe_write_u16_be(uint8_t **cursor, uint16_t value)
{
    (*cursor)[0] = (uint8_t)(value >> 8U);
    (*cursor)[1] = (uint8_t)(value);
    *cursor += 2U;
}

/**
 * @brief Write one `uint32_t` in little-endian order without checks.
 */
static inline void packer_unsafe_write_u32_le(uint8_t **cursor, uint32_t value)
{
    (*cursor)[0] = (uint8_t)(value);
    (*cursor)[1] = (uint8_t)(value >> 8U);
    (*cursor)[2] = (uint8_t)(value >> 16U);
    (*cursor)[3] = (uint8_t)(value >> 24U);
    *cursor += 4U;
}

/**
 * @brief Write one `uint32_t` in big-endian order without checks.
 */
static inline void packer_unsafe_write_u32_be(uint8_t **cursor, uint32_t value)
{
    (*cursor)[0] = (uint8_t)(value >> 24U);
    (*cursor)[1] = (uint8_t)(value >> 16U);
    (*cursor)[2] = (uint8_t)(value >> 8U);
    (*cursor)[3] = (uint8_t)(value);
    *cursor += 4U;
}

/**
 * @brief Write one `uint64_t` in little-endian order without checks.
 */
static inline void packer_unsafe_write_u64_le(uint8_t **cursor, uint64_t value)
{
    (*cursor)[0] = (uint8_t)(value);
    (*cursor)[1] = (uint8_t)(value >> 8U);
    (*cursor)[2] = (uint8_t)(value >> 16U);
    (*cursor)[3] = (uint8_t)(value >> 24U);
    (*cursor)[4] = (uint8_t)(value >> 32U);
    (*cursor)[5] = (uint8_t)(value >> 40U);
    (*cursor)[6] = (uint8_t)(value >> 48U);
    (*cursor)[7] = (uint8_t)(value >> 56U);
    *cursor += 8U;
}

/**
 * @brief Write one `uint64_t` in big-endian order without checks.
 */
static inline void packer_unsafe_write_u64_be(uint8_t **cursor, uint64_t value)
{
    (*cursor)[0] = (uint8_t)(value >> 56U);
    (*cursor)[1] = (uint8_t)(value >> 48U);
    (*cursor)[2] = (uint8_t)(value >> 40U);
    (*cursor)[3] = (uint8_t)(value >> 32U);
    (*cursor)[4] = (uint8_t)(value >> 24U);
    (*cursor)[5] = (uint8_t)(value >> 16U);
    (*cursor)[6] = (uint8_t)(value >> 8U);
    (*cursor)[7] = (uint8_t)(value);
    *cursor += 8U;
}

/**
 * @brief Write one `int8_t` without boundary checks.
 */
static inline void packer_unsafe_write_i8(uint8_t **cursor, int8_t value)
{
    packer_unsafe_write_u8(cursor, (uint8_t)value);
}

/**
 * @brief Write one `int16_t` in little-endian order without checks.
 */
static inline void packer_unsafe_write_i16_le(uint8_t **cursor, int16_t value)
{
    packer_unsafe_write_u16_le(cursor, (uint16_t)value);
}

/**
 * @brief Write one `int16_t` in big-endian order without checks.
 */
static inline void packer_unsafe_write_i16_be(uint8_t **cursor, int16_t value)
{
    packer_unsafe_write_u16_be(cursor, (uint16_t)value);
}

/**
 * @brief Write one `int32_t` in little-endian order without checks.
 */
static inline void packer_unsafe_write_i32_le(uint8_t **cursor, int32_t value)
{
    packer_unsafe_write_u32_le(cursor, (uint32_t)value);
}

/**
 * @brief Write one `int32_t` in big-endian order without checks.
 */
static inline void packer_unsafe_write_i32_be(uint8_t **cursor, int32_t value)
{
    packer_unsafe_write_u32_be(cursor, (uint32_t)value);
}

/**
 * @brief Write one `int64_t` in little-endian order without checks.
 */
static inline void packer_unsafe_write_i64_le(uint8_t **cursor, int64_t value)
{
    packer_unsafe_write_u64_le(cursor, (uint64_t)value);
}

/**
 * @brief Write one `int64_t` in big-endian order without checks.
 */
static inline void packer_unsafe_write_i64_be(uint8_t **cursor, int64_t value)
{
    packer_unsafe_write_u64_be(cursor, (uint64_t)value);
}

/**
 * @brief Write one `float` in little-endian order without checks.
 */
static inline void packer_unsafe_write_f32_le(uint8_t **cursor, float value)
{
    uint32_t bits = 0U;

    memcpy(&bits, &value, sizeof(bits));
    packer_unsafe_write_u32_le(cursor, bits);
}

/**
 * @brief Write one `float` in big-endian order without checks.
 */
static inline void packer_unsafe_write_f32_be(uint8_t **cursor, float value)
{
    uint32_t bits = 0U;

    memcpy(&bits, &value, sizeof(bits));
    packer_unsafe_write_u32_be(cursor, bits);
}

/**
 * @brief Write one `double` in little-endian order without checks.
 */
static inline void packer_unsafe_write_f64_le(uint8_t **cursor, double value)
{
    uint64_t bits = 0U;

    memcpy(&bits, &value, sizeof(bits));
    packer_unsafe_write_u64_le(cursor, bits);
}

/**
 * @brief Write one `double` in big-endian order without checks.
 */
static inline void packer_unsafe_write_f64_be(uint8_t **cursor, double value)
{
    uint64_t bits = 0U;

    memcpy(&bits, &value, sizeof(bits));
    packer_unsafe_write_u64_be(cursor, bits);
}

/**
 * @brief Copy raw bytes without boundary checks.
 */
static inline void packer_unsafe_write_bytes(uint8_t **cursor, const uint8_t *data, size_t len)
{
    if (len == 0U) {
        return;
    }

    memcpy(*cursor, data, len);
    *cursor += len;
}

/**
 * @brief Read one `uint8_t` without boundary checks.
 */
static inline uint8_t packer_unsafe_read_u8(const uint8_t **cursor)
{
    uint8_t value = (*cursor)[0];

    *cursor += 1U;
    return value;
}

/**
 * @brief Read one `uint16_t` in little-endian order without checks.
 */
static inline uint16_t packer_unsafe_read_u16_le(const uint8_t **cursor)
{
    uint16_t value = (uint16_t)(*cursor)[0] | ((uint16_t)(*cursor)[1] << 8U);

    *cursor += 2U;
    return value;
}

/**
 * @brief Read one `uint16_t` in big-endian order without checks.
 */
static inline uint16_t packer_unsafe_read_u16_be(const uint8_t **cursor)
{
    uint16_t value = ((uint16_t)(*cursor)[0] << 8U) | (uint16_t)(*cursor)[1];

    *cursor += 2U;
    return value;
}

/**
 * @brief Read one `uint32_t` in little-endian order without checks.
 */
static inline uint32_t packer_unsafe_read_u32_le(const uint8_t **cursor)
{
    uint32_t value = (uint32_t)(*cursor)[0] | ((uint32_t)(*cursor)[1] << 8U) |
                     ((uint32_t)(*cursor)[2] << 16U) | ((uint32_t)(*cursor)[3] << 24U);

    *cursor += 4U;
    return value;
}

/**
 * @brief Read one `uint32_t` in big-endian order without checks.
 */
static inline uint32_t packer_unsafe_read_u32_be(const uint8_t **cursor)
{
    uint32_t value = ((uint32_t)(*cursor)[0] << 24U) | ((uint32_t)(*cursor)[1] << 16U) |
                     ((uint32_t)(*cursor)[2] << 8U) | (uint32_t)(*cursor)[3];

    *cursor += 4U;
    return value;
}

/**
 * @brief Read one `uint64_t` in little-endian order without checks.
 */
static inline uint64_t packer_unsafe_read_u64_le(const uint8_t **cursor)
{
    uint64_t value = (uint64_t)(*cursor)[0] | ((uint64_t)(*cursor)[1] << 8U) |
                     ((uint64_t)(*cursor)[2] << 16U) | ((uint64_t)(*cursor)[3] << 24U) |
                     ((uint64_t)(*cursor)[4] << 32U) | ((uint64_t)(*cursor)[5] << 40U) |
                     ((uint64_t)(*cursor)[6] << 48U) | ((uint64_t)(*cursor)[7] << 56U);

    *cursor += 8U;
    return value;
}

/**
 * @brief Read one `uint64_t` in big-endian order without checks.
 */
static inline uint64_t packer_unsafe_read_u64_be(const uint8_t **cursor)
{
    uint64_t value = ((uint64_t)(*cursor)[0] << 56U) | ((uint64_t)(*cursor)[1] << 48U) |
                     ((uint64_t)(*cursor)[2] << 40U) | ((uint64_t)(*cursor)[3] << 32U) |
                     ((uint64_t)(*cursor)[4] << 24U) | ((uint64_t)(*cursor)[5] << 16U) |
                     ((uint64_t)(*cursor)[6] << 8U) | (uint64_t)(*cursor)[7];

    *cursor += 8U;
    return value;
}

/**
 * @brief Read one `int8_t` without boundary checks.
 */
static inline int8_t packer_unsafe_read_i8(const uint8_t **cursor)
{
    return (int8_t)packer_unsafe_read_u8(cursor);
}

/**
 * @brief Read one `int16_t` in little-endian order without checks.
 */
static inline int16_t packer_unsafe_read_i16_le(const uint8_t **cursor)
{
    return (int16_t)packer_unsafe_read_u16_le(cursor);
}

/**
 * @brief Read one `int16_t` in big-endian order without checks.
 */
static inline int16_t packer_unsafe_read_i16_be(const uint8_t **cursor)
{
    return (int16_t)packer_unsafe_read_u16_be(cursor);
}

/**
 * @brief Read one `int32_t` in little-endian order without checks.
 */
static inline int32_t packer_unsafe_read_i32_le(const uint8_t **cursor)
{
    return (int32_t)packer_unsafe_read_u32_le(cursor);
}

/**
 * @brief Read one `int32_t` in big-endian order without checks.
 */
static inline int32_t packer_unsafe_read_i32_be(const uint8_t **cursor)
{
    return (int32_t)packer_unsafe_read_u32_be(cursor);
}

/**
 * @brief Read one `int64_t` in little-endian order without checks.
 */
static inline int64_t packer_unsafe_read_i64_le(const uint8_t **cursor)
{
    return (int64_t)packer_unsafe_read_u64_le(cursor);
}

/**
 * @brief Read one `int64_t` in big-endian order without checks.
 */
static inline int64_t packer_unsafe_read_i64_be(const uint8_t **cursor)
{
    return (int64_t)packer_unsafe_read_u64_be(cursor);
}

/**
 * @brief Read one `float` in little-endian order without checks.
 */
static inline float packer_unsafe_read_f32_le(const uint8_t **cursor)
{
    uint32_t bits  = packer_unsafe_read_u32_le(cursor);
    float    value = 0.0f;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

/**
 * @brief Read one `float` in big-endian order without checks.
 */
static inline float packer_unsafe_read_f32_be(const uint8_t **cursor)
{
    uint32_t bits  = packer_unsafe_read_u32_be(cursor);
    float    value = 0.0f;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

/**
 * @brief Read one `double` in little-endian order without checks.
 */
static inline double packer_unsafe_read_f64_le(const uint8_t **cursor)
{
    uint64_t bits  = packer_unsafe_read_u64_le(cursor);
    double   value = 0.0;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

/**
 * @brief Read one `double` in big-endian order without checks.
 */
static inline double packer_unsafe_read_f64_be(const uint8_t **cursor)
{
    uint64_t bits  = packer_unsafe_read_u64_be(cursor);
    double   value = 0.0;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

/**
 * @brief Copy raw bytes without boundary checks.
 */
static inline void packer_unsafe_read_bytes(const uint8_t **cursor, uint8_t *out, size_t len)
{
    if (len == 0U) {
        return;
    }

    memcpy(out, *cursor, len);
    *cursor += len;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
