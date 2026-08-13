#include <rbtree.h>

static int rb_is_red(const struct rb_node *node)
{
	return node && node->red;
}

static int rb_is_black(const struct rb_node *node)
{
	return !node || !node->red;
}

static void rb_rotate_left(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *right = node->right;

	node->right = right->left;
	if (right->left)
		right->left->parent = node;
	right->parent = node->parent;
	if (!node->parent)
		root->node = right;
	else if (node == node->parent->left)
		node->parent->left = right;
	else
		node->parent->right = right;
	right->left = node;
	node->parent = right;
}

static void rb_rotate_right(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *left = node->left;

	node->left = left->right;
	if (left->right)
		left->right->parent = node;
	left->parent = node->parent;
	if (!node->parent)
		root->node = left;
	else if (node == node->parent->right)
		node->parent->right = left;
	else
		node->parent->left = left;
	left->right = node;
	node->parent = left;
}

void rb_insert_color(struct rb_node *node, struct rb_root *root)
{
	while (node->parent && node->parent->red) {
		struct rb_node *parent = node->parent;
		struct rb_node *grandparent = parent->parent;

		if (parent == grandparent->left) {
			struct rb_node *uncle = grandparent->right;

			if (rb_is_red(uncle)) {
				parent->red = 0;
				uncle->red = 0;
				grandparent->red = 1;
				node = grandparent;
				continue;
			}
			if (node == parent->right) {
				node = parent;
				rb_rotate_left(node, root);
				parent = node->parent;
				grandparent = parent->parent;
			}
			parent->red = 0;
			grandparent->red = 1;
			rb_rotate_right(grandparent, root);
		} else {
			struct rb_node *uncle = grandparent->left;

			if (rb_is_red(uncle)) {
				parent->red = 0;
				uncle->red = 0;
				grandparent->red = 1;
				node = grandparent;
				continue;
			}
			if (node == parent->left) {
				node = parent;
				rb_rotate_right(node, root);
				parent = node->parent;
				grandparent = parent->parent;
			}
			parent->red = 0;
			grandparent->red = 1;
			rb_rotate_left(grandparent, root);
		}
	}
	root->node->red = 0;
}

static void rb_replace_node(struct rb_root *root, struct rb_node *old,
			    struct rb_node *new)
{
	if (!old->parent)
		root->node = new;
	else if (old == old->parent->left)
		old->parent->left = new;
	else
		old->parent->right = new;
	if (new)
		new->parent = old->parent;
}

static struct rb_node *rb_subtree_first(struct rb_node *node)
{
	while (node->left)
		node = node->left;
	return node;
}

static void rb_erase_fixup(struct rb_root *root, struct rb_node *node,
			   struct rb_node *parent)
{
	while (node != root->node && rb_is_black(node)) {
		struct rb_node *sibling;

		if (!parent)
			break;
		if (node == parent->left) {
			sibling = parent->right;
			if (rb_is_red(sibling)) {
				sibling->red = 0;
				parent->red = 1;
				rb_rotate_left(parent, root);
				sibling = parent->right;
			}
			if (!sibling) {
				node = parent;
				parent = node->parent;
				continue;
			}
			if (rb_is_black(sibling->left) &&
			    rb_is_black(sibling->right)) {
				sibling->red = 1;
				node = parent;
				parent = node->parent;
				continue;
			}
			if (rb_is_black(sibling->right)) {
				if (sibling->left)
					sibling->left->red = 0;
				sibling->red = 1;
				rb_rotate_right(sibling, root);
				sibling = parent->right;
			}
			sibling->red = parent->red;
			parent->red = 0;
			if (sibling->right)
				sibling->right->red = 0;
			rb_rotate_left(parent, root);
			node = root->node;
			parent = 0;
		} else {
			sibling = parent->left;
			if (rb_is_red(sibling)) {
				sibling->red = 0;
				parent->red = 1;
				rb_rotate_right(parent, root);
				sibling = parent->left;
			}
			if (!sibling) {
				node = parent;
				parent = node->parent;
				continue;
			}
			if (rb_is_black(sibling->left) &&
			    rb_is_black(sibling->right)) {
				sibling->red = 1;
				node = parent;
				parent = node->parent;
				continue;
			}
			if (rb_is_black(sibling->left)) {
				if (sibling->right)
					sibling->right->red = 0;
				sibling->red = 1;
				rb_rotate_left(sibling, root);
				sibling = parent->left;
			}
			sibling->red = parent->red;
			parent->red = 0;
			if (sibling->left)
				sibling->left->red = 0;
			rb_rotate_right(parent, root);
			node = root->node;
			parent = 0;
		}
	}
	if (node)
		node->red = 0;
}

void rb_erase(struct rb_node *node, struct rb_root *root)
{
	struct rb_node *child;
	struct rb_node *child_parent;
	struct rb_node *replacement = node;
	uint8 replacement_red = replacement->red;

	if (!node->left) {
		child = node->right;
		child_parent = node->parent;
		rb_replace_node(root, node, node->right);
	} else if (!node->right) {
		child = node->left;
		child_parent = node->parent;
		rb_replace_node(root, node, node->left);
	} else {
		replacement = rb_subtree_first(node->right);
		replacement_red = replacement->red;
		child = replacement->right;
		if (replacement->parent == node) {
			child_parent = replacement;
			if (child)
				child->parent = replacement;
		} else {
			child_parent = replacement->parent;
			rb_replace_node(root, replacement, replacement->right);
			replacement->right = node->right;
			replacement->right->parent = replacement;
		}
		rb_replace_node(root, node, replacement);
		replacement->left = node->left;
		replacement->left->parent = replacement;
		replacement->red = node->red;
	}
	if (!replacement_red)
		rb_erase_fixup(root, child, child_parent);
	rb_node_init(node);
}

struct rb_node *rb_first(const struct rb_root *root)
{
	struct rb_node *node = root->node;

	return node ? rb_subtree_first(node) : 0;
}

struct rb_node *rb_next(const struct rb_node *node)
{
	struct rb_node *parent;

	if (node->right)
		return rb_subtree_first(node->right);
	parent = node->parent;
	while (parent && node == parent->right) {
		node = parent;
		parent = parent->parent;
	}
	return parent;
}
