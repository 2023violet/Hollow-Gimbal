/**
 * @file    ring_buffer.c
 * @brief   Caller-owned single-producer single-consumer byte ring buffer
 * @version v4.0.0
 * @date    2026.05.01
 *
 * Implementation role
 * -------------------
 * - pairs with `ring_buffer.h`
 * - owns publication ordering and wrap-index optimisation details
 * - keeps the public surface POD while sealing atomic backend choices here
 *
 * Key collaborators
 * -----------------
 * - `ring_buffer.h`: caller-facing contract and POD control block
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "ring_buffer.h"
#include <string.h>

/* Private Macros ------------------------------------------------------------------------------- */

#define _RB_MIN(a, b) ((a) < (b) ? (a) : (b))

/* Private Helpers ------------------------------------------------------------------------------ */

static inline bool _ring_capacity_has_wrap_mask(size_t capacity)
{
    return (capacity != 0U) && ((capacity & (capacity - 1U)) == 0U);
}

static inline size_t _ring_index_of(const struct ring_buf *rb, size_t cursor)
{
    if (rb->wrap_mask != 0U) {
        return cursor & rb->wrap_mask;
    }

    return cursor % rb->capacity;
}

static inline size_t _ring_head_load_relaxed(const struct ring_buf *rb)
{
    return __atomic_load_n(&rb->head, __ATOMIC_RELAXED);
}

static inline size_t _ring_head_load_acquire(const struct ring_buf *rb)
{
    /*
     * The public control block stays POD for static initialisation and ABI
     * portability, but publication still needs real atomic semantics. The
     * compiler builtin operates directly on the aligned size_t field without
     * leaking wrapper types into the public header.
     */
    return __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
}

static inline void _ring_head_store_release(struct ring_buf *rb, size_t value)
{
    __atomic_store_n(&rb->head, value, __ATOMIC_RELEASE);
}

static inline size_t _ring_tail_load_relaxed(const struct ring_buf *rb)
{
    return __atomic_load_n(&rb->tail, __ATOMIC_RELAXED);
}

static inline size_t _ring_tail_load_acquire(const struct ring_buf *rb)
{
    return __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
}

static inline void _ring_tail_store_release(struct ring_buf *rb, size_t value)
{
    __atomic_store_n(&rb->tail, value, __ATOMIC_RELEASE);
}

static inline size_t _ring_used_bytes(const struct ring_buf *rb, size_t head, size_t tail)
{
    (void)rb;
    return head - tail;
}

static inline size_t _ring_free_bytes(const struct ring_buf *rb, size_t head, size_t tail)
{
    return rb->capacity - _ring_used_bytes(rb, head, tail);
}

static inline size_t _ring_write_window(const struct ring_buf *rb, size_t head, size_t tail)
{
    size_t space;
    size_t offset;
    size_t to_end;

    space = _ring_free_bytes(rb, head, tail);
    if (space == 0U) {
        return 0U;
    }

    offset = _ring_index_of(rb, head);
    to_end = rb->capacity - offset;
    return _RB_MIN(space, to_end);
}

static inline size_t _ring_read_window(const struct ring_buf *rb, size_t head, size_t tail)
{
    size_t used;
    size_t offset;
    size_t to_end;

    used = _ring_used_bytes(rb, head, tail);
    if (used == 0U) {
        return 0U;
    }

    offset = _ring_index_of(rb, tail);
    to_end = rb->capacity - offset;
    return _RB_MIN(used, to_end);
}

static inline ssize_t _ring_validate_copy_args(const struct ring_buf *rb,
                                               const void            *buffer,
                                               size_t                 len)
{
    if ((rb == NULL) || ((buffer == NULL) && (len > 0U))) {
        return -EINVAL;
    }

    if (len == 0U) {
        return 0;
    }

    if ((rb->buffer == NULL) || (rb->capacity == 0U)) {
        return -EINVAL;
    }

    return 1;
}

static inline ssize_t _ring_validate_ready(const struct ring_buf *rb)
{
    if ((rb == NULL) || (rb->buffer == NULL) || (rb->capacity == 0U)) {
        return -EINVAL;
    }

    return 0;
}

static inline void _ring_copy_in(struct ring_buf *rb, size_t start, const uint8_t *data, size_t len)
{
    size_t first_chunk;
    size_t to_end;

    to_end      = rb->capacity - start;
    first_chunk = _RB_MIN(len, to_end);

    memcpy(&rb->buffer[start], data, first_chunk);
    if (len > first_chunk) {
        memcpy(rb->buffer, &data[first_chunk], len - first_chunk);
    }
}

static inline void _ring_copy_out(const struct ring_buf *rb, size_t start, uint8_t *data, size_t len)
{
    size_t first_chunk;
    size_t to_end;

    to_end      = rb->capacity - start;
    first_chunk = _RB_MIN(len, to_end);

    memcpy(data, &rb->buffer[start], first_chunk);
    if (len > first_chunk) {
        memcpy(&data[first_chunk], rb->buffer, len - first_chunk);
    }
}

static inline void _ring_clear_reservations(struct ring_buf *rb)
{
    rb->write_reserve_len = 0U;
    rb->read_reserve_len  = 0U;
}

/* Public Functions ----------------------------------------------------------------------------- */

int ring_buf_init(struct ring_buf *rb, uint8_t *pool, size_t capacity)
{
    if ((rb == NULL) || (pool == NULL) || (capacity == 0U)) {
        return -EINVAL;
    }

    rb->buffer            = pool;
    rb->capacity          = capacity;
    rb->wrap_mask         = _ring_capacity_has_wrap_mask(capacity) ? (capacity - 1U) : 0U;
    rb->head              = 0U;
    rb->tail              = 0U;
    rb->write_reserve_len = 0U;
    rb->read_reserve_len  = 0U;

    return 0;
}

int ring_buf_reset(struct ring_buf *rb)
{
    if (rb == NULL) {
        return -EINVAL;
    }

    if ((rb->buffer == NULL) || (rb->capacity == 0U)) {
        return -EINVAL;
    }

    _ring_head_store_release(rb, 0U);
    _ring_tail_store_release(rb, 0U);
    _ring_clear_reservations(rb);
    return 0;
}

size_t ring_buf_data_len(const struct ring_buf *rb)
{
    size_t head;
    size_t tail;

    if ((rb == NULL) || (rb->buffer == NULL) || (rb->capacity == 0U)) {
        return 0U;
    }

    head = _ring_head_load_acquire(rb);
    tail = _ring_tail_load_acquire(rb);
    return _ring_used_bytes(rb, head, tail);
}

size_t ring_buf_space_len(const struct ring_buf *rb)
{
    size_t head;
    size_t tail;

    if ((rb == NULL) || (rb->buffer == NULL) || (rb->capacity == 0U)) {
        return 0U;
    }

    head = _ring_head_load_acquire(rb);
    tail = _ring_tail_load_acquire(rb);
    return _ring_free_bytes(rb, head, tail);
}

bool ring_buf_is_empty(const struct ring_buf *rb)
{
    return ring_buf_data_len(rb) == 0U;
}

bool ring_buf_is_full(const struct ring_buf *rb)
{
    return (rb != NULL) && (ring_buf_space_len(rb) == 0U);
}

int ring_buf_put_char(struct ring_buf *rb, uint8_t ch)
{
    size_t head;
    size_t tail;

    if (_ring_validate_ready(rb) != 0) {
        return -EINVAL;
    }

    if (rb->write_reserve_len != 0U) {
        return -EBUSY;
    }

    head = _ring_head_load_relaxed(rb);
    tail = _ring_tail_load_acquire(rb);

    if (_ring_free_bytes(rb, head, tail) == 0U) {
        return -ENOSPC;
    }

    rb->buffer[_ring_index_of(rb, head)] = ch;
    _ring_head_store_release(rb, head + 1U);
    return 0;
}

ssize_t ring_buf_put(struct ring_buf *rb, const uint8_t *data, size_t len)
{
    size_t head;
    size_t tail;
    size_t put_len;
    size_t start;
    ssize_t valid;

    valid = _ring_validate_copy_args(rb, data, len);
    if (valid <= 0) {
        return valid;
    }

    if (rb->write_reserve_len != 0U) {
        return -EBUSY;
    }

    head    = _ring_head_load_relaxed(rb);
    tail    = _ring_tail_load_acquire(rb);
    put_len = _RB_MIN(len, _ring_free_bytes(rb, head, tail));

    if (put_len == 0U) {
        return 0;
    }

    start = _ring_index_of(rb, head);
    _ring_copy_in(rb, start, data, put_len);
    _ring_head_store_release(rb, head + put_len);
    return (ssize_t)put_len;
}

ssize_t ring_buf_put_force(struct ring_buf *rb, const uint8_t *data, size_t len)
{
    size_t copy_len;
    size_t head;
    size_t new_head;
    size_t new_tail;
    size_t start;
    ssize_t valid;

    valid = _ring_validate_copy_args(rb, data, len);
    if (valid <= 0) {
        return valid;
    }

    if ((rb->write_reserve_len != 0U) || (rb->read_reserve_len != 0U)) {
        /*
         * Force-write only proceeds when no zero-copy reservation is active.
         * That keeps the caller from silently mixing DMA-style reservations
         * with destructive overwrite semantics.
         */
        return -EBUSY;
    }

    copy_len = _RB_MIN(len, rb->capacity);
    data += (len - copy_len);

    head    = _ring_head_load_relaxed(rb);
    new_head = head + copy_len;
    new_tail = _ring_tail_load_relaxed(rb);
    if (_ring_used_bytes(rb, new_head, new_tail) > rb->capacity) {
        new_tail = new_head - rb->capacity;
    }

    /*
     * Overwrite mode preserves the newest bytes by trimming the oldest tail
     * first. That keeps the ring's retention policy explicit instead of
     * pretending a full overwrite can be lossless.
     */
    start = _ring_index_of(rb, head);
    _ring_copy_in(rb, start, data, copy_len);

    _ring_tail_store_release(rb, new_tail);
    _ring_head_store_release(rb, new_head);
    return (ssize_t)copy_len;
}

ssize_t ring_buf_write_rollback(struct ring_buf *rb, size_t len)
{
    size_t head;
    size_t tail;
    size_t rollback_len;

    if (_ring_validate_ready(rb) != 0) {
        return -EINVAL;
    }

    if ((rb->write_reserve_len != 0U) || (len == 0U)) {
        return (len == 0U) ? 0 : -EBUSY;
    }

    head         = _ring_head_load_relaxed(rb);
    tail         = _ring_tail_load_relaxed(rb);
    rollback_len = _RB_MIN(len, _ring_used_bytes(rb, head, tail));

    if (rollback_len == 0U) {
        return 0;
    }

    _ring_head_store_release(rb, head - rollback_len);
    return (ssize_t)rollback_len;
}

int ring_buf_get_char(struct ring_buf *rb, uint8_t *ch)
{
    size_t head;
    size_t tail;

    if ((_ring_validate_ready(rb) != 0) || (ch == NULL)) {
        return -EINVAL;
    }

    if (rb->read_reserve_len != 0U) {
        return -EBUSY;
    }

    tail = _ring_tail_load_relaxed(rb);
    head = _ring_head_load_acquire(rb);

    if (_ring_used_bytes(rb, head, tail) == 0U) {
        return -ENODATA;
    }

    *ch = rb->buffer[_ring_index_of(rb, tail)];
    _ring_tail_store_release(rb, tail + 1U);
    return 0;
}

ssize_t ring_buf_get(struct ring_buf *rb, uint8_t *data, size_t len)
{
    size_t head;
    size_t tail;
    size_t get_len;
    size_t start;
    ssize_t valid;

    valid = _ring_validate_copy_args(rb, data, len);
    if (valid <= 0) {
        return valid;
    }

    if (rb->read_reserve_len != 0U) {
        return -EBUSY;
    }

    tail    = _ring_tail_load_relaxed(rb);
    head    = _ring_head_load_acquire(rb);
    get_len = _RB_MIN(len, _ring_used_bytes(rb, head, tail));

    if (get_len == 0U) {
        return 0;
    }

    start = _ring_index_of(rb, tail);
    _ring_copy_out(rb, start, data, get_len);
    _ring_tail_store_release(rb, tail + get_len);
    return (ssize_t)get_len;
}

int ring_buf_peek_char(const struct ring_buf *rb, size_t offset, uint8_t *ch)
{
    size_t head;
    size_t tail;
    size_t used;

    if ((_ring_validate_ready(rb) != 0) || (ch == NULL)) {
        return -EINVAL;
    }

    tail = _ring_tail_load_acquire(rb);
    head = _ring_head_load_acquire(rb);
    used = _ring_used_bytes(rb, head, tail);

    if (offset >= used) {
        return -ERANGE;
    }

    *ch = rb->buffer[_ring_index_of(rb, tail + offset)];
    return 0;
}

ssize_t ring_buf_peek(const struct ring_buf *rb, size_t offset, uint8_t *data, size_t len)
{
    size_t head;
    size_t tail;
    size_t used;
    size_t peek_len;
    size_t start;
    ssize_t valid;

    valid = _ring_validate_copy_args(rb, data, len);
    if (valid <= 0) {
        return valid;
    }

    tail = _ring_tail_load_acquire(rb);
    head = _ring_head_load_acquire(rb);
    used = _ring_used_bytes(rb, head, tail);

    if (offset >= used) {
        return 0;
    }

    peek_len = _RB_MIN(len, used - offset);
    start    = _ring_index_of(rb, tail + offset);
    _ring_copy_out(rb, start, data, peek_len);
    return (ssize_t)peek_len;
}

int ring_buf_find_byte(const struct ring_buf *rb, uint8_t byte, size_t *pos)
{
    size_t head;
    size_t tail;
    size_t used;
    size_t i;

    if ((_ring_validate_ready(rb) != 0) || (pos == NULL)) {
        return -EINVAL;
    }

    tail = _ring_tail_load_acquire(rb);
    head = _ring_head_load_acquire(rb);
    used = _ring_used_bytes(rb, head, tail);

    for (i = 0U; i < used; ++i) {
        if (rb->buffer[_ring_index_of(rb, tail + i)] == byte) {
            *pos = i;
            return 0;
        }
    }

    return -ENOENT;
}

ssize_t ring_buf_drop(struct ring_buf *rb, size_t len)
{
    size_t head;
    size_t tail;
    size_t drop_len;

    if (_ring_validate_ready(rb) != 0) {
        return -EINVAL;
    }

    if (rb->read_reserve_len != 0U) {
        return -EBUSY;
    }

    if (len == 0U) {
        return 0;
    }

    tail     = _ring_tail_load_relaxed(rb);
    head     = _ring_head_load_acquire(rb);
    drop_len = _RB_MIN(len, _ring_used_bytes(rb, head, tail));

    if (drop_len == 0U) {
        return 0;
    }

    _ring_tail_store_release(rb, tail + drop_len);
    return (ssize_t)drop_len;
}

uint8_t *ring_buf_get_write_ptr(struct ring_buf *rb, size_t *size)
{
    size_t head;
    size_t tail;
    size_t window;

    if ((_ring_validate_ready(rb) != 0) || (size == NULL)) {
        return NULL;
    }

    if (rb->write_reserve_len != 0U) {
        *size = rb->write_reserve_len;
        return &rb->buffer[_ring_index_of(rb, rb->head)];
    }

    head   = _ring_head_load_relaxed(rb);
    tail   = _ring_tail_load_acquire(rb);
    window = _ring_write_window(rb, head, tail);

    rb->write_reserve_len = window;
    *size                 = window;
    if (window == 0U) {
        return NULL;
    }

    return &rb->buffer[_ring_index_of(rb, head)];
}

int ring_buf_write_advance(struct ring_buf *rb, size_t len)
{
    size_t head;
    size_t tail;
    size_t window;

    if (_ring_validate_ready(rb) != 0) {
        return -EINVAL;
    }

    head = _ring_head_load_relaxed(rb);
    tail = _ring_tail_load_acquire(rb);

    if (rb->write_reserve_len != 0U) {
        if (len > rb->write_reserve_len) {
            return -ERANGE;
        }

        if (len == 0U) {
            rb->write_reserve_len = 0U;
            return 0;
        }

        rb->write_reserve_len = 0U;
        _ring_head_store_release(rb, head + len);
        return 0;
    }

    window = _ring_write_window(rb, head, tail);
    if (len > window) {
        return -ERANGE;
    }

    if (len == 0U) {
        return 0;
    }

    _ring_head_store_release(rb, head + len);
    return 0;
}

uint8_t *ring_buf_get_read_ptr(struct ring_buf *rb, size_t *size)
{
    size_t head;
    size_t tail;
    size_t window;

    if ((_ring_validate_ready(rb) != 0) || (size == NULL)) {
        return NULL;
    }

    if (rb->read_reserve_len != 0U) {
        *size = rb->read_reserve_len;
        return (rb->read_reserve_len == 0U) ? NULL : &rb->buffer[_ring_index_of(rb, rb->tail)];
    }

    tail   = _ring_tail_load_relaxed(rb);
    head   = _ring_head_load_acquire(rb);
    window = _ring_read_window(rb, head, tail);

    rb->read_reserve_len = window;
    *size                = window;
    if (window == 0U) {
        return NULL;
    }

    return &rb->buffer[_ring_index_of(rb, tail)];
}

int ring_buf_read_advance(struct ring_buf *rb, size_t len)
{
    size_t head;
    size_t tail;
    size_t window;

    if (_ring_validate_ready(rb) != 0) {
        return -EINVAL;
    }

    tail = _ring_tail_load_relaxed(rb);
    head = _ring_head_load_acquire(rb);

    if (rb->read_reserve_len != 0U) {
        if (len > rb->read_reserve_len) {
            return -ERANGE;
        }

        if (len == 0U) {
            rb->read_reserve_len = 0U;
            return 0;
        }

        rb->read_reserve_len = 0U;
        _ring_tail_store_release(rb, tail + len);
        return 0;
    }

    window = _ring_read_window(rb, head, tail);
    if (len > window) {
        return -ERANGE;
    }

    if (len == 0U) {
        return 0;
    }

    _ring_tail_store_release(rb, tail + len);
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
