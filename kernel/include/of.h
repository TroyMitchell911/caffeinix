/* Read-only flattened Device Tree lookup interface backed by boot FDT data. */
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

/**
 * of_init() - Validate and index the immutable boot FDT.
 * @fdt: Non-NULL flattened Device Tree blob supplied by firmware.
 *
 * The caller retains the blob for the kernel lifetime.  Indexed nodes and all
 * returned property pointers borrow that immutable storage; no overlays exist.
 *
 * Context:
 * Early boot process context; allocates no memory and does not sleep.
 *
 * Return:
 * Zero on success, negative for a missing, oversized, or malformed blob.
 */
int of_init(const void *fdt);
/**
 * of_fdt() - Return the validated boot FDT.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Borrowed immutable blob pointer, or %NULL before initialization.
 */
const void *of_fdt(void);
/**
 * of_root_node() - Return the indexed FDT root node.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Borrowed node pointer, or %NULL before initialization.
 */
struct device_node *of_root_node(void);
/**
 * of_machine_model() - Return the optional root model property.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Borrowed NUL-terminated model string, or %NULL when missing or malformed.
 */
const char *of_machine_model(void);
/**
 * of_next_node() - Iterate indexed nodes in FDT preorder.
 * @node: Previous borrowed node, or %NULL for the first node.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Next borrowed node, or %NULL at end or for an invalid @node.
 */
struct device_node *of_next_node(struct device_node *node);
/**
 * of_find_node_by_path() - Find an indexed absolute FDT path.
 * @path: Non-NULL NUL-terminated absolute path.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Borrowed node pointer, or %NULL when @path is absent or invalid.
 */
struct device_node *of_find_node_by_path(const char *path);
/**
 * of_stdout_node() - Resolve /chosen stdout-path including aliases.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Borrowed selected node, or %NULL for absent or malformed configuration.
 */
struct device_node *of_stdout_node(void);
/**
 * of_get_property() - Return raw immutable property bytes.
 * @node: Borrowed indexed node.
 * @name: Non-NULL property name.
 * @length: Optional destination for signed byte length.
 *
 * The bytes retain FDT byte order; multi-cell users must decode big-endian
 * cells rather than treating the result as native integers.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Borrowed property bytes, or %NULL when absent or invalid.
 */
const void *of_get_property(const struct device_node *node,
			    const char *name, int *length);
/**
 * of_property_count_u32() - Count complete 32-bit big-endian property cells.
 * @node: Borrowed indexed node.
 * @name: Non-NULL property name.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Non-negative cell count, or negative for an absent or misaligned property.
 */
int of_property_count_u32(const struct device_node *node, const char *name);
/**
 * of_property_read_u32() - Decode the first 32-bit big-endian cell.
 * @node: Borrowed indexed node.
 * @name: Non-NULL property name.
 * @value: Non-NULL destination for decoded native-endian value.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Zero on success, negative for invalid input, absent property, or short data.
 */
int of_property_read_u32(const struct device_node *node,
			 const char *name, uint32 *value);
/**
 * of_property_read_u32_index() - Decode one indexed big-endian property cell.
 * @node: Borrowed indexed node.
 * @name: Non-NULL property name.
 * @index: Zero-based non-negative cell index.
 * @value: Non-NULL destination for decoded native-endian value.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Zero on success, negative for invalid input, absent property, or short data.
 */
int of_property_read_u32_index(const struct device_node *node,
			       const char *name, int index, uint32 *value);
/**
 * of_node_phandle() - Return a node's numeric phandle.
 * @node: Borrowed indexed node.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Native-endian phandle, or zero when absent or invalid.
 */
uint32 of_node_phandle(const struct device_node *node);
/**
 * of_find_node_by_phandle() - Resolve a non-zero phandle to an indexed node.
 * @phandle: Native-endian non-zero phandle.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Borrowed node pointer, or %NULL when unresolved.
 */
struct device_node *of_find_node_by_phandle(uint32 phandle);
/**
 * of_device_is_available() - Interpret a node's optional status property.
 * @node: Borrowed indexed node.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Non-zero for absent, "ok", or "okay" status; zero otherwise.
 */
int of_device_is_available(const struct device_node *node);
/**
 * of_device_is_compatible() - Test one compatible string in a node list.
 * @node: Borrowed indexed node.
 * @compatible: Non-NULL NUL-terminated compatible string.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Non-zero on exact match, zero otherwise.
 */
int of_device_is_compatible(const struct device_node *node,
			    const char *compatible);
/**
 * of_address_to_resource() - Decode one reg tuple into an inclusive resource.
 * @node: Borrowed indexed child node.
 * @index: Zero-based reg tuple index.
 * @resource: Non-NULL destination.
 *
 * Parent #address-cells and #size-cells define tuple layout and are decoded
 * from big-endian cells.  Zero lengths and overflowing inclusive ends fail.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Zero on success, negative for malformed, missing, or out-of-range tuples.
 */
int of_address_to_resource(const struct device_node *node, int index,
			   struct resource *resource);
/**
 * of_irq_get() - Decode one legacy interrupt cell.
 * @node: Borrowed indexed node.
 * @index: Zero-based interrupt index.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Non-negative IRQ value, or negative for missing or short property.
 */
int of_irq_get(const struct device_node *node, int index);
/**
 * of_alias_get_id() - Find a decimal alias ID for a node and stem.
 * @node: Borrowed indexed node.
 * @stem: Non-NULL alias prefix, such as "serial".
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Non-negative alias ID, or negative when no matching alias exists.
 */
int of_alias_get_id(const struct device_node *node, const char *stem);
/**
 * of_node_path() - Copy a node's absolute FDT path into caller storage.
 * @node: Borrowed indexed node.
 * @path: Non-NULL destination buffer.
 * @size: Destination capacity in bytes, including NUL terminator.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Zero on success, negative for invalid input or insufficient capacity.
 */
int of_node_path(const struct device_node *node, char *path, uint32 size);
/**
 * of_memory_range_count() - Count usable memory reg tuples.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Non-negative count, or negative when the memory tree is malformed.
 */
int of_memory_range_count(void);
/**
 * of_memory_range_get() - Decode one usable physical memory range.
 * @index: Zero-based range index.
 * @range: Non-NULL destination.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Zero on success, negative for invalid index or malformed tuple.
 */
int of_memory_range_get(int index, struct of_memory_range *range);
/**
 * of_reserved_memory_range_count() - Count firmware RAM reservations
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Non-negative count, or negative for malformed reservation data.
 */
int of_reserved_memory_range_count(void);
/**
 * of_reserved_memory_range_get() - Decode one reserved physical memory range.
 * @index: Zero-based combined reservation index.
 * @range: Non-NULL destination.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Zero on success, negative for invalid index or malformed reservation data.
 */
int of_reserved_memory_range_get(int index,
				 struct of_memory_range *range);
/**
 * of_cpu_count() - Count available CPU nodes.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Number of available CPU nodes.
 */
int of_cpu_count(void);
/**
 * of_cpu_get() - Return one available CPU node and hart ID.
 * @index: Zero-based available CPU index.
 * @node: Non-NULL destination for borrowed CPU node.
 * @hart_id: Non-NULL destination for decoded native-endian hart ID.
 *
 * Context:
 * Any non-sleeping context after successful of_init().
 *
 * Return:
 * Zero on success, negative for invalid index or malformed reg layout.
 */
int of_cpu_get(int index, struct device_node **node, uint64 *hart_id);

#endif
