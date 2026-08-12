#include <debug.h>
#include <file.h>
#include <mystring.h>
#include <spinlock.h>

static struct {
	struct spinlock lock;
	struct vfs_file files[NFILE];
} file_table;

void file_init(void)
{
	spinlock_init(&file_table.lock, "file table");
}

file_t file_alloc(void)
{
	file_t file;

	spinlock_acquire(&file_table.lock);
	for (file = file_table.files;
	     file != &file_table.files[NFILE]; file++) {
		if (!file->ref) {
			memset(file, 0, sizeof(*file));
			file->ref = 1;
			spinlock_release(&file_table.lock);
			return file;
		}
	}
	spinlock_release(&file_table.lock);
	return 0;
}

file_t file_dup(file_t file)
{
	spinlock_acquire(&file_table.lock);
	if (!file || file->ref < 1)
		PANIC("file_dup");
	file->ref++;
	spinlock_release(&file_table.lock);
	return file;
}

void file_close(file_t file)
{
	struct vfs_file released;

	spinlock_acquire(&file_table.lock);
	if (!file || file->ref < 1)
		PANIC("file_close");
	if (--file->ref) {
		spinlock_release(&file_table.lock);
		return;
	}
	released = *file;
	memset(file, 0, sizeof(*file));
	spinlock_release(&file_table.lock);

	if (released.operations && released.operations->release)
		released.operations->release(&released);
	vfs_path_put(&released.path);
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
