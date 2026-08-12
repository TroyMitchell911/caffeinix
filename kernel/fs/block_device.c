#include <block_device.h>
#include <spinlock.h>

static struct {
	struct spinlock lock;
	struct block_device *devices[BLOCK_DEVICE_MAX];
} block_devices;

void block_device_init(void)
{
	spinlock_init(&block_devices.lock, "block devices");
}

int block_device_register(struct block_device *device)
{
	uint32 id;

	if (!device || !device->name || !device->operations ||
	    !device->operations->read || !device->operations->write ||
	    !device->sector_size || !device->sector_count)
		return -1;

	spinlock_acquire(&block_devices.lock);
	for (id = 1; id < BLOCK_DEVICE_MAX; id++) {
		if (!block_devices.devices[id]) {
			device->id = id;
			block_devices.devices[id] = device;
			spinlock_release(&block_devices.lock);
			return 0;
		}
	}
	spinlock_release(&block_devices.lock);
	return -1;
}

struct block_device *block_device_get(uint32 id)
{
	struct block_device *device = 0;

	if (id >= BLOCK_DEVICE_MAX)
		return 0;
	spinlock_acquire(&block_devices.lock);
	device = block_devices.devices[id];
	spinlock_release(&block_devices.lock);
	return device;
}

static int block_device_range_valid(struct block_device *device,
				    uint64 sector, uint32 count)
{
	if (!device || !count || sector >= device->sector_count)
		return 0;
	return count <= device->sector_count - sector;
}

int block_device_read(struct block_device *device, uint64 sector,
		      void *buffer, uint32 count)
{
	if (!buffer || !block_device_range_valid(device, sector, count))
		return -1;
	return device->operations->read(device, sector, buffer, count);
}

int block_device_write(struct block_device *device, uint64 sector,
		       const void *buffer, uint32 count)
{
	if (!buffer || !block_device_range_valid(device, sector, count))
		return -1;
	return device->operations->write(device, sector, buffer, count);
}

int block_device_flush(struct block_device *device)
{
	if (!device)
		return -1;
	if (!device->operations->flush)
		return 0;
	return device->operations->flush(device);
}
