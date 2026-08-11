#include <dirent.h>
#include <driver.h>
#include <file.h>
#include <inode.h>
#include <log.h>
#include <mystring.h>
#include <scheduler.h>
#include <vfs.h>

static int fd_alloc(file_t file, int minimum, uint8 flags)
{
	process_t process = cur_proc();
	int fd;

	if (minimum < 0)
		minimum = 0;
	for (fd = minimum; fd < NOFILE; fd++) {
		if (!process->ofile[fd]) {
			process->ofile[fd] = file;
			process->fd_flags[fd] = flags;
			return fd;
		}
	}
	return -1;
}

static int fd_get(int fd, file_t *file)
{
	if (fd < 0 || fd >= NOFILE || !cur_proc()->ofile[fd])
		return VFS_ERR_BADF;
	*file = cur_proc()->ofile[fd];
	return VFS_OK;
}

static inode_t create(const char *path, short type, short major,
		      short minor)
{
	inode_t directory, inode;
	char name[DIRSIZ];

	directory = nameiparent((char *)path, name);
	if (!directory)
		return 0;
	ilock(directory);
	inode = dirlookup(directory, name, 0);
	if (inode) {
		iunlockput(directory);
		ilock(inode);
		if (type == T_FILE &&
		    (inode->d.type == T_FILE || inode->d.type == T_DEVICE))
			return inode;
		iunlockput(inode);
		return 0;
	}

	inode = ialloc(directory->dev, type);
	if (!inode) {
		iunlockput(directory);
		return 0;
	}
	ilock(inode);
	inode->d.nlink = 1;
	if (type == T_DEVICE) {
		inode->d.major = major;
		inode->d.minor = minor;
	}
	iupdate(inode);

	if (type == T_DIR &&
	    (dirlink(inode, ".", inode->inum) ||
	     dirlink(inode, "..", directory->inum)))
		goto fail;
	if (dirlink(directory, name, inode->inum))
		goto fail;

	iunlockput(directory);
	return inode;

fail:
	inode->d.nlink = 0;
	iupdate(inode);
	iunlockput(directory);
	iunlockput(inode);
	return 0;
}

static void fill_stat(inode_t inode, struct vfs_stat *stat)
{
	stat->dev = inode->dev;
	stat->ino = inode->inum;
	stat->type = inode->d.type;
	stat->nlink = inode->d.nlink;
	stat->rdev = ((uint64)(uint16)inode->d.major << 8) |
	             (uint16)inode->d.minor;
	stat->size = inode->d.size;
}

int vfs_open(const char *path, uint32 flags, int *fd_out)
{
	inode_t inode;
	file_t file;
	int fd, existed;

	log_begin();
	inode = namei((char *)path);
	existed = inode != 0;
	if (existed) {
		if (flags & VFS_OPEN_EXCLUSIVE) {
			iput(inode);
			log_end();
			return VFS_ERR_EXIST;
		}
		ilock(inode);
	} else if (flags & VFS_OPEN_CREATE) {
		inode = create(path, T_FILE, 0, 0);
		if (!inode) {
			log_end();
			return VFS_ERR_IO;
		}
	} else {
		log_end();
		return VFS_ERR_NOENT;
	}

	if ((flags & VFS_OPEN_DIRECTORY) && inode->d.type != T_DIR) {
		iunlockput(inode);
		log_end();
		return VFS_ERR_NOTDIR;
	}
	if (inode->d.type == T_DIR && (flags & VFS_OPEN_WRITE)) {
		iunlockput(inode);
		log_end();
		return VFS_ERR_ISDIR;
	}
	if (inode->d.type == T_DEVICE &&
	    (inode->d.major < 0 || inode->d.major >= NDEV)) {
		iunlockput(inode);
		log_end();
		return VFS_ERR_NODEV;
	}

	file = file_alloc();
	if (!file) {
		iunlockput(inode);
		log_end();
		return VFS_ERR_MFILE;
	}
	fd = fd_alloc(file, 0, 0);
	if (fd < 0) {
		file_close(file);
		iunlockput(inode);
		log_end();
		return VFS_ERR_MFILE;
	}

	file->type = inode->d.type == T_DEVICE ? FD_DEVICE : FD_INODE;
	file->major = inode->d.major;
	file->ip = inode;
	file->off = (flags & VFS_OPEN_APPEND) ? inode->d.size : 0;
	file->readable = !!(flags & VFS_OPEN_READ);
	file->writable = !!(flags & VFS_OPEN_WRITE);
	file->flags = flags;
	if ((flags & VFS_OPEN_TRUNCATE) && inode->d.type == T_FILE)
		itrunc(inode);

	iunlock(inode);
	log_end();
	*fd_out = fd;
	return VFS_OK;
}

int vfs_close(int fd)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	cur_proc()->ofile[fd] = 0;
	cur_proc()->fd_flags[fd] = 0;
	file_close(file);
	return VFS_OK;
}

int vfs_dup(int oldfd, int minimum, uint8 flags, int *fd_out)
{
	file_t file;
	int fd;

	if (fd_get(oldfd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	fd = fd_alloc(file, minimum, flags);
	if (fd < 0)
		return VFS_ERR_MFILE;
	file_dup(file);
	*fd_out = fd;
	return VFS_OK;
}

int vfs_dup_to(int oldfd, int newfd, uint8 flags)
{
	process_t process = cur_proc();
	file_t file;

	if (fd_get(oldfd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (newfd < 0 || newfd >= NOFILE)
		return VFS_ERR_BADF;
	if (oldfd == newfd)
		return VFS_ERR_INVAL;
	if (process->ofile[newfd])
		vfs_close(newfd);
	process->ofile[newfd] = file_dup(file);
	process->fd_flags[newfd] = flags;
	return VFS_OK;
}

int vfs_read(int fd, uint64 address, int length)
{
	file_t file;
	int result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (length < 0)
		return VFS_ERR_INVAL;
	result = file_read(file, address, length);
	return result < 0 ? VFS_ERR_IO : result;
}

int vfs_write(int fd, uint64 address, int length)
{
	file_t file;
	int result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (length < 0)
		return VFS_ERR_INVAL;
	result = file_write(file, address, length);
	return result < 0 ? VFS_ERR_IO : result;
}

int vfs_seek(int fd, int64 offset, int whence, uint64 *result)
{
	file_t file;
	int64 next;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (file->type != FD_INODE)
		return VFS_ERR_INVAL;
	if (whence == 0)
		next = offset;
	else if (whence == 1)
		next = (int64)file->off + offset;
	else if (whence == 2)
		next = (int64)file->ip->d.size + offset;
	else
		return VFS_ERR_INVAL;
	if (next < 0 || (uint64)next > 0xffffffffULL)
		return VFS_ERR_INVAL;
	file->off = next;
	*result = next;
	return VFS_OK;
}

int vfs_stat_fd(int fd, struct vfs_stat *stat)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (file->type != FD_INODE && file->type != FD_DEVICE)
		return VFS_ERR_INVAL;
	ilock(file->ip);
	fill_stat(file->ip, stat);
	iunlock(file->ip);
	return VFS_OK;
}

int vfs_stat_path(const char *path, struct vfs_stat *stat)
{
	inode_t inode = namei((char *)path);

	if (!inode)
		return VFS_ERR_NOENT;
	ilock(inode);
	fill_stat(inode, stat);
	iunlockput(inode);
	return VFS_OK;
}

int vfs_next_dirent(int fd, struct vfs_dirent *dirent)
{
	struct dirent disk_dirent;
	file_t file;
	int i;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (file->type != FD_INODE)
		return VFS_ERR_NOTDIR;
	ilock(file->ip);
	if (file->ip->d.type != T_DIR) {
		iunlock(file->ip);
		return VFS_ERR_NOTDIR;
	}
	while (file->off < file->ip->d.size) {
		if (readi(file->ip, 0, (uint64)&disk_dirent, file->off,
		          sizeof(disk_dirent)) != sizeof(disk_dirent)) {
			iunlock(file->ip);
			return VFS_ERR_IO;
		}
		file->off += sizeof(disk_dirent);
		if (!disk_dirent.inum)
			continue;
		dirent->ino = disk_dirent.inum;
		dirent->next_offset = file->off;
		dirent->type = 0;
		for (i = 0; i < DIRSIZ && disk_dirent.name[i]; i++)
			dirent->name[i] = disk_dirent.name[i];
		dirent->name[i] = 0;
		iunlock(file->ip);
		return 1;
	}
	iunlock(file->ip);
	return 0;
}

int vfs_mkdir(const char *path)
{
	inode_t inode;

	log_begin();
	inode = create(path, T_DIR, 0, 0);
	if (!inode) {
		log_end();
		return VFS_ERR_EXIST;
	}
	iunlockput(inode);
	log_end();
	return VFS_OK;
}

static int directory_empty(inode_t directory)
{
	struct dirent dirent;
	uint32 offset;

	for (offset = 2 * sizeof(dirent); offset < directory->d.size;
	     offset += sizeof(dirent)) {
		if (readi(directory, 0, (uint64)&dirent, offset,
		          sizeof(dirent)) != sizeof(dirent))
			return 0;
		if (dirent.inum)
			return 0;
	}
	return 1;
}

int vfs_unlink(const char *path, int remove_directory)
{
	struct dirent empty;
	inode_t inode, parent;
	char name[DIRSIZ];
	uint32 offset;
	int result = VFS_ERR_NOENT;

	log_begin();
	parent = nameiparent((char *)path, name);
	if (!parent)
		goto out;
	ilock(parent);
	if (!strncmp(name, ".", DIRSIZ) || !strncmp(name, "..", DIRSIZ)) {
		result = VFS_ERR_INVAL;
		goto bad_parent;
	}
	inode = dirlookup(parent, name, &offset);
	if (!inode)
		goto bad_parent;
	ilock(inode);
	if (remove_directory && inode->d.type != T_DIR) {
		result = VFS_ERR_NOTDIR;
		goto bad_inode;
	}
	if (!remove_directory && inode->d.type == T_DIR) {
		result = VFS_ERR_ISDIR;
		goto bad_inode;
	}
	if (inode->d.type == T_DIR && !directory_empty(inode)) {
		result = VFS_ERR_NOTEMPTY;
		goto bad_inode;
	}
	memset(&empty, 0, sizeof(empty));
	if (writei(parent, 0, (uint64)&empty, offset,
	           sizeof(empty)) != sizeof(empty))
		goto bad_inode;
	iunlockput(parent);
	inode->d.nlink--;
	iupdate(inode);
	iunlockput(inode);
	log_end();
	return VFS_OK;

bad_inode:
	iunlockput(inode);
bad_parent:
	iunlockput(parent);
out:
	log_end();
	return result;
}

int vfs_chdir(const char *path)
{
	process_t process = cur_proc();
	inode_t inode;
	char *last;

	log_begin();
	inode = namei((char *)path);
	if (!inode) {
		log_end();
		return VFS_ERR_NOENT;
	}
	ilock(inode);
	if (inode->d.type != T_DIR) {
		iunlockput(inode);
		log_end();
		return VFS_ERR_NOTDIR;
	}
	iunlock(inode);
	iput(process->cwd);
	process->cwd = inode;
	log_end();

	if (path[0] == '/') {
		safe_strncpy(process->cwd_name, path, MAXPATH);
	} else if (!strncmp(path, "..", DIRSIZ)) {
		if (strlen(process->cwd_name) != 1) {
			last = strrchr(process->cwd_name, '/');
			if (last == process->cwd_name)
				last++;
			*last = 0;
		}
	} else if (strncmp(path, ".", DIRSIZ)) {
		if (process->cwd_name[strlen(process->cwd_name) - 1] != '/')
			strcat(process->cwd_name, "/");
		strcat(process->cwd_name, path);
	}
	return VFS_OK;
}

int vfs_access(const char *path)
{
	inode_t inode = namei((char *)path);

	if (!inode)
		return VFS_ERR_NOENT;
	iput(inode);
	return VFS_OK;
}

int vfs_get_fd_flags(int fd, uint8 *flags)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	(void)file;
	*flags = cur_proc()->fd_flags[fd];
	return VFS_OK;
}

int vfs_set_fd_flags(int fd, uint8 flags)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	(void)file;
	cur_proc()->fd_flags[fd] = flags;
	return VFS_OK;
}

int vfs_get_file_flags(int fd, uint32 *flags)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	*flags = file->flags;
	return VFS_OK;
}

int vfs_is_terminal(int fd)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	return file->type == FD_DEVICE && file->major == CONSOLE;
}

void vfs_close_on_exec(void)
{
	int fd;

	for (fd = 0; fd < NOFILE; fd++) {
		if (cur_proc()->ofile[fd] &&
		    (cur_proc()->fd_flags[fd] & VFS_FD_CLOEXEC))
			vfs_close(fd);
	}
}
