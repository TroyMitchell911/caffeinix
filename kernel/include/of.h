#ifndef __CAFFEINIX_KERNEL_OF_H
#define __CAFFEINIX_KERNEL_OF_H

#include <typedefs.h>

#define OF_MAX_NODES 64
#define OF_PATH_MAX 128

struct of_memory_range {
	uint64 start;
	uint64 size;
};

struct resource;

struct device_node {
	const char *name;
	struct device_node *parent;
	int offset;
	int depth;
};

int of_init(const void *fdt);
const void *of_fdt(void);
struct device_node *of_root_node(void);
struct device_node *of_next_node(struct device_node *node);
struct device_node *of_find_node_by_path(const char *path);
struct device_node *of_stdout_node(void);
const void *of_get_property(const struct device_node *node,
			    const char *name, int *length);
int of_property_read_u32(const struct device_node *node,
			 const char *name, uint32 *value);
int of_device_is_available(const struct device_node *node);
int of_device_is_compatible(const struct device_node *node,
			    const char *compatible);
int of_address_to_resource(const struct device_node *node, int index,
			   struct resource *resource);
int of_irq_get(const struct device_node *node, int index);
int of_alias_get_id(const struct device_node *node, const char *stem);
int of_node_path(const struct device_node *node, char *path, uint32 size);
int of_memory_range_count(void);
int of_memory_range_get(int index, struct of_memory_range *range);
int of_reserved_memory_range_count(void);
int of_reserved_memory_range_get(int index,
				 struct of_memory_range *range);

#endif
