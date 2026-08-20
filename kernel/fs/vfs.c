#include <debug.h>
#include <device.h>
#include <file.h>
#include <ktime.h>
#include <mystring.h>
#include <mmap.h>
#include <page_cache.h>
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

#define VFS_MAX_FILE_OFFSET 0x7fffffffffffffffULL

#define VFS_LOOKUP_PARENT (1U << 0)
#define VFS_LOOKUP_NOFOLLOW_FINAL (1U << 1)

#define VFS_INODE_ACCESS_WRITE (1U << 0)
#define VFS_INODE_ACCESS_EXEC  (1U << 1)

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
	struct sleeplock mount_lock;
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

static int vfs_credentials_in_group(
	const struct process_credentials *credentials, uint32 gid,
	int use_real_ids)
{
	uint32 primary = use_real_ids ? credentials->gid : credentials->fsgid;
	uint32 index;

	if (primary == gid)
		return 1;
	for (index = 0; index < credentials->group_count; index++) {
		if (credentials->groups[index] == gid)
			return 1;
	}
	return 0;
}

static int vfs_inode_permission_credentials(
	const struct vfs_inode *inode, uint32 access,
	const struct process_credentials *credentials, int use_real_ids)
{
	uint32 bits;
	uint32 uid = use_real_ids ? credentials->uid : credentials->fsuid;

	if (!access)
		return VFS_OK;
	if (!uid) {
		if ((access & VFS_ACCESS_EXEC) &&
		    inode->type != VFS_INODE_DIRECTORY &&
		    !(inode->mode & 0111))
			return VFS_ERR_ACCES;
		return VFS_OK;
	}
	if (uid == inode->uid)
		bits = (inode->mode >> 6) & 7;
	else if (vfs_credentials_in_group(credentials, inode->gid,
					  use_real_ids))
		bits = (inode->mode >> 3) & 7;
	else
		bits = inode->mode & 7;
	return (bits & access) == access ? VFS_OK : VFS_ERR_ACCES;
}

static int vfs_inode_permission(const struct vfs_inode *inode,
				uint32 access)
{
	struct process_credentials credentials;

	process_credentials_get(&credentials);
	return vfs_inode_permission_credentials(inode, access, &credentials, 0);
}

static int vfs_inode_same_identity(const struct vfs_inode *left,
				   const struct vfs_inode *right)
{
	return left->superblock == right->superblock &&
	       left->number == right->number;
}

static void vfs_inode_sync_metadata(struct vfs_inode *source)
{
	struct vfs_inode *inode;

	spinlock_acquire(&vfs.lock);
	for (inode = vfs.inodes;
	     inode != &vfs.inodes[VFS_INODE_MAX]; inode++) {
		if (inode->ref < 1 ||
		    !vfs_inode_same_identity(inode, source))
			continue;
		inode->mode = source->mode;
		inode->uid = source->uid;
		inode->gid = source->gid;
		inode->ctime = source->ctime;
	}
	spinlock_release(&vfs.lock);
}

static int vfs_inode_apply_attributes(
	struct vfs_inode *inode, const struct vfs_iattr *attributes)
{
	int result = inode->operations->setattr(inode, attributes);

	if (result == VFS_OK)
		vfs_inode_sync_metadata(inode);
	return result;
}

static int vfs_inode_access_acquire(struct vfs_inode *inode, uint8 access)
{
	struct vfs_inode *candidate;
	uint32 writers = 0, executors = 0;
	int result = VFS_OK;

	if (!access || !inode || inode->type != VFS_INODE_REGULAR)
		return VFS_OK;
	spinlock_acquire(&vfs.lock);
	for (candidate = vfs.inodes;
	     candidate != &vfs.inodes[VFS_INODE_MAX]; candidate++) {
		if (candidate->ref < 1 ||
		    !vfs_inode_same_identity(candidate, inode))
			continue;
		writers += candidate->write_open_count;
		executors += candidate->exec_open_count;
	}
	if ((access & VFS_INODE_ACCESS_WRITE) && executors)
		result = VFS_ERR_TXTBSY;
	else if ((access & VFS_INODE_ACCESS_EXEC) && writers)
		result = VFS_ERR_TXTBSY;
	else if ((access & VFS_INODE_ACCESS_WRITE) &&
		 inode->write_open_count == ~(uint32)0)
		result = VFS_ERR_MFILE;
	else if ((access & VFS_INODE_ACCESS_EXEC) &&
		 inode->exec_open_count == ~(uint32)0)
		result = VFS_ERR_MFILE;
	else {
		if (access & VFS_INODE_ACCESS_WRITE)
			inode->write_open_count++;
		if (access & VFS_INODE_ACCESS_EXEC)
			inode->exec_open_count++;
	}
	spinlock_release(&vfs.lock);
	return result;
}

static void vfs_inode_access_release(struct vfs_inode *inode, uint8 access)
{
	if (!access)
		return;
	spinlock_acquire(&vfs.lock);
	if (!inode || inode->ref < 1 ||
	    ((access & VFS_INODE_ACCESS_WRITE) && !inode->write_open_count) ||
	    ((access & VFS_INODE_ACCESS_EXEC) && !inode->exec_open_count))
		PANIC("VFS inode access release");
	if (access & VFS_INODE_ACCESS_WRITE)
		inode->write_open_count--;
	if (access & VFS_INODE_ACCESS_EXEC)
		inode->exec_open_count--;
	spinlock_release(&vfs.lock);
}

void vfs_file_release_inode_access(struct vfs_file *file)
{
	struct vfs_inode *inode = 0;

	if (!file || !file->inode_access)
		return;
	if (file->path.dentry)
		inode = file->path.dentry->inode;
	vfs_inode_access_release(inode, file->inode_access);
	file->inode_access = 0;
}

int vfs_exec_mapping_get(struct vfs_file *file)
{
	struct vfs_inode *inode;

	if (!file || !file->path.dentry ||
	    !(inode = file->path.dentry->inode))
		return VFS_ERR_INVAL;
	return vfs_inode_access_acquire(inode, VFS_INODE_ACCESS_EXEC);
}

void vfs_exec_mapping_put(struct vfs_file *file)
{
	struct vfs_inode *inode;

	if (!file || !file->path.dentry ||
	    !(inode = file->path.dentry->inode))
		PANIC("VFS exec mapping release");
	vfs_inode_access_release(inode, VFS_INODE_ACCESS_EXEC);
}

static int vfs_inode_remove_privileges(struct vfs_inode *inode)
{
	struct process_credentials credentials;
	struct vfs_iattr attributes;
	struct vfs_stat stat;
	sleeplock_t lock;
	uint32 clear;
	int result;

	if (!inode || inode->type != VFS_INODE_REGULAR)
		return VFS_OK;
	process_credentials_get(&credentials);
	if (!credentials.euid)
		return VFS_OK;
	lock = inode->superblock ? &inode->superblock->attribute_lock : 0;
	if (lock)
		sleeplock_acquire(lock);
	result = vfs_inode_stat(inode, &stat);
	if (result < 0)
		goto out;
	clear = stat.mode & 04000;
	if ((stat.mode & 02010) == 02010)
		clear |= 02000;
	if (!clear) {
		result = VFS_OK;
		goto out;
	}
	if (!inode->operations || !inode->operations->setattr) {
		result = VFS_ERR_NOTSUPP;
		goto out;
	}
	memset(&attributes, 0, sizeof(attributes));
	attributes.mask = VFS_ATTR_MODE;
	attributes.mode = stat.mode & ~clear;
	result = vfs_inode_apply_attributes(inode, &attributes);
out:
	if (lock)
		sleeplock_release(lock);
	return result;
}

static int vfs_iov_has_data(const struct vfs_iovec *iovecs, uint32 count)
{
	uint32 index;

	for (index = 0; index < count; index++) {
		if (iovecs[index].length)
			return 1;
	}
	return 0;
}
static void vfs_creation_credentials(const struct vfs_inode *parent,
				     uint32 *mode, uint32 *uid,
				     uint32 *gid, int directory)
{
	struct process_credentials credentials;

	process_credentials_get(&credentials);
	*uid = credentials.fsuid;
	*gid = parent->mode & 02000 ? parent->gid : credentials.fsgid;
	if (directory && parent->mode & 02000)
		*mode |= 02000;
	else if (credentials.fsuid && (*mode & 02000) &&
		 !vfs_credentials_in_group(&credentials, *gid, 0))
		*mode &= ~02000;
}

static int vfs_mutation_permission(const struct vfs_inode *directory)
{
	if (!directory || directory->type != VFS_INODE_DIRECTORY)
		return VFS_ERR_NOTDIR;
	return vfs_inode_permission(directory,
				    VFS_ACCESS_WRITE | VFS_ACCESS_EXEC);
}

static int vfs_sticky_permission(const struct vfs_inode *directory,
				 const struct vfs_inode *target)
{
	struct process_credentials credentials;

	if (!(directory->mode & 01000))
		return VFS_OK;
	process_credentials_get(&credentials);
	if (!credentials.fsuid || credentials.fsuid == directory->uid ||
	    (target && credentials.fsuid == target->uid))
		return VFS_OK;
	return VFS_ERR_PERM;
}

static sleeplock_t vfs_inode_write_lock(struct vfs_inode *inode)
{
	if (!inode || inode->type != VFS_INODE_REGULAR ||
	    !inode->superblock)
		return 0;
	return &inode->superblock->write_lock;
}

int vfs_file_mark_shared_dirty(struct vfs_file *file, uint64 offset)
{
	struct vfs_inode *inode;
	int result;

	if (!file || !file->path.dentry ||
	    !(inode = file->path.dentry->inode))
		return VFS_ERR_INVAL;
	result = vfs_inode_remove_privileges(inode);
	return result < 0 ? result : page_cache_mark_dirty(file, offset);
}

static int vfs_truncate_inode(struct vfs_inode *inode, uint64 size)
{
	struct vfs_stat stat;
	sleeplock_t lock;
	int result;

	if (!inode || inode->type != VFS_INODE_REGULAR ||
	    !inode->operations || !inode->operations->truncate)
		return VFS_ERR_INVAL;
	result = vfs_inode_access_acquire(inode, VFS_INODE_ACCESS_WRITE);
	if (result < 0)
		return result;
	lock = vfs_inode_write_lock(inode);
	if (lock)
		sleeplock_acquire(lock);
	if (vfs_inode_stat(inode, &stat) < 0)
		result = VFS_ERR_IO;
	else if (page_cache_writeback_inode_locked(inode) < 0)
		result = VFS_ERR_IO;
	else {
		result = stat.size == size ? VFS_OK :
			vfs_inode_remove_privileges(inode);
		if (result >= 0)
			result = inode->operations->truncate(inode, size);
	}
	if (result >= 0 && mmap_file_truncate(inode, stat.size, size) < 0)
		result = VFS_ERR_IO;
	if (lock)
		sleeplock_release(lock);
	vfs_inode_access_release(inode, VFS_INODE_ACCESS_WRITE);
	return result;
}

void vfs_init(void)
{
	spinlock_init(&vfs.lock, "vfs");
	sleeplock_init(&vfs.mount_lock, "VFS mount");
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
			sleeplock_init(&superblock->attribute_lock,
				       "VFS attributes");
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
	if (inode->write_open_count || inode->exec_open_count)
		PANIC("release accessed VFS inode");
	inode->ref = -1;
	operations = inode->superblock->operations;
	spinlock_release(&vfs.lock);

	if (operations && operations->put_inode)
		operations->put_inode(inode);
	spinlock_acquire(&vfs.lock);
	memset(inode, 0, sizeof(*inode));
	spinlock_release(&vfs.lock);
}

static void vfs_super_destroy(struct vfs_super_block *superblock)
{
	struct block_device *device;
	struct vfs_inode *root;

	if (!superblock)
		return;
	device = superblock->device;
	root = superblock->root;
	superblock->root = 0;
	if (root)
		vfs_inode_put(root);
	if (superblock->operations && superblock->operations->unmount)
		superblock->operations->unmount(superblock);
	vfs_super_free(superblock);
	block_device_close(device);
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
	stat->atime = inode->atime;
	stat->mtime = inode->mtime;
	stat->ctime = inode->ctime;
	return VFS_OK;
}

int vfs_current_time(struct vfs_timespec *time)
{
	uint64 nanoseconds;

	if (!time || ktime_get_realtime_ns(&nanoseconds) < 0)
		return VFS_ERR_IO;
	time->seconds = nanoseconds / NSEC_PER_SEC;
	time->nanoseconds = nanoseconds % NSEC_PER_SEC;
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

static void vfs_mount_free(struct vfs_mount *mount)
{
	if (!mount)
		return;
	spinlock_acquire(&vfs.lock);
	if (mount->ref != 1 || mount->attached)
		PANIC("vfs mount free");
	memset(mount, 0, sizeof(*mount));
	spinlock_release(&vfs.lock);
}

void vfs_path_copy(struct vfs_path *destination,
		   const struct vfs_path *source)
{
	if (!source || !source->mount || !source->dentry)
		PANIC("vfs path copy");
	spinlock_acquire(&vfs.lock);
	if (source->mount->ref < 1 || source->dentry->ref < 1)
		PANIC("vfs path copy reference");
	destination->mount = source->mount;
	destination->dentry = source->dentry;
	destination->dentry->ref++;
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

static void vfs_mount_destroy(struct vfs_mount *mount)
{
	struct vfs_super_block *superblock;

	if (!mount)
		return;
	superblock = mount->superblock;
	if (mount->root)
		vfs_dentry_put(mount->root);
	mount->root = 0;
	vfs_super_destroy(superblock);
	mount->superblock = 0;
	vfs_path_put(&mount->mountpoint);
	vfs_mount_free(mount);
}

static int vfs_format_path_locked(const struct vfs_path *path,
				  char *buffer, uint32 size)
{
	struct vfs_mount *mount;
	struct vfs_dentry *dentry;
	char temporary[VFS_PATH_MAX];
	const char *component;
	uint32 component_length, length;
	uint32 position = sizeof(temporary) - 1;

	if (!path || !path->mount || !path->dentry || !vfs.root || !size)
		return VFS_ERR_INVAL;
	mount = path->mount;
	dentry = path->dentry;
	temporary[position] = 0;
	while (mount != vfs.root || dentry != vfs.root->root) {
		if (dentry == mount->root && mount->parent) {
			dentry = mount->mountpoint.dentry;
			mount = mount->mountpoint.mount;
		}
		component = dentry->name;
		component_length = strlen(component);
		if (!component_length || component_length + 1 > position)
			return VFS_ERR_NAMETOOLONG;
		position -= component_length;
		memmove(temporary + position, component, component_length);
		temporary[--position] = '/';
		dentry = dentry->parent;
		if (!dentry)
			return VFS_ERR_INVAL;
	}
	if (position == sizeof(temporary) - 1)
		temporary[--position] = '/';
	length = sizeof(temporary) - position;
	if (length > size)
		return VFS_ERR_NOSPC;
	memmove(buffer, temporary + position, length);
	return VFS_OK;
}

int vfs_get_root(struct vfs_path *path)
{
	spinlock_acquire(&vfs.lock);
	if (!vfs.root || !vfs.root->attached) {
		spinlock_release(&vfs.lock);
		return VFS_ERR_NOENT;
	}
	path->mount = vfs.root;
	path->dentry = vfs.root->root;
	path->mount->ref++;
	path->dentry->ref++;
	spinlock_release(&vfs.lock);
	return VFS_OK;
}

static int vfs_mount_type(struct vfs_filesystem_type *type,
			  uint32 device_id, const void *data,
			  struct vfs_super_block **result)
{
	struct block_device *device = 0;
	int status;

	if (!type || !result)
		return VFS_ERR_INVAL;
	*result = 0;
	if (type->flags & VFS_FS_REQUIRES_DEVICE) {
		device = block_device_open(device_id);
		if (!device)
			return VFS_ERR_NODEV;
	}
	status = type->mount(type, device, data, result);
	if (status < 0 || !*result) {
		block_device_close(device);
		if (status >= 0)
			status = VFS_ERR_IO;
	}
	return status;
}

int vfs_mount_root(const char *filesystem, uint32 device_id,
		   const void *data)
{
	struct vfs_filesystem_type *type;
	struct vfs_super_block *superblock = 0;
	struct vfs_dentry *root;
	struct vfs_mount *mount;
	int result;

	spinlock_acquire(&vfs.lock);
	result = vfs.root ? VFS_ERR_BUSY : VFS_OK;
	spinlock_release(&vfs.lock);
	if (result < 0)
		return result;
	type = vfs_find_filesystem(filesystem);
	if (!type)
		return VFS_ERR_NODEV;
	result = vfs_mount_type(type, device_id, data, &superblock);
	if (result < 0)
		return result;
	if (!superblock || !superblock->root) {
		vfs_super_destroy(superblock);
		return VFS_ERR_IO;
	}
	root = vfs_dentry_alloc(0, "", superblock->root);
	if (!root) {
		vfs_super_destroy(superblock);
		return VFS_ERR_NOMEM;
	}
	mount = vfs_mount_alloc();
	if (!mount) {
		vfs_dentry_put(root);
		vfs_super_destroy(superblock);
		return VFS_ERR_NOMEM;
	}
	mount->superblock = superblock;
	mount->root = root;
	spinlock_acquire(&vfs.lock);
	if (vfs.root) {
		result = VFS_ERR_BUSY;
	} else {
		mount->attached = 1;
		vfs.root = mount;
		result = VFS_OK;
	}
	spinlock_release(&vfs.lock);
	if (result < 0) {
		vfs_dentry_put(root);
		vfs_super_destroy(superblock);
		vfs_mount_free(mount);
		return result;
	}
	pr_info("VFS: mounted root (%s) on %s", filesystem,
		superblock->device ? superblock->device->name : "none");
	return VFS_OK;
}

static int vfs_same_inode(struct vfs_inode *left,
			  struct vfs_inode *right)
{
	return vfs_inode_same_identity(left, right);
}

static struct vfs_mount *vfs_child_mount_locked(
	const struct vfs_path *path)
{
	struct vfs_mount *mount;

	for (mount = vfs.mounts;
	     mount != &vfs.mounts[VFS_MOUNT_MAX]; mount++) {
		if (mount->attached && mount->parent == path->mount &&
		    mount->mountpoint.dentry &&
		    vfs_same_inode(mount->mountpoint.dentry->inode,
		                   path->dentry->inode))
			return mount;
	}
	return 0;
}

static int vfs_has_child_mount(const struct vfs_path *path)
{
	int result;

	spinlock_acquire(&vfs.lock);
	result = vfs_child_mount_locked(path) != 0;
	spinlock_release(&vfs.lock);
	return result;
}

static int vfs_child_path(const struct vfs_path *path,
			  struct vfs_path *child)
{
	struct vfs_mount *mount;

	spinlock_acquire(&vfs.lock);
	mount = vfs_child_mount_locked(path);
	if (mount) {
		mount->ref++;
		mount->root->ref++;
		child->mount = mount;
		child->dentry = mount->root;
	}
	spinlock_release(&vfs.lock);
	return mount != 0;
}

static void vfs_follow_mount(struct vfs_path *path)
{
	struct vfs_path next;

	while (vfs_child_path(path, &next)) {
		vfs_path_put(path);
		*path = next;
	}
}

static int vfs_start_path(const char *name, const struct vfs_path *base,
			  struct vfs_path *path)
{
	process_t process = cur_proc();

	if (name[0] == '/') {
		if (process && process->root.dentry) {
			vfs_path_copy(path, &process->root);
			return VFS_OK;
		}
		return vfs_get_root(path);
	}
	if (base) {
		vfs_path_copy(path, base);
		return VFS_OK;
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

static int vfs_walk_credentials(
	const char *name, uint32 flags, struct vfs_path *result, char *last,
	const struct process_credentials *credentials,
	const struct vfs_path *base)
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
	status = vfs_start_path(buffer->pending, base, &current);
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
		if (current.dentry->inode->type != VFS_INODE_DIRECTORY) {
			status = VFS_ERR_NOTDIR;
			goto fail;
		}
		status = credentials ? vfs_inode_permission_credentials(
			current.dentry->inode, VFS_ACCESS_EXEC, credentials, 0) :
			vfs_inode_permission(current.dentry->inode,
					     VFS_ACCESS_EXEC);
		if (status < 0)
			goto fail;
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
				status = vfs_start_path(buffer->pending, base,
							&current);
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

static int vfs_walk(const char *name, uint32 flags, struct vfs_path *result,
		    char *last)
{
	return vfs_walk_credentials(name, flags, result, last, 0, 0);
}

static int vfs_walk_from(const char *name, uint32 flags,
			 const struct vfs_path *base,
			 struct vfs_path *result, char *last)
{
	return vfs_walk_credentials(name, flags, result, last, 0, base);
}

static int vfs_walk_base(const char *name, uint32 flags,
			 const struct vfs_path *base,
			 struct vfs_path *result, char *last)
{
	return base ? vfs_walk_from(name, flags, base, result, last) :
		vfs_walk(name, flags, result, last);
}

static int vfs_mount_locked(const char *filesystem,
			    uint32 device_id,
			    const char *target, const void *data)
{
	struct vfs_filesystem_type *type;
	struct vfs_super_block *superblock = 0;
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
	if (vfs_has_child_mount(&mountpoint)) {
		vfs_path_put(&mountpoint);
		return VFS_ERR_BUSY;
	}
	status = vfs_mount_type(type, device_id, data, &superblock);
	if (status < 0) {
		vfs_path_put(&mountpoint);
		return status;
	}
	if (!superblock || !superblock->root) {
		vfs_super_destroy(superblock);
		vfs_path_put(&mountpoint);
		return VFS_ERR_IO;
	}
	root = vfs_dentry_alloc(0, "", superblock->root);
	if (!root) {
		vfs_super_destroy(superblock);
		vfs_path_put(&mountpoint);
		return VFS_ERR_NOMEM;
	}
	mount = vfs_mount_alloc();
	if (!mount) {
		vfs_dentry_put(root);
		vfs_super_destroy(superblock);
		vfs_path_put(&mountpoint);
		return VFS_ERR_NOMEM;
	}
	mount->superblock = superblock;
	mount->root = root;
	mount->parent = mountpoint.mount;
	mount->mountpoint = mountpoint;
	spinlock_acquire(&vfs.lock);
	if (vfs_child_mount_locked(&mount->mountpoint)) {
		status = VFS_ERR_BUSY;
	} else {
		mount->attached = 1;
		status = VFS_OK;
	}
	spinlock_release(&vfs.lock);
	if (status < 0) {
		vfs_mount_destroy(mount);
		return status;
	}
	pr_info("VFS: mounted %s on %s", filesystem, target);
	return VFS_OK;
}

static int vfs_mount_id(const char *filesystem, uint32 device_id,
			const char *target, const void *data)
{
	int status;

	sleeplock_acquire(&vfs.mount_lock);
	status = vfs_mount_locked(filesystem, device_id, target, data);
	sleeplock_release(&vfs.mount_lock);
	return status;
}

int vfs_mount(const char *filesystem, uint32 device_id,
	      const char *target, const void *data)
{
	return vfs_mount_id(filesystem, device_id, target, data);
}

static int vfs_block_device_path(const char *source,
				 uint32 *result)
{
	struct vfs_inode *inode;
	struct vfs_path path;
	uint32 minor;
	int status;

	status = vfs_walk(source, 0, &path, 0);
	if (status < 0)
		return status;
	inode = path.dentry->inode;
	if (inode->type != VFS_INODE_BLOCK_DEVICE) {
		status = VFS_ERR_NODEV;
		goto out;
	}
	minor = VFS_DEVICE_MINOR(inode->device);
	if (VFS_DEVICE_MAJOR(inode->device) != BLOCK_DEVICE_NODE_MAJOR ||
	    minor >= BLOCK_DEVICE_MAX - 1) {
		status = VFS_ERR_NODEV;
		goto out;
	}
	*result = minor + 1;
	status = VFS_OK;
out:
	vfs_path_put(&path);
	return status;
}

int vfs_mount_path(const char *filesystem, const char *source,
		   const char *target, const void *data)
{
	struct vfs_filesystem_type *type;
	uint32 device_id = 0;
	int status;

	type = vfs_find_filesystem(filesystem);
	if (!type)
		return VFS_ERR_NODEV;
	if (type->flags & VFS_FS_REQUIRES_DEVICE) {
		if (!source)
			return VFS_ERR_NODEV;
		status = vfs_block_device_path(source, &device_id);
		if (status < 0)
			return status;
	}
	return vfs_mount_id(filesystem, device_id, target, data);
}

static int vfs_mount_has_children_locked(struct vfs_mount *parent)
{
	struct vfs_mount *mount;

	for (mount = vfs.mounts;
	     mount != &vfs.mounts[VFS_MOUNT_MAX]; mount++) {
		if (mount->attached && mount->parent == parent)
			return 1;
	}
	return 0;
}

static int vfs_unmount_locked(const char *target, uint32 flags)
{
	struct vfs_super_block *superblock;
	struct vfs_mount *mount;
	struct vfs_path path;
	char resolved_target[VFS_PATH_MAX];
	int status;

	if (flags)
		return VFS_ERR_INVAL;
	status = vfs_walk(target, 0, &path, 0);
	if (status < 0)
		return status;
	mount = path.mount;
	superblock = mount->superblock;
	if (mount == vfs.root) {
		status = VFS_ERR_BUSY;
		goto out;
	}
	if (!mount->parent || path.dentry != mount->root) {
		status = VFS_ERR_INVAL;
		goto out;
	}
	status = page_cache_evict_super(superblock);
	if (status < 0)
		goto out;
	if (superblock->operations && superblock->operations->sync) {
		status = superblock->operations->sync(superblock);
		if (status < 0)
			goto out;
	}
	spinlock_acquire(&vfs.lock);
	if (!mount->attached || vfs_mount_has_children_locked(mount) ||
	    mount->ref != 2) {
		status = VFS_ERR_BUSY;
	} else {
		if (vfs_format_path_locked(&mount->mountpoint,
		                           resolved_target,
		                           sizeof(resolved_target)) < 0)
			resolved_target[0] = 0;
		mount->attached = 0;
		status = VFS_OK;
	}
	spinlock_release(&vfs.lock);
	if (status < 0)
		goto out;
	pr_info("VFS: unmounted %s from %s",
		mount->superblock->type->name,
		resolved_target[0] ? resolved_target : target);
	vfs_path_put(&path);
	vfs_mount_destroy(mount);
	return VFS_OK;
out:
	vfs_path_put(&path);
	return status;
}

int vfs_unmount(const char *target, uint32 flags)
{
	int status;

	sleeplock_acquire(&vfs.mount_lock);
	status = vfs_unmount_locked(target, flags);
	sleeplock_release(&vfs.mount_lock);
	return status;
}

uint32 vfs_snapshot_mounts(struct vfs_mount_snapshot *snapshots,
			   uint32 capacity)
{
	struct vfs_mount *mount;
	uint32 count = 0;

	if (!snapshots || !capacity)
		return 0;
	spinlock_acquire(&vfs.lock);
	for (mount = vfs.mounts;
	     mount != &vfs.mounts[VFS_MOUNT_MAX] && count < capacity;
	     mount++) {
		struct vfs_mount_snapshot *snapshot;

		if (!mount->attached || !mount->superblock ||
		    !mount->superblock->type)
			continue;
		snapshot = &snapshots[count++];
		memset(snapshot, 0, sizeof(*snapshot));
		safe_strncpy(snapshot->filesystem,
			     mount->superblock->type->name,
			     sizeof(snapshot->filesystem));
		if (mount->superblock->device)
			safe_strncpy(snapshot->source,
				     mount->superblock->device->name,
				     sizeof(snapshot->source));
		else
			safe_strncpy(snapshot->source, snapshot->filesystem,
				     sizeof(snapshot->source));
		if (mount == vfs.root) {
			struct vfs_path root = {
				.mount = mount,
				.dentry = mount->root,
			};

			if (vfs_format_path_locked(&root, snapshot->target,
						   sizeof(snapshot->target)) < 0)
				snapshot->target[0] = 0;
		} else if (vfs_format_path_locked(&mount->mountpoint,
						  snapshot->target,
						  sizeof(snapshot->target)) < 0) {
			snapshot->target[0] = 0;
		}
		snapshot->flags = mount->flags;
	}
	spinlock_release(&vfs.lock);
	return count;
}

static int fd_alloc(file_t file, int minimum, uint8 flags)
{
	process_t process = cur_proc();
	int fd;

	if (minimum < 0)
		minimum = 0;
	spinlock_acquire(&process->files_lock);
	for (fd = minimum; fd < NOFILE; fd++) {
		if (!process->ofile[fd]) {
			process->ofile[fd] = file;
			process->fd_flags[fd] = flags;
			spinlock_release(&process->files_lock);
			return fd;
		}
	}
	spinlock_release(&process->files_lock);
	return -1;
}

static int fd_get(int fd, file_t *file)
{
	process_t process = cur_proc();

	if (fd < 0 || fd >= NOFILE)
		return VFS_ERR_BADF;
	spinlock_acquire(&process->files_lock);
	if (!process->ofile[fd]) {
		spinlock_release(&process->files_lock);
		return VFS_ERR_BADF;
	}
	*file = file_dup(process->ofile[fd]);
	spinlock_release(&process->files_lock);
	return VFS_OK;
}

static int vfs_directory_fd_get(int fd, file_t *directory)
{
	int status;

	status = fd_get(fd, directory);
	if (status < 0)
		return status;
	if (!(*directory)->path.dentry ||
	    (*directory)->path.dentry->inode->type != VFS_INODE_DIRECTORY) {
		file_close(*directory);
		*directory = 0;
		return VFS_ERR_NOTDIR;
	}
	return VFS_OK;
}

static int vfs_leaf_valid(const char *name)
{
	return name && name[0] && !string_equal(name, ".") &&
	       !string_equal(name, "..");
}

static int vfs_create_path_from(const char *name, uint32 mode,
				const struct vfs_path *base,
				struct vfs_path *result)
{
	struct vfs_inode *inode;
	struct vfs_dentry *dentry;
	struct vfs_path parent;
	char last[VFS_NAME_MAX + 1];
	uint32 uid, gid;
	int status;

	status = vfs_walk_base(name, VFS_LOOKUP_PARENT, base, &parent, last);
	if (status < 0)
		return status;
	if (!vfs_leaf_valid(last)) {
		vfs_path_put(&parent);
		return VFS_ERR_INVAL;
	}
	status = vfs_mutation_permission(parent.dentry->inode);
	if (status < 0) {
		vfs_path_put(&parent);
		return status;
	}
	if (!parent.dentry->inode->operations ||
	    !parent.dentry->inode->operations->create) {
		vfs_path_put(&parent);
		return VFS_ERR_NOTSUPP;
	}
	vfs_creation_credentials(parent.dentry->inode, &mode, &uid, &gid, 0);
	status = parent.dentry->inode->operations->create(
		parent.dentry->inode, last, mode, uid, gid, &inode);
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

static int vfs_create_path(const char *name, uint32 mode,
			   struct vfs_path *result)
{
	return vfs_create_path_from(name, mode, 0, result);
}

int vfs_open_file(const char *name, uint32 flags, uint32 mode,
		  file_t *result)
{
	struct vfs_stat stat;
	struct vfs_path path;
	file_t file;
	uint8 inode_access = 0;
	uint32 access = 0;
	int existed, status;

	if ((flags & VFS_OPEN_EXEC) &&
	    (flags & (VFS_OPEN_WRITE | VFS_OPEN_TRUNCATE)))
		return VFS_ERR_INVAL;
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
	if (flags & VFS_OPEN_EXEC)
		access |= VFS_ACCESS_EXEC;
	else {
		if (flags & VFS_OPEN_READ)
			access |= VFS_ACCESS_READ;
		if (flags & VFS_OPEN_WRITE)
			access |= VFS_ACCESS_WRITE;
		if (flags & VFS_OPEN_TRUNCATE)
			access |= VFS_ACCESS_WRITE;
	}
	if (existed) {
		status = vfs_inode_permission(path.dentry->inode, access);
		if (status < 0)
			goto fail;
	}
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
	file = file_alloc();
	if (!file) {
		status = VFS_ERR_MFILE;
		goto fail;
	}
	file->path = path;
	file->operations = path.dentry->inode->file_operations;
	file->capabilities = file->operations ? file->operations->flags : 0;
	file->flags = flags | (flags & VFS_OPEN_EXEC ? VFS_OPEN_READ : 0);
	if (stat.type == VFS_INODE_REGULAR) {
		if (flags & VFS_OPEN_EXEC)
			inode_access = VFS_INODE_ACCESS_EXEC;
		else if (flags & (VFS_OPEN_WRITE | VFS_OPEN_TRUNCATE))
			inode_access = VFS_INODE_ACCESS_WRITE;
	}
	status = vfs_inode_access_acquire(path.dentry->inode, inode_access);
	if (status < 0)
		goto fail_file;
	file->inode_access = inode_access;
	file->access_ref = inode_access ? 1 : 0;
	if ((flags & VFS_OPEN_TRUNCATE) &&
	    stat.type == VFS_INODE_REGULAR) {
		status = vfs_truncate_inode(path.dentry->inode, 0);
		if (status < 0)
			goto fail_file;
		stat.size = 0;
	}
	file->position = flags & VFS_OPEN_APPEND ? stat.size : 0;
	if (file->operations && file->operations->open) {
		status = file->operations->open(path.dentry->inode, file);
		if (status < 0)
			goto fail_file;
	}
	*result = file;
	return VFS_OK;

fail_file:
	file_close(file);
	return status;
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

struct vfs_file *vfs_file_hold(struct vfs_file *file)
{
	return file_hold(file);
}

void vfs_file_unhold(struct vfs_file *file)
{
	file_unhold(file);
}

int64 vfs_file_pread_raw(struct vfs_file *file, int user_destination,
			 uint64 destination, uint64 count, uint64 offset)
{
	if (file && file->path.dentry && file->path.dentry->inode &&
	    file->path.dentry->inode->type == VFS_INODE_DIRECTORY)
		return VFS_ERR_ISDIR;
	if (!file || !file->operations ||
	    !(file->capabilities & VFS_FILE_CAN_PREAD))
		return VFS_ERR_SPIPE;
	return file_read(file, user_destination, destination, count, &offset);
}

int64 vfs_file_pread(struct vfs_file *file, int user_destination,
		     uint64 destination, uint64 count, uint64 offset)
{
	if (!file || !(file->flags & VFS_OPEN_READ))
		return VFS_ERR_BADF;
	if (page_cache_writeback_file(file) < 0)
		return VFS_ERR_IO;
	return vfs_file_pread_raw(file, user_destination, destination,
				  count, offset);
}

int64 vfs_file_pwrite_raw(struct vfs_file *file, int user_source,
			  uint64 source, uint64 count, uint64 offset)
{
	if (file && file->path.dentry && file->path.dentry->inode &&
	    file->path.dentry->inode->type == VFS_INODE_DIRECTORY)
		return VFS_ERR_ISDIR;
	if (!file || !file->operations ||
	    !(file->capabilities & VFS_FILE_CAN_PREAD))
		return VFS_ERR_SPIPE;
	return file_write(file, user_source, source, count, &offset);
}

static int vfs_prepare_positioned_write(file_t file, uint32 flags,
					uint64 *offset, uint64 *old_size)
{
	struct vfs_inode *inode;
	struct vfs_stat stat;
	int result;

	*old_size = 0;
	if (!file->path.dentry ||
	    !(inode = file->path.dentry->inode) ||
	    inode->type != VFS_INODE_REGULAR)
		return VFS_OK;
	result = vfs_inode_stat(inode, &stat);
	if (result < 0)
		return result;
	*old_size = stat.size;
	if (!(flags & VFS_WRITE_NOAPPEND) &&
	    file->flags & VFS_OPEN_APPEND)
		*offset = stat.size;
	return VFS_OK;
}

int64 vfs_file_pwrite(struct vfs_file *file, int user_source,
			 uint64 source, uint64 count, uint64 offset,
			 uint32 flags)
{
	sleeplock_t lock;
	uint64 old_size;
	int64 result;

	if (!file || !(file->flags & VFS_OPEN_WRITE))
		return VFS_ERR_BADF;
	lock = file->path.dentry ?
		vfs_inode_write_lock(file->path.dentry->inode) : 0;
	if (lock)
		sleeplock_acquire(lock);
	result = vfs_prepare_positioned_write(file, flags, &offset,
					      &old_size);
	if (result < 0)
		goto out;
	if (count > VFS_MAX_FILE_OFFSET ||
	    offset > VFS_MAX_FILE_OFFSET - count) {
		result = VFS_ERR_INVAL;
		goto out;
	}
	if (count &&
	    (result = vfs_inode_remove_privileges(
		    file->path.dentry ? file->path.dentry->inode : 0)) < 0)
		goto out;
	result = vfs_file_pwrite_raw(file, user_source, source, count,
				     offset);
	if (result > 0 && lock)
		page_cache_refresh(file, user_source, source, offset, result,
				   old_size);
out:
	if (lock)
		sleeplock_release(lock);
	return result;
}

int64 vfs_file_preadv(struct vfs_file *file, int user_destination,
			 const struct vfs_iovec *iovecs, uint32 count,
			 uint64 offset)
{
	uint64 total = 0;
	uint32 index;
	int64 result;

	if (!file || !(file->flags & VFS_OPEN_READ))
		return VFS_ERR_BADF;
	if (page_cache_writeback_file(file) < 0)
		return VFS_ERR_IO;
	for (index = 0; index < count; index++) {
		result = vfs_file_pread_raw(file, user_destination,
					    iovecs[index].base,
					    iovecs[index].length, offset);
		if (result < 0)
			return total ? total : result;
		offset += result;
		total += result;
		if ((uint64)result != iovecs[index].length)
			break;
	}
	return total;
}

int64 vfs_file_pwritev(struct vfs_file *file, int user_source,
			  const struct vfs_iovec *iovecs, uint32 count,
			  uint64 offset, uint32 flags)
{
	sleeplock_t lock;
	uint64 length = 0, old_size, start, total = 0;
	uint32 index;
	int64 result;

	if (!file || !(file->flags & VFS_OPEN_WRITE))
		return VFS_ERR_BADF;
	lock = file->path.dentry ?
		vfs_inode_write_lock(file->path.dentry->inode) : 0;
	if (lock)
		sleeplock_acquire(lock);
	result = vfs_prepare_positioned_write(file, flags, &offset,
					      &old_size);
	if (result < 0)
		goto out;
	for (index = 0; index < count; index++) {
		if (iovecs[index].length > VFS_MAX_FILE_OFFSET - length) {
			result = VFS_ERR_INVAL;
			goto out;
		}
		length += iovecs[index].length;
	}
	if (offset > VFS_MAX_FILE_OFFSET - length) {
		result = VFS_ERR_INVAL;
		goto out;
	}
	if (vfs_iov_has_data(iovecs, count) &&
	    (result = vfs_inode_remove_privileges(
		    file->path.dentry ? file->path.dentry->inode : 0)) < 0)
		goto out;
	for (index = 0; index < count; index++) {
		start = offset;
		result = vfs_file_pwrite_raw(file, user_source,
					     iovecs[index].base,
					     iovecs[index].length, offset);
		if (result < 0) {
			result = total ? total : result;
			goto out;
		}
		if (result > 0 && lock)
			page_cache_refresh(file, user_source, iovecs[index].base,
					   start, result, old_size);
		offset += result;
		if (offset > old_size)
			old_size = offset;
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
	if (!result)
		return VFS_ERR_INVAL;
	return fd_get(fd, result);
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
		if (vfs_poll_wait(generation, remaining) == VFS_ERR_INTR) {
			ready = VFS_ERR_INTR;
			break;
		}
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
		result = wait_queue_sleep_interruptible(
			&poll_state.wait, &poll_state.lock);
	else
		result = wait_queue_sleep_interruptible_timeout(
			&poll_state.wait, &poll_state.lock, timeout_ms);
	if (result == WAIT_QUEUE_INTERRUPTED)
		result = VFS_ERR_INTR;
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
	process_t process = cur_proc();
	file_t file;

	if (fd < 0 || fd >= NOFILE)
		return VFS_ERR_BADF;
	spinlock_acquire(&process->files_lock);
	file = process->ofile[fd];
	if (!file) {
		spinlock_release(&process->files_lock);
		return VFS_ERR_BADF;
	}
	process->ofile[fd] = 0;
	process->fd_flags[fd] = 0;
	spinlock_release(&process->files_lock);
	file_close(file);
	vfs_poll_notify();
	return VFS_OK;
}

int vfs_dup(int oldfd, int minimum, uint8 flags, int *fd_out)
{
	process_t process = cur_proc();
	file_t file;
	int fd;

	if (oldfd < 0 || oldfd >= NOFILE)
		return VFS_ERR_BADF;
	if (minimum < 0)
		minimum = 0;
	spinlock_acquire(&process->files_lock);
	file = process->ofile[oldfd];
	if (!file) {
		spinlock_release(&process->files_lock);
		return VFS_ERR_BADF;
	}
	for (fd = minimum; fd < NOFILE; fd++) {
		if (!process->ofile[fd])
			break;
	}
	if (fd == NOFILE) {
		spinlock_release(&process->files_lock);
		return VFS_ERR_MFILE;
	}
	process->ofile[fd] = file_dup(file);
	process->fd_flags[fd] = flags;
	spinlock_release(&process->files_lock);
	*fd_out = fd;
	return VFS_OK;
}

int vfs_dup_to(int oldfd, int newfd, uint8 flags)
{
	process_t process = cur_proc();
	file_t file, replaced;

	if (oldfd < 0 || oldfd >= NOFILE || newfd < 0 || newfd >= NOFILE)
		return VFS_ERR_BADF;
	if (oldfd == newfd)
		return VFS_ERR_INVAL;
	spinlock_acquire(&process->files_lock);
	file = process->ofile[oldfd];
	if (!file) {
		spinlock_release(&process->files_lock);
		return VFS_ERR_BADF;
	}
	replaced = process->ofile[newfd];
	process->ofile[newfd] = file_dup(file);
	process->fd_flags[newfd] = flags;
	spinlock_release(&process->files_lock);
	if (replaced)
		file_close(replaced);
	vfs_poll_notify();
	return VFS_OK;
}

int vfs_read(int fd, uint64 address, int length)
{
	file_t file;
	int64 result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (length < 0)
		result = VFS_ERR_INVAL;
	else if (page_cache_writeback_file(file) < 0)
		result = VFS_ERR_IO;
	else
		result = file_read(file, 1, address, length, &file->position);
	vfs_file_put(file);
	return result > 0x7fffffff ? VFS_ERR_INVAL : result;
}

int64 vfs_readv(int fd, int user_destination,
		const struct vfs_iovec *iovecs, uint32 count)
{
	file_t file;
	uint64 total = 0;
	uint32 index;
	int64 result = VFS_OK;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	for (index = 0; index < count; index++) {
		if (iovecs[index].length > 0x7fffffff - total) {
			result = VFS_ERR_INVAL;
			goto out;
		}
		total += iovecs[index].length;
	}
	if (!(file->flags & VFS_OPEN_READ)) {
		result = VFS_ERR_BADF;
		goto out;
	}
	if (page_cache_writeback_file(file) < 0) {
		result = VFS_ERR_IO;
		goto out;
	}
	total = 0;
	sleeplock_acquire(&file->position_lock);
	if (file->operations && file->operations->readv) {
		result = file->operations->readv(file, user_destination, iovecs,
					       count);
		goto out_unlock;
	}
	for (index = 0; index < count; index++) {
		result = file_read(file, user_destination, iovecs[index].base,
				   iovecs[index].length, &file->position);
		if (result < 0) {
			result = total ? total : result;
			goto out_unlock;
		}
		total += result;
		if ((uint64)result != iovecs[index].length)
			break;
	}
	result = total;
out_unlock:
	sleeplock_release(&file->position_lock);
out:
	vfs_file_put(file);
	return result;
}

static int vfs_prepare_write(file_t file, uint64 *old_size)
{
	return vfs_prepare_positioned_write(file, 0, &file->position,
					    old_size);
}

static sleeplock_t vfs_regular_write_lock(file_t file)
{
	if (!file->path.dentry)
		return 0;
	return vfs_inode_write_lock(file->path.dentry->inode);
}

int64 vfs_file_write_current(struct vfs_file *file, int user_source,
			     uint64 source, uint64 count)
{
	sleeplock_t lock;
	uint64 old_size = 0, start = 0;
	int64 result;

	if (!file)
		return VFS_ERR_BADF;
	if (!(file->flags & VFS_OPEN_WRITE))
		return VFS_ERR_BADF;
	if (count > 0x7fffffff)
		return VFS_ERR_INVAL;
	lock = vfs_regular_write_lock(file);
	if (lock)
		sleeplock_acquire(lock);
	result = vfs_prepare_write(file, &old_size);
	if (result >= 0 && count)
		result = vfs_inode_remove_privileges(
			file->path.dentry ? file->path.dentry->inode : 0);
	if (result >= 0) {
		start = file->position;
		result = file_write(file, user_source, source, count,
				    &file->position);
		if (result > 0 && lock)
			page_cache_refresh(file, user_source, source, start, result,
					   old_size);
	}
	if (lock)
		sleeplock_release(lock);
	return result > 0x7fffffff ? VFS_ERR_INVAL : result;
}

int vfs_write(int fd, uint64 address, int length)
{
	file_t file;
	int result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (length < 0)
		result = VFS_ERR_INVAL;
	else
		result = vfs_file_write_current(file, 1, address, length);
	vfs_file_put(file);
	return result;
}

int64 vfs_writev(int fd, int user_source,
		 const struct vfs_iovec *iovecs, uint32 count,
		 uint32 flags)
{
	sleeplock_t lock;
	file_t file;
	uint64 old_size = 0, start, total = 0;
	uint32 index;
	int64 result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!(file->flags & VFS_OPEN_WRITE)) {
		result = VFS_ERR_BADF;
		goto out_file;
	}
	for (index = 0; index < count; index++) {
		if (iovecs[index].length > 0x7fffffff) {
			result = VFS_ERR_INVAL;
			goto out_file;
		}
	}
	lock = vfs_regular_write_lock(file);
	if (lock)
		sleeplock_acquire(lock);
	if (vfs_iov_has_data(iovecs, count) &&
	    (result = vfs_inode_remove_privileges(
		    file->path.dentry ? file->path.dentry->inode : 0)) < 0)
		goto out;
	if (file->operations && file->operations->writev) {
		result = file->operations->writev(file, user_source, iovecs,
						  count);
		goto out;
	}
	result = vfs_prepare_positioned_write(file, flags, &file->position,
					      &old_size);
	if (result < 0)
		goto out;
	for (index = 0; index < count; index++) {
		start = file->position;
		result = file_write(file, user_source, iovecs[index].base,
				    iovecs[index].length, &file->position);
		if (result < 0) {
			result = total ? total : result;
			goto out;
		}
		if (result > 0 && lock)
			page_cache_refresh(file, user_source, iovecs[index].base,
					   start, result, old_size);
		if (file->position > old_size)
			old_size = file->position;
		total += result;
		if ((uint64)result != iovecs[index].length)
			break;
	}
	result = total;
out:
	if (lock)
		sleeplock_release(lock);
out_file:
	vfs_file_put(file);
	return result;
}

int vfs_ftruncate(int fd, uint64 size)
{
	struct vfs_inode *inode;
	file_t file;
	int result;

	result = vfs_get_file_fd(fd, &file);
	if (result < 0)
		return result;
	if (!(file->flags & VFS_OPEN_WRITE)) {
		result = VFS_ERR_BADF;
		goto out;
	}
	inode = file->path.dentry ? file->path.dentry->inode : 0;
	result = vfs_truncate_inode(inode, size);
out:
	vfs_file_put(file);
	return result;
}

int vfs_truncate(const char *path, uint64 size)
{
	struct vfs_path resolved;
	int result;

	result = vfs_walk(path, 0, &resolved, 0);
	if (result < 0)
		return result;
	result = vfs_inode_permission(resolved.dentry->inode,
				      VFS_ACCESS_WRITE);
	if (result >= 0)
		result = vfs_truncate_inode(resolved.dentry->inode, size);
	vfs_path_put(&resolved);
	return result;
}

int vfs_fallocate(int fd, uint64 offset, uint64 length)
{
	struct vfs_inode *inode;
	sleeplock_t lock;
	file_t file;
	int result;

	if (!length || offset > (uint64)-1 - length)
		return VFS_ERR_INVAL;
	result = vfs_get_file_fd(fd, &file);
	if (result < 0)
		return result;
	if (!(file->flags & VFS_OPEN_WRITE)) {
		result = VFS_ERR_BADF;
		goto out;
	}
	if (!file->path.dentry ||
	    !(inode = file->path.dentry->inode) ||
	    inode->type != VFS_INODE_REGULAR || !file->operations ||
	    !file->operations->fallocate) {
		result = VFS_ERR_NOTSUPP;
		goto out;
	}
	lock = vfs_regular_write_lock(file);
	if (lock)
		sleeplock_acquire(lock);
	if (page_cache_writeback_inode_locked(inode) < 0)
		result = VFS_ERR_IO;
	else {
		result = vfs_inode_remove_privileges(inode);
		if (result >= 0)
			result = file->operations->fallocate(file, offset, length);
	}
	if (lock)
		sleeplock_release(lock);
out:
	vfs_file_put(file);
	return result;
}

int64 vfs_ioctl(int fd, uint64 request, uint64 argument)
{
	file_t file;
	int64 result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	result = file_ioctl(file, request, argument);
	vfs_file_put(file);
	return result;
}

int vfs_seek(int fd, int64 offset, int whence, uint64 *result)
{
	struct vfs_stat stat;
	file_t file;
	int64 next;
	int status;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->path.dentry) {
		status = VFS_ERR_SPIPE;
		goto out;
	}
	if (whence == 0)
		next = offset;
	else if (whence == 1)
		next = (int64)file->position + offset;
	else if (whence == 2) {
		status = vfs_inode_stat(file->path.dentry->inode, &stat);
		if (status < 0)
			goto out;
		next = (int64)stat.size + offset;
	} else {
		status = VFS_ERR_INVAL;
		goto out;
	}
	if (next < 0) {
		status = VFS_ERR_INVAL;
		goto out;
	}
	file->position = next;
	*result = next;
	status = VFS_OK;
out:
	vfs_file_put(file);
	return status;
}

int vfs_stat_fd(int fd, struct vfs_stat *stat)
{
	file_t file;
	int result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->path.dentry) {
		if (!file->operations || !file->operations->getattr)
			result = VFS_ERR_INVAL;
		else
			result = file->operations->getattr(file, stat);
	} else {
		result = vfs_inode_stat(file->path.dentry->inode, stat);
	}
	vfs_file_put(file);
	return result;
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

static int vfs_walk_at(int dirfd, const char *name, uint32 flags,
		       struct vfs_path *path)
{
	file_t directory;
	int status;

	status = vfs_directory_fd_get(dirfd, &directory);
	if (status < 0)
		return status;
	status = vfs_walk_from(name, flags, &directory->path, path, 0);
	vfs_file_put(directory);
	return status;
}

int vfs_stat_at(int dirfd, const char *name, int follow_symlink,
		struct vfs_stat *stat)
{
	struct vfs_path path;
	uint32 flags = follow_symlink ? 0 : VFS_LOOKUP_NOFOLLOW_FINAL;
	int status = vfs_walk_at(dirfd, name, flags, &path);

	if (status < 0)
		return status;
	status = vfs_inode_stat(path.dentry->inode, stat);
	vfs_path_put(&path);
	return status;
}

static int vfs_statfs_super(struct vfs_super_block *superblock,
			    struct vfs_statfs *stat)
{
	if (!superblock || !stat)
		return VFS_ERR_INVAL;
	if (superblock->operations && superblock->operations->statfs)
		return superblock->operations->statfs(superblock, stat);
	memset(stat, 0, sizeof(*stat));
	stat->block_size = superblock->block_size;
	stat->fragment_size = superblock->block_size;
	stat->name_length = VFS_NAME_MAX;
	if (superblock->device && superblock->block_size) {
		stat->blocks = superblock->device->sector_count *
			superblock->device->sector_size / superblock->block_size;
	}
	return VFS_OK;
}

int vfs_statfs_fd(int fd, struct vfs_statfs *stat)
{
	file_t file;
	int result;

	result = vfs_get_file_fd(fd, &file);
	if (result < 0)
		return result;
	if (!file->path.dentry)
		result = VFS_ERR_INVAL;
	else
		result = vfs_statfs_super(
			file->path.dentry->inode->superblock, stat);
	vfs_file_put(file);
	return result;
}

int vfs_statfs_path(const char *name, struct vfs_statfs *stat)
{
	struct vfs_path path;
	int status = vfs_walk(name, 0, &path, 0);

	if (status < 0)
		return status;
	status = vfs_statfs_super(path.dentry->inode->superblock, stat);
	vfs_path_put(&path);
	return status;
}

static int vfs_setattr_inode(struct vfs_inode *inode,
			     const struct vfs_iattr *attributes)
{
	struct process_credentials credentials;
	struct vfs_iattr next;
	struct vfs_stat stat;
	sleeplock_t attribute_lock, write_lock;
	int owner, status;

	if (!inode || !attributes || !attributes->mask ||
	    (attributes->mask & ~(VFS_ATTR_MODE | VFS_ATTR_UID |
				   VFS_ATTR_GID)))
		return VFS_ERR_INVAL;
	if (!inode->operations || !inode->operations->setattr)
		return VFS_ERR_NOTSUPP;
	write_lock = inode->superblock ? &inode->superblock->write_lock : 0;
	attribute_lock = inode->superblock ?
		&inode->superblock->attribute_lock : 0;
	if (write_lock)
		sleeplock_acquire(write_lock);
	if (attribute_lock)
		sleeplock_acquire(attribute_lock);
	status = vfs_inode_stat(inode, &stat);
	if (status < 0)
		goto out;
	process_credentials_get(&credentials);
	owner = credentials.fsuid == stat.uid;
	next = *attributes;
	if (attributes->mask & VFS_ATTR_MODE) {
		if (credentials.fsuid && !owner) {
			status = VFS_ERR_PERM;
			goto out;
		}
		next.mode &= VFS_MODE_PERMISSIONS;
		if (credentials.fsuid && (next.mode & 02000) &&
		    !vfs_credentials_in_group(&credentials, stat.gid, 0))
			next.mode &= ~02000U;
	}
	if (attributes->mask & (VFS_ATTR_UID | VFS_ATTR_GID)) {
		if (credentials.fsuid) {
			if (!owner || ((attributes->mask & VFS_ATTR_UID) &&
				       attributes->uid != stat.uid) ||
			    ((attributes->mask & VFS_ATTR_GID) &&
			     attributes->gid != stat.gid &&
			     !vfs_credentials_in_group(&credentials,
						       attributes->gid, 0))) {
				status = VFS_ERR_PERM;
				goto out;
			}
		}
		if (((attributes->mask & VFS_ATTR_UID) &&
		     attributes->uid != stat.uid) ||
		    ((attributes->mask & VFS_ATTR_GID) &&
		     attributes->gid != stat.gid)) {
			next.mask |= VFS_ATTR_MODE;
			next.mode = (attributes->mask & VFS_ATTR_MODE ?
				next.mode : stat.mode) & ~06000U;
		}
	}
	status = vfs_inode_apply_attributes(inode, &next);
out:
	if (attribute_lock)
		sleeplock_release(attribute_lock);
	if (write_lock)
		sleeplock_release(write_lock);
	return status;
}

int vfs_setattr_path(const char *name, int follow_symlink,
		     const struct vfs_iattr *attributes)
{
	struct vfs_path path;
	uint32 flags = follow_symlink ? 0 : VFS_LOOKUP_NOFOLLOW_FINAL;
	int status = vfs_walk(name, flags, &path, 0);

	if (status < 0)
		return status;
	status = vfs_setattr_inode(path.dentry->inode, attributes);
	vfs_path_put(&path);
	return status;
}

int vfs_setattr_at(int dirfd, const char *name, int follow_symlink,
		   const struct vfs_iattr *attributes)
{
	struct vfs_path path;
	uint32 flags = follow_symlink ? 0 : VFS_LOOKUP_NOFOLLOW_FINAL;
	int status = vfs_walk_at(dirfd, name, flags, &path);

	if (status < 0)
		return status;
	status = vfs_setattr_inode(path.dentry->inode, attributes);
	vfs_path_put(&path);
	return status;
}

int vfs_setattr_fd(int fd, const struct vfs_iattr *attributes)
{
	file_t file;
	int status;

	status = vfs_get_file_fd(fd, &file);
	if (status < 0)
		return status;
	if (!file->path.dentry)
		status = VFS_ERR_INVAL;
	else
		status = vfs_setattr_inode(file->path.dentry->inode,
					   attributes);
	vfs_file_put(file);
	return status;
}
static int vfs_time_permission(struct vfs_inode *inode, int owner_only)
{
	struct process_credentials credentials;
	struct vfs_stat stat;
	int status;

	status = vfs_inode_stat(inode, &stat);
	if (status < 0)
		return status;
	process_credentials_get(&credentials);
	if (!credentials.fsuid || credentials.fsuid == stat.uid)
		return VFS_OK;
	if (owner_only)
		return VFS_ERR_PERM;
	return vfs_inode_permission_credentials(inode, VFS_ACCESS_WRITE,
						&credentials, 0);
}

static int vfs_set_times_inode(
	struct vfs_inode *inode, const struct vfs_timespec times[2],
	uint32 mask, int owner_only)
{
	sleeplock_t lock;
	int status;

	if (!inode || !times ||
	    (mask & ~(VFS_TIME_ATIME | VFS_TIME_MTIME)))
		return VFS_ERR_INVAL;
	if (!mask)
		return VFS_OK;
	lock = inode->superblock ? &inode->superblock->attribute_lock : 0;
	if (lock)
		sleeplock_acquire(lock);
	status = vfs_time_permission(inode, owner_only);
	if (status < 0)
		goto out;
	if (!inode->operations || !inode->operations->set_times)
		status = VFS_ERR_NOTSUPP;
	else
		status = inode->operations->set_times(inode, times, mask);
out:
	if (lock)
		sleeplock_release(lock);
	return status;
}

int vfs_set_times_path(const char *name, int follow_symlink,
		       const struct vfs_timespec times[2], uint32 mask,
		       int owner_only)
{
	struct vfs_path path;
	uint32 flags = follow_symlink ? 0 : VFS_LOOKUP_NOFOLLOW_FINAL;
	int status;

	if (!times || (mask & ~(VFS_TIME_ATIME | VFS_TIME_MTIME)))
		return VFS_ERR_INVAL;
	status = vfs_walk(name, flags, &path, 0);
	if (status < 0)
		return status;
	status = vfs_set_times_inode(path.dentry->inode, times, mask,
				     owner_only);
	vfs_path_put(&path);
	return status;
}

int vfs_set_times_at(int dirfd, const char *name, int follow_symlink,
		     const struct vfs_timespec times[2], uint32 mask,
		     int owner_only)
{
	struct vfs_path path;
	uint32 flags = follow_symlink ? 0 : VFS_LOOKUP_NOFOLLOW_FINAL;
	int status;

	if (!times || (mask & ~(VFS_TIME_ATIME | VFS_TIME_MTIME)))
		return VFS_ERR_INVAL;
	status = vfs_walk_at(dirfd, name, flags, &path);
	if (status < 0)
		return status;
	status = vfs_set_times_inode(path.dentry->inode, times, mask,
				     owner_only);
	vfs_path_put(&path);
	return status;
}

int vfs_set_times_fd(int fd, const struct vfs_timespec times[2],
		     uint32 mask, int owner_only)
{
	struct vfs_inode *inode;
	file_t file;
	int status;

	if (!times || (mask & ~(VFS_TIME_ATIME | VFS_TIME_MTIME)))
		return VFS_ERR_INVAL;
	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->path.dentry) {
		status = VFS_ERR_INVAL;
		goto out;
	}
	inode = file->path.dentry->inode;
	status = vfs_set_times_inode(inode, times, mask, owner_only);
out:
	vfs_file_put(file);
	return status;
}

int vfs_next_dirent(int fd, vfs_dirent_emit_t emit, void *context)
{
	struct vfs_dirent dirent;
	file_t file;
	uint64 position;
	int result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->path.dentry)
		result = VFS_ERR_NOTDIR;
	else if (file->path.dentry->inode->type != VFS_INODE_DIRECTORY)
		result = VFS_ERR_NOTDIR;
	else if (!file->operations || !file->operations->readdir)
		result = VFS_ERR_NOTSUPP;
	else if (!emit)
		result = VFS_ERR_INVAL;
	else {
		int emitted;

		sleeplock_acquire(&file->position_lock);
		position = file->position;
		result = file->operations->readdir(file, &dirent);
		if (result > 0) {
			emitted = emit(&dirent, context);
			if (emitted < 0) {
				result = file->operations->seekdir ?
					file->operations->seekdir(file, position) :
					VFS_OK;
				if (result >= 0) {
					file->position = position;
					result = emitted;
				}
			}
		}
		sleeplock_release(&file->position_lock);
	}
	vfs_file_put(file);
	return result;
}

int vfs_mkdir(const char *name, uint32 mode)
{
	struct vfs_inode *inode;
	struct vfs_path existing, parent;
	char last[VFS_NAME_MAX + 1];
	uint32 uid, gid;
	int status;

	status = vfs_walk(name, VFS_LOOKUP_PARENT, &parent, last);
	if (status == VFS_ERR_INVAL &&
	    vfs_walk(name, 0, &existing, 0) == VFS_OK) {
		vfs_path_put(&existing);
		return VFS_ERR_EXIST;
	}
	if (status < 0)
		return status;
	if (!vfs_leaf_valid(last)) {
		vfs_path_put(&parent);
		return string_equal(last, ".") || string_equal(last, "..") ?
			VFS_ERR_EXIST : VFS_ERR_INVAL;
	}
	status = vfs_mutation_permission(parent.dentry->inode);
	if (status < 0) {
		vfs_path_put(&parent);
		return status;
	}
	if (!parent.dentry->inode->operations ||
	    !parent.dentry->inode->operations->mkdir) {
		vfs_path_put(&parent);
		return VFS_ERR_NOTSUPP;
	}
	vfs_creation_credentials(parent.dentry->inode, &mode, &uid, &gid, 1);
	status = parent.dentry->inode->operations->mkdir(
		parent.dentry->inode, last, mode, uid, gid, &inode);
	if (status == VFS_OK)
		vfs_inode_put(inode);
	vfs_path_put(&parent);
	return status;
}

static int vfs_mknod_from(const char *name, enum vfs_inode_type type,
			  uint32 mode, uint64 device,
			  const struct vfs_path *base)
{
	struct process_credentials credentials;
	struct vfs_inode *inode;
	struct vfs_path parent, path;
	char last[VFS_NAME_MAX + 1];
	uint32 uid, gid;
	int status;

	if (type != VFS_INODE_REGULAR && type != VFS_INODE_CHAR_DEVICE &&
	    type != VFS_INODE_BLOCK_DEVICE && type != VFS_INODE_FIFO &&
	    type != VFS_INODE_SOCKET)
		return VFS_ERR_INVAL;
	if (type == VFS_INODE_REGULAR) {
		status = vfs_create_path_from(name, mode, base, &path);
		if (status == VFS_OK)
			vfs_path_put(&path);
		return status;
	}
	if (type == VFS_INODE_FIFO)
		return VFS_ERR_NOTSUPP;
	process_credentials_get(&credentials);
	if ((type == VFS_INODE_CHAR_DEVICE ||
	     type == VFS_INODE_BLOCK_DEVICE) && credentials.euid)
		return VFS_ERR_PERM;
	status = vfs_walk_base(name, VFS_LOOKUP_PARENT, base, &parent, last);
	if (status < 0)
		return status;
	if (!vfs_leaf_valid(last)) {
		status = VFS_ERR_INVAL;
		goto out;
	}
	status = vfs_mutation_permission(parent.dentry->inode);
	if (status < 0)
		goto out;
	if (!parent.dentry->inode->operations ||
	    !parent.dentry->inode->operations->mknod) {
		status = VFS_ERR_NOTSUPP;
		goto out;
	}
	vfs_creation_credentials(parent.dentry->inode, &mode, &uid, &gid, 0);
	status = parent.dentry->inode->operations->mknod(
		parent.dentry->inode, last, type, mode, uid, gid, device,
		&inode);
	if (status == VFS_OK)
		vfs_inode_put(inode);
out:
	vfs_path_put(&parent);
	return status;
}

int vfs_mknod(const char *name, enum vfs_inode_type type, uint32 mode,
	      uint64 device)
{
	return vfs_mknod_from(name, type, mode, device, 0);
}

int vfs_mknod_at(int dirfd, const char *name, enum vfs_inode_type type,
		 uint32 mode, uint64 device)
{
	file_t directory;
	int status;

	status = vfs_directory_fd_get(dirfd, &directory);
	if (status < 0)
		return status;
	status = vfs_mknod_from(name, type, mode, device, &directory->path);
	vfs_file_put(directory);
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
	if (target.mount->parent && target.dentry == target.mount->root) {
		vfs_path_put(&target);
		return VFS_ERR_BUSY;
	}
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
	status = vfs_mutation_permission(parent.dentry->inode);
	if (status < 0)
		goto out_unlink;
	status = vfs_sticky_permission(parent.dentry->inode,
				       target.dentry->inode);
	if (status < 0)
		goto out_unlink;
	if (remove_directory) {
		status = operations && operations->rmdir ?
			operations->rmdir(parent.dentry->inode, last) :
			VFS_ERR_NOTSUPP;
	} else {
		status = operations && operations->unlink ?
			operations->unlink(parent.dentry->inode, last) :
			VFS_ERR_NOTSUPP;
	}
out_unlink:
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
	status = vfs_mutation_permission(parent.dentry->inode);
	if (status < 0)
		goto out;
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
	uint32 mode = 0777, uid, gid;
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
	status = vfs_mutation_permission(parent.dentry->inode);
	if (status < 0) {
		vfs_path_put(&parent);
		return status;
	}
	vfs_creation_credentials(parent.dentry->inode, &mode, &uid, &gid, 0);
	operations = parent.dentry->inode->operations;
	status = operations && operations->symlink ?
		operations->symlink(parent.dentry->inode, last, target,
		                    uid, gid, &inode) : VFS_ERR_NOTSUPP;
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
	struct vfs_path source, old_parent, new_parent, destination;
	char old_last[VFS_NAME_MAX + 1];
	char new_last[VFS_NAME_MAX + 1];
	int destination_found = 0, status;

	if (flags & ~VFS_RENAME_NOREPLACE)
		return VFS_ERR_INVAL;
	status = vfs_walk(old_name, VFS_LOOKUP_NOFOLLOW_FINAL, &source, 0);
	if (status < 0)
		return status;
	if (source.mount->parent && source.dentry == source.mount->root) {
		status = VFS_ERR_BUSY;
		goto out_source;
	}
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
	status = vfs_mutation_permission(old_parent.dentry->inode);
	if (status < 0)
		goto out;
	status = vfs_mutation_permission(new_parent.dentry->inode);
	if (status < 0)
		goto out;
	status = vfs_sticky_permission(old_parent.dentry->inode,
				       source.dentry->inode);
	if (status < 0)
		goto out;
	status = vfs_walk(new_name, VFS_LOOKUP_NOFOLLOW_FINAL,
			  &destination, 0);
	if (status == VFS_OK) {
		destination_found = 1;
		if (destination.mount->parent &&
		    destination.dentry == destination.mount->root) {
			status = VFS_ERR_BUSY;
			goto out;
		}
		status = vfs_sticky_permission(new_parent.dentry->inode,
					       destination.dentry->inode);
		if (status < 0)
			goto out;
	} else if (status != VFS_ERR_NOENT) {
		goto out;
	}
	operations = old_parent.dentry->inode->operations;
	status = operations && operations->rename ?
		operations->rename(old_parent.dentry->inode, old_last,
		                   new_parent.dentry->inode, new_last,
		                   flags) : VFS_ERR_NOTSUPP;
out:
	if (destination_found)
		vfs_path_put(&destination);
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
	int result;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	if (!file->operations || !file->operations->fsync)
		result = VFS_ERR_INVAL;
	else if (page_cache_writeback_file(file) < 0)
		result = VFS_ERR_IO;
	else
		result = file->operations->fsync(file);
	vfs_file_put(file);
	return result;
}

int vfs_sync(void)
{
	struct vfs_super_block *superblocks[VFS_SUPER_MAX];
	struct vfs_super_block *superblock;
	int count = 0, first_error = VFS_OK, status;

	sleeplock_acquire(&vfs.mount_lock);
	spinlock_acquire(&vfs.lock);
	for (superblock = vfs.superblocks;
	     superblock != &vfs.superblocks[VFS_SUPER_MAX]; superblock++) {
		if (superblock->ref)
			superblocks[count++] = superblock;
	}
	spinlock_release(&vfs.lock);
	for (int i = 0; i < count; i++) {
		superblock = superblocks[i];
		if (page_cache_writeback_super(superblock) < 0 &&
		    first_error == VFS_OK)
			first_error = VFS_ERR_IO;
		if (!superblock->operations || !superblock->operations->sync)
			continue;
		status = superblock->operations->sync(superblock);
		if (status < 0 && first_error == VFS_OK)
			first_error = status;
	}
	sleeplock_release(&vfs.mount_lock);
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
	status = vfs_inode_permission(path.dentry->inode, VFS_ACCESS_EXEC);
	if (status < 0) {
		vfs_path_put(&path);
		return status;
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

int vfs_access(const char *name, uint32 mode, int use_effective_ids)
{
	struct process_credentials credentials;
	struct vfs_path path;
	int status;

	process_credentials_get(&credentials);
	credentials.fsuid = use_effective_ids ? credentials.euid :
		credentials.uid;
	credentials.fsgid = use_effective_ids ? credentials.egid :
		credentials.gid;
	status = vfs_walk_credentials(name, 0, &path, 0, &credentials, 0);
	if (status == VFS_OK) {
		status = vfs_inode_permission_credentials(path.dentry->inode,
						      mode, &credentials, 0);
		vfs_path_put(&path);
	}
	return status;
}

int vfs_get_fd_flags(int fd, uint8 *flags)
{
	process_t process = cur_proc();

	if (fd < 0 || fd >= NOFILE)
		return VFS_ERR_BADF;
	spinlock_acquire(&process->files_lock);
	if (!process->ofile[fd]) {
		spinlock_release(&process->files_lock);
		return VFS_ERR_BADF;
	}
	*flags = process->fd_flags[fd];
	spinlock_release(&process->files_lock);
	return VFS_OK;
}

int vfs_set_fd_flags(int fd, uint8 flags)
{
	process_t process = cur_proc();

	if (fd < 0 || fd >= NOFILE)
		return VFS_ERR_BADF;
	spinlock_acquire(&process->files_lock);
	if (!process->ofile[fd]) {
		spinlock_release(&process->files_lock);
		return VFS_ERR_BADF;
	}
	process->fd_flags[fd] = flags;
	spinlock_release(&process->files_lock);
	return VFS_OK;
}

int vfs_get_file_flags(int fd, uint32 *flags)
{
	file_t file;

	if (fd_get(fd, &file) != VFS_OK)
		return VFS_ERR_BADF;
	*flags = file->flags;
	vfs_file_put(file);
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
		if (status < 0) {
			vfs_file_put(file);
			return status;
		}
	}
	file->flags = next;
	vfs_file_put(file);
	return VFS_OK;
}

void vfs_close_on_exec(void)
{
	process_t process = cur_proc();
	file_t files[NOFILE];
	int count = 0, fd;

	spinlock_acquire(&process->files_lock);
	for (fd = 0; fd < NOFILE; fd++) {
		if (!process->ofile[fd] ||
		    !(process->fd_flags[fd] & VFS_FD_CLOEXEC))
			continue;
		files[count++] = process->ofile[fd];
		process->ofile[fd] = 0;
		process->fd_flags[fd] = 0;
	}
	spinlock_release(&process->files_lock);
	for (fd = 0; fd < count; fd++)
		file_close(files[fd]);
	if (count)
		vfs_poll_notify();
}
