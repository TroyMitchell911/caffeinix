#include <debug.h>
#include <dma.h>
#include <mystring.h>
#include <palloc.h>
#include <virtio.h>
#include <virtio_ring.h>

static uint64 vring_available_size(uint16 size)
{
	return sizeof(struct vring_avail) +
	       sizeof(uint16) * size + sizeof(uint16);
}

static uint64 vring_used_size(uint16 size)
{
	return sizeof(struct vring_used) +
	       sizeof(struct vring_used_element) * size + sizeof(uint16);
}

struct virtqueue *virtqueue_create(struct virtio_device *device,
				   uint16 index, uint16 size,
				   void (*callback)(struct virtqueue *queue),
				   void (*notify)(struct virtqueue *queue),
				   const char *name)
{
	struct virtqueue *queue;
	uint16 descriptor;

	if (!device || !size || (size & (size - 1)) ||
	    sizeof(struct vring_desc) * size > PGSIZE ||
	    vring_available_size(size) > PGSIZE ||
	    vring_used_size(size) > PGSIZE)
		return 0;
	queue = calloc(1, sizeof(*queue));
	if (!queue)
		return 0;
	queue->device = device;
	queue->name = name;
	queue->index = index;
	queue->size = size;
	queue->callback = callback;
	queue->notify = notify;
	queue->free_count = size;
	queue->free_head = 0;
	queue->free_next = calloc(size, sizeof(*queue->free_next));
	queue->tokens = calloc(size, sizeof(*queue->tokens));
	queue->dma_addresses = calloc(size, sizeof(*queue->dma_addresses));
	queue->dma_lengths = calloc(size, sizeof(*queue->dma_lengths));
	queue->dma_directions = calloc(size, sizeof(*queue->dma_directions));
	queue->chain_heads = calloc(size, sizeof(*queue->chain_heads));
	if (!queue->free_next || !queue->tokens || !queue->dma_addresses ||
	    !queue->dma_lengths || !queue->dma_directions ||
	    !queue->chain_heads)
		goto failed;
	queue->descriptors = dma_alloc_coherent(&device->device, PGSIZE,
						&queue->descriptors_dma);
	queue->available = dma_alloc_coherent(&device->device, PGSIZE,
					      &queue->available_dma);
	queue->used = dma_alloc_coherent(&device->device, PGSIZE,
					 &queue->used_dma);
	if (!queue->descriptors || !queue->available || !queue->used)
		goto failed;
	for (descriptor = 0; descriptor < size; descriptor++) {
		queue->free_next[descriptor] = descriptor + 1;
		queue->chain_heads[descriptor] = ~(uint16)0;
	}
	spinlock_init(&queue->lock, name);
	return queue;

failed:
	virtqueue_destroy(queue);
	return 0;
}

void virtqueue_destroy(struct virtqueue *queue)
{
	if (!queue)
		return;
	if (queue->free_count != queue->size && queue->size)
		PANIC("destroy busy virtqueue");
	if (queue->descriptors)
		dma_free_coherent(&queue->device->device, PGSIZE,
				  queue->descriptors,
				  queue->descriptors_dma);
	if (queue->available)
		dma_free_coherent(&queue->device->device, PGSIZE,
				  queue->available, queue->available_dma);
	if (queue->used)
		dma_free_coherent(&queue->device->device, PGSIZE,
				  queue->used, queue->used_dma);
	free(queue->free_next);
	free(queue->tokens);
	free(queue->dma_addresses);
	free(queue->dma_lengths);
	free(queue->dma_directions);
	free(queue->chain_heads);
	free(queue);
}

static uint16 virtqueue_alloc_desc(struct virtqueue *queue)
{
	uint16 descriptor = queue->free_head;

	if (!queue->free_count)
		PANIC("virtqueue descriptor underflow");
	queue->free_head = queue->free_next[descriptor];
	queue->free_count--;
	return descriptor;
}

static void virtqueue_free_desc(struct virtqueue *queue,
				uint16 descriptor)
{
	queue->free_next[descriptor] = queue->free_head;
	queue->free_head = descriptor;
	queue->free_count++;
}

int virtqueue_add(struct virtqueue *queue, struct virtio_buffer *buffers,
		  uint16 count, void *token)
{
	uint16 allocated[32];
	uint16 descriptor, head, index, mapped = 0;

	if (!queue || !buffers || !count || count > 32 || !token)
		return -2;
	for (index = 0; index < count; index++) {
		if (buffers[index].direction != DMA_TO_DEVICE &&
		    buffers[index].direction != DMA_FROM_DEVICE)
			return -2;
	}
	spinlock_acquire(&queue->lock);
	if (queue->free_count < count) {
		spinlock_release(&queue->lock);
		return -1;
	}
	for (index = 0; index < count; index++)
		allocated[index] = virtqueue_alloc_desc(queue);
	for (index = 0; index < count; index++) {
		descriptor = allocated[index];
		if (dma_map_single(&queue->device->device,
				   buffers[index].address,
				   buffers[index].length,
				   buffers[index].direction,
				   &queue->dma_addresses[descriptor]) < 0)
			goto unwind;
		mapped++;
		queue->dma_lengths[descriptor] = buffers[index].length;
		queue->dma_directions[descriptor] =
			buffers[index].direction;
		queue->descriptors[descriptor].address =
			queue->dma_addresses[descriptor];
		queue->descriptors[descriptor].length =
			buffers[index].length;
		queue->descriptors[descriptor].flags =
			buffers[index].direction == DMA_FROM_DEVICE ?
			VRING_DESC_F_WRITE : 0;
		queue->descriptors[descriptor].next =
			index + 1 < count ? allocated[index + 1] : 0;
		if (index + 1 < count)
			queue->descriptors[descriptor].flags |=
				VRING_DESC_F_NEXT;
	}
	head = allocated[0];
	for (index = 0; index < count; index++)
		queue->chain_heads[allocated[index]] = head;
	queue->tokens[head] = token;
	queue->available->ring[queue->available_shadow % queue->size] = head;
	dma_wmb();
	queue->available_shadow++;
	queue->available->index = queue->available_shadow;
	spinlock_release(&queue->lock);
	return 0;

unwind:
	for (index = 0; index < mapped; index++) {
		descriptor = allocated[index];
		dma_unmap_single(&queue->device->device,
				 queue->dma_addresses[descriptor],
				 queue->dma_lengths[descriptor],
				 queue->dma_directions[descriptor]);
	}
	for (index = 0; index < count; index++)
		virtqueue_free_desc(queue, allocated[index]);
	spinlock_release(&queue->lock);
	return -2;
}

void virtqueue_kick(struct virtqueue *queue)
{
	if (!queue || !queue->notify)
		return;
	dma_wmb();
	queue->notify(queue);
}

static void virtqueue_release_chain(struct virtqueue *queue, uint16 head)
{
	uint16 descriptor = head;
	uint16 flags, next;
	uint16 count = 0;

	for (;;) {
		if (descriptor >= queue->size || count++ >= queue->size ||
		    queue->chain_heads[descriptor] != head)
			PANIC("invalid virtqueue descriptor chain");
		flags = queue->descriptors[descriptor].flags;
		next = queue->descriptors[descriptor].next;
		if (!(flags & VRING_DESC_F_NEXT))
			break;
		descriptor = next;
	}

	descriptor = head;
	for (;;) {
		flags = queue->descriptors[descriptor].flags;
		next = queue->descriptors[descriptor].next;
		dma_unmap_single(&queue->device->device,
				 queue->dma_addresses[descriptor],
				 queue->dma_lengths[descriptor],
				 queue->dma_directions[descriptor]);
		memset(&queue->descriptors[descriptor], 0,
		       sizeof(queue->descriptors[descriptor]));
		queue->chain_heads[descriptor] = ~(uint16)0;
		virtqueue_free_desc(queue, descriptor);
		if (!(flags & VRING_DESC_F_NEXT))
			break;
		descriptor = next;
	}
}

static uint16 virtqueue_used_pending_locked(struct virtqueue *queue)
{
	uint16 used_index;
	uint16 pending;

	used_index = __atomic_load_n(&queue->used->index,
				     __ATOMIC_RELAXED);
	pending = (uint16)(used_index - queue->used_shadow);
	if (pending > queue->size)
		PANIC("invalid virtqueue used index");
	return pending;
}

void *virtqueue_get_used(struct virtqueue *queue, uint32 *length)
{
	struct vring_used_element used;
	void *token;
	uint16 descriptor;

	if (!queue)
		return 0;
	spinlock_acquire(&queue->lock);
	if (!virtqueue_used_pending_locked(queue)) {
		spinlock_release(&queue->lock);
		return 0;
	}
	dma_rmb();
	used = queue->used->ring[queue->used_shadow % queue->size];
	if (used.id >= queue->size || !queue->tokens[used.id])
		PANIC("invalid virtqueue used descriptor");
	token = queue->tokens[used.id];
	queue->tokens[used.id] = 0;
	descriptor = used.id;
	virtqueue_release_chain(queue, descriptor);
	queue->used_shadow++;
	if (length)
		*length = used.length;
	spinlock_release(&queue->lock);
	return token;
}

void *virtqueue_detach_unused(struct virtqueue *queue)
{
	void *token = 0;
	uint16 descriptor;

	if (!queue)
		return 0;
	spinlock_acquire(&queue->lock);
	for (descriptor = 0; descriptor < queue->size; descriptor++) {
		if (!queue->tokens[descriptor])
			continue;
		token = queue->tokens[descriptor];
		queue->tokens[descriptor] = 0;
		virtqueue_release_chain(queue, descriptor);
		break;
	}
	spinlock_release(&queue->lock);
	return token;
}

int virtqueue_has_used(struct virtqueue *queue)
{
	int used;

	if (!queue)
		return 0;
	spinlock_acquire(&queue->lock);
	used = virtqueue_used_pending_locked(queue) != 0;
	spinlock_release(&queue->lock);
	return used;
}

uint16 virtqueue_num_free(struct virtqueue *queue)
{
	uint16 count;

	if (!queue)
		return 0;
	spinlock_acquire(&queue->lock);
	count = queue->free_count;
	spinlock_release(&queue->lock);
	return count;
}
