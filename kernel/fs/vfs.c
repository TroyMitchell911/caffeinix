#include <debug.h>
#include <device.h>
#include <file.h>
#include <ktime.h>
#include <mystring.h>
#include <palloc.h>
#include <printk.h>
#include <process.h>
#include <scheduler.h>
#include <spinlock.h>
#include <vfs.h>
#include <wait.h>

#define VFS_FILESYSTEM_MAX 8
#define VFS_SUPER_MAX 16
#define VFS_INODE_MAX 256
#define VFS_DENTRY_MAX 256
#define VFS_MOUNT_MAX 16
#define VFS_SYMLINK_MAX 8

#define VFS_LOOKUP_PARENT (1U << 0)
#define VFS_LOOKUP_NOFOLLOW_FINAL (1U << 1)

struct vfs_walk_buffer {
	char component[VFS_NAME_MAX + 1];
	char combined[VFS_PATH_MAX];
	char link[VFS_PATH_MAX];
	char pending[VFS_PATH_MAX];
};

_Static_assert(sizeof(struct vfs_walk_buffer) <= PGSIZE,
	       "VFS path scratch space exceeds one page");

static struct {
	struct spinlock lock;
	struct vfs_filesystem_type *filesystems[VFS_FILESYSTEM_MAX];
	struct vfs_super_block superblocks[VFS_SUPER_MAX];
	struct vfs_inode inodes[VFS_INODE_MAX];
	struct vfs_dentry dentries[VFS_DENTRY_MAX];
	struct vfs_mount mounts[VFS_MOUNT_MAX];
	struct vfs_mount *root;
} vfs;

static struct {
	struct spinlock lock;
	struct wait_queue wait;
	uint64 generation;
} poll_state;

static int string_equal(const char *left, const char *right)
{
	uint32 left_length = strlen(left);
	uint32 right_length = strlen(right);

	return left_length == right_length &&
	       !strncmp(left, right, left_length);
}

static sleeplock_t vfs_inode_write_lock(struct vfs_inode *inode)
{
	if (!inode || inode->type != VFS_INODE_REGULAR ||
	    !inode->superblock)
		return 0;
	return &inode->superblock->write_lock;
}

void vfs_init(void)
{
	spinlock_init(&vfs.lock, "vfs");
	spinlock_init(&poll_state.lock, "VFS poll");
	wait_queue_init(&poll_state.wait, "VFS poll");
}

int vfs_register_filesystem(struct vfs_filesystem_type *type)
{
	int i, free_slot = -1;

	if (!type || !type->name || !type->mount)
		return VFS_ERR_INVAL;
	spinlock_acquire(&vfs.lock);
	for (i = 0; i < VFS_FILESYSTEM_MAX; i++) {
		if (vfs.filesystems[i] &&
		    string_equal(vfs.filesystems[i]->name, type->name)) {
			spinlock_release(&vfs.lock);
			return VFS_ERR_EXIST;
		}
		if (!vfs.filesystems[i] && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0) {
		spinlock_release(&vfs.lock);
		return VFS_ERR_NOSPC;
	}
	vfs.filesystems[free_slot] = type;
	spinlock_release(&vfs.lock);
	return VFS_OK;
}

static struct vfs_filesystem_type *vfs_find_filesystem(const char *name)
{
	struct vfs_filesystem_type *type = 0;
	int i;

	spinlock_acquire(&vfs.lock);
	for (i = 0; i < VFS_FILESYSTEM_MAX; i++) {
		if (vfs.filesystems[i] &&
		    string_equal(vfs.filesystems[i]->name, name)) {
			type = vfs.filesystems[i];
			break;
		}
	}
	spinlock_release(&vfs.lock);
	return type;
}

struct vfs_super_block *vfs_super_alloc(struct vfs_filesystem_type *type,
					struct block_device *device)
{
	struct vfs_super_block *superblock;

	spinlock_acquire(&vfs.lock);
	for (superblock = vfs.superblocks;
	     superblock != &vfs.superblocks[VFS_SUPER_MAX]; superblock++) {
		if (!superblock->ref) {
			memset(superblock, 0, sizeof(*superblock));
			superblock->ref = 1;
			sleeplock_init(&superblock->write_lock, "VFS write");
			superblock->type = type;
			superblock->device = device;
			spinlock_release(&vfs.lock);
			return superblock;
		}
	}
	spinlock_release(&vfs.lock);
	return 0;
}

void vfs_super_free(struct vfs_super_block *superblock)
{
	if (!superblock)
		return;
	spinlock_acquire(&vfs.lock);
	if (superblock->ref != 1 || superblock->root)
		PANIC("vfs super free");
	memset(superblock, 0, sizeof(*superblock));
	spinlock_release(&vfs.lock);
}

struct vfs_inode *vfs_inode_alloc(struct vfs_super_block *superblock)
{
	struct vfs_inode *inode;

	spinlock_acquire(&vfs.lock);
	for (inode = vfs.inodes;
	     inode != &vfs.inodes[VFS_INODE_MAX]; inode++) {
		if (!inode->ref) {
			memset(inode, 0, sizeof(*inode));
			inode->ref = 1;
			inode->superblock = superblock;
			spinlock_release(&vfs.lock);
			return inode;
		}
	}
	spinlock_release(&vfs.lock);
	return 0;
}

struct vfs_inode *vfs_inode_get(struct vfs_inode *inode)
{
	spinlock_acquire(&vfs.lock);
	if (!inode || inode->ref < 1)
		PANIC("vfs inode get");
	inode->ref++;
	spinlock_release(&vfs.lock);
	return inode;
}

void vfs_inode_put(struct vfs_inode *inode)
{
	const struct vfs_super_operations *operations;

	if (!inode)
		return;
	spinlock_acquire(&vfs.lock);
	if (inode->ref < 1)
		PANIC("vfs inode put");
	if (--inode->ref) {
		spinlock_release(&vfs.lock);
		return;
	}
	inode->ref = -1;
	operations = inode->superblock->operations;
	spinlock_release(&vfs.lock);

	if (operations && operations->put_inode)
		operations->put_inode(inode);
	spinlock_acquire(&vfs.lock);
	memset(inode, 0, sizeof(*inode));
	spinlock_release(&vfs.lock);
}

int vfs_inode_stat_default(struct vfs_inode *inode, struct vfs_stat *stat)
{
	if (!inode || !stat)
		return VFS_ERR_INVAL;
	memset(stat, 0, sizeof(*stat));
	stat->dev = inode->superblock->device ?
		inode->superblock->device->id : 0;
	stat->ino = inode->number;
	stat->type = inode->type;
	stat->mode = inode->mode;
	stat->uid = inode->uid;
	stat->gid = inode->gid;
	stat->nlink = inode->nlink;
	stat->rdev = inode->device;
	stat->size = inode->size;
	stat->blocks = inode->blocks;
	stat->block_size = inode->superblock->block_size;
	return VFS_OK;
}

int vfs_inode_stat(struct vfs_inode *inode, struct vfs_stat *stat)
{
	if (!inode || !stat)
		return VFS_ERR_INVAL;
	if (inode->operations && inode->operations->getattr)
		return inode->operations->getattr(inode, stat);
	return vfs_inode_stat_default(inode, stat);
}

static struct vfs_dentry *vfs_dentry_alloc(struct vfs_dentry *parent,
					    const char *name,
					    struct vfs_inode *inode)
{
	struct vfs_dentry *dentry;

	if (strlen(name) > VFS_NAME_MAX)
		return 0;
	spinlock_acquire(&vfs.lock);
	for (dentry = vfs.dentries;
	     dentry != &vfs.dentries[VFS_DENTRY_MAX]; dentry++) {
		if (!dentry->ref) {
			memset(dentry, 0, sizeof(*dentry));
			dentry->ref = 1;
			dentry->parent = parent;
			if (parent)
				parent->ref++;
			dentry->inode = inode;
			inode->ref++;
			safe_strncpy(dentry->name, name, sizeof(dentry->name));
			spinlock_release(&vfs.lock);
			return dentry;
		}
	}
	spinlock_release(&vfs.lock);
	return 0;
}

static struct vfs_dentry *vfs_dentry_get(struct vfs_dentry *dentry)
{
	spinlock_acquire(&vfs.lock);
	if (!dentry || dentry->ref < 1)
		PANIC("vfs dentry get");
	dentry->ref++;
	spinlock_release(&vfs.lock);
	return dentry;
}

static void vfs_dentry_put(struct vfs_dentry *dentry)
{
	struct vfs_dentry *parent;
	struct vfs_inode *inode;

	while (dentry) {
		spinlock_acquire(&vfs.lock);
		if (dentry->ref < 1)
			PANIC("vfs dentry put");
		if (--dentry->ref) {
			spinlock_release(&vfs.lock);
			return;
		}
		parent = dentry->parent;
		inode = dentry->inode;
		dentry->parent = 0;
		dentry->inode = 0;
		dentry->name[0] = 0;
		spinlock_release(&vfs.lock);
		vfs_inode_put(inode);
		dentry = parent;
	}
}

static struct vfs_mount *vfs_mount_alloc(void)
{
	struct vfs_mount *mount;

	spinlock_acquire(&vfs.lock);
	for (mount = vfs.mounts;
	     mount != &vfs.mounts[VFS_MOUNT_MAX]; mount++) {
		if (!mount->ref) {
			memset(mount, 0, sizeof(*mount));
			mount->ref = 1;
			spinlock_release(&vfs.lock);
			return mount;
		}
	}
	spinlock_release(&vfs.lock);
	return 0;
}

void vfs_path_copy(struct vfs_path *destination,
		   const struct vfs_path *source)
{
	if (!source || !source->mount || !source->dentry)
		PANIC("vfs path copy");
	destination->mount = source->mount;
	destination->dentry = vfs_dentry_get(source->dentry);
	spinlock_acquire(&vfs.lock);
	destination->mount->ref++;
	spinlock_release(&vfs.lock);
}

void vfs_path_put(struct vfs_path *path)
{
	if (!path || !path->mount)
		return;
	vfs_dentry_put(path->dentry);
	spinlock_acquire(&vfs.lock);
	if (path->mount->ref < 1)
		PANIC("vfs mount put");
	path->mount->ref--;
	spinlock_release(&vfs.lock);
	path->mount = 0;
	path->dentry = 0;
}

int vfs_get_root(struct vfs_path *path)
{
	struct vfs_path root;

	spinlock_acquire(&vfs.lock);
	if (!vfs.root) {
		spinlock_release(&vfs.lock);
		return VFS_ERR_NOENT;
	}
	root.mount = vfs.root;
	root.dentry = vfs.root->root;
	spinlock_release(&vfs.lock);
	vfs_path_copy(path, &root);
	return VFS_OK;
}

static int vfs_mount_type(struct vfs_filesystem_type *type,
			  struct block_device *device, const void *data,
			  struct vfs_super_block **result)
{
	if ((type->flags & VFS_FS_REQUIRES_DEVICE) && !device)
		return VFS_ERR_NODEV;
	return type->mount(type, device, data, result);
}

int vfs_mount_root(const char *filesystem, struct block_device *device,
		   const void *data)
{
	struct vfs_filesystem_type *type;
	struct vfs_super_block *superblock;
	struct vfs_dentry *root;
	struct vfs_mount *mount;
	int result;

	if (vfs.root)
		return VFS_ERR_BUSY;
	type = vfs_find_filesystem(filesystem);
	if (!type)
		return VFS_ERR_NODEV;
	result = vfs_mount_type(type, device, data, &superblock);
	if (result < 0)
		return result;
	if (!superblock || !superblock->root)
		return VFS_ERR_IO;
	root = vfs_dentry_alloc(0, "", superblock->root);
	if (!root)
		return VFS_ERR_NOMEM;
	mount = vfs_mount_alloc();
	if (!mount) {
		vfs_dentry_put(root);
		return VFS_ERR_NOMEM;
	}
	mount->superblock = superblock;
	mount->root = root;
	spinlock_acquire(&vfs.lock);
	vfs.root = mount;
	spinlock_release(&vfs.lock);
	pr_info("VFS: mounted root (%s) on %s", filesystem,
		device ? device->name : "none");
	return VFS_OK;
}

static int vfs_same_inode(struct vfs_inode *left,
			  struct vfs_inode *right)
{
	return left->superblock == right->superblock &&
	       left->number == right->number;
}

static struct vfs_mount *vfs_child_mount(const struct vfs_path *path)
{
	struct vfs_mount *mount, *result = 0;

	spinlock_acquire(&vfs.lock);
	for (mount = vfs.mounts;
	     mount != &vfs.mounts[VFS_MOUNT_MAX]; mount++) {
		if (mount->ref && mount->parent == path->mount &&
		    mount->mountpoint.dentry &&
		    vfs_same_inode(mount->mountpoint.dentry->inode,
		                   path->dentry->inode)) {
			result = mount;
			break;
		}
	}
	spinlock_release(&vfs.lock);
	return result;
}

static void vfs_follow_mount(struct vfs_path *path)
{
	struct vfs_mount *child;
	struct vfs_path next, source;

	while ((child = vfs_child_mount(path))) {
		source.mount = child;
		source.dentry = child->root;
		vfs_path_copy(&next, &source);
		vfs_path_put(path);
		*path = next;
	}
}

static int vfs_start_path(const char *name, struct vfs_path *path)
{
	process_t process = cur_proc();

	if (name[0] == '/') {
		if (process && process->root.dentry) {
			vfs_path_copy(path, &process->root);
			return VFS_OK;
		}
		return vfs_get_root(path);
	}
	if (!process || !process->cwd.dentry)
		return VFS_ERR_NOENT;
	vfs_path_copy(path, &process->cwd);
	return VFS_OK;
}

static void vfs_path_parent(struct vfs_path *path)
{
	struct vfs_path next, source;

	if (path->dentry == path->mount->root && path->mount->parent) {
		vfs_path_copy(&next, &path->mount->mountpoint);
		vfs_path_put(path);
		*path = next;
	}
	if (!path->dentry->parent)
		return;
	source.mount = path->mount;
	source.dentry = path->dentry->parent;
	vfs_path_copy(&next, &source);
	vfs_path_put(path);
	*path = next;
}

static int vfs_next_component(const char *path, uint32 *offset,
			      char *name, int *final)
{
	uint32 length = 0, position = *offset;

	while (path[position] == '/')
		position++;
	if (!path[position]) {
		*offset = position;
		return 0;
	}
	while (path[position + length] &&
	       path[position + length] != '/') {
		if (length == VFS_NAME_MAX)
			return VFS_ERR_NAMETOOLONG;
		name[length] = path[position + length];
		length++;
	}
	name[length] = 0;
	position += length;
	while (path[position] == '/')
		position++;
	*offset = position;
	*final = !path[position];
	return 1;
}

static int vfs_join_link(char *destination, const char *target,
			 const char *remaining)
{
	uint32 length = strlen(target);
	uint32 remaining_length = strlen(remaining);
	uint32 position = 0;

	if (length + remaining_length + 2 > VFS_PATH_MAX)
		return VFS_ERR_NAMETOOLONG;
	while (*target)
		destination[position++] = *target++;
	if (position && remaining_length && destination[position - 1] != '/')
		destination[position++] = '/';
	while (*remaining)
		destination[position++] = *remaining++;
	destination[position] = 0;
	return VFS_OK;
}

static int vfs_walk(const char *name, uint32 flags, struct vfs_path *result,
		    char *last)
{
	struct vfs_walk_buffer *buffer;
	struct vfs_inode *inode;
	struct vfs_dentry *dentry;
	struct vfs_path current, next;
	uint32 offset = 0;
	int final, status, symlinks = 0;

	if (!name || !name[0])
		return VFS_ERR_NOENT;
	if (strlen(name) >= VFS_PATH_MAX)
		return VFS_ERR_NAMETOOLONG;
	buffer = palloc();
	if (!buffer)
		return VFS_ERR_NOMEM;
	safe_strncpy(buffer->pending, name, sizeof(buffer->pending));
	status = vfs_start_path(buffer->pending, &current);
	if (status < 0) {
		pfree(buffer);
		return status;
	}

	for (;;) {
		status = vfs_next_component(buffer->pending, &offset,
		                            buffer->component, &final);
		if (status < 0)
			goto fail;
		if (!status) {
			if (flags & VFS_LOOKUP_PARENT) {
				status = VFS_ERR_INVAL;
				goto fail;
			}
			*result = current;
			pfree(buffer);
			return VFS_OK;
		}
		if ((flags & VFS_LOOKUP_PARENT) && final) {
			safe_strncpy(last, buffer->component,
			             VFS_NAME_MAX + 1);
			*result = current;
			pfree(buffer);
			return VFS_OK;
		}
		if (string_equal(buffer->component, ".")) {
			if (final) {
				*result = current;
				pfree(buffer);
				return VFS_OK;
			}
			continue;
		}
		if (string_equal(buffer->component, "..")) {
			vfs_path_parent(&current);
			if (final) {
				*result = current;
				pfree(buffer);
				return VFS_OK;
			}
			continue;
		}
		if (current.dentry->inode->type != VFS_INODE_DIRECTORY) {
			status = VFS_ERR_NOTDIR;
			goto fail;
		}
		if (!current.dentry->inode->operations ||
		    !current.dentry->inode->operations->lookup) {
			status = VFS_ERR_NOTSUPP;
			goto fail;
		}
		status = current.dentry->inode->operations->lookup(
			current.dentry->inode, buffer->component, &inode);
		if (status < 0)
			goto fail;
		dentry = vfs_dentry_alloc(current.dentry, buffer->component,
		                           inode);
		vfs_inode_put(inode);
		if (!dentry) {
			status = VFS_ERR_NOMEM;
			goto fail;
		}
		next.mount = current.mount;
		next.dentry = dentry;
		spinlock_acquire(&vfs.lock);
		next.mount->ref++;
		spinlock_release(&vfs.lock);
		vfs_follow_mount(&next);

		if (next.dentry->inode->type == VFS_INODE_SYMLINK &&
		    (!final || !(flags & VFS_LOOKUP_NOFOLLOW_FINAL))) {
			if (++symlinks > VFS_SYMLINK_MAX) {
				vfs_path_put(&next);
				status = VFS_ERR_LOOP;
				goto fail;
			}
			if (!next.dentry->inode->operations ||
			    !next.dentry->inode->operations->readlink) {
				vfs_path_put(&next);
				status = VFS_ERR_NOTSUPP;
				goto fail;
			}
			status = next.dentry->inode->operations->readlink(
				next.dentry->inode, buffer->link,
				sizeof(buffer->link));
			vfs_path_put(&next);
			if (status < 0)
				goto fail;
			if ((uint32)status >= sizeof(buffer->link)) {
				status = VFS_ERR_NAMETOOLONG;
				goto fail;
			}
			buffer->link[status] = 0;
			status = vfs_join_link(buffer->combined, buffer->link,
			                       buffer->pending + offset);
			if (status < 0)
				goto fail;
			safe_strncpy(buffer->pending, buffer->combined,
			             sizeof(buffer->pending));
			offset = 0;
			if (buffer->pending[0] == '/') {
				vfs_path_put(&current);
				status = vfs_start_path(buffer->pending, &current);
				if (status < 0) {
					pfree(buffer);
					return status;
				}
			}
			continue;
		}

		vfs_path_put(&current);
		current = next;
		if (final) {
			*result = current;
			pfree(buffer);
			return VFS_OK;
		}
	}

fail:
	vfs_path_put(&current);
	pfree(buffer);
	return status;
}

int vfs_mount(const char *filesystem, struct block_device *device,
	      const char *target, const void *data)
{
	struct vfs_filesystem_type *type;
	struct vfs_super_block *superblock;
	struct vfs_dentry *root;
	struct vfs_mount *mount;
	struct vfs_path mountpoint;
	int status;

	type = vfs_find_filesystem(filesystem);
	if (!type)
		return VFS_ERR_NODEV;
	status = vfs_walk(target, 0, &mountpoint, 0);
	if (status < 0)
		return status;
	if (mountpoint.dentry->inode->type != VFS_INODE_DIRECTORY) {
		vfs_path_put(&mountpoint);
		return VFS_ERR_NOTDIR;
	}
	if (vfs_child_mount(&mountpoint)) {
		vfs_path_put(&mountpoint);
		return VFS_ERR_BUSY;
	}
	status = vfs_mount_type(type, device, data, &superblock);
	if (status < 0) {
		vfs_path_put(&mountpoint);
		return status;
	}
	root = vfs_dentry_alloc(0, "", superblock->root);
	if (!root) {
		vfs_path_put(&mountpoint);
		return VFS_ERR_NOMEM;
	}
	mount = vfs_mount_alloc();
	if (!mount) {
		vfs_dentry_put(root);
		vfs_path_put(&mountpoint);
		return VFS_ERR_NOMEM;
	}
	mount->superblock = superblock;
	mount->root = root;
	mount->parent = mountpoint.mount;
	mount->mountpoint = mountpoint;
	pr_info("VFS: mounted %s on %s", filesystem, target);
	return VFS_OK;
}

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

static int vfs_leaf_valid(const char *name)
{
	return name && name[0] && !string_equal(name, ".") &&
	       !string_equal(name, "..");
}

static int vfs_create_path(const char *name, uint32 mode,
			   struct vfs_path *result)
{
	struct vfs_inode *inode;
	struct vfs_dentry *dentry;
	struct vfs_path parent;
	char last[VFS_NAME_MAX + 1];
	int status;

	status = vfs_walk(name, VFS_LOOKUP_PARENT, &parent, last);
	if (status < 0)
		return status;
	if (!vfs_leaf_valid(last)) {
		vfs_path_put(&parent);
		return VFS_ERR_INVAL;
	}
	if (!parent.dentry->inode->operations ||
	    !parent.dentry->inode->operations->create) {
		vfs_path_put(&parent);
		return VFS_ERR_NOTSUPP;
	}
	status = parent.dentry->inode->operations->create(
		parent.dentry->inode, last, mode, &inode);
	if (status < 0) {
		vfs_path_put(&parent);
		return status;
	}
	dentry = vfs_dentry_alloc(parent.dentry, last, inode);
	vfs_inode_put(inode);
	if (!dentry) {
		vfs_path_put(&parent);
		return VFS_ERR_NOMEM;
	}
	result->mount = parent.mount;
	result->dentry = dentry;
	spinlock_acquire(&vfs.lock);
	result->mount->ref++;
	spinlock_release(&vfs.lock);
	vfs_path_put(&parent);
	return VFS_OK;
}

int vfs_open_file(const char *name, uint32 flags, uint32 mode,
		  file_t *result)
{
	struct vfs_stat stat;
	struct vfs_path path;
	sleeplock_t lock;
	file_t file;
	int existed, status;

	status = vfs_walk(name, 0, &path, 0);
	existed = status == VFS_OK;
	if (!existed && status != VFS_ERR_NOENT)
		return status;
	if (existed && (flags & VFS_OPEN_CREATE) &&
	    (flags & VFS_OPEN_EXCLUSIVE)) {
		vfs_path_put(&path);
		return VFS_ERR_EXIST;
	}
	if (!existed) {
		if (!(flags & VFS_OPEN_CREATE))
			return VFS_ERR_NOENT;
		status = vfs_create_path(name, mode, &path);
		if (status < 0)
			return status;
	}
	status = vfs_inode_stat(path.dentry->inode, &stat);
	if (status < 0)
		goto fail;
	if ((flags & VFS_OPEN_DIRECTORY) &&
	    stat.type != VFS_INODE_DIRECTORY) {
		status = VFS_ERR_NOTDIR;
		goto fail;
	}
	if (stat.type == VFS_INODE_DIRECTORY &&
	    (flags & VFS_OPEN_WRITE)) {
		status = VFS_ERR_ISDIR;
		goto fail;
	}
	if ((flags & VFS_OPEN_TRUNCATE) &&
	    stat.type == VFS_INODE_REGULAR) {
		if (!path.dentry->inode->operations ||
		    !path.dentry->inode->operations->truncate) {
			status = VFS_ERR_NOTSUPP;
			goto fail;
		}
		lock = vfs_inode_write_lock(path.dentry->inode);
		if (lock)
			sleeplock_acquire(lock);
		status = path.dentry->inode->operations->truncate(
			path.dentry->inode, 0);
		if (lock)
			sleeplock_release(lock);
		if (status < 0)
			goto fail;
		stat.size = 0;
	}
	file = file_alloc();
	if (!file) {
		status = VFS_ERR_MFILE;
		goto fail;
	}
	file->path = path;
	file->operations = path.dentry->inode->file_operations;
	file->flags = flags;
	file->position = flags & VFS_OPEN_APPEND ? stat.size : 0;
	if (file->operations && file->operations->open) {
		status = file->operations->open(path.dentry->inode, file);
		if (status < 0) {
			file_close(file);
			return status;
		}
	}
	*result = file;
	return VFS_OK;

fail:
	vfs_path_put(&path);
	return status;
}

struct vfs_file *vfs_file_get(struct vfs_file *file)
{
	return file_dup(file);
}

void vfs_file_put(struct vfs_file *file)
{
	file_close(file);
}

int64 vfs_file_pread(struct vfs_file *file, int user_destination,
		     uint64 destination, uint64 count, uint64 offset)
{
	return file_read(file, user_destination, destination, count, &offset);
}

int vfs_open(const char *path, uint32 flags, uint32 mode, int *fd_out)
{
	file_t file;
	int fd, status;

	status = vfs_open_file(path, flags, mode, &file);
	if (status < 0)
		return status;
	fd = fd_alloc(file, 0, 0);
	if (fd < 0) {
		file_close(file);
		return VFS_ERR_MFILE;
	}
	*fd_out = fd;
	return VFS_OK;
}

int vfs_install_file(file_t file, uint8 flags, int *fd_out)
{
	int fd;

	if (!file || !fd_out)
		return VFS_ERR_INVAL;
	fd = fd_alloc(file, 0, flags);
	if (fd < 0)
		return VFS_ERR_MFILE;
	*fd_out = fd;
	return VFS_OK;
}

int vfs_get_file_fd(int fd, file_t *result)
{
	file_t file;

	if (!result)
		return VFS_ERR_INVAL;
	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	*result = file_dup(file);
	return VFS_OK;
}

uint32 vfs_file_poll(struct vfs_file *file, uint32 events)
{
	uint32 ready = 0;

	if (!file || !file->operations)
		return VFS_POLL_NVAL;
	if (file->operations->poll)
		return file->operations->poll(file, events);
	if ((events & VFS_POLL_IN) && file->operations->read)
		ready |= VFS_POLL_IN;
	if ((events & VFS_POLL_OUT) && file->operations->write)
		ready |= VFS_POLL_OUT;
	return ready;
}

int vfs_poll(struct vfs_pollfd *fds, uint32 count, int timeout_ms)
{
	file_t files[NOFILE];
	uint64 generation, start = ktime_get_ms();
	uint32 index;
	int ready, remaining;

	if (!fds || count > NOFILE)
		return VFS_ERR_INVAL;
	memset(files, 0, sizeof(files));
	for (index = 0; index < count; index++) {
		fds[index].revents = 0;
		if (fds[index].fd < 0)
			continue;
		if (vfs_get_file_fd(fds[index].fd, &files[index]) < 0)
			fds[index].revents = VFS_POLL_NVAL;
	}
	for (;;) {
		generation = vfs_poll_generation();
		ready = 0;
		for (index = 0; index < count; index++) {
			if (fds[index].fd < 0)
				continue;
			if (!files[index]) {
				fds[index].revents = VFS_POLL_NVAL;
				ready++;
				continue;
			}
			fds[index].revents = vfs_file_poll(
				files[index], fds[index].events);
			if (fds[index].revents)
				ready++;
		}
		if (ready || !timeout_ms)
			break;
		if (timeout_ms < 0)
			remaining = -1;
		else {
			uint64 elapsed = ktime_get_ms() - start;

			if (elapsed >= (uint32)timeout_ms)
				break;
			remaining = timeout_ms - elapsed;
		}
		vfs_poll_wait(generation, remaining);
	}
	for (index = 0; index < count; index++) {
		if (files[index])
			vfs_file_put(files[index]);
	}
	return ready;
}

uint64 vfs_poll_generation(void)
{
	uint64 generation;

	spinlock_acquire(&poll_state.lock);
	generation = poll_state.generation;
	spinlock_release(&poll_state.lock);
	return generation;
}

int vfs_poll_wait(uint64 generation, int timeout_ms)
{
	int result = 0;

	spinlock_acquire(&poll_state.lock);
	if (poll_state.generation != generation)
		goto out;
	if (!timeout_ms) {
		result = -1;
		goto out;
	}
	if (timeout_ms < 0)
		wait_queue_sleep(&poll_state.wait, &poll_state.lock);
	else
		result = wait_queue_sleep_timeout(
			&poll_state.wait, &poll_state.lock, timeout_ms);
out:
	spinlock_release(&poll_state.lock);
	return result;
}

void vfs_poll_notify(void)
{
	spinlock_acquire(&poll_state.lock);
	poll_state.generation++;
	wait_queue_wake_all(&poll_state.wait);
	spinlock_release(&poll_state.lock);
}

int vfs_close(int fd)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	cur_proc()->ofile[fd] = 0;
	cur_proc()->fd_flags[fd] = 0;
	file_close(file);
	vfs_poll_notify();
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
	int64 result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (length < 0)
		return VFS_ERR_INVAL;
	result = file_read(file, 1, address, length, &file->position);
	return result > 0x7fffffff ? VFS_ERR_INVAL : result;
}

static int vfs_prepare_append(file_t file)
{
	struct vfs_stat stat;
	int result;

	if (!(file->flags & VFS_OPEN_APPEND) || !file->path.dentry)
		return VFS_OK;
	result = vfs_inode_stat(file->path.dentry->inode, &stat);
	if (result < 0)
		return result;
	file->position = stat.size;
	return VFS_OK;
}

static sleeplock_t vfs_regular_write_lock(file_t file)
{
	if (!file->path.dentry)
		return 0;
	return vfs_inode_write_lock(file->path.dentry->inode);
}

int vfs_write(int fd, uint64 address, int length)
{
	sleeplock_t lock;
	file_t file;
	int64 result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (length < 0)
		return VFS_ERR_INVAL;
	lock = vfs_regular_write_lock(file);
	if (lock)
		sleeplock_acquire(lock);
	result = vfs_prepare_append(file);
	if (result >= 0)
		result = file_write(file, 1, address, length,
				    &file->position);
	if (lock)
		sleeplock_release(lock);
	return result > 0x7fffffff ? VFS_ERR_INVAL : result;
}

int64 vfs_writev(int fd, int user_source,
		 const struct vfs_iovec *iovecs, uint32 count)
{
	sleeplock_t lock;
	file_t file;
	uint64 total = 0;
	uint32 index;
	int64 result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!(file->flags & VFS_OPEN_WRITE))
		return VFS_ERR_BADF;
	for (index = 0; index < count; index++) {
		if (iovecs[index].length > 0x7fffffff)
			return VFS_ERR_INVAL;
	}
	lock = vfs_regular_write_lock(file);
	if (lock)
		sleeplock_acquire(lock);
	if (file->operations && file->operations->writev) {
		result = file->operations->writev(file, user_source, iovecs,
						  count);
		goto out;
	}
	result = vfs_prepare_append(file);
	if (result < 0)
		goto out;
	for (index = 0; index < count; index++) {
		result = file_write(file, user_source, iovecs[index].base,
				    iovecs[index].length, &file->position);
		if (result < 0) {
			result = total ? total : result;
			goto out;
		}
		total += result;
		if ((uint64)result != iovecs[index].length)
			break;
	}
	result = total;
out:
	if (lock)
		sleeplock_release(lock);
	return result;
}

int vfs_ftruncate(int fd, uint64 size)
{
	struct vfs_inode *inode;
	sleeplock_t lock;
	file_t file;
	int result;

	result = vfs_get_file_fd(fd, &file);
	if (result < 0)
		return result;
	if (!(file->flags & VFS_OPEN_WRITE)) {
		result = VFS_ERR_BADF;
		goto out;
	}
	if (!file->path.dentry ||
	    !(inode = file->path.dentry->inode) ||
	    inode->type != VFS_INODE_REGULAR || !inode->operations ||
	    !inode->operations->truncate) {
		result = VFS_ERR_INVAL;
		goto out;
	}
	lock = vfs_regular_write_lock(file);
	if (lock)
		sleeplock_acquire(lock);
	result = inode->operations->truncate(inode, size);
	if (lock)
		sleeplock_release(lock);
out:
	vfs_file_put(file);
	return result;
}

int64 vfs_ioctl(int fd, uint64 request, uint64 argument)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	return file_ioctl(file, request, argument);
}

int vfs_seek(int fd, int64 offset, int whence, uint64 *result)
{
	struct vfs_stat stat;
	file_t file;
	int64 next;
	int status;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->path.dentry)
		return VFS_ERR_SPIPE;
	if (whence == 0)
		next = offset;
	else if (whence == 1)
		next = (int64)file->position + offset;
	else if (whence == 2) {
		status = vfs_inode_stat(file->path.dentry->inode, &stat);
		if (status < 0)
			return status;
		next = (int64)stat.size + offset;
	} else {
		return VFS_ERR_INVAL;
	}
	if (next < 0)
		return VFS_ERR_INVAL;
	file->position = next;
	*result = next;
	return VFS_OK;
}

int vfs_stat_fd(int fd, struct vfs_stat *stat)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->path.dentry) {
		if (!file->operations || !file->operations->getattr)
			return VFS_ERR_INVAL;
		return file->operations->getattr(file, stat);
	}
	return vfs_inode_stat(file->path.dentry->inode, stat);
}

int vfs_stat_path(const char *name, int follow_symlink,
		  struct vfs_stat *stat)
{
	struct vfs_path path;
	uint32 flags = follow_symlink ? 0 : VFS_LOOKUP_NOFOLLOW_FINAL;
	int status = vfs_walk(name, flags, &path, 0);

	if (status < 0)
		return status;
	status = vfs_inode_stat(path.dentry->inode, stat);
	vfs_path_put(&path);
	return status;
}

int vfs_next_dirent(int fd, struct vfs_dirent *dirent)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->path.dentry)
		return VFS_ERR_NOTDIR;
	if (file->path.dentry->inode->type != VFS_INODE_DIRECTORY)
		return VFS_ERR_NOTDIR;
	if (!file->operations || !file->operations->readdir)
		return VFS_ERR_NOTSUPP;
	return file->operations->readdir(file, dirent);
}

int vfs_mkdir(const char *name, uint32 mode)
{
	struct vfs_inode *inode;
	struct vfs_path parent;
	char last[VFS_NAME_MAX + 1];
	int status;

	status = vfs_walk(name, VFS_LOOKUP_PARENT, &parent, last);
	if (status < 0)
		return status;
	if (!vfs_leaf_valid(last)) {
		vfs_path_put(&parent);
		return VFS_ERR_INVAL;
	}
	if (!parent.dentry->inode->operations ||
	    !parent.dentry->inode->operations->mkdir) {
		vfs_path_put(&parent);
		return VFS_ERR_NOTSUPP;
	}
	status = parent.dentry->inode->operations->mkdir(
		parent.dentry->inode, last, mode, &inode);
	if (status == VFS_OK)
		vfs_inode_put(inode);
	vfs_path_put(&parent);
	return status;
}

int vfs_unlink(const char *name, int remove_directory)
{
	struct vfs_path parent, target;
	const struct vfs_inode_operations *operations;
	char last[VFS_NAME_MAX + 1];
	int status;

	status = vfs_walk(name, VFS_LOOKUP_NOFOLLOW_FINAL, &target, 0);
	if (status < 0)
		return status;
	if (remove_directory &&
	    target.dentry->inode->type != VFS_INODE_DIRECTORY) {
		vfs_path_put(&target);
		return VFS_ERR_NOTDIR;
	}
	if (!remove_directory &&
	    target.dentry->inode->type == VFS_INODE_DIRECTORY) {
		vfs_path_put(&target);
		return VFS_ERR_ISDIR;
	}
	status = vfs_walk(name, VFS_LOOKUP_PARENT, &parent, last);
	if (status < 0) {
		vfs_path_put(&target);
		return status;
	}
	operations = parent.dentry->inode->operations;
	if (remove_directory) {
		status = operations && operations->rmdir ?
			operations->rmdir(parent.dentry->inode, last) :
			VFS_ERR_NOTSUPP;
	} else {
		status = operations && operations->unlink ?
			operations->unlink(parent.dentry->inode, last) :
			VFS_ERR_NOTSUPP;
	}
	vfs_path_put(&parent);
	vfs_path_put(&target);
	return status;
}

int vfs_link(const char *old_name, const char *new_name,
	     int follow_symlink)
{
	const struct vfs_inode_operations *operations;
	struct vfs_path source, parent;
	char last[VFS_NAME_MAX + 1];
	uint32 flags = follow_symlink ? 0 : VFS_LOOKUP_NOFOLLOW_FINAL;
	int status;

	status = vfs_walk(old_name, flags, &source, 0);
	if (status < 0)
		return status;
	if (source.dentry->inode->type == VFS_INODE_DIRECTORY) {
		vfs_path_put(&source);
		return VFS_ERR_PERM;
	}
	status = vfs_walk(new_name, VFS_LOOKUP_PARENT, &parent, last);
	if (status < 0) {
		vfs_path_put(&source);
		return status;
	}
	if (!vfs_leaf_valid(last)) {
		status = VFS_ERR_INVAL;
		goto out;
	}
	if (source.mount != parent.mount ||
	    source.dentry->inode->superblock !=
	    parent.dentry->inode->superblock) {
		status = VFS_ERR_XDEV;
		goto out;
	}
	operations = parent.dentry->inode->operations;
	status = operations && operations->link ?
		operations->link(source.dentry->inode,
		                 parent.dentry->inode, last) :
		VFS_ERR_NOTSUPP;
out:
	vfs_path_put(&parent);
	vfs_path_put(&source);
	return status;
}

int vfs_symlink(const char *target, const char *link_name)
{
	const struct vfs_inode_operations *operations;
	struct vfs_inode *inode;
	struct vfs_path parent;
	char last[VFS_NAME_MAX + 1];
	int status;

	if (!target || !target[0] || strlen(target) >= VFS_PATH_MAX)
		return VFS_ERR_NAMETOOLONG;
	status = vfs_walk(link_name, VFS_LOOKUP_PARENT, &parent, last);
	if (status < 0)
		return status;
	if (!vfs_leaf_valid(last)) {
		vfs_path_put(&parent);
		return VFS_ERR_INVAL;
	}
	operations = parent.dentry->inode->operations;
	status = operations && operations->symlink ?
		operations->symlink(parent.dentry->inode, last, target,
		                    &inode) : VFS_ERR_NOTSUPP;
	if (status == VFS_OK)
		vfs_inode_put(inode);
	vfs_path_put(&parent);
	return status;
}

int vfs_readlink(const char *name, char *buffer, uint32 size)
{
	const struct vfs_inode_operations *operations;
	struct vfs_path path;
	int status;

	if (!buffer || !size)
		return VFS_ERR_INVAL;
	status = vfs_walk(name, VFS_LOOKUP_NOFOLLOW_FINAL, &path, 0);
	if (status < 0)
		return status;
	if (path.dentry->inode->type != VFS_INODE_SYMLINK) {
		vfs_path_put(&path);
		return VFS_ERR_INVAL;
	}
	operations = path.dentry->inode->operations;
	status = operations && operations->readlink ?
		operations->readlink(path.dentry->inode, buffer, size) :
		VFS_ERR_NOTSUPP;
	vfs_path_put(&path);
	return status;
}

int vfs_rename(const char *old_name, const char *new_name,
	       uint32 flags)
{
	const struct vfs_inode_operations *operations;
	struct vfs_path source, old_parent, new_parent;
	char old_last[VFS_NAME_MAX + 1];
	char new_last[VFS_NAME_MAX + 1];
	int status;

	if (flags & ~VFS_RENAME_NOREPLACE)
		return VFS_ERR_INVAL;
	status = vfs_walk(old_name, VFS_LOOKUP_NOFOLLOW_FINAL, &source, 0);
	if (status < 0)
		return status;
	if (source.dentry == source.mount->root) {
		vfs_path_put(&source);
		return VFS_ERR_BUSY;
	}
	status = vfs_walk(old_name, VFS_LOOKUP_PARENT,
	                  &old_parent, old_last);
	if (status < 0)
		goto out_source;
	status = vfs_walk(new_name, VFS_LOOKUP_PARENT,
	                  &new_parent, new_last);
	if (status < 0)
		goto out_old_parent;
	if (!vfs_leaf_valid(old_last) || !vfs_leaf_valid(new_last)) {
		status = VFS_ERR_INVAL;
		goto out;
	}
	if (source.mount != old_parent.mount ||
	    old_parent.mount != new_parent.mount ||
	    old_parent.dentry->inode->superblock !=
	    new_parent.dentry->inode->superblock) {
		status = VFS_ERR_XDEV;
		goto out;
	}
	operations = old_parent.dentry->inode->operations;
	status = operations && operations->rename ?
		operations->rename(old_parent.dentry->inode, old_last,
		                   new_parent.dentry->inode, new_last,
		                   flags) : VFS_ERR_NOTSUPP;
out:
	vfs_path_put(&new_parent);
out_old_parent:
	vfs_path_put(&old_parent);
out_source:
	vfs_path_put(&source);
	return status;
}

int vfs_fsync(int fd)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->operations || !file->operations->fsync)
		return VFS_ERR_INVAL;
	return file->operations->fsync(file);
}

int vfs_sync(void)
{
	struct vfs_super_block *superblocks[VFS_SUPER_MAX];
	struct vfs_super_block *superblock;
	int count = 0, first_error = VFS_OK, status;

	spinlock_acquire(&vfs.lock);
	for (superblock = vfs.superblocks;
	     superblock != &vfs.superblocks[VFS_SUPER_MAX]; superblock++) {
		if (superblock->ref)
			superblocks[count++] = superblock;
	}
	spinlock_release(&vfs.lock);
	for (int i = 0; i < count; i++) {
		superblock = superblocks[i];
		if (!superblock->operations || !superblock->operations->sync)
			continue;
		status = superblock->operations->sync(superblock);
		if (status < 0 && first_error == VFS_OK)
			first_error = status;
	}
	return first_error;
}

int vfs_chdir(const char *name)
{
	process_t process = cur_proc();
	struct vfs_path path;
	int status = vfs_walk(name, 0, &path, 0);

	if (status < 0)
		return status;
	if (path.dentry->inode->type != VFS_INODE_DIRECTORY) {
		vfs_path_put(&path);
		return VFS_ERR_NOTDIR;
	}
	vfs_path_put(&process->cwd);
	process->cwd = path;
	return VFS_OK;
}

static int vfs_path_equal(const struct vfs_path *left,
			  const struct vfs_path *right)
{
	return left->mount == right->mount &&
	       left->dentry == right->dentry;
}

int vfs_getcwd(char *buffer, uint32 size)
{
	process_t process = cur_proc();
	struct vfs_path current, mountpoint;
	char temporary[VFS_PATH_MAX];
	const char *component;
	uint32 component_length, position = sizeof(temporary) - 1;
	uint32 length;

	if (!size)
		return VFS_ERR_INVAL;
	temporary[position] = 0;
	vfs_path_copy(&current, &process->cwd);
	while (!vfs_path_equal(&current, &process->root)) {
		if (current.dentry == current.mount->root &&
		    current.mount->parent) {
			vfs_path_copy(&mountpoint,
			              &current.mount->mountpoint);
			component = mountpoint.dentry->name;
			vfs_path_put(&current);
			current = mountpoint;
		} else {
			component = current.dentry->name;
		}
		component_length = strlen(component);
		if (!component_length || component_length + 1 > position) {
			vfs_path_put(&current);
			return VFS_ERR_NAMETOOLONG;
		}
		position -= component_length;
		memmove(temporary + position, component, component_length);
		temporary[--position] = '/';
		vfs_path_parent(&current);
	}
	vfs_path_put(&current);
	if (position == sizeof(temporary) - 1)
		temporary[--position] = '/';
	length = sizeof(temporary) - position;
	if (length > size)
		return VFS_ERR_NOSPC;
	memmove(buffer, temporary + position, length);
	return length;
}

int vfs_access(const char *name)
{
	struct vfs_path path;
	int status = vfs_walk(name, 0, &path, 0);

	if (status == VFS_OK)
		vfs_path_put(&path);
	return status;
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

int vfs_set_file_flags(int fd, uint32 flags)
{
	const uint32 mutable = VFS_OPEN_APPEND | VFS_OPEN_NONBLOCK;
	file_t file;
	uint32 next;
	int status;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	next = (file->flags & ~mutable) | (flags & mutable);
	if (file->operations && file->operations->set_flags) {
		status = file->operations->set_flags(file, next);
		if (status < 0)
			return status;
	}
	file->flags = next;
	return VFS_OK;
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
