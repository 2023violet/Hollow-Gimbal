/**
 * @file    spsc_queue.c
 * @brief   Caller-owned lock-free single-producer / single-consumer pointer queue
 * @version v1.2.0
 * @date    2026.04.04
 *
 * Implementation role
 * -------------------
 * - pairs with `spsc_queue.h`
 * - keeps the queue on acquire/release load-store primitives only
 * - implements producer publication and consumer retirement without extra locks
 *
 * Key collaborators
 * -----------------
 * - `spsc_queue.h`: caller-facing SPSC ownership and capacity contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "spsc_queue.h"
#include <errno.h>

/* Private Helpers ------------------------------------------------------------------------------ */

static inline size_t _spsc_head_load_relaxed(const struct spsc_queue *q)
{
    return __atomic_load_n(&q->head, __ATOMIC_RELAXED);
}

static inline size_t _spsc_head_load_acquire(const struct spsc_queue *q)
{
    return __atomic_load_n(&q->head, __ATOMIC_ACQUIRE);
}

static inline void _spsc_head_store_release(struct spsc_queue *q, size_t value)
{
    __atomic_store_n(&q->head, value, __ATOMIC_RELEASE);
}

static inline size_t _spsc_tail_load_relaxed(const struct spsc_queue *q)
{
    return __atomic_load_n(&q->tail, __ATOMIC_RELAXED);
}

static inline size_t _spsc_tail_load_acquire(const struct spsc_queue *q)
{
    return __atomic_load_n(&q->tail, __ATOMIC_ACQUIRE);
}

static inline void _spsc_tail_store_release(struct spsc_queue *q, size_t value)
{
    __atomic_store_n(&q->tail, value, __ATOMIC_RELEASE);
}

static inline size_t _spsc_slot_index(const struct spsc_queue *q, size_t index)
{
    return index % q->capacity;
}

/* Public Functions ----------------------------------------------------------------------------- */

int spsc_queue_init(struct spsc_queue *q, void **buffer, size_t capacity)
{
    if ((q == NULL) || (buffer == NULL) || (capacity == 0U)) {
        return -EINVAL;
    }

    q->buffer   = buffer;
    q->capacity = capacity;
    q->head     = 0U;
    q->tail     = 0U;

    return 0;
}

int spsc_queue_push(struct spsc_queue *q, void *ptr)
{
    size_t head;
    size_t tail;

    if ((q == NULL) || (q->buffer == NULL)) {
        return -EINVAL;
    }

    head = _spsc_head_load_relaxed(q);
    tail = _spsc_tail_load_acquire(q);

    if ((head - tail) == q->capacity) {
        return -ENOSPC;
    }

    q->buffer[_spsc_slot_index(q, head)] = ptr;
    _spsc_head_store_release(q, head + 1U);

    return 0;
}

int spsc_queue_pop(struct spsc_queue *q, void **out)
{
    size_t head;
    size_t tail;

    if (out != NULL) {
        *out = NULL;
    }

    if ((q == NULL) || (q->buffer == NULL) || (out == NULL)) {
        return -EINVAL;
    }

    tail = _spsc_tail_load_relaxed(q);
    head = _spsc_head_load_acquire(q);

    if (head == tail) {
        return -ENOENT;
    }

    *out = q->buffer[_spsc_slot_index(q, tail)];
    _spsc_tail_store_release(q, tail + 1U);

    return 0;
}

size_t spsc_queue_get_count(const struct spsc_queue *q)
{
    size_t head;
    size_t tail;

    if ((q == NULL) || (q->buffer == NULL)) {
        return 0U;
    }

    head = _spsc_head_load_acquire(q);
    tail = _spsc_tail_load_acquire(q);
    return head - tail;
}

bool spsc_queue_is_empty(const struct spsc_queue *q)
{
    size_t head;
    size_t tail;

    if ((q == NULL) || (q->buffer == NULL)) {
        return true;
    }

    head = _spsc_head_load_acquire(q);
    tail = _spsc_tail_load_acquire(q);
    return head == tail;
}

bool spsc_queue_is_full(const struct spsc_queue *q)
{
    size_t head;
    size_t tail;

    if ((q == NULL) || (q->buffer == NULL)) {
        return false;
    }

    head = _spsc_head_load_acquire(q);
    tail = _spsc_tail_load_acquire(q);
    return (head - tail) == q->capacity;
}

/* ---------------------------------------- end of file ----------------------------------------- */
