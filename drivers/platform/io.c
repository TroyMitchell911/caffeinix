/*
 * Identity mappings for platform MMIO resources. Callers serialize setup;
 * mappings and any allocated page tables persist after iounmap().
 */
#include <io.h>
#include <vm.h>

void *ioremap(uint64 address, uint64 size)
{
	if (!size || kvm_map_mmio(address, size) < 0)
		return 0;
	return (void *)address;
}

void iounmap(void *address, uint64 size)
{
	(void)address;
	(void)size;
}
