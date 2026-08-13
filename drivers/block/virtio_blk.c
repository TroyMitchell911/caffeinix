#include <block_device.h>
#include <debug.h>
#include <mystring.h>
#include <palloc.h>
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

struct virtio_blk_request {
	struct virtio_blk_header header;
	struct spinlock lock;
	struct wait_queue completion;
	volatile uint8 pending;
	uint8 status;
	int result;
};

struct virtio_blk {
	struct virtio_device *virtio;
	struct virtqueue *queue;
	struct spinlock lock;
	struct wait_queue descriptor_wait;
	struct block_device block;
	uint8 has_flush;
	char name[16];
};

static int virtio_blk_submit(struct virtio_blk *disk, uint32 type,
			     uint64 sector, void *buffer, uint32 length)
{
	struct virtio_blk_request request;
	struct virtio_buffer buffers[3];
	uint16 count = 0;
	int status;

	memset(&request, 0, sizeof(request));
	request.header.type = type;
	request.header.sector = sector;
	request.status = 0xff;
	request.pending = 1;
	request.result = -1;
	spinlock_init(&request.lock, "virtio-blk request");
	wait_queue_init(&request.completion, "virtio-blk completion");
	buffers[count].address = &request.header;
	buffers[count].length = sizeof(request.header);
	buffers[count++].direction = DMA_TO_DEVICE;
	if (length) {
		buffers[count].address = buffer;
		buffers[count].length = length;
		buffers[count++].direction =
			type == VIRTIO_BLK_T_IN ? DMA_FROM_DEVICE :
			DMA_TO_DEVICE;
	}
	buffers[count].address = &request.status;
	buffers[count].length = sizeof(request.status);
	buffers[count++].direction = DMA_FROM_DEVICE;

	spinlock_acquire(&disk->lock);
	while ((status = virtqueue_add(disk->queue, buffers, count,
				       &request)) == -1)
		wait_queue_sleep(&disk->descriptor_wait, &disk->lock);
	if (status < 0) {
		spinlock_release(&disk->lock);
		return -1;
	}
	virtqueue_kick(disk->queue);
	spinlock_release(&disk->lock);

	spinlock_acquire(&request.lock);
	while (request.pending)
		wait_queue_sleep(&request.completion, &request.lock);
	status = request.result;
	if (!wait_queue_empty(&request.completion))
		PANIC("virtio-blk completion waiters");
	spinlock_release(&request.lock);
	return status;
}

static void virtio_blk_done(struct virtqueue *queue)
{
	struct virtio_blk *disk = queue->private;
	struct virtio_blk_request *request;

	while ((request = virtqueue_get_used(queue, 0))) {
		spinlock_acquire(&request->lock);
		if (!request->pending)
			PANIC("duplicate virtio-blk completion");
		request->result =
			request->status == VIRTIO_BLK_S_OK ? 0 : -1;
		request->pending = 0;
		wait_queue_wake_one(&request->completion);
		spinlock_release(&request->lock);
	}
	spinlock_acquire(&disk->lock);
	wait_queue_wake_all(&disk->descriptor_wait);
	spinlock_release(&disk->lock);
}

static int virtio_blk_read(struct block_device *device, uint64 sector,
			   void *buffer, uint32 count)
{
	struct virtio_blk *disk = device->private;

	if (count > 0xffffffffU / VIRTIO_BLK_SECTOR_SIZE)
		return -1;
	return virtio_blk_submit(disk, VIRTIO_BLK_T_IN, sector, buffer,
				 count * VIRTIO_BLK_SECTOR_SIZE);
}

static int virtio_blk_write(struct block_device *device, uint64 sector,
			    const void *buffer, uint32 count)
{
	struct virtio_blk *disk = device->private;

	if (count > 0xffffffffU / VIRTIO_BLK_SECTOR_SIZE)
		return -1;
	return virtio_blk_submit(disk, VIRTIO_BLK_T_OUT, sector,
				 (void *)buffer,
				 count * VIRTIO_BLK_SECTOR_SIZE);
}

static int virtio_blk_flush(struct block_device *device)
{
	struct virtio_blk *disk = device->private;

	return disk->has_flush ? virtio_blk_submit(
		disk, VIRTIO_BLK_T_FLUSH, 0, 0, 0) : 0;
}

static const struct block_device_operations virtio_blk_operations = {
	.read = virtio_blk_read,
	.write = virtio_blk_write,
	.flush = virtio_blk_flush,
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
	disk->queue->private = disk;
	device->config->get_config(device, 0, &capacity, sizeof(capacity));
	if (!capacity)
		goto failed;
	safe_strncpy(disk->name, "virtio-blk0", sizeof(disk->name));
	disk->block.name = disk->name;
	disk->block.sector_size = VIRTIO_BLK_SECTOR_SIZE;
	disk->block.sector_count = capacity;
	disk->block.operations = &virtio_blk_operations;
	disk->block.private = disk;
	disk->has_flush = virtio_has_feature(device, VIRTIO_BLK_F_FLUSH);
	if (block_device_register(&disk->block) < 0)
		goto failed;
	disk->name[10] = '0' + disk->block.id - 1;
	dev_set_drvdata(&device->device, disk);
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
	if (!wait_queue_empty(&disk->descriptor_wait))
		PANIC("remove busy virtio-blk");
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
