#include <riscv.h>
#include <vm_layout.h>

uint64 sv39_level_size(unsigned int level)
{
	if (level > SV39_LEVEL_MAX)
		return 0;
	return 1UL << PTEXSHIFT(level);
}

int sv39_best_map_level(uint64 virtual, uint64 physical, uint64 size)
{
	int level;

	for (level = SV39_LEVEL_MAX; level > 0; level--) {
		uint64 leaf_size = sv39_level_size(level);

		if (size >= leaf_size && !(virtual % leaf_size) &&
		    !(physical % leaf_size))
			return level;
	}
	return 0;
}
