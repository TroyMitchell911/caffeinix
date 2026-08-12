#include <device.h>
#include <driver.h>
#include <process.h>

static char zero_page[PGSIZE];

int vfs_device_is_terminal(uint64 device)
{
	return device == DEVICE_CONSOLE || device == DEVICE_TTY ||
	       device == VFS_MAKE_DEVICE(CONSOLE, 0);
}

static int vfs_device_driver(uint64 device)
{
	uint32 major = VFS_DEVICE_MAJOR(device);

	if (vfs_device_is_terminal(device))
		return CONSOLE;
	return major < NDEV ? major : -1;
}

static int64 vfs_device_read(struct vfs_file *file, int user_destination,
			     uint64 destination, uint64 count,
			     uint64 *position)
{
	uint64 device = file->path.dentry->inode->device;
	uint64 total = 0;
	int driver;

	(void)position;
	if (device == DEVICE_NULL)
		return 0;
	if (device == DEVICE_ZERO) {
		while (total < count) {
			uint32 chunk = count - total > PGSIZE ?
				PGSIZE : count - total;

			if (either_copyout(user_destination,
			                   destination + total, zero_page,
			                   chunk) < 0)
				return total ? total : VFS_ERR_IO;
			total += chunk;
		}
		return total;
	}
	driver = vfs_device_driver(device);
	if (!user_destination || driver < 0 || !dev[driver].read ||
	    count > 0x7fffffff)
		return VFS_ERR_NODEV;
	return dev[driver].read(destination, count);
}

static int64 vfs_device_write(struct vfs_file *file, int user_source,
			      uint64 source, uint64 count,
			      uint64 *position)
{
	uint64 device = file->path.dentry->inode->device;
	int driver;

	(void)source;
	(void)position;
	if (device == DEVICE_NULL || device == DEVICE_ZERO)
		return count;
	driver = vfs_device_driver(device);
	if (!user_source || driver < 0 || !dev[driver].write ||
	    count > 0x7fffffff)
		return VFS_ERR_NODEV;
	return dev[driver].write(source, count);
}

static int vfs_device_sync(struct vfs_file *file)
{
	(void)file;
	return VFS_OK;
}

const struct vfs_file_operations vfs_device_operations = {
	.read = vfs_device_read,
	.write = vfs_device_write,
	.fsync = vfs_device_sync,
};
