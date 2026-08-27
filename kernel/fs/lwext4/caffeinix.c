#include <block_device.h>
#include <debug.h>
#include <device.h>
#include <ext4.h>
#include <ext4_errno.h>
#include <ext4_inode.h>
#include <ext4_super.h>
#include <ext4_types.h>
#include <ext4fs.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
#include <sleeplock.h>
#include <vfs.h>

#define EXT4FS_DEVICE_NAME "root"
#define EXT4FS_MOUNT_POINT "/"
#define EXT4FS_ORPHAN_DIRECTORY "/.caffeinix-orphans"
#define EXT4FS_ORPHAN_NAME ".caffeinix-orphans"
#define EXT4FS_OPEN_MAX 100

struct ext4fs_port {
	int active;
	struct block_device *device;
	struct ext4_blockdev_iface interface;
	struct ext4_blockdev blockdev;
	struct ext4_sblock *raw_superblock;
};

struct ext4fs_inode {
	struct ext4_inode raw;
	char path[VFS_PATH_MAX];
};

struct ext4fs_file {
	ext4_file file;
	void *buffer;
};

struct ext4fs_directory {
	ext4_dir directory;
	int root;
};

struct ext4fs_open_inode {
	uint32 inode;
	uint32 count;
	int orphaned;
};

static struct ext4fs_port ext4fs_port;
static struct sleeplock ext4fs_lock;
static thread_t ext4fs_lock_owner;
static uint32 ext4fs_lock_depth;
static struct ext4fs_open_inode ext4fs_open_inodes[EXT4FS_OPEN_MAX];

static const struct vfs_inode_operations ext4fs_inode_operations;
static const struct vfs_file_operations ext4fs_file_operations;
static const struct vfs_file_operations ext4fs_directory_operations;

void ext4fs_debug_dump(void)
{
	thread_t owner = __atomic_load_n(&ext4fs_lock_owner,
					 __ATOMIC_RELAXED);
	thread_t sleep_owner = __atomic_load_n(&ext4fs_lock.owner,
					       __ATOMIC_RELAXED);

	printf_emergency("ext4 locked=%d owner=%p owner_tid=%d depth=%u "
			 "sleep_owner=%p sleep_owner_tid=%d\n",
			 ext4fs_lock.locked, (uint64)owner,
			 owner ? owner->tid : -1, ext4fs_lock_depth,
			 (uint64)sleep_owner,
			 sleep_owner ? sleep_owner->tid : -1);
}

static int ext4fs_is_orphan_directory(const char *directory,
				      const char *name)
{
	return !strcmp(directory, EXT4FS_MOUNT_POINT) &&
	       !strcmp(name, EXT4FS_ORPHAN_NAME);
}

static void ext4fs_orphan_path(char *path, uint32 inode)
{
	static const char digits[] = "0123456789abcdef";
	uint32 position = 0;
	int shift;

	while (EXT4FS_ORPHAN_DIRECTORY[position]) {
		path[position] = EXT4FS_ORPHAN_DIRECTORY[position];
		position++;
	}
	path[position++] = '/';
	for (shift = 28; shift >= 0; shift -= 4)
		path[position++] = digits[(inode >> shift) & 0xf];
	path[position] = 0;
}

static struct ext4fs_open_inode *ext4fs_find_open_inode(uint32 inode)
{
	struct ext4fs_open_inode *entry;

	for (entry = ext4fs_open_inodes;
	     entry != &ext4fs_open_inodes[EXT4FS_OPEN_MAX]; entry++) {
		if ((entry->count || entry->orphaned) &&
		    entry->inode == inode)
			return entry;
	}
	return 0;
}

static int ext4fs_get_open_inode(uint32 inode)
{
	struct ext4fs_open_inode *entry;
	struct ext4fs_open_inode *free_entry = 0;

	for (entry = ext4fs_open_inodes;
	     entry != &ext4fs_open_inodes[EXT4FS_OPEN_MAX]; entry++) {
		if ((entry->count || entry->orphaned) &&
		    entry->inode == inode) {
			entry->count++;
			return VFS_OK;
		}
		if (!entry->count && !entry->orphaned && !free_entry)
			free_entry = entry;
	}
	if (!free_entry)
		return VFS_ERR_NOSPC;
	free_entry->inode = inode;
	free_entry->count = 1;
	return VFS_OK;
}

static int ext4fs_result(int result)
{
	if (result == EOK)
		return VFS_OK;
	switch (result) {
	case EPERM:
	case EACCES:
	case EROFS:
		return VFS_ERR_PERM;
	case ENOENT:
		return VFS_ERR_NOENT;
	case EIO:
		return VFS_ERR_IO;
	case EEXIST:
		return VFS_ERR_EXIST;
	case ENOTDIR:
		return VFS_ERR_NOTDIR;
	case EISDIR:
		return VFS_ERR_ISDIR;
	case EINVAL:
		return VFS_ERR_INVAL;
	case ENOSPC:
		return VFS_ERR_NOSPC;
	case ENOTEMPTY:
		return VFS_ERR_NOTEMPTY;
	case ENODEV:
	case ENXIO:
		return VFS_ERR_NODEV;
	case EMLINK:
		return VFS_ERR_MLINK;
	case ENOMEM:
		return VFS_ERR_NOMEM;
	case ENOTSUP:
		return VFS_ERR_NOTSUPP;
	case ERANGE:
		return VFS_ERR_NAMETOOLONG;
	default:
		return VFS_ERR_IO;
	}
}

static int ext4fs_block_open(struct ext4_blockdev *blockdev)
{
	(void)blockdev;
	return EOK;
}

static int ext4fs_block_read(struct ext4_blockdev *blockdev, void *buffer,
			     uint64_t block, uint32_t count)
{
	struct block_device *device = blockdev->bdif->p_user;

	return block_device_read(device, block, buffer, count) ? EIO : EOK;
}

static int ext4fs_block_write(struct ext4_blockdev *blockdev,
			      const void *buffer, uint64_t block,
			      uint32_t count)
{
	struct block_device *device = blockdev->bdif->p_user;

	return block_device_write(device, block, buffer, count) ? EIO : EOK;
}

static int ext4fs_block_close(struct ext4_blockdev *blockdev)
{
	struct block_device *device = blockdev->bdif->p_user;

	return block_device_flush(device) ? EIO : EOK;
}

static void ext4fs_lock_mount(void)
{
	thread_t current = cur_thread();

	if (ext4fs_lock_owner == current) {
		ext4fs_lock_depth++;
		return;
	}
	sleeplock_acquire(&ext4fs_lock);
	ext4fs_lock_owner = current;
	ext4fs_lock_depth = 1;
}

static void ext4fs_unlock_mount(void)
{
	if (ext4fs_lock_owner != cur_thread() ||
	    !ext4fs_lock_depth)
		PANIC("ext4 lock owner");
	if (--ext4fs_lock_depth)
		return;
	ext4fs_lock_owner = 0;
	sleeplock_release(&ext4fs_lock);
}

static const struct ext4_lock ext4fs_mount_locks = {
	.lock = ext4fs_lock_mount,
	.unlock = ext4fs_unlock_mount,
};

static enum vfs_inode_type ext4fs_inode_type(uint32 mode)
{
	switch (mode & EXT4_INODE_MODE_TYPE_MASK) {
	case EXT4_INODE_MODE_FILE:
		return VFS_INODE_REGULAR;
	case EXT4_INODE_MODE_DIRECTORY:
		return VFS_INODE_DIRECTORY;
	case EXT4_INODE_MODE_CHARDEV:
		return VFS_INODE_CHAR_DEVICE;
	case EXT4_INODE_MODE_BLOCKDEV:
		return VFS_INODE_BLOCK_DEVICE;
	case EXT4_INODE_MODE_SOFTLINK:
		return VFS_INODE_SYMLINK;
	case EXT4_INODE_MODE_FIFO:
		return VFS_INODE_FIFO;
	case EXT4_INODE_MODE_SOCKET:
		return VFS_INODE_SOCKET;
	default:
		return VFS_INODE_NONE;
	}
}

static uint64 ext4fs_device_number(uint32 raw)
{
	uint32 major = (raw >> 8) & 0xfff;
	uint32 minor = (raw & 0xff) | ((raw >> 12) & 0xfff00);

	return VFS_MAKE_DEVICE(major, minor);
}

static void ext4fs_set_file_operations(struct vfs_inode *inode)
{
	if (inode->type == VFS_INODE_REGULAR)
		inode->file_operations = &ext4fs_file_operations;
	else if (inode->type == VFS_INODE_DIRECTORY)
		inode->file_operations = &ext4fs_directory_operations;
	else if (inode->type == VFS_INODE_CHAR_DEVICE)
		inode->file_operations = &vfs_device_operations;
	else
		inode->file_operations = 0;
}

static void ext4fs_copy_time(struct vfs_timespec *destination,
			     const struct ext4_timespec *source)
{
	destination->seconds = source->seconds;
	destination->nanoseconds = source->nanoseconds;
}

static int ext4fs_now(struct ext4_timespec *time)
{
	struct vfs_timespec now;
	int result = vfs_current_time(&now);

	if (result < 0)
		return result;
	time->seconds = now.seconds;
	time->nanoseconds = now.nanoseconds;
	return VFS_OK;
}

static int ext4fs_touch(const char *path, uint32 mask)
{
	struct ext4_timespec times[3];
	struct ext4_timespec now;
	int result;

	result = ext4fs_now(&now);
	if (result < 0)
		return result;
	times[0] = now;
	times[1] = now;
	times[2] = now;
	result = ext4_times_set(path, times, mask);
	return result == ERANGE ? VFS_ERR_OVERFLOW : ext4fs_result(result);
}

static int ext4fs_refresh(struct vfs_inode *inode)
{
	struct ext4fs_inode *private = inode->private;
	struct ext4_timespec time;
	uint32 mode, number;
	int result;

	if (inode->number) {
		number = inode->number;
		result = ext4_raw_inode_fill_by_number(EXT4FS_MOUNT_POINT,
						       number, &private->raw);
	} else {
		result = ext4_raw_inode_fill(private->path, &number,
					    &private->raw);
	}
	if (result != EOK)
		return ext4fs_result(result);
	mode = ext4_inode_get_mode(ext4fs_port.raw_superblock,
	                           &private->raw);
	inode->number = number;
	inode->type = ext4fs_inode_type(mode);
	inode->mode = mode & VFS_MODE_PERMISSIONS;
	inode->uid = ext4_inode_get_uid(&private->raw);
	inode->gid = ext4_inode_get_gid(&private->raw);
	inode->nlink = ext4_inode_get_links_cnt(&private->raw);
	inode->device = ext4fs_device_number(
		ext4_inode_get_dev(&private->raw));
	inode->size = ext4_inode_get_size(ext4fs_port.raw_superblock,
	                                 &private->raw);
	inode->blocks = ext4_inode_get_blocks_count(
		ext4fs_port.raw_superblock, &private->raw);
	ext4_inode_get_access_time_ext(ext4fs_port.raw_superblock,
				       &private->raw, &time);
	ext4fs_copy_time(&inode->atime, &time);
	ext4_inode_get_modif_time_ext(ext4fs_port.raw_superblock,
				      &private->raw, &time);
	ext4fs_copy_time(&inode->mtime, &time);
	ext4_inode_get_change_time_ext(ext4fs_port.raw_superblock,
				       &private->raw, &time);
	ext4fs_copy_time(&inode->ctime, &time);
	ext4fs_set_file_operations(inode);
	return VFS_OK;
}

static int ext4fs_wrap(struct vfs_super_block *superblock,
		       const char *path, struct vfs_inode **result)
{
	struct ext4fs_inode *private;
	struct vfs_inode *inode;
	int status;

	if (strlen(path) >= VFS_PATH_MAX)
		return VFS_ERR_NAMETOOLONG;
	private = malloc(sizeof(*private));
	if (!private)
		return VFS_ERR_NOMEM;
	memset(private, 0, sizeof(*private));
	safe_strncpy(private->path, path, sizeof(private->path));
	inode = vfs_inode_alloc(superblock);
	if (!inode) {
		free(private);
		return VFS_ERR_NOMEM;
	}
	inode->private = private;
	inode->operations = &ext4fs_inode_operations;
	status = ext4fs_refresh(inode);
	if (status < 0) {
		vfs_inode_put(inode);
		return status;
	}
	*result = inode;
	return VFS_OK;
}

static int ext4fs_join(char *path, const char *directory, const char *name)
{
	uint32 directory_length = strlen(directory);
	uint32 name_length = strlen(name);
	uint32 position = 0;

	if (!name_length || name_length > VFS_NAME_MAX)
		return name_length ? VFS_ERR_NAMETOOLONG : VFS_ERR_INVAL;
	if (directory_length + name_length + 2 > VFS_PATH_MAX)
		return VFS_ERR_NAMETOOLONG;
	while (*directory)
		path[position++] = *directory++;
	if (!position || path[position - 1] != '/')
		path[position++] = '/';
	while (*name)
		path[position++] = *name++;
	path[position] = 0;
	return VFS_OK;
}

static int ext4fs_remove_locked(const char *path, uint32 inode,
				struct ext4_inode *raw)
{
	struct ext4fs_open_inode *entry = ext4fs_find_open_inode(inode);
	char *orphan_path;
	int result;

	if (!entry || !entry->count ||
	    ext4_inode_get_links_cnt(raw) != 1)
		return ext4fs_result(ext4_fremove(path));

	orphan_path = palloc();
	if (!orphan_path)
		return VFS_ERR_NOMEM;
	ext4fs_orphan_path(orphan_path, inode);
	result = ext4_flink(path, orphan_path);
	if (result == EOK) {
		result = ext4_fremove(path);
		if (result == EOK)
			entry->orphaned = 1;
		else
			ext4_fremove(orphan_path);
	}
	pfree(orphan_path);
	return ext4fs_result(result);
}

static void ext4fs_put_open_inode(uint32 inode)
{
	struct ext4fs_open_inode *entry = ext4fs_find_open_inode(inode);
	char *path;

	if (!entry || !entry->count)
		PANIC("ext4 open inode");
	entry->count--;
	if (entry->count)
		return;
	if (!entry->orphaned) {
		memset(entry, 0, sizeof(*entry));
		return;
	}
	path = palloc();
	if (!path)
		return;
	ext4fs_orphan_path(path, inode);
	if (ext4_fremove(path) == EOK)
		memset(entry, 0, sizeof(*entry));
	pfree(path);
}

static int ext4fs_clean_orphan_directory(void)
{
	const ext4_direntry *entry;
	ext4_dir directory;
	char name[VFS_NAME_MAX + 1];
	char *path = palloc();
	uint32 length;
	int found, result;

	if (!path)
		return VFS_ERR_NOMEM;
	result = ext4_dir_mk(EXT4FS_ORPHAN_DIRECTORY);
	if (result != EOK && result != EEXIST)
		goto out;
	result = ext4_mode_set(EXT4FS_ORPHAN_DIRECTORY, 0700);
	if (result != EOK)
		goto out;
	do {
		found = 0;
		result = ext4_dir_open(&directory, EXT4FS_ORPHAN_DIRECTORY);
		if (result != EOK)
			goto out;
		while ((entry = ext4_dir_entry_next(&directory))) {
			if ((entry->name_length == 1 && entry->name[0] == '.') ||
			    (entry->name_length == 2 && entry->name[0] == '.' &&
			     entry->name[1] == '.'))
				continue;
			length = entry->name_length;
			if (length > VFS_NAME_MAX) {
				result = ERANGE;
				break;
			}
			memmove(name, entry->name, length);
			name[length] = 0;
			found = 1;
			break;
		}
		ext4_dir_close(&directory);
		if (result != EOK || !found)
			break;
		result = ext4fs_join(path, EXT4FS_ORPHAN_DIRECTORY, name);
		if (result != VFS_OK) {
			result = EIO;
			break;
		}
		result = ext4_fremove(path);
	} while (result == EOK);
out:
	pfree(path);
	return ext4fs_result(result);
}

static void ext4fs_put_inode(struct vfs_inode *inode)
{
	free(inode->private);
}

static int ext4fs_sync(struct vfs_super_block *superblock)
{
	int result = ext4_cache_flush(EXT4FS_MOUNT_POINT);

	if (result != EOK)
		return ext4fs_result(result);
	result = ext4_sb_write(&ext4fs_port.blockdev,
	                       ext4fs_port.raw_superblock);
	if (result != EOK)
		return ext4fs_result(result);
	return block_device_flush(superblock->device) ?
		VFS_ERR_IO : VFS_OK;
}

static const struct vfs_super_operations ext4fs_super_operations = {
	.put_inode = ext4fs_put_inode,
	.sync = ext4fs_sync,
};

static int ext4fs_getattr(struct vfs_inode *inode, struct vfs_stat *stat)
{
	int result = ext4fs_refresh(inode);

	if (result < 0)
		return result;
	return vfs_inode_stat_default(inode, stat);
}

static int ext4fs_lookup(struct vfs_inode *directory, const char *name,
				  struct vfs_inode **result)
{
	struct ext4fs_inode *private = directory->private;
	char *path;
	int status;

	if (ext4fs_is_orphan_directory(private->path, name))
		return VFS_ERR_NOENT;
	path = palloc();
	if (!path)
		return VFS_ERR_NOMEM;
	status = ext4fs_join(path, private->path, name);
	if (status == VFS_OK)
		status = ext4fs_wrap(directory->superblock, path, result);
	pfree(path);
	return status;
}

static int ext4fs_create(struct vfs_inode *directory, const char *name,
			 uint32 mode, struct vfs_inode **result)
{
	struct ext4fs_inode *private = directory->private;
	char *path = palloc();
	ext4_file file;
	int status;

	if (!path)
		return VFS_ERR_NOMEM;
	ext4fs_lock_mount();
	status = ext4fs_join(path, private->path, name);
	if (status < 0)
		goto out;
	status = ext4_fopen2(&file, path, O_CREAT | O_EXCL | O_RDWR);
	if (status != EOK) {
		status = ext4fs_result(status);
		goto out;
	}
	ext4_fclose(&file);
	status = ext4_mode_set(path, mode & VFS_MODE_PERMISSIONS);
	if (status != EOK) {
		ext4_fremove(path);
		status = ext4fs_result(status);
		goto out;
	}
	status = ext4fs_touch(path, EXT4_TIME_ATIME | EXT4_TIME_MTIME |
				     EXT4_TIME_CTIME);
	if (status < 0) {
		ext4_fremove(path);
		goto out;
	}
	status = ext4fs_wrap(directory->superblock, path, result);
	if (status < 0) {
		ext4_fremove(path);
		goto out;
	}
	ext4fs_touch(private->path, EXT4_TIME_MTIME | EXT4_TIME_CTIME);
out:
	ext4fs_unlock_mount();
	pfree(path);
	return status;
}

static int ext4fs_mkdir(struct vfs_inode *directory, const char *name,
			uint32 mode, struct vfs_inode **result)
{
	struct ext4fs_inode *private = directory->private;
	char *path = palloc();
	int status;

	if (!path)
		return VFS_ERR_NOMEM;
	ext4fs_lock_mount();
	status = ext4fs_join(path, private->path, name);
	if (status < 0)
		goto out;
	status = ext4_dir_mk(path);
	if (status != EOK) {
		status = ext4fs_result(status);
		goto out;
	}
	status = ext4_mode_set(path, mode & VFS_MODE_PERMISSIONS);
	if (status != EOK) {
		ext4_dir_rm(path);
		status = ext4fs_result(status);
		goto out;
	}
	status = ext4fs_touch(path, EXT4_TIME_ATIME | EXT4_TIME_MTIME |
				     EXT4_TIME_CTIME);
	if (status < 0) {
		ext4_dir_rm(path);
		goto out;
	}
	status = ext4fs_wrap(directory->superblock, path, result);
	if (status < 0) {
		ext4_dir_rm(path);
		goto out;
	}
	ext4fs_touch(private->path, EXT4_TIME_MTIME | EXT4_TIME_CTIME);
out:
	ext4fs_unlock_mount();
	pfree(path);
	return status;
}

static int ext4fs_unlink(struct vfs_inode *directory, const char *name)
{
	struct ext4fs_inode *private = directory->private;
	struct ext4_inode raw;
	uint32 inode;
	char *path = palloc();
	int status;

	if (!path)
		return VFS_ERR_NOMEM;
	status = ext4fs_join(path, private->path, name);
	if (status == VFS_OK) {
		ext4fs_lock_mount();
		status = ext4_raw_inode_fill(path, &inode, &raw);
		if (status == EOK) {
			ext4fs_touch(path, EXT4_TIME_CTIME);
			status = ext4fs_remove_locked(path, inode, &raw);
		} else {
			status = ext4fs_result(status);
		}
		ext4fs_unlock_mount();
	}
	if (status == VFS_OK)
		ext4fs_touch(private->path,
			     EXT4_TIME_MTIME | EXT4_TIME_CTIME);
	pfree(path);
	return status;
}

static int ext4fs_directory_empty(const char *path)
{
	const ext4_direntry *entry;
	ext4_dir directory;
	int result;

	result = ext4_dir_open(&directory, path);
	if (result != EOK)
		return ext4fs_result(result);
	while ((entry = ext4_dir_entry_next(&directory))) {
		if ((entry->name_length == 1 && entry->name[0] == '.') ||
		    (entry->name_length == 2 && entry->name[0] == '.' &&
		     entry->name[1] == '.'))
			continue;
		ext4_dir_close(&directory);
		return VFS_ERR_NOTEMPTY;
	}
	ext4_dir_close(&directory);
	return VFS_OK;
}

static int ext4fs_rmdir(struct vfs_inode *directory, const char *name)
{
	struct ext4fs_inode *private = directory->private;
	char *path = palloc();
	int status;

	if (!path)
		return VFS_ERR_NOMEM;
	status = ext4fs_join(path, private->path, name);
	if (status == VFS_OK)
		status = ext4fs_directory_empty(path);
	if (status == VFS_OK)
		status = ext4fs_result(ext4_dir_rm(path));
	if (status == VFS_OK)
		ext4fs_touch(private->path,
			     EXT4_TIME_MTIME | EXT4_TIME_CTIME);
	pfree(path);
	return status;
}

static int ext4fs_link(struct vfs_inode *inode,
		       struct vfs_inode *directory, const char *name)
{
	struct ext4fs_inode *source = inode->private;
	struct ext4fs_inode *parent = directory->private;
	char *path = palloc();
	int status;

	if (!path)
		return VFS_ERR_NOMEM;
	status = ext4fs_join(path, parent->path, name);
	if (status == VFS_OK)
		status = ext4fs_result(ext4_flink(source->path, path));
	if (status == VFS_OK)
		ext4fs_refresh(inode);
	if (status == VFS_OK) {
		ext4fs_touch(source->path, EXT4_TIME_CTIME);
		ext4fs_touch(parent->path,
			     EXT4_TIME_MTIME | EXT4_TIME_CTIME);
	}
	pfree(path);
	return status;
}

static int ext4fs_symlink(struct vfs_inode *directory, const char *name,
			  const char *target, struct vfs_inode **result)
{
	struct ext4fs_inode *parent = directory->private;
	char *path = palloc();
	int status;

	if (!path)
		return VFS_ERR_NOMEM;
	ext4fs_lock_mount();
	status = ext4fs_join(path, parent->path, name);
	if (status < 0)
		goto out;
	status = ext4_fsymlink(target, path);
	if (status != EOK) {
		status = ext4fs_result(status);
		goto out;
	}
	status = ext4fs_touch(path, EXT4_TIME_ATIME | EXT4_TIME_MTIME |
				     EXT4_TIME_CTIME);
	if (status < 0) {
		ext4_fremove(path);
		goto out;
	}
	status = ext4fs_wrap(directory->superblock, path, result);
	if (status < 0) {
		ext4_fremove(path);
		goto out;
	}
	ext4fs_touch(parent->path, EXT4_TIME_MTIME | EXT4_TIME_CTIME);
out:
	ext4fs_unlock_mount();
	pfree(path);
	return status;
}

static int ext4fs_rename(struct vfs_inode *old_directory,
			 const char *old_name,
			 struct vfs_inode *new_directory,
			 const char *new_name, uint32 flags)
{
	struct ext4fs_inode *old_parent = old_directory->private;
	struct ext4fs_inode *new_parent = new_directory->private;
	struct ext4_inode old_raw, new_raw;
	uint32 old_number, new_number;
	enum vfs_inode_type old_type, new_type;
	char *old_path = palloc();
	char *new_path;
	int result, status;

	if (!old_path)
		return VFS_ERR_NOMEM;
	new_path = old_path + VFS_PATH_MAX;
	status = ext4fs_join(old_path, old_parent->path, old_name);
	if (status < 0)
		goto out;
	status = ext4fs_join(new_path, new_parent->path, new_name);
	if (status < 0)
		goto out;
	ext4fs_lock_mount();
	result = ext4_raw_inode_fill(old_path, &old_number, &old_raw);
	if (result != EOK) {
		status = ext4fs_result(result);
		goto unlock;
	}
	result = ext4_raw_inode_fill(new_path, &new_number, &new_raw);
	if (result == EOK) {
		if (flags & VFS_RENAME_NOREPLACE) {
			status = VFS_ERR_EXIST;
			goto unlock;
		}
		if (old_number == new_number) {
			status = VFS_OK;
			goto unlock;
		}
		old_type = ext4fs_inode_type(ext4_inode_get_mode(
			ext4fs_port.raw_superblock, &old_raw));
		new_type = ext4fs_inode_type(ext4_inode_get_mode(
			ext4fs_port.raw_superblock, &new_raw));
		if (old_type == VFS_INODE_DIRECTORY &&
		    new_type != VFS_INODE_DIRECTORY) {
			status = VFS_ERR_NOTDIR;
			goto unlock;
		}
		if (old_type != VFS_INODE_DIRECTORY &&
		    new_type == VFS_INODE_DIRECTORY) {
			status = VFS_ERR_ISDIR;
			goto unlock;
		}
		if (new_type == VFS_INODE_DIRECTORY) {
			status = ext4fs_directory_empty(new_path);
			if (status < 0)
				goto unlock;
			result = ext4_dir_rm(new_path);
		} else {
			status = ext4fs_remove_locked(new_path, new_number,
						     &new_raw);
			result = status == VFS_OK ? EOK : EIO;
		}
		if (status != VFS_OK || result != EOK) {
			if (status == VFS_OK)
				status = ext4fs_result(result);
			goto unlock;
		}
	} else if (result != ENOENT) {
		status = ext4fs_result(result);
		goto unlock;
	}
	status = ext4fs_result(ext4_frename(old_path, new_path));
unlock:
	ext4fs_unlock_mount();
	if (status == VFS_OK) {
		ext4fs_touch(new_path, EXT4_TIME_CTIME);
		ext4fs_touch(old_parent->path,
			     EXT4_TIME_MTIME | EXT4_TIME_CTIME);
		if (strcmp(old_parent->path, new_parent->path))
			ext4fs_touch(new_parent->path,
				     EXT4_TIME_MTIME | EXT4_TIME_CTIME);
	}
out:
	pfree(old_path);
	return status;
}

static int ext4fs_readlink(struct vfs_inode *inode, char *buffer,
			   uint32 size)
{
	struct ext4fs_inode *private = inode->private;
	size_t count;
	int result = ext4_readlink(private->path, buffer, size, &count);
	if (result == EOK)
		ext4fs_touch(private->path, EXT4_TIME_ATIME);

	return result == EOK ? (int)count : ext4fs_result(result);
}

static int ext4fs_truncate(struct vfs_inode *inode, uint64 size)
{
	struct ext4fs_inode *private = inode->private;
	ext4_file file;
	void *zeros = 0;
	uint64 position;
	int result;

	result = ext4_fopen2(&file, private->path, O_RDWR);
	if (result != EOK)
		return ext4fs_result(result);
	if (size > file.fsize) {
		zeros = palloc_zero();
		if (!zeros) {
			ext4_fclose(&file);
			return VFS_ERR_NOMEM;
		}
		position = file.fsize;
		result = ext4_fseek(&file, position, SEEK_SET);
		while (result == EOK && position < size) {
			size_t count = size - position > PGSIZE ?
				PGSIZE : size - position;
			size_t written = 0;

			result = ext4_fwrite(&file, zeros, count, &written);
			if (result == EOK && written != count)
				result = EIO;
			position += written;
		}
		pfree(zeros);
	} else {
		result = ext4_ftruncate(&file, size);
	}
	ext4_fclose(&file);
	if (result == EOK)
		ext4fs_touch(private->path,
			     EXT4_TIME_MTIME | EXT4_TIME_CTIME);
	if (result == EOK)
		ext4fs_refresh(inode);
	return ext4fs_result(result);
}

static int ext4fs_set_times(struct vfs_inode *inode,
			    const struct vfs_timespec times[2], uint32 mask)
{
	struct ext4fs_inode *private = inode->private;
	struct ext4_timespec ext4_times[3];
	int result;

	if (mask & VFS_TIME_ATIME) {
		ext4_times[0].seconds = times[0].seconds;
		ext4_times[0].nanoseconds = times[0].nanoseconds;
	}
	if (mask & VFS_TIME_MTIME) {
		ext4_times[1].seconds = times[1].seconds;
		ext4_times[1].nanoseconds = times[1].nanoseconds;
	}
	result = ext4fs_now(&ext4_times[2]);
	if (result < 0)
		return result;
	result = ext4_times_set(private->path, ext4_times,
				(mask & VFS_TIME_ATIME ? EXT4_TIME_ATIME : 0) |
				(mask & VFS_TIME_MTIME ? EXT4_TIME_MTIME : 0) |
				EXT4_TIME_CTIME);
	if (result == ERANGE)
		return VFS_ERR_OVERFLOW;
	if (result != EOK)
		return ext4fs_result(result);
	return ext4fs_refresh(inode);
}

static const struct vfs_inode_operations ext4fs_inode_operations = {
	.lookup = ext4fs_lookup,
	.create = ext4fs_create,
	.mkdir = ext4fs_mkdir,
	.unlink = ext4fs_unlink,
	.rmdir = ext4fs_rmdir,
	.rename = ext4fs_rename,
	.link = ext4fs_link,
	.symlink = ext4fs_symlink,
	.readlink = ext4fs_readlink,
	.truncate = ext4fs_truncate,
	.set_times = ext4fs_set_times,
	.getattr = ext4fs_getattr,
};

static int ext4fs_file_open(struct vfs_inode *inode, struct vfs_file *file)
{
	struct ext4fs_inode *private = inode->private;
	struct ext4fs_file *handle;
	int flags, result;

	handle = malloc(sizeof(*handle));
	if (!handle)
		return VFS_ERR_NOMEM;
	memset(handle, 0, sizeof(*handle));
	handle->buffer = palloc();
	if (!handle->buffer) {
		free(handle);
		return VFS_ERR_NOMEM;
	}
	if ((file->flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) ==
	    (VFS_OPEN_READ | VFS_OPEN_WRITE))
		flags = O_RDWR;
	else if (file->flags & VFS_OPEN_WRITE)
		flags = O_WRONLY;
	else
		flags = O_RDONLY;
	ext4fs_lock_mount();
	result = ext4_fopen2(&handle->file, private->path, flags);
	if (result == EOK) {
		result = ext4fs_get_open_inode(handle->file.inode);
		if (result != VFS_OK)
			ext4_fclose(&handle->file);
	}
	ext4fs_unlock_mount();
	if (result != EOK) {
		pfree(handle->buffer);
		free(handle);
		return result < 0 ? result : ext4fs_result(result);
	}
	file->private = handle;
	return VFS_OK;
}

static void ext4fs_file_release(struct vfs_file *file)
{
	struct ext4fs_file *handle = file->private;
	uint32 inode;

	if (!handle)
		return;
	inode = handle->file.inode;
	ext4fs_lock_mount();
	ext4_fclose(&handle->file);
	ext4fs_put_open_inode(inode);
	ext4fs_unlock_mount();
	pfree(handle->buffer);
	free(handle);
}

static int64 ext4fs_read(struct vfs_file *file, int user_destination,
			 uint64 destination, uint64 count, uint64 *position)
{
	struct ext4fs_file *handle = file->private;
	uint64 total = 0;
	size_t transferred;
	uint32 chunk;
	int64 status;
	int result;

	ext4fs_lock_mount();
	result = ext4_fseek(&handle->file, *position, SEEK_SET);
	if (result != EOK) {
		status = ext4fs_result(result);
		goto out;
	}
	while (total < count) {
		chunk = count - total > PGSIZE ? PGSIZE : count - total;
		result = ext4_fread(&handle->file, handle->buffer, chunk,
		                    &transferred);
		if (result != EOK) {
			status = total ? total : ext4fs_result(result);
			goto out;
		}
		if (either_copyout(user_destination, destination + total,
		                   handle->buffer, transferred) < 0) {
			status = total ? total : VFS_ERR_FAULT;
			goto out;
		}
		total += transferred;
		if (transferred != chunk)
			break;
	}
	*position += total;
	status = total;
out:
	if (status >= 0 && count)
		ext4fs_touch(((struct ext4fs_inode *)
			     file->path.dentry->inode->private)->path,
			     EXT4_TIME_ATIME);
	ext4fs_unlock_mount();
	return status;
}

static int64 ext4fs_write(struct vfs_file *file, int user_source,
			  uint64 source, uint64 count, uint64 *position)
{
	struct ext4fs_file *handle = file->private;
	uint64 total = 0;
	size_t transferred;
	uint32 chunk;
	int64 status;
	int result;

	ext4fs_lock_mount();
	result = ext4_fseek(&handle->file, *position, SEEK_SET);
	if (result != EOK) {
		status = ext4fs_result(result);
		goto out;
	}
	while (total < count) {
		chunk = count - total > PGSIZE ? PGSIZE : count - total;
		if (either_copyin(handle->buffer, user_source, source + total,
		                  chunk) < 0) {
			status = total ? total : VFS_ERR_IO;
			goto out;
		}
		result = ext4_fwrite(&handle->file, handle->buffer, chunk,
		                     &transferred);
		if (result != EOK) {
			status = total ? total : ext4fs_result(result);
			goto out;
		}
		total += transferred;
		if (transferred != chunk)
			break;
	}
	*position += total;
	if (total)
		ext4fs_touch(((struct ext4fs_inode *)
			     file->path.dentry->inode->private)->path,
			     EXT4_TIME_MTIME | EXT4_TIME_CTIME);
	ext4fs_refresh(file->path.dentry->inode);
	status = total;
out:
	ext4fs_unlock_mount();
	return status;
}

static int ext4fs_file_sync(struct vfs_file *file)
{
	return ext4fs_sync(file->path.dentry->inode->superblock);
}

static int ext4fs_directory_open(struct vfs_inode *inode,
				 struct vfs_file *file)
{
	struct ext4fs_inode *private = inode->private;
	struct ext4fs_directory *handle = malloc(sizeof(*handle));
	int result;

	if (!handle)
		return VFS_ERR_NOMEM;
	result = ext4_dir_open(&handle->directory, private->path);
	if (result != EOK) {
		free(handle);
		return ext4fs_result(result);
	}
	handle->root = !strcmp(private->path, EXT4FS_MOUNT_POINT);
	file->private = handle;
	return VFS_OK;
}

static void ext4fs_directory_release(struct vfs_file *file)
{
	struct ext4fs_directory *handle = file->private;

	if (!handle)
		return;
	ext4_dir_close(&handle->directory);
	free(handle);
}

static uint8 ext4fs_dirent_type(uint8 type)
{
	switch (type) {
	case EXT4_DE_REG_FILE:
		return VFS_DT_REGULAR;
	case EXT4_DE_DIR:
		return VFS_DT_DIR;
	case EXT4_DE_CHRDEV:
		return VFS_DT_CHAR;
	case EXT4_DE_BLKDEV:
		return VFS_DT_BLOCK;
	case EXT4_DE_FIFO:
		return VFS_DT_FIFO;
	case EXT4_DE_SOCK:
		return VFS_DT_SOCKET;
	case EXT4_DE_SYMLINK:
		return VFS_DT_SYMLINK;
	default:
		return VFS_DT_UNKNOWN;
	}
}

static int ext4fs_readdir(struct vfs_file *file,
			  struct vfs_dirent *result)
{
	struct ext4fs_directory *handle = file->private;
	const ext4_direntry *entry;
	uint32 length;

	do {
		entry = ext4_dir_entry_next(&handle->directory);
		if (!entry)
			return 0;
	} while (handle->root &&
		 entry->name_length == sizeof(EXT4FS_ORPHAN_NAME) - 1 &&
		 !strncmp((const char *)entry->name, EXT4FS_ORPHAN_NAME,
		          sizeof(EXT4FS_ORPHAN_NAME) - 1));
	length = entry->name_length;
	if (length > VFS_NAME_MAX)
		return VFS_ERR_NAMETOOLONG;
	result->ino = entry->inode;
	result->next_offset = handle->directory.next_off;
	result->type = ext4fs_dirent_type(entry->inode_type);
	memmove(result->name, entry->name, length);
	result->name[length] = 0;
	file->position = result->next_offset;
	ext4fs_touch(((struct ext4fs_inode *)
		     file->path.dentry->inode->private)->path,
		     EXT4_TIME_ATIME);
	return 1;
}

static int ext4fs_seekdir(struct vfs_file *file, uint64 position)
{
	struct ext4fs_directory *handle = file->private;

	if (!handle)
		return VFS_ERR_INVAL;
	handle->directory.next_off = position;
	file->position = position;
	return VFS_OK;
}

static const struct vfs_file_operations ext4fs_file_operations = {
	.flags = VFS_FILE_CAN_PREAD,
	.open = ext4fs_file_open,
	.release = ext4fs_file_release,
	.read = ext4fs_read,
	.write = ext4fs_write,
	.fsync = ext4fs_file_sync,
};

static const struct vfs_file_operations ext4fs_directory_operations = {
	.open = ext4fs_directory_open,
	.release = ext4fs_directory_release,
	.readdir = ext4fs_readdir,
	.seekdir = ext4fs_seekdir,
	.fsync = ext4fs_file_sync,
};

static int ext4fs_mount(struct vfs_filesystem_type *type,
			struct block_device *device, const void *data,
			struct vfs_super_block **result)
{
	struct vfs_super_block *superblock;
	struct ext4_mount_stats stats;
	int status;

	(void)data;
	if (ext4fs_port.active || !device)
		return VFS_ERR_BUSY;
	memset(&ext4fs_port, 0, sizeof(ext4fs_port));
	memset(ext4fs_open_inodes, 0, sizeof(ext4fs_open_inodes));
	ext4fs_port.device = device;
	ext4fs_port.interface.open = ext4fs_block_open;
	ext4fs_port.interface.bread = ext4fs_block_read;
	ext4fs_port.interface.bwrite = ext4fs_block_write;
	ext4fs_port.interface.close = ext4fs_block_close;
	ext4fs_port.interface.ph_bsize = device->sector_size;
	ext4fs_port.interface.ph_bcnt = device->sector_count;
	ext4fs_port.interface.p_user = device;
	ext4fs_port.interface.ph_bbuf = malloc(device->sector_size);
	if (!ext4fs_port.interface.ph_bbuf)
		return VFS_ERR_NOMEM;
	ext4fs_port.blockdev.bdif = &ext4fs_port.interface;
	ext4fs_port.blockdev.part_size =
		device->sector_count * device->sector_size;
	status = ext4_device_register(&ext4fs_port.blockdev,
	                              EXT4FS_DEVICE_NAME);
	if (status != EOK)
		goto fail_buffer;
	status = ext4_mount(EXT4FS_DEVICE_NAME, EXT4FS_MOUNT_POINT, false);
	if (status != EOK)
		goto fail_device;
	status = ext4_mount_setup_locks(EXT4FS_MOUNT_POINT,
	                                &ext4fs_mount_locks);
	if (status != EOK)
		goto fail_mount;
	status = ext4_recover(EXT4FS_MOUNT_POINT);
	if (status != EOK)
		goto fail_mount;
	status = ext4_journal_start(EXT4FS_MOUNT_POINT);
	if (status != EOK)
		goto fail_mount;
	status = ext4_mount_point_stats(EXT4FS_MOUNT_POINT, &stats);
	if (status != EOK)
		goto fail_journal;
	status = ext4_get_sblock(EXT4FS_MOUNT_POINT,
	                         &ext4fs_port.raw_superblock);
	if (status != EOK)
		goto fail_journal;
	status = ext4fs_clean_orphan_directory();
	if (status != VFS_OK) {
		status = EIO;
		goto fail_journal;
	}
	ext4fs_port.active = 1;
	superblock = vfs_super_alloc(type, device);
	if (!superblock) {
		status = ENOMEM;
		goto fail_active;
	}
	superblock->operations = &ext4fs_super_operations;
	superblock->block_size = stats.block_size;
	superblock->private = &ext4fs_port;
	status = ext4fs_wrap(superblock, EXT4FS_MOUNT_POINT,
	                    &superblock->root);
	if (status < 0) {
		vfs_super_free(superblock);
		goto fail_active;
	}
	*result = superblock;
	return VFS_OK;

fail_active:
	ext4fs_port.active = 0;
fail_journal:
	ext4_journal_stop(EXT4FS_MOUNT_POINT);
fail_mount:
	ext4_umount(EXT4FS_MOUNT_POINT);
fail_device:
	ext4_device_unregister(EXT4FS_DEVICE_NAME);
fail_buffer:
	free(ext4fs_port.interface.ph_bbuf);
	memset(&ext4fs_port, 0, sizeof(ext4fs_port));
	return status < 0 ? status : ext4fs_result(status);
}

static struct vfs_filesystem_type ext4fs_type = {
	.name = "ext4",
	.flags = VFS_FS_REQUIRES_DEVICE,
	.mount = ext4fs_mount,
};

void ext4fs_init(void)
{
	sleeplock_init(&ext4fs_lock, "ext4");
	if (vfs_register_filesystem(&ext4fs_type) != VFS_OK)
		PANIC("register ext4");
}
