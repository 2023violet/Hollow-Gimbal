/**
 * @file    pubsub.c
 * @brief   Zero-allocation synchronous publish-subscribe implementation
 * @version v1.0.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `pubsub.h`
 * - owns intrusive list splicing, mutation-safe publish iteration, and
 *   defensive node reset rules
 *
 * Key collaborators
 * -----------------
 * - `pubsub.h`: caller-facing ownership and synchronous-delivery contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "pubsub.h"
#include <errno.h>
#include <string.h>

/* Internal Helpers ----------------------------------------------------------------------------- */

struct pubsub_publish_frame {
    struct pubsub_publish_frame *prev;
    struct pubsub_node         **cursor_link;
    struct pubsub_node          *current;
};

static bool _pubsub_bus_pre_init_state_is_valid(const struct pubsub_bus *bus)
{
    if (bus == NULL) {
        return false;
    }

    return (((bus->topic_heads == NULL) && (bus->max_topics == 0U) && (bus->active_frame == NULL)) ||
            ((bus->topic_heads != NULL) && (bus->max_topics != 0U)));
}

static bool _pubsub_bus_has_active_subscriptions(const struct pubsub_bus *bus)
{
    size_t topic_id;

    if ((bus == NULL) || (bus->topic_heads == NULL) || (bus->max_topics == 0U)) {
        return false;
    }

    for (topic_id = 0U; topic_id < bus->max_topics; topic_id++) {
        if (bus->topic_heads[topic_id] != NULL) {
            return true;
        }
    }

    return false;
}

static void _pubsub_node_reset(struct pubsub_node *node)
{
    if (node == NULL) {
        return;
    }

    node->next     = NULL;
    node->callback = NULL;

    /*
     * Clearing user_data makes reused-node bugs fail earlier instead of
     * silently preserving a stale owner pointer.
     */
    node->user_data = NULL;
}

/* Public Functions ----------------------------------------------------------------------------- */

int pubsub_init(struct pubsub_bus *bus, struct pubsub_node **topic_heads, size_t max_topics)
{
    if ((bus == NULL) || (topic_heads == NULL) || (max_topics == 0U)) {
        return -EINVAL;
    }

    if (!_pubsub_bus_pre_init_state_is_valid(bus)) {
        return -EINVAL;
    }

    /*
     * Re-initialization is only safe when the old bus is empty. Reject first
     * so callers do not lose the existing routing topology on failure.
     */
    if (_pubsub_bus_has_active_subscriptions(bus)) {
        return -EBUSY;
    }

    memset(topic_heads, 0, max_topics * sizeof(*topic_heads));

    bus->topic_heads = topic_heads;
    bus->max_topics  = max_topics;
    bus->active_frame = NULL;

    return 0;
}

int pubsub_subscribe(struct pubsub_bus  *bus,
                     struct pubsub_node *node,
                     uint16_t            topic_id,
                     pubsub_callback_t   callback,
                     void               *user_data)
{
    if ((bus == NULL) || (node == NULL) || (callback == NULL)) {
        return -EINVAL;
    }

    if ((size_t)topic_id >= bus->max_topics) {
        return -ERANGE;
    }

    /*
     * `callback != NULL` is the authoritative active marker.
     * `next != NULL` catches stale intrusive-link state on reused nodes.
     */
    if ((node->callback != NULL) || (node->next != NULL)) {
        return -EBUSY;
    }

    node->topic_id  = topic_id;
    node->callback  = callback;
    node->user_data = user_data;

    /* Head insertion keeps subscription O(1). */
    node->next                 = bus->topic_heads[topic_id];
    bus->topic_heads[topic_id] = node;

    return 0;
}

int pubsub_unsubscribe(struct pubsub_bus *bus, struct pubsub_node *node)
{
    struct pubsub_node **pp;
    uint16_t             topic_id;

    if (bus == NULL) {
        return -EINVAL;
    }

    if ((node == NULL) || (node->callback == NULL)) {
        return 0;
    }

    topic_id = node->topic_id;
    if ((size_t)topic_id >= bus->max_topics) {
        _pubsub_node_reset(node);
        return 0;
    }

    /*
     * Pointer-to-pointer traversal keeps removal logic uniform for both the
     * head node and interior nodes.
     */
    pp = &bus->topic_heads[topic_id];

    while (*pp != NULL) {
        if (*pp == node) {
            struct pubsub_node *successor = node->next;
            struct pubsub_publish_frame *frame = bus->active_frame;

            *pp = successor;

            while (frame != NULL) {
                if (frame->current == node) {
                    frame->current = successor;
                }
                if (frame->cursor_link == &node->next) {
                    frame->cursor_link = pp;
                }
                frame = frame->prev;
            }

            _pubsub_node_reset(node);
            return 0;
        }
        pp = &(*pp)->next;
    }

    _pubsub_node_reset(node);
    return 0;
}

int pubsub_publish(struct pubsub_bus *bus, uint16_t topic_id, const void *payload)
{
    int                         dispatched = 0;
    struct pubsub_publish_frame frame;

    if (bus == NULL) {
        return -EINVAL;
    }

    if ((size_t)topic_id >= bus->max_topics) {
        return -ERANGE;
    }

    frame.prev        = bus->active_frame;
    frame.cursor_link = &bus->topic_heads[topic_id];
    frame.current     = *frame.cursor_link;
    bus->active_frame = &frame;

    while (frame.current != NULL) {
        struct pubsub_node *node = frame.current;

        frame.cursor_link = &node->next;
        frame.current     = node->next;

        if (node->callback != NULL) {
            node->callback(topic_id, payload, node->user_data);
            dispatched++;
        }
    }

    bus->active_frame = frame.prev;
    return dispatched;
}

int pubsub_subscriber_count(const struct pubsub_bus *bus, uint16_t topic_id)
{
    int count = 0;

    if (bus == NULL) {
        return -EINVAL;
    }

    if ((size_t)topic_id >= bus->max_topics) {
        return -ERANGE;
    }

    for (const struct pubsub_node *n = bus->topic_heads[topic_id]; n != NULL; n = n->next) {
        count++;
    }

    return count;
}

/* ---------------------------------------- end of file ----------------------------------------- */
