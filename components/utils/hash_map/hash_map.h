/**
 * @file    hash_map.h
 * @brief   Intrusive static hash map with caller-owned storage
 * @version v1.1.0
 * @date    2026.04.24
 *
 * Design overview
 * ---------------
 * `hash_map` is a separate-chaining intrusive hash map built on `slist.h`.
 * The component never allocates memory: callers own the bucket array, the map
 * control block, and the user objects that embed `struct hash_node`.
 *
 * Usage example
 * -------------
 * HASH_MAP_DEFINE(g_param_map, 32);
 * hash_map_init(&g_param_map, _g_param_map_buckets, 32, hash_fnv1a_32, hash_cmp_bytes);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - Bucket arrays and embedded nodes stay caller-owned for the full map lifetime.
 * - `bucket_count` must be non-zero. Arbitrary positive bucket counts are
 *   supported; power-of-two counts may use a mask fast path internally, while
 *   other counts fall back to modulo.
 * - One execution context must own each map instance unless callers serialize
 *   access externally.
 * - @note Not thread-safe. Caller must ensure external serialization. Not
 *   ISR-safe.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking, no allocation, and no RTOS dependency.
 * - Slot selection uses a mask fast path for power-of-two bucket counts and
 *   a modulo fallback for arbitrary bucket counts.
 * - Lookup is two-phase: cached hash first, key compare second.
 *
 * Failure / recovery
 * ------------------
 * - Invalid init and operation contracts fail with `-EINVAL`.
 * - Reinserting a node that is still linked in the same map fails with
 *   `-EBUSY` so stale key mutation cannot splice one intrusive node into two
 *   bucket chains.
 * - Removed nodes reset their intrusive link to `NULL` so stale reuse is
 *   easier to detect during diagnostics.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "slist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief  Recover the enclosing struct pointer from an embedded member pointer.
 *
 * Mirrors the Linux kernel `container_of` idiom. The `offsetof()` subtraction
 * is required whenever `struct hash_node` is not the first member.
 *
 * @param  ptr     Pointer to the embedded member.
 * @param  type    Enclosing structure type.
 * @param  member  Embedded member name inside @p type.
 */
#define HASH_ENTRY(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/**
 * @brief  Declare a static hash map and its backing bucket array in one step.
 *
 * This macro only allocates static storage. Callers must still invoke
 * hash_map_init() before first use.
 *
 * @param  name          Identifier of the `struct hash_map` instance.
 * @param  bucket_count  Number of buckets. Must be non-zero.
 */
#define HASH_MAP_DEFINE(name, bucket_count)                   \
    static sys_slist_t     _##name##_buckets[(bucket_count)]; \
    static struct hash_map name

/**
 * @brief  Register every descriptor entry in @p array into @p map_ptr.
 *
 * This helper expects each element of @p array to expose:
 * - `hnode` as an embedded `struct hash_node`
 * - `name` as the key string
 *
 * The macro writes `hnode.key` and `hnode.key_len`, then inserts the node with
 * hash_map_put().
 *
 * @param  map_ptr  Pointer to an initialized hash map.
 * @param  array    Statically declared descriptor array.
 */
#define HASH_MAP_REGISTER(map_ptr, array)                         \
    do {                                                          \
        size_t _n = sizeof(array) / sizeof((array)[0]);           \
        for (size_t _i = 0U; _i < _n; _i++) {                     \
            (array)[_i].hnode.key     = (array)[_i].name;         \
            (array)[_i].hnode.key_len = strlen((array)[_i].name); \
            hash_map_put((map_ptr), &(array)[_i].hnode);          \
        }                                                         \
    } while (0)

/* Public Types --------------------------------------------------------------------------------- */

struct hash_node;

/**
 * @brief  Hash function callback.
 *
 * @param  key      Pointer to the key bytes. Must not be NULL.
 * @param  key_len  Key length in bytes. May be 0.
 *
 * @return 32-bit hash value. Zero is a legal result.
 */
typedef uint32_t (*hash_func_t)(const void *key, size_t key_len);

/**
 * @brief  Key equality callback used after a hash collision is detected.
 *
 * @param  key1  First key bytes.
 * @param  len1  Length of @p key1 in bytes.
 * @param  key2  Second key bytes.
 * @param  len2  Length of @p key2 in bytes.
 *
 * @return `true` when the two keys are logically equal; otherwise `false`.
 */
typedef bool (*hash_cmp_t)(const void *key1, size_t len1, const void *key2, size_t len2);

/**
 * @brief  Visitor callback used by hash_map_foreach().
 *
 * @param  node       Current node being visited. Never NULL.
 * @param  user_data  Opaque caller context forwarded unchanged.
 *
 * @return `true` to continue iteration, or `false` to stop early.
 */
typedef bool (*hash_iter_fn_t)(struct hash_node *node, void *user_data);

/**
 * @brief  Intrusive node embedded in each user object stored in the map.
 *
 * Before hash_map_put(), callers must populate `key` and `key_len`. The map
 * computes `hash` automatically.
 */
struct hash_node {
    /**
     * @brief  Intrusive single-linked-list node used by the bucket chain.
     */
    sys_snode_t snode;

    /**
     * @brief  Pointer to caller-owned key data.
     *
     * The pointed-to storage must remain valid while the node stays in the map.
     */
    const void *key;

    /**
     * @brief  Length of @ref key in bytes.
     */
    size_t key_len;

    /**
     * @brief  Cached hash value written by hash_map_put().
     */
    uint32_t hash;
};

/**
 * @brief  Hash map control block.
 *
 * All fields are owned by the hash_map API after initialization. Callers
 * provide the storage but should not write these fields directly.
 */
struct hash_map {
    /**
     * @brief  Caller-supplied bucket array with `bucket_count` entries.
     */
    sys_slist_t *buckets;

    /**
     * @brief  Number of buckets in @ref buckets.
     */
    size_t bucket_count;

    /**
     * @brief  Number of nodes currently stored in the map.
     */
    size_t node_count;

    /**
     * @brief  Hash computation callback.
     */
    hash_func_t hash_func;

    /**
     * @brief  Key equality callback used after hash match.
     */
    hash_cmp_t key_cmp;
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief  Initialize a hash map control block.
 *
 * @param  map           Control block to initialize. Must not be NULL.
 * @param  buckets       Caller-allocated bucket array. Must not be NULL and
 *                       must remain valid for the full map lifetime.
 * @param  bucket_count  Number of elements in @p buckets. Must be non-zero.
 * @param  hash_func     Hash computation callback. Must not be NULL.
 * @param  key_cmp       Key equality callback. Must not be NULL.
 *
 * @retval 0        Initialization succeeded.
 * @retval -EINVAL  Any pointer is NULL or @p bucket_count is zero.
 */
int hash_map_init(struct hash_map *map,
                  sys_slist_t     *buckets,
                  size_t           bucket_count,
                  hash_func_t      hash_func,
                  hash_cmp_t       key_cmp);

/* Mutation ------------------------------------------------------------------------------------- */

/**
 * @brief  Insert a node into the map.
 *
 * @param  map   Initialized hash map. Must not be NULL.
 * @param  node  Node to insert. Must not be NULL. `node->key` must already be
 *               populated by the caller and must not be NULL. The node must
 *               not already be linked in this map; remove it before mutating
 *               the key or attempting reinsertion.
 *
 * @retval 0        Insert succeeded.
 * @retval -EINVAL  @p map or @p node is NULL, or `node->key` is NULL.
 * @retval -EBUSY   The same intrusive node is already linked in this map.
 * @retval -EEXIST  A node with the same key is already present.
 */
int hash_map_put(struct hash_map *map, struct hash_node *node);

/**
 * @brief  Remove a node identified by key.
 *
 * @param  map      Initialized hash map. Must not be NULL.
 * @param  key      Key bytes to remove. Must not be NULL.
 * @param  key_len  Length of @p key in bytes.
 *
 * @retval 0        Remove succeeded and ownership of the removed node returns
 *                  to the caller.
 * @retval -EINVAL  @p map or @p key is NULL.
 * @retval -ENOENT  No node with the requested key exists.
 */
int hash_map_remove(struct hash_map *map, const void *key, size_t key_len);

/**
 * @brief  Remove all nodes and reset the map to an empty state.
 *
 * Node memory is not freed; callers still own every node instance.
 *
 * @param  map  Initialized hash map. NULL is silently ignored.
 */
void hash_map_clear(struct hash_map *map);

/* Query ---------------------------------------------------------------------------------------- */

/**
 * @brief  Look up a node by key.
 *
 * The lookup performs two-phase matching: cached hash first, then `key_cmp()`.
 *
 * @param  map      Initialized hash map. Must not be NULL.
 * @param  key      Search key bytes. Must not be NULL.
 * @param  key_len  Length of @p key in bytes.
 *
 * @return Matching node pointer on success, or `NULL` when not found or when
 *         the contract is invalid.
 */
struct hash_node *hash_map_get(const struct hash_map *map, const void *key, size_t key_len);

/**
 * @brief  Visit every node currently stored in the map.
 *
 * Iteration stops early when @p fn returns false. Callers must not mutate the
 * map from inside the callback.
 *
 * @param  map        Initialized hash map. Must not be NULL.
 * @param  fn         Visitor callback. Must not be NULL.
 * @param  user_data  Opaque value forwarded to @p fn.
 */
void hash_map_foreach(struct hash_map *map, hash_iter_fn_t fn, void *user_data);

/* Inline Helpers ------------------------------------------------------------------------------- */

/**
 * @brief  Return the number of nodes currently stored in the map.
 *
 * @param  map  Initialized hash map. May be NULL.
 *
 * @return Node count, or 0 when @p map is NULL.
 */
static inline size_t hash_map_size(const struct hash_map *map)
{
    return (map != NULL) ? map->node_count : 0U;
}

/**
 * @brief  Return whether the map is empty.
 *
 * @param  map  Initialized hash map. May be NULL.
 *
 * @return `true` when the map has no nodes or @p map is NULL; otherwise
 *         `false`.
 */
static inline bool hash_map_is_empty(const struct hash_map *map)
{
    return (map == NULL) || (map->node_count == 0U);
}

/**
 * @brief  Test whether a key exists without retrieving the stored node.
 *
 * @param  map      Initialized hash map. Must not be NULL.
 * @param  key      Search key bytes. Must not be NULL.
 * @param  key_len  Length of @p key in bytes.
 *
 * @return `true` when a node with the key exists; otherwise `false`.
 */
static inline bool hash_map_contains(const struct hash_map *map, const void *key, size_t key_len)
{
    return hash_map_get(map, key, key_len) != NULL;
}

/* Built-In Hash Functions ---------------------------------------------------------------------- */

/**
 * @brief  FNV-1a 32-bit hash with small code size and good general-purpose distribution.
 *
 * Suitable for strings, fixed-width integers treated as bytes, and arbitrary
 * binary keys.
 *
 * @param  key      Pointer to key bytes. Must not be NULL.
 * @param  key_len  Number of bytes to hash. May be 0.
 *
 * @return 32-bit FNV-1a digest.
 */
uint32_t hash_fnv1a_32(const void *key, size_t key_len);

/* Built-In Comparison Functions ---------------------------------------------------------------- */

/**
 * @brief  Compare two keys byte-for-byte.
 *
 * @param  key1  First key bytes.
 * @param  len1  Length of @p key1 in bytes.
 * @param  key2  Second key bytes.
 * @param  len2  Length of @p key2 in bytes.
 *
 * @return `true` when the keys are byte-identical; otherwise `false`.
 */
bool hash_cmp_bytes(const void *key1, size_t len1, const void *key2, size_t len2);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
