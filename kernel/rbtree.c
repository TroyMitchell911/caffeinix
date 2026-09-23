/*
 * Intrusive red-black tree balancing and ordered traversal. Callers choose
 * keys, perform the search/insertion link, and serialize all mutations. The
 * tree never allocates or owns its containing objects.
 */
#include <rbtree.h>

/*
 * NULL leaves are black; only a present node may carry the red bit.
 */
static int rb_is_red(const struct rb_node *node)
{
	return node && node->red;
}

/*
 * Treat absent leaves as black when checking deletion invariants.
 */
static int rb_is_black(const struct rb_node *node)
{
	return !node || !node->red;
}

/**
 * rb_rotate_left() - Rotate a subtree through its right child
 * @node: Subtree root with a non-NULL right child.
 * @root: Containing tree whose root may change.
 *
 * Preserves in-order key order and repairs parent links; colors are
 * unchanged.
 *
 * Context: Caller serializes tree mutation; does not sleep.
 */
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

/**
 * rb_rotate_right() - Rotate a subtree through its left child
 * @node: Subtree root with a non-NULL left child.
 * @root: Containing tree whose root may change.
 *
 * Preserves in-order key order and repairs parent links; colors are
 * unchanged.
 *
 * Context: Caller serializes tree mutation; does not sleep.
 */
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

/**
 * rb_insert_color() - Restore balance after linking a red leaf
 * @node: New node installed with rb_link_node().
 * @root: Containing tree.
 *
 * The search and duplicate-key policy belong to the caller. Recolors and
 * rotates without allocating; the resulting root is black.
 *
 * Context: Caller serializes insertion and balancing as one mutation.
 */
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

/**
 * rb_replace_node() - Replace one structural link
 * @root: Containing tree.
 * @old: Linked node whose parent/root link is replaced.
 * @new: Replacement node, or NULL to remove the link.
 *
 * Only repairs the parent/root link and replacement parent; child links and
 * colors remain the caller's responsibility.
 *
 * Context: Caller holds the tree mutation lock.
 */
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

/*
 * Follow left children to the minimum node in a nonempty subtree; the caller
 * excludes concurrent mutation.
 */
static struct rb_node *rb_subtree_first(struct rb_node *node)
{
	while (node->left)
		node = node->left;
	return node;
}

/**
 * rb_erase_fixup() - Repair the missing black height after deletion
 * @root: Tree after removal or successor substitution.
 * @node: Replacement child, possibly NULL.
 * @parent: Parent of @node, supplied separately for NULL leaves.
 *
 * Moves the black-height deficit toward the root or resolves it with sibling
 * recoloring and rotations; allocates no storage.
 *
 * Context: Caller serializes the entire erase operation.
 */
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

/**
 * rb_erase() - Unlink and rebalance an intrusive node
 * @node: Node currently linked into @root.
 * @root: Containing tree.
 *
 * Resets the removed node for reuse but does not free its containing object.
 * Existing pointers to other nodes remain valid.
 *
 * Context: Caller excludes concurrent traversal and mutation.
 */
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

/**
 * rb_first() - Find the minimum linked node
 * @root: Initialized tree, possibly empty.
 *
 * Context: Caller prevents concurrent tree mutation.
 * Return: Borrowed first node, or NULL for an empty tree.
 */
struct rb_node *rb_first(const struct rb_root *root)
{
	struct rb_node *node = root->node;

	return node ? rb_subtree_first(node) : 0;
}

/**
 * rb_next() - Find the in-order successor
 * @node: Node currently linked in a stable tree.
 *
 * Context: Caller prevents concurrent tree mutation.
 * Return: Borrowed successor, or NULL after the maximum node.
 */
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
