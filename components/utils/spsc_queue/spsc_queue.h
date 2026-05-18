/**
 * @file    spsc_queue.h
 * @brief   Caller-owned lock-free single-producer / single-consumer pointer queue
 * @version v1.2.0
 * @date    2026.04.04
 *
 * Design overview
 * ---------------
 * `spsc_queue` provides one fixed-capacity pointer queue for the common
 * embedded handoff pattern where one producer publishes caller-owned pointers
 * and one consumer later retires them.
 *
 * Usage example
 * -------------
 * static void *g_queue_storage[32];
 * static struct spsc_queue g_queue;
 *
 * spsc_queue_init(&g_queue, g_queue_storage, 32U);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - The caller owns both `struct spsc_queue` and the backing `void *buffer[]`.
 * - Exactly one producer may call `spsc_queue_push()`.
 * - Exactly one consumer may call `spsc_queue_pop()`.
 * - The buffer must remain valid until the queue is permanently retired.
 * - @note Strictly supports single-producer single-consumer. Mixing Task and
 *   ISR on the same end is implementation-defined and not guaranteed.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no dynamic allocation, and no hidden buffering.
 * - Head/tail publication uses internal acquire/release synchronization rather
 *   than `volatile`.
 *
 * Failure / recovery
 * ------------------
 * - Invalid input fails with `-EINVAL`.
 * - Full queues fail with `-ENOSPC`; empty queues fail with `-ENOENT`.
 * - Capacity must be non-zero. Arbitrary positive capacities are fully
 *   supported, and the implementation may use a fast path internally when a
 *   capacity happens to be a power of two.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tmdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief  Declare one queue object plus its caller-owned backing array.
 *
 * @param  name  Queue variable name.
 * @param  cap   Queue capacity in slots. Must be non-zero.
 *
 * @note   The macro declares a ready-to-use queue that already points at its
 *         embedded storage. Use spsc_queue_init() only when the backing array
 *         is supplied separately at runtime.
 */
#define SPSC_QUEUE_DEFINE(name, cap)                                                          \
    typedef char TM_CONCAT(_##name##_spsc_queue_capacity_must_be_nonzero_, __LINE__)         \
        [((cap) > 0U) ? 1 : -1];                                                             \
    static void             *_##name##_buf[(cap)];                                           \
    static struct spsc_queue name = {                                                        \
        .buffer   = _##name##_buf,                                                           \
        .capacity = (cap),                                                                   \
        .head     = 0U,                                                                      \
        .tail     = 0U,                                                                      \
    }

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief  Queue control block.
 *
 * `head` is producer-owned publication state.
 * `tail` is consumer-owned retirement state.
 *
 * Both fields are represented as free-running indices so occupancy can be
 * computed as `head - tail` without special wraparound branches.
 */
struct spsc_queue {
    /** Caller-owned pointer array borrowed for the queue lifetime. */
    void           **buffer;
    /** Slot capacity. */
    size_t           capacity;
    /** Producer-owned publish index observed by the consumer. */
    size_t           head;
    /** Consumer-owned retire index observed by the producer. */
    size_t           tail;
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief  Initialise a queue over caller-supplied pointer storage.
 *
 * @param  q         Queue control block.
 * @param  buffer    Pointer array with at least @p capacity elements.
 * @param  capacity  Slot count. Must be non-zero.
 *
 * @retval 0         Initialization succeeded.
 * @retval -EINVAL   Arguments are invalid.
 */
int spsc_queue_init(struct spsc_queue *q, void **buffer, size_t capacity);

/* Producer / Consumer -------------------------------------------------------------------------- */

/**
 * @brief  Push one pointer into the queue.
 *
 * This API is producer-only. `NULL` is accepted as a valid payload.
 *
 * @param  q    Initialised queue.
 * @param  ptr  Pointer value to enqueue.
 *
 * @retval 0         Push succeeded.
 * @retval -EINVAL   @p q is NULL or does not reference valid backing storage.
 * @retval -ENOSPC   Queue is full.
 */
int spsc_queue_push(struct spsc_queue *q, void *ptr);

/**
 * @brief  Pop one pointer from the queue.
 *
 * This API is consumer-only.
 *
 * @param  q    Initialised queue.
 * @param  out  Output location for the dequeued pointer. Set to NULL on error.
 *
 * @retval 0         Pop succeeded.
 * @retval -EINVAL   Arguments are invalid.
 * @retval -ENOENT   Queue is empty.
 */
int spsc_queue_pop(struct spsc_queue *q, void **out);

/* Snapshot ------------------------------------------------------------------------------------- */

/**
 * @brief  Return a snapshot of the current queue occupancy.
 *
 * @note   The returned value is only a hint. Producer and consumer may change
 *         the occupancy again immediately after this call returns.
 *
 * @param  q  Initialised queue.
 *
 * @return Occupied slot count, or 0 when @p q is NULL.
 */
size_t spsc_queue_get_count(const struct spsc_queue *q);

/**
 * @brief  Return true when the queue is empty.
 *
 * @param  q  Initialised queue.
 *
 * @return `true` when the queue is empty or @p q is NULL; otherwise `false`.
 */
bool spsc_queue_is_empty(const struct spsc_queue *q);

/**
 * @brief  Return true when the queue is full.
 *
 * @note   This is a snapshot helper, not a reservation API.
 *
 * @param  q  Initialised queue.
 *
 * @return `true` when no free slot is currently visible; otherwise `false`.
 */
bool spsc_queue_is_full(const struct spsc_queue *q);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
