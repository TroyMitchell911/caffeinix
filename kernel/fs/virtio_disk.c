#include <block_device.h>
#include <debug.h>
#include <mem_layout.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <spinlock.h>
#include <virtio_disk.h>

#define NUM VIRTIO_DES_NUM
#define VIRTIO_SECTOR_SIZE 512

#define R(offset) ((volatile uint32 *)(VIRTIO0 + (offset)))

struct virtio_request {
	volatile uint8 pending;
	int result;
};

static struct {
	struct virtq_desc *desc;
	struct virtq_avail *avail;
	struct virtq_used *used;
	uint8 free[VIRTIO_DES_NUM];
	uint16 used_idx;
	uint8 has_flush;
	struct {
		struct virtio_request *request;
		uint8 status;
	} info[VIRTIO_DES_NUM];
	struct virtio_blk_req requests[VIRTIO_DES_NUM];
	struct spinlock lock;
} disk;

static int virtio_submit(uint32 type, uint64 sector, void *buffer,
			 uint32 length);

static int virtio_read(struct block_device *device, uint64 sector,
		       void *buffer, uint32 count)
{
	(void)device;
	if (count > 0xffffffffU / VIRTIO_SECTOR_SIZE)
		return -1;
	return virtio_submit(VIRTIO_BLK_T_IN, sector, buffer,
			     count * VIRTIO_SECTOR_SIZE);
}

static int virtio_write(struct block_device *device, uint64 sector,
			const void *buffer, uint32 count)
{
	(void)device;
	if (count > 0xffffffffU / VIRTIO_SECTOR_SIZE)
		return -1;
	return virtio_submit(VIRTIO_BLK_T_OUT, sector, (void *)buffer,
			     count * VIRTIO_SECTOR_SIZE);
}

static int virtio_flush(struct block_device *device)
{
	(void)device;
	if (!disk.has_flush)
		return 0;
	return virtio_submit(VIRTIO_BLK_T_FLUSH, 0, 0, 0);
}

static const struct block_device_operations virtio_operations = {
	.read = virtio_read,
	.write = virtio_write,
	.flush = virtio_flush,
};

static struct block_device virtio_device = {
	.name = "virtio0",
	.sector_size = VIRTIO_SECTOR_SIZE,
	.operations = &virtio_operations,
	.private = &disk,
};

static uint64 virtio_capacity(void)
{
	uint64 low = *R(VIRTIO_MMIO_CONFIG);
	uint64 high = *R(VIRTIO_MMIO_CONFIG + sizeof(uint32));

	return low | high << 32;
}

void virtio_disk_init(void)
{
	uint32 accepted, offered, status = 0;
	uint32 max;
	int i;

	spinlock_init(&disk.lock, "virtio disk");
	if (*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
	    *R(VIRTIO_MMIO_VERSION) != 2 ||
	    *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
	    *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551)
		PANIC("could not find virtio disk");

	*R(VIRTIO_MMIO_STATUS) = status;
	status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
	*R(VIRTIO_MMIO_STATUS) = status;
	status |= VIRTIO_CONFIG_S_DRIVER;
	*R(VIRTIO_MMIO_STATUS) = status;

	*R(VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 0;
	offered = *R(VIRTIO_MMIO_DEVICE_FEATURES);
	accepted = offered & (1U << VIRTIO_BLK_F_FLUSH);
	disk.has_flush = !!(accepted & (1U << VIRTIO_BLK_F_FLUSH));
	*R(VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 0;
	*R(VIRTIO_MMIO_DRIVER_FEATURES) = accepted;

	status |= VIRTIO_CONFIG_S_FEATURES_OK;
	*R(VIRTIO_MMIO_STATUS) = status;
	status = *R(VIRTIO_MMIO_STATUS);
	if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
		PANIC("virtio disk FEATURES_OK unset");

	*R(VIRTIO_MMIO_QUEUE_SEL) = 0;
	if (*R(VIRTIO_MMIO_QUEUE_READY))
		PANIC("virtio disk queue already ready");
	max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (!max || max < NUM)
		PANIC("virtio disk queue too short");

	disk.desc = palloc();
	disk.avail = palloc();
	disk.used = palloc();
	if (!disk.desc || !disk.avail || !disk.used)
		PANIC("virtio disk queue allocation");
	memset(disk.desc, 0, PGSIZE);
	memset(disk.avail, 0, PGSIZE);
	memset(disk.used, 0, PGSIZE);

	*R(VIRTIO_MMIO_QUEUE_NUM) = NUM;
	*R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
	*R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
	*R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
	*R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
	*R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
	*R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;
	*R(VIRTIO_MMIO_QUEUE_READY) = 1;

	for (i = 0; i < NUM; i++)
		disk.free[i] = 1;
	status |= VIRTIO_CONFIG_S_DRIVER_OK;
	*R(VIRTIO_MMIO_STATUS) = status;

	virtio_device.sector_count = virtio_capacity();
	if (block_device_register(&virtio_device))
		PANIC("virtio disk registration");
}

static int alloc_desc(void)
{
	int i;

	for (i = 0; i < NUM; i++) {
		if (disk.free[i]) {
			disk.free[i] = 0;
			return i;
		}
	}
	return -1;
}

static void free_desc(int index)
{
	if (index < 0 || index >= NUM || disk.free[index])
		PANIC("virtio free descriptor");
	disk.desc[index].addr = 0;
	disk.desc[index].len = 0;
	disk.desc[index].flags = 0;
	disk.desc[index].next = 0;
	disk.free[index] = 1;
	wakeup(&disk.free[0]);
}

static void free_chain(int index)
{
	int flags, next;

	for (;;) {
		flags = disk.desc[index].flags;
		next = disk.desc[index].next;
		free_desc(index);
		if (!(flags & VRING_DESC_F_NEXT))
			break;
		index = next;
	}
}

static int alloc_descs(int *indices, int count)
{
	int i, j;

	for (i = 0; i < count; i++) {
		indices[i] = alloc_desc();
		if (indices[i] >= 0)
			continue;
		for (j = 0; j < i; j++)
			free_desc(indices[j]);
		return -1;
	}
	return 0;
}

static int virtio_submit(uint32 type, uint64 sector, void *buffer,
			 uint32 length)
{
	struct virtio_blk_req *header;
	struct virtio_request request;
	int descriptor_count = length ? 3 : 2;
	int indices[3], status_index, result;

	request.pending = 1;
	request.result = -1;
	spinlock_acquire(&disk.lock);
	while (alloc_descs(indices, descriptor_count))
		sleep(&disk.free[0], &disk.lock);

	header = &disk.requests[indices[0]];
	header->type = type;
	header->reserved = 0;
	header->sector = sector;
	disk.desc[indices[0]].addr = (uint64)header;
	disk.desc[indices[0]].len = sizeof(*header);
	disk.desc[indices[0]].flags = VRING_DESC_F_NEXT;
	disk.desc[indices[0]].next = indices[1];

	if (length) {
		disk.desc[indices[1]].addr = (uint64)buffer;
		disk.desc[indices[1]].len = length;
		disk.desc[indices[1]].flags = VRING_DESC_F_NEXT;
		if (type == VIRTIO_BLK_T_IN)
			disk.desc[indices[1]].flags |= VRING_DESC_F_WRITE;
		disk.desc[indices[1]].next = indices[2];
	}

	status_index = indices[descriptor_count - 1];
	disk.info[indices[0]].status = 0xff;
	disk.info[indices[0]].request = &request;
	disk.desc[status_index].addr =
		(uint64)&disk.info[indices[0]].status;
	disk.desc[status_index].len = 1;
	disk.desc[status_index].flags = VRING_DESC_F_WRITE;
	disk.desc[status_index].next = 0;

	disk.avail->ring[disk.avail->idx % NUM] = indices[0];
	__sync_synchronize();
	disk.avail->idx++;
	__sync_synchronize();
	*R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

	while (request.pending)
		sleep(&request, &disk.lock);
	result = request.result;
	disk.info[indices[0]].request = 0;
	free_chain(indices[0]);
	spinlock_release(&disk.lock);
	return result;
}

void virtio_disk_intr(void)
{
	struct virtio_request *request;
	int id;

	spinlock_acquire(&disk.lock);
	*R(VIRTIO_MMIO_INTERRUPT_ACK) =
		*R(VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
	__sync_synchronize();

	while (disk.used_idx != disk.used->idx) {
		__sync_synchronize();
		id = disk.used->ring[disk.used_idx % NUM].id;
		request = disk.info[id].request;
		if (!request)
			PANIC("virtio disk completion");
		request->result = disk.info[id].status ? -1 : 0;
		request->pending = 0;
		wakeup(request);
		disk.used_idx++;
	}
	spinlock_release(&disk.lock);
}
