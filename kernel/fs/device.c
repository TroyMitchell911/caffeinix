#include <char_device.h>
#include <device.h>

static struct char_device *file_char_device(struct vfs_file *file)
{
	return file ? file->private : 0;
}

static int vfs_device_open(struct vfs_inode *inode, struct vfs_file *file)
{
	struct char_device *device = char_device_lookup(inode->device);

	if (!device)
		return VFS_ERR_NODEV;
	file->private = device;
	if (device->flags & CHAR_DEVICE_CAN_PREAD)
		file->capabilities |= VFS_FILE_CAN_PREAD;
	if (!device->operations->open)
		return VFS_OK;
	return device->operations->open(device, file);
}

static void vfs_device_release(struct vfs_file *file)
{
	struct char_device *device = file_char_device(file);

	if (device && device->operations->release)
		device->operations->release(device, file);
}

static int64 vfs_device_read(struct vfs_file *file, int user_destination,
			     uint64 destination, uint64 count,
			     uint64 *position)
{
	struct char_device *device = file_char_device(file);

	(void)position;
	if (!device || !device->operations->read)
		return VFS_ERR_NODEV;
	return device->operations->read(device, file, user_destination,
	                                destination, count);
}

static int64 vfs_device_write(struct vfs_file *file, int user_source,
			      uint64 source, uint64 count,
			      uint64 *position)
{
	struct char_device *device = file_char_device(file);

	(void)position;
	if (!device || !device->operations->write)
		return VFS_ERR_NODEV;
	return device->operations->write(device, file, user_source, source,
	                                 count);
}

static int64 vfs_device_ioctl(struct vfs_file *file, uint64 request,
			      uint64 argument)
{
	struct char_device *device = file_char_device(file);

	if (!device || !device->operations->ioctl)
		return VFS_ERR_NOTTY;
	return device->operations->ioctl(device, file, request, argument);
}

static int vfs_device_sync(struct vfs_file *file)
{
	struct char_device *device = file_char_device(file);

	if (!device)
		return VFS_ERR_NODEV;
	if (!device->operations->fsync)
		return VFS_OK;
	return device->operations->fsync(device, file);
}

static uint32 vfs_device_poll(struct vfs_file *file, uint32 events)
{
	struct char_device *device = file_char_device(file);
	uint32 ready = 0;

	if (!device)
		return VFS_POLL_NVAL;
	if (device->operations->poll)
		return device->operations->poll(device, file, events);
	if ((events & VFS_POLL_IN) && device->operations->read)
		ready |= VFS_POLL_IN;
	if ((events & VFS_POLL_OUT) && device->operations->write)
		ready |= VFS_POLL_OUT;
	return ready;
}

const struct vfs_file_operations vfs_device_operations = {
	.open = vfs_device_open,
	.release = vfs_device_release,
	.read = vfs_device_read,
	.write = vfs_device_write,
	.ioctl = vfs_device_ioctl,
	.fsync = vfs_device_sync,
	.poll = vfs_device_poll,
};
