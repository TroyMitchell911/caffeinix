#include <debug.h>
#include <fatfs.h>
#include <ff.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <riscv.h>
#include <sleeplock.h>
#include <vfs.h>

#define FATFS_ROOT "0:/"
#define FATFS_PATH_MAX (VFS_PATH_MAX + 2)

struct fatfs_port {
	int active;
	struct block_device *device;
	FATFS filesystem;
	struct sleeplock lock;
};

struct fatfs_inode {
	FILINFO info;
	char path[FATFS_PATH_MAX];
};

struct fatfs_file {
	FIL file;
	void *buffer;
};

struct fatfs_directory {
	DIR directory;
	FILINFO info;
};

static struct fatfs_port fatfs_port;

static const struct vfs_inode_operations fatfs_inode_operations;
static const struct vfs_file_operations fatfs_file_operations;
static const struct vfs_file_operations fatfs_directory_operations;

static int fatfs_result(FRESULT result)
{
	switch (result) {
	case FR_OK:
		return VFS_OK;
	case FR_NO_FILE:
	case FR_NO_PATH:
		return VFS_ERR_NOENT;
	case FR_INVALID_NAME:
	case FR_INVALID_PARAMETER:
		return VFS_ERR_INVAL;
	case FR_DENIED:
	case FR_WRITE_PROTECTED:
		return VFS_ERR_PERM;
	case FR_EXIST:
		return VFS_ERR_EXIST;
	case FR_NOT_READY:
	case FR_INVALID_DRIVE:
	case FR_NOT_ENABLED:
	case FR_NO_FILESYSTEM:
		return VFS_ERR_NODEV;
	case FR_LOCKED:
		return VFS_ERR_BUSY;
	case FR_NOT_ENOUGH_CORE:
		return VFS_ERR_NOMEM;
	case FR_TOO_MANY_OPEN_FILES:
		return VFS_ERR_MFILE;
	case FR_DISK_ERR:
	case FR_INT_ERR:
	case FR_INVALID_OBJECT:
	case FR_TIMEOUT:
	case FR_MKFS_ABORTED:
	default:
		return VFS_ERR_IO;
	}
}

static uint64 fatfs_inode_number(const char *path)
{
	uint64 hash = 1469598103934665603ULL;
	uint8 character;

	while ((character = *path++)) {
		if (character >= 'A' && character <= 'Z')
			character += 'a' - 'A';
		hash ^= character;
		hash *= 1099511628211ULL;
	}
	return hash ? hash : 1;
}

static int fatfs_join(char *path, const char *directory, const char *name)
{
	uint32 directory_length = strlen(directory);
	uint32 name_length = strlen(name);
	uint32 position = 0;

	if (!name_length)
		return VFS_ERR_INVAL;
	if (name_length > VFS_NAME_MAX)
		return VFS_ERR_NAMETOOLONG;
	if (directory_length + name_length + 2 > FATFS_PATH_MAX)
		return VFS_ERR_NAMETOOLONG;
	while (*directory)
		path[position++] = *directory++;
	if (path[position - 1] != '/')
		path[position++] = '/';
	while (*name)
		path[position++] = *name++;
	path[position] = 0;
	return VFS_OK;
}

static int fatfs_path_is_root(const char *path)
{
	return !strcmp(path, FATFS_ROOT);
}

static FRESULT fatfs_stat_locked(const char *path, FILINFO *info)
{
	if (fatfs_path_is_root(path)) {
		memset(info, 0, sizeof(*info));
		info->fattrib = AM_DIR;
		return FR_OK;
	}
	return f_stat(path, info);
}

static void fatfs_refresh_locked(struct vfs_inode *inode)
{
	struct fatfs_inode *private = inode->private;

	inode->number = fatfs_inode_number(private->path);
	inode->type = private->info.fattrib & AM_DIR ?
		VFS_INODE_DIRECTORY : VFS_INODE_REGULAR;
	inode->mode = inode->type == VFS_INODE_DIRECTORY ? 0777 : 0666;
	if (private->info.fattrib & AM_RDO)
		inode->mode &= ~0222U;
	inode->nlink = inode->type == VFS_INODE_DIRECTORY ? 2 : 1;
	inode->size = private->info.fsize;
	inode->blocks = (private->info.fsize + 511) / 512;
	inode->operations = &fatfs_inode_operations;
	inode->file_operations = inode->type == VFS_INODE_DIRECTORY ?
		&fatfs_directory_operations : &fatfs_file_operations;
}

static struct vfs_inode *fatfs_wrap_locked(
	struct vfs_super_block *superblock, const char *path)
{
	struct fatfs_inode *private;
	struct vfs_inode *inode;
	FRESULT result;

	if (strlen(path) >= FATFS_PATH_MAX)
		return 0;
	private = malloc(sizeof(*private));
	if (!private)
		return 0;
	memset(private, 0, sizeof(*private));
	safe_strncpy(private->path, path, sizeof(private->path));
	result = fatfs_stat_locked(path, &private->info);
	if (result != FR_OK) {
		free(private);
		return 0;
	}
	inode = vfs_inode_alloc(superblock);
	if (!inode) {
		free(private);
		return 0;
	}
	inode->private = private;
	fatfs_refresh_locked(inode);
	return inode;
}

static void fatfs_put_inode(struct vfs_inode *inode)
{
	free(inode->private);
}

static int fatfs_sync(struct vfs_super_block *superblock)
{
	return block_device_flush(superblock->device) ?
		VFS_ERR_IO : VFS_OK;
}

static const struct vfs_super_operations fatfs_super_operations = {
	.put_inode = fatfs_put_inode,
	.sync = fatfs_sync,
};

static int fatfs_getattr(struct vfs_inode *inode, struct vfs_stat *stat)
{
	struct fatfs_inode *private = inode->private;
	FRESULT result;

	sleeplock_acquire(&fatfs_port.lock);
	result = fatfs_stat_locked(private->path, &private->info);
	if (result == FR_OK)
		fatfs_refresh_locked(inode);
	sleeplock_release(&fatfs_port.lock);
	return result == FR_OK ? vfs_inode_stat_default(inode, stat) :
		fatfs_result(result);
}

static int fatfs_lookup(struct vfs_inode *directory, const char *name,
			struct vfs_inode **result)
{
	struct fatfs_inode *parent = directory->private;
	char *path = palloc();
	FILINFO info;
	FRESULT operation;
	int status;

	if (!path)
		return VFS_ERR_NOMEM;
	status = fatfs_join(path, parent->path, name);
	if (status == VFS_OK) {
		sleeplock_acquire(&fatfs_port.lock);
		operation = fatfs_stat_locked(path, &info);
		*result = operation == FR_OK ?
			fatfs_wrap_locked(directory->superblock, path) : 0;
		sleeplock_release(&fatfs_port.lock);
		status = operation != FR_OK ? fatfs_result(operation) :
			*result ? VFS_OK : VFS_ERR_NOMEM;
	}
	pfree(path);
	return status;
}

static int fatfs_create(struct vfs_inode *directory, const char *name,
			uint32 mode, struct vfs_inode **result)
{
	struct fatfs_inode *parent = directory->private;
	char *path = palloc();
	FIL file;
	FRESULT operation;
	int status;

	(void)mode;
	if (!path)
		return VFS_ERR_NOMEM;
	status = fatfs_join(path, parent->path, name);
	if (status < 0)
		goto out;
	sleeplock_acquire(&fatfs_port.lock);
	operation = f_open(&file, path,
	                   FA_CREATE_NEW | FA_READ | FA_WRITE);
	if (operation == FR_OK)
		operation = f_close(&file);
	if (operation == FR_OK)
		*result = fatfs_wrap_locked(directory->superblock, path);
	else
		*result = 0;
	if (operation == FR_OK && !*result) {
		f_unlink(path);
		status = VFS_ERR_NOMEM;
	} else {
		status = fatfs_result(operation);
	}
	sleeplock_release(&fatfs_port.lock);
out:
	pfree(path);
	return status;
}

static int fatfs_mkdir(struct vfs_inode *directory, const char *name,
		       uint32 mode, struct vfs_inode **result)
{
	struct fatfs_inode *parent = directory->private;
	char *path = palloc();
	FRESULT operation;
	int status;

	(void)mode;
	if (!path)
		return VFS_ERR_NOMEM;
	status = fatfs_join(path, parent->path, name);
	if (status < 0)
		goto out;
	sleeplock_acquire(&fatfs_port.lock);
	operation = f_mkdir(path);
	if (operation == FR_OK)
		*result = fatfs_wrap_locked(directory->superblock, path);
	else
		*result = 0;
	if (operation == FR_OK && !*result) {
		f_unlink(path);
		status = VFS_ERR_NOMEM;
	} else {
		status = fatfs_result(operation);
	}
	sleeplock_release(&fatfs_port.lock);
out:
	pfree(path);
	return status;
}

static int fatfs_remove(struct vfs_inode *directory, const char *name,
			int remove_directory)
{
	struct fatfs_inode *parent = directory->private;
	char *path = palloc();
	FILINFO info;
	FRESULT operation;
	int status;

	if (!path)
		return VFS_ERR_NOMEM;
	status = fatfs_join(path, parent->path, name);
	if (status < 0)
		goto out;
	sleeplock_acquire(&fatfs_port.lock);
	operation = f_stat(path, &info);
	if (operation == FR_OK && remove_directory !=
	    !!(info.fattrib & AM_DIR)) {
		status = remove_directory ? VFS_ERR_NOTDIR : VFS_ERR_ISDIR;
	} else if (operation == FR_OK) {
		status = fatfs_result(f_unlink(path));
	} else {
		status = fatfs_result(operation);
	}
	sleeplock_release(&fatfs_port.lock);
out:
	pfree(path);
	return status;
}

static int fatfs_unlink(struct vfs_inode *directory, const char *name)
{
	return fatfs_remove(directory, name, 0);
}

static int fatfs_rmdir(struct vfs_inode *directory, const char *name)
{
	return fatfs_remove(directory, name, 1);
}

static int fatfs_rename(struct vfs_inode *old_directory,
			const char *old_name,
			struct vfs_inode *new_directory,
			const char *new_name, uint32 flags)
{
	struct fatfs_inode *old_parent = old_directory->private;
	struct fatfs_inode *new_parent = new_directory->private;
	char *old_path = palloc();
	char *new_path;
	FILINFO old_info, new_info;
	FRESULT operation;
	int old_directory_type, new_directory_type;
	int status;

	if (!old_path)
		return VFS_ERR_NOMEM;
	new_path = old_path + FATFS_PATH_MAX;
	status = fatfs_join(old_path, old_parent->path, old_name);
	if (status < 0)
		goto out;
	status = fatfs_join(new_path, new_parent->path, new_name);
	if (status < 0)
		goto out;
	sleeplock_acquire(&fatfs_port.lock);
	operation = f_stat(old_path, &old_info);
	if (operation != FR_OK) {
		status = fatfs_result(operation);
		goto unlock;
	}
	operation = f_stat(new_path, &new_info);
	if (operation == FR_OK) {
		if (flags & VFS_RENAME_NOREPLACE) {
			status = VFS_ERR_EXIST;
			goto unlock;
		}
		old_directory_type = !!(old_info.fattrib & AM_DIR);
		new_directory_type = !!(new_info.fattrib & AM_DIR);
		if (old_directory_type != new_directory_type) {
			status = old_directory_type ?
				VFS_ERR_NOTDIR : VFS_ERR_ISDIR;
			goto unlock;
		}
		operation = f_unlink(new_path);
		if (operation != FR_OK) {
			status = fatfs_result(operation);
			goto unlock;
		}
	} else if (operation != FR_NO_FILE && operation != FR_NO_PATH) {
		status = fatfs_result(operation);
		goto unlock;
	}
	status = fatfs_result(f_rename(old_path, new_path));
unlock:
	sleeplock_release(&fatfs_port.lock);
out:
	pfree(old_path);
	return status;
}

static int fatfs_truncate(struct vfs_inode *inode, uint64 size)
{
	struct fatfs_inode *private = inode->private;
	FIL file;
	FRESULT operation;

	if (size > 0xffffffffULL)
		return VFS_ERR_NOSPC;
	memset(&file, 0, sizeof(file));
	sleeplock_acquire(&fatfs_port.lock);
	operation = f_open(&file, private->path, FA_WRITE);
	if (operation == FR_OK)
		operation = f_lseek(&file, size);
	if (operation == FR_OK)
		operation = f_truncate(&file);
	if (file.obj.fs)
		f_close(&file);
	if (operation == FR_OK) {
		private->info.fsize = size;
		fatfs_refresh_locked(inode);
	}
	sleeplock_release(&fatfs_port.lock);
	return fatfs_result(operation);
}

static const struct vfs_inode_operations fatfs_inode_operations = {
	.lookup = fatfs_lookup,
	.create = fatfs_create,
	.mkdir = fatfs_mkdir,
	.unlink = fatfs_unlink,
	.rmdir = fatfs_rmdir,
	.rename = fatfs_rename,
	.truncate = fatfs_truncate,
	.getattr = fatfs_getattr,
};

static int fatfs_file_open(struct vfs_inode *inode, struct vfs_file *file)
{
	struct fatfs_inode *private = inode->private;
	struct fatfs_file *handle;
	BYTE mode = 0;
	FRESULT result;

	handle = malloc(sizeof(*handle));
	if (!handle)
		return VFS_ERR_NOMEM;
	memset(handle, 0, sizeof(*handle));
	handle->buffer = palloc();
	if (!handle->buffer) {
		free(handle);
		return VFS_ERR_NOMEM;
	}
	if (file->flags & VFS_OPEN_READ)
		mode |= FA_READ;
	if (file->flags & VFS_OPEN_WRITE)
		mode |= FA_WRITE;
	sleeplock_acquire(&fatfs_port.lock);
	result = f_open(&handle->file, private->path, mode);
	sleeplock_release(&fatfs_port.lock);
	if (result != FR_OK) {
		pfree(handle->buffer);
		free(handle);
		return fatfs_result(result);
	}
	file->private = handle;
	return VFS_OK;
}

static void fatfs_file_release(struct vfs_file *file)
{
	struct fatfs_file *handle = file->private;

	if (!handle)
		return;
	sleeplock_acquire(&fatfs_port.lock);
	f_close(&handle->file);
	sleeplock_release(&fatfs_port.lock);
	pfree(handle->buffer);
	free(handle);
}

static int64 fatfs_read(struct vfs_file *file, int user_destination,
			uint64 destination, uint64 count, uint64 *position)
{
	struct fatfs_file *handle = file->private;
	uint64 total = 0;
	UINT transferred;
	uint32 chunk;
	FRESULT result;
	int copy_error = VFS_OK;

	if (*position > 0xffffffffULL)
		return 0;
	sleeplock_acquire(&fatfs_port.lock);
	result = f_lseek(&handle->file, *position);
	while (result == FR_OK && total < count) {
		chunk = count - total > PGSIZE ? PGSIZE : count - total;
		result = f_read(&handle->file, handle->buffer, chunk,
		                &transferred);
		if (result != FR_OK)
			break;
		if (either_copyout(user_destination, destination + total,
		                   handle->buffer, transferred) < 0) {
			copy_error = VFS_ERR_FAULT;
			break;
		}
		total += transferred;
		if (transferred != chunk)
			break;
	}
	*position += total;
	sleeplock_release(&fatfs_port.lock);
	if (total)
		return total;
	if (copy_error < 0)
		return copy_error;
	return result == FR_OK ? 0 : fatfs_result(result);
}

static int64 fatfs_write(struct vfs_file *file, int user_source,
			 uint64 source, uint64 count, uint64 *position)
{
	struct fatfs_file *handle = file->private;
	uint64 total = 0;
	UINT transferred;
	uint32 chunk;
	FRESULT result;

	if (*position > 0xffffffffULL ||
	    count > 0xffffffffULL - *position)
		return VFS_ERR_NOSPC;
	sleeplock_acquire(&fatfs_port.lock);
	result = f_lseek(&handle->file, *position);
	while (result == FR_OK && total < count) {
		chunk = count - total > PGSIZE ? PGSIZE : count - total;
		if (either_copyin(handle->buffer, user_source,
		                  source + total, chunk) < 0) {
			result = FR_INT_ERR;
			break;
		}
		result = f_write(&handle->file, handle->buffer, chunk,
		                 &transferred);
		if (result != FR_OK)
			break;
		total += transferred;
		if (transferred != chunk)
			break;
	}
	*position += total;
	file->path.dentry->inode->size = f_size(&handle->file);
	file->path.dentry->inode->blocks =
		(f_size(&handle->file) + 511) / 512;
	sleeplock_release(&fatfs_port.lock);
	return (result == FR_OK || total) ? total : fatfs_result(result);
}

static int fatfs_file_sync(struct vfs_file *file)
{
	struct fatfs_file *handle = file->private;
	FRESULT result;

	sleeplock_acquire(&fatfs_port.lock);
	result = handle ? f_sync(&handle->file) : FR_OK;
	if (result == FR_OK && block_device_flush(fatfs_port.device))
		result = FR_DISK_ERR;
	sleeplock_release(&fatfs_port.lock);
	return fatfs_result(result);
}

static const struct vfs_file_operations fatfs_file_operations = {
	.flags = VFS_FILE_CAN_PREAD,
	.open = fatfs_file_open,
	.release = fatfs_file_release,
	.read = fatfs_read,
	.write = fatfs_write,
	.fsync = fatfs_file_sync,
};

static int fatfs_directory_open(struct vfs_inode *inode,
				struct vfs_file *file)
{
	struct fatfs_inode *private = inode->private;
	struct fatfs_directory *handle;
	FRESULT result;

	handle = malloc(sizeof(*handle));
	if (!handle)
		return VFS_ERR_NOMEM;
	memset(handle, 0, sizeof(*handle));
	sleeplock_acquire(&fatfs_port.lock);
	result = f_opendir(&handle->directory, private->path);
	sleeplock_release(&fatfs_port.lock);
	if (result != FR_OK) {
		free(handle);
		return fatfs_result(result);
	}
	file->private = handle;
	return VFS_OK;
}

static void fatfs_directory_release(struct vfs_file *file)
{
	struct fatfs_directory *handle = file->private;

	if (!handle)
		return;
	sleeplock_acquire(&fatfs_port.lock);
	f_closedir(&handle->directory);
	sleeplock_release(&fatfs_port.lock);
	free(handle);
}

static int fatfs_directory_sync(struct vfs_file *file)
{
	return fatfs_sync(file->path.dentry->inode->superblock);
}

static int fatfs_readdir(struct vfs_file *file,
			 struct vfs_dirent *result)
{
	struct fatfs_directory *handle = file->private;
	struct fatfs_inode *inode = file->path.dentry->inode->private;
	char *path;
	uint32 length;
	FRESULT operation;
	int status;

	if (file->position < 2) {
		result->ino = file->path.dentry->inode->number;
		result->type = VFS_DT_DIR;
		safe_strncpy(result->name, file->position ? ".." : ".",
		             sizeof(result->name));
		result->next_offset = ++file->position;
		return 1;
	}
	sleeplock_acquire(&fatfs_port.lock);
	operation = f_readdir(&handle->directory, &handle->info);
	sleeplock_release(&fatfs_port.lock);
	if (operation != FR_OK)
		return fatfs_result(operation);
	if (!handle->info.fname[0])
		return 0;
	length = strlen(handle->info.fname);
	if (length > VFS_NAME_MAX)
		return VFS_ERR_NAMETOOLONG;
	path = palloc();
	if (!path)
		return VFS_ERR_NOMEM;
	status = fatfs_join(path, inode->path, handle->info.fname);
	if (status < 0) {
		pfree(path);
		return status;
	}
	result->ino = fatfs_inode_number(path);
	pfree(path);
	result->type = handle->info.fattrib & AM_DIR ?
		VFS_DT_DIR : VFS_DT_REGULAR;
	safe_strncpy(result->name, handle->info.fname,
	             sizeof(result->name));
	result->next_offset = ++file->position;
	return 1;
}

static int fatfs_seekdir(struct vfs_file *file, uint64 position)
{
	struct fatfs_directory *handle = file->private;
	uint64 index;
	FRESULT operation;

	if (!handle)
		return VFS_ERR_INVAL;
	sleeplock_acquire(&fatfs_port.lock);
	operation = f_rewinddir(&handle->directory);
	for (index = 2; operation == FR_OK && index < position; index++) {
		operation = f_readdir(&handle->directory, &handle->info);
		if (operation == FR_OK && !handle->info.fname[0])
			operation = FR_INT_ERR;
	}
	sleeplock_release(&fatfs_port.lock);
	if (operation != FR_OK)
		return fatfs_result(operation);
	file->position = position;
	return VFS_OK;
}

static const struct vfs_file_operations fatfs_directory_operations = {
	.open = fatfs_directory_open,
	.release = fatfs_directory_release,
	.readdir = fatfs_readdir,
	.seekdir = fatfs_seekdir,
	.fsync = fatfs_directory_sync,
};

static int fatfs_mount(struct vfs_filesystem_type *type,
		       struct block_device *device, const void *data,
		       struct vfs_super_block **result)
{
	struct vfs_super_block *superblock = 0;
	FRESULT operation;

	(void)data;
	if (fatfs_port.active)
		return VFS_ERR_BUSY;
	if (!device || device->sector_size != 512 ||
	    device->sector_count > 0xffffffffULL)
		return VFS_ERR_NODEV;
	memset(&fatfs_port.filesystem, 0, sizeof(fatfs_port.filesystem));
	fatfs_port.device = device;
	fatfs_set_block_device(device);
	sleeplock_acquire(&fatfs_port.lock);
	operation = f_mount(&fatfs_port.filesystem, "0:", 1);
	sleeplock_release(&fatfs_port.lock);
	if (operation != FR_OK) {
		fatfs_set_block_device(0);
		fatfs_port.device = 0;
		return fatfs_result(operation);
	}
	superblock = vfs_super_alloc(type, device);
	if (!superblock)
		goto fail;
	superblock->operations = &fatfs_super_operations;
	superblock->block_size = fatfs_port.filesystem.csize * 512;
	sleeplock_acquire(&fatfs_port.lock);
	superblock->root = fatfs_wrap_locked(superblock, FATFS_ROOT);
	sleeplock_release(&fatfs_port.lock);
	if (!superblock->root)
		goto fail;
	fatfs_port.active = 1;
	*result = superblock;
	return VFS_OK;
fail:
	if (superblock) {
		superblock->root = 0;
		vfs_super_free(superblock);
	}
	sleeplock_acquire(&fatfs_port.lock);
	f_mount(0, "0:", 0);
	sleeplock_release(&fatfs_port.lock);
	fatfs_set_block_device(0);
	fatfs_port.device = 0;
	return VFS_ERR_NOMEM;
}

static struct vfs_filesystem_type fatfs_type = {
	.name = "fat",
	.flags = VFS_FS_REQUIRES_DEVICE,
	.mount = fatfs_mount,
};

void fatfs_init(void)
{
	sleeplock_init(&fatfs_port.lock, "fatfs");
	if (vfs_register_filesystem(&fatfs_type) != VFS_OK)
		PANIC("register fatfs");
}
