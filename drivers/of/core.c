#include <libfdt.h>
#include <mystring.h>
#include <of.h>
#include <resource.h>

#define OF_DTB_MAX_SIZE (64 * 1024)

static uint64 of_blob_storage[OF_DTB_MAX_SIZE / sizeof(uint64)];
static struct device_node of_nodes[OF_MAX_NODES];
static const void *of_blob;
static int of_node_count;

static uint64 of_read_number(const fdt32_t *cells, int count)
{
	uint64 value = 0;

	while (count--)
		value = value << 32 | fdt32_to_cpu(*cells++);
	return value;
}

static int of_cells(const struct device_node *node, const char *name,
		    int default_value)
{
	const fdt32_t *property;
	uint32 value;
	int length;

	property = of_get_property(node, name, &length);
	if (!property)
		return default_value;
	if (length != sizeof(*property))
		return -1;
	value = fdt32_to_cpu(*property);
	return value <= 2 ? value : -1;
}

static int of_reg_info(const struct device_node *node, const fdt32_t **reg,
		       int *address_cells, int *size_cells)
{
	int entry_cells, length;

	if (!node || !node->parent || !reg || !address_cells || !size_cells)
		return -1;
	*address_cells = of_cells(node->parent, "#address-cells", 2);
	*size_cells = of_cells(node->parent, "#size-cells", 1);
	if (*address_cells < 1 || *address_cells > 2 ||
	    *size_cells < 0 || *size_cells > 2)
		return -1;
	entry_cells = *address_cells + *size_cells;
	*reg = of_get_property(node, "reg", &length);
	if (!*reg || !entry_cells || length % (entry_cells * sizeof(**reg)))
		return -1;
	return length / (entry_cells * sizeof(**reg));
}

static int of_node_is_type(const struct device_node *node, const char *type)
{
	const char *property;
	int length, type_length;

	property = of_get_property(node, "device_type", &length);
	type_length = strlen(type);
	return property && length == type_length + 1 &&
	       !memcmp(property, type, length);
}

int of_init(const void *fdt)
{
	struct device_node *parents[OF_MAX_NODES];
	int depth = 0, offset = -1, total, status;

	if (!fdt)
		return -1;
	status = fdt_check_header(fdt);
	if (status < 0)
		return status;
	total = fdt_totalsize(fdt);
	if (total <= 0 || total > OF_DTB_MAX_SIZE)
		return -1;
	memmove(of_blob_storage, fdt, total);
	status = fdt_check_full(of_blob_storage, total);
	if (status < 0)
		return status;
	of_blob = of_blob_storage;
	of_node_count = 0;
	memset(parents, 0, sizeof(parents));
	while ((offset = fdt_next_node(of_blob, offset, &depth)) >= 0) {
		struct device_node *node;

		if (of_node_count >= OF_MAX_NODES || depth < 0 ||
		    depth >= OF_MAX_NODES)
			return -1;
		node = &of_nodes[of_node_count++];
		node->offset = offset;
		node->depth = depth;
		node->name = fdt_get_name(of_blob, offset, 0);
		node->parent = depth ? parents[depth - 1] : 0;
		parents[depth] = node;
	}
	return of_node_count ? 0 : -1;
}

const void *of_fdt(void)
{
	return of_blob;
}

struct device_node *of_root_node(void)
{
	return of_node_count ? &of_nodes[0] : 0;
}

struct device_node *of_next_node(struct device_node *node)
{
	if (!of_node_count)
		return 0;
	if (!node)
		return &of_nodes[0];
	if (node < of_nodes || node >= &of_nodes[of_node_count - 1])
		return 0;
	return node + 1;
}

struct device_node *of_find_node_by_path(const char *path)
{
	int offset, i;

	if (!of_blob || !path)
		return 0;
	offset = fdt_path_offset(of_blob, path);
	if (offset < 0)
		return 0;
	for (i = 0; i < of_node_count; i++) {
		if (of_nodes[i].offset == offset)
			return &of_nodes[i];
	}
	return 0;
}

const void *of_get_property(const struct device_node *node,
			    const char *name, int *length)
{
	if (!of_blob || !node || !name)
		return 0;
	return fdt_getprop(of_blob, node->offset, name, length);
}

int of_property_read_u32(const struct device_node *node,
			 const char *name, uint32 *value)
{
	const fdt32_t *property;
	int length;

	if (!value)
		return -1;
	property = of_get_property(node, name, &length);
	if (!property || length < sizeof(*property))
		return -1;
	*value = fdt32_to_cpu(*property);
	return 0;
}

int of_device_is_available(const struct device_node *node)
{
	const char *status;
	int length;

	status = of_get_property(node, "status", &length);
	if (!status)
		return 1;
	return (length == 3 && !memcmp(status, "ok", 3)) ||
	       (length == 5 && !memcmp(status, "okay", 5));
}

int of_device_is_compatible(const struct device_node *node,
			    const char *compatible)
{
	if (!of_blob || !node || !compatible)
		return 0;
	return fdt_node_check_compatible(of_blob, node->offset,
	                                 compatible) == 0;
}

int of_node_path(const struct device_node *node, char *path, uint32 size)
{
	if (!of_blob || !node || !path || !size)
		return -1;
	return fdt_get_path(of_blob, node->offset, path, size);
}

struct device_node *of_stdout_node(void)
{
	struct device_node *chosen;
	const char *property;
	char path[OF_PATH_MAX];
	int length, i;

	chosen = of_find_node_by_path("/chosen");
	property = of_get_property(chosen, "stdout-path", &length);
	if (!property || length <= 1 || length > sizeof(path))
		return 0;
	for (i = 0; i < length && property[i] && property[i] != ':'; i++)
		path[i] = property[i];
	if (i == 0 || i >= sizeof(path))
		return 0;
	path[i] = 0;
	if (path[0] != '/') {
		property = fdt_get_alias(of_blob, path);
		if (!property || strlen(property) >= sizeof(path))
			return 0;
		safe_strncpy(path, property, sizeof(path));
	}
	return of_find_node_by_path(path);
}

int of_address_to_resource(const struct device_node *node, int index,
			   struct resource *resource)
{
	const fdt32_t *reg;
	uint64 length;
	int address_cells, count, entry_cells, size_cells;

	if (!resource || index < 0)
		return -1;
	count = of_reg_info(node, &reg, &address_cells, &size_cells);
	if (count < 0 || index >= count)
		return -1;
	entry_cells = address_cells + size_cells;
	reg += index * entry_cells;
	resource->name = node->name;
	resource->start = of_read_number(reg, address_cells);
	length = of_read_number(reg + address_cells, size_cells);
	if (!length || resource->start + length - 1 < resource->start)
		return -1;
	resource->end = resource->start + length - 1;
	resource->flags = RESOURCE_MEM;
	return 0;
}

int of_irq_get(const struct device_node *node, int index)
{
	const fdt32_t *interrupts;
	int length;

	if (!node || index < 0)
		return -1;
	interrupts = of_get_property(node, "interrupts", &length);
	if (!interrupts || length < (index + 1) * sizeof(*interrupts))
		return -1;
	return fdt32_to_cpu(interrupts[index]);
}

int of_alias_get_id(const struct device_node *node, const char *stem)
{
	const char *alias;
	char name[32], path[OF_PATH_MAX];
	int id;

	if (!node || !stem || of_node_path(node, path, sizeof(path)) < 0)
		return -1;
	for (id = 0; id < 32; id++) {
		int length = strlen(stem);

		if (length + 3 >= sizeof(name))
			return -1;
		memcpy(name, stem, length);
		if (id >= 10)
			name[length++] = '0' + id / 10;
		name[length++] = '0' + id % 10;
		name[length] = 0;
		alias = fdt_get_alias(of_blob, name);
		if (alias && !strcmp(alias, path))
			return id;
	}
	return -1;
}

int of_memory_range_count(void)
{
	struct device_node *node = 0;
	int count = 0;

	while ((node = of_next_node(node))) {
		const fdt32_t *reg;
		int address_cells, entries, size_cells;

		if (!of_device_is_available(node) ||
		    !of_node_is_type(node, "memory"))
			continue;
		entries = of_reg_info(node, &reg, &address_cells, &size_cells);
		if (entries < 0)
			return -1;
		count += entries;
	}
	return count;
}

int of_memory_range_get(int index, struct of_memory_range *range)
{
	struct device_node *node = 0;

	if (index < 0 || !range)
		return -1;
	while ((node = of_next_node(node))) {
		const fdt32_t *reg;
		int address_cells, entries, entry_cells, size_cells;

		if (!of_device_is_available(node) ||
		    !of_node_is_type(node, "memory"))
			continue;
		entries = of_reg_info(node, &reg, &address_cells, &size_cells);
		if (entries < 0)
			return -1;
		if (index >= entries) {
			index -= entries;
			continue;
		}
		entry_cells = address_cells + size_cells;
		reg += index * entry_cells;
		range->start = of_read_number(reg, address_cells);
		range->size = of_read_number(reg + address_cells, size_cells);
		if (!range->size || range->start + range->size < range->start)
			return -1;
		return 0;
	}
	return -1;
}

static int of_reserved_tree_range_count(void)
{
	struct device_node *node = 0;
	struct device_node *reserved;
	int count = 0;

	reserved = of_find_node_by_path("/reserved-memory");
	if (!reserved)
		return 0;
	while ((node = of_next_node(node))) {
		const fdt32_t *reg;
		int address_cells, entries, size_cells;

		if (node->parent != reserved || !of_device_is_available(node))
			continue;
		entries = of_reg_info(node, &reg, &address_cells, &size_cells);
		if (entries < 0)
			return -1;
		count += entries;
	}
	return count;
}

int of_reserved_memory_range_count(void)
{
	int count, tree_count;

	if (!of_blob)
		return -1;
	count = fdt_num_mem_rsv(of_blob);
	if (count < 0)
		return count;
	tree_count = of_reserved_tree_range_count();
	if (tree_count < 0)
		return tree_count;
	return count + tree_count;
}

int of_reserved_memory_range_get(int index, struct of_memory_range *range)
{
	struct device_node *node = 0;
	struct device_node *reserved;
	int map_count;

	if (!of_blob || index < 0 || !range)
		return -1;
	map_count = fdt_num_mem_rsv(of_blob);
	if (map_count < 0)
		return map_count;
	if (index < map_count)
		return fdt_get_mem_rsv(of_blob, index, &range->start,
		                       &range->size);
	index -= map_count;
	reserved = of_find_node_by_path("/reserved-memory");
	if (!reserved)
		return -1;
	while ((node = of_next_node(node))) {
		const fdt32_t *reg;
		int address_cells, entries, entry_cells, size_cells;

		if (node->parent != reserved || !of_device_is_available(node))
			continue;
		entries = of_reg_info(node, &reg, &address_cells, &size_cells);
		if (entries < 0)
			return -1;
		if (index >= entries) {
			index -= entries;
			continue;
		}
		entry_cells = address_cells + size_cells;
		reg += index * entry_cells;
		range->start = of_read_number(reg, address_cells);
		range->size = of_read_number(reg + address_cells, size_cells);
		if (!range->size || range->start + range->size < range->start)
			return -1;
		return 0;
	}
	return -1;
}
