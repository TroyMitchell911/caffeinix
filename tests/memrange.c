#include <stdio.h>
#include <stdlib.h>

#include <memrange.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "memrange check failed at line %d\n", __LINE__); \
		exit(1); \
	} \
} while (0)

static void check_range(const struct memrange_set *set, int index,
			uint64 start, uint64 end)
{
	uint64 actual_start, actual_end;

	CHECK(memrange_get(set, index, &actual_start, &actual_end) == 0);
	CHECK(actual_start == start);
	CHECK(actual_end == end);
}

static void test_add_and_merge(void)
{
	struct memrange_set set;

	memrange_init(&set);
	CHECK(memrange_add(&set, 0x4000, 0x8000) == 0);
	CHECK(memrange_add(&set, 0x1000, 0x2000) == 0);
	CHECK(memrange_add(&set, 0x2000, 0x5000) == 0);
	CHECK(set.count == 1);
	check_range(&set, 0, 0x1000, 0x8000);
}

static void test_remove(void)
{
	struct memrange_set set;
	uint64 total;

	memrange_init(&set);
	CHECK(memrange_add(&set, 0x1000, 0x10000) == 0);
	CHECK(memrange_remove(&set, 0x4000, 0x6000) == 0);
	CHECK(set.count == 2);
	check_range(&set, 0, 0x1000, 0x4000);
	check_range(&set, 1, 0x6000, 0x10000);
	CHECK(memrange_remove(&set, 0, 0x2000) == 0);
	CHECK(memrange_remove(&set, 0xe000, 0x20000) == 0);
	check_range(&set, 0, 0x2000, 0x4000);
	check_range(&set, 1, 0x6000, 0xe000);
	CHECK(memrange_total(&set, &total) == 0);
	CHECK(total == 0xa000);
	CHECK(memrange_contains(&set, 0x2000, 0x3000));
	CHECK(!memrange_contains(&set, 0x4000, 0x5000));
}

static void test_large_sparse_layout(void)
{
	struct memrange_set set;
	uint64 gib = 1UL << 30;
	uint64 total;

	memrange_init(&set);
	CHECK(memrange_add(&set, 2 * gib, 130 * gib) == 0);
	CHECK(memrange_remove(&set, 2 * gib, 2 * gib + 16 * 1024 * 1024) == 0);
	CHECK(memrange_remove(&set, 64 * gib, 65 * gib) == 0);
	CHECK(set.count == 2);
	CHECK(memrange_total(&set, &total) == 0);
	CHECK(total == 127 * gib - 16 * 1024 * 1024);
}

int main(void)
{
	test_add_and_merge();
	test_remove();
	test_large_sparse_layout();
	puts("MEMRANGE_OK");
	return 0;
}
