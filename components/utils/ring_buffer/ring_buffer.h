/**
 * @file    ring_buffer.h
 * @brief   Caller-owned single-producer single-consumer byte ring buffer
 * @version v4.0.0
 * @date    2026.05.01
 *
 * Design overview
 * ---------------
 * `ring_buffer` is a caller-owned byte FIFO for strict single-producer /
 * single-consumer data streams. The public structure is plain old data so it
 * can live inside larger runtime objects, be zero-initialised statically, and
 * cross compiler boundaries without leaking atomic backend types.
 *
 * The implementation deliberately separates contract from optimisation:
 * - any positive capacity is accepted;
 * - capacities that support a binary wrap mask use that fast path internally;
 * - other capacities transparently fall back to modulo indexing;
 * - producer/consumer publication ordering stays sealed inside `ring_buffer.c`.
 *
 * Usage example
 * -------------
 * uint8_t         storage[13];
 * struct ring_buf rb = {0};
 *
 * if (ring_buf_init(&rb, storage, sizeof(storage)) == 0) {
 *     (void)ring_buf_put(&rb, (const uint8_t *)"ABC", 3U);
 * }
 *
 * Ownership / lifecycle / context
 * --------------------------------
 * - The caller owns both `struct ring_buf` and the backing byte storage.
 * - Producer-side APIs: `ring_buf_put*()`, `ring_buf_get_write_ptr()`,
 *   `ring_buf_write_advance()`, `ring_buf_write_rollback()`, and
 *   `ring_buf_put_force()`.
 * - Consumer-side APIs: `ring_buf_get*()`, `ring_buf_drop()`,
 *   `ring_buf_get_read_ptr()`, and `ring_buf_read_advance()`.
 * - Zero-copy reservations are side-local. After `ring_buf_get_write_ptr()`,
 *   the producer must finish with `ring_buf_write_advance()` before invoking
 *   another producer mutation API on the same ring. The read side follows the
 *   same rule for `ring_buf_get_read_ptr()` and `ring_buf_read_advance()`.
 * - Strictly supports one producer and one consumer only. Mixing Task and ISR
 *   on the same end is not guaranteed.
 * - `ring_buf_reset()`, `ring_buf_put_force()`, and `ring_buf_write_rollback()`
 *   may modify the opposite side's visibility and therefore require external
 *   serialization when the other side can run concurrently.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no RTOS dependency.
 * - Copy APIs may split a transfer into two `memcpy()` calls when the logical
 *   window wraps around the backing storage.
 *
 * Failure / recovery
 * ------------------
 * - Initialization and boundary-contract failures return negative `-errno`.
 * - Copy APIs return partial byte counts as `ssize_t` when the ring fills or
 *   drains before the requested length is satisfied.
 * - `ring_buf_write_advance()` and `ring_buf_read_advance()` never clamp.
 *   Committing more bytes than the reserved or currently contiguous window
 *   fails with `-ERANGE`.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include "tmdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief Compile-time fast-path cache for static ring declarations.
 *
 * A non-zero result means the capacity can use a binary wrap mask and the
 * implementation may use bit masking instead of modulo for index wrap.
 */
#define RB_STATIC_WRAP_MASK(sz) \
    ((((sz) != 0U) && (((sz) & ((sz) - 1U)) == 0U)) ? ((size_t)(sz) - 1U) : 0U)

/**
 * @brief Declare one static ring buffer with embedded backing storage.
 *
 * @param name  Public ring object name.
 * @param sz    Backing storage size in bytes. Any positive constant is valid.
 */
#define RB_DEFINE(name, sz)                           \
    static uint8_t _##name##_pool[(sz)];             \
    static struct ring_buf name = {                  \
        .buffer            = _##name##_pool,         \
        .capacity          = (sz),                   \
        .wrap_mask         = RB_STATIC_WRAP_MASK(sz),\
        .head              = 0U,                     \
        .tail              = 0U,                     \
        .write_reserve_len = 0U,                     \
        .read_reserve_len  = 0U,                     \
    }

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief Caller-owned ring buffer control block.
 *
 * Do not mutate these fields directly after initialisation. The layout is
 * public only so other Themis POD runtime objects can embed the control block
 * without exposing compiler-specific atomic wrapper types.
 */
struct ring_buf {
    /**
     * @brief Backing byte storage owned by the caller.
     */
    uint8_t *buffer;

    /**
     * @brief Capacity in bytes. Any positive value is valid.
     */
    size_t capacity;

    /**
     * @brief Cached wrap mask for binary-mask-friendly capacities.
     *
     * Zero means the implementation must use modulo indexing.
     */
    size_t wrap_mask;

    /**
     * @brief Producer publication cursor in free-running bytes.
     */
    size_t head;

    /**
     * @brief Consumer retirement cursor in free-running bytes.
     */
    size_t tail;

    /**
     * @brief Outstanding producer-side zero-copy reservation length.
     */
    size_t write_reserve_len;

    /**
     * @brief Outstanding consumer-side zero-copy reservation length.
     */
    size_t read_reserve_len;
};

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief Initialise one ring buffer over caller-owned storage.
 *
 * @param[in,out] rb        Ring control block.
 * @param[in]     pool      Caller-owned backing storage.
 * @param[in]     capacity  Backing storage size in bytes. Must be > 0.
 *
 * @retval 0         Initialization succeeded.
 * @retval -EINVAL   A required pointer was NULL or @p capacity was zero.
 */
int ring_buf_init(struct ring_buf *rb, uint8_t *pool, size_t capacity);

/**
 * @brief Reset one ring to the empty state and clear zero-copy reservations.
 *
 * @param[in,out] rb  Ring control block.
 *
 * @retval 0         Reset succeeded.
 * @retval -EINVAL   @p rb was NULL.
 */
int ring_buf_reset(struct ring_buf *rb);

/* Snapshot Helpers ----------------------------------------------------------------------------- */

/**
 * @brief Query the committed unread byte count.
 *
 * @param[in] rb  Ring control block.
 *
 * @return Current unread byte count, or `0` when @p rb is NULL.
 */
size_t ring_buf_data_len(const struct ring_buf *rb);

/**
 * @brief Query the committed free byte count.
 *
 * @param[in] rb  Ring control block.
 *
 * @return Current free byte count, or `0` when @p rb is NULL.
 */
size_t ring_buf_space_len(const struct ring_buf *rb);

/**
 * @brief Check whether the committed ring state is empty.
 *
 * @param[in] rb  Ring control block.
 *
 * @return `true` when no committed unread bytes exist, otherwise `false`.
 */
bool ring_buf_is_empty(const struct ring_buf *rb);

/**
 * @brief Check whether the committed ring state is full.
 *
 * @param[in] rb  Ring control block.
 *
 * @return `true` when no committed free bytes remain, otherwise `false`.
 */
bool ring_buf_is_full(const struct ring_buf *rb);

/* Producer APIs -------------------------------------------------------------------------------- */

/**
 * @brief Write one byte into the ring.
 *
 * @param[in,out] rb  Ring control block.
 * @param[in]     ch  Byte to publish.
 *
 * @retval 0         The byte was written.
 * @retval -EINVAL   @p rb was NULL.
 * @retval -EBUSY    A producer-side zero-copy reservation is still active.
 * @retval -ENOSPC   The ring is full.
 */
int ring_buf_put_char(struct ring_buf *rb, uint8_t ch);

/**
 * @brief Copy as many bytes as possible from @p data into the ring.
 *
 * @param[in,out] rb    Ring control block.
 * @param[in]     data  Source buffer.
 * @param[in]     len   Requested byte count.
 *
 * @return Non-negative written byte count, or negative `-errno` on contract
 *         failure.
 */
ssize_t ring_buf_put(struct ring_buf *rb, const uint8_t *data, size_t len);

/**
 * @brief Force-write bytes into the ring, discarding the oldest data when
 *        necessary.
 *
 * @param[in,out] rb    Ring control block.
 * @param[in]     data  Source buffer.
 * @param[in]     len   Requested byte count.
 *
 * @return Non-negative byte count retained from @p data, or negative
 *         `-errno` on contract failure.
 */
ssize_t ring_buf_put_force(struct ring_buf *rb, const uint8_t *data, size_t len);

/**
 * @brief Roll back the newest committed producer bytes.
 *
 * @param[in,out] rb   Ring control block.
 * @param[in]     len  Maximum rollback byte count.
 *
 * @return Non-negative rolled-back byte count, or negative `-errno` on
 *         contract failure.
 */
ssize_t ring_buf_write_rollback(struct ring_buf *rb, size_t len);

/* Consumer APIs -------------------------------------------------------------------------------- */

/**
 * @brief Read and consume one byte from the ring.
 *
 * @param[in,out] rb  Ring control block.
 * @param[out]    ch  Destination byte.
 *
 * @retval 0         One byte was read.
 * @retval -EINVAL   A required pointer was NULL.
 * @retval -EBUSY    A consumer-side zero-copy reservation is still active.
 * @retval -ENODATA  The ring is empty.
 */
int ring_buf_get_char(struct ring_buf *rb, uint8_t *ch);

/**
 * @brief Copy and consume as many bytes as possible from the ring.
 *
 * @param[in,out] rb    Ring control block.
 * @param[out]    data  Destination buffer.
 * @param[in]     len   Requested byte count.
 *
 * @return Non-negative consumed byte count, or negative `-errno` on contract
 *         failure.
 */
ssize_t ring_buf_get(struct ring_buf *rb, uint8_t *data, size_t len);

/**
 * @brief Peek one byte without consuming it.
 *
 * @param[in]  rb      Ring control block.
 * @param[in]  offset  Offset from the current read cursor.
 * @param[out] ch      Destination byte.
 *
 * @retval 0         The byte was peeked.
 * @retval -EINVAL   A required pointer was NULL.
 * @retval -ERANGE   @p offset lies outside committed unread data.
 */
int ring_buf_peek_char(const struct ring_buf *rb, size_t offset, uint8_t *ch);

/**
 * @brief Copy unread bytes without consuming them.
 *
 * @param[in]  rb      Ring control block.
 * @param[in]  offset  Offset from the current read cursor.
 * @param[out] data    Destination buffer.
 * @param[in]  len     Requested byte count.
 *
 * @return Non-negative peeked byte count, or negative `-errno` on contract
 *         failure.
 */
ssize_t ring_buf_peek(const struct ring_buf *rb, size_t offset, uint8_t *data, size_t len);

/**
 * @brief Find one byte value inside the committed unread window.
 *
 * @param[in]  rb    Ring control block.
 * @param[in]  byte  Byte value to search for.
 * @param[out] pos   Offset from the current read cursor.
 *
 * @retval 0         The byte was found and @p pos was written.
 * @retval -EINVAL   A required pointer was NULL.
 * @retval -ENOENT   The byte is not present in the committed unread window.
 */
int ring_buf_find_byte(const struct ring_buf *rb, uint8_t byte, size_t *pos);

/**
 * @brief Drop committed unread bytes without copying them out.
 *
 * @param[in,out] rb   Ring control block.
 * @param[in]     len  Maximum drop byte count.
 *
 * @return Non-negative dropped byte count, or negative `-errno` on contract
 *         failure.
 */
ssize_t ring_buf_drop(struct ring_buf *rb, size_t len);

/* Zero-Copy APIs ------------------------------------------------------------------------------- */

/**
 * @brief Reserve or re-query the current contiguous producer span.
 *
 * When a producer reservation is already active, the same reservation is
 * returned again until `ring_buf_write_advance()` finishes it.
 *
 * @param[in,out] rb    Ring control block.
 * @param[out]    size  Reserved contiguous byte count.
 *
 * @return Pointer to the reserved contiguous region, or `NULL` when no region
 *         is available or arguments are invalid.
 */
uint8_t *ring_buf_get_write_ptr(struct ring_buf *rb, size_t *size);

/**
 * @brief Commit bytes previously written into the contiguous producer span.
 *
 * If no explicit reservation is active, this function validates @p len
 * against the ring's current contiguous producer window. This keeps DMA-style
 * producers usable while still rejecting over-advance instead of silently
 * clamping it.
 *
 * @param[in,out] rb   Ring control block.
 * @param[in]     len  Produced byte count. `0` cancels an active reservation.
 *
 * @retval 0         The advance succeeded.
 * @retval -EINVAL   @p rb was NULL.
 * @retval -ERANGE   @p len exceeds the reserved or implicit contiguous window.
 */
int ring_buf_write_advance(struct ring_buf *rb, size_t len);

/**
 * @brief Reserve or re-query the current contiguous consumer span.
 *
 * When a consumer reservation is already active, the same reservation is
 * returned again until `ring_buf_read_advance()` finishes it.
 *
 * @param[in,out] rb    Ring control block.
 * @param[out]    size  Reserved contiguous byte count.
 *
 * @return Pointer to the reserved contiguous region, or `NULL` when no region
 *         is available or arguments are invalid.
 */
uint8_t *ring_buf_get_read_ptr(struct ring_buf *rb, size_t *size);

/**
 * @brief Retire bytes previously consumed from the contiguous consumer span.
 *
 * If no explicit reservation is active, this function validates @p len
 * against the ring's current contiguous consumer window.
 *
 * @param[in,out] rb   Ring control block.
 * @param[in]     len  Consumed byte count. `0` cancels an active reservation.
 *
 * @retval 0         The advance succeeded.
 * @retval -EINVAL   @p rb was NULL.
 * @retval -ERANGE   @p len exceeds the reserved or implicit contiguous window.
 */
int ring_buf_read_advance(struct ring_buf *rb, size_t len);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
