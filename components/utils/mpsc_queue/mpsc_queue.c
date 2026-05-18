/**
 * @file    mpsc_queue.c
 * @brief   Intrusive lock-free MPSC queue for caller-owned nodes
 * @version v1.0.0
 * @date    2026.04.04
 *
 * Implementation role
 * -------------------
 * - pairs with `mpsc_queue.h`
 * - implements a stub-node MPSC queue in the style of the Vyukov algorithm
 * - keeps producer publication and single-consumer retirement invariants local
 *
 * Key collaborators
 * -----------------
 * - `mpsc_queue.h`: caller-facing ownership and return-code contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "mpsc_queue.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

/* Private Helpers ------------------------------------------------------------------------------ */

static inline struct mpsc_queue_node *_mpsc_ptr_load_relaxed(struct mpsc_queue_node *const *slot)
{
    return (struct mpsc_queue_node *)__atomic_load_n(slot, __ATOMIC_RELAXED);
}

static inline struct mpsc_queue_node *_mpsc_ptr_load_acquire(struct mpsc_queue_node *const *slot)
{
    return (struct mpsc_queue_node *)__atomic_load_n(slot, __ATOMIC_ACQUIRE);
}

static inline void _mpsc_ptr_store_relaxed(struct mpsc_queue_node **slot, struct mpsc_queue_node *value)
{
    __atomic_store_n(slot, value, __ATOMIC_RELAXED);
}

static inline void _mpsc_ptr_store_release(struct mpsc_queue_node **slot, struct mpsc_queue_node *value)
{
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
}

static inline struct mpsc_queue_node *_mpsc_ptr_exchange_acq_rel(struct mpsc_queue_node **slot,
                                                                 struct mpsc_queue_node  *value)
{
    return (struct mpsc_queue_node *)__atomic_exchange_n(slot, value, __ATOMIC_ACQ_REL);
}

static inline uint32_t _mpsc_u32_load_relaxed(const uint32_t *slot)
{
    return __atomic_load_n(slot, __ATOMIC_RELAXED);
}

static inline void _mpsc_u32_store_release(uint32_t *slot, uint32_t value)
{
    __atomic_store_n(slot, value, __ATOMIC_RELEASE);
}

static inline void _mpsc_u32_store_relaxed(uint32_t *slot, uint32_t value)
{
    __atomic_store_n(slot, value, __ATOMIC_RELAXED);
}

static inline bool _mpsc_u32_compare_exchange_acq_rel(uint32_t *slot,
                                                       uint32_t *expected,
                                                       uint32_t  desired)
{
    return __atomic_compare_exchange_n(slot,
                                       expected,
                                       desired,
                                       false,
                                       __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

static void _mpsc_queue_push_node(struct mpsc_queue *queue, struct mpsc_queue_node *node)
{
    struct mpsc_queue_node *prev;

    /* Step 1: prepare the new logical tail node before publication. */
    _mpsc_ptr_store_relaxed(&node->next, NULL);

    /*
     * Step 2: atomically swap the global head pointer.  This serialises all
     * producers and returns the previous last node in the chain.
     */
    prev = _mpsc_ptr_exchange_acq_rel(&queue->head, node);

    /*
     * Step 3: publish the forward link from the previous tail to the new node.
     * The consumer may observe queue->head before this store becomes visible,
     * which is why mpsc_queue_try_pop() can transiently return -EAGAIN.
     */
    _mpsc_ptr_store_release(&prev->next, node);
}

/* Public Functions ----------------------------------------------------------------------------- */

void mpsc_queue_node_init(struct mpsc_queue_node *node)
{
    if (node == NULL) {
        return;
    }

    _mpsc_ptr_store_relaxed(&node->next, NULL);
    _mpsc_u32_store_relaxed(&node->queued, 0U);
}

int mpsc_queue_init(struct mpsc_queue *queue)
{
    if (queue == NULL) {
        return -EINVAL;
    }

    mpsc_queue_node_init(&queue->stub);
    _mpsc_ptr_store_relaxed(&queue->head, &queue->stub);
    queue->tail = &queue->stub;
    return 0;
}

int mpsc_queue_push(struct mpsc_queue *queue, struct mpsc_queue_node *node)
{
    uint32_t expected;

    if ((queue == NULL) || (node == NULL) || (node == &queue->stub)) {
        return -EINVAL;
    }

    if (_mpsc_ptr_load_relaxed(&node->next) != NULL) {
        return -EBUSY;
    }

    expected = 0U;
    if (!_mpsc_u32_compare_exchange_acq_rel(&node->queued, &expected, 1U)) {
        return -EBUSY;
    }

    _mpsc_queue_push_node(queue, node);
    return 0;
}

int mpsc_queue_try_pop(struct mpsc_queue *queue, struct mpsc_queue_node **out_node)
{
    struct mpsc_queue_node *tail;
    struct mpsc_queue_node *next;
    struct mpsc_queue_node *head;

    if ((queue == NULL) || (out_node == NULL)) {
        return -EINVAL;
    }

    *out_node = NULL;

    tail = queue->tail;
    next = _mpsc_ptr_load_acquire(&tail->next);

    if (tail == &queue->stub) {
        /* Skip the internal stub once a real node appears behind it. */
        if (next == NULL) {
            return -ENOENT;
        }

        queue->tail = next;
        tail        = next;
        next        = _mpsc_ptr_load_acquire(&tail->next);
    }

    if (next != NULL) {
        /* Fast path: tail points at a real node with a visible successor. */
        queue->tail = next;
        _mpsc_ptr_store_relaxed(&tail->next, NULL);
        _mpsc_u32_store_release(&tail->queued, 0U);
        *out_node = tail;
        return 0;
    }

    head = _mpsc_ptr_load_acquire(&queue->head);
    if (tail != head) {
        /* A producer has exchanged head but has not linked tail->next yet. */
        return -EAGAIN;
    }

    /*
     * Reinsert the stub so the queue returns to the canonical "tail always has
     * a successor when data exists" shape, then retry once.
     */
    _mpsc_queue_push_node(queue, &queue->stub);
    next = _mpsc_ptr_load_acquire(&tail->next);
    if (next == NULL) {
        return -EAGAIN;
    }

    queue->tail = next;
    _mpsc_ptr_store_relaxed(&tail->next, NULL);
    _mpsc_u32_store_release(&tail->queued, 0U);
    *out_node = tail;
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
