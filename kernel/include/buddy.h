#ifndef __CAFFEINIX_KERNEL_BUDDY_H
#define __CAFFEINIX_KERNEL_BUDDY_H

#include <list.h>
#include <memrange.h>
#include <typedefs.h>

#define BUDDY_MAX_ORDER 18

struct buddy_area {
	struct list blocks;
	uint64 count;
};

struct buddy_region {
	uint64 start;
	uint64 end;
	uint64 allocatable_start;
	uint8 *states;
	uint64 state_count;
};

struct buddy_allocator {
	struct buddy_area areas[BUDDY_MAX_ORDER + 1];
	struct buddy_region regions[MEMRANGE_MAX];
	int region_count;
	uint64 free_pages;
};

void buddy_init(struct buddy_allocator *allocator);
int buddy_add_region(struct buddy_allocator *allocator, uint64 start,
		     uint64 end, uint64 allocatable_start, uint8 *states,
		     uint64 state_count);
void *buddy_alloc(struct buddy_allocator *allocator, unsigned int order);
int buddy_free(struct buddy_allocator *allocator, void *address,
	       unsigned int order);
int buddy_contains(const struct buddy_allocator *allocator, uint64 address);
int buddy_allocated(const struct buddy_allocator *allocator, uint64 address,
		    unsigned int order);
uint64 buddy_free_page_count(const struct buddy_allocator *allocator);

#endif
