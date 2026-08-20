#include <block_device.h>
#include <linux_uapi.h>
#include <palloc.h>
#include <process.h>
#include <vfs.h>

struct block_device_file {
	struct block_device *device;
	struct sleeplock buffer_lock;
	void *buffer;
};

static int block_file_open(struct vfs_inode *inode, struct vfs_file *file)
{
	struct block_device_file *open;
	struct block_device *device;
	uint32 minor = VFS_DEVICE_MINOR(inode->device);
	uint32 id;

	if (VFS_DEVICE_MAJOR(inode->device) != BLOCK_DEVICE_NODE_MAJOR ||
	    minor >= BLOCK_DEVICE_MAX - 1)
		return VFS_ERR_NODEV;
	id = minor + 1;
	device = block_device_open(id);
	if (!device)
		return VFS_ERR_NODEV;
	if (device->sector_size > PGSIZE) {
		block_device_close(device);
		return VFS_ERR_NODEV;
	}
	open = malloc(sizeof(*open));
	if (!open) {
		block_device_close(device);
		return VFS_ERR_NOMEM;
	}
	open->buffer = palloc();
	if (!open->buffer) {
		free(open);
		block_device_close(device);
		return VFS_ERR_NOMEM;
	}
	open->device = device;
	sleeplock_init(&open->buffer_lock, "block file buffer");
	file->private = open;
	file->capabilities |= VFS_FILE_CAN_PREAD;
	return VFS_OK;
}

static void block_file_release(struct vfs_file *file)
{
	struct block_device_file *open = file->private;

	if (!open)
		return;
	pfree(open->buffer);
	block_device_close(open->device);
	free(open);
	file->private = 0;
}

static int64 block_file_read_locked(struct vfs_file *file,
				    int user_destination,
				    uint64 destination, uint64 count,
				    uint64 *position)
{
	struct block_device_file *open = file->private;
	struct block_device *device;
	uint64 capacity, sector, total = 0;
	uint32 chunk, offset, sectors;

	if (!open || !(device = open->device))
		return VFS_ERR_NODEV;
	capacity = device->sector_count * device->sector_size;
	if (*position >= capacity)
		return 0;
	if (count > capacity - *position)
		count = capacity - *position;
	while (total < count) {
		sector = *position / device->sector_size;
		offset = *position % device->sector_size;
		if (!offset && count - total >= device->sector_size) {
			sectors = (count - total) / device->sector_size;
			if (sectors > PGSIZE / device->sector_size)
				sectors = PGSIZE / device->sector_size;
			chunk = sectors * device->sector_size;
		} else {
			sectors = 1;
			chunk = device->sector_size - offset;
			if (chunk > count - total)
				chunk = count - total;
		}
		if (block_device_read(device, sector, open->buffer, sectors))
			return total ? total : VFS_ERR_IO;
		if (either_copyout(user_destination, destination + total,
		                   (char *)open->buffer + offset, chunk) < 0)
			return total ? total : VFS_ERR_FAULT;
		*position += chunk;
		total += chunk;
	}
	return total;
}

static int64 block_file_read(struct vfs_file *file, int user_destination,
			     uint64 destination, uint64 count,
			     uint64 *position)
{
	struct block_device_file *open = file->private;
	int64 result;

	if (!open || !open->device)
		return VFS_ERR_NODEV;
	sleeplock_acquire(&open->buffer_lock);
	result = block_file_read_locked(file, user_destination, destination,
					count, position);
	sleeplock_release(&open->buffer_lock);
	return result;
}

static int64 block_file_write_locked(struct vfs_file *file, int user_source,
				     uint64 source, uint64 count,
				     uint64 *position)
{
	struct block_device_file *open = file->private;
	struct block_device *device;
	uint64 capacity, sector, total = 0;
	uint32 chunk, offset, sectors;

	if (!open || !(device = open->device))
		return VFS_ERR_NODEV;
	if (!count)
		return 0;
	capacity = device->sector_count * device->sector_size;
	if (*position >= capacity)
		return VFS_ERR_NOSPC;
	if (count > capacity - *position)
		count = capacity - *position;
	while (total < count) {
		sector = *position / device->sector_size;
		offset = *position % device->sector_size;
		if (!offset && count - total >= device->sector_size) {
			sectors = (count - total) / device->sector_size;
			if (sectors > PGSIZE / device->sector_size)
				sectors = PGSIZE / device->sector_size;
			chunk = sectors * device->sector_size;
		} else {
			sectors = 1;
			chunk = device->sector_size - offset;
			if (chunk > count - total)
				chunk = count - total;
			if (block_device_read(device, sector, open->buffer, 1))
				return total ? total : VFS_ERR_IO;
		}
		if (either_copyin((char *)open->buffer + offset, user_source,
		                  source + total, chunk) < 0)
			return total ? total : VFS_ERR_FAULT;
		if (block_device_write(device, sector, open->buffer, sectors))
			return total ? total : VFS_ERR_IO;
		*position += chunk;
		total += chunk;
	}
	return total;
}

static int64 block_file_write(struct vfs_file *file, int user_source,
			      uint64 source, uint64 count, uint64 *position)
{
	struct block_device_file *open = file->private;
	int64 result;

	if (!open || !open->device)
		return VFS_ERR_NODEV;
	sleeplock_acquire(&open->buffer_lock);
	sleeplock_acquire(&open->device->raw_write_lock);
	result = block_file_write_locked(file, user_source, source, count,
					 position);
	sleeplock_release(&open->device->raw_write_lock);
	sleeplock_release(&open->buffer_lock);
	return result;
}

static int64 block_file_ioctl(struct vfs_file *file, uint64 request,
			      uint64 argument)
{
	struct block_device_file *open = file->private;
	uint64 size;
	uint32 sector_size;

	if (!open || !open->device)
		return VFS_ERR_NODEV;
	if (request == LINUX_BLKSSZGET) {
		sector_size = open->device->sector_size;
		return either_copyout(1, argument, &sector_size,
				      sizeof(sector_size)) < 0 ?
			VFS_ERR_FAULT : VFS_OK;
	}
	if (request == LINUX_BLKGETSIZE64) {
		size = open->device->sector_count * open->device->sector_size;
		return either_copyout(1, argument, &size, sizeof(size)) < 0 ?
			VFS_ERR_FAULT : VFS_OK;
	}
	return VFS_ERR_NOTTY;
}

static int block_file_sync(struct vfs_file *file)
{
	struct block_device_file *open = file->private;

	return !open || !open->device || block_device_flush(open->device) ?
		VFS_ERR_IO : VFS_OK;
}

const struct vfs_file_operations vfs_block_device_operations = {
	.flags = VFS_FILE_CAN_PREAD,
	.open = block_file_open,
	.release = block_file_release,
	.read = block_file_read,
	.write = block_file_write,
	.ioctl = block_file_ioctl,
	.fsync = block_file_sync,
};
