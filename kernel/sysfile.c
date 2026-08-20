#include <file.h>
#include <ktime.h>
#include <linux_uapi.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
#include <signal.h>
#include <syscall.h>
#include <vfs.h>
#include <vm.h>

extern int exec_linux(char *path, char **argv, char **envp);

static int copy_user_vector(uint64 user_vector, char **vector)
{
	uint64 user_string;
	int i;

	memset(vector, 0, MAXARG * sizeof(*vector));
	if (!user_vector)
		return 0;

	for (i = 0; i < MAXARG; i++) {
		if (fetch_addr_from_user(user_vector + i * sizeof(uint64),
		                         &user_string) < 0)
			return -LINUX_EFAULT;
		if (!user_string)
			return 0;

		vector[i] = alloc_pages(0, 0);
		if (!vector[i])
			return -LINUX_ENOMEM;
		if (fetch_str_from_user(user_string, vector[i], PGSIZE) < 0)
			return -LINUX_EFAULT;
	}
	return -LINUX_E2BIG;
}

static void free_user_vector(char **vector)
{
	int i;

	for (i = 0; i < MAXARG && vector[i]; i++)
		pfree(vector[i]);
}

static int linux_open_flags(int linux_flags, uint32 *vfs_flags)
{
	uint32 flags = 0;

	switch (linux_flags & LINUX_O_ACCMODE) {
	case LINUX_O_RDONLY:
		flags |= VFS_OPEN_READ;
		break;
	case LINUX_O_WRONLY:
		flags |= VFS_OPEN_WRITE;
		break;
	case LINUX_O_RDWR:
		flags |= VFS_OPEN_READ | VFS_OPEN_WRITE;
		break;
	default:
		return -1;
	}
	if (linux_flags & LINUX_O_CREAT)
		flags |= VFS_OPEN_CREATE;
	if (linux_flags & LINUX_O_EXCL)
		flags |= VFS_OPEN_EXCLUSIVE;
	if (linux_flags & LINUX_O_NOCTTY)
		flags |= VFS_OPEN_NOCTTY;
	if (linux_flags & LINUX_O_TRUNC)
		flags |= VFS_OPEN_TRUNCATE;
	if (linux_flags & LINUX_O_APPEND)
		flags |= VFS_OPEN_APPEND;
	if (linux_flags & LINUX_O_NONBLOCK)
		flags |= VFS_OPEN_NONBLOCK;
	if (linux_flags & LINUX_O_DIRECTORY)
		flags |= VFS_OPEN_DIRECTORY;
	*vfs_flags = flags;
	return 0;
}

static uint64 linux_encode_device(uint64 device)
{
	uint32 major = VFS_DEVICE_MAJOR(device);
	uint32 minor = VFS_DEVICE_MINOR(device);

	return (minor & 0xff) | ((major & 0xfff) << 8) |
	       ((uint64)(minor & ~0xff) << 12) |
	       ((uint64)(major & ~0xfff) << 32);
}

static void make_linux_stat(struct linux_stat *linux_stat,
			    struct vfs_stat *vfs_stat)
{
	memset(linux_stat, 0, sizeof(*linux_stat));
	linux_stat->dev = vfs_stat->dev;
	linux_stat->ino = vfs_stat->ino;
	linux_stat->uid = vfs_stat->uid;
	linux_stat->gid = vfs_stat->gid;
	linux_stat->nlink = vfs_stat->nlink;
	linux_stat->rdev = linux_encode_device(vfs_stat->rdev);
	linux_stat->size = vfs_stat->size;
	linux_stat->blksize = vfs_stat->block_size;
	linux_stat->blocks = vfs_stat->blocks;
	linux_stat->atime = vfs_stat->atime.seconds;
	linux_stat->atime_nsec = vfs_stat->atime.nanoseconds;
	linux_stat->mtime = vfs_stat->mtime.seconds;
	linux_stat->mtime_nsec = vfs_stat->mtime.nanoseconds;
	linux_stat->ctime = vfs_stat->ctime.seconds;
	linux_stat->ctime_nsec = vfs_stat->ctime.nanoseconds;
	linux_stat->mode = vfs_stat->mode & VFS_MODE_PERMISSIONS;
	if (vfs_stat->type == VFS_INODE_DIRECTORY)
		linux_stat->mode |= LINUX_S_IFDIR;
	else if (vfs_stat->type == VFS_INODE_CHAR_DEVICE)
		linux_stat->mode |= LINUX_S_IFCHR;
	else if (vfs_stat->type == VFS_INODE_BLOCK_DEVICE)
		linux_stat->mode |= LINUX_S_IFBLK;
	else if (vfs_stat->type == VFS_INODE_SYMLINK)
		linux_stat->mode |= LINUX_S_IFLNK;
	else if (vfs_stat->type == VFS_INODE_FIFO)
		linux_stat->mode |= LINUX_S_IFIFO;
	else if (vfs_stat->type == VFS_INODE_SOCKET)
		linux_stat->mode |= LINUX_S_IFSOCK;
	else
		linux_stat->mode |= LINUX_S_IFREG;
}

uint64 sys_linux_openat(void)
{
	char path[MAXPATH];
	uint32 flags;
	int dirfd, fd, linux_flags, mode, result;

	argint(0, &dirfd);
	argint(2, &linux_flags);
	argint(3, &mode);
	if (argstr(1, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	if (linux_open_flags(linux_flags, &flags) < 0)
		return -LINUX_EINVAL;
	mode &= VFS_MODE_PERMISSIONS;
	mode &= ~process_umask_get();
	result = vfs_open(path, flags, mode, &fd);
	if (result < 0)
		return linux_error(result);
	if (linux_flags & LINUX_O_CLOEXEC)
		vfs_set_fd_flags(fd, VFS_FD_CLOEXEC);
	return fd;
}

uint64 sys_linux_ftruncate(void)
{
	uint64 length;
	int64 signed_length;
	int fd, result;

	argint(0, &fd);
	argaddr(1, &length);
	signed_length = (int64)length;
	if (signed_length < 0)
		return -LINUX_EINVAL;
	result = vfs_ftruncate(fd, signed_length);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_close(void)
{
	int fd, result;

	argint(0, &fd);
	result = vfs_close(fd);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_pipe2(void)
{
	process_t process = cur_proc();
	uint64 address;
	uint32 file_flags = 0;
	uint8 fd_flags = 0;
	int descriptors[2], flags, result;

	argaddr(0, &address);
	argint(1, &flags);
	if (flags & ~(LINUX_O_CLOEXEC | LINUX_O_NONBLOCK))
		return -LINUX_EINVAL;
	if (flags & LINUX_O_NONBLOCK)
		file_flags |= VFS_OPEN_NONBLOCK;
	if (flags & LINUX_O_CLOEXEC)
		fd_flags |= VFS_FD_CLOEXEC;
	result = vfs_pipe(file_flags, fd_flags, descriptors);
	if (result < 0)
		return linux_error(result);
	if (copyout(process->pagetable, address, (char *)descriptors,
	            sizeof(descriptors)) < 0) {
		vfs_close(descriptors[0]);
		vfs_close(descriptors[1]);
		return -LINUX_EFAULT;
	}
	return 0;
}

uint64 sys_linux_read(void)
{
	uint64 address;
	int fd, length, result;

	argint(0, &fd);
	argaddr(1, &address);
	argint(2, &length);
	result = vfs_read(fd, address, length);
	if (result == VFS_ERR_INTR)
		return -SIGNAL_RESTART_SYS;
	return result < 0 ? linux_error(result) : result;
}

uint64 sys_linux_write(void)
{
	uint64 address;
	int fd, length, result;

	argint(0, &fd);
	argaddr(1, &address);
	argint(2, &length);
	result = vfs_write(fd, address, length);
	if (result == VFS_ERR_INTR)
		return -SIGNAL_RESTART_SYS;
	return result < 0 ? linux_error(result) : result;
}

uint64 sys_linux_pread64(void)
{
	struct vfs_file *file;
	uint64 address, count, offset;
	int fd;
	int64 result;

	argint(0, &fd);
	argaddr(1, &address);
	argaddr(2, &count);
	argaddr(3, &offset);
	if ((int64)offset < 0)
		return -LINUX_EINVAL;
	if (vfs_get_file_fd(fd, &file) < 0)
		return -LINUX_EBADF;
	result = vfs_file_pread(file, 1, address, count, offset);
	vfs_file_put(file);
	return result < 0 ? linux_error(result) : result;
}

uint64 sys_linux_pwrite64(void)
{
	struct vfs_file *file;
	uint64 address, count, offset;
	int fd;
	int64 result;

	argint(0, &fd);
	argaddr(1, &address);
	argaddr(2, &count);
	argaddr(3, &offset);
	if ((int64)offset < 0 || count > 0x7fffffff)
		return -LINUX_EINVAL;
	if (vfs_get_file_fd(fd, &file) < 0)
		return -LINUX_EBADF;
	result = vfs_file_pwrite(file, 1, address, count, offset, 0);
	vfs_file_put(file);
	return result < 0 ? linux_error(result) : result;
}

uint64 sys_linux_lseek(void)
{
	uint64 result;
	int fd, whence, status;
	int64 offset;

	argint(0, &fd);
	argaddr(1, (uint64 *)&offset);
	argint(2, &whence);
	status = vfs_seek(fd, offset, whence, &result);
	return status < 0 ? linux_error(status) : result;
}

uint64 sys_linux_fstat(void)
{
	struct linux_stat linux_stat;
	struct vfs_stat stat;
	process_t process = cur_proc();
	uint64 address;
	int fd, result;

	argint(0, &fd);
	argaddr(1, &address);
	result = vfs_stat_fd(fd, &stat);
	if (result < 0)
		return linux_error(result);
	make_linux_stat(&linux_stat, &stat);
	if (copyout(process->pagetable, address, (char *)&linux_stat,
	            sizeof(linux_stat)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

uint64 sys_linux_newfstatat(void)
{
	struct linux_stat linux_stat;
	struct vfs_stat stat;
	process_t process = cur_proc();
	char path[MAXPATH];
	uint64 address;
	int dirfd, flags, result;

	argint(0, &dirfd);
	argaddr(2, &address);
	argint(3, &flags);
	if (argstr(1, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	if (flags & ~(LINUX_AT_SYMLINK_NOFOLLOW))
		return -LINUX_EINVAL;
	result = vfs_stat_path(path,
	                       !(flags & LINUX_AT_SYMLINK_NOFOLLOW),
	                       &stat);
	if (result < 0)
		return linux_error(result);
	make_linux_stat(&linux_stat, &stat);
	if (copyout(process->pagetable, address, (char *)&linux_stat,
	            sizeof(linux_stat)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

struct linux_getdents_context {
	process_t process;
	uint64 address;
	int length;
	int used;
};

static int linux_emit_dirent(const struct vfs_dirent *dirent, void *opaque)
{
	struct {
		uint64 ino;
		int64 offset;
		uint16 reclen;
		uint8 type;
		char name[VFS_NAME_MAX + 1];
	} linux_dirent;
	struct linux_getdents_context *context = opaque;
	int name_length, record_length;

	name_length = strlen(dirent->name);
	record_length = (19 + name_length + 1 + 7) & ~7;
	if (context->used + record_length > context->length)
		return VFS_ERR_NOSPC;
	memset(&linux_dirent, 0, sizeof(linux_dirent));
	linux_dirent.ino = dirent->ino;
	linux_dirent.offset = dirent->next_offset;
	linux_dirent.reclen = record_length;
	linux_dirent.type = dirent->type;
	safe_strncpy(linux_dirent.name, dirent->name,
	             sizeof(linux_dirent.name));
	if (copyout(context->process->pagetable,
	            context->address + context->used,
	            (char *)&linux_dirent, record_length) < 0)
		return VFS_ERR_FAULT;
	context->used += record_length;
	return VFS_OK;
}

uint64 sys_linux_getdents64(void)
{
	struct linux_getdents_context context = {
		.process = cur_proc(),
	};
	int fd, result = 0;

	argint(0, &fd);
	argaddr(1, &context.address);
	argint(2, &context.length);
	if (context.length < 24)
		return -LINUX_EINVAL;
	while (context.length - context.used >= 24 &&
	       (result = vfs_next_dirent(fd, linux_emit_dirent,
	                                 &context)) > 0)
		;
	if (result == VFS_ERR_NOSPC)
		return context.used ? context.used : -LINUX_EINVAL;
	if (result < 0)
		return context.used ? context.used : linux_error(result);
	return context.used;
}

uint64 sys_linux_fcntl(void)
{
	uint32 file_flags;
	uint8 fd_flags;
	int command, fd, newfd, value, result;

	argint(0, &fd);
	argint(1, &command);
	argint(2, &value);
	if (command == LINUX_F_DUPFD ||
	    command == LINUX_F_DUPFD_CLOEXEC) {
		result = vfs_dup(fd, value,
		                 command == LINUX_F_DUPFD_CLOEXEC ?
		                 VFS_FD_CLOEXEC : 0, &newfd);
		return result < 0 ? linux_error(result) : newfd;
	}
	if (command == LINUX_F_GETFD) {
		result = vfs_get_fd_flags(fd, &fd_flags);
		return result < 0 ? linux_error(result) : fd_flags;
	}
	if (command == LINUX_F_SETFD) {
		result = vfs_set_fd_flags(fd, value & LINUX_FD_CLOEXEC);
		return result < 0 ? linux_error(result) : 0;
	}
	if (command == LINUX_F_GETFL) {
		result = vfs_get_file_flags(fd, &file_flags);
		if (result < 0)
			return linux_error(result);
		if ((file_flags & (VFS_OPEN_READ | VFS_OPEN_WRITE)) ==
		    (VFS_OPEN_READ | VFS_OPEN_WRITE))
			value = LINUX_O_RDWR;
		else if (file_flags & VFS_OPEN_WRITE)
			value = LINUX_O_WRONLY;
		else
			value = LINUX_O_RDONLY;
		if (file_flags & VFS_OPEN_APPEND)
			value |= LINUX_O_APPEND;
		if (file_flags & VFS_OPEN_NONBLOCK)
			value |= LINUX_O_NONBLOCK;
		return value;
	}
	if (command == LINUX_F_SETFL) {
		file_flags = 0;
		if (value & LINUX_O_APPEND)
			file_flags |= VFS_OPEN_APPEND;
		if (value & LINUX_O_NONBLOCK)
			file_flags |= VFS_OPEN_NONBLOCK;
		result = vfs_set_file_flags(fd, file_flags);
		return result < 0 ? linux_error(result) : 0;
	}
	return -LINUX_EINVAL;
}

uint64 sys_linux_mkdirat(void)
{
	char path[MAXPATH];
	int dirfd, mode, result;

	argint(0, &dirfd);
	argint(2, &mode);
	if (argstr(1, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	mode &= VFS_MODE_PERMISSIONS;
	mode &= ~process_umask_get();
	result = vfs_mkdir(path, mode);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_unlinkat(void)
{
	char path[MAXPATH];
	int dirfd, flags, result;

	argint(0, &dirfd);
	argint(2, &flags);
	if (argstr(1, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	if (flags & ~LINUX_AT_REMOVEDIR)
		return -LINUX_EINVAL;
	result = vfs_unlink(path, flags & LINUX_AT_REMOVEDIR);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_symlinkat(void)
{
	char target[MAXPATH], path[MAXPATH];
	int dirfd, result;

	argint(1, &dirfd);
	if (argstr(0, target, sizeof(target)) < 0 ||
	    argstr(2, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	result = vfs_symlink(target, path);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_linkat(void)
{
	char old_path[MAXPATH], new_path[MAXPATH];
	int old_dirfd, new_dirfd, flags, result;

	argint(0, &old_dirfd);
	argint(2, &new_dirfd);
	argint(4, &flags);
	if (argstr(1, old_path, sizeof(old_path)) < 0 ||
	    argstr(3, new_path, sizeof(new_path)) < 0)
		return -LINUX_EFAULT;
	if ((old_path[0] != '/' && old_dirfd != LINUX_AT_FDCWD) ||
	    (new_path[0] != '/' && new_dirfd != LINUX_AT_FDCWD))
		return -LINUX_EBADF;
	if (flags & ~LINUX_AT_SYMLINK_FOLLOW)
		return -LINUX_EINVAL;
	result = vfs_link(old_path, new_path,
	                  flags & LINUX_AT_SYMLINK_FOLLOW);
	return result < 0 ? linux_error(result) : 0;
}

static uint64 linux_rename(uint32 flags)
{
	char old_path[MAXPATH], new_path[MAXPATH];
	uint32 vfs_flags = 0;
	int old_dirfd, new_dirfd, result;

	argint(0, &old_dirfd);
	argint(2, &new_dirfd);
	if (argstr(1, old_path, sizeof(old_path)) < 0 ||
	    argstr(3, new_path, sizeof(new_path)) < 0)
		return -LINUX_EFAULT;
	if ((old_path[0] != '/' && old_dirfd != LINUX_AT_FDCWD) ||
	    (new_path[0] != '/' && new_dirfd != LINUX_AT_FDCWD))
		return -LINUX_EBADF;
	if (flags & ~LINUX_RENAME_NOREPLACE)
		return -LINUX_EINVAL;
	if (flags & LINUX_RENAME_NOREPLACE)
		vfs_flags |= VFS_RENAME_NOREPLACE;
	result = vfs_rename(old_path, new_path, vfs_flags);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_renameat2(void)
{
	int flags;

	argint(4, &flags);
	return linux_rename(flags);
}

uint64 sys_linux_readlinkat(void)
{
	process_t process = cur_proc();
	char path[MAXPATH];
	char *target;
	uint64 address, size;
	int dirfd, result;

	argint(0, &dirfd);
	argaddr(2, &address);
	argaddr(3, &size);
	if (argstr(1, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	if (!size)
		return -LINUX_EINVAL;
	if (size > VFS_PATH_MAX)
		size = VFS_PATH_MAX;
	target = palloc();
	result = vfs_readlink(path, target, size);
	if (result >= 0 &&
	    copyout(process->pagetable, address, target, result) < 0)
		result = -LINUX_EFAULT;
	else if (result < 0)
		result = linux_error(result);
	pfree(target);
	return result;
}

uint64 sys_linux_sync(void)
{
	(void)vfs_sync();
	return 0;
}

uint64 sys_linux_fsync(void)
{
	int fd, result;

	argint(0, &fd);
	result = vfs_fsync(fd);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_fdatasync(void)
{
	return sys_linux_fsync();
}

uint64 sys_linux_faccessat(void)
{
	char path[MAXPATH];
	uint32 access = 0;
	int dirfd, mode, result;

	argint(0, &dirfd);
	argint(2, &mode);
	if (argstr(1, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	if (mode & ~(LINUX_R_OK | LINUX_W_OK | LINUX_X_OK))
		return -LINUX_EINVAL;
	if (mode & LINUX_R_OK)
		access |= VFS_ACCESS_READ;
	if (mode & LINUX_W_OK)
		access |= VFS_ACCESS_WRITE;
	if (mode & LINUX_X_OK)
		access |= VFS_ACCESS_EXEC;
	result = vfs_access(path, access, 0);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_utimensat(void)
{
	struct linux_timespec linux_times[2];
	struct vfs_timespec times[2], now;
	process_t process = cur_proc();
	char path[MAXPATH];
	uint64 address, path_address;
	uint32 mask = 0;
	int owner_only = 0, use_fd = 0;
	int dirfd, flags, i, result;

	argint(0, &dirfd);
	argaddr(1, &path_address);
	argaddr(2, &address);
	argint(3, &flags);
	if (flags & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH))
		return -LINUX_EINVAL;
	if (!address) {
		if (vfs_current_time(&now) < 0)
			return -LINUX_EIO;
		times[0] = now;
		times[1] = now;
		mask = VFS_TIME_ATIME | VFS_TIME_MTIME;
	} else {
		if (copyin(process->pagetable, (char *)linux_times, address,
		           sizeof(linux_times)) < 0)
			return -LINUX_EFAULT;
		owner_only = linux_times[0].nanoseconds != LINUX_UTIME_NOW ||
			     linux_times[1].nanoseconds != LINUX_UTIME_NOW;
		for (i = 0; i < 2; i++) {
			if (linux_times[i].nanoseconds == LINUX_UTIME_OMIT)
				continue;
			if (linux_times[i].nanoseconds == LINUX_UTIME_NOW) {
				if (vfs_current_time(&times[i]) < 0)
					return -LINUX_EIO;
			} else {
				if (linux_times[i].nanoseconds < 0 ||
				    linux_times[i].nanoseconds >= NSEC_PER_SEC)
					return -LINUX_EINVAL;
				times[i].seconds = linux_times[i].seconds;
				times[i].nanoseconds =
					linux_times[i].nanoseconds;
			}
			mask |= i ? VFS_TIME_MTIME : VFS_TIME_ATIME;
		}
		if (!mask)
			return 0;
	}
	if (!path_address) {
		if (flags)
			return -LINUX_EINVAL;
		use_fd = 1;
	} else {
		if (fetch_str_from_user(path_address, path, sizeof(path)) < 0)
			return -LINUX_EFAULT;
		if (!path[0]) {
			if (!(flags & LINUX_AT_EMPTY_PATH))
				return -LINUX_ENOENT;
			use_fd = 1;
		} else if (path[0] != '/' && dirfd != LINUX_AT_FDCWD) {
			return -LINUX_EBADF;
		}
	}
	if (use_fd)
		result = vfs_set_times_fd(dirfd, times, mask, owner_only);
	else
		result = vfs_set_times_path(path,
				!(flags & LINUX_AT_SYMLINK_NOFOLLOW),
				times, mask, owner_only);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_chdir(void)
{
	char path[MAXPATH];
	int result;

	if (argstr(0, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	result = vfs_chdir(path);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_getcwd(void)
{
	process_t process = cur_proc();
	char path[VFS_PATH_MAX];
	uint64 address, size;
	int length;

	argaddr(0, &address);
	argaddr(1, &size);
	if (!size)
		return -LINUX_EINVAL;
	if (size > sizeof(path))
		size = sizeof(path);
	length = vfs_getcwd(path, size);
	if (length == VFS_ERR_NOSPC)
		return -LINUX_ERANGE;
	if (length < 0)
		return linux_error(length);
	if (copyout(process->pagetable, address, path, length) < 0)
		return -LINUX_EFAULT;
	return length;
}

uint64 sys_linux_dup(void)
{
	int oldfd, fd, result;

	argint(0, &oldfd);
	result = vfs_dup(oldfd, 0, 0, &fd);
	return result < 0 ? linux_error(result) : fd;
}

uint64 sys_linux_dup3(void)
{
	int oldfd, newfd, flags, result;

	argint(0, &oldfd);
	argint(1, &newfd);
	argint(2, &flags);
	if (flags & ~LINUX_O_CLOEXEC)
		return -LINUX_EINVAL;
	result = vfs_dup_to(oldfd, newfd,
	                    flags & LINUX_O_CLOEXEC ? VFS_FD_CLOEXEC : 0);
	return result < 0 ? linux_error(result) : newfd;
}

uint64 sys_linux_ioctl(void)
{
	uint64 address, request;
	int fd;
	int64 result;

	argint(0, &fd);
	argaddr(1, &request);
	argaddr(2, &address);
	result = vfs_ioctl(fd, (uint32)request, address);
	if (result == VFS_ERR_INTR)
		return -SIGNAL_RESTART_SYS;
	return result < 0 ? linux_error(result) : result;
}

static uint64 linux_positioned_iov(int write_operation, int version_two)
{
	struct vfs_iovec *iovecs;
	file_t file;
	uint64 address, offset, offset_high, offset_low;
	unsigned int order;
	int count, error, fd, flags = 0;
	uint32 vfs_flags = 0;
	int64 result;

	argint(0, &fd);
	argaddr(1, &address);
	argint(2, &count);
	argaddr(3, &offset_low);
	argaddr(4, &offset_high);
	offset = (uint32)offset_low | ((uint64)(uint32)offset_high << 32);
	if (version_two)
		argint(5, &flags);
	if ((!write_operation && flags) ||
	    (write_operation && flags & ~LINUX_RWF_NOAPPEND))
		return -LINUX_EOPNOTSUPP;
	if (flags & LINUX_RWF_NOAPPEND)
		vfs_flags |= VFS_WRITE_NOAPPEND;
	error = copy_user_iov(address, count, &iovecs, &order);
	if (error < 0)
		return error;
	if (version_two && offset == (uint64)-1) {
		result = write_operation ?
			vfs_writev(fd, 1, iovecs, count, vfs_flags) :
			vfs_readv(fd, 1, iovecs, count);
		goto translate;
	}
	if ((int64)offset < 0) {
		result = -LINUX_EINVAL;
		goto out_iov;
	}
	if (vfs_get_file_fd(fd, &file) < 0) {
		result = -LINUX_EBADF;
		goto out_iov;
	}
	result = write_operation ?
		vfs_file_pwritev(file, 1, iovecs, count, offset, vfs_flags) :
		vfs_file_preadv(file, 1, iovecs, count, offset);
	vfs_file_put(file);
translate:
	if (result == VFS_ERR_INTR)
		result = -SIGNAL_RESTART_SYS;
	else if (result < 0)
		result = linux_error(result);
out_iov:
	if (iovecs)
		free_pages(iovecs, order);
	return result;
}

uint64 sys_linux_preadv(void)
{
	return linux_positioned_iov(0, 0);
}

uint64 sys_linux_pwritev(void)
{
	return linux_positioned_iov(1, 0);
}

uint64 sys_linux_preadv2(void)
{
	return linux_positioned_iov(0, 1);
}

uint64 sys_linux_pwritev2(void)
{
	return linux_positioned_iov(1, 1);
}

uint64 sys_linux_readv(void)
{
	struct vfs_iovec *iovecs;
	uint64 address;
	unsigned int order;
	int count, error, fd;
	int64 result;

	argint(0, &fd);
	argaddr(1, &address);
	argint(2, &count);
	error = copy_user_iov(address, count, &iovecs, &order);
	if (error < 0)
		return error;
	result = vfs_readv(fd, 1, iovecs, count);
	if (iovecs)
		free_pages(iovecs, order);
	if (result == VFS_ERR_INTR)
		return -SIGNAL_RESTART_SYS;
	return result < 0 ? linux_error(result) : result;
}

uint64 sys_linux_writev(void)
{
	struct vfs_iovec *iovecs;
	uint64 address;
	unsigned int order;
	int count, error, fd;
	int64 result;

	argint(0, &fd);
	argaddr(1, &address);
	argint(2, &count);
	error = copy_user_iov(address, count, &iovecs, &order);
	if (error < 0)
		return error;
	result = vfs_writev(fd, 1, iovecs, count, 0);
	if (iovecs)
		free_pages(iovecs, order);
	if (result == VFS_ERR_INTR)
		return -SIGNAL_RESTART_SYS;
	return result < 0 ? linux_error(result) : result;
}

uint64 sys_linux_sendfile(void)
{
	process_t process = cur_proc();
	file_t input = 0, output = 0;
	uint64 buffer_address, count, offset_address;
	uint64 chunk, offset, total = 0;
	char *buffer;
	int input_fd, output_fd, position_locked = 0;
	int64 read_result = 0, write_result;

	argint(0, &output_fd);
	argint(1, &input_fd);
	argaddr(2, &offset_address);
	argaddr(3, &count);
	if (count > 0x7fffffff)
		count = 0x7fffffff;
	if (vfs_get_file_fd(input_fd, &input) < 0 ||
	    vfs_get_file_fd(output_fd, &output) < 0) {
		read_result = -LINUX_EBADF;
		goto out;
	}
	if (offset_address) {
		if (copyin(process->pagetable, (char *)&offset,
		           offset_address, sizeof(offset)) < 0) {
			read_result = -LINUX_EFAULT;
			goto out;
		}
		if ((int64)offset < 0) {
			read_result = -LINUX_EINVAL;
			goto out;
		}
	} else {
		sleeplock_acquire(&input->position_lock);
		position_locked = 1;
		offset = input->position;
	}
	if (output->flags & VFS_OPEN_APPEND) {
		read_result = -LINUX_EINVAL;
		goto out;
	}
	buffer = alloc_pages(0, 0);
	if (!buffer) {
		read_result = -LINUX_ENOMEM;
		goto out;
	}
	buffer_address = (uint64)buffer;
	while (total < count) {
		chunk = count - total;
		if (chunk > PGSIZE)
			chunk = PGSIZE;
		read_result = vfs_file_pread(input, 0, buffer_address,
					chunk, offset);
		if (read_result <= 0)
			break;
		write_result = vfs_file_write_current(output, 0,
					      buffer_address, read_result);
		if (write_result <= 0) {
			read_result = write_result;
			break;
		}
		offset += write_result;
		total += write_result;
		if (write_result != read_result)
			break;
	}
	free_pages(buffer, 0);
	if (!offset_address)
		input->position = offset;
	else if (copyout(process->pagetable, offset_address,
	                 (char *)&offset, sizeof(offset)) < 0) {
		read_result = -LINUX_EFAULT;
		goto out;
	}
	if (total)
		read_result = total;
	else if (read_result == VFS_ERR_INTR)
		read_result = -SIGNAL_RESTART_SYS;
	else if (read_result < 0)
		read_result = linux_error(read_result);
out:
	if (position_locked)
		sleeplock_release(&input->position_lock);
	if (output)
		vfs_file_put(output);
	if (input)
		vfs_file_put(input);
	return read_result;
}

uint64 sys_linux_execve(void)
{
	char path[MAXPATH], *argv[MAXARG], *envp[MAXARG];
	uint64 user_argv, user_envp;
	int env_error, argv_error, ret;

	argaddr(1, &user_argv);
	argaddr(2, &user_envp);
	if (argstr(0, path, MAXPATH) < 0)
		return -LINUX_EFAULT;

	memset(argv, 0, sizeof(argv));
	memset(envp, 0, sizeof(envp));
	argv_error = copy_user_vector(user_argv, argv);
	env_error = argv_error < 0 ? 0 :
		copy_user_vector(user_envp, envp);
	if (argv_error < 0 || env_error < 0) {
		ret = argv_error < 0 ? argv_error : env_error;
		goto out;
	}

	ret = exec_linux(path, argv, envp);
	if (ret >= 0)
		vfs_close_on_exec();
out:
	free_user_vector(argv);
	free_user_vector(envp);
	return ret < 0 ? ret : 0;
}
