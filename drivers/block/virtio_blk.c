#include <block_device.h>
#include <debug.h>
#include <mystring.h>
#include <palloc.h>
#include <printk.h>
#include <spinlock.h>
#include <virtio.h>
#include <virtio_ring.h>
#include <wait.h>

#define VIRTIO_BLK_F_FLUSH 9

#define VIRTIO_BLK_T_IN 0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_T_FLUSH 4

#define VIRTIO_BLK_S_OK 0
#define VIRTIO_BLK_SECTOR_SIZE 512

struct virtio_blk_header {
	uint32 type;
	uint32 reserved;
	uint64 sector;
};

struct virtio_blk_io {
	struct virtio_blk_header header;
	struct block_request *request;
	uint8 status;
};

struct virtio_blk {
	struct virtio_device *virtio;
	struct virtqueue *queue;
	struct spinlock lock;
	struct wait_queue descriptor_wait;
	struct block_device block;
	uint32 outstanding;
	uint8 has_flush;
	char name[16];
};

static struct virtio_blk *debug_disks[BLOCK_DEVICE_MAX];
static uint32 debug_readers;

void virtio_blk_debug_dump(void)
{
	struct virtio_blk *disk;
	struct virtqueue *queue;
	uint16 available, used;
	int found = 0;
	uint32 id;

	__atomic_add_fetch(&debug_readers, 1, __ATOMIC_SEQ_CST);
	for (id = 1; id < BLOCK_DEVICE_MAX; id++) {
		disk = __atomic_load_n(&debug_disks[id], __ATOMIC_SEQ_CST);
		if (!disk || !(queue = disk->queue))
			continue;
		found = 1;
		available = __atomic_load_n(&queue->available->index,
					    __ATOMIC_RELAXED);
		used = __atomic_load_n(&queue->used->index,
				       __ATOMIC_RELAXED);
		printf_emergency("%s avail=%u avail_shadow=%u used=%u "
				 "used_shadow=%u free=%u size=%u\n", disk->name,
				 available, queue->available_shadow, used,
				 queue->used_shadow, queue->free_count,
				 queue->size);
	}
	if (!found)
		printf_emergency("virtio-blk unavailable\n");
	__atomic_sub_fetch(&debug_readers, 1, __ATOMIC_SEQ_CST);
}

static int virtio_blk_submit(struct block_device *device,
			     struct block_request *request)
{
	struct virtio_blk *disk = device->private;
	struct virtio_blk_io *io;
	struct virtio_buffer buffers[BLOCK_REQUEST_MAX_SEGMENTS + 2];
	uint32 length;
	uint16 index;
	uint16 count = 0;
	int status;

	io = calloc(1, sizeof(*io));
	if (!io)
		return -1;
	io->request = request;
	io->header.sector = request->sector;
	io->status = 0xff;
	switch (request->operation) {
	case BLOCK_REQUEST_READ:
		io->header.type = VIRTIO_BLK_T_IN;
		break;
	case BLOCK_REQUEST_WRITE:
		io->header.type = VIRTIO_BLK_T_OUT;
		break;
	case BLOCK_REQUEST_FLUSH:
		if (!disk->has_flush) {
			block_request_complete(request, 0);
			free(io);
			return 0;
		}
		io->header.type = VIRTIO_BLK_T_FLUSH;
		break;
	default:
		free(io);
		return -1;
	}
	buffers[count].address = &io->header;
	buffers[count].length = sizeof(io->header);
	buffers[count++].direction = DMA_TO_DEVICE;
	for (index = 0; index < request->segment_count; index++) {
		if (request->segments[index].sector_count >
		    0xffffffffU / VIRTIO_BLK_SECTOR_SIZE) {
			free(io);
			return -1;
		}
		length = request->segments[index].sector_count *
			 VIRTIO_BLK_SECTOR_SIZE;
		buffers[count].address = request->segments[index].buffer;
		buffers[count].length = length;
		buffers[count++].direction =
			request->operation == BLOCK_REQUEST_READ ?
			DMA_FROM_DEVICE :
			DMA_TO_DEVICE;
	}
	buffers[count].address = &io->status;
	buffers[count].length = sizeof(io->status);
	buffers[count++].direction = DMA_FROM_DEVICE;
	if (count > disk->queue->size) {
		free(io);
		return -1;
	}

	spinlock_acquire(&disk->lock);
	while ((status = virtqueue_add(disk->queue, buffers, count,
				       io)) == -1)
		wait_queue_sleep(&disk->descriptor_wait, &disk->lock);
	if (status < 0) {
		spinlock_release(&disk->lock);
		free(io);
		return -1;
	}
	disk->outstanding++;
	virtqueue_kick(disk->queue);
	spinlock_release(&disk->lock);
	return 0;
}

static void virtio_blk_done(struct virtqueue *queue)
{
	struct virtio_blk *disk = queue->private;
	struct virtio_blk_io *io;

	while ((io = virtqueue_get_used(queue, 0))) {
		block_request_complete(io->request,
			io->status == VIRTIO_BLK_S_OK ? 0 : -1);
		free(io);
		spinlock_acquire(&disk->lock);
		if (!disk->outstanding)
			PANIC("virtio-blk outstanding underflow");
		disk->outstanding--;
		spinlock_release(&disk->lock);
	}
	spinlock_acquire(&disk->lock);
	wait_queue_wake_all(&disk->descriptor_wait);
	spinlock_release(&disk->lock);
}

static const struct block_device_operations virtio_blk_operations = {
	.submit = virtio_blk_submit,
};

static int virtio_blk_probe(struct virtio_device *device)
{
	static const char *const names[] = { "virtio-blk requests" };
	void (*callbacks[])(struct virtqueue *) = { virtio_blk_done };
	struct virtio_blk *disk;
	uint64 capacity;

	disk = calloc(1, sizeof(*disk));
	if (!disk)
		return DRIVER_ERR_BUSY;
	disk->virtio = device;
	spinlock_init(&disk->lock, "virtio-blk");
	wait_queue_init(&disk->descriptor_wait,
			"virtio-blk descriptors");
	if (virtio_find_vqs(device, 1, &disk->queue, callbacks, names) < 0)
		goto failed;
	if (disk->queue->size <= 2)
		goto failed;
	disk->queue->private = disk;
	device->config->get_config(device, 0, &capacity, sizeof(capacity));
	if (!capacity)
		goto failed;
	safe_strncpy(disk->name, "virtio-blk0", sizeof(disk->name));
	disk->block.name = disk->name;
	disk->block.sector_size = VIRTIO_BLK_SECTOR_SIZE;
	disk->block.sector_count = capacity;
	disk->block.max_segments = disk->queue->size - 2;
	if (disk->block.max_segments > BLOCK_REQUEST_MAX_SEGMENTS)
		disk->block.max_segments = BLOCK_REQUEST_MAX_SEGMENTS;
	disk->block.operations = &virtio_blk_operations;
	disk->block.private = disk;
	disk->has_flush = virtio_has_feature(device, VIRTIO_BLK_F_FLUSH);
	if (block_device_register(&disk->block) < 0)
		goto failed;
	disk->name[10] = '0' + disk->block.id - 1;
	__atomic_store_n(&debug_disks[disk->block.id], disk,
			 __ATOMIC_SEQ_CST);
	dev_set_drvdata(&device->device, disk);
	pr_info("%s: %lu sectors (%lu MiB)", disk->name,
		disk->block.sector_count,
		disk->block.sector_count / 2048);
	return DRIVER_OK;

failed:
	if (disk->queue)
		device->config->del_vqs(device);
	free(disk);
	return DRIVER_ERR_NODEV;
}

static void virtio_blk_remove(struct virtio_device *device)
{
	struct virtio_blk *disk = dev_get_drvdata(&device->device);

	if (!disk)
		return;
	if (disk->outstanding || !wait_queue_empty(&disk->descriptor_wait))
		PANIC("remove busy virtio-blk");
	__atomic_store_n(&debug_disks[disk->block.id], 0, __ATOMIC_SEQ_CST);
	while (__atomic_load_n(&debug_readers, __ATOMIC_SEQ_CST))
		;
	block_device_unregister(&disk->block);
	dev_set_drvdata(&device->device, 0);
	free(disk);
}

static const struct virtio_device_id virtio_blk_ids[] = {
	{ VIRTIO_ID_BLOCK, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_blk_driver = {
	.driver = {
		.name = "virtio-blk",
	},
	.id_table = virtio_blk_ids,
	.feature_table = 1ULL << VIRTIO_BLK_F_FLUSH,
	.probe = virtio_blk_probe,
	.remove = virtio_blk_remove,
};

int virtio_blk_init(void)
{
	return virtio_driver_register(&virtio_blk_driver);
}
