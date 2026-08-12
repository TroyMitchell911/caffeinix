#include <char_device.h>
#include <debug.h>
#include <devfs.h>
#include <mystring.h>
#include <riscv.h>
#include <vfs.h>

static const struct vfs_inode_operations devfs_inode_operations;
static const struct vfs_file_operations devfs_directory_operations;

static struct vfs_inode *devfs_wrap(struct vfs_super_block *superblock,
				    const struct char_device_node *node)
{
	struct vfs_inode *inode = vfs_inode_alloc(superblock);

	if (!inode)
		return 0;
	inode->operations = &devfs_inode_operations;
	if (!node) {
		inode->number = 1;
		inode->type = VFS_INODE_DIRECTORY;
		inode->mode = 0755;
		inode->nlink = 2;
		inode->file_operations = &devfs_directory_operations;
	} else {
		inode->number = node->inode_number;
		inode->type = VFS_INODE_CHAR_DEVICE;
		inode->mode = node->mode;
		inode->nlink = 1;
		inode->device = node->device;
		inode->file_operations = &vfs_device_operations;
	}
	return inode;
}

static int devfs_lookup(struct vfs_inode *directory, const char *name,
			struct vfs_inode **result)
{
	struct char_device_node node;
	int status;

	if (directory->number != 1)
		return VFS_ERR_NOTDIR;
	status = char_device_node_find(name, &node);
	if (status < 0)
		return status;
	*result = devfs_wrap(directory->superblock, &node);
	return *result ? VFS_OK : VFS_ERR_NOMEM;
}

static int devfs_getattr(struct vfs_inode *inode, struct vfs_stat *stat)
{
	return vfs_inode_stat_default(inode, stat);
}

static const struct vfs_inode_operations devfs_inode_operations = {
	.lookup = devfs_lookup,
	.getattr = devfs_getattr,
};

static int devfs_readdir(struct vfs_file *file, struct vfs_dirent *result)
{
	struct char_device_node node;
	uint64 position = file->position;

	if (position >= char_device_node_count() + 2)
		return 0;
	if (position < 2) {
		result->ino = 1;
		result->type = VFS_DT_DIR;
		safe_strncpy(result->name, position ? ".." : ".",
		             sizeof(result->name));
	} else {
		if (char_device_node_get(position - 2, &node) < 0)
			return 0;
		result->ino = node.inode_number;
		result->type = VFS_DT_CHAR;
		safe_strncpy(result->name, node.name,
		             sizeof(result->name));
	}
	result->next_offset = ++file->position;
	return 1;
}

static const struct vfs_file_operations devfs_directory_operations = {
	.readdir = devfs_readdir,
};

static int devfs_sync(struct vfs_super_block *superblock)
{
	(void)superblock;
	return VFS_OK;
}

static const struct vfs_super_operations devfs_super_operations = {
	.sync = devfs_sync,
};

static int devfs_mount(struct vfs_filesystem_type *type,
			struct block_device *device, const void *data,
			struct vfs_super_block **result)
{
	struct vfs_super_block *superblock;

	(void)data;
	if (device)
		return VFS_ERR_INVAL;
	superblock = vfs_super_alloc(type, 0);
	if (!superblock)
		return VFS_ERR_NOMEM;
	superblock->operations = &devfs_super_operations;
	superblock->block_size = PGSIZE;
	superblock->root = devfs_wrap(superblock, 0);
	if (!superblock->root) {
		vfs_super_free(superblock);
		return VFS_ERR_NOMEM;
	}
	*result = superblock;
	return VFS_OK;
}

static struct vfs_filesystem_type devfs_type = {
	.name = "devfs",
	.mount = devfs_mount,
};

void devfs_init(void)
{
	if (vfs_register_filesystem(&devfs_type) != VFS_OK)
		PANIC("register devfs");
}
