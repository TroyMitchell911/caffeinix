/* VirtIO split-ring queue API and ownership rules. */
#ifndef __CAFFEINIX_KERNEL_VIRTIO_RING_H
#define __CAFFEINIX_KERNEL_VIRTIO_RING_H

#include <dma.h>
#include <spinlock.h>
#include <typedefs.h>

#define VRING_DESC_F_NEXT 1
#define VRING_DESC_F_WRITE 2

struct virtio_device;
struct virtqueue;

struct virtio_buffer {
	void *address;
	uint32 length;
	enum dma_data_direction direction;
};

struct vring_desc {
	uint64 address;
	uint32 length;
	uint16 flags;
	uint16 next;
};

struct vring_avail {
	uint16 flags;
	uint16 index;
	uint16 ring[];
};

struct vring_used_element {
	uint32 id;
	uint32 length;
};

struct vring_used {
	uint16 flags;
	uint16 index;
	struct vring_used_element ring[];
};

struct virtqueue {
	struct virtio_device *device;
	const char *name;
	uint16 index;
	uint16 size;
	struct vring_desc *descriptors;
	struct vring_avail *available;
	struct vring_used *used;
	dma_addr_t descriptors_dma;
	dma_addr_t available_dma;
	dma_addr_t used_dma;
	uint16 available_shadow;
	uint16 used_shadow;
	uint16 free_count;
	uint16 free_head;
	uint16 *free_next;
	void **tokens;
	dma_addr_t *dma_addresses;
	uint32 *dma_lengths;
	enum dma_data_direction *dma_directions;
	uint16 *chain_heads;
	void (*callback)(struct virtqueue *queue);
	void (*notify)(struct virtqueue *queue);
	struct spinlock lock;
	void *private;
};

/**
 * virtqueue_create() - Allocate one power-of-two split virtqueue.
 * @device: Bound VirtIO device that owns the DMA mappings.
 * @index: Transport queue index.
 * @size: Non-zero power-of-two descriptor count fitting supported ring pages.
 * @callback: IRQ completion callback, or %NULL.
 * @notify: Transport notification callback, or %NULL.
 * @name: Stable diagnostic name.
 *
 * Context:
 * Process context; may allocate.  Caller owns the returned queue and
 * may destroy it only after reclaiming all tokens.
 * Return:
 * New queue or %NULL on invalid size or allocation failure.
 */
struct virtqueue *virtqueue_create(struct virtio_device *device,
				   uint16 index, uint16 size,
				   void (*callback)(struct virtqueue *queue),
				   void (*notify)(struct virtqueue *queue),
				   const char *name);
/**
 * virtqueue_destroy() - Free an idle virtqueue and its coherent rings.
 * @queue: Queue returned by virtqueue_create(), or %NULL.
 *
 * Context:
 * Process context; queue must have no submitted descriptor chains.
 */
void virtqueue_destroy(struct virtqueue *queue);
/**
 * virtqueue_add() - Map and publish one descriptor chain.
 * @queue: Live queue.
 * @buffers: Non-empty buffer array with valid DMA directions.
 * @count: Number of buffers, at most 32.
 * @token: Non-NULL caller token returned upon completion or detach.
 *
 * Context:
 * Atomic-safe; serializes queue state internally and does not notify.
 * @buffers and their memory stay valid until the token is returned.
 * Return:
 * Zero, -1 when full, or -2 for invalid input or mapping failure.
 */
int virtqueue_add(struct virtqueue *queue, struct virtio_buffer *buffers,
		  uint16 count, void *token);
/**
 * virtqueue_kick() - Notify a transport after publishing descriptors.
 * @queue: Live queue with a notify callback.
 *
 * Context:
 * Atomic-safe; issues a write barrier before notification.
 */
void virtqueue_kick(struct virtqueue *queue);
/**
 * virtqueue_get_used() - Reclaim the next device-completed chain.
 * @queue: Live queue.
 * @length: Optional destination for device-reported used length.
 *
 * Context:
 * Atomic-safe; completion callback callers must not sleep.
 * Return:
 * Original token, or %NULL when no completion is pending.
 */
void *virtqueue_get_used(struct virtqueue *queue, uint32 *length);
/**
 * virtqueue_detach_unused() - Reclaim one chain not completed by the device.
 * @queue: Live queue being stopped or reset.
 *
 * Context:
 * Process context after preventing new submissions.
 * Return:
 * One detached token, or %NULL when none remain.
 */
void *virtqueue_detach_unused(struct virtqueue *queue);
/**
 * virtqueue_has_used() - Test whether a completion is pending.
 * @queue: Live queue.
 *
 * Context:
 * Atomic-safe.
 * Return:
 * Non-zero when get_used() can reclaim one completion.
 */
int virtqueue_has_used(struct virtqueue *queue);
/**
 * virtqueue_num_free() - Return currently available descriptor slots.
 * @queue: Live queue.
 *
 * Context:
 * Atomic-safe.
 * Return:
 * Free descriptors, or zero for %NULL.
 */
uint16 virtqueue_num_free(struct virtqueue *queue);

#endif
