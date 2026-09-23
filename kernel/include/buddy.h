/*
 * Page-granular buddy allocator metadata.
 *
 * The allocator manages disjoint physical regions.  It does not lock itself:
 * palloc serializes it and supplies the per-page state storage.
 */
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

/**
 * buddy_init() - Initialize empty buddy free lists.
 * @allocator: Allocator state to reset.
 *
 * Context: Early setup or caller-held allocator serialization. Does not sleep.
 */
void buddy_init(struct buddy_allocator *allocator);
/**
 * buddy_add_region() - Add one disjoint page-aligned managed region.
 * @allocator: Initialized allocator.
 * @start: Inclusive physical start of metadata-covered memory.
 * @end: Exclusive physical end.
 * @allocatable_start: First page available to callers.
 * @states: Caller-owned one-byte-per-page state storage.
 * @state_count: Number of elements in @states.
 *
 * Context: Early setup or externally serialized allocator access.
 * Return: %0 or %-1 for invalid, overlapping, or excess regions.
 */
int buddy_add_region(struct buddy_allocator *allocator, uint64 start,
		     uint64 end, uint64 allocatable_start, uint8 *states,
		     uint64 state_count);
/**
 * buddy_alloc() - Allocate a physically contiguous power-of-two page block.
 * @allocator: Allocator serialized by the caller.
 * @order: log2 of the requested page count, at most BUDDY_MAX_ORDER.
 *
 * Return: Page-aligned block address, or NULL when no suitable block exists.
 */
void *buddy_alloc(struct buddy_allocator *allocator, unsigned int order);
/**
 * buddy_free() - Return exactly one allocated buddy block.
 * @allocator: Allocator serialized by the caller.
 * @address: Address returned by buddy_alloc().
 * @order: Original allocation order.
 *
 * Return: %0 or %-1 when @address and @order do not describe a live block.
 */
int buddy_free(struct buddy_allocator *allocator, void *address,
	       unsigned int order);
/**
 * buddy_contains() - Test a page-aligned address for managed membership.
 * @allocator: Allocator to query.
 * @address: Physical page address.
 *
 * Context: Caller serializes concurrent allocator mutation. Does not sleep.
 * Return: Nonzero if the page is allocatable in one registered region.
 */
int buddy_contains(const struct buddy_allocator *allocator, uint64 address);
/**
 * buddy_allocated() - Validate an exact live allocation.
 * @allocator: Allocator to query.
 * @address: Block base.
 * @order: Block order.
 *
 * Context: Caller serializes concurrent allocator mutation. Does not sleep.
 * Return: Nonzero if the requested block is currently allocated.
 */
int buddy_allocated(const struct buddy_allocator *allocator, uint64 address,
		    unsigned int order);
/**
 * buddy_free_page_count() - Return pages presently on buddy free lists.
 * @allocator: Allocator to query.
 *
 * Context: Caller serializes concurrent allocator mutation. Does not sleep.
 * Return: Free page count, or zero for NULL.
 */
uint64 buddy_free_page_count(const struct buddy_allocator *allocator);

#endif
