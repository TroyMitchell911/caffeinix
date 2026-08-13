#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include <rbtree.h>

#define NODE_COUNT 128
#define OPERATION_COUNT 20000

struct test_node {
	struct rb_node node;
	int key;
	int linked;
};

static struct rb_root tree;
static struct test_node nodes[NODE_COUNT];
static unsigned int random_state = 1;

static unsigned int next_random(void)
{
	random_state = random_state * 1103515245U + 12345U;
	return random_state;
}

static void insert_node(struct test_node *entry)
{
	struct rb_node **link = &tree.node;
	struct rb_node *parent = NULL;

	while (*link) {
		struct test_node *current;

		parent = *link;
		current = rb_entry(parent, struct test_node, node);
		if (entry->key < current->key)
			link = &parent->left;
		else
			link = &parent->right;
	}
	rb_link_node(&entry->node, parent, link);
	rb_insert_color(&entry->node, &tree);
	entry->linked = 1;
}

static int validate_subtree(struct rb_node *node, struct rb_node *parent,
			    int minimum, int maximum)
{
	struct test_node *entry;
	int left_height, right_height;

	if (!node)
		return 1;
	entry = rb_entry(node, struct test_node, node);
	if (node->parent != parent || entry->key <= minimum ||
	    entry->key >= maximum)
		return -1;
	if (node->red && ((node->left && node->left->red) ||
			 (node->right && node->right->red)))
		return -1;
	left_height = validate_subtree(node->left, node, minimum, entry->key);
	right_height = validate_subtree(node->right, node, entry->key, maximum);
	if (left_height < 0 || right_height < 0 ||
	    left_height != right_height)
		return -1;
	return left_height + !node->red;
}

static int validate_tree(void)
{
	struct rb_node *node;
	int previous = -1;
	int count = 0;

	if (tree.node && tree.node->red)
		return -1;
	if (validate_subtree(tree.node, NULL, -1, NODE_COUNT) < 0)
		return -1;
	for (node = rb_first(&tree); node; node = rb_next(node)) {
		struct test_node *entry;

		entry = rb_entry(node, struct test_node, node);
		if (entry->key <= previous)
			return -1;
		previous = entry->key;
		count++;
	}
	for (int index = 0; index < NODE_COUNT; index++)
		count -= nodes[index].linked;
	return count ? -1 : 0;
}

int main(void)
{
	int operation;

	rb_root_init(&tree);
	for (int index = 0; index < NODE_COUNT; index++) {
		nodes[index].key = index;
		rb_node_init(&nodes[index].node);
	}
	for (operation = 0; operation < OPERATION_COUNT; operation++) {
		struct test_node *entry = &nodes[next_random() % NODE_COUNT];

		if (entry->linked) {
			rb_erase(&entry->node, &tree);
			entry->linked = 0;
		} else {
			insert_node(entry);
		}
		if (validate_tree()) {
			fprintf(stderr, "rbtree validation failed at operation %d\n",
				operation);
			return EXIT_FAILURE;
		}
	}
	for (int index = 0; index < NODE_COUNT; index++) {
		if (!nodes[index].linked)
			continue;
		rb_erase(&nodes[index].node, &tree);
		nodes[index].linked = 0;
	}
	if (tree.node || validate_tree())
		return EXIT_FAILURE;
	puts("RBTREE_OK");
	return EXIT_SUCCESS;
}
