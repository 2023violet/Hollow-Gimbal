/**
 * @file    atomic.h
 * @brief   Portable atomic primitives for Themis utils components
 * @version v1.0.0
 * @date    2026.04.04
 *
 * Copyright (c) 2026 Zhang Jiaming <164293474@qq.com>. All rights reserved.
 *
 * ============================================================================
 * Design philosophy: a tiny portability layer, not a synchronization framework
 * ============================================================================
 *
 * utils components occasionally need exactly three atomic capabilities:
 *   - publish a pointer with release ordering;
 *   - observe a counter or pointer with acquire ordering;
 *   - issue an explicit fence when data and publication happen separately.
 *
 * This header centralises those primitives so utils code does not scatter
 * direct <stdatomic.h> usage throughout the tree.  The goal is portability and
 * contract clarity, not to recreate an RTOS synchronisation subsystem.
 *
 * Consequences:
 *   - No RTOS, BSP, vendor-register, or board-level dependency.
 *   - No blocking, no heap allocation, and no hidden global state.
 *   - Only a very small set of types is exposed: u32, size_t, and pointer.
 *
 * ============================================================================
 * Portability contract
 * ============================================================================
 *
 * The implementation prefers C11 atomics when the toolchain supports them.
 * If <stdatomic.h> is unavailable, it falls back to GCC/Clang __atomic
 * builtins.  The public API stays the same in either case.
 *
 * This header guarantees atomic semantics for the wrapped object itself.  It
 * does NOT guarantee lock-free progress on every target; some toolchains may
 * lower certain operations to helper routines.  Callers that need ISR use on a
 * specific platform must confirm the target's pointer-width atomic support.
 *
 * Cortex-M note
 * -------------
 * On Cortex-M profiles that implement read-modify-write atomics through
 * LDREX/STREX, aligned word-sized load/store remains naturally atomic, but
 * fetch_add / exchange / compare_exchange still rely on the exclusive-monitor
 * retry protocol.  Those read-modify-write operations are therefore a task-
 * context primitive by default.  ISR paths should stay with plain aligned
 * load/store or use an explicit critical section around the shared state.
 *
 * ============================================================================
 * Ownership and concurrency contract
 * ============================================================================
 *
 * Atomic variables are caller-owned objects.  This header does not manage
 * lifetime, buffering, wakeups, retries, or fairness.  Higher-level protocols
 * such as queues, reference counters, or publish/consume handshakes must still
 * be defined by the calling component.
 *
 * Typical rules:
 *   - A store_release() publishes already-written data.
 *   - A load_acquire() consumes data after publication.
 *   - Relaxed operations are only for independent counters or internal book-
 *     keeping where ordering does not matter.
 *
 * ============================================================================
 * Minimal usage example
 * ============================================================================
 *
 * @code
 * static tm_atomic_ptr_t  g_head    = TM_ATOMIC_PTR_INIT(NULL);
 * static tm_atomic_u32_t  g_counter = TM_ATOMIC_U32_INIT(0U);
 *
 * void publish(void *node)
 * {
 *     tm_atomic_ptr_store_release(&g_head, node);
 *     (void)tm_atomic_u32_fetch_add_relaxed(&g_counter, 1U);
 * }
 *
 * void *consume(void)
 * {
 *     return tm_atomic_ptr_load_acquire(&g_head);
 * }
 * @endcode
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public configuration ------------------------------------------------------------------------- */

/**
 * @brief  Implementation selector for this header.
 *
 * Value 1 means the toolchain provides <stdatomic.h>.  Value 0 means the API
 * is implemented through compiler __atomic builtins instead.
 */
#if !defined(__cplusplus) && defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L) && \
    !defined(__STDC_NO_ATOMICS__)
#define TM_ATOMIC_USE_STDATOMIC 1
#include <stdatomic.h>
#else
#define TM_ATOMIC_USE_STDATOMIC 0
#endif

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief  Atomic 32-bit unsigned integer.
 *
 * Caller-owned storage for counters, flags, and sequence numbers.  The type
 * itself carries no ownership or wakeup semantics.
 */
#if TM_ATOMIC_USE_STDATOMIC
typedef struct {
    _Atomic uint32_t value;
} tm_atomic_u32_t;
#else
typedef struct {
    uint32_t value;
} tm_atomic_u32_t;
#endif

/**
 * @brief  Atomic size_t value.
 *
 * Useful for capacities, indices, and sizes whose natural type is size_t.
 */
#if TM_ATOMIC_USE_STDATOMIC
typedef struct {
    _Atomic size_t value;
} tm_atomic_size_t;
#else
typedef struct {
    size_t value;
} tm_atomic_size_t;
#endif

/**
 * @brief  Atomic uintptr_t value.
 *
 * Low-level carrier used to implement pointer publication without exposing
 * compiler-specific atomic syntax to other utils components.
 */
#if TM_ATOMIC_USE_STDATOMIC
typedef struct {
    _Atomic uintptr_t value;
} tm_atomic_uptr_t;
#else
typedef struct {
    uintptr_t value;
} tm_atomic_uptr_t;
#endif

/**
 * @brief  Atomic pointer carrier.
 *
 * Semantically equivalent to an atomic void * stored via uintptr_t.
 */
typedef tm_atomic_uptr_t tm_atomic_ptr_t;

/* Public macros -------------------------------------------------------------------------------- */

/**
 * @brief  Static initializer for tm_atomic_u32_t.
 *
 * Intended for file-scope or block-scope declaration initialisers.
 */
#define TM_ATOMIC_U32_INIT(v)  {(uint32_t)(v)}

/**
 * @brief  Static initializer for tm_atomic_size_t.
 *
 * Intended for file-scope or block-scope declaration initialisers.
 */
#define TM_ATOMIC_SIZE_INIT(v) {(size_t)(v)}

/**
 * @brief  Static initializer for tm_atomic_ptr_t.
 *
 * The pointer is stored as uintptr_t and may be NULL.
 */
#define TM_ATOMIC_PTR_INIT(v)  {(uintptr_t)(v)}

/* Private inline helpers ---------------------------------------------------------------------- */

#if !TM_ATOMIC_USE_STDATOMIC
static inline uint32_t _tm_atomic_builtin_load_u32(const uint32_t *value, int memory_order)
{
    uint32_t loaded = 0U;

    __atomic_load(value, &loaded, memory_order);
    return loaded;
}

static inline size_t _tm_atomic_builtin_load_size(const size_t *value, int memory_order)
{
    size_t loaded = 0U;

    __atomic_load(value, &loaded, memory_order);
    return loaded;
}

static inline uintptr_t _tm_atomic_builtin_load_uptr(const uintptr_t *value, int memory_order)
{
    uintptr_t loaded = (uintptr_t)0U;

    __atomic_load(value, &loaded, memory_order);
    return loaded;
}
#endif

/* Public inline functions ---------------------------------------------------------------------- */

/**
 * @brief  Issue an acquire fence.
 *
 * Prevents later loads and stores in the current execution context from moving
 * before the fence.  Use when consuming data that another execution context
 * published with a matching release operation.
 */
static inline void tm_atomic_thread_fence_acquire(void)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_thread_fence(memory_order_acquire);
#else
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief  Issue a release fence.
 *
 * Prevents earlier loads and stores in the current execution context from
 * moving after the fence.  Use before publishing availability metadata.
 */
static inline void tm_atomic_thread_fence_release(void)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_thread_fence(memory_order_release);
#else
    __atomic_thread_fence(__ATOMIC_RELEASE);
#endif
}

/**
 * @brief  Issue a sequentially consistent fence.
 *
 * This is the strongest ordering primitive provided by this header.  Use it
 * only when acquire/release is insufficient, because it may be more expensive
 * on some targets.
 */
static inline void tm_atomic_thread_fence_seq_cst(void)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_thread_fence(memory_order_seq_cst);
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

/**
 * @brief  Initialise an atomic u32 object.
 *
 * @param  atom   Atomic object to initialise.  Must not be NULL.
 * @param  value  Initial value stored in the object.
 */
static inline void tm_atomic_u32_init(tm_atomic_u32_t *atom, uint32_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_init(&atom->value, value);
#else
    __atomic_store_n(&atom->value, value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Load an atomic u32 with relaxed ordering.
 *
 * Use only when the load does not participate in a publication/consumption
 * protocol.
 *
 * @param  atom  Atomic object to read.  Must not be NULL.
 *
 * @return Current value of the atomic object.
 */
static inline uint32_t tm_atomic_u32_load_relaxed(const tm_atomic_u32_t *atom)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_load_explicit(&atom->value, memory_order_relaxed);
#else
    return _tm_atomic_builtin_load_u32(&atom->value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Load an atomic u32 with acquire ordering.
 *
 * @param  atom  Atomic object to read.  Must not be NULL.
 *
 * @return Current value of the atomic object, observed with acquire semantics.
 */
static inline uint32_t tm_atomic_u32_load_acquire(const tm_atomic_u32_t *atom)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_load_explicit(&atom->value, memory_order_acquire);
#else
    return _tm_atomic_builtin_load_u32(&atom->value, __ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief  Store to an atomic u32 with relaxed ordering.
 *
 * Use only for independent counters or bookkeeping that do not participate
 * in a release/acquire publication protocol.
 *
 * @param  atom   Atomic object to write.  Must not be NULL.
 * @param  value  Value to store.
 */
static inline void tm_atomic_u32_store_relaxed(tm_atomic_u32_t *atom, uint32_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_store_explicit(&atom->value, value, memory_order_relaxed);
#else
    __atomic_store_n(&atom->value, value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Store to an atomic u32 with release ordering.
 *
 * @param  atom   Atomic object to write.  Must not be NULL.
 * @param  value  Value to publish.
 */
static inline void tm_atomic_u32_store_release(tm_atomic_u32_t *atom, uint32_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_store_explicit(&atom->value, value, memory_order_release);
#else
    __atomic_store_n(&atom->value, value, __ATOMIC_RELEASE);
#endif
}

/**
 * @brief  Atomically add to a u32 and return the previous value.
 *
 * @param  atom   Atomic counter to update.  Must not be NULL.
 * @param  value  Increment amount.
 *
 * @return Value held in @p atom before the addition.
 */
static inline uint32_t tm_atomic_u32_fetch_add_relaxed(tm_atomic_u32_t *atom, uint32_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_fetch_add_explicit(&atom->value, value, memory_order_relaxed);
#else
    return __atomic_fetch_add(&atom->value, value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Atomically add to a u32 with acquire-release ordering.
 *
 * @param  atom   Atomic counter to update.  Must not be NULL.
 * @param  value  Increment amount.
 *
 * @return Value held in @p atom before the addition.
 */
static inline uint32_t tm_atomic_u32_fetch_add_acq_rel(tm_atomic_u32_t *atom, uint32_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_fetch_add_explicit(&atom->value, value, memory_order_acq_rel);
#else
    return __atomic_fetch_add(&atom->value, value, __ATOMIC_ACQ_REL);
#endif
}

/**
 * @brief  Atomically replace a u32 and return the previous value.
 *
 * @param  atom   Atomic object to exchange.  Must not be NULL.
 * @param  value  New value written to the atomic object.
 *
 * @return Previous value of the atomic object.
 */
static inline uint32_t tm_atomic_u32_exchange_acq_rel(tm_atomic_u32_t *atom, uint32_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_exchange_explicit(&atom->value, value, memory_order_acq_rel);
#else
    return __atomic_exchange_n(&atom->value, value, __ATOMIC_ACQ_REL);
#endif
}

/**
 * @brief  Compare an atomic u32 with *expected and exchange on match.
 *
 * @param  atom      Atomic object to update.  Must not be NULL.
 * @param  expected  In: expected value.  Out: observed value when the compare-
 *                   exchange fails.  Must not be NULL.
 * @param  desired   Replacement value written on success.
 *
 * @return `true` when the exchange succeeded and @p desired was written;
 *         otherwise `false` after updating @p *expected to the observed
 *         value.
 */
static inline bool tm_atomic_u32_compare_exchange_acq_rel(tm_atomic_u32_t *atom,
                                                          uint32_t        *expected,
                                                          uint32_t         desired)
{
    bool rc;

    if (expected == NULL) {
        return false;
    }

#if TM_ATOMIC_USE_STDATOMIC
    rc = atomic_compare_exchange_strong_explicit(&atom->value,
                                                 expected,
                                                 desired,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire);
#else
    rc = __atomic_compare_exchange_n(&atom->value,
                                     expected,
                                     desired,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
#endif

    return rc;
}

/**
 * @brief  Initialise an atomic size_t object.
 *
 * @param  atom   Atomic object to initialise.  Must not be NULL.
 * @param  value  Initial value stored in the object.
 */
static inline void tm_atomic_size_init(tm_atomic_size_t *atom, size_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_init(&atom->value, value);
#else
    __atomic_store_n(&atom->value, value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Load an atomic size_t with relaxed ordering.
 *
 * @param  atom  Atomic object to read.  Must not be NULL.
 *
 * @return Current size_t value.
 */
static inline size_t tm_atomic_size_load_relaxed(const tm_atomic_size_t *atom)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_load_explicit(&atom->value, memory_order_relaxed);
#else
    return _tm_atomic_builtin_load_size(&atom->value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Load an atomic size_t with acquire ordering.
 *
 * @param  atom  Atomic object to read.  Must not be NULL.
 *
 * @return Current size_t value observed with acquire semantics.
 */
static inline size_t tm_atomic_size_load_acquire(const tm_atomic_size_t *atom)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_load_explicit(&atom->value, memory_order_acquire);
#else
    return _tm_atomic_builtin_load_size(&atom->value, __ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief  Store to an atomic size_t with release ordering.
 *
 * @param  atom   Atomic object to write.  Must not be NULL.
 * @param  value  Value to publish.
 */
static inline void tm_atomic_size_store_release(tm_atomic_size_t *atom, size_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_store_explicit(&atom->value, value, memory_order_release);
#else
    __atomic_store_n(&atom->value, value, __ATOMIC_RELEASE);
#endif
}

/**
 * @brief  Atomically add to a size_t and return the previous value.
 *
 * @param  atom   Atomic counter to update.  Must not be NULL.
 * @param  value  Increment amount.
 *
 * @return Value held in @p atom before the addition.
 */
static inline size_t tm_atomic_size_fetch_add_relaxed(tm_atomic_size_t *atom, size_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_fetch_add_explicit(&atom->value, value, memory_order_relaxed);
#else
    return __atomic_fetch_add(&atom->value, value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Atomically add to a size_t with acquire-release ordering.
 *
 * @param  atom   Atomic counter to update.  Must not be NULL.
 * @param  value  Increment amount.
 *
 * @return Value held in @p atom before the addition.
 */
static inline size_t tm_atomic_size_fetch_add_acq_rel(tm_atomic_size_t *atom, size_t value)
{
#if TM_ATOMIC_USE_STDATOMIC
    return atomic_fetch_add_explicit(&atom->value, value, memory_order_acq_rel);
#else
    return __atomic_fetch_add(&atom->value, value, __ATOMIC_ACQ_REL);
#endif
}

/**
 * @brief  Initialise an atomic pointer object.
 *
 * @param  atom   Atomic pointer to initialise.  Must not be NULL.
 * @param  value  Initial pointer value.  May be NULL.
 */
static inline void tm_atomic_ptr_init(tm_atomic_ptr_t *atom, void *value)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_init(&atom->value, (uintptr_t)value);
#else
    __atomic_store_n(&atom->value, (uintptr_t)value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Load an atomic pointer with relaxed ordering.
 *
 * @param  atom  Atomic pointer to read.  Must not be NULL.
 *
 * @return Current pointer value, possibly NULL.
 */
static inline void *tm_atomic_ptr_load_relaxed(const tm_atomic_ptr_t *atom)
{
#if TM_ATOMIC_USE_STDATOMIC
    return (void *)atomic_load_explicit(&atom->value, memory_order_relaxed);
#else
    return (void *)_tm_atomic_builtin_load_uptr(&atom->value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Load an atomic pointer with acquire ordering.
 *
 * @param  atom  Atomic pointer to read.  Must not be NULL.
 *
 * @return Current pointer value, observed with acquire semantics.
 */
static inline void *tm_atomic_ptr_load_acquire(const tm_atomic_ptr_t *atom)
{
#if TM_ATOMIC_USE_STDATOMIC
    return (void *)atomic_load_explicit(&atom->value, memory_order_acquire);
#else
    return (void *)_tm_atomic_builtin_load_uptr(&atom->value, __ATOMIC_ACQUIRE);
#endif
}

/**
 * @brief  Store to an atomic pointer with relaxed ordering.
 *
 * @param  atom   Atomic pointer to write.  Must not be NULL.
 * @param  value  Pointer value to store.  May be NULL.
 */
static inline void tm_atomic_ptr_store_relaxed(tm_atomic_ptr_t *atom, void *value)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_store_explicit(&atom->value, (uintptr_t)value, memory_order_relaxed);
#else
    __atomic_store_n(&atom->value, (uintptr_t)value, __ATOMIC_RELAXED);
#endif
}

/**
 * @brief  Store to an atomic pointer with release ordering.
 *
 * @param  atom   Atomic pointer to write.  Must not be NULL.
 * @param  value  Pointer value to publish.  May be NULL.
 */
static inline void tm_atomic_ptr_store_release(tm_atomic_ptr_t *atom, void *value)
{
#if TM_ATOMIC_USE_STDATOMIC
    atomic_store_explicit(&atom->value, (uintptr_t)value, memory_order_release);
#else
    __atomic_store_n(&atom->value, (uintptr_t)value, __ATOMIC_RELEASE);
#endif
}

/**
 * @brief  Atomically replace a pointer and return the previous value.
 *
 * @param  atom   Atomic pointer to exchange.  Must not be NULL.
 * @param  value  New pointer value.  May be NULL.
 *
 * @return Previous pointer value.
 */
static inline void *tm_atomic_ptr_exchange_acq_rel(tm_atomic_ptr_t *atom, void *value)
{
#if TM_ATOMIC_USE_STDATOMIC
    return (void *)atomic_exchange_explicit(&atom->value, (uintptr_t)value, memory_order_acq_rel);
#else
    return (void *)__atomic_exchange_n(&atom->value, (uintptr_t)value, __ATOMIC_ACQ_REL);
#endif
}

/**
 * @brief  Compare an atomic pointer with *expected and exchange on match.
 *
 * @param  atom      Atomic pointer to update.  Must not be NULL.
 * @param  expected  In: expected pointer value.  Out: actual pointer value if
 *                   the compare-exchange fails.  Must not be NULL.
 * @param  desired   Replacement pointer value written on success.  May be NULL.
 *
 * @return `true` when the exchange succeeded and @p desired was written;
 *         otherwise `false` after updating @p *expected to the observed
 *         value.
 */
static inline bool tm_atomic_ptr_compare_exchange_acq_rel(tm_atomic_ptr_t *atom,
                                                          void           **expected,
                                                          void            *desired)
{
    uintptr_t expected_value;
    bool      rc;

    if (expected == NULL) {
        return false;
    }

    expected_value = (uintptr_t)(*expected);

#if TM_ATOMIC_USE_STDATOMIC
    rc = atomic_compare_exchange_strong_explicit(&atom->value,
                                                 &expected_value,
                                                 (uintptr_t)desired,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire);
#else
    rc = __atomic_compare_exchange_n(&atom->value,
                                     &expected_value,
                                     (uintptr_t)desired,
                                     false,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE);
#endif

    *expected = (void *)expected_value;
    return rc;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
