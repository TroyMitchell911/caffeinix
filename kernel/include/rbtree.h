/*
 * Intrusive ordered-tree nodes and linking primitives. Search and comparison
 * policy belong to the caller; insertion must be followed by
 * rb_insert_color() before releasing the tree lock.
 */
#ifndef __CAFFEINIX_KERNEL_RBTREE_H
#define __CAFFEINIX_KERNEL_RBTREE_H

#include <typedefs.h>

struct rb_node {
	struct rb_node *parent;
	struct rb_node *left;
	struct rb_node *right;
	uint8 red;
};

struct rb_root {
	struct rb_node *node;
};

#define rb_entry(pointer, type, member) \
	((type *)((char *)(pointer) - \
	 (unsigned long)(&((type *)0)->member)))

/**
 * rb_root_init() - Initialize an empty tree
 * @root: Caller-owned tree header.
 *
 * Does not release nodes from a previously populated tree.
 *
 * Context: Before publication or under caller serialization.
 */
static inline void rb_root_init(struct rb_root *root)
{
	root->node = 0;
}

/**
 * rb_node_init() - Reset an unlinked node
 * @node: Caller-owned node, not currently in a tree.
 *
 * Context: Before publication or after removal.
 */
static inline void rb_node_init(struct rb_node *node)
{
	node->parent = 0;
	node->left = 0;
	node->right = 0;
	node->red = 1;
}

/**
 * rb_link_node() - Install a new leaf at the caller-selected position
 * @node: Unlinked caller-owned node.
 * @parent: Parent node, or NULL for the root.
 * @link: Empty child link or address of the tree root pointer.
 *
 * Context: Caller serializes search, link, and subsequent rb_insert_color().
 */
static inline void rb_link_node(struct rb_node *node,
				struct rb_node *parent,
				struct rb_node **link)
{
	node->parent = parent;
	node->left = 0;
	node->right = 0;
	node->red = 1;
	*link = node;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);
struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);

#endif
