/*
 * Sorted, non-overlapping half-open physical-memory intervals.
 *
 * Users construct sets during early memory discovery.  All operations merge
 * adjacent ranges, so callers must not retain pointers into ranges[].
 */
#ifndef __CAFFEINIX_KERNEL_MEMRANGE_H
#define __CAFFEINIX_KERNEL_MEMRANGE_H

#include <typedefs.h>

#define MEMRANGE_MAX 64

struct memrange {
	uint64 start;
	uint64 end;
};

struct memrange_set {
	struct memrange ranges[MEMRANGE_MAX];
	int count;
};

/**
 * memrange_init() - Empty a range set.
 * @set: Set to initialize.
 *
 * Context: Caller exclusively owns @set; does not sleep.
 */
void memrange_init(struct memrange_set *set);
/**
 * memrange_add() - Union a half-open interval into a set.
 * @set: Initialized destination set.
 * @start: Inclusive byte address.
 * @end: Exclusive byte address; must exceed @start.
 *
 * Context: Non-sleeping; callers serialize concurrent access.
 * Return: %0 on success or %-1 for invalid input or a full set.
 */
int memrange_add(struct memrange_set *set, uint64 start, uint64 end);
/**
 * memrange_remove() - Subtract a half-open interval from a set.
 * @set: Initialized destination set.
 * @start: Inclusive byte address.
 * @end: Exclusive byte address; must exceed @start.
 *
 * Context: Non-sleeping; callers serialize concurrent access.
 * Return: %0 on success or %-1 for invalid input or an unrepresentable split.
 */
int memrange_remove(struct memrange_set *set, uint64 start, uint64 end);
/**
 * memrange_contains() - Test whether one interval is wholly represented.
 * @set: Set to query.
 * @start: Inclusive byte address.
 * @end: Exclusive byte address.
 *
 * Return: Nonzero when [@start, @end) belongs to one stored range.
 */
int memrange_contains(const struct memrange_set *set, uint64 start,
		      uint64 end);
/**
 * memrange_get() - Copy one stored interval.
 * @set: Set to query.
 * @index: Zero-based range index.
 * @start: Receives the inclusive address.
 * @end: Receives the exclusive address.
 *
 * Return: %0 or %-1 for invalid arguments or index.
 */
int memrange_get(const struct memrange_set *set, int index, uint64 *start,
		 uint64 *end);
/**
 * memrange_total() - Sum bytes represented by a set.
 * @set: Set to query.
 * @total: Receives the byte count.
 *
 * Return: %0 or %-1 for invalid arguments or an unsigned overflow.
 */
int memrange_total(const struct memrange_set *set, uint64 *total);

#endif
