/**
 * @file    tm_ipc.c
 * @brief   Themis IPC v7.0 - Implementation
 * @version v7.0.0
 * @date    2026.03.01
 *
 * Design notes (implementation perspective)
 * ------------------------------------------
 *
 * Memory model (unchanged from v6.0)
 *   Four statically-allocated tier buffers form the slab pools. Any tier
 *   count may be configured to 0, in which case the buffer collapses to a
 *   1-byte placeholder and allocation falls through to the next larger enabled
 *   tier. Each live block is a fixed-size struct containing an ipc_env_hdr
 *   followed by a payload array. The payload field is always 8-byte aligned
 *   (enforced by static_assert).
 *
 * Direct pool pointer in envelope header (v7.0 change)
 *   v6.0 stored payload_cap in the header and re-derived the pool pointer via
 *   _pool_select() on every free() call (an if-else chain traversal).
 *   v7.0 stores struct ipc_slab_pool *pool directly.  _pool_free() is now a
 *   single pointer dereference - O(1) with no branching.
 *   sizeof(ipc_env_hdr) is unchanged at 24 bytes: _pad was replaced by pool.
 *
 * Single-pass PUB_SUB delivery (v7.0 change)
 *   v6.0 traversed the subscriber list twice: once to count subscribers (to
 *   set env->ref_count), then again to deliver.  For a topic with N subs this
 *   cost 2N list node accesses and one extra critical section.
 *
 *   v7.0 uses the addref-before-put pattern:
 *     1. Set env->ref_count = 1  (caller holds one reference during the loop)
 *     2. For each subscriber:
 *          a. Increment ref_count (+1) inside critical section
 *          b. Attempt queue put (timeout=0)
 *          c. If put fails: _env_release() (decrements, may not reach 0)
 *     3. After the loop: _env_release() releases the caller's hold
 *   Correctness proof:
 *     - ref_count >= 1 throughout the loop (step 1) -> block is never freed
 *       prematurely even if a fast subscriber releases concurrently.
 *     - Each successful put corresponds to exactly one addref, which will be
 *       balanced by the subscriber's tm_ipc_msg_release().
 *     - If all queue puts fail, the block is freed at step 3 (ref reaches 0).
 *   Cost: N critical sections (one addref per sub) vs. 1 CS + N list nodes in
 *   v6.0's count pass.  For the typical case of 1-3 subscribers the addref
 *   CS overhead is negligible and the second list traversal is eliminated.
 *
 * Deferred ring wrap is capacity-aware.
 *   Arbitrary positive depths are supported, and the implementation selects
 *   the cheapest correct wrap path for the configured depth.
 *
 * pub_count consistency (v7.0 fix)
 *   v6.0 incremented pub_count after releasing topic->mutex in the PUB_SUB
 *   path, creating a brief window where pub_count was stale.  v7.0 increments
 *   pub_count inside the mutex for both delivery modes.
 */

/* -- Includes --------------------------------------------------------------- */
#include "tm_ipc.h"
#include "tm_assert.h"
#include "tm_export.h"
#include "tm_log.h"
#include "tm_runtime.h"

#if defined(__CC_ARM) || defined(__CLANG_ARM) || defined(__GNUC__)
#include "cmsis_compiler.h" /* __get_IPSR */
#endif

#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

#if TM_IPC_ENABLE

LOG_TAG("tm_ipc");

/* ========================================================================== */
/* SECTION 1 - Private constants                                             */
/* ========================================================================== */

/** Magic sentinel stored in every envelope to detect corruption. */
#define IPC_ENV_MAGIC (0xA5C3D7F1U)

/** Sentinel written on free to detect double-release or use-after-free. */
#define IPC_ENV_DEAD  (0xDEADBEEFU)

/**
 * @brief Deferred-ring wrap helper for any positive depth.
 *
 * The implementation may use a mask fast path when it is correct; otherwise
 * it falls back to modulo wrap.
 */
#define _DEFER_MASK   ((uint32_t)(TM_IPC_DEFER_DEPTH) - 1U)

/* ========================================================================== */
/* SECTION 2 - Private types                                                 */
/* ========================================================================== */

/* Forward declaration required by ipc_env_hdr. */
struct ipc_slab_pool;

/**
 * @brief Envelope header prepended to every slab block.
 *
 * Layout on 32-bit ARM (all fields naturally aligned):
 *   offset  0 - free_node      (4 B)  slab free-list linkage
 *   offset  4 - magic          (4 B)  IPC_ENV_MAGIC / IPC_ENV_DEAD
 *   offset  8 - source_topic   (4 B)  back-pointer (for diagnostics)
 *   offset 12 - pool           (4 B)  direct pool pointer (v7.0: was _pad)
 *   offset 16 - ref_count      (4 B)  fanout reference counter
 *   offset 20 - payload_cap    (4 B)  payload capacity (diagnostics / assert)
 *   offset 24 - payload[]            8-byte aligned on 32-bit ARM
 *
 * v7.0 swap: _pad at offset 12 became pool, gaining O(1) free without
 * changing sizeof(ipc_env_hdr) = 24 B.
 */
struct ipc_env_hdr {
    sys_snode_t           free_node;    /* Slab free-list linkage                    */
    uint32_t              magic;        /* IPC_ENV_MAGIC or IPC_ENV_DEAD             */
    struct tm_ipc_topic  *source_topic; /* Back-pointer (diagnostics only)           */
    struct ipc_slab_pool *pool;         /* Direct pool pointer - O(1) free (v7.0)    */
    volatile int32_t      ref_count;    /* Fanout ref count; every RMW stays in IRQ CS*/
    uint32_t              payload_cap;  /* Payload bytes this block can hold         */
    /* payload data follows at offset 24, naturally 8-byte aligned                     */
};

/* Compile-time verification: header must be a multiple of 8 bytes. */
typedef char _assert_env_hdr_size[(sizeof(struct ipc_env_hdr) % 8U == 0U) ? 1 : -1];

/* Any positive deferred depth is supported; only zero is invalid. */
typedef char _assert_defer_depth_nonzero[(TM_IPC_DEFER_DEPTH > 0U) ? 1 : -1];

/**
 * @brief Typed slab block structures - one per tier.
 */
typedef struct {
    struct ipc_env_hdr hdr;
    uint8_t            payload[TM_IPC_SLAB_TINY_PAYLOAD];
} ipc_block_tiny_t;
typedef struct {
    struct ipc_env_hdr hdr;
    uint8_t            payload[TM_IPC_SLAB_SMALL_PAYLOAD];
} ipc_block_small_t;
typedef struct {
    struct ipc_env_hdr hdr;
    uint8_t            payload[TM_IPC_SLAB_MEDIUM_PAYLOAD];
} ipc_block_medium_t;
typedef struct {
    struct ipc_env_hdr hdr;
    uint8_t            payload[TM_IPC_SLAB_LARGE_PAYLOAD];
} ipc_block_large_t;

/**
 * @brief Slab pool descriptor (one per tier).
 */
struct ipc_slab_pool {
    sys_slist_t      free_list;   /* Free-list head                       */
    uint32_t         payload_cap; /* Max payload bytes for this tier      */
    uint32_t         total;       /* Total blocks allocated at init       */
    volatile int32_t avail;       /* Current free block count             */
};

/**
 * @brief One entry in the ISR-to-broker deferred ring.
 */
struct ipc_defer_entry {
    struct tm_ipc_topic *topic; /* Target topic pointer                   */
    void                *msg;   /* Payload pointer (envelope->payload)    */
};

/* ========================================================================== */
/* SECTION 3 - Static storage                                                */
/* ========================================================================== */

/* -- Slab block arrays --------------------------------------------------- */

/* TM_ALIGN(8): guarantees payload fields are 8-byte aligned.
 * Subsequent blocks in the array are automatically aligned because each
 * block struct size is a multiple of 8. */
#define _IPC_SLAB_STORAGE_BYTES(_block_type, _count) \
    (((_count) > 0U) ? (sizeof(_block_type) * (_count)) : 1U)

TM_ALIGN(8)
static uint8_t _slab_tiny[_IPC_SLAB_STORAGE_BYTES(ipc_block_tiny_t, TM_IPC_SLAB_TINY_CNT)];
TM_ALIGN(8)
static uint8_t _slab_small[_IPC_SLAB_STORAGE_BYTES(ipc_block_small_t, TM_IPC_SLAB_SMALL_CNT)];
TM_ALIGN(8)
static uint8_t _slab_medium[_IPC_SLAB_STORAGE_BYTES(ipc_block_medium_t, TM_IPC_SLAB_MEDIUM_CNT)];
TM_ALIGN(8)
static uint8_t _slab_large[_IPC_SLAB_STORAGE_BYTES(ipc_block_large_t, TM_IPC_SLAB_LARGE_CNT)];

static struct ipc_slab_pool _pool_tiny   = {.payload_cap = TM_IPC_SLAB_TINY_PAYLOAD};
static struct ipc_slab_pool _pool_small  = {.payload_cap = TM_IPC_SLAB_SMALL_PAYLOAD};
static struct ipc_slab_pool _pool_medium = {.payload_cap = TM_IPC_SLAB_MEDIUM_PAYLOAD};
static struct ipc_slab_pool _pool_large  = {.payload_cap = TM_IPC_SLAB_LARGE_PAYLOAD};

/* -- Deferred ISR-publish ring ------------------------------------------- */

/* volatile on indices: ISR writes _defer_write, broker reads it.
 * Compiler must not cache them in registers across function calls. */
static struct ipc_defer_entry _defer_ring[TM_IPC_DEFER_DEPTH];
static volatile uint32_t      _defer_write      = 0U; /* ISR producer index (bitmask wrapped) */
static volatile uint32_t      _defer_read       = 0U; /* Broker consumer index                */
static volatile uint32_t      _defer_high_water = 0U;
static volatile uint32_t      _defer_drop_count = 0U;
static volatile uint32_t      _broker_dispatch_count     = 0U;
static volatile uint32_t      _broker_error_count        = 0U;
static volatile uint32_t      _broker_last_progress_tick = 0U;

/* Counting semaphore: one token per pushed ring entry.
 * ISR side: osSemaphoreRelease() (CMSIS-RTOS2 ISR-safe variant).
 * Broker side: osSemaphoreAcquire() with osWaitForever. */
static osSemaphoreId_t _defer_sem = NULL;

/* -- Global topic registry ------------------------------------------------ */
static sys_slist_t _topic_list;
static osMutexId_t _topic_list_mutex = NULL;

/* -- Module init flag ----------------------------------------------------- */
static volatile bool _ipc_ready = false;

/* ========================================================================== */
/* SECTION 4 - Private helpers                                               */
/* ========================================================================== */

/**
 * @brief  Detect ISR context via the ARM IPSR register.
 * @return true if the caller is running inside an interrupt handler.
 */
static inline bool _ipc_in_isr(void)
{
#if defined(__CC_ARM) || defined(__CLANG_ARM) || defined(__GNUC__)
    /* __get_IPSR() is a single-cycle read returning the active exception
     * number.  Zero = Thread mode (task); non-zero = Handler mode (ISR). */
    return (__get_IPSR() != 0U);
#else
    return false;
#endif
}

static inline bool _ipc_is_ready(void)
{
    return _ipc_ready;
}

static inline uint32_t _ipc_defer_index_wrap(uint32_t value)
{
    if (((TM_IPC_DEFER_DEPTH & (TM_IPC_DEFER_DEPTH - 1U)) == 0U) && (TM_IPC_DEFER_DEPTH != 0U)) {
        return value & _DEFER_MASK;
    }

    return (uint32_t)(value % (uint32_t)TM_IPC_DEFER_DEPTH);
}

static inline uint32_t _ipc_defer_pending_locked(void)
{
    return (_defer_write - _defer_read);
}

/**
 * @brief  Recover the ipc_env_hdr pointer from a payload pointer.
 *
 * The payload region immediately follows the header in every slab block, so
 * subtracting sizeof(ipc_env_hdr) gives the header address.
 * uintptr_t cast avoids strict-aliasing UB.
 */
static inline struct ipc_env_hdr *_env_from_payload(void *payload)
{
    return (struct ipc_env_hdr *)((uintptr_t)payload - sizeof(struct ipc_env_hdr));
}

static inline bool _pool_is_enabled(const struct ipc_slab_pool *pool)
{
    return ((pool != NULL) && (pool->total > 0U));
}

static void _ipc_counter_inc(volatile uint32_t *counter)
{
    uint32_t level;

    if (counter == NULL) {
        return;
    }

    /*
     * Keep one helper for both drop_count (ISR-capable path) and pub_count
     * (often already under topic->mutex). The extra short IRQ critical section
     * on the pub_count path is acceptable and keeps the stats contract uniform.
     */
    level = tm_irq_critical_enter();
    (*counter)++;
    tm_irq_critical_exit(level);
}

static void _ipc_runtime_note_broker_progress(bool is_error)
{
    uint32_t level;

    level = tm_irq_critical_enter();
    _broker_dispatch_count++;
    if (is_error) {
        _broker_error_count++;
    }
    _broker_last_progress_tick = osKernelGetTickCount();
    tm_irq_critical_exit(level);
}

static void _ipc_runtime_note_broker_error(void)
{
    uint32_t level;

    level = tm_irq_critical_enter();
    _broker_error_count++;
    tm_irq_critical_exit(level);
}

static bool _ipc_topic_ref_try_get(struct tm_ipc_topic *topic)
{
    bool     acquired = false;
    uint32_t level;

    if (topic == NULL) {
        return false;
    }

    level = tm_irq_critical_enter();
    if (_ipc_ready && topic->active && (topic->mutex != NULL)) {
        topic->in_flight++;
        acquired = true;
    }
    tm_irq_critical_exit(level);

    return acquired;
}

static void _ipc_topic_ref_put(struct tm_ipc_topic *topic)
{
    uint32_t level;

    if (topic == NULL) {
        return;
    }

    level = tm_irq_critical_enter();
    tm_assert(topic->in_flight > 0U);
    if (topic->in_flight > 0U) {
        topic->in_flight--;
    }
    tm_irq_critical_exit(level);
}

/**
 * @brief  Select the smallest slab pool that can hold @p msg_size bytes.
 * @return Pool pointer, or NULL if msg_size exceeds LARGE tier capacity.
 */
static struct ipc_slab_pool *_pool_select(size_t msg_size)
{
    if (_pool_is_enabled(&_pool_tiny) && (msg_size <= (size_t)TM_IPC_SLAB_TINY_PAYLOAD)) {
        return &_pool_tiny;
    }
    if (_pool_is_enabled(&_pool_small) && (msg_size <= (size_t)TM_IPC_SLAB_SMALL_PAYLOAD)) {
        return &_pool_small;
    }
    if (_pool_is_enabled(&_pool_medium) && (msg_size <= (size_t)TM_IPC_SLAB_MEDIUM_PAYLOAD)) {
        return &_pool_medium;
    }
    if (_pool_is_enabled(&_pool_large) && (msg_size <= (size_t)TM_IPC_SLAB_LARGE_PAYLOAD)) {
        return &_pool_large;
    }
    return NULL;
}

/**
 * @brief  Populate a slab pool's free list during initialisation.
 *
 * @param  pool        Pool descriptor to initialise.
 * @param  base        Pointer to the first byte of the block array.
 * @param  block_size  Total size of one block in bytes.
 * @param  count       Number of blocks in the array.
 * @param  cap         Payload capacity of this tier (bytes).
 * @param  pool_ptr    The pool's own address (stored in each header for O(1) free).
 */
static void _pool_populate(struct ipc_slab_pool *pool,
                           uint8_t              *base,
                           size_t                block_size,
                           uint32_t              count,
                           uint32_t              cap,
                           struct ipc_slab_pool *pool_ptr)
{
    sys_slist_init(&pool->free_list);
    pool->payload_cap = cap;
    pool->total       = count;
    pool->avail       = (int32_t)count;

    for (uint32_t i = 0U; i < count; i++) {
        struct ipc_env_hdr *hdr =
            (struct ipc_env_hdr *)((uintptr_t)base + (uintptr_t)(i * block_size));
        hdr->magic       = 0U;
        hdr->ref_count   = 0;
        hdr->payload_cap = cap;
        hdr->pool        = pool_ptr; /* v7.0: store direct pointer for O(1) free */
        sys_slist_append(&pool->free_list, &hdr->free_node);
    }
}

/**
 * @brief  Allocate one block from @p pool.
 *
 * ISR-safe (critical section only, no RTOS blocking).
 *
 * @return Payload pointer (8-byte aligned), or NULL if pool is empty.
 */
static void *_pool_alloc(struct ipc_slab_pool *pool, struct tm_ipc_topic *topic)
{
    struct ipc_env_hdr *hdr = NULL;

    if ((pool == NULL) || !_pool_is_enabled(pool)) {
        return NULL;
    }

    uint32_t level = tm_irq_critical_enter();
    {
        sys_snode_t *node = sys_slist_get(&pool->free_list);
        if (node != NULL) {
            hdr = TM_CONTAINER_OF(node, struct ipc_env_hdr, free_node);
            pool->avail--;
        }
    }
    tm_irq_critical_exit(level);

    if (hdr == NULL) {
        return NULL;
    }

    hdr->magic        = IPC_ENV_MAGIC;
    hdr->ref_count    = 1; /* Initial reference; caller holds it. */
    hdr->source_topic = topic;
    /* hdr->pool is already set at init - never changes. */

    return (void *)((uintptr_t)hdr + sizeof(struct ipc_env_hdr));
}

/**
 * @brief  Return one block to its slab pool.
 *
 * v7.0: uses hdr->pool directly (O(1), no _pool_select branch chain).
 * ISR-safe (critical section only).
 *
 * @param  hdr  Envelope header to release.
 */
static void _pool_free(struct ipc_env_hdr *hdr)
{
    /* Why direct pool pointer: avoids an if-else chain to re-derive the pool
     * from payload_cap at every free(), saving 3 comparisons per call. */
    struct ipc_slab_pool *pool = hdr->pool;
    tm_assert(pool != NULL);
    if (pool == NULL) {
        return;
    }

    /* Poison the magic to catch use-after-free and double-release. */
    hdr->magic     = IPC_ENV_DEAD;
    hdr->ref_count = 0;

    uint32_t level = tm_irq_critical_enter();
    {
        sys_slist_append(&pool->free_list, &hdr->free_node);
        pool->avail++;
    }
    tm_irq_critical_exit(level);
}

/**
 * @brief  Atomically decrement ref_count; free the block when it reaches 0.
 *
 * ISR-safe.
 */
static void _env_release(struct ipc_env_hdr *hdr)
{
    bool should_free = false;

    uint32_t level = tm_irq_critical_enter();
    {
        if (hdr->ref_count > 0) {
            hdr->ref_count--;
            should_free = (hdr->ref_count == 0);
        }
    }
    tm_irq_critical_exit(level);

    if (should_free) {
        _pool_free(hdr);
    }
}

/**
 * @brief  Deliver a message to all (or one) subscriber queue(s).
 *
 * Must be called from task context only (uses osMutexAcquire and
 * osMessageQueuePut).  Shared by the direct task-publish path and the broker.
 *
 * PUB_SUB algorithm (v7.0 - single-pass addref-before-put):
 *   1. Set env->ref_count = 1  (we hold a reference during the loop)
 *   2. For each subscriber:
 *      a. Increment ref_count  (+1) inside a CS
 *      b. osMessageQueuePut (timeout=0, non-blocking)
 *      c. If put fails: _env_release() - immediately undoes the addref
 *   3. Increment pub_count inside the mutex (v7.0 fix for consistency)
 *   4. Release the mutex
 *   5. _env_release() - release our hold; frees the block if ref_count == 0
 *
 * The invariant ref_count >= 1 throughout steps 1-4 guarantees the block
 * cannot be freed prematurely, even if a fast subscriber calls release
 * concurrently after receiving the message.
 *
 * @param  topic  Destination topic.
 * @param  msg    Payload pointer.
 */
static int _ipc_deliver(struct tm_ipc_topic *topic, void *msg)
{
    struct ipc_env_hdr *env;
    int                 ret = TM_IPC_ERR_STATE;

    if ((topic == NULL) || (msg == NULL) || (topic->mutex == NULL)) {
        return TM_IPC_ERR_STATE;
    }

    env = _env_from_payload(msg);
    tm_assert(env->magic == IPC_ENV_MAGIC);
    if (env->magic != IPC_ENV_MAGIC) {
        return TM_IPC_ERR_PARAM;
    }

    if (osMutexAcquire(topic->mutex, osWaitForever) != osOK) {
        _pool_free(env);
        return TM_IPC_ERR_STATE;
    }

    /* -- Empty subscriber list: drop immediately --------------------- */
    if (sys_slist_is_empty(&topic->sub_list)) {
        _ipc_counter_inc(&topic->drop_count);
        osMutexRelease(topic->mutex);
        _pool_free(env);
        return TM_IPC_ERR_NOSUB;
    }

    if (topic->type == TM_IPC_PUB_SUB) {
        bool any_sent = false;
        /* -- Single-pass broadcast ----------------------------------- */

        /* Step 1: Set our caller-hold reference (ref_count was 1 from alloc).
         * No CS needed here - we hold the topic mutex and the block hasn't
         * been published to any queue yet, so no subscriber can release yet. */
        env->ref_count = 1; /* Explicit; already 1, documents the invariant. */

        sys_snode_t *node;
        SYS_SLIST_FOR_EACH_NODE(&topic->sub_list, node)
        {
            struct tm_ipc_sub *sub = TM_CONTAINER_OF(node, struct tm_ipc_sub, node);

            /* Step 2a: Increment ref_count before put to maintain invariant. */
            uint32_t level = tm_irq_critical_enter();
            env->ref_count++;
            tm_irq_critical_exit(level);

            /* Step 2b-c: Attempt non-blocking put; release on failure. */
            if (osMessageQueuePut(sub->queue, &msg, 0U, 0U) != osOK) {
                _env_release(env); /* Undoes the addref; block stays live. */
                _ipc_counter_inc(&topic->drop_count);
            } else {
                any_sent = true;
            }
        }

        /* Step 3: pub_count inside mutex - consistent with PUSH_PULL path. */
        if (any_sent) {
            _ipc_counter_inc(&topic->pub_count);
            ret = TM_IPC_OK;
        } else {
            ret = TM_IPC_ERR_FULL;
        }
        osMutexRelease(topic->mutex);

        /* Step 5: Release our hold.  If all queue puts failed, ref reaches 0
         * here and the block is returned to the slab pool. */
        _env_release(env);
        return ret;

    } else {
        /* -- PUSH_PULL: round-robin to exactly one subscriber ------- */

        uint32_t sub_count = topic->sub_count;

        tm_assert(sub_count > 0U);
        if (sub_count == 0U) {
            _pool_free(env);
            _ipc_counter_inc(&topic->drop_count);
            osMutexRelease(topic->mutex);
            return TM_IPC_ERR_STATE;
        }

        uint32_t target_idx = topic->rr_index % sub_count;
        topic->rr_index     = (topic->rr_index + 1U) % sub_count;

        uint32_t cur_idx = 0U;
        bool     found   = false;

        sys_snode_t *node;
        SYS_SLIST_FOR_EACH_NODE(&topic->sub_list, node)
        {
            if (cur_idx == target_idx) {
                struct tm_ipc_sub *sub = TM_CONTAINER_OF(node, struct tm_ipc_sub, node);
                found                  = true;
                /* ref_count is already 1 from alloc - no addref needed. */
                if (osMessageQueuePut(sub->queue, &msg, 0U, 0U) == osOK) {
                    _ipc_counter_inc(&topic->pub_count);
                    ret = TM_IPC_OK;
                } else {
                    _pool_free(env);
                    _ipc_counter_inc(&topic->drop_count);
                    ret = TM_IPC_ERR_FULL;
                }
                break;
            }
            cur_idx++;
        }

        if (!found) {
            /* Guard: cached sub_count and the live list topology diverged. */
            tm_assert_msg(0, "IPC: PUSH_PULL subscriber count mismatch");
            _pool_free(env);
            _ipc_counter_inc(&topic->drop_count);
            ret = TM_IPC_ERR_STATE;
        }

        osMutexRelease(topic->mutex);
    }
    return ret;
}

/**
 * @brief  Push a (topic, msg) pair onto the deferred ring from ISR context.
 *
 * Deferred ring push: uses the correct wrap path for the configured depth,
 * with a mask fast path only when it is valid.
 * Writer-side safety comes from tm_irq_critical_enter()/exit(): porters must
 * map that pair to a primitive that excludes concurrent ISR writers which are
 * allowed to call tm_ipc_topic_publish() on the same core.
 *
 * @param  topic  Destination topic.
 * @param  msg    Payload pointer.
 * @return        TM_IPC_OK or TM_IPC_ERR_FULL.
 */
static int _ipc_defer_from_isr(struct tm_ipc_topic *topic, void *msg)
{
    struct ipc_env_hdr *env = _env_from_payload(msg);

    uint32_t level = tm_irq_critical_enter();
    {
        uint32_t next = _defer_write + 1U;

        if (_ipc_defer_pending_locked() >= (uint32_t)TM_IPC_DEFER_DEPTH) {
            /* Ring full: release the message immediately to prevent a leak. */
            _defer_drop_count++;
            tm_irq_critical_exit(level);
            _pool_free(env);
            _ipc_counter_inc(&topic->drop_count);
            return TM_IPC_ERR_FULL;
        }

        _defer_ring[_ipc_defer_index_wrap(_defer_write)].topic = topic;
        _defer_ring[_ipc_defer_index_wrap(_defer_write)].msg   = msg;
        _defer_write                    = next;
        if (_ipc_defer_pending_locked() > _defer_high_water) {
            _defer_high_water = _ipc_defer_pending_locked();
        }
    }
    tm_irq_critical_exit(level);

    /* osSemaphoreRelease is ISR-safe and provides the memory barrier that
     * ensures the broker task sees the ring entry written above. */
    osSemaphoreRelease(_defer_sem);
    return TM_IPC_OK;
}

/* ========================================================================== */
/* SECTION 5 - Broker task                                                   */
/* ========================================================================== */

/**
 * @brief  Internal broker task - drains the deferred ISR ring.
 *
 * Runs at TM_IPC_BROKER_PRIORITY (osPriorityRealtime by default).  Sleeps on
 * the counting semaphore; each token corresponds to exactly one ring entry,
 * ensuring FIFO ordering and preventing the broker from reading empty slots.
 */
static void _ipc_broker_task(void *arg)
{
    (void)arg;

    for (;;) {
        osStatus_t stat = osSemaphoreAcquire(_defer_sem, osWaitForever);
        if (stat != osOK) {
            _ipc_runtime_note_broker_error();
            log_error("Broker: semaphore acquire failed (%d)", (int32_t)stat);
            continue;
        }

        /* Sole consumer: no lock needed on _defer_read.
         * Semaphore acquire acts as the acquire memory barrier. */
        uint32_t idx = _ipc_defer_index_wrap(_defer_read);

        struct tm_ipc_topic *topic = _defer_ring[idx].topic;
        void                *msg   = _defer_ring[idx].msg;
        int                  ret;

        /* Advance the consumer index before delivery so the ISR can reuse the
         * slot immediately for the next publish. Clearing after the advance is
         * safe because the ring-full invariant prevents the ISR from wrapping
         * _defer_write back to idx before this clear executes. */
        _defer_read            = _defer_read + 1U;
        _defer_ring[idx].topic = NULL;
        _defer_ring[idx].msg   = NULL;

        if (topic != NULL) {
            if (msg != NULL) {
                ret = _ipc_deliver(topic, msg);
                if ((ret != TM_IPC_OK) && (ret != TM_IPC_ERR_NOSUB) && (ret != TM_IPC_ERR_FULL)) {
                    _ipc_runtime_note_broker_progress(true);
                    log_warn("Broker: deliver failed for topic '%s' (%d)",
                             (topic->name != NULL) ? topic->name : "UNK",
                             ret);
                } else {
                    _ipc_runtime_note_broker_progress(false);
                }
            } else {
                _ipc_runtime_note_broker_progress(true);
                _ipc_counter_inc(&topic->drop_count);
            }
            _ipc_topic_ref_put(topic);
        } else if (msg != NULL) {
            struct ipc_env_hdr *env = _env_from_payload(msg);
            _ipc_runtime_note_broker_progress(true);
            if (env->magic == IPC_ENV_MAGIC) {
                _pool_free(env);
            }
        } else {
            _ipc_runtime_note_broker_progress(true);
        }
    }
}

/* ========================================================================== */
/* SECTION 6 - Public API implementation                                     */
/* ========================================================================== */

void tm_ipc_init(void)
{
    osThreadId_t broker_tid;

    if (_ipc_is_ready()) {
        return;
    }
    /* -- Populate slab free lists ------------------------------------ */
    _pool_populate(&_pool_tiny,
                   (uint8_t *)_slab_tiny,
                   sizeof(ipc_block_tiny_t),
                   TM_IPC_SLAB_TINY_CNT,
                   TM_IPC_SLAB_TINY_PAYLOAD,
                   &_pool_tiny); /* v7.0: pass self-pointer for header storage */

    _pool_populate(&_pool_small,
                   (uint8_t *)_slab_small,
                   sizeof(ipc_block_small_t),
                   TM_IPC_SLAB_SMALL_CNT,
                   TM_IPC_SLAB_SMALL_PAYLOAD,
                   &_pool_small);

    _pool_populate(&_pool_medium,
                   (uint8_t *)_slab_medium,
                   sizeof(ipc_block_medium_t),
                   TM_IPC_SLAB_MEDIUM_CNT,
                   TM_IPC_SLAB_MEDIUM_PAYLOAD,
                   &_pool_medium);

    _pool_populate(&_pool_large,
                   (uint8_t *)_slab_large,
                   sizeof(ipc_block_large_t),
                   TM_IPC_SLAB_LARGE_CNT,
                   TM_IPC_SLAB_LARGE_PAYLOAD,
                   &_pool_large);

    /* -- Global topic registry --------------------------------------- */
    sys_slist_init(&_topic_list);

    static const osMutexAttr_t _list_mutex_attr = {
        .attr_bits = osMutexRecursive | osMutexPrioInherit,
    };
    _topic_list_mutex = osMutexNew(&_list_mutex_attr);
    tm_assert(_topic_list_mutex != NULL);
    if (_topic_list_mutex == NULL) {
        log_error("IPC init failed: topic-list mutex create failed");
        return;
    }

    /* -- Deferred publish semaphore ---------------------------------- */
    _defer_sem = osSemaphoreNew((uint32_t)TM_IPC_DEFER_DEPTH, 0U, NULL);
    tm_assert(_defer_sem != NULL);
    if (_defer_sem == NULL) {
        log_error("IPC init failed: defer semaphore create failed");
        (void)osMutexDelete(_topic_list_mutex);
        _topic_list_mutex = NULL;
        return;
    }

    /* -- Broker task ------------------------------------------------- */
    static const osThreadAttr_t _broker_attr = {
        .name       = "ipc_broker",
        .priority   = (osPriority_t)TM_IPC_BROKER_PRIORITY,
        .stack_size = TM_IPC_BROKER_STACK_SIZE,
    };
    broker_tid = osThreadNew(_ipc_broker_task, NULL, &_broker_attr);
    tm_assert(broker_tid != NULL);
    if (broker_tid == NULL) {
        log_error("IPC init failed: broker thread create failed");
        (void)osSemaphoreDelete(_defer_sem);
        _defer_sem = NULL;
        (void)osMutexDelete(_topic_list_mutex);
        _topic_list_mutex = NULL;
        return;
    }

    _ipc_ready = true;

    log_info("Init OK  TINY:%u SMALL:%u MED:%u LARGE:%u  DEFER:%u",
             TM_IPC_SLAB_TINY_CNT,
             TM_IPC_SLAB_SMALL_CNT,
             TM_IPC_SLAB_MEDIUM_CNT,
             TM_IPC_SLAB_LARGE_CNT,
             TM_IPC_DEFER_DEPTH);
}
INIT_EXPORT_VOID_NAMED(tm_ipc_export_init, tm_ipc_init, EXPORT_LEVEL_SUBSYS, EXPORT_PRIO_DEFAULT);

int tm_ipc_topic_init(struct tm_ipc_topic *topic,
                      const char          *name,
                      enum tm_ipc_type     type,
                      size_t               msg_size)
{
    tm_assert(topic != NULL);
    tm_assert(name != NULL);
    tm_assert(msg_size > 0U);
    tm_assert(msg_size <= (size_t)TM_IPC_SLAB_LARGE_PAYLOAD);

    if ((topic == NULL) || (name == NULL)) {
        return TM_IPC_ERR_PARAM;
    }
    if (!_ipc_is_ready()) {
        return TM_IPC_ERR_STATE;
    }
    if ((msg_size == 0U) || (msg_size > (size_t)TM_IPC_SLAB_LARGE_PAYLOAD)) {
        return TM_IPC_ERR_TOOBIG;
    }

    topic->name       = name;
    topic->msg_size   = msg_size;
    topic->type       = type;
    topic->pub_count  = 0U;
    topic->drop_count = 0U;
    topic->sub_count  = 0U;
    topic->rr_index   = 0U;
    topic->in_flight  = 0U;
    topic->active     = false;

    sys_slist_init(&topic->sub_list);

    static const osMutexAttr_t _topic_mutex_attr = {
        .attr_bits = osMutexRecursive | osMutexPrioInherit,
    };
    topic->mutex = osMutexNew(&_topic_mutex_attr);
    if (topic->mutex == NULL) {
        log_error("Topic '%s': mutex create failed", name);
        return TM_IPC_ERR_NOMEM;
    }

    osMutexAcquire(_topic_list_mutex, osWaitForever);
    sys_slist_append(&_topic_list, &topic->list_node);
    osMutexRelease(_topic_list_mutex);
    topic->active = true;

    log_info("Topic '%s' init  type=%u  msg_size=%u", name, (uint32_t)type, (uint32_t)msg_size);
    return TM_IPC_OK;
}

void tm_ipc_topic_deinit(struct tm_ipc_topic *topic)
{
    uint32_t waited_ms = 0U;
    uint32_t level;

    if (topic == NULL) {
        return;
    }
    if (!_ipc_is_ready()) {
        return;
    }

    tm_assert(!_ipc_in_isr());

    level         = tm_irq_critical_enter();
    topic->active = false;
    tm_irq_critical_exit(level);

    if (topic->mutex == NULL) {
        return;
    }

    /* Remove from global list. */
    osMutexAcquire(_topic_list_mutex, osWaitForever);
    sys_slist_find_and_remove(&_topic_list, &topic->list_node);
    osMutexRelease(_topic_list_mutex);

    /*
     * in_flight spans every path that may still touch topic->mutex, including
     * task-context publish/sub init/sub deinit and broker-side deferred
     * delivery. Once the count reaches zero, no remaining path can newly enter
     * osMutexAcquire(topic->mutex) on this topic.
     */
    while (topic->in_flight != 0U) {
        if (osKernelGetState() != osKernelRunning) {
            tm_assert_msg(0, "IPC: topic deinit cannot drain in-flight ops before kernel start");
            return;
        }
        osDelay(1U);
        waited_ms++;
        if (waited_ms >= TM_IPC_DEINIT_DRAIN_TIMEOUT_MS) {
            log_error("Topic '%s' deinit timed out with %u in-flight op(s)",
                      (topic->name != NULL) ? topic->name : "UNK",
                      (unsigned)topic->in_flight);
            tm_assert_msg(0, "IPC: topic deinit drain timeout");
            return;
        }
    }

    /* Null out back-pointers in all attached subscribers. */
    osMutexAcquire(topic->mutex, osWaitForever);
    {
        sys_snode_t *node;
        sys_snode_t *next;
        SYS_SLIST_FOR_EACH_NODE_SAFE(&topic->sub_list, node, next)
        {
            struct tm_ipc_sub *sub = TM_CONTAINER_OF(node, struct tm_ipc_sub, node);
            sub->topic             = NULL;
        }
        sys_slist_init(&topic->sub_list);
        topic->sub_count = 0U;
    }
    osMutexRelease(topic->mutex);
    osMutexDelete(topic->mutex);
    topic->mutex = NULL;

    log_info("Topic '%s' deinit", topic->name);
}

int tm_ipc_sub_init(struct tm_ipc_sub   *sub,
                    struct tm_ipc_topic *topic,
                    const char          *name,
                    uint32_t             queue_depth)
{
    osStatus_t stat;
    tm_assert(sub != NULL);
    tm_assert(topic != NULL);
    tm_assert(queue_depth > 0U);

    if ((sub == NULL) || (topic == NULL) || (queue_depth == 0U)) {
        return TM_IPC_ERR_PARAM;
    }
    if (!_ipc_is_ready()) {
        return TM_IPC_ERR_STATE;
    }
    if (!_ipc_topic_ref_try_get(topic)) {
        return TM_IPC_ERR_STATE;
    }

    const char *safe_name = (name != NULL) ? name : "UNK";
    strncpy(sub->name, safe_name, TM_IPC_NAME_LEN - 1U);
    sub->name[TM_IPC_NAME_LEN - 1U] = '\0';
    sub->topic                      = NULL;

    /* Queue holds void* pointers to payload regions - zero-copy fanout. */
    sub->queue = osMessageQueueNew(queue_depth, sizeof(void *), NULL);
    if (sub->queue == NULL) {
        _ipc_topic_ref_put(topic);
        log_error("Sub '%s': queue create failed (depth=%u)", sub->name, queue_depth);
        return TM_IPC_ERR_NOMEM;
    }

    stat = osMutexAcquire(topic->mutex, osWaitForever);
    if (stat != osOK) {
        osMessageQueueDelete(sub->queue);
        sub->queue = NULL;
        _ipc_topic_ref_put(topic);
        return TM_IPC_ERR_STATE;
    }
    if (!topic->active) {
        osMutexRelease(topic->mutex);
        osMessageQueueDelete(sub->queue);
        sub->queue = NULL;
        _ipc_topic_ref_put(topic);
        return TM_IPC_ERR_STATE;
    }

    sub->topic = topic;
    sys_slist_append(&topic->sub_list, &sub->node);
    topic->sub_count++;
    osMutexRelease(topic->mutex);
    _ipc_topic_ref_put(topic);

    log_info("Sub '%s' attached to topic '%s' (depth=%u)", sub->name, topic->name, queue_depth);
    return TM_IPC_OK;
}

void tm_ipc_sub_deinit(struct tm_ipc_sub *sub)
{
    if (sub == NULL) {
        return;
    }

    if (sub->topic != NULL) {
        struct tm_ipc_topic *topic = sub->topic;
        if (_ipc_topic_ref_try_get(topic)) {
            if ((topic->mutex != NULL) && (osMutexAcquire(topic->mutex, osWaitForever) == osOK)) {
                if (sys_slist_find_and_remove(&topic->sub_list, &sub->node)) {
                    tm_assert(topic->sub_count > 0U);
                    if (topic->sub_count > 0U) {
                        topic->sub_count--;
                    }
                }
                osMutexRelease(topic->mutex);
            }
            _ipc_topic_ref_put(topic);
        }
        sub->topic = NULL;
    }

    if (sub->queue != NULL) {
        void *msg;
        /* Drain and release all buffered messages before destroying the queue. */
        /* Zero-timeout queue get is an explicit drain loop; only osOK means a message was dequeued. */
        while (osMessageQueueGet(sub->queue, &msg, NULL, 0U) == osOK) {
            tm_ipc_msg_release(msg);
        }
        osMessageQueueDelete(sub->queue);
        sub->queue = NULL;
    }
}

void *tm_ipc_msg_alloc(struct tm_ipc_topic *topic)
{
    if ((topic == NULL) || !_ipc_is_ready() || !topic->active) {
        return NULL;
    }

    struct ipc_slab_pool *pool = _pool_select(topic->msg_size);
    if (pool == NULL) {
        log_error("Alloc: no IPC slab tier available for msg_size=%u", (uint32_t)topic->msg_size);
        return NULL;
    }

    void *payload = _pool_alloc(pool, topic);
    if (payload == NULL) {
        log_warn("Alloc: pool cap=%u is empty", pool->payload_cap);
    }
    return payload;
}

void *tm_ipc_msg_alloc_zero(struct tm_ipc_topic *topic)
{
    void *payload = tm_ipc_msg_alloc(topic);
    if (payload != NULL) {
        /* Zero the payload region only - the header is managed by the pool. */
        struct ipc_env_hdr *env = _env_from_payload(payload);
        memset(payload, 0, (size_t)env->payload_cap);
    }
    return payload;
}

int tm_ipc_topic_publish(struct tm_ipc_topic *topic, void *msg)
{
    struct ipc_env_hdr *env;
    int                 ret;

    if (msg == NULL) {
        /* Transfer of ownership was promised - free msg to prevent leak. */
        return TM_IPC_ERR_PARAM;
    }

    env = _env_from_payload(msg);
    tm_assert(env->magic == IPC_ENV_MAGIC);
    if (env->magic != IPC_ENV_MAGIC) {
        return TM_IPC_ERR_PARAM;
    }

    if (topic == NULL) {
        _pool_free(env);
        return TM_IPC_ERR_PARAM;
    }
    if (!_ipc_topic_ref_try_get(topic)) {
        _pool_free(env);
        return TM_IPC_ERR_STATE;
    }

    if (_ipc_in_isr()) {
        ret = _ipc_defer_from_isr(topic, msg);
        if (ret != TM_IPC_OK) {
            _ipc_topic_ref_put(topic);
        }
        return ret;
    }

    ret = _ipc_deliver(topic, msg);
    _ipc_topic_ref_put(topic);
    return ret;
}

int tm_ipc_sub_recv(struct tm_ipc_sub *sub, void **msg_out, uint32_t timeout_ms)
{
    osStatus_t stat;

    if ((sub == NULL) || (msg_out == NULL)) {
        return TM_IPC_ERR_PARAM;
    }
    if (!_ipc_is_ready() || (sub->queue == NULL)) {
        return TM_IPC_ERR_STATE;
    }

    tm_assert(!_ipc_in_isr());

    stat = osMessageQueueGet(sub->queue, msg_out, NULL, timeout_ms);
    if (stat == osOK) {
        return TM_IPC_OK;
    }
    if ((stat == osErrorTimeout) || ((timeout_ms == 0U) && (stat == osErrorResource))) {
        /*
         * CMSIS-RTOS2 reports an empty zero-timeout queue as `osErrorResource`.
         * The IPC northbound contract treats both forms as "no message yet".
         */
        return TM_IPC_ERR_TIMEOUT;
    }
    return TM_IPC_ERR_STATE;
}

void tm_ipc_msg_release(void *msg)
{
    if (msg == NULL) {
        return;
    }

    struct ipc_env_hdr *env = _env_from_payload(msg);
    if (env->magic != IPC_ENV_MAGIC) {
        tm_assert_msg(0, "IPC: release with bad magic (double-free or UAF')");
        return;
    }

    _env_release(env);
}

void tm_ipc_topic_stat(const struct tm_ipc_topic *topic, uint32_t *pub_count, uint32_t *drop_count)
{
    if (topic == NULL) {
        return;
    }
    if (pub_count != NULL) {
        *pub_count = topic->pub_count;
    }
    if (drop_count != NULL) {
        *drop_count = topic->drop_count;
    }
}

void tm_ipc_slab_stat(uint32_t *tiny_free,
                      uint32_t *small_free,
                      uint32_t *medium_free,
                      uint32_t *large_free)
{
    /* Diagnostic snapshot: momentary races are acceptable for monitoring. */
    if (tiny_free != NULL) {
        *tiny_free = (uint32_t)_pool_tiny.avail;
    }
    if (small_free != NULL) {
        *small_free = (uint32_t)_pool_small.avail;
    }
    if (medium_free != NULL) {
        *medium_free = (uint32_t)_pool_medium.avail;
    }
    if (large_free != NULL) {
        *large_free = (uint32_t)_pool_large.avail;
    }
}

void tm_ipc_runtime_stat(struct tm_ipc_runtime_stat *stat)
{
    uint32_t level;

    if (stat == NULL) {
        return;
    }

    memset(stat, 0, sizeof(*stat));
    if (!_ipc_is_ready()) {
        return;
    }

    level                           = tm_irq_critical_enter();
    stat->defer_pending             = _ipc_defer_pending_locked();
    stat->defer_high_water          = _defer_high_water;
    stat->defer_drop_count          = _defer_drop_count;
    stat->broker_dispatch_count     = _broker_dispatch_count;
    stat->broker_error_count        = _broker_error_count;
    stat->broker_last_progress_tick = _broker_last_progress_tick;
    tm_irq_critical_exit(level);
}

void tm_ipc_topic_foreach(tm_ipc_topic_visitor_fn visitor, void *arg)
{
    sys_snode_t *node;

    if (visitor == NULL) {
        return;
    }
    if (!_ipc_is_ready()) {
        return;
    }

    tm_assert(!_ipc_in_isr());

    osMutexAcquire(_topic_list_mutex, osWaitForever);
    node = sys_slist_peek_head(&_topic_list);
    osMutexRelease(_topic_list_mutex);

    while (node != NULL) {
        const struct tm_ipc_topic *topic;
        sys_snode_t               *next;
        bool                       active;
        uint32_t                   level;

        osMutexAcquire(_topic_list_mutex, osWaitForever);
        topic = (const struct tm_ipc_topic *)TM_CONTAINER_OF(node, struct tm_ipc_topic, list_node);
        next  = sys_slist_peek_next(node);
        osMutexRelease(_topic_list_mutex);

        level  = tm_irq_critical_enter();
        active = topic->active;
        tm_irq_critical_exit(level);

        if (active) {
            visitor(topic, arg);
        }

        node = next;
    }
}

#endif /* TM_IPC_ENABLE */

/* ---------------------------------------- end of file ----------------------------------------- */
