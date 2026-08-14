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

void memrange_init(struct memrange_set *set);
int memrange_add(struct memrange_set *set, uint64 start, uint64 end);
int memrange_remove(struct memrange_set *set, uint64 start, uint64 end);
int memrange_contains(const struct memrange_set *set, uint64 start,
		      uint64 end);
int memrange_get(const struct memrange_set *set, int index, uint64 *start,
		 uint64 *end);
int memrange_total(const struct memrange_set *set, uint64 *total);

#endif
