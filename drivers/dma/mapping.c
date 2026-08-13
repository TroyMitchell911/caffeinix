#include <debug.h>
#include <device_model.h>
#include <dma.h>
#include <mem_layout.h>
#include <mystring.h>
#include <palloc.h>
#include <vm.h>

static int dma_direction_valid(enum dma_data_direction direction)
{
	return direction == DMA_BIDIRECTIONAL ||
	       direction == DMA_TO_DEVICE ||
	       direction == DMA_FROM_DEVICE;
}

static int dma_range_valid(struct device *device, dma_addr_t address,
			   uint64 size)
{
	uint64 mask;

	if (!size || address + size - 1 < address)
		return 0;
	mask = device ? device->dma_mask : ~(uint64)0;
	return address <= mask && size - 1 <= mask - address;
}

void *dma_alloc_coherent(struct device *device, uint64 size,
			 dma_addr_t *dma_address)
{
	void *cpu_address;
	dma_addr_t address;

	if (!dma_address || !size || size > PGSIZE)
		return 0;
	cpu_address = palloc();
	if (!cpu_address)
		return 0;
	address = kvm_va2pa((uint64)cpu_address);
	if (!dma_range_valid(device, address, size)) {
		pfree(cpu_address);
		return 0;
	}
	memset(cpu_address, 0, PGSIZE);
	dma_wmb();
	*dma_address = address;
	return cpu_address;
}

void dma_free_coherent(struct device *device, uint64 size,
		       void *cpu_address, dma_addr_t dma_address)
{
	(void)device;
	if (!cpu_address || !size || size > PGSIZE ||
	    kvm_va2pa((uint64)cpu_address) != dma_address)
		PANIC("invalid coherent DMA free");
	dma_mb();
	pfree(cpu_address);
}

int dma_map_single(struct device *device, void *cpu_address, uint64 size,
		   enum dma_data_direction direction,
		   dma_addr_t *dma_address)
{
	uint64 end, page, physical, previous = 0;
	dma_addr_t first;

	if (!cpu_address || !size || !dma_address ||
	    !dma_direction_valid(direction) ||
	    (uint64)cpu_address + size - 1 < (uint64)cpu_address)
		return -1;
	first = kvm_va2pa((uint64)cpu_address);
	if (!first || !dma_range_valid(device, first, size))
		return -1;
	page = PGROUNDDOWN((uint64)cpu_address);
	end = PGROUNDDOWN((uint64)cpu_address + size - 1);
	for (;;) {
		physical = kvm_va2pa(page);
		if (!physical || (previous && physical != previous + PGSIZE))
			return -1;
		if (page == end)
			break;
		previous = physical;
		page += PGSIZE;
	}
	dma_sync_single_for_device(device, first, size, direction);
	*dma_address = first;
	return 0;
}

void dma_unmap_single(struct device *device, dma_addr_t dma_address,
		      uint64 size, enum dma_data_direction direction)
{
	dma_sync_single_for_cpu(device, dma_address, size, direction);
}

void dma_sync_single_for_cpu(struct device *device,
			     dma_addr_t dma_address, uint64 size,
			     enum dma_data_direction direction)
{
	(void)device;
	(void)dma_address;
	(void)size;
	(void)direction;
	dma_mb();
}

void dma_sync_single_for_device(struct device *device,
				dma_addr_t dma_address, uint64 size,
				enum dma_data_direction direction)
{
	(void)device;
	(void)dma_address;
	(void)size;
	(void)direction;
	dma_mb();
}
