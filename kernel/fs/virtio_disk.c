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

#define R(disk, offset) \
	((volatile uint32 *)((disk)->base + (offset)))

struct virtio_request {
	volatile uint8 pending;
	int result;
};

struct virtio_disk {
	uint64 base;
	uint32 irq;
	int present;
	char name[16];
	struct block_device device;
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
};

static struct virtio_disk disks[VIRTIO_MMIO_SLOTS];

static int virtio_submit(struct virtio_disk *disk, uint32 type,
			 uint64 sector, void *buffer, uint32 length);

static int virtio_read(struct block_device *device, uint64 sector,
		       void *buffer, uint32 count)
{
	struct virtio_disk *disk = device->private;

	if (count > 0xffffffffU / VIRTIO_SECTOR_SIZE)
		return -1;
	return virtio_submit(disk, VIRTIO_BLK_T_IN, sector, buffer,
	                     count * VIRTIO_SECTOR_SIZE);
}

static int virtio_write(struct block_device *device, uint64 sector,
			const void *buffer, uint32 count)
{
	struct virtio_disk *disk = device->private;

	if (count > 0xffffffffU / VIRTIO_SECTOR_SIZE)
		return -1;
	return virtio_submit(disk, VIRTIO_BLK_T_OUT, sector, (void *)buffer,
	                     count * VIRTIO_SECTOR_SIZE);
}

static int virtio_flush(struct block_device *device)
{
	struct virtio_disk *disk = device->private;

	if (!disk->has_flush)
		return 0;
	return virtio_submit(disk, VIRTIO_BLK_T_FLUSH, 0, 0, 0);
}

static const struct block_device_operations virtio_operations = {
	.read = virtio_read,
	.write = virtio_write,
	.flush = virtio_flush,
};

static uint64 virtio_capacity(struct virtio_disk *disk)
{
	uint64 low = *R(disk, VIRTIO_MMIO_CONFIG);
	uint64 high = *R(disk, VIRTIO_MMIO_CONFIG + sizeof(uint32));

	return low | high << 32;
}

static int virtio_disk_init_one(struct virtio_disk *disk, int index)
{
	uint32 accepted, offered, status = 0;
	uint32 max;
	int i;

	disk->base = VIRTIO0 + index * PGSIZE;
	disk->irq = VIRTIO0_IRQ + index;
	if (*R(disk, VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
	    *R(disk, VIRTIO_MMIO_VERSION) != 2 ||
	    *R(disk, VIRTIO_MMIO_DEVICE_ID) != 2 ||
	    *R(disk, VIRTIO_MMIO_VENDOR_ID) != 0x554d4551)
		return 0;
	spinlock_init(&disk->lock, "virtio disk");

	*R(disk, VIRTIO_MMIO_STATUS) = status;
	status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
	*R(disk, VIRTIO_MMIO_STATUS) = status;
	status |= VIRTIO_CONFIG_S_DRIVER;
	*R(disk, VIRTIO_MMIO_STATUS) = status;

	*R(disk, VIRTIO_MMIO_DEVICE_FEATURES_SEL) = 0;
	offered = *R(disk, VIRTIO_MMIO_DEVICE_FEATURES);
	accepted = offered & (1U << VIRTIO_BLK_F_FLUSH);
	disk->has_flush = !!(accepted & (1U << VIRTIO_BLK_F_FLUSH));
	*R(disk, VIRTIO_MMIO_DRIVER_FEATURES_SEL) = 0;
	*R(disk, VIRTIO_MMIO_DRIVER_FEATURES) = accepted;

	status |= VIRTIO_CONFIG_S_FEATURES_OK;
	*R(disk, VIRTIO_MMIO_STATUS) = status;
	status = *R(disk, VIRTIO_MMIO_STATUS);
	if (!(status & VIRTIO_CONFIG_S_FEATURES_OK))
		PANIC("virtio disk FEATURES_OK unset");

	*R(disk, VIRTIO_MMIO_QUEUE_SEL) = 0;
	if (*R(disk, VIRTIO_MMIO_QUEUE_READY))
		PANIC("virtio disk queue already ready");
	max = *R(disk, VIRTIO_MMIO_QUEUE_NUM_MAX);
	if (!max || max < NUM)
		PANIC("virtio disk queue too short");

	disk->desc = palloc();
	disk->avail = palloc();
	disk->used = palloc();
	if (!disk->desc || !disk->avail || !disk->used)
		PANIC("virtio disk queue allocation");
	memset(disk->desc, 0, PGSIZE);
	memset(disk->avail, 0, PGSIZE);
	memset(disk->used, 0, PGSIZE);

	*R(disk, VIRTIO_MMIO_QUEUE_NUM) = NUM;
	*R(disk, VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk->desc;
	*R(disk, VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk->desc >> 32;
	*R(disk, VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk->avail;
	*R(disk, VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk->avail >> 32;
	*R(disk, VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk->used;
	*R(disk, VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk->used >> 32;
	*R(disk, VIRTIO_MMIO_QUEUE_READY) = 1;

	for (i = 0; i < NUM; i++)
		disk->free[i] = 1;
	status |= VIRTIO_CONFIG_S_DRIVER_OK;
	*R(disk, VIRTIO_MMIO_STATUS) = status;

	safe_strncpy(disk->name, "virtio0", sizeof(disk->name));
	disk->name[6] = '0' + index;
	disk->device.name = disk->name;
	disk->device.sector_size = VIRTIO_SECTOR_SIZE;
	disk->device.sector_count = virtio_capacity(disk);
	disk->device.operations = &virtio_operations;
	disk->device.private = disk;
	disk->present = 1;
	if (block_device_register(&disk->device))
		PANIC("virtio disk registration");
	return 1;
}

void virtio_disk_init(void)
{
	int found = 0, index;

	for (index = 0; index < VIRTIO_MMIO_SLOTS; index++)
		found += virtio_disk_init_one(&disks[index], index);
	if (!found)
		PANIC("could not find virtio disk");
}

static int alloc_desc(struct virtio_disk *disk)
{
	int i;

	for (i = 0; i < NUM; i++) {
		if (disk->free[i]) {
			disk->free[i] = 0;
			return i;
		}
	}
	return -1;
}

static void free_desc(struct virtio_disk *disk, int index)
{
	if (index < 0 || index >= NUM || disk->free[index])
		PANIC("virtio free descriptor");
	disk->desc[index].addr = 0;
	disk->desc[index].len = 0;
	disk->desc[index].flags = 0;
	disk->desc[index].next = 0;
	disk->free[index] = 1;
	wakeup(&disk->free[0]);
}

static void free_chain(struct virtio_disk *disk, int index)
{
	int flags, next;

	for (;;) {
		flags = disk->desc[index].flags;
		next = disk->desc[index].next;
		free_desc(disk, index);
		if (!(flags & VRING_DESC_F_NEXT))
			break;
		index = next;
	}
}

static int alloc_descs(struct virtio_disk *disk, int *indices, int count)
{
	int i, j;

	for (i = 0; i < count; i++) {
		indices[i] = alloc_desc(disk);
		if (indices[i] >= 0)
			continue;
		for (j = 0; j < i; j++)
			free_desc(disk, indices[j]);
		return -1;
	}
	return 0;
}

static int virtio_submit(struct virtio_disk *disk, uint32 type,
			 uint64 sector, void *buffer, uint32 length)
{
	struct virtio_blk_req *header;
	struct virtio_request request;
	int descriptor_count = length ? 3 : 2;
	int indices[3], status_index, result;

	request.pending = 1;
	request.result = -1;
	spinlock_acquire(&disk->lock);
	while (alloc_descs(disk, indices, descriptor_count))
		sleep(&disk->free[0], &disk->lock);

	header = &disk->requests[indices[0]];
	header->type = type;
	header->reserved = 0;
	header->sector = sector;
	disk->desc[indices[0]].addr = (uint64)header;
	disk->desc[indices[0]].len = sizeof(*header);
	disk->desc[indices[0]].flags = VRING_DESC_F_NEXT;
	disk->desc[indices[0]].next = indices[1];

	if (length) {
		disk->desc[indices[1]].addr = (uint64)buffer;
		disk->desc[indices[1]].len = length;
		disk->desc[indices[1]].flags = VRING_DESC_F_NEXT;
		if (type == VIRTIO_BLK_T_IN)
			disk->desc[indices[1]].flags |= VRING_DESC_F_WRITE;
		disk->desc[indices[1]].next = indices[2];
	}

	status_index = indices[descriptor_count - 1];
	disk->info[indices[0]].status = 0xff;
	disk->info[indices[0]].request = &request;
	disk->desc[status_index].addr =
		(uint64)&disk->info[indices[0]].status;
	disk->desc[status_index].len = 1;
	disk->desc[status_index].flags = VRING_DESC_F_WRITE;
	disk->desc[status_index].next = 0;

	disk->avail->ring[disk->avail->idx % NUM] = indices[0];
	__sync_synchronize();
	disk->avail->idx++;
	__sync_synchronize();
	*R(disk, VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

	while (request.pending)
		sleep(&request, &disk->lock);
	result = request.result;
	disk->info[indices[0]].request = 0;
	free_chain(disk, indices[0]);
	spinlock_release(&disk->lock);
	return result;
}

void virtio_disk_intr(int irq)
{
	struct virtio_disk *disk;
	struct virtio_request *request;
	int id;

	if (irq < VIRTIO0_IRQ ||
	    irq >= VIRTIO0_IRQ + VIRTIO_MMIO_SLOTS)
		PANIC("virtio disk interrupt");
	disk = &disks[irq - VIRTIO0_IRQ];
	if (!disk->present)
		PANIC("missing virtio disk interrupt");
	spinlock_acquire(&disk->lock);
	*R(disk, VIRTIO_MMIO_INTERRUPT_ACK) =
		*R(disk, VIRTIO_MMIO_INTERRUPT_STATUS) & 0x3;
	__sync_synchronize();

	while (disk->used_idx != disk->used->idx) {
		__sync_synchronize();
		id = disk->used->ring[disk->used_idx % NUM].id;
		request = disk->info[id].request;
		if (!request)
			PANIC("virtio disk completion");
		request->result = disk->info[id].status ? -1 : 0;
		request->pending = 0;
		wakeup(request);
		disk->used_idx++;
	}
	spinlock_release(&disk->lock);
}
