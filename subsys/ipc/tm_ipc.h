/**
 * @file    tm_ipc.h
 * @brief   Themis IPC - Industrial-Grade, Zero-Copy, ISR-Safe Message Bus
 * @version v7.0.0
 * @date    2026.03.01
 *
 * Architecture overview
 * ---------------------
 *   - Static topic registration via IPC_TOPIC_DEFINE() - topics are global
 *     structs referenced by direct pointer.  No hash table, no strcmp, no
 *     RB-tree at runtime.  Lookup is O(1) by construction.
 *
 *   - Multi-tier slab allocator (TINY/SMALL/MEDIUM/LARGE).  Payload capacities
 *     are compile-time constants.  The envelope header stores a direct pool
 *     pointer (v7.0: replaces payload_cap-based re-lookup), so free() is O(1)
 *     with no if-else chain.  8-byte payload alignment is guaranteed via
 *     static_assert.
 *
 *   - ISR-safe deferred publish.  When tm_ipc_topic_publish() detects ISR
 *     context (__get_IPSR() != 0), the (topic, msg) pair is written into a
 *     bounded deferred ring.  Arbitrary positive depths remain supported,
 *     and the implementation may use a mask fast path when it is valid.
 *     A dedicated high-priority broker task drains the ring in task context.
 *
 *   - Ref-counted fanout for PUB_SUB (v7.0: single-pass delivery replaces
 *     the two-pass count+deliver loop).  ref_count starts at 1 (caller hold),
 *     increments by 1 before each queue put.  On queue-full failure the ref is
 *     immediately released.  The caller releases its hold after the loop.
 *     The block returns to the pool when every subscriber releases.
 *
 * Improvements over v6.0
 * ----------------------
 *   - _ipc_deliver: single-pass delivery (addref-before-put) eliminates the
 *     O(n) subscriber-count traversal that preceded delivery in v6.0.
 *   - ipc_env_hdr: stores struct ipc_slab_pool *pool directly.  _pool_free()
 *     no longer calls _pool_select() (if-else chain) at runtime.
 *   - Deferred ring: arbitrary positive depths remain valid, with mask-based
 *     wrapping available only when the configured depth makes it correct.
 *   - pub_count incremented inside topic->mutex for PUB_SUB (was after
 *     mutex release in v6.0 - minor atomicity inconsistency fixed).
 *   - New API: tm_ipc_msg_alloc_zero() - alloc + zero-fill in one call.
 *   - New API: tm_ipc_topic_foreach() - iterate topics for shell diagnostics.
 *
 * Message lifecycle
 * -----------------
 *   alloc -> [fill] -> publish -(task)--> deliver -> recv -> [consume] -> release
 *                           \\-(ISR)---> defer ring -> broker -> deliver -/
 *
 * Thread-safety contract
 * ----------------------
 *   tm_ipc_msg_alloc()        ISR-safe  (critical section, no blocking)
 *   tm_ipc_msg_alloc_zero()   ISR-safe  (critical section, no blocking)
 *   tm_ipc_topic_publish()    ISR-safe  (deferred path for ISR context)
 *   tm_ipc_sub_recv()         Task only (blocking RTOS wait)
 *   tm_ipc_msg_release()      ISR-safe  (critical section, no blocking)
 *   tm_ipc_topic_init()       Init only (before scheduler or in INIT_EXPORT fn)
 *   tm_ipc_topic_foreach()    Task only (best-effort walk, visitor runs unlocked)
 *
 * Porting / ISR priority contract
 * -------------------------------
 *   The deferred ISR publish path relies on tm_irq_critical_enter()/exit()
 *   serialising all ring-index updates performed by contexts that are allowed
 *   to call tm_ipc_topic_publish() concurrently.
 *
 *   ISR callers must also run at an RTOS-API-safe interrupt priority because
 *   the deferred path releases a CMSIS-RTOS2 semaphore from ISR context.
 *   If a target cannot provide both guarantees, keep tm_ipc off that path or
 *   adapt tm_runtime before enabling ISR publish.
 *
 * When to use tm_ipc vs pubsub
 * ----------------------------
 *   Use tm_ipc when messages must cross task boundaries, be buffered per
 *   subscriber, or be published from ISR and consumed later in task context.
 *   Use pubsub when one execution context needs immediate synchronous callback
 *   notification without RTOS objects or queue ownership.
 *
 * Build-time disable contract
 * ---------------------------
 *   When TM_IPC_ENABLE == 0, the public APIs collapse to stub macros that
 *   preserve call sites, return deterministic status values, and do NOT
 *   evaluate argument expressions at runtime. This keeps tiny-profile builds
 *   side-effect free while avoiding call-site #ifdef branches.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <errno.h>
#include "cmsis_os2.h"
#include "config/tm_core_config.h"
#include "slist.h"
#include "tmdef.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public macros -------------------------------------------------------------------------------- */

/* Project-level IPC configuration is owned by tm_core_config.h. */
#if !defined(TM_IPC_ENABLE) || !defined(TM_IPC_SLAB_TINY_CNT) ||              \
    !defined(TM_IPC_SLAB_SMALL_CNT) || !defined(TM_IPC_SLAB_MEDIUM_CNT) ||    \
    !defined(TM_IPC_SLAB_LARGE_CNT) || !defined(TM_IPC_DEFER_DEPTH) ||        \
    !defined(TM_IPC_BROKER_STACK_SIZE) || !defined(TM_IPC_BROKER_PRIORITY) || \
    !defined(TM_IPC_NAME_LEN)
#error "IPC configuration must be provided by tm_core_config.h"
#endif

/*
 * Slab pool block counts
 * ----------------------
 * Static RAM budget per tier = count x (sizeof(ipc_env_hdr) + payload_cap).
 * sizeof(ipc_env_hdr) is 24 bytes on a 32-bit ARM target.
 *
 * Example with current defaults:
 *   TINY    8 x (24 +  16) = 320 B
 *   SMALL   4 x (24 +  64) = 352 B
 *   MEDIUM  2 x (24 + 128) = 304 B
 *   LARGE   1 x (24 + 256) = 280 B
 *   Total                  = 1256 B
 *
 * Tuned by TM_IPC_SLAB_xxx_CNT in tm_core_config.h.
 * Zero-count tiers are allowed; tier selection automatically skips them and
 * falls through to the next larger enabled tier instead of reserving dead
 * RAM. Once a tier is selected for a given msg_size, runtime exhaustion does
 * not spill into larger tiers; each enabled pool keeps its own reserved budget.
 */

/** Payload capacity (bytes) for each slab tier - payload only, not the header. */
#define TM_IPC_SLAB_TINY_PAYLOAD   (16U)
#define TM_IPC_SLAB_SMALL_PAYLOAD  (64U)
#define TM_IPC_SLAB_MEDIUM_PAYLOAD (128U)
#define TM_IPC_SLAB_LARGE_PAYLOAD  (256U)

/**
 * @brief Depth of the ISR-to-broker deferred ring buffer.
 *
 * Tuned by TM_IPC_DEFER_DEPTH in tm_core_config.h.
 * Any positive depth is supported.  The implementation may choose a mask
 * fast path when the configured depth makes it correct, otherwise it uses
 * modulo wrap inside the implementation.
 * Represents the maximum burst of ISR publishes that can be buffered before
 * delivery; exceeding this count drops the message and increments drop_count
 * on the topic.
 */

/**
 * @brief Stack depth (bytes) for the internal broker task.
 *
 * Tuned by TM_IPC_BROKER_STACK_SIZE in tm_core_config.h.
 */

/**
 * @brief CMSIS-RTOS2 priority for the broker task.
 *
 * Tuned by TM_IPC_BROKER_PRIORITY in tm_core_config.h.
 * Must be higher than any task that publishes from ISR and then immediately
 * waits on a subscriber queue, to guarantee delivery order.
 */

/**
 * @brief Maximum debug name length including the NUL terminator.
 *
 * Tuned by TM_IPC_NAME_LEN in tm_core_config.h.
 */

/** Convenience alias for an infinite receive timeout. */
#define TM_IPC_WAIT_FOREVER        osWaitForever

#ifndef TM_IPC_DEINIT_DRAIN_TIMEOUT_MS
#define TM_IPC_DEINIT_DRAIN_TIMEOUT_MS 500U
#endif

/* Error codes - all public IPC status values are negative errno mappings. */
#define TM_IPC_OK          (0)          /* Operation succeeded                   */
#define TM_IPC_ERR_PARAM   (-EINVAL)    /* NULL pointer or out-of-range argument */
#define TM_IPC_ERR_NOMEM   (-ENOMEM)    /* Slab pool exhausted                   */
#define TM_IPC_ERR_TIMEOUT (-ETIMEDOUT) /* Receive timed out                     */
#define TM_IPC_ERR_FULL    (-ENOBUFS)   /* Deferred ring or subscriber queue full*/
#define TM_IPC_ERR_NOSUB   (-ENXIO)     /* No subscribers registered on topic    */
#define TM_IPC_ERR_STATE   (-EPERM)     /* IPC/topic state is inactive or invalid*/
#define TM_IPC_ERR_TOOBIG  (-EMSGSIZE)  /* msg_size exceeds LARGE slab capacity  */

/* Public types --------------------------------------------------------------------------------- */

/**
 * @brief Message bus delivery mode.
 */
enum tm_ipc_type {
    /** Broadcast: every subscriber receives every message. */
    TM_IPC_PUB_SUB = 0U,
    /** Round-robin: messages are distributed one-per-subscriber. */
    TM_IPC_PUSH_PULL = 1U,
};

/**
 * @brief IPC topic descriptor.
 *
 * Allocate statically with IPC_TOPIC_DEFINE() and activate with
 * tm_ipc_topic_init().  Reference by pointer only - never by name - to
 * achieve O(1) publish/subscribe at runtime.
 *
 * Fields tagged [internal] must not be read or written by application code.
 */
struct tm_ipc_topic {
    /** Debug/logging name configured at init and read-only thereafter. */
    const char *name;
    /** Maximum payload bytes accepted by this topic. */
    size_t msg_size;
    /** Delivery mode configured at init and read-only thereafter. */
    enum tm_ipc_type type;

    /** Total successful publishes. */
    volatile uint32_t pub_count;
    /** Messages dropped because no subscriber existed or one queue/defer path was full. */
    volatile uint32_t drop_count;

    /** [internal] Subscriber linked list. */
    sys_slist_t sub_list;
    /** [internal] Mutex protecting `sub_list` and related topic state. */
    osMutexId_t mutex;
    /** [internal] Node in the global topic list. */
    sys_snode_t list_node;
    /** [internal] Cached subscriber count. */
    volatile uint32_t sub_count;
    /** [internal] PUSH_PULL dispatch cursor. */
    volatile uint32_t rr_index;
    /** [internal] Active publish/subscriber references. */
    volatile uint32_t in_flight;
    /** [internal] `true` while the topic accepts new operations. */
    volatile bool active;
};

/**
 * @brief IPC subscriber descriptor.
 *
 * Initialise with tm_ipc_sub_init() before calling tm_ipc_sub_recv().
 * All fields are managed internally.
 */
struct tm_ipc_sub {
    /** [internal] Linkage node inside the topic subscriber list. */
    sys_snode_t node;
    /** [internal] Owning topic pointer. */
    struct tm_ipc_topic *topic;
    /** [internal] RTOS queue carrying payload pointers. */
    osMessageQueueId_t queue;
    /** Debug name copied into the subscriber descriptor. */
    char name[TM_IPC_NAME_LEN];
};

/**
 * @brief Callback signature for tm_ipc_topic_foreach().
 *
 * @param topic  Pointer to the topic currently being visited.
 * @param arg    User-supplied argument passed through from foreach call.
 */
typedef void (*tm_ipc_topic_visitor_fn)(const struct tm_ipc_topic *topic, void *arg);

/**
 * @brief Runtime-health snapshot for the deferred ISR publish path.
 *
 * This struct is owned by the caller and filled by tm_ipc_runtime_stat().
 * All fields are diagnostic hints rather than transactional guarantees.
 */
struct tm_ipc_runtime_stat {
    /** Current deferred-ring backlog. */
    uint32_t defer_pending;
    /** Peak deferred backlog observed since init. */
    uint32_t defer_high_water;
    /** Ring-full drops on the ISR deferred path. */
    uint32_t defer_drop_count;
    /** Deferred entries drained by the broker task. */
    uint32_t broker_dispatch_count;
    /** Unexpected broker-side error incidents. */
    uint32_t broker_error_count;
    /** `osKernelGetTickCount()` value recorded at the last broker drain. */
    uint32_t broker_last_progress_tick;
};

/* Static topic registration macro -------------------------------------------------------------- */

/**
 * @brief Declare and zero-initialise a static IPC topic.
 *
 * Generates a global @c struct @c tm_ipc_topic named @p _var.  The struct
 * is placed in BSS; @c mutex and @c sub_list are activated by
 * tm_ipc_topic_init() during system startup.
 *
 * Pattern:
 * @code
 *   // In my_topics.h - visible to both publisher and subscribers
 *   extern struct tm_ipc_topic g_vel_topic;
 *
 *   // In my_topics.c
 *   IPC_TOPIC_DEFINE(g_vel_topic, "vel", TM_IPC_PUB_SUB, sizeof(struct vel_msg));
 *
 *   // In bsp_init() or an INIT_EXPORT'd function
 *   tm_ipc_topic_init(&g_vel_topic, "vel", TM_IPC_PUB_SUB, sizeof(struct vel_msg));
 * @endcode
 *
 * @param _var   C identifier for the generated topic variable.
 * @param _name  String literal for debug logging (not used for lookup).
 * @param _type  TM_IPC_PUB_SUB or TM_IPC_PUSH_PULL.
 * @param _size  Maximum payload size in bytes (<= TM_IPC_SLAB_LARGE_PAYLOAD).
 */
#define IPC_TOPIC_DEFINE(_var, _name, _type, _size) \
    struct tm_ipc_topic _var = {                    \
        .name     = (_name),                        \
        .msg_size = (size_t)(_size),                \
        /* All RTOS handles and lists are zero-init in BSS. */                    \
        .type = (_type),                            \
    }

/* Public Functions ----------------------------------------------------------------------------- */

#if TM_IPC_ENABLE

/**
 * @brief  Initialise the IPC subsystem.
 *
 * Populates all slab free-lists, creates the deferred-publish counting
 * semaphore, and starts the internal broker task.  Must be called exactly
 * once, before any other tm_ipc_xxx call, typically from an INIT_EXPORT'd
 * function at EXPORT_LEVEL_MIDWARE.
 */
void tm_ipc_init(void);

/**
 * @brief  Initialise an IPC topic descriptor.
 *
 * Creates the RTOS mutex protecting the subscriber list and registers the
 * topic in the global debug list.  tm_ipc_init() must already have completed,
 * otherwise TM_IPC_ERR_STATE is returned.
 *
 * @param  topic     Pointer to a topic struct.  Must not be NULL.
 * @param  name      Null-terminated debug name.
 * @param  type      TM_IPC_PUB_SUB or TM_IPC_PUSH_PULL.
 * @param  msg_size  Maximum payload bytes.  Must be <= TM_IPC_SLAB_LARGE_PAYLOAD.
 * @return           TM_IPC_OK on success, TM_IPC_ERR_xxx on failure.
 */
int tm_ipc_topic_init(struct tm_ipc_topic *topic,
                      const char          *name,
                      enum tm_ipc_type     type,
                      size_t               msg_size);

/**
 * @brief  De-initialise an IPC topic.
 *
 * Task context only.  Marks the topic inactive, removes it from the global
 * list, waits for all in-flight publish/subscriber operations to drain, then
 * nulls the topic back-pointer in every active subscriber and deletes the
 * RTOS mutex. The in-flight drain covers any publisher or broker path that may
 * still acquire @c topic->mutex after observing the topic pointer. If the
 * drain exceeds TM_IPC_DEINIT_DRAIN_TIMEOUT_MS, teardown stops early so live
 * RTOS objects are not destroyed under an active path.
 *
 * @param  topic  Topic to de-initialise.  NULL is silently ignored.
 */
void tm_ipc_topic_deinit(struct tm_ipc_topic *topic);

/**
 * @brief  Attach a subscriber to a topic.
 *
 * Allocates an internal RTOS message queue of @p queue_depth pointer-sized
 * slots and appends @p sub to the topic's subscriber list.
 *
 * @param  sub         Subscriber struct (caller-owned lifetime).
 * @param  topic       Target topic.  Must be active and initialised.
 * @param  name        Null-terminated debug name.
 * @param  queue_depth Maximum unconsumed messages that can be buffered.
 * @return             TM_IPC_OK on success, TM_IPC_ERR_xxx on failure.
 */
int tm_ipc_sub_init(struct tm_ipc_sub   *sub,
                    struct tm_ipc_topic *topic,
                    const char          *name,
                    uint32_t             queue_depth);

/**
 * @brief  Detach a subscriber and release all held resources.
 *
 * Drains pending message pointers from the internal queue, decrements their
 * reference counts, and destroys the RTOS queue object.
 *
 * @param  sub  Subscriber to de-initialise.  NULL is silently ignored.
 */
void tm_ipc_sub_deinit(struct tm_ipc_sub *sub);

/**
 * @brief  Allocate a message buffer from the slab pool.
 *
 * Selects the smallest enabled tier whose payload capacity >= topic->msg_size.
 * The returned pointer addresses the writable payload region and is
 * guaranteed to be 8-byte aligned. If that selected tier is temporarily empty,
 * allocation returns NULL instead of borrowing from a larger tier.
 *
 * ISR-safe (critical section only, no RTOS blocking).
 *
 * @param  topic  Topic the message will be published to.  Must not be NULL.
 * @return        Pointer to the payload buffer, or NULL if the pool is full
 *                or the IPC/topic state is inactive.
 */
void *tm_ipc_msg_alloc(struct tm_ipc_topic *topic);

/**
 * @brief  Allocate a message buffer and zero-fill the payload region.
 *
 * Convenience wrapper over tm_ipc_msg_alloc() that memsets the payload to 0
 * before returning.  Useful when only a subset of fields will be written.
 *
 * ISR-safe (critical section only).
 *
 * @param  topic  Topic the message will be published to.  Must not be NULL.
 * @return        Pointer to a zeroed payload buffer, or NULL if the pool is
 *                full or the IPC/topic state is inactive.
 */
void *tm_ipc_msg_alloc_zero(struct tm_ipc_topic *topic);

/**
 * @brief  Publish a message to a topic.
 *
 * Task context path: fills subscriber queues synchronously under the topic
 * mutex and returns after delivery.
 *
 * ISR context path:  pushes the (topic, msg) pair to the deferred ring using
 * internal wrap logic that accepts any positive depth.  Power-of-two depths
 * may still take a mask fast path internally.  Returns immediately; the broker
 * delivers to subscriber queues asynchronously.
 * This path is only valid for ISR priorities that are allowed to call the
 * project's CMSIS-RTOS2 ISR APIs.
 *
 * Ownership of @p msg is unconditionally transferred to the IPC system on
 * return.  Do NOT read or write @p msg after calling this function.
 *
 * For TM_IPC_PUB_SUB, TM_IPC_OK means at least one subscriber queue accepted
 * the message. Individual queue-full failures are counted in drop_count. The
 * call returns TM_IPC_ERR_FULL only when no subscriber accepted the message.
 *
 * @param  topic  Destination topic.  Must not be NULL.
 * @param  msg    Payload pointer from tm_ipc_msg_alloc().  Must not be NULL.
 * @return        TM_IPC_OK, TM_IPC_ERR_NOSUB, TM_IPC_ERR_FULL,
 *                TM_IPC_ERR_STATE, or TM_IPC_ERR_PARAM.
 */
int tm_ipc_topic_publish(struct tm_ipc_topic *topic, void *msg);

/**
 * @brief  Receive a message from a subscriber queue (blocking).
 *
 * Blocks until a message is available or @p timeout_ms elapses.
 * A zero timeout is treated as a non-blocking poll; an empty queue maps to
 * TM_IPC_ERR_TIMEOUT rather than TM_IPC_ERR_STATE.
 * On success, *msg_out points to the payload.  The caller must call
 * tm_ipc_msg_release(*msg_out) when finished.
 *
 * Task context only.
 *
 * @param  sub         Subscriber to receive from.  Must not be NULL.
 * @param  msg_out     Receives a pointer to the payload buffer.
 * @param  timeout_ms  Milliseconds to wait; TM_IPC_WAIT_FOREVER for infinite.
 * @return             TM_IPC_OK, TM_IPC_ERR_TIMEOUT, TM_IPC_ERR_STATE,
 *                     or TM_IPC_ERR_PARAM.
 */
int tm_ipc_sub_recv(struct tm_ipc_sub *sub, void **msg_out, uint32_t timeout_ms);

/**
 * @brief  Release a received message back to the slab pool.
 *
 * Decrements the envelope reference count.  The slab block is freed only
 * when the count reaches zero (all pub-sub subscribers have released).
 *
 * ISR-safe (critical section only).
 *
 * @param  msg  Payload pointer from tm_ipc_sub_recv().  NULL is ignored.
 */
void tm_ipc_msg_release(void *msg);

/**
 * @brief  Read publish and drop counters for a topic (non-blocking snapshot).
 *
 * @param  topic       Topic to query.  Must not be NULL.
 * @param  pub_count   Output: total successful publishes (may be NULL).
 * @param  drop_count  Output: total dropped messages (may be NULL).
 */
void tm_ipc_topic_stat(const struct tm_ipc_topic *topic, uint32_t *pub_count, uint32_t *drop_count);

/**
 * @brief  Read free-block counts for all slab tiers (diagnostics).
 *
 * Any output pointer may be NULL.
 */
void tm_ipc_slab_stat(uint32_t *tiny_free,
                      uint32_t *small_free,
                      uint32_t *medium_free,
                      uint32_t *large_free);

/**
 * @brief  Read a best-effort runtime snapshot for deferred IPC health.
 *
 * The snapshot is non-blocking and safe from both task and ISR context.
 * It is intended for shell/diag/watchdog-style monitoring, not for flow
 * control decisions.
 *
 * @param  stat  Output snapshot buffer. NULL is ignored.
 */
void tm_ipc_runtime_stat(struct tm_ipc_runtime_stat *stat);

/**
 * @brief  Iterate all registered topics and invoke @p visitor for each one.
 *
 * Takes a best-effort walk over the current topic list. Each iteration briefly
 * re-acquires the global topic-list mutex long enough to fetch the current
 * node and its next pointer, then invokes @p visitor after releasing that
 * mutex again.
 *
 * The walk is intentionally diagnostic-grade rather than a perfect atomic
 * snapshot, and the visitor always runs unlocked.
 *
 * Task context only.
 *
 * @param  visitor  Callback invoked for each topic (must not be NULL).
 * @param  arg      User-supplied argument forwarded to each visitor call.
 */
void tm_ipc_topic_foreach(tm_ipc_topic_visitor_fn visitor, void *arg);

#else /* !TM_IPC_ENABLE */

#define _TM_IPC_DISABLED_ARG(_arg) ((void)sizeof(_arg))

#define tm_ipc_init()              ((void)0)
#define tm_ipc_topic_init(t, n, tp, s) \
    (_TM_IPC_DISABLED_ARG(t),          \
     _TM_IPC_DISABLED_ARG(n),          \
     _TM_IPC_DISABLED_ARG(tp),         \
     _TM_IPC_DISABLED_ARG(s),          \
     (int)(-ENOSYS))
#define tm_ipc_topic_deinit(t)   \
    do {                         \
        _TM_IPC_DISABLED_ARG(t); \
    } while (0)
#define tm_ipc_sub_init(s, t, n, d) \
    (_TM_IPC_DISABLED_ARG(s),       \
     _TM_IPC_DISABLED_ARG(t),       \
     _TM_IPC_DISABLED_ARG(n),       \
     _TM_IPC_DISABLED_ARG(d),       \
     (int)(-ENOSYS))
#define tm_ipc_sub_deinit(s)     \
    do {                         \
        _TM_IPC_DISABLED_ARG(s); \
    } while (0)
#define tm_ipc_msg_alloc(t)      (_TM_IPC_DISABLED_ARG(t), NULL)
#define tm_ipc_msg_alloc_zero(t) (_TM_IPC_DISABLED_ARG(t), NULL)
#define tm_ipc_topic_publish(t, m) \
    (_TM_IPC_DISABLED_ARG(t), _TM_IPC_DISABLED_ARG(m), (int)TM_IPC_ERR_STATE)
#define tm_ipc_sub_recv(s, m, to) \
    (_TM_IPC_DISABLED_ARG(s),     \
     _TM_IPC_DISABLED_ARG(m),     \
     _TM_IPC_DISABLED_ARG(to),    \
     (int)TM_IPC_ERR_TIMEOUT)
#define tm_ipc_msg_release(m)    \
    do {                         \
        _TM_IPC_DISABLED_ARG(m); \
    } while (0)
#define tm_ipc_topic_stat(t, p, d) \
    do {                           \
        _TM_IPC_DISABLED_ARG(t);   \
        _TM_IPC_DISABLED_ARG(p);   \
        _TM_IPC_DISABLED_ARG(d);   \
    } while (0)
#define tm_ipc_slab_stat(a, b, c, d) \
    do {                             \
        _TM_IPC_DISABLED_ARG(a);     \
        _TM_IPC_DISABLED_ARG(b);     \
        _TM_IPC_DISABLED_ARG(c);     \
        _TM_IPC_DISABLED_ARG(d);     \
    } while (0)
#define tm_ipc_runtime_stat(s)   \
    do {                         \
        _TM_IPC_DISABLED_ARG(s); \
    } while (0)
#define tm_ipc_topic_foreach(v, a) \
    do {                           \
        _TM_IPC_DISABLED_ARG(v);   \
        _TM_IPC_DISABLED_ARG(a);   \
    } while (0)

#endif /* TM_IPC_ENABLE */

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
