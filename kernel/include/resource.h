/*
 * Device-tree resource descriptions shared by platform drivers.
 *
 * Memory resources use inclusive physical start and end addresses; IRQ
 * resources identify a controller source rather than an MMIO range.
 */
#ifndef __CAFFEINIX_KERNEL_RESOURCE_H
#define __CAFFEINIX_KERNEL_RESOURCE_H

#include <typedefs.h>

#define RESOURCE_MEM (1U << 0)
#define RESOURCE_IRQ (1U << 1)

struct resource {
	const char *name;
	uint64 start;
	uint64 end;
	uint32 flags;
};

/**
 * resource_size() - Return an inclusive memory resource length.
 * @resource: Resource with @end greater than or equal to @start.
 *
 * Context:
 * Any context; no locking or validation is performed.
 * Return:
 * Number of bytes in @resource.
 */
static inline uint64 resource_size(const struct resource *resource)
{
	return resource->end - resource->start + 1;
}

#endif
