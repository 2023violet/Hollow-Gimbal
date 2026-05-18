/**
 * @file    mpsc_queue.h
 * @brief   Intrusive lock-free MPSC queue for caller-owned nodes
 * @version v1.0.0
 * @date    2026.04.04
 *
 * Design overview
 * ---------------
 * `mpsc_queue` is an intrusive multi-producer / single-consumer queue for
 * caller-owned work items. The queue stores no payload and allocates no
 * memory; callers embed `struct mpsc_queue_node` inside their own objects.
 *
 * Usage example
 * -------------
 * struct work_item {
 *     struct mpsc_queue_node node;
 *     uint32_t               id;
 * };
 *
 * static struct mpsc_queue g_queue;
 *
 * mpsc_queue_init(&g_queue);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - Multiple producers may call `mpsc_queue_push()` concurrently.
 * - Exactly one consumer may call `mpsc_queue_try_pop()`.
 * - A node must not be pushed again until the consumer has popped it and the
 *   owner has prepared it for reuse.
 * - @note Thread-safe only for the documented MPSC topology. Not ISR-safe
 *   unless the target atomic backend and caller scheduling model explicitly
 *   permit that usage.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no RTOS dependency.
 * - `mpsc_queue_try_pop()` distinguishes `-ENOENT` from `-EAGAIN` so the
 *   single consumer can tell a truly empty queue from an in-flight producer.
 *
 * Failure / recovery
 * ------------------
 * - Invalid input fails with `-EINVAL`.
 * - `mpsc_queue_node_init()` resets a caller-owned node back to a known state.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public macros -------------------------------------------------------------------------------- */

/**
 * @brief  Recover the enclosing object pointer from an embedded queue node.
 *
 * @param  ptr     Pointer returned by mpsc_queue_try_pop().
 * @param  type    Type of the enclosing caller-owned structure.
 * @param  member  Name of the struct mpsc_queue_node member within @p type.
 */
#define MPSC_QUEUE_ENTRY(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief  Intrusive node embedded by the caller inside each queued object.
 *
 * The field is managed by the queue while the object is enqueued.  Callers
 * must not modify it concurrently with queue operations.
 *
 * The @c queued flag is a single-owner guard used to reject double-enqueue
 * attempts before the node is linked into the queue.
 */
struct mpsc_queue_node {
    struct mpsc_queue_node *next;
    uint32_t                queued;
};

/**
 * @brief  Queue control block.
 *
 * The structure is caller-owned and must be initialised once with
 * mpsc_queue_init() before use.  Its fields are queue-managed after that and
 * must not be accessed directly by callers.
 *
 * Layout note
 * -----------
 * This control block deliberately stays tiny and portable.  It does not bake
 * in cache-line padding because the primary Themis targets are single-core
 * MCUs without data cache.  If a future cached platform proves false-sharing
 * pressure here, pad or wrap the queue at the integration layer rather than
 * turning this portable helper into a platform-tuned structure.
 */
struct mpsc_queue {
    struct mpsc_queue_node *head;
    struct mpsc_queue_node *tail;
    struct mpsc_queue_node  stub;
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief  Reset a queue node before first use or reuse.
 *
 * @param  node  Caller-owned node to prepare.  NULL is ignored.
 *
 * @note   This helper does not enqueue the node.  It only clears the embedded
 *         next pointer so the node starts from a known state.
 */
void mpsc_queue_node_init(struct mpsc_queue_node *node);

/**
 * @brief  Initialise an MPSC queue.
 *
 * @param  queue  Queue control block to initialise.  Must not be NULL.
 *
 * @retval 0        Initialization succeeded.
 * @retval -EINVAL  @p queue is NULL.
 *
 * @note   Call only when the queue is not concurrently in use.
 */
int mpsc_queue_init(struct mpsc_queue *queue);

/* Producer / Consumer -------------------------------------------------------------------------- */

/**
 * @brief  Push one caller-owned node into the queue.
 *
 * Safe for concurrent use by multiple producers.  The call is non-blocking and
 * does not take ownership of the enclosing object beyond linking it into the
 * queue.
 *
 * Producer-side push is intentionally split into a reservation guard and a
 * publication step so the single consumer can distinguish a truly empty queue
 * from a node that is still being linked by another producer.
 *
 * @param  queue  Initialised queue.  Must not be NULL.
 * @param  node   Caller-owned node to enqueue.  Must not be NULL and must not
 *                be the queue's internal stub node.
 *
 * @retval 0        Push succeeded.
 * @retval -EINVAL  Invalid input.
 * @retval -EBUSY   The node is already queued or still carries stale queue
 *                  linkage from a previous push attempt.
 */
int mpsc_queue_push(struct mpsc_queue *queue, struct mpsc_queue_node *node);

/**
 * @brief  Try to pop one node from the queue.
 *
 * This API is single-consumer only.  Calling it from more than one consumer
 * context at the same time is undefined.
 *
 * @param  queue     Initialised queue.  Must not be NULL.
 * @param  out_node  Output location for the popped node.  Must not be NULL.
 *                   On failure the function stores NULL.
 *
 * @retval 0        Pop succeeded and one node was written to @p *out_node.
 * @retval -EINVAL  Invalid input.
 * @retval -ENOENT  Queue is definitely empty.
 * @retval -EAGAIN  A producer is in the middle of publishing; retry later.
 */
int mpsc_queue_try_pop(struct mpsc_queue *queue, struct mpsc_queue_node **out_node);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
