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

static inline uint64 resource_size(const struct resource *resource)
{
	return resource->end - resource->start + 1;
}

#endif
