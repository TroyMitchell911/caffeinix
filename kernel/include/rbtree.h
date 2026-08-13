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

static inline void rb_root_init(struct rb_root *root)
{
	root->node = 0;
}

static inline void rb_node_init(struct rb_node *node)
{
	node->parent = 0;
	node->left = 0;
	node->right = 0;
	node->red = 1;
}

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
