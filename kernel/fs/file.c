/*
 * Legacy fixed-size kernel file table.
 *
 * This table is used by early kernel code that still consumes file_t handles.
 * It owns references to VFS files and serializes allocation and release with
 * file_table.lock; normal process descriptor handling lives in vfs.c.
 */
#include <debug.h>
#include <file.h>
#include <mystring.h>
#include <spinlock.h>

static struct {
	struct spinlock lock;
	struct vfs_file files[NFILE];
} file_table;

/* Initialize the table before any compatibility-handle allocation. */
void file_init(void)
{
	spinlock_init(&file_table.lock, "file table");
}

/* Reserve a zeroed slot while holding file_table.lock. */
file_t file_alloc(void)
{
	file_t file;

	spinlock_acquire(&file_table.lock);
	for (file = file_table.files;
	     file != &file_table.files[NFILE]; file++) {
		if (!file->ref) {
			memset(file, 0, sizeof(*file));
			file->ref = 1;
			sleeplock_init(&file->position_lock, "file position");
			spinlock_release(&file_table.lock);
			return file;
		}
	}
	spinlock_release(&file_table.lock);
	return 0;
}

/* Increment a valid fixed-table reference under file_table.lock. */
file_t file_dup(file_t file)
{
	spinlock_acquire(&file_table.lock);
	if (!file || file->ref < 1)
		PANIC("file_dup");
	file->ref++;
	if (file->inode_access)
		file->access_ref++;
	spinlock_release(&file_table.lock);
	return file;
}

/* Release the VFS file only when the final compatibility reference is gone. */
static void file_release(file_t file, int access)
{
	struct vfs_file released;

	spinlock_acquire(&file_table.lock);
	if (!file || file->ref < 1)
		PANIC("file_close");
	if (access && file->inode_access) {
		if (!file->access_ref)
			PANIC("file access reference");
		if (!--file->access_ref)
			vfs_file_release_inode_access(file);
	}
	if (--file->ref) {
		spinlock_release(&file_table.lock);
		return;
	}
	if (file->access_ref || file->inode_access)
		PANIC("release accessed file");
	released = *file;
	memset(file, 0, sizeof(*file));
	spinlock_release(&file_table.lock);

	if (released.operations && released.operations->release)
		released.operations->release(&released);
	vfs_path_put(&released.path);
}

/* Release a fixed-table reference and its VFS access on the final close. */
void file_close(file_t file)
{
	file_release(file, 1);
}

/* Take a table reference without acquiring an inode access reference. */
file_t file_hold(file_t file)
{
	spinlock_acquire(&file_table.lock);
	if (!file || file->ref < 1)
		PANIC("file_hold");
	file->ref++;
	spinlock_release(&file_table.lock);
	return file;
}

/* Drop a table reference retained by file_hold(). */
void file_unhold(file_t file)
{
	file_release(file, 0);
}

int64 file_read(file_t file, int user_destination, uint64 destination,
		uint64 count, uint64 *position)
{
	if (!(file->flags & VFS_OPEN_READ))
		return VFS_ERR_BADF;
	if (!file->operations || !file->operations->read)
		return VFS_ERR_INVAL;
	return file->operations->read(file, user_destination, destination,
	                              count, position);
}

int64 file_write(file_t file, int user_source, uint64 source,
		 uint64 count, uint64 *position)
{
	if (!(file->flags & VFS_OPEN_WRITE))
		return VFS_ERR_BADF;
	if (!file->operations || !file->operations->write)
		return VFS_ERR_INVAL;
	return file->operations->write(file, user_source, source, count,
	                               position);
}

int64 file_ioctl(file_t file, uint64 request, uint64 argument)
{
	if (!file->operations || !file->operations->ioctl)
		return VFS_ERR_NOTTY;
	return file->operations->ioctl(file, request, argument);
}
