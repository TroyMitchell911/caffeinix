#include <debug.h>
#include <file.h>
#include <linux_uapi.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
#include <signal.h>
#include <spinlock.h>
#include <vfs.h>
#include <vm.h>
#include <wait.h>

#define PIPE_CAPACITY PGSIZE
#define PIPE_BUF      PGSIZE

static uint64 pipe_next_inode = 1;

struct vfs_pipe {
	struct spinlock lock;
	struct wait_queue read_wait;
	struct wait_queue write_wait;
	char *buffer;
	uint32 read_position;
	uint32 write_position;
	uint32 used;
	uint32 readers;
	uint32 writers;
	uint64 inode;
	uint32 uid;
	uint32 gid;
	uint8 read_busy;
};

static int pipe_prefault_output(int user_destination,
				const struct vfs_iovec *iovecs,
				uint32 count, uint32 limit)
{
	process_t process;
	uint32 chunk, index, total = 0;

	if (!user_destination)
		return VFS_OK;
	process = cur_proc();
	if (!process)
		return VFS_ERR_FAULT;
	for (index = 0; index < count && total < limit; index++) {
		chunk = iovecs[index].length;
		if (chunk > limit - total)
			chunk = limit - total;
		if (vm_prefault_user_write(process->pagetable,
					   iovecs[index].base, chunk) < 0)
			return VFS_ERR_FAULT;
		total += chunk;
	}
	return VFS_OK;
}

static uint32 pipe_copy_out(struct vfs_pipe *pipe, int user_destination,
			    uint64 destination, uint32 count, int *error)
{
	process_t process = user_destination ? cur_proc() : 0;
	uint32 copied = 0;

	*error = VFS_OK;
	while (copied < count && pipe->used) {
		uint32 chunk = count - copied;
		uint32 contiguous = PIPE_CAPACITY - pipe->read_position;

		if (chunk > pipe->used)
			chunk = pipe->used;
		if (chunk > contiguous)
			chunk = contiguous;
		if (user_destination &&
		    chunk > PGSIZE - ((destination + copied) % PGSIZE))
			chunk = PGSIZE - ((destination + copied) % PGSIZE);
		if ((user_destination &&
		     (!process || copyout_nofault(
			process->pagetable, destination + copied,
			pipe->buffer + pipe->read_position, chunk) < 0))) {
			*error = VFS_ERR_FAULT;
			break;
		}
		if (!user_destination)
			memmove((void *)(destination + copied),
				pipe->buffer + pipe->read_position, chunk);
		pipe->read_position =
			(pipe->read_position + chunk) % PIPE_CAPACITY;
		pipe->used -= chunk;
		copied += chunk;
	}
	return copied;
}

static int64 pipe_readv(struct vfs_file *file, int user_destination,
			const struct vfs_iovec *iovecs, uint32 count)
{
	struct vfs_pipe *pipe = file ? file->private : 0;
	uint64 requested = 0, total = 0;
	uint32 available, copied, index;
	int error = VFS_OK;

	if (!pipe)
		return VFS_ERR_BADF;
	for (index = 0; index < count; index++) {
		if (iovecs[index].length > 0x7fffffff - requested)
			return VFS_ERR_INVAL;
		requested += iovecs[index].length;
	}
	if (!requested)
		return 0;
	spinlock_acquire(&pipe->lock);
	while (!pipe->used || pipe->read_busy) {
		if (!pipe->used && !pipe->writers && !pipe->read_busy) {
			spinlock_release(&pipe->lock);
			return 0;
		}
		if (file->flags & VFS_OPEN_NONBLOCK) {
			spinlock_release(&pipe->lock);
			return VFS_ERR_AGAIN;
		}
		if (wait_queue_sleep_interruptible(&pipe->read_wait,
		                                   &pipe->lock) ==
		    WAIT_QUEUE_INTERRUPTED) {
			spinlock_release(&pipe->lock);
			return VFS_ERR_INTR;
		}
	}
	pipe->read_busy = 1;
	available = pipe->used;
	if (available > requested)
		available = requested;
	spinlock_release(&pipe->lock);
	error = pipe_prefault_output(user_destination, iovecs, count,
				     available);
	spinlock_acquire(&pipe->lock);
	if (error < 0)
		goto out;
	for (index = 0; index < count && total < available; index++) {
		uint32 length = iovecs[index].length;

		if (length > available - total)
			length = available - total;
		copied = pipe_copy_out(pipe, user_destination,
		                       iovecs[index].base,
		                       length, &error);
		total += copied;
		if (error < 0 || copied != length)
			break;
	}
out:
	pipe->read_busy = 0;
	wait_queue_wake_all(&pipe->read_wait);
	if (total)
		wait_queue_wake_all(&pipe->write_wait);
	spinlock_release(&pipe->lock);
	if (total)
		vfs_poll_notify();
	return total ? (int64)total : error;
}

static int64 pipe_read(struct vfs_file *file, int user_destination,
		       uint64 destination, uint64 count, uint64 *position)
{
	struct vfs_iovec iovec = {
		.base = destination,
		.length = count,
	};

	(void)position;
	return pipe_readv(file, user_destination, &iovec, 1);
}

static void pipe_copy_in(struct vfs_pipe *pipe, const char *source,
			 uint32 count)
{
	uint32 first = count;

	if (first > PIPE_CAPACITY - pipe->write_position)
		first = PIPE_CAPACITY - pipe->write_position;
	memmove(pipe->buffer + pipe->write_position, source, first);
	if (first < count)
		memmove(pipe->buffer, source + first, count - first);
	pipe->write_position =
		(pipe->write_position + count) % PIPE_CAPACITY;
	pipe->used += count;
}

static int64 pipe_write_staged(struct vfs_file *file, const char *source,
			       uint32 count, int atomic)
{
	struct vfs_pipe *pipe = file ? file->private : 0;
	uint32 available, chunk, total = 0;
	int broken = 0;
	int64 result = 0;

	if (!pipe)
		return VFS_ERR_BADF;
	spinlock_acquire(&pipe->lock);
	while (total < count) {
		if (!pipe->readers) {
			broken = 1;
			result = total ? (int64)total : VFS_ERR_PIPE;
			break;
		}
		available = PIPE_CAPACITY - pipe->used;
		if ((atomic && available < count) || !available) {
			if (file->flags & VFS_OPEN_NONBLOCK) {
				result = total ? (int64)total : VFS_ERR_AGAIN;
				break;
			}
			if (wait_queue_sleep_interruptible(&pipe->write_wait,
			                                   &pipe->lock) ==
			    WAIT_QUEUE_INTERRUPTED) {
				result = total ? (int64)total : VFS_ERR_INTR;
				break;
			}
			continue;
		}
		chunk = count - total;
		if (chunk > available)
			chunk = available;
		pipe_copy_in(pipe, source + total, chunk);
		total += chunk;
		result = total;
		wait_queue_wake_all(&pipe->read_wait);
		vfs_poll_notify();
	}
	spinlock_release(&pipe->lock);
	if (broken)
		signal_raise_current(LINUX_SIGPIPE, LINUX_SI_KERNEL);
	return result;
}

static int64 pipe_write_user(struct vfs_file *file, int user_source,
			     uint64 source, uint64 count, int atomic)
{
	char *staging;
	uint64 total = 0;
	int64 result;

	if (!count)
		return 0;
	if (count > 0x7fffffff)
		return VFS_ERR_INVAL;
	staging = palloc();
	if (!staging)
		return VFS_ERR_NOMEM;
	while (total < count) {
		uint32 chunk = count - total > PGSIZE ?
			       PGSIZE : count - total;

		if (either_copyin(staging, user_source, source + total,
		                  chunk) < 0) {
			result = total ? total : VFS_ERR_FAULT;
			goto out;
		}
		result = pipe_write_staged(file, staging, chunk, atomic);
		if (result < 0) {
			result = total ? (int64)total : result;
			goto out;
		}
		total += result;
		if ((uint32)result != chunk) {
			result = total;
			goto out;
		}
	}
	result = total;
out:
	pfree(staging);
	return result;
}

static int64 pipe_write(struct vfs_file *file, int user_source,
			uint64 source, uint64 count, uint64 *position)
{
	(void)position;
	return pipe_write_user(file, user_source, source, count,
			       count <= PIPE_BUF);
}

static int64 pipe_writev(struct vfs_file *file, int user_source,
			 const struct vfs_iovec *iovecs, uint32 count)
{
	char *staging;
	uint64 total = 0;
	uint32 index;
	int64 result;

	for (index = 0; index < count; index++) {
		if (iovecs[index].length > 0x7fffffff - total)
			return VFS_ERR_INVAL;
		total += iovecs[index].length;
	}
	if (!total)
		return 0;
	if (total <= PIPE_BUF) {
		uint64 copied = 0;

		staging = palloc();
		if (!staging)
			return VFS_ERR_NOMEM;
		for (index = 0; index < count; index++) {
			if (either_copyin(staging + copied, user_source,
			                  iovecs[index].base,
			                  iovecs[index].length) < 0) {
				pfree(staging);
				return VFS_ERR_FAULT;
			}
			copied += iovecs[index].length;
		}
		result = pipe_write_staged(file, staging, total, 1);
		pfree(staging);
		return result;
	}
	total = 0;
	for (index = 0; index < count; index++) {
		result = pipe_write_user(file, user_source,
					 iovecs[index].base,
					 iovecs[index].length, 0);
		if (result < 0)
			return total ? (int64)total : result;
		total += result;
		if ((uint64)result != iovecs[index].length)
			break;
	}
	return total;
}

static uint32 pipe_poll(struct vfs_file *file, uint32 events)
{
	struct vfs_pipe *pipe = file ? file->private : 0;
	uint32 ready = 0;

	if (!pipe)
		return VFS_POLL_ERR;
	spinlock_acquire(&pipe->lock);
	if (file->flags & VFS_OPEN_READ) {
		if ((events & VFS_POLL_IN) && pipe->used)
			ready |= VFS_POLL_IN;
		if (!pipe->writers)
			ready |= VFS_POLL_HUP;
	} else {
		if (!pipe->readers)
			ready |= VFS_POLL_ERR;
		else if ((events & VFS_POLL_OUT) &&
		         pipe->used < PIPE_CAPACITY)
			ready |= VFS_POLL_OUT;
	}
	spinlock_release(&pipe->lock);
	return ready;
}

static int pipe_getattr(struct vfs_file *file, struct vfs_stat *stat)
{
	struct vfs_pipe *pipe = file ? file->private : 0;

	if (!pipe || !stat)
		return VFS_ERR_INVAL;
	memset(stat, 0, sizeof(*stat));
	stat->ino = pipe->inode;
	stat->type = VFS_INODE_FIFO;
	stat->mode = 0600;
	stat->uid = pipe->uid;
	stat->gid = pipe->gid;
	stat->nlink = 1;
	stat->block_size = PGSIZE;
	return VFS_OK;
}

static void pipe_release(struct vfs_file *file)
{
	struct vfs_pipe *pipe = file ? file->private : 0;
	int destroy;

	if (!pipe)
		return;
	spinlock_acquire(&pipe->lock);
	if (file->flags & VFS_OPEN_READ) {
		if (!pipe->readers)
			PANIC("pipe reader underflow");
		pipe->readers--;
		wait_queue_wake_all(&pipe->write_wait);
	}
	if (file->flags & VFS_OPEN_WRITE) {
		if (!pipe->writers)
			PANIC("pipe writer underflow");
		pipe->writers--;
		wait_queue_wake_all(&pipe->read_wait);
	}
	destroy = !pipe->readers && !pipe->writers;
	spinlock_release(&pipe->lock);
	vfs_poll_notify();
	if (!destroy)
		return;
	if (!wait_queue_empty(&pipe->read_wait) ||
	    !wait_queue_empty(&pipe->write_wait))
		PANIC("release pipe with waiters");
	pfree(pipe->buffer);
	free(pipe);
}

static const struct vfs_file_operations pipe_operations = {
	.release = pipe_release,
	.read = pipe_read,
	.readv = pipe_readv,
	.write = pipe_write,
	.writev = pipe_writev,
	.getattr = pipe_getattr,
	.poll = pipe_poll,
};

int vfs_pipe(uint32 file_flags, uint8 fd_flags, int descriptors[2])
{
	struct process_credentials credentials;
	struct vfs_pipe *pipe;
	file_t read_file, write_file;
	int result;

	if (!descriptors || file_flags & ~VFS_OPEN_NONBLOCK)
		return VFS_ERR_INVAL;
	pipe = malloc(sizeof(*pipe));
	if (!pipe)
		return VFS_ERR_NOMEM;
	memset(pipe, 0, sizeof(*pipe));
	process_credentials_get(&credentials);
	pipe->inode = __atomic_fetch_add(&pipe_next_inode, 1,
					 __ATOMIC_RELAXED);
	pipe->uid = credentials.fsuid;
	pipe->gid = credentials.fsgid;
	pipe->buffer = palloc();
	if (!pipe->buffer) {
		free(pipe);
		return VFS_ERR_NOMEM;
	}
	read_file = file_alloc();
	write_file = file_alloc();
	if (!read_file || !write_file) {
		if (read_file)
			file_close(read_file);
		if (write_file)
			file_close(write_file);
		pfree(pipe->buffer);
		free(pipe);
		return VFS_ERR_MFILE;
	}
	spinlock_init(&pipe->lock, "pipe");
	wait_queue_init(&pipe->read_wait, "pipe read");
	wait_queue_init(&pipe->write_wait, "pipe write");
	pipe->readers = 1;
	pipe->writers = 1;
	read_file->operations = &pipe_operations;
	read_file->flags = VFS_OPEN_READ | file_flags;
	read_file->private = pipe;
	write_file->operations = &pipe_operations;
	write_file->flags = VFS_OPEN_WRITE | file_flags;
	write_file->private = pipe;

	result = vfs_install_file(read_file, fd_flags, &descriptors[0]);
	if (result < 0) {
		file_close(read_file);
		file_close(write_file);
		return result;
	}
	result = vfs_install_file(write_file, fd_flags, &descriptors[1]);
	if (result < 0) {
		vfs_close(descriptors[0]);
		file_close(write_file);
		return result;
	}
	return VFS_OK;
}
