#include <stdio.h>
#include <stdlib.h>

#include <riscv.h>
#include <vm_layout.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "Sv39 check failed at line %d\n", __LINE__); \
		exit(1); \
	} \
} while (0)

static uint64 mapping_count(uint64 address, uint64 size)
{
	uint64 count = 0;

	while (size) {
		int level = sv39_best_map_level(address, address, size);
		uint64 leaf_size = sv39_level_size(level);

		CHECK(leaf_size && leaf_size <= size);
		address += leaf_size;
		size -= leaf_size;
		count++;
	}
	return count;
}

int main(void)
{
	uint64 gib = 1UL << 30;
	uint64 mib = 1UL << 20;

	CHECK(sv39_level_size(0) == 4 * 1024);
	CHECK(sv39_level_size(1) == 2 * mib);
	CHECK(sv39_level_size(2) == gib);
	CHECK(sv39_best_map_level(2 * gib, 2 * gib, gib) == 2);
	CHECK(sv39_best_map_level(2 * mib, 2 * mib, 2 * mib) == 1);
	CHECK(sv39_best_map_level(2 * mib + 4096, 2 * mib + 4096,
				  2 * mib) == 0);
	CHECK(mapping_count(2 * gib, 128 * gib) == 128);
	CHECK(mapping_count(2 * gib + 4096, gib - 4096) == 1022);
	puts("SV39_OK");
	return 0;
}
