#include <file.h>
#include <linux_uapi.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
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
			return -1;
		if (!user_string)
			return 0;

		vector[i] = palloc();
		if (!vector[i])
			return -1;
		if (fetch_str_from_user(user_string, vector[i], PGSIZE) < 0)
			return -1;
	}
	return -1;
}

static void free_user_vector(char **vector)
{
	int i;

	for (i = 0; i < MAXARG && vector[i]; i++)
		pfree(vector[i]);
}

static uint64 linux_error(int result)
{
	switch (result) {
	case VFS_ERR_PERM:
		return -LINUX_EPERM;
	case VFS_ERR_NOENT:
		return -LINUX_ENOENT;
	case VFS_ERR_BADF:
		return -LINUX_EBADF;
	case VFS_ERR_EXIST:
		return -LINUX_EEXIST;
	case VFS_ERR_NOTDIR:
		return -LINUX_ENOTDIR;
	case VFS_ERR_ISDIR:
		return -LINUX_EISDIR;
	case VFS_ERR_INVAL:
		return -LINUX_EINVAL;
	case VFS_ERR_MFILE:
		return -LINUX_EMFILE;
	case VFS_ERR_NOSPC:
		return -LINUX_ENOSPC;
	case VFS_ERR_NOTEMPTY:
		return -LINUX_ENOTEMPTY;
	case VFS_ERR_NODEV:
		return -LINUX_ENODEV;
	case VFS_ERR_NOMEM:
		return -LINUX_ENOMEM;
	case VFS_ERR_NOTSUPP:
		return -LINUX_EOPNOTSUPP;
	case VFS_ERR_NAMETOOLONG:
		return -LINUX_ENAMETOOLONG;
	case VFS_ERR_BUSY:
		return -LINUX_EBUSY;
	case VFS_ERR_LOOP:
		return -LINUX_ELOOP;
	case VFS_ERR_XDEV:
		return -LINUX_EXDEV;
	case VFS_ERR_MLINK:
		return -LINUX_EMLINK;
	case VFS_ERR_NOTTY:
		return -LINUX_ENOTTY;
	case VFS_ERR_NXIO:
		return -LINUX_ENXIO;
	case VFS_ERR_FAULT:
		return -LINUX_EFAULT;
	case VFS_ERR_IO:
		return -LINUX_EIO;
	case VFS_ERR_SPIPE:
		return -LINUX_ESPIPE;
	default:
		return -LINUX_EIO;
	}
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
	if (linux_flags & LINUX_O_TRUNC)
		flags |= VFS_OPEN_TRUNCATE;
	if (linux_flags & LINUX_O_APPEND)
		flags |= VFS_OPEN_APPEND;
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
	mode &= ~cur_proc()->umask;
	result = vfs_open(path, flags, mode, &fd);
	if (result < 0)
		return linux_error(result);
	if (linux_flags & LINUX_O_CLOEXEC)
		vfs_set_fd_flags(fd, VFS_FD_CLOEXEC);
	return fd;
}

uint64 sys_linux_close(void)
{
	int fd, result;

	argint(0, &fd);
	result = vfs_close(fd);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_read(void)
{
	uint64 address;
	int fd, length, result;

	argint(0, &fd);
	argaddr(1, &address);
	argint(2, &length);
	result = vfs_read(fd, address, length);
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

uint64 sys_linux_getdents64(void)
{
	struct {
		uint64 ino;
		int64 offset;
		uint16 reclen;
		uint8 type;
		char name[VFS_NAME_MAX + 1];
	} linux_dirent;
	struct vfs_dirent dirent;
	process_t process = cur_proc();
	uint64 address;
	int fd, length, result = 0, used = 0;
	int name_length, record_length;

	argint(0, &fd);
	argaddr(1, &address);
	argint(2, &length);
	if (length < 24)
		return -LINUX_EINVAL;
	while (length - used >= 24 &&
	       (result = vfs_next_dirent(fd, &dirent)) > 0) {
		name_length = strlen(dirent.name);
		record_length = (19 + name_length + 1 + 7) & ~7;
		if (used + record_length > length)
			return used ? used : -LINUX_EINVAL;
		memset(&linux_dirent, 0, sizeof(linux_dirent));
		linux_dirent.ino = dirent.ino;
		linux_dirent.offset = dirent.next_offset;
		linux_dirent.reclen = record_length;
		linux_dirent.type = dirent.type;
		safe_strncpy(linux_dirent.name, dirent.name,
		             sizeof(linux_dirent.name));
		if (copyout(process->pagetable, address + used,
		            (char *)&linux_dirent, record_length) < 0)
			return used ? used : -LINUX_EFAULT;
		used += record_length;
	}
	if (result < 0)
		return used ? used : linux_error(result);
	return used;
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
		return value;
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
	mode &= ~cur_proc()->umask;
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
	int dirfd, result;

	argint(0, &dirfd);
	if (argstr(1, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	result = vfs_access(path);
	return result < 0 ? linux_error(result) : 0;
}

uint64 sys_linux_utimensat(void)
{
	char path[MAXPATH];
	int dirfd, result;

	argint(0, &dirfd);
	if (argstr(1, path, sizeof(path)) < 0)
		return -LINUX_EFAULT;
	if (path[0] != '/' && dirfd != LINUX_AT_FDCWD)
		return -LINUX_EBADF;
	/* Caffeinix does not persist inode timestamps yet. */
	result = vfs_access(path);
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
	uint64 address;
	int fd, request;
	int64 result;

	argint(0, &fd);
	argint(1, &request);
	argaddr(2, &address);
	result = vfs_ioctl(fd, request, address);
	return result < 0 ? linux_error(result) : result;
}

uint64 sys_linux_writev(void)
{
	process_t p = cur_proc();
	struct linux_iovec iov;
	uint64 iov_address;
	uint64 total = 0;
	file_t f;
	int fd, count, i, written;

	argint(0, &fd);
	argaddr(1, &iov_address);
	argint(2, &count);

	if (fd < 0 || fd >= NOFILE || !(f = p->ofile[fd]))
		return -LINUX_EBADF;
	if (count < 0 || count > LINUX_IOV_MAX)
		return -LINUX_EINVAL;

	for (i = 0; i < count; i++) {
		if (copyin(p->pagetable, (char *)&iov,
		           iov_address + i * sizeof(iov), sizeof(iov)) < 0)
			return total ? total : -LINUX_EFAULT;
		if (iov.len > 0x7fffffff)
			return total ? total : -LINUX_EINVAL;

		written = vfs_write(fd, iov.base, iov.len);
		if (written < 0)
			return total ? total : linux_error(written);
		total += written;
		if ((uint64)written != iov.len)
			break;
	}

	return total;
}

uint64 sys_linux_execve(void)
{
	char path[MAXPATH], *argv[MAXARG], *envp[MAXARG];
	uint64 user_argv, user_envp;
	int ret;

	argaddr(1, &user_argv);
	argaddr(2, &user_envp);
	if (argstr(0, path, MAXPATH) < 0)
		return -LINUX_EFAULT;

	memset(argv, 0, sizeof(argv));
	memset(envp, 0, sizeof(envp));
	if (copy_user_vector(user_argv, argv) < 0 ||
	    copy_user_vector(user_envp, envp) < 0)
		goto fault;

	ret = exec_linux(path, argv, envp);
	if (ret >= 0)
		vfs_close_on_exec();
	free_user_vector(argv);
	free_user_vector(envp);
	return ret < 0 ? -LINUX_EFAULT : 0;

fault:
	free_user_vector(argv);
	free_user_vector(envp);
	return -LINUX_EFAULT;
}
