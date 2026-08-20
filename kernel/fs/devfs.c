#include <block_device.h>
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

static struct vfs_inode *devfs_wrap_block(
	struct vfs_super_block *superblock, struct block_device *device)
{
	struct vfs_inode *inode;

	if (!device)
		return 0;
	inode = vfs_inode_alloc(superblock);
	if (!inode)
		return 0;
	inode->operations = &devfs_inode_operations;
	inode->number = 0x100000000ULL + device->id;
	inode->type = VFS_INODE_BLOCK_DEVICE;
	inode->mode = 0660;
	inode->nlink = 1;
	inode->device = VFS_MAKE_DEVICE(BLOCK_DEVICE_NODE_MAJOR,
					device->id - 1);
	inode->size = device->sector_count * device->sector_size;
	inode->blocks = device->sector_count * device->sector_size / 512;
	inode->file_operations = &vfs_block_device_operations;
	return inode;
}

static struct block_device *devfs_find_block(const char *name)
{
	struct block_device *device;
	uint32 id;

	for (id = 1; id < BLOCK_DEVICE_MAX; id++) {
		device = block_device_open(id);
		if (device && !strcmp(device->name, name))
			return device;
		block_device_close(device);
	}
	return 0;
}

static struct block_device *devfs_get_block(uint32 index)
{
	struct block_device *device;
	uint32 current = 0, id;

	for (id = 1; id < BLOCK_DEVICE_MAX; id++) {
		device = block_device_open(id);
		if (device && current++ == index)
			return device;
		block_device_close(device);
	}
	return 0;
}

static int devfs_lookup(struct vfs_inode *directory, const char *name,
			struct vfs_inode **result)
{
	struct char_device_node node;
	struct block_device *device;
	int status;

	if (directory->number != 1)
		return VFS_ERR_NOTDIR;
	status = char_device_node_find(name, &node);
	if (status == VFS_OK)
		*result = devfs_wrap(directory->superblock, &node);
	else if ((device = devfs_find_block(name))) {
		*result = devfs_wrap_block(directory->superblock, device);
		block_device_close(device);
	} else
		return VFS_ERR_NOENT;
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
	struct block_device *device;
	struct char_device_node node;
	uint32 char_count = char_device_node_count();
	uint64 position = file->position;

	if (position < 2) {
		result->ino = 1;
		result->type = VFS_DT_DIR;
		safe_strncpy(result->name, position ? ".." : ".",
		             sizeof(result->name));
	} else if (position - 2 < char_count) {
		if (char_device_node_get(position - 2, &node) < 0)
			return 0;
		result->ino = node.inode_number;
		result->type = VFS_DT_CHAR;
		safe_strncpy(result->name, node.name,
		             sizeof(result->name));
	} else {
		device = devfs_get_block(position - 2 - char_count);
		if (!device)
			return 0;
		result->ino = 0x100000000ULL + device->id;
		result->type = VFS_DT_BLOCK;
		safe_strncpy(result->name, device->name,
		             sizeof(result->name));
		block_device_close(device);
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
	.flags = VFS_FS_NO_DENTRY_CACHE,
	.mount = devfs_mount,
};

void devfs_init(void)
{
	if (vfs_register_filesystem(&devfs_type) != VFS_OK)
		PANIC("register devfs");
}
