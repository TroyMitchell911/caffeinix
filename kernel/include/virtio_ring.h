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

struct virtqueue *virtqueue_create(struct virtio_device *device,
				   uint16 index, uint16 size,
				   void (*callback)(struct virtqueue *queue),
				   void (*notify)(struct virtqueue *queue),
				   const char *name);
void virtqueue_destroy(struct virtqueue *queue);
int virtqueue_add(struct virtqueue *queue, struct virtio_buffer *buffers,
		  uint16 count, void *token);
void virtqueue_kick(struct virtqueue *queue);
void *virtqueue_get_used(struct virtqueue *queue, uint32 *length);
void *virtqueue_detach_unused(struct virtqueue *queue);
int virtqueue_has_used(struct virtqueue *queue);
uint16 virtqueue_num_free(struct virtqueue *queue);

#endif
