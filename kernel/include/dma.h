#ifndef __CAFFEINIX_KERNEL_DMA_H
#define __CAFFEINIX_KERNEL_DMA_H

#include <typedefs.h>

struct device;

typedef uint64 dma_addr_t;

enum dma_data_direction {
	DMA_BIDIRECTIONAL,
	DMA_TO_DEVICE,
	DMA_FROM_DEVICE,
};

void *dma_alloc_coherent(struct device *device, uint64 size,
			 dma_addr_t *dma_address);
void dma_free_coherent(struct device *device, uint64 size,
		       void *cpu_address, dma_addr_t dma_address);
int dma_map_single(struct device *device, void *cpu_address, uint64 size,
		   enum dma_data_direction direction,
		   dma_addr_t *dma_address);
void dma_unmap_single(struct device *device, dma_addr_t dma_address,
		      uint64 size, enum dma_data_direction direction);
void dma_sync_single_for_cpu(struct device *device,
			     dma_addr_t dma_address, uint64 size,
			     enum dma_data_direction direction);
void dma_sync_single_for_device(struct device *device,
				dma_addr_t dma_address, uint64 size,
				enum dma_data_direction direction);

static inline void dma_mb(void)
{
	__sync_synchronize();
}

static inline void dma_rmb(void)
{
	__sync_synchronize();
}

static inline void dma_wmb(void)
{
	__sync_synchronize();
}

#endif
