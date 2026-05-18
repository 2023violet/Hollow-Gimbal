/**
 * @file    pubsub.h
 * @brief   Zero-allocation synchronous publish-subscribe event bus
 * @version v1.0.0
 * @date    2026.04.24
 *
 * Design overview
 * ---------------
 * `pubsub` is an intrusive synchronous event bus. The bus owns no subscriber
 * storage: callers provide the topic-head array at init time and each
 * subscriber owns its own `struct pubsub_node`.
 *
 * Usage example
 * -------------
 * static struct pubsub_node *topic_heads[TOPIC_COUNT];
 * static struct pubsub_bus   g_bus;
 *
 * pubsub_init(&g_bus, topic_heads, TOPIC_COUNT);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - Subscriber nodes are caller-owned and must outlive the active subscription.
 * - The same bus instance must be used from one execution context or be
 *   serialized externally.
 * - `pubsub_publish()` invokes callbacks synchronously in the publisher's
 *   current execution context.
 * - @note Not thread-safe. Caller must ensure external serialization. Not
 *   ISR-safe.
 *
 * Blocking / allocation
 * ---------------------
 * - No allocation and no internal queueing.
 * - Publish walks only the topic-local subscriber list.
 * - Mutation-safe iteration caches `next` before invoking callbacks, so a
 *   callback may unsubscribe itself safely.
 *
 * Failure / recovery
 * ------------------
 * - Invalid initialization and subscription requests fail with `-EINVAL`.
 * - For cross-task delivery, buffered fanout, or ISR-to-task transport, use a
 *   queueing subsystem instead of extending this synchronous component.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types --------------------------------------------------------------------------------- */

/* Opaque publish-frame node used only while a publish call is in flight. */
struct pubsub_publish_frame;

/**
 * @brief  Subscriber callback signature.
 *
 * @param  topic      Published topic ID.
 * @param  payload    Publisher-owned payload pointer. The bus does not copy
 *                    the payload, and subscribers must not retain it after the
 *                    callback returns.
 * @param  user_data  Opaque context pointer provided at subscribe() time.
 */
typedef void (*pubsub_callback_t)(uint16_t topic, const void *payload, void *user_data);

/**
 * @brief  Subscriber node owned by the subscribing module.
 *
 * Each active subscription requires its own node instance. Nodes must be
 * zero-initialized before first use.
 */
struct pubsub_node {
    /**
     * @brief  Topic ID this node is currently subscribed to.
     */
    uint16_t topic_id;

    /**
     * @brief  Callback invoked when the topic is published.
     */
    pubsub_callback_t callback;

    /**
     * @brief  Opaque context forwarded to @ref callback.
     */
    void *user_data;

    /**
     * @brief  Intrusive next pointer managed by the bus.
     */
    struct pubsub_node *next;
};

/**
 * @brief  Event bus control block.
 *
 * The bus allocates nothing; all routing storage is caller-provided.
 */
struct pubsub_bus {
    /**
     * @brief  Array of topic-head pointers indexed by topic ID.
     */
    struct pubsub_node **topic_heads;

    /**
     * @brief  Number of topics supported by @ref topic_heads.
     */
    size_t max_topics;

    /**
     * @brief  Internal publish-stack head used to keep callback mutation safe.
     *
     * Callers must treat this as opaque implementation state.
     */
    struct pubsub_publish_frame *active_frame;
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief  Initialize an event bus.
 *
 * The topic-head array is zeroed during initialization.
 *
 * @param  bus          Bus control block to initialize.
 * @param  topic_heads  Caller-supplied topic-head array. Must remain valid for
 *                      the bus lifetime.
 * @param  max_topics   Number of entries in @p topic_heads. Must be non-zero.
 *
 * @retval 0        Initialization succeeded.
 * @retval -EINVAL  Any pointer is NULL or @p max_topics is 0.
 * @retval -EBUSY   The bus is already active and still owns subscriber links.
 */
int pubsub_init(struct pubsub_bus *bus, struct pubsub_node **topic_heads, size_t max_topics);

/* Subscription Management ---------------------------------------------------------------------- */

/**
 * @brief  Subscribe a node to a topic.
 *
 * The node is inserted at the head of the topic list, so the most recently
 * subscribed node runs first.
 *
 * @param  bus       Initialized bus.
 * @param  node      Caller-owned subscriber node. Must remain valid until
 *                   pubsub_unsubscribe() succeeds.
 * @param  topic_id  Topic to subscribe to. Must be less than `bus->max_topics`.
 * @param  callback  Callback invoked for publishes on the topic.
 * @param  user_data Opaque context forwarded to @p callback.
 *
 * @retval 0        Subscribe succeeded.
 * @retval -EINVAL  Any required pointer is NULL or @p callback is NULL.
 * @retval -ERANGE  @p topic_id is outside the configured topic range.
 * @retval -EBUSY   @p node is already active or still carries stale link state.
 */
int pubsub_subscribe(struct pubsub_bus  *bus,
                     struct pubsub_node *node,
                     uint16_t            topic_id,
                     pubsub_callback_t   callback,
                     void               *user_data);

/**
 * @brief  Unsubscribe a node from its current topic.
 *
 * Safe to call from the node's own callback because publish iteration caches
 * the next pointer before invoking each subscriber.
 *
 * @param  bus   Initialized bus. Must not be NULL.
 * @param  node  Node to unsubscribe. NULL is ignored.
 *
 * @retval 0        Unsubscribe succeeded, including the already-inactive case.
 * @retval -EINVAL  @p bus is NULL.
 */
int pubsub_unsubscribe(struct pubsub_bus *bus, struct pubsub_node *node);

/* Publish / Query ------------------------------------------------------------------------------ */

/**
 * @brief  Publish an event synchronously to all subscribers of a topic.
 *
 * The payload pointer is forwarded as-is to each callback. Payload lifetime
 * must therefore cover the full publish call. Subscribers added during a
 * publish are not visited until a later publish call.
 *
 * @param  bus       Initialized bus.
 * @param  topic_id  Topic to publish. Must be less than `bus->max_topics`.
 * @param  payload   Publisher-owned payload pointer. May be NULL.
 *
 * @return Non-negative callback count on success, or negative `-errno` on
 *         failure.
 */
int pubsub_publish(struct pubsub_bus *bus, uint16_t topic_id, const void *payload);

/**
 * @brief  Return the number of active subscribers for one topic.
 *
 * This is an O(N) diagnostic helper over the topic-local subscriber list.
 *
 * @param  bus       Initialized bus.
 * @param  topic_id  Topic to query.
 *
 * @return Non-negative subscriber count on success, or negative `-errno` on
 *         failure.
 */
int pubsub_subscriber_count(const struct pubsub_bus *bus, uint16_t topic_id);

/* Inline Helpers ------------------------------------------------------------------------------- */

/**
 * @brief  Return whether a node is currently subscribed.
 *
 * `callback != NULL` is the authoritative active-subscription marker.
 *
 * @param  node  Subscriber node to query.
 *
 * @return `true` when the node is active; otherwise `false`.
 */
static inline bool pubsub_is_subscribed(const struct pubsub_node *node)
{
    if (node == NULL) {
        return false;
    }

    return node->callback != NULL;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
