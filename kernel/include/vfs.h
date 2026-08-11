#ifndef __CAFFEINIX_KERNEL_VFS_H
#define __CAFFEINIX_KERNEL_VFS_H

#include <file.h>
#include <fs.h>
#include <typedefs.h>

enum vfs_result {
	VFS_OK = 0,
	VFS_ERR_PERM = -1,
	VFS_ERR_NOENT = -2,
	VFS_ERR_IO = -3,
	VFS_ERR_BADF = -4,
	VFS_ERR_EXIST = -5,
	VFS_ERR_NOTDIR = -6,
	VFS_ERR_ISDIR = -7,
	VFS_ERR_INVAL = -8,
	VFS_ERR_MFILE = -9,
	VFS_ERR_NOSPC = -10,
	VFS_ERR_NOTEMPTY = -11,
	VFS_ERR_NODEV = -12,
};

#define VFS_OPEN_READ       (1 << 0)
#define VFS_OPEN_WRITE      (1 << 1)
#define VFS_OPEN_CREATE     (1 << 2)
#define VFS_OPEN_EXCLUSIVE  (1 << 3)
#define VFS_OPEN_TRUNCATE   (1 << 4)
#define VFS_OPEN_DIRECTORY  (1 << 5)
#define VFS_OPEN_APPEND     (1 << 6)

#define VFS_FD_CLOEXEC      (1 << 0)

struct vfs_stat {
	uint64 dev;
	uint64 ino;
	uint32 type;
	uint32 nlink;
	uint64 rdev;
	uint64 size;
};

struct vfs_dirent {
	uint64 ino;
	uint64 next_offset;
	uint8 type;
	char name[DIRSIZ + 1];
};

int vfs_open(const char *path, uint32 flags, int *fd_out);
int vfs_close(int fd);
int vfs_dup(int oldfd, int minimum, uint8 flags, int *fd_out);
int vfs_dup_to(int oldfd, int newfd, uint8 flags);
int vfs_read(int fd, uint64 address, int length);
int vfs_write(int fd, uint64 address, int length);
int vfs_seek(int fd, int64 offset, int whence, uint64 *result);
int vfs_stat_fd(int fd, struct vfs_stat *stat);
int vfs_stat_path(const char *path, struct vfs_stat *stat);
int vfs_next_dirent(int fd, struct vfs_dirent *dirent);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path, int remove_directory);
int vfs_chdir(const char *path);
int vfs_access(const char *path);
int vfs_get_fd_flags(int fd, uint8 *flags);
int vfs_set_fd_flags(int fd, uint8 flags);
int vfs_get_file_flags(int fd, uint32 *flags);
int vfs_is_terminal(int fd);
void vfs_close_on_exec(void);

#endif
