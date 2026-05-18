/**
 * @file    rb_tree.c
 * @brief   Intrusive red-black tree with traversal support
 * @version v3.0.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `rb_tree.h`
 * - owns rebalancing, rotations, and explicit parent/color manipulation
 * - keeps intrusive storage and ordering under caller-provided comparisons
 *
 * Key collaborators
 * -----------------
 * - `rb_tree.h`: caller-facing traversal and ownership contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "rb_tree.h"

/* Private Defines ------------------------------------------------------------------------------ */

#define RB_RED   (0U) /* Color bit value for red. */
#define RB_BLACK (1U) /* Color bit value for black. */

#define _RB_PARENT(n) ((n)->parent)
#define _RB_COLOR(n)  ((unsigned int)((n)->color))

/* Private Helper Functions --------------------------------------------------------------------- */

static inline void _rb_set_parent(struct rb_node *n, struct rb_node *p)
{
    n->parent = p;
}

static inline void _rb_set_color(struct rb_node *n, unsigned int color)
{
    n->color = (uint8_t)color;
}

static inline void _rb_set_red(struct rb_node *n)
{
    n->color = (uint8_t)RB_RED;
}

static inline void _rb_set_black(struct rb_node *n)
{
    n->color = (uint8_t)RB_BLACK;
}

static inline bool _rb_is_red(const struct rb_node *n)
{
    return n && (_RB_COLOR(n) == RB_RED);
}

static inline bool _rb_is_black(const struct rb_node *n)
{
    return !n || (_RB_COLOR(n) == RB_BLACK);
}

static inline struct rb_node *_rb_left(const struct rb_node *n)
{
    return (n == NULL) ? NULL : n->left;
}

static inline struct rb_node *_rb_right(const struct rb_node *n)
{
    return (n == NULL) ? NULL : n->right;
}

/* Left rotation: pivot `node` down-left so `node->right` moves up. */
static void _rb_rotate_left(struct rb_node *node, struct rb_tree *tree)
{
    struct rb_node *right  = node->right;
    struct rb_node *parent = _RB_PARENT(node);

    /* Attach right's left subtree as node's new right child. */
    node->right = right->left;
    if (right->left) {
        _rb_set_parent(right->left, node);
    }

    /* Place right where node used to be. */
    right->left = node;
    _rb_set_parent(right, parent);

    if (parent) {
        if (node == parent->left) {
            parent->left = right;
        } else {
            parent->right = right;
        }
    } else {
        tree->root = right;
    }

    _rb_set_parent(node, right);
}

/* Right rotation: pivot `node` down-right so `node->left` moves up. */
static void _rb_rotate_right(struct rb_node *node, struct rb_tree *tree)
{
    struct rb_node *left   = node->left;
    struct rb_node *parent = _RB_PARENT(node);

    /* Attach left's right subtree as node's new left child. */
    node->left = left->right;
    if (left->right) {
        _rb_set_parent(left->right, node);
    }

    /* Place left where node used to be. */
    left->right = node;
    _rb_set_parent(left, parent);

    if (parent) {
        if (node == parent->right) {
            parent->right = left;
        } else {
            parent->left = left;
        }
    } else {
        tree->root = left;
    }

    _rb_set_parent(node, left);
}

/**
 * @brief  Restore red-black invariants after inserting a red node.
 *
 * @param  tree  Tree being modified.
 * @param  node  Newly inserted red node.
 */
static void _rb_insert_fixup(struct rb_tree *tree, struct rb_node *node)
{
    struct rb_node *parent;
    struct rb_node *gparent;

    /* Loop while the parent is red, which violates the red-black rules. */
    while ((parent = _RB_PARENT(node)) && _rb_is_red(parent)) {
        gparent = _RB_PARENT(parent);

        if (parent == gparent->left) {
            struct rb_node *uncle = gparent->right;

            if (_rb_is_red(uncle)) {
                /* Case 1: uncle is red, so recolor and move the violation up. */
                _rb_set_black(parent);
                _rb_set_black(uncle);
                _rb_set_red(gparent);
                node = gparent;
                continue;
            }

            if (parent->right == node) {
                /* Case 2: node is a right child, so rotate into case 3. */
                struct rb_node *tmp;

                _rb_rotate_left(parent, tree);
                tmp    = parent;
                parent = node;
                node   = tmp;
            }

            /* Case 3: node is a left child, so rotate right and recolor. */
            _rb_set_black(parent);
            _rb_set_red(gparent);
            _rb_rotate_right(gparent, tree);

        } else {
            struct rb_node *uncle = gparent->left;

            if (_rb_is_red(uncle)) {
                /* Case 1, mirrored. */
                _rb_set_black(parent);
                _rb_set_black(uncle);
                _rb_set_red(gparent);
                node = gparent;
                continue;
            }

            if (parent->left == node) {
                /* Case 2, mirrored. */
                struct rb_node *tmp;

                _rb_rotate_right(parent, tree);
                tmp    = parent;
                parent = node;
                node   = tmp;
            }

            /* Case 3, mirrored. */
            _rb_set_black(parent);
            _rb_set_red(gparent);
            _rb_rotate_left(gparent, tree);
        }
    }

    /* The root must always be black. */
    _rb_set_black(tree->root);
}

/**
 * @brief  Restore red-black invariants after removing a black node.
 *
 * @param  tree    Tree being modified.
 * @param  node    Node that replaced the removed node. May be NULL.
 * @param  parent  Parent of @p node. Needed when @p node is NULL.
 */
static void _rb_remove_fixup(struct rb_tree *tree, struct rb_node *node, struct rb_node *parent)
{
    struct rb_node *sibling;

    if (parent == NULL) {
        if (node != NULL) {
            _rb_set_black(node);
        }
        return;
    }

    while (_rb_is_black(node) && node != tree->root) {
        if (parent->left == node) {
            sibling = parent->right;

            if (_rb_is_red(sibling)) {
                /* Case 1: sibling is red, so rotate to make it black. */
                _rb_set_black(sibling);
                _rb_set_red(parent);
                _rb_rotate_left(parent, tree);
                sibling = parent->right;
            }

            if (_rb_is_black(_rb_left(sibling)) && _rb_is_black(_rb_right(sibling))) {
                /* Case 2: sibling children are both black, so recolor sibling. */
                if (sibling != NULL) {
                    _rb_set_red(sibling);
                }
                node   = parent;
                parent = _RB_PARENT(node);
            } else {
                if (_rb_is_black(_rb_right(sibling))) {
                    /* Case 3: sibling right child is black, so rotate sibling. */
                    if (_rb_left(sibling) != NULL) {
                        _rb_set_black(_rb_left(sibling));
                    }
                    _rb_set_red(sibling);
                    _rb_rotate_right(sibling, tree);
                    sibling = parent->right;
                }

                /* Case 4: sibling right child is red, so rotate parent. */
                _rb_set_color(sibling, _RB_COLOR(parent));
                _rb_set_black(parent);
                if (_rb_right(sibling) != NULL) {
                    _rb_set_black(_rb_right(sibling));
                }
                _rb_rotate_left(parent, tree);
                node = tree->root;
                break;
            }

        } else {
            sibling = parent->left;

            if (_rb_is_red(sibling)) {
                /* Case 1, mirrored. */
                _rb_set_black(sibling);
                _rb_set_red(parent);
                _rb_rotate_right(parent, tree);
                sibling = parent->left;
            }

            if (_rb_is_black(_rb_right(sibling)) && _rb_is_black(_rb_left(sibling))) {
                /* Case 2, mirrored. */
                if (sibling != NULL) {
                    _rb_set_red(sibling);
                }
                node   = parent;
                parent = _RB_PARENT(node);
            } else {
                if (_rb_is_black(_rb_left(sibling))) {
                    /* Case 3, mirrored. */
                    if (_rb_right(sibling) != NULL) {
                        _rb_set_black(_rb_right(sibling));
                    }
                    _rb_set_red(sibling);
                    _rb_rotate_left(sibling, tree);
                    sibling = parent->left;
                }

                /* Case 4, mirrored. */
                _rb_set_color(sibling, _RB_COLOR(parent));
                _rb_set_black(parent);
                if (_rb_left(sibling) != NULL) {
                    _rb_set_black(_rb_left(sibling));
                }
                _rb_rotate_right(parent, tree);
                node = tree->root;
                break;
            }
        }
    }

    if (node) {
        _rb_set_black(node);
    }
}

/* Public Functions ----------------------------------------------------------------------------- */

void rb_tree_insert(struct rb_tree *tree,
                    struct rb_node *node,
                    int (*cmp)(const struct rb_node *, const struct rb_node *))
{
    struct rb_node **link   = &tree->root;
    struct rb_node  *parent = NULL;

    /* Standard BST descent to find the insertion point. */
    while (*link) {
        int order;

        parent = *link;
        order  = cmp(node, parent);

        if (order < 0) {
            link = &parent->left;
        } else if (order > 0) {
            link = &parent->right;
        } else {
            return; /* Duplicate key, keep the existing node. */
        }
    }

    /*
     * Initialize the new node with explicit parent/color state so no target
     * alignment assumptions leak into the tree invariants.
     */
    node->parent = parent;
    node->left   = NULL;
    node->right  = NULL;
    node->color  = (uint8_t)RB_RED;

    *link = node;

    _rb_insert_fixup(tree, node);
}

void rb_tree_remove(struct rb_tree *tree, struct rb_node *node)
{
    struct rb_node *child;
    struct rb_node *parent;
    unsigned int    color;

    if (!node->left) {
        /* Case A: no left child, so replace with the right child. */
        child = node->right;
    } else if (!node->right) {
        /* Case B: no right child, so replace with the left child. */
        child = node->left;
    } else {
        /*
         * Case C: both children exist.
         * Find the in-order successor, splice it out, and move it into the
         * removed node's position.
         */
        struct rb_node *successor = node->right;
        struct rb_node *old       = node;
        struct rb_node *old_parent;

        while (successor->left) {
            successor = successor->left;
        }

        child  = successor->right;
        parent = _RB_PARENT(successor);
        color  = _RB_COLOR(successor);

        if (child) {
            _rb_set_parent(child, parent);
        }

        if (parent == old) {
            parent->right = child;
            parent        = successor;
        } else {
            parent->left = child;
        }

        successor->parent = old->parent;
        successor->color  = old->color;
        successor->right  = old->right;
        successor->left   = old->left;

        old_parent = _RB_PARENT(old);
        if (old_parent) {
            if (old_parent->left == old) {
                old_parent->left = successor;
            } else {
                old_parent->right = successor;
            }
        } else {
            tree->root = successor;
        }

        _rb_set_parent(successor->left, successor);
        if (successor->right) {
            _rb_set_parent(successor->right, successor);
        }

        if (color == RB_BLACK) {
            _rb_remove_fixup(tree, child, parent);
        }
        return;
    }

    parent = _RB_PARENT(node);
    color  = _RB_COLOR(node);

    if (child) {
        _rb_set_parent(child, parent);
    }

    if (parent) {
        if (parent->left == node) {
            parent->left = child;
        } else {
            parent->right = child;
        }
    } else {
        tree->root = child;
    }

    if (color == RB_BLACK) {
        _rb_remove_fixup(tree, child, parent);
    }
}

struct rb_node *rb_tree_lower_bound(const struct rb_tree *tree,
                                    const void           *key,
                                    int (*cmp)(const struct rb_node *, const void *))
{
    struct rb_node *candidate = NULL;
    struct rb_node *x         = tree->root;

    while (x) {
        int c = cmp(x, key);

        if (c == 0) {
            return x;
        } else if (c > 0) {
            candidate = x;
            x         = x->left;
        } else {
            x = x->right;
        }
    }

    return candidate;
}

struct rb_node *rb_node_next(const struct rb_node *node)
{
    const struct rb_node *n = node;
    struct rb_node       *parent;

    if (n->right) {
        n = n->right;
        while (n->left) {
            n = n->left;
        }
        return (struct rb_node *)n;
    }

    while ((parent = _RB_PARENT(n)) && n == parent->right) {
        n = parent;
    }

    return parent;
}

struct rb_node *rb_node_prev(const struct rb_node *node)
{
    const struct rb_node *n = node;
    struct rb_node       *parent;

    if (n->left) {
        n = n->left;
        while (n->right) {
            n = n->right;
        }
        return (struct rb_node *)n;
    }

    while ((parent = _RB_PARENT(n)) && n == parent->left) {
        n = parent;
    }

    return parent;
}

/* ---------------------------------------- end of file ----------------------------------------- */
