#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dma.h>
#include <virtio.h>
#include <virtio_ring.h>

#define QUEUE_SIZE 8
#define WRAP_ROUNDS 1024

static unsigned int notifications;

void panic(char *message)
{
	fprintf(stderr, "virtqueue panic: %s\n", message);
	exit(EXIT_FAILURE);
}

void spinlock_init(spinlock_t lock, const char *name)
{
	memset(lock, 0, sizeof(*lock));
	lock->name = name;
}

void spinlock_acquire(spinlock_t lock)
{
	if (lock->locked)
		panic("recursive test lock");
	lock->locked = 1;
}

void spinlock_release(spinlock_t lock)
{
	if (!lock->locked)
		panic("unheld test lock");
	lock->locked = 0;
}

int spinlock_holding(spinlock_t lock)
{
	return lock->locked;
}

void *dma_alloc_coherent(struct device *device, uint64 size,
			 dma_addr_t *dma_address)
{
	void *memory;

	(void)device;
	if (posix_memalign(&memory, 4096, size))
		return NULL;
	memset(memory, 0, size);
	*dma_address = (dma_addr_t)memory;
	return memory;
}

void dma_free_coherent(struct device *device, uint64 size,
		       void *cpu_address, dma_addr_t dma_address)
{
	(void)device;
	(void)size;
	if ((dma_addr_t)cpu_address != dma_address)
		panic("coherent address mismatch");
	free(cpu_address);
}

int dma_map_single(struct device *device, void *cpu_address, uint64 size,
		   enum dma_data_direction direction,
		   dma_addr_t *dma_address)
{
	(void)device;
	(void)direction;
	if (!cpu_address || !size)
		return -1;
	*dma_address = (dma_addr_t)cpu_address;
	return 0;
}

void dma_unmap_single(struct device *device, dma_addr_t dma_address,
		      uint64 size, enum dma_data_direction direction)
{
	(void)device;
	(void)dma_address;
	(void)size;
	(void)direction;
}

static void notify(struct virtqueue *queue)
{
	(void)queue;
	notifications++;
}

static void complete_next(struct virtqueue *queue, uint32 length)
{
	uint16 slot = queue->used->index % queue->size;
	uint16 available = queue->used->index % queue->size;

	queue->used->ring[slot].id = queue->available->ring[available];
	queue->used->ring[slot].length = length;
	queue->used->index++;
}

static int test_chain(struct virtqueue *queue)
{
	unsigned char buffers[3][32];
	struct virtio_buffer scatter[3];
	uint32 length;
	int token;

	for (int index = 0; index < 3; index++) {
		scatter[index].address = buffers[index];
		scatter[index].length = sizeof(buffers[index]);
		scatter[index].direction = index == 1 ?
			DMA_FROM_DEVICE : DMA_TO_DEVICE;
	}
	if (virtqueue_add(queue, scatter, 3, &token) ||
	    queue->free_count != QUEUE_SIZE - 3)
		return -1;
	if (!(queue->descriptors[0].flags & VRING_DESC_F_NEXT) ||
	    !(queue->descriptors[1].flags & VRING_DESC_F_WRITE) ||
	    (queue->descriptors[2].flags & VRING_DESC_F_NEXT))
		return -1;
	complete_next(queue, 73);
	if (virtqueue_get_used(queue, &length) != &token || length != 73 ||
	    queue->free_count != QUEUE_SIZE)
		return -1;
	return 0;
}

static int test_exhaustion(struct virtqueue *queue)
{
	unsigned char buffers[QUEUE_SIZE + 1];
	struct virtio_buffer buffer = {
		.length = 1,
		.direction = DMA_TO_DEVICE,
	};
	int tokens[QUEUE_SIZE + 1];

	for (int index = 0; index < QUEUE_SIZE; index++) {
		buffer.address = &buffers[index];
		if (virtqueue_add(queue, &buffer, 1, &tokens[index]))
			return -1;
	}
	buffer.address = &buffers[QUEUE_SIZE];
	if (virtqueue_add(queue, &buffer, 1, &tokens[QUEUE_SIZE]) != -1)
		return -1;
	for (int index = 0; index < QUEUE_SIZE; index++) {
		complete_next(queue, 1);
		if (virtqueue_get_used(queue, NULL) != &tokens[index])
			return -1;
	}
	return queue->free_count == QUEUE_SIZE ? 0 : -1;
}

static int test_wraparound(struct virtqueue *queue)
{
	unsigned char data = 0;
	struct virtio_buffer buffer = {
		.address = &data,
		.length = 1,
		.direction = DMA_TO_DEVICE,
	};
	int token;

	for (int round = 0; round < WRAP_ROUNDS; round++) {
		if (virtqueue_add(queue, &buffer, 1, &token))
			return -1;
		virtqueue_kick(queue);
		complete_next(queue, 1);
		if (!virtqueue_has_used(queue) ||
		    virtqueue_get_used(queue, NULL) != &token ||
		    virtqueue_has_used(queue))
			return -1;
	}
	return notifications == WRAP_ROUNDS ? 0 : -1;
}

static int test_invalid_direction(struct virtqueue *queue)
{
	unsigned char data = 0;
	struct virtio_buffer buffer = {
		.address = &data,
		.length = 1,
		.direction = DMA_BIDIRECTIONAL,
	};
	int token;

	if (virtqueue_add(queue, &buffer, 1, &token) != -2)
		return -1;
	return queue->free_count == QUEUE_SIZE ? 0 : -1;
}

int main(void)
{
	struct virtio_device device = { 0 };
	struct virtqueue *queue;

	device.device.dma_mask = ~(uint64)0;
	queue = virtqueue_create(&device, 0, QUEUE_SIZE, NULL, notify,
				 "test virtqueue");
	if (!queue || test_chain(queue) || test_exhaustion(queue) ||
	    test_wraparound(queue) || test_invalid_direction(queue)) {
		fputs("virtqueue validation failed\n", stderr);
		return EXIT_FAILURE;
	}
	virtqueue_destroy(queue);
	puts("VIRTQUEUE_OK");
	return EXIT_SUCCESS;
}
