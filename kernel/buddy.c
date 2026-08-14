#include <buddy.h>
#include <riscv.h>

#define BUDDY_STATE_FREE 0x80
#define BUDDY_STATE_ALLOCATED 0x40
#define BUDDY_STATE_UNUSED 0xff

struct buddy_block {
	struct list node;
};

static uint64 buddy_order_size(unsigned int order)
{
	return PGSIZE << order;
}

static struct buddy_region *buddy_find_region(
	struct buddy_allocator *allocator, uint64 address)
{
	int i;

	for (i = 0; i < allocator->region_count; i++) {
		struct buddy_region *region = &allocator->regions[i];

		if (address >= region->start && address < region->end)
			return region;
	}
	return 0;
}

static const struct buddy_region *buddy_find_region_const(
	const struct buddy_allocator *allocator, uint64 address)
{
	int i;

	for (i = 0; i < allocator->region_count; i++) {
		const struct buddy_region *region = &allocator->regions[i];

		if (address >= region->start && address < region->end)
			return region;
	}
	return 0;
}

static uint8 *buddy_state(struct buddy_region *region, uint64 address)
{
	uint64 index = (address - region->start) / PGSIZE;

	if (index >= region->state_count)
		return 0;
	return &region->states[index];
}

static const uint8 *buddy_state_const(const struct buddy_region *region,
				      uint64 address)
{
	uint64 index = (address - region->start) / PGSIZE;

	if (index >= region->state_count)
		return 0;
	return &region->states[index];
}

static int buddy_block_fits(const struct buddy_region *region,
			    uint64 address, unsigned int order)
{
	uint64 size;

	if (order > BUDDY_MAX_ORDER || address < region->allocatable_start ||
	    address % PGSIZE)
		return 0;
	size = buddy_order_size(order);
	return !(address % size) && address <= region->end &&
	       size <= region->end - address;
}

static void buddy_list_add(struct buddy_allocator *allocator,
			   struct buddy_region *region, uint64 address,
			   unsigned int order)
{
	struct buddy_block *block = (struct buddy_block *)address;
	uint8 *state = buddy_state(region, address);

	list_init(&block->node);
	list_insert_after(&allocator->areas[order].blocks, &block->node);
	allocator->areas[order].count++;
	*state = BUDDY_STATE_FREE | order;
}

static void buddy_list_remove(struct buddy_allocator *allocator,
			      uint64 address, unsigned int order)
{
	struct buddy_block *block = (struct buddy_block *)address;

	list_remove(&block->node);
	allocator->areas[order].count--;
}

static unsigned int buddy_largest_order(uint64 address, uint64 pages)
{
	unsigned int order = 0;
	uint64 page = address / PGSIZE;

	while (order < BUDDY_MAX_ORDER &&
	       !(page & (1UL << order)) &&
	       (1UL << (order + 1)) <= pages)
		order++;
	return order;
}

void buddy_init(struct buddy_allocator *allocator)
{
	int order;

	allocator->region_count = 0;
	allocator->free_pages = 0;
	for (order = 0; order <= BUDDY_MAX_ORDER; order++) {
		list_init(&allocator->areas[order].blocks);
		allocator->areas[order].count = 0;
	}
}

int buddy_add_region(struct buddy_allocator *allocator, uint64 start,
		     uint64 end, uint64 allocatable_start, uint8 *states,
		     uint64 state_count)
{
	struct buddy_region *region;
	uint64 address, pages;
	unsigned int order;
	int i;

	if (!allocator || !states || start >= end ||
	    start % PGSIZE || end % PGSIZE ||
	    allocatable_start < start || allocatable_start > end ||
	    allocatable_start % PGSIZE ||
	    state_count < (end - start) / PGSIZE ||
	    allocator->region_count >= MEMRANGE_MAX)
		return -1;
	for (i = 0; i < allocator->region_count; i++) {
		region = &allocator->regions[i];
		if (start < region->end && end > region->start)
			return -1;
	}
	region = &allocator->regions[allocator->region_count++];
	region->start = start;
	region->end = end;
	region->allocatable_start = allocatable_start;
	region->states = states;
	region->state_count = state_count;
	for (address = 0; address < state_count; address++)
		states[address] = BUDDY_STATE_UNUSED;

	address = allocatable_start;
	while (address < end) {
		pages = (end - address) / PGSIZE;
		order = buddy_largest_order(address, pages);
		buddy_list_add(allocator, region, address, order);
		allocator->free_pages += 1UL << order;
		address += buddy_order_size(order);
	}
	return 0;
}

void *buddy_alloc(struct buddy_allocator *allocator, unsigned int order)
{
	struct buddy_area *area;
	struct buddy_block *block;
	struct buddy_region *region;
	uint64 address, right;
	uint8 *state;
	unsigned int current;

	if (!allocator || order > BUDDY_MAX_ORDER)
		return 0;
	for (current = order; current <= BUDDY_MAX_ORDER; current++) {
		area = &allocator->areas[current];
		if (area->count)
			break;
	}
	if (current > BUDDY_MAX_ORDER)
		return 0;
	block = list_entry(area->blocks.next, struct buddy_block, node);
	address = (uint64)block;
	region = buddy_find_region(allocator, address);
	state = buddy_state(region, address);
	buddy_list_remove(allocator, address, current);
	*state = BUDDY_STATE_UNUSED;
	while (current > order) {
		current--;
		right = address + buddy_order_size(current);
		buddy_list_add(allocator, region, right, current);
	}
	*state = BUDDY_STATE_ALLOCATED | order;
	allocator->free_pages -= 1UL << order;
	return (void *)address;
}

int buddy_allocated(const struct buddy_allocator *allocator, uint64 address,
		    unsigned int order)
{
	const struct buddy_region *region;
	const uint8 *state;

	if (!allocator || order > BUDDY_MAX_ORDER || address % PGSIZE)
		return 0;
	region = buddy_find_region_const(allocator, address);
	if (!region || !buddy_block_fits(region, address, order))
		return 0;
	state = buddy_state_const(region, address);
	return state && *state == (BUDDY_STATE_ALLOCATED | order);
}

int buddy_free(struct buddy_allocator *allocator, void *pointer,
	       unsigned int order)
{
	struct buddy_region *region;
	uint64 address = (uint64)pointer;
	uint64 buddy, size;
	uint8 *state, *buddy_status;
	unsigned int current = order;

	if (!buddy_allocated(allocator, address, order))
		return -1;
	region = buddy_find_region(allocator, address);
	state = buddy_state(region, address);
	*state = BUDDY_STATE_UNUSED;
	while (current < BUDDY_MAX_ORDER) {
		size = buddy_order_size(current);
		buddy = address ^ size;
		if (!buddy_block_fits(region, buddy, current))
			break;
		buddy_status = buddy_state(region, buddy);
		if (!buddy_status ||
		    *buddy_status != (BUDDY_STATE_FREE | current))
			break;
		buddy_list_remove(allocator, buddy, current);
		*buddy_status = BUDDY_STATE_UNUSED;
		if (buddy < address)
			address = buddy;
		current++;
	}
	buddy_list_add(allocator, region, address, current);
	allocator->free_pages += 1UL << order;
	return 0;
}

int buddy_contains(const struct buddy_allocator *allocator, uint64 address)
{
	const struct buddy_region *region;

	if (!allocator || address % PGSIZE)
		return 0;
	region = buddy_find_region_const(allocator, address);
	return region && address >= region->allocatable_start;
}

uint64 buddy_free_page_count(const struct buddy_allocator *allocator)
{
	return allocator ? allocator->free_pages : 0;
}
