/**
 * @file    hash_map.c
 * @brief   Intrusive static hash map implementation
 * @version v1.1.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `hash_map.h`
 * - owns slot selection, bucket walks, and intrusive remove/reset details
 * - keeps the component free of mutable global state outside caller-owned maps
 *
 * Key collaborators
 * -----------------
 * - `hash_map.h`: caller-facing ownership and slist-backend contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "hash_map.h"
#include <errno.h>
#include <string.h>

/* Private Helpers ------------------------------------------------------------------------------ */

/**
 * @brief  Return whether @p x is a non-zero power of two.
 *
 * @param  x  Value to test.
 *
 * @return `true` when @p x is a non-zero power of two; otherwise `false`.
 */
static inline bool _is_power_of_two(size_t x)
{
    return (x != 0U) && ((x & (x - 1U)) == 0U);
}

/**
 * @brief  Map a 32-bit hash value to a bucket index.
 *
 * @param  map   Initialized hash map.
 * @param  hash  Raw hash value.
 *
 * @return Slot index in `[0, bucket_count)`.
 */
static inline size_t _slot(const struct hash_map *map, uint32_t hash)
{
    if (_is_power_of_two(map->bucket_count)) {
        return (size_t)(hash & (uint32_t)(map->bucket_count - 1U));
    }

    return (size_t)(hash % map->bucket_count);
}

/**
 * @brief  Return whether the exact intrusive node is already linked in the map.
 *
 * This guard closes a subtle corruption hole: callers may accidentally mutate
 * `node->key` after insertion and call hash_map_put() again. Key-based
 * duplicate detection alone would miss that stale reuse and splice one node
 * into two bucket chains.
 */
static bool _map_contains_node(const struct hash_map *map, const struct hash_node *needle)
{
    for (size_t bucket_index = 0U; bucket_index < map->bucket_count; ++bucket_index) {
        sys_snode_t *cur = sys_slist_peek_head(&map->buckets[bucket_index]);

        while (cur != NULL) {
            if (cur == &needle->snode) {
                return true;
            }
            cur = sys_slist_peek_next(cur);
        }
    }

    return false;
}

/**
 * @brief  Search one bucket chain for a key match.
 *
 * The search is intentionally two-phase:
 * 1. Compare cached 32-bit hash values.
 * 2. Only on a hash match, call the byte-level key comparator.
 *
 * This keeps miss cost low even for long keys.
 *
 * @param  map      Hash map that owns the key comparator.
 * @param  bucket   Bucket list to search.
 * @param  key      Search key.
 * @param  key_len  Search key length in bytes.
 * @param  hash     Pre-computed hash of (`key`, `key_len`).
 *
 * @return Matching node pointer on success, or `NULL` when the bucket does not
 *         contain the key.
 */
static struct hash_node *_bucket_find(const struct hash_map *map,
                                      sys_slist_t           *bucket,
                                      const void            *key,
                                      size_t                 key_len,
                                      uint32_t               hash)
{
    sys_snode_t *cur = sys_slist_peek_head(bucket);

    while (cur != NULL) {
        struct hash_node *node = (struct hash_node *)cur;

        if (node->hash == hash) {
            if (map->key_cmp(node->key, node->key_len, key, key_len)) {
                return node;
            }
        }

        cur = sys_slist_peek_next(cur);
    }

    return NULL;
}

/* Public Functions ----------------------------------------------------------------------------- */

int hash_map_init(struct hash_map *map,
                  sys_slist_t     *buckets,
                  size_t           bucket_count,
                  hash_func_t      hash_func,
                  hash_cmp_t       key_cmp)
{
    if (map == NULL || buckets == NULL || hash_func == NULL || key_cmp == NULL) {
        return -EINVAL;
    }

    if (bucket_count == 0U) {
        return -EINVAL;
    }

    map->buckets      = buckets;
    map->bucket_count = bucket_count;
    map->node_count   = 0U;
    map->hash_func    = hash_func;
    map->key_cmp      = key_cmp;

    for (size_t i = 0U; i < bucket_count; i++) {
        sys_slist_init(&buckets[i]);
    }

    return 0;
}

int hash_map_put(struct hash_map *map, struct hash_node *node)
{
    size_t       slot;
    sys_slist_t *bucket;

    if (map == NULL || node == NULL || node->key == NULL) {
        return -EINVAL;
    }

    if (_map_contains_node(map, node)) {
        return -EBUSY;
    }

    node->hash = map->hash_func(node->key, node->key_len);

    slot   = _slot(map, node->hash);
    bucket = &map->buckets[slot];

    if (_bucket_find(map, bucket, node->key, node->key_len, node->hash) != NULL) {
        return -EEXIST;
    }

    /* Prepend so insertion stays O(1). */
    sys_slist_prepend(bucket, &node->snode);
    map->node_count++;

    return 0;
}

struct hash_node *hash_map_get(const struct hash_map *map, const void *key, size_t key_len)
{
    uint32_t     hash;
    size_t       slot;
    sys_slist_t *bucket;

    if (map == NULL || key == NULL) {
        return NULL;
    }

    hash   = map->hash_func(key, key_len);
    slot   = _slot(map, hash);
    bucket = &map->buckets[slot];

    /*
     * `map` is const, but the bucket list storage is still caller-owned mutable
     * data. The slist helpers require non-const access for traversal, and that
     * does not violate the public read-only lookup contract.
     */
    return _bucket_find(map, bucket, key, key_len, hash);
}

int hash_map_remove(struct hash_map *map, const void *key, size_t key_len)
{
    uint32_t     hash;
    size_t       slot;
    sys_slist_t *bucket;
    sys_snode_t *prev = NULL;
    sys_snode_t *cur;

    if (map == NULL || key == NULL) {
        return -EINVAL;
    }

    hash   = map->hash_func(key, key_len);
    slot   = _slot(map, hash);
    bucket = &map->buckets[slot];
    cur    = sys_slist_peek_head(bucket);

    while (cur != NULL) {
        struct hash_node *node = (struct hash_node *)cur;

        if ((node->hash == hash) && map->key_cmp(node->key, node->key_len, key, key_len)) {
            /*
             * `sys_slist_remove()` handles both head removal (`prev == NULL`)
             * and interior removal (`prev != NULL`) with one call.
             */
            sys_slist_remove(bucket, prev, cur);

            /* Reset the intrusive link so stale reuse is easier to spot. */
            node->snode.next = NULL;

            map->node_count--;
            return 0;
        }

        prev = cur;
        cur  = sys_slist_peek_next(cur);
    }

    return -ENOENT;
}

void hash_map_clear(struct hash_map *map)
{
    if (map == NULL) {
        return;
    }

    for (size_t i = 0U; i < map->bucket_count; i++) {
        sys_slist_t *bucket = &map->buckets[i];
        sys_snode_t *cur    = sys_slist_peek_head(bucket);

        while (cur != NULL) {
            sys_snode_t *next = sys_slist_peek_next(cur);

            /*
             * Poison the stale link before re-initializing the list so any
             * later misuse fails in a visible way instead of silently
             * following an old next pointer.
             */
            cur->next = NULL;
            cur       = next;
        }

        sys_slist_init(bucket);
    }

    map->node_count = 0U;
}

void hash_map_foreach(struct hash_map *map, hash_iter_fn_t fn, void *user_data)
{
    if (map == NULL || fn == NULL) {
        return;
    }

    for (size_t i = 0U; i < map->bucket_count; i++) {
        sys_snode_t *cur = sys_slist_peek_head(&map->buckets[i]);

        while (cur != NULL) {
            sys_snode_t      *next = sys_slist_peek_next(cur);
            struct hash_node *node = (struct hash_node *)cur;

            /*
             * Snapshot `next` before invoking the callback so the iterator does
             * not rely on `cur->next` staying stable across callback execution.
             * The public API still forbids mutating the map from inside `fn`.
             */
            if (!fn(node, user_data)) {
                return;
            }

            cur = next;
        }
    }
}

/* Built-In Hash Functions ---------------------------------------------------------------------- */

uint32_t hash_fnv1a_32(const void *key, size_t key_len)
{
    static const uint32_t FNV_OFFSET_BASIS = 2166136261U;
    static const uint32_t FNV_PRIME        = 16777619U;

    const uint8_t *bytes = (const uint8_t *)key;
    uint32_t       hash  = FNV_OFFSET_BASIS;

    for (size_t i = 0U; i < key_len; i++) {
        hash ^= (uint32_t)bytes[i];
        hash *= FNV_PRIME;
    }

    return hash;
}

/* Built-In Comparison Functions ---------------------------------------------------------------- */

bool hash_cmp_bytes(const void *key1, size_t len1, const void *key2, size_t len2)
{
    if (len1 != len2) {
        return false;
    }

    return memcmp(key1, key2, len1) == 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
