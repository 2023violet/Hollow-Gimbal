/**
 * @file    rb_tree.h
 * @brief   Themis intrusive red-black tree
 * @version v3.0.0
 * @date    2026.02.28
 *
 * Design overview
 * ---------------
 * `rb_tree` is an intrusive red-black tree. Callers embed `struct rb_node`
 * inside their own objects and provide comparison callbacks for insert and
 * search.
 *
 * Usage example
 * -------------
 * struct rb_tree tree = {0};
 * rb_tree_init(&tree);
 * rb_tree_insert(&tree, &item->node, _cmp_node);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - Callers own node storage and key data.
 * - Parent linkage and color are stored explicitly for alignment-agnostic
 *   portability across 8/16/32/64-bit targets.
 * - Search helpers are inline; insert/remove remain in `.c`.
 * - @note Not thread-safe. Caller must ensure external serialization. Not
 *   ISR-safe.
 *
 * Blocking / allocation
 * ---------------------
 * - No allocation and no RTOS dependency.
 * - Tree operations are logarithmic when comparisons provide a stable order.
 *
 * Failure / recovery
 * ------------------
 * - Duplicate inserts are no-ops by comparison contract.
 * - Removal never frees user storage; callers remain responsible for node
 *   lifetime.
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

/**
 * @brief  Intrusive red-black tree node.
 *
 */
struct rb_node {
    /** Parent node, or NULL when this node is the root. */
    struct rb_node *parent;
    /** Left child containing smaller keys. */
    struct rb_node *left;
    /** Right child containing larger keys. */
    struct rb_node *right;
    /** Internal color bit: 0 = red, 1 = black. */
    uint8_t         color;
};

/**
 * @brief  Red-black tree root container.
 */
struct rb_tree {
    /** Root node pointer, or `NULL` when the tree is empty. */
    struct rb_node *root;
};

/* Public Inline Functions ---------------------------------------------------------------------- */

/**
 * @brief  Initialize one red-black tree to the empty state.
 *
 * @param[out] tree  Tree root container.
 */
static inline void rb_tree_init(struct rb_tree *tree)
{
    tree->root = NULL;
}

/**
 * @brief  Check whether one tree currently contains no nodes.
 *
 * @param[in] tree  Tree root container.
 *
 * @return `true` when the tree is empty; otherwise `false`.
 */
static inline bool rb_tree_is_empty(const struct rb_tree *tree)
{
    return tree->root == NULL;
}

/**
 * @brief  Search for one node by key.
 *
 * @param[in] tree  Tree root container.
 * @param[in] key   Opaque pointer to the search key, passed to `cmp()`.
 * @param[in] cmp   Comparison function:
 *                  - `cmp(node, key) < 0`: key is in `node`'s right subtree.
 *                  - `cmp(node, key) > 0`: key is in `node`'s left subtree.
 *                  - `cmp(node, key) == 0`: node matches key.
 *
 * @return Matching node pointer, or `NULL` when not found.
 *
 * @note   O(log n). Callers should use `rb_entry()` to recover the enclosing
 *         structure.
 */
static inline struct rb_node *rb_tree_search(const struct rb_tree *tree,
                                             const void           *key,
                                             int (*cmp)(const struct rb_node *, const void *))
{
    struct rb_node *x = tree->root;

    while (x != NULL) {
        int c = cmp(x, key);

        if (c < 0) {
            x = x->right;
        } else if (c > 0) {
            x = x->left;
        } else {
            return x;
        }
    }

    return NULL;
}

/**
 * @brief  Return the node with the smallest key.
 *
 * @param[in] tree  Tree root container.
 *
 * @return Minimum node, or `NULL` when the tree is empty.
 */
static inline struct rb_node *rb_tree_first(const struct rb_tree *tree)
{
    struct rb_node *n = tree->root;

    if (n == NULL) {
        return NULL;
    }

    while (n->left != NULL) {
        n = n->left;
    }

    return n;
}

/**
 * @brief  Return the node with the largest key.
 *
 * @param[in] tree  Tree root container.
 *
 * @return Maximum node, or `NULL` when the tree is empty.
 */
static inline struct rb_node *rb_tree_last(const struct rb_tree *tree)
{
    struct rb_node *n = tree->root;

    if (n == NULL) {
        return NULL;
    }

    while (n->right != NULL) {
        n = n->right;
    }

    return n;
}

/* Public Functions ----------------------------------------------------------------------------- */

/**
 * @brief  Insert one node into the tree.
 *
 * @param[in,out] tree  Tree root container.
 * @param[in,out] node  Embedded node owned by the caller.
 * @param[in]     cmp   Comparison function between two nodes:
 *                      - `cmp(new_node, existing_node) < 0`: `new_node` goes left.
 *                      - `cmp(new_node, existing_node) > 0`: `new_node` goes right.
 *                      - `cmp(new_node, existing_node) == 0`: duplicate, no-op.
 *
 * @note   O(log n). No dynamic memory is allocated.
 */
void rb_tree_insert(struct rb_tree *tree,
                    struct rb_node *node,
                    int (*cmp)(const struct rb_node *, const struct rb_node *));

/**
 * @brief  Remove one node from the tree.
 *
 * @param[in,out] tree  Tree root container.
 * @param[in,out] node  Node to remove. It must currently belong to this tree.
 *
 * @note   O(log n). This helper does not free memory; the caller still owns
 *         the removed node storage.
 */
void rb_tree_remove(struct rb_tree *tree, struct rb_node *node);

/**
 * @brief  Find the lower bound, the smallest node whose key is greater than
 *         or equal to the requested key.
 *
 * @param[in] tree  Tree root container.
 * @param[in] key   Opaque pointer to the search key.
 * @param[in] cmp   Same comparison function shape as `rb_tree_search()`.
 *
 * @return Lower-bound node, or `NULL` when every key is smaller than `key`.
 */
struct rb_node *rb_tree_lower_bound(const struct rb_tree *tree,
                                    const void           *key,
                                    int (*cmp)(const struct rb_node *, const void *));

/**
 * @brief  Advance to the in-order successor.
 *
 * @param[in] node  Current node.
 *
 * @return Successor node, or `NULL` when `node` is already the maximum.
 */
struct rb_node *rb_node_next(const struct rb_node *node);

/**
 * @brief  Move to the in-order predecessor.
 *
 * @param[in] node  Current node.
 *
 * @return Predecessor node, or `NULL` when `node` is already the minimum.
 */
struct rb_node *rb_node_prev(const struct rb_node *node);

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief  Recover the enclosing structure from an embedded `rb_node`.
 *
 * @param[in] ptr     Pointer to the embedded `rb_node`.
 * @param[in] type    Type of the enclosing structure.
 * @param[in] member  Name of the `rb_node` member inside `type`.
 */
#define rb_entry(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/**
 * @brief  Iterate over all nodes in ascending key order.
 *
 * @param[in,out] pos   `struct rb_node *` loop cursor.
 * @param[in]     tree  Tree to iterate.
 *
 * @warning Do not remove the current node inside this loop. Use a safe
 *          traversal helper when mutation is required.
 */
#define rb_tree_for_each(pos, tree) \
    for ((pos) = rb_tree_first(tree); (pos) != NULL; (pos) = rb_node_next(pos))

/**
 * @brief  Iterate in ascending order while caching the successor node.
 *
 * @param[in,out] pos    `struct rb_node *` loop cursor.
 * @param[in,out] next   Cached successor cursor.
 * @param[in]     tree   Tree to iterate.
 */
#define rb_tree_for_each_safe(pos, next, tree)                                                \
    for ((pos) = rb_tree_first(tree), (next) = ((pos) != NULL ? rb_node_next(pos) : NULL);   \
         (pos) != NULL;                                                                        \
         (pos) = (next), (next) = ((pos) != NULL ? rb_node_next(pos) : NULL))

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
