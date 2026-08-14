#include <memrange.h>

static void memrange_delete(struct memrange_set *set, int index)
{
	int i;

	for (i = index; i + 1 < set->count; i++)
		set->ranges[i] = set->ranges[i + 1];
	set->count--;
}

void memrange_init(struct memrange_set *set)
{
	set->count = 0;
}

int memrange_add(struct memrange_set *set, uint64 start, uint64 end)
{
	int first, last, i;

	if (!set || start >= end)
		return -1;
	for (first = 0; first < set->count; first++) {
		if (set->ranges[first].end >= start)
			break;
	}
	last = first;
	while (last < set->count && set->ranges[last].start <= end) {
		if (set->ranges[last].start < start)
			start = set->ranges[last].start;
		if (set->ranges[last].end > end)
			end = set->ranges[last].end;
		last++;
	}
	if (first == last) {
		if (set->count >= MEMRANGE_MAX)
			return -1;
		for (i = set->count; i > first; i--)
			set->ranges[i] = set->ranges[i - 1];
		set->count++;
	} else {
		while (last-- > first + 1)
			memrange_delete(set, first + 1);
	}
	set->ranges[first].start = start;
	set->ranges[first].end = end;
	return 0;
}

int memrange_remove(struct memrange_set *set, uint64 start, uint64 end)
{
	int i, j;

	if (!set || start >= end)
		return -1;
	for (i = 0; i < set->count;) {
		struct memrange *range = &set->ranges[i];

		if (end <= range->start)
			break;
		if (start >= range->end) {
			i++;
			continue;
		}
		if (start <= range->start && end >= range->end) {
			memrange_delete(set, i);
			continue;
		}
		if (start <= range->start) {
			range->start = end;
			break;
		}
		if (end >= range->end) {
			range->end = start;
			i++;
			continue;
		}
		if (set->count >= MEMRANGE_MAX)
			return -1;
		for (j = set->count; j > i + 1; j--)
			set->ranges[j] = set->ranges[j - 1];
		set->count++;
		set->ranges[i + 1].start = end;
		set->ranges[i + 1].end = range->end;
		range->end = start;
		break;
	}
	return 0;
}

int memrange_contains(const struct memrange_set *set, uint64 start,
		      uint64 end)
{
	int i;

	if (!set || start >= end)
		return 0;
	for (i = 0; i < set->count; i++) {
		if (start < set->ranges[i].start)
			return 0;
		if (start >= set->ranges[i].start &&
		    end <= set->ranges[i].end)
			return 1;
	}
	return 0;
}

int memrange_get(const struct memrange_set *set, int index, uint64 *start,
		 uint64 *end)
{
	if (!set || index < 0 || index >= set->count || !start || !end)
		return -1;
	*start = set->ranges[index].start;
	*end = set->ranges[index].end;
	return 0;
}

int memrange_total(const struct memrange_set *set, uint64 *total)
{
	uint64 bytes = 0;
	int i;

	if (!set || !total)
		return -1;
	for (i = 0; i < set->count; i++) {
		uint64 size = set->ranges[i].end - set->ranges[i].start;

		if (bytes + size < bytes)
			return -1;
		bytes += size;
	}
	*total = bytes;
	return 0;
}
