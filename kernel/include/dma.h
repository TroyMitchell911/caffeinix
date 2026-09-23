/*
 * Caffeinix DMA mapping interface.
 *
 * Current platforms use direct, coherent mappings.  The API retains mapping
 * direction and ordering semantics so drivers do not depend on that shortcut.
 */
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

/**
 * dma_alloc_coherent() - Allocate contiguous memory shared with a device.
 * @device: Device whose DMA mask constrains the allocation.
 * @size: Non-zero byte count.
 * @dma_address: Receives the device-visible address.
 *
 * Context:
 * Process context; may allocate and sleep.  The caller owns the
 * returned memory and must release it with dma_free_coherent().
 * Return:
 * CPU address on success, or %NULL on invalid input or allocation
 * failure.
 */
void *dma_alloc_coherent(struct device *device, uint64 size,
			 dma_addr_t *dma_address);
/**
 * dma_free_coherent() - Release memory returned by dma_alloc_coherent().
 * @device: Device used for allocation.
 * @size: Original allocation size in bytes.
 * @cpu_address: CPU address returned by dma_alloc_coherent().
 * @dma_address: DMA address returned with @cpu_address.
 *
 * Context:
 * Process context; @cpu_address must not be in flight on the device.
 */
void dma_free_coherent(struct device *device, uint64 size,
		       void *cpu_address, dma_addr_t dma_address);
/**
 * dma_map_single() - Map one contiguous CPU buffer for a DMA transaction.
 * @device: DMA target.
 * @cpu_address: Non-NULL buffer address.
 * @size: Non-zero byte count.
 * @direction: Ownership direction for the transfer.
 * @dma_address: Receives the device-visible address.
 *
 * Context:
 * Atomic-safe; does not sleep.  Pair each success with unmap after
 * device ownership ends.
 * Return:
 * Zero on success, negative for invalid direction, range, or mask.
 */
int dma_map_single(struct device *device, void *cpu_address, uint64 size,
		   enum dma_data_direction direction,
		   dma_addr_t *dma_address);
/**
 * dma_unmap_single() - End a DMA mapping created by dma_map_single().
 * @device: Mapped device.
 * @dma_address: DMA address returned by map.
 * @size: Original mapped byte count.
 * @direction: Original mapping direction.
 *
 * Context:
 * Atomic-safe; no operation currently for coherent mappings.
 */
void dma_unmap_single(struct device *device, dma_addr_t dma_address,
		      uint64 size, enum dma_data_direction direction);
/**
 * dma_sync_single_for_cpu() - Make a DMA buffer visible to the CPU.
 * @device: Mapped device.
 * @dma_address: Active mapping address.
 * @size: Byte range to synchronize.
 * @direction: Mapping direction.
 *
 * Context:
 * Atomic-safe; an ordering barrier on current coherent platforms.
 */
void dma_sync_single_for_cpu(struct device *device,
			     dma_addr_t dma_address, uint64 size,
			     enum dma_data_direction direction);
/**
 * dma_sync_single_for_device() - Make CPU writes visible to a device.
 * @device: Mapped device.
 * @dma_address: Active mapping address.
 * @size: Byte range to synchronize.
 * @direction: Mapping direction.
 *
 * Context:
 * Atomic-safe; an ordering barrier on current coherent platforms.
 */
void dma_sync_single_for_device(struct device *device,
				dma_addr_t dma_address, uint64 size,
				enum dma_data_direction direction);

/**
 * dma_mb() - Order CPU and device-visible memory accesses in both directions.
 *
 * Context:
 * Atomic-safe; does not transfer ownership by itself.
 */
static inline void dma_mb(void)
{
	__sync_synchronize();
}

/**
 * dma_rmb() - Order a device completion observation before its data reads.
 *
 * Context:
 * Atomic-safe; use after observing a device-owned completion.
 */
static inline void dma_rmb(void)
{
	__sync_synchronize();
}

/**
 * dma_wmb() - Publish descriptor data before a device-visible producer index.
 *
 * Context:
 * Atomic-safe; use before handing a buffer to a device.
 */
static inline void dma_wmb(void)
{
	__sync_synchronize();
}

#endif
