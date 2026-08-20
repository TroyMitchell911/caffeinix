#include <block_device.h>
#include <char_device.h>
#include <debug.h>
#include <mystring.h>
#include <palloc.h>
#include <process.h>
#include <riscv.h>
#include <sleeplock.h>
#include <tmpfs.h>
#include <vfs.h>

struct tmpfs_page {
	struct tmpfs_page *next;
	uint64 index;
	void *data;
};

struct tmpfs_inode;

struct tmpfs_entry {
	struct tmpfs_entry *next;
	struct tmpfs_inode *inode;
	char name[VFS_NAME_MAX + 1];
};

struct tmpfs_inode {
	uint64 number;
	enum vfs_inode_type type;
	uint32 mode;
	uint32 uid;
	uint32 gid;
	uint32 nlink;
	uint32 references;
	uint64 device;
	uint64 size;
	uint64 page_count;
	struct vfs_timespec atime;
	struct vfs_timespec mtime;
	struct vfs_timespec ctime;
	struct tmpfs_inode *parent;
	union {
		struct tmpfs_entry *entries;
		struct tmpfs_page *pages;
		char *target;
	};
};

struct tmpfs_super {
	struct sleeplock lock;
	uint64 next_inode;
	struct tmpfs_inode *root;
};

static char tmpfs_zero_page[PGSIZE];

static const struct vfs_inode_operations tmpfs_inode_operations;
static const struct vfs_file_operations tmpfs_file_operations;
static const struct vfs_file_operations tmpfs_directory_operations;

#define TMPFS_TIME_CTIME (1U << 2)

static void tmpfs_touch_locked(struct tmpfs_inode *inode, uint32 mask)
{
	struct vfs_timespec now;

	if (vfs_current_time(&now) < 0)
		return;
	if (mask & VFS_TIME_ATIME)
		inode->atime = now;
	if (mask & VFS_TIME_MTIME)
		inode->mtime = now;
	if (mask & TMPFS_TIME_CTIME)
		inode->ctime = now;
}

static struct tmpfs_inode *tmpfs_inode_alloc_locked(
	struct tmpfs_super *super, enum vfs_inode_type type, uint32 mode,
	uint32 uid, uint32 gid)
{
	struct tmpfs_inode *inode = malloc(sizeof(*inode));

	if (!inode)
		return 0;
	memset(inode, 0, sizeof(*inode));
	inode->number = super->next_inode++;
	inode->type = type;
	inode->mode = mode & VFS_MODE_PERMISSIONS;
	inode->uid = uid;
	inode->gid = gid;
	tmpfs_touch_locked(inode, VFS_TIME_ATIME | VFS_TIME_MTIME |
			   TMPFS_TIME_CTIME);
	return inode;
}

static void tmpfs_inode_destroy_locked(struct tmpfs_inode *inode)
{
	struct tmpfs_page *page, *next;

	if (inode->type == VFS_INODE_REGULAR) {
		for (page = inode->pages; page; page = next) {
			next = page->next;
			pfree(page->data);
			free(page);
		}
	} else if (inode->type == VFS_INODE_SYMLINK) {
		free(inode->target);
	}
	free(inode);
}

static void tmpfs_inode_maybe_destroy_locked(struct tmpfs_inode *inode)
{
	if (!inode->nlink && !inode->references)
		tmpfs_inode_destroy_locked(inode);
}

static void tmpfs_refresh_locked(struct vfs_inode *inode)
{
	struct tmpfs_inode *node = inode->private;

	inode->number = node->number;
	inode->type = node->type;
	inode->mode = node->mode;
	inode->uid = node->uid;
	inode->gid = node->gid;
	inode->nlink = node->nlink;
	inode->size = node->size;
	inode->blocks = node->page_count * (PGSIZE / 512);
	inode->device = node->device;
	inode->atime = node->atime;
	inode->mtime = node->mtime;
	inode->ctime = node->ctime;
	inode->operations = &tmpfs_inode_operations;
	if (node->type == VFS_INODE_DIRECTORY)
		inode->file_operations = &tmpfs_directory_operations;
	else if (node->type == VFS_INODE_REGULAR)
		inode->file_operations = &tmpfs_file_operations;
	else if (node->type == VFS_INODE_CHAR_DEVICE)
		inode->file_operations = &vfs_device_operations;
	else if (node->type == VFS_INODE_BLOCK_DEVICE)
		inode->file_operations = &vfs_block_device_operations;
}

static struct vfs_inode *tmpfs_wrap_locked(
	struct vfs_super_block *superblock, struct tmpfs_inode *node)
{
	struct vfs_inode *inode = vfs_inode_alloc(superblock);

	if (!inode)
		return 0;
	node->references++;
	inode->private = node;
	tmpfs_refresh_locked(inode);
	return inode;
}

static struct tmpfs_entry *tmpfs_find_entry_locked(
	struct tmpfs_inode *directory, const char *name,
	struct tmpfs_entry **previous)
{
	struct tmpfs_entry *entry, *last = 0;

	for (entry = directory->entries; entry; entry = entry->next) {
		if (!strcmp(entry->name, name)) {
			if (previous)
				*previous = last;
			return entry;
		}
		last = entry;
	}
	if (previous)
		*previous = last;
	return 0;
}

static int tmpfs_add_entry_locked(struct tmpfs_inode *directory,
				  const char *name,
				  struct tmpfs_inode *inode)
{
	struct tmpfs_entry *entry;
	uint32 length = strlen(name);

	if (!length)
		return VFS_ERR_INVAL;
	if (length > VFS_NAME_MAX)
		return VFS_ERR_NAMETOOLONG;
	if (tmpfs_find_entry_locked(directory, name, 0))
		return VFS_ERR_EXIST;
	entry = malloc(sizeof(*entry));
	if (!entry)
		return VFS_ERR_NOMEM;
	memset(entry, 0, sizeof(*entry));
	entry->inode = inode;
	safe_strncpy(entry->name, name, sizeof(entry->name));
	entry->next = directory->entries;
	directory->entries = entry;
	return VFS_OK;
}

static void tmpfs_detach_entry_locked(struct tmpfs_inode *directory,
				      struct tmpfs_entry *entry,
				      struct tmpfs_entry *previous)
{
	if (previous)
		previous->next = entry->next;
	else
		directory->entries = entry->next;
}

static void tmpfs_remove_entry_locked(struct tmpfs_inode *directory,
				      struct tmpfs_entry *entry,
				      struct tmpfs_entry *previous)
{
	struct tmpfs_inode *inode = entry->inode;

	tmpfs_detach_entry_locked(directory, entry, previous);
	if (inode->type == VFS_INODE_DIRECTORY) {
		if (directory->nlink)
			directory->nlink--;
		inode->nlink = 0;
	} else if (inode->nlink) {
		inode->nlink--;
	}
	free(entry);
	tmpfs_inode_maybe_destroy_locked(inode);
}

static void tmpfs_put_inode(struct vfs_inode *inode)
{
	struct tmpfs_super *super = inode->superblock->private;
	struct tmpfs_inode *node = inode->private;

	sleeplock_acquire(&super->lock);
	if (!node->references)
		PANIC("tmpfs inode reference");
	node->references--;
	tmpfs_inode_maybe_destroy_locked(node);
	sleeplock_release(&super->lock);
}

static int tmpfs_sync(struct vfs_super_block *superblock)
{
	(void)superblock;
	return VFS_OK;
}

static int tmpfs_statfs(struct vfs_super_block *superblock,
			struct vfs_statfs *stat)
{
	uint64 total = palloc_usable_bytes() / PGSIZE;

	(void)superblock;
	memset(stat, 0, sizeof(*stat));
	stat->type = 0x01021994;
	stat->block_size = PGSIZE;
	stat->fragment_size = PGSIZE;
	stat->blocks = total;
	stat->blocks_free = palloc_free_pages();
	stat->blocks_available = stat->blocks_free;
	stat->files = total;
	stat->files_free = stat->blocks_free;
	stat->name_length = VFS_NAME_MAX;
	return VFS_OK;
}

static const struct vfs_super_operations tmpfs_super_operations = {
	.put_inode = tmpfs_put_inode,
	.sync = tmpfs_sync,
	.statfs = tmpfs_statfs,
};

static int tmpfs_getattr(struct vfs_inode *inode, struct vfs_stat *stat)
{
	struct tmpfs_super *super = inode->superblock->private;

	sleeplock_acquire(&super->lock);
	tmpfs_refresh_locked(inode);
	sleeplock_release(&super->lock);
	return vfs_inode_stat_default(inode, stat);
}

static int tmpfs_lookup(struct vfs_inode *directory, const char *name,
			struct vfs_inode **result)
{
	struct tmpfs_super *super = directory->superblock->private;
	struct tmpfs_inode *node = directory->private;
	struct tmpfs_entry *entry;

	if (node->type != VFS_INODE_DIRECTORY)
		return VFS_ERR_NOTDIR;
	sleeplock_acquire(&super->lock);
	entry = tmpfs_find_entry_locked(node, name, 0);
	if (!entry) {
		sleeplock_release(&super->lock);
		return VFS_ERR_NOENT;
	}
	*result = tmpfs_wrap_locked(directory->superblock, entry->inode);
	sleeplock_release(&super->lock);
	return *result ? VFS_OK : VFS_ERR_NOMEM;
}

static int tmpfs_create(struct vfs_inode *directory, const char *name,
			uint32 mode, uint32 uid, uint32 gid,
			struct vfs_inode **result)
{
	struct tmpfs_super *super = directory->superblock->private;
	struct tmpfs_inode *parent = directory->private;
	struct tmpfs_inode *node;
	int status;

	sleeplock_acquire(&super->lock);
	if (tmpfs_find_entry_locked(parent, name, 0)) {
		status = VFS_ERR_EXIST;
		goto out;
	}
	node = tmpfs_inode_alloc_locked(super, VFS_INODE_REGULAR, mode,
					uid, gid);
	if (!node) {
		status = VFS_ERR_NOMEM;
		goto out;
	}
	status = tmpfs_add_entry_locked(parent, name, node);
	if (status < 0) {
		tmpfs_inode_destroy_locked(node);
		goto out;
	}
	node->nlink = 1;
	*result = tmpfs_wrap_locked(directory->superblock, node);
	if (!*result) {
		struct tmpfs_entry *entry =
			tmpfs_find_entry_locked(parent, name, 0);

		parent->entries = entry->next;
		free(entry);
		node->nlink = 0;
		tmpfs_inode_destroy_locked(node);
		status = VFS_ERR_NOMEM;
		goto out;
	}
	status = VFS_OK;
	tmpfs_touch_locked(parent, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
out:
	sleeplock_release(&super->lock);
	return status;
}

static int tmpfs_mkdir(struct vfs_inode *directory, const char *name,
		       uint32 mode, uint32 uid, uint32 gid,
		       struct vfs_inode **result)
{
	struct tmpfs_super *super = directory->superblock->private;
	struct tmpfs_inode *parent = directory->private;
	struct tmpfs_inode *node;
	int status;

	sleeplock_acquire(&super->lock);
	if (tmpfs_find_entry_locked(parent, name, 0)) {
		status = VFS_ERR_EXIST;
		goto out;
	}
	node = tmpfs_inode_alloc_locked(super, VFS_INODE_DIRECTORY, mode,
					uid, gid);
	if (!node) {
		status = VFS_ERR_NOMEM;
		goto out;
	}
	status = tmpfs_add_entry_locked(parent, name, node);
	if (status < 0) {
		tmpfs_inode_destroy_locked(node);
		goto out;
	}
	node->nlink = 2;
	node->parent = parent;
	parent->nlink++;
	*result = tmpfs_wrap_locked(directory->superblock, node);
	if (!*result) {
		struct tmpfs_entry *entry =
			tmpfs_find_entry_locked(parent, name, 0);

		parent->entries = entry->next;
		free(entry);
		parent->nlink--;
		node->nlink = 0;
		tmpfs_inode_destroy_locked(node);
		status = VFS_ERR_NOMEM;
		goto out;
	}
	status = VFS_OK;
	tmpfs_touch_locked(parent, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
out:
	sleeplock_release(&super->lock);
	return status;
}

static int tmpfs_unlink(struct vfs_inode *directory, const char *name)
{
	struct tmpfs_super *super = directory->superblock->private;
	struct tmpfs_inode *parent = directory->private;
	struct tmpfs_entry *entry, *previous;
	int status = VFS_OK;

	sleeplock_acquire(&super->lock);
	entry = tmpfs_find_entry_locked(parent, name, &previous);
	if (!entry)
		status = VFS_ERR_NOENT;
	else if (entry->inode->type == VFS_INODE_DIRECTORY)
		status = VFS_ERR_ISDIR;
	else {
		tmpfs_touch_locked(entry->inode, TMPFS_TIME_CTIME);
		tmpfs_remove_entry_locked(parent, entry, previous);
		tmpfs_touch_locked(parent, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
	}
	sleeplock_release(&super->lock);
	return status;
}

static int tmpfs_rmdir(struct vfs_inode *directory, const char *name)
{
	struct tmpfs_super *super = directory->superblock->private;
	struct tmpfs_inode *parent = directory->private;
	struct tmpfs_entry *entry, *previous;
	int status = VFS_OK;

	sleeplock_acquire(&super->lock);
	entry = tmpfs_find_entry_locked(parent, name, &previous);
	if (!entry)
		status = VFS_ERR_NOENT;
	else if (entry->inode->type != VFS_INODE_DIRECTORY)
		status = VFS_ERR_NOTDIR;
	else if (entry->inode->entries)
		status = VFS_ERR_NOTEMPTY;
	else {
		tmpfs_touch_locked(entry->inode, TMPFS_TIME_CTIME);
		tmpfs_remove_entry_locked(parent, entry, previous);
		tmpfs_touch_locked(parent, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
	}
	sleeplock_release(&super->lock);
	return status;
}

static int tmpfs_link(struct vfs_inode *inode,
		      struct vfs_inode *directory, const char *name)
{
	struct tmpfs_super *super = directory->superblock->private;
	struct tmpfs_inode *parent = directory->private;
	struct tmpfs_inode *node = inode->private;
	int status;

	if (node->type == VFS_INODE_DIRECTORY)
		return VFS_ERR_PERM;
	sleeplock_acquire(&super->lock);
	status = tmpfs_add_entry_locked(parent, name, node);
	if (status == VFS_OK) {
		node->nlink++;
		tmpfs_touch_locked(node, TMPFS_TIME_CTIME);
		tmpfs_touch_locked(parent, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
	}
	sleeplock_release(&super->lock);
	return status;
}

static int tmpfs_symlink(struct vfs_inode *directory, const char *name,
			 const char *target, uint32 uid, uint32 gid,
			 struct vfs_inode **result)
{
	struct tmpfs_super *super = directory->superblock->private;
	struct tmpfs_inode *parent = directory->private;
	struct tmpfs_inode *node;
	uint32 length = strlen(target);
	int status;

	sleeplock_acquire(&super->lock);
	if (tmpfs_find_entry_locked(parent, name, 0)) {
		status = VFS_ERR_EXIST;
		goto out;
	}
	node = tmpfs_inode_alloc_locked(super, VFS_INODE_SYMLINK, 0777,
					uid, gid);
	if (!node) {
		status = VFS_ERR_NOMEM;
		goto out;
	}
	node->target = malloc(length + 1);
	if (!node->target) {
		tmpfs_inode_destroy_locked(node);
		status = VFS_ERR_NOMEM;
		goto out;
	}
	memmove(node->target, target, length + 1);
	node->size = length;
	status = tmpfs_add_entry_locked(parent, name, node);
	if (status < 0) {
		tmpfs_inode_destroy_locked(node);
		goto out;
	}
	node->nlink = 1;
	*result = tmpfs_wrap_locked(directory->superblock, node);
	if (!*result) {
		struct tmpfs_entry *entry =
			tmpfs_find_entry_locked(parent, name, 0);

		parent->entries = entry->next;
		free(entry);
		node->nlink = 0;
		tmpfs_inode_destroy_locked(node);
		status = VFS_ERR_NOMEM;
		goto out;
	}
	status = VFS_OK;
	tmpfs_touch_locked(parent, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
out:
	sleeplock_release(&super->lock);
	return status;
}

static int tmpfs_mknod(struct vfs_inode *directory, const char *name,
		       enum vfs_inode_type type, uint32 mode, uint32 uid,
		       uint32 gid, uint64 device, struct vfs_inode **result)
{
	struct tmpfs_super *super = directory->superblock->private;
	struct tmpfs_inode *parent = directory->private;
	struct tmpfs_inode *node;
	int status;

	if (type != VFS_INODE_CHAR_DEVICE &&
	    type != VFS_INODE_BLOCK_DEVICE && type != VFS_INODE_FIFO &&
	    type != VFS_INODE_SOCKET)
		return VFS_ERR_INVAL;
	sleeplock_acquire(&super->lock);
	if (tmpfs_find_entry_locked(parent, name, 0)) {
		status = VFS_ERR_EXIST;
		goto out;
	}
	node = tmpfs_inode_alloc_locked(super, type, mode, uid, gid);
	if (!node) {
		status = VFS_ERR_NOMEM;
		goto out;
	}
	node->device = device;
	status = tmpfs_add_entry_locked(parent, name, node);
	if (status < 0) {
		tmpfs_inode_destroy_locked(node);
		goto out;
	}
	node->nlink = 1;
	*result = tmpfs_wrap_locked(directory->superblock, node);
	if (!*result) {
		struct tmpfs_entry *entry =
			tmpfs_find_entry_locked(parent, name, 0);

		parent->entries = entry->next;
		free(entry);
		node->nlink = 0;
		tmpfs_inode_destroy_locked(node);
		status = VFS_ERR_NOMEM;
		goto out;
	}
	status = VFS_OK;
	tmpfs_touch_locked(parent, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
out:
	sleeplock_release(&super->lock);
	return status;
}

static int tmpfs_readlink(struct vfs_inode *inode, char *buffer,
			  uint32 size)
{
	struct tmpfs_super *super = inode->superblock->private;
	struct tmpfs_inode *node = inode->private;
	uint32 length;

	sleeplock_acquire(&super->lock);
	length = node->size > size ? size : node->size;
	memmove(buffer, node->target, length);
	tmpfs_touch_locked(node, VFS_TIME_ATIME);
	sleeplock_release(&super->lock);
	return length;
}

static struct tmpfs_page *tmpfs_find_page_locked(struct tmpfs_inode *inode,
						 uint64 index)
{
	struct tmpfs_page *page;

	for (page = inode->pages; page; page = page->next) {
		if (page->index == index)
			return page;
	}
	return 0;
}

static struct tmpfs_page *tmpfs_get_page_locked(struct tmpfs_inode *inode,
						uint64 index)
{
	struct tmpfs_page *page = tmpfs_find_page_locked(inode, index);

	if (page)
		return page;
	page = malloc(sizeof(*page));
	if (!page)
		return 0;
	page->data = palloc_zero();
	if (!page->data) {
		free(page);
		return 0;
	}
	page->index = index;
	page->next = inode->pages;
	inode->pages = page;
	inode->page_count++;
	return page;
}

static int tmpfs_truncate(struct vfs_inode *inode, uint64 size)
{
	struct tmpfs_super *super = inode->superblock->private;
	struct tmpfs_inode *node = inode->private;
	struct tmpfs_page **link, *page;
	uint64 page_limit;
	uint32 offset;

	if (node->type != VFS_INODE_REGULAR)
		return VFS_ERR_INVAL;
	sleeplock_acquire(&super->lock);
	if (size < node->size) {
		page_limit = size / PGSIZE + !!(size % PGSIZE);
		for (link = &node->pages; (page = *link);) {
			if (page->index >= page_limit) {
				*link = page->next;
				pfree(page->data);
				free(page);
				node->page_count--;
				continue;
			}
			link = &page->next;
		}
		offset = size % PGSIZE;
		if (offset) {
			page = tmpfs_find_page_locked(node, size / PGSIZE);
			if (page)
				memset((char *)page->data + offset, 0,
				       PGSIZE - offset);
		}
	}
	node->size = size;
	tmpfs_touch_locked(node, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
	tmpfs_refresh_locked(inode);
	sleeplock_release(&super->lock);
	return VFS_OK;
}

static int tmpfs_rename(struct vfs_inode *old_directory,
			const char *old_name,
			struct vfs_inode *new_directory,
			const char *new_name, uint32 flags)
{
	struct tmpfs_super *super = old_directory->superblock->private;
	struct tmpfs_inode *old_parent = old_directory->private;
	struct tmpfs_inode *new_parent = new_directory->private;
	struct tmpfs_inode *cursor, *source;
	struct tmpfs_entry *old_entry, *old_previous;
	struct tmpfs_entry *new_entry, *new_previous;
	int status = VFS_OK;

	sleeplock_acquire(&super->lock);
	old_entry = tmpfs_find_entry_locked(old_parent, old_name,
	                                      &old_previous);
	if (!old_entry) {
		status = VFS_ERR_NOENT;
		goto out;
	}
	source = old_entry->inode;
	new_entry = tmpfs_find_entry_locked(new_parent, new_name,
	                                      &new_previous);
	if (new_entry && new_entry->inode == source)
		goto out;
	if (new_entry && (flags & VFS_RENAME_NOREPLACE)) {
		status = VFS_ERR_EXIST;
		goto out;
	}
	if (new_entry && source->type == VFS_INODE_DIRECTORY &&
	    new_entry->inode->type != VFS_INODE_DIRECTORY) {
		status = VFS_ERR_NOTDIR;
		goto out;
	}
	if (new_entry && source->type != VFS_INODE_DIRECTORY &&
	    new_entry->inode->type == VFS_INODE_DIRECTORY) {
		status = VFS_ERR_ISDIR;
		goto out;
	}
	if (new_entry && new_entry->inode->type == VFS_INODE_DIRECTORY &&
	    new_entry->inode->entries) {
		status = VFS_ERR_NOTEMPTY;
		goto out;
	}
	if (source->type == VFS_INODE_DIRECTORY) {
		for (cursor = new_parent;; cursor = cursor->parent) {
			if (cursor == source) {
				status = VFS_ERR_INVAL;
				goto out;
			}
			if (cursor == cursor->parent)
				break;
		}
	}
	if (new_entry) {
		tmpfs_touch_locked(new_entry->inode, TMPFS_TIME_CTIME);
		tmpfs_remove_entry_locked(new_parent, new_entry, new_previous);
		old_entry = tmpfs_find_entry_locked(old_parent, old_name,
		                                      &old_previous);
		if (!old_entry)
			PANIC("tmpfs rename source");
	}
	tmpfs_detach_entry_locked(old_parent, old_entry, old_previous);
	safe_strncpy(old_entry->name, new_name, sizeof(old_entry->name));
	old_entry->next = new_parent->entries;
	new_parent->entries = old_entry;
	if (source->type == VFS_INODE_DIRECTORY && old_parent != new_parent) {
		old_parent->nlink--;
		new_parent->nlink++;
		source->parent = new_parent;
	}
	tmpfs_touch_locked(source, TMPFS_TIME_CTIME);
	tmpfs_touch_locked(old_parent, VFS_TIME_MTIME | TMPFS_TIME_CTIME);
	if (new_parent != old_parent)
		tmpfs_touch_locked(new_parent,
				   VFS_TIME_MTIME | TMPFS_TIME_CTIME);
out:
	sleeplock_release(&super->lock);
	return status;
}

static int tmpfs_set_times(struct vfs_inode *inode,
			   const struct vfs_timespec times[2], uint32 mask)
{
	struct tmpfs_super *super = inode->superblock->private;
	struct tmpfs_inode *node = inode->private;

	sleeplock_acquire(&super->lock);
	if (mask & VFS_TIME_ATIME)
		node->atime = times[0];
	if (mask & VFS_TIME_MTIME)
		node->mtime = times[1];
	tmpfs_touch_locked(node, TMPFS_TIME_CTIME);
	tmpfs_refresh_locked(inode);
	sleeplock_release(&super->lock);
	return VFS_OK;
}

static int tmpfs_setattr(struct vfs_inode *inode,
			 const struct vfs_iattr *attributes)
{
	struct tmpfs_super *super = inode->superblock->private;
	struct tmpfs_inode *node = inode->private;

	sleeplock_acquire(&super->lock);
	if (attributes->mask & VFS_ATTR_MODE)
		node->mode = attributes->mode & VFS_MODE_PERMISSIONS;
	if (attributes->mask & VFS_ATTR_UID)
		node->uid = attributes->uid;
	if (attributes->mask & VFS_ATTR_GID)
		node->gid = attributes->gid;
	tmpfs_touch_locked(node, TMPFS_TIME_CTIME);
	tmpfs_refresh_locked(inode);
	sleeplock_release(&super->lock);
	return VFS_OK;
}

static const struct vfs_inode_operations tmpfs_inode_operations = {
	.lookup = tmpfs_lookup,
	.create = tmpfs_create,
	.mkdir = tmpfs_mkdir,
	.unlink = tmpfs_unlink,
	.rmdir = tmpfs_rmdir,
	.rename = tmpfs_rename,
	.link = tmpfs_link,
	.symlink = tmpfs_symlink,
	.mknod = tmpfs_mknod,
	.readlink = tmpfs_readlink,
	.truncate = tmpfs_truncate,
	.setattr = tmpfs_setattr,
	.set_times = tmpfs_set_times,
	.getattr = tmpfs_getattr,
};

static int64 tmpfs_read(struct vfs_file *file, int user_destination,
			uint64 destination, uint64 count, uint64 *position)
{
	struct tmpfs_super *super =
		file->path.dentry->inode->superblock->private;
	struct tmpfs_inode *inode = file->path.dentry->inode->private;
	struct tmpfs_page *page;
	int64 total = 0;
	uint64 available;
	uint32 offset, chunk;
	const void *source;

	sleeplock_acquire(&super->lock);
	if (*position >= inode->size)
		goto out;
	available = inode->size - *position;
	if (count > available)
		count = available;
	while (total < count) {
		offset = *position % PGSIZE;
		chunk = count - (uint64)total > PGSIZE - offset ?
			PGSIZE - offset : count - total;
		page = tmpfs_find_page_locked(inode, *position / PGSIZE);
		source = page ? (char *)page->data + offset :
			(char *)tmpfs_zero_page + offset;
		if (either_copyout(user_destination, destination + total,
		                   (void *)source, chunk) < 0) {
			total = total ? total : VFS_ERR_FAULT;
			goto out;
		}
		*position += chunk;
		total += chunk;
	}
out:
	if (total >= 0 && count)
		tmpfs_touch_locked(inode, VFS_TIME_ATIME);
	sleeplock_release(&super->lock);
	return total;
}

static int64 tmpfs_write(struct vfs_file *file, int user_source,
			 uint64 source, uint64 count, uint64 *position)
{
	struct tmpfs_super *super =
		file->path.dentry->inode->superblock->private;
	struct tmpfs_inode *inode = file->path.dentry->inode->private;
	struct tmpfs_page *page;
	uint64 total = 0;
	uint32 offset, chunk;
	int error = VFS_ERR_NOMEM;

	if (count > ~(uint64)0 - *position)
		return VFS_ERR_INVAL;
	sleeplock_acquire(&super->lock);
	while (total < count) {
		offset = *position % PGSIZE;
		chunk = count - total > PGSIZE - offset ?
			PGSIZE - offset : count - total;
		page = tmpfs_get_page_locked(inode, *position / PGSIZE);
		if (!page)
			break;
		if (either_copyin((char *)page->data + offset, user_source,
		                  source + total, chunk) < 0) {
			error = VFS_ERR_IO;
			break;
		}
		*position += chunk;
		total += chunk;
	}
	if (*position > inode->size)
		inode->size = *position;
	if (total)
		tmpfs_touch_locked(inode,
				   VFS_TIME_MTIME | TMPFS_TIME_CTIME);
	tmpfs_refresh_locked(file->path.dentry->inode);
	sleeplock_release(&super->lock);
	return total ? total : count ? error : 0;
}

static int tmpfs_fallocate(struct vfs_file *file, uint64 offset,
			   uint64 length)
{
	struct tmpfs_super *super =
		file->path.dentry->inode->superblock->private;
	struct tmpfs_inode *inode = file->path.dentry->inode->private;
	uint64 end = offset + length, first, last, index;
	int result = VFS_OK;

	if (!length)
		return VFS_OK;
	first = offset / PGSIZE;
	last = (end - 1) / PGSIZE;
	sleeplock_acquire(&super->lock);
	for (index = first; index <= last; index++) {
		if (!tmpfs_get_page_locked(inode, index)) {
			result = VFS_ERR_NOSPC;
			break;
		}
	}
	if (result == VFS_OK && end > inode->size)
		inode->size = end;
	if (result == VFS_OK)
		tmpfs_touch_locked(inode,
				   VFS_TIME_MTIME | TMPFS_TIME_CTIME);
	tmpfs_refresh_locked(file->path.dentry->inode);
	sleeplock_release(&super->lock);
	return result;
}

static int tmpfs_file_sync(struct vfs_file *file)
{
	(void)file;
	return VFS_OK;
}

static const struct vfs_file_operations tmpfs_file_operations = {
	.flags = VFS_FILE_CAN_PREAD,
	.read = tmpfs_read,
	.write = tmpfs_write,
	.fsync = tmpfs_file_sync,
	.fallocate = tmpfs_fallocate,
};

static uint8 tmpfs_dirent_type(enum vfs_inode_type type)
{
	switch (type) {
	case VFS_INODE_DIRECTORY:
		return VFS_DT_DIR;
	case VFS_INODE_REGULAR:
		return VFS_DT_REGULAR;
	case VFS_INODE_SYMLINK:
		return VFS_DT_SYMLINK;
	case VFS_INODE_CHAR_DEVICE:
		return VFS_DT_CHAR;
	case VFS_INODE_BLOCK_DEVICE:
		return VFS_DT_BLOCK;
	case VFS_INODE_FIFO:
		return VFS_DT_FIFO;
	case VFS_INODE_SOCKET:
		return VFS_DT_SOCKET;
	default:
		return VFS_DT_UNKNOWN;
	}
}

static int tmpfs_readdir(struct vfs_file *file,
			 struct vfs_dirent *result)
{
	struct tmpfs_super *super =
		file->path.dentry->inode->superblock->private;
	struct tmpfs_inode *directory = file->path.dentry->inode->private;
	struct tmpfs_entry *entry;
	uint64 position = file->position;

	sleeplock_acquire(&super->lock);
	if (position < 2) {
		result->ino = position ? directory->parent->number :
			directory->number;
		result->type = VFS_DT_DIR;
		safe_strncpy(result->name, position ? ".." : ".",
		             sizeof(result->name));
	} else {
		entry = directory->entries;
		while (entry && position-- > 2)
			entry = entry->next;
		if (!entry) {
			sleeplock_release(&super->lock);
			return 0;
		}
		result->ino = entry->inode->number;
		result->type = tmpfs_dirent_type(entry->inode->type);
		safe_strncpy(result->name, entry->name,
		             sizeof(result->name));
	}
	result->next_offset = ++file->position;
	tmpfs_touch_locked(directory, VFS_TIME_ATIME);
	sleeplock_release(&super->lock);
	return 1;
}

static const struct vfs_file_operations tmpfs_directory_operations = {
	.readdir = tmpfs_readdir,
	.fsync = tmpfs_file_sync,
};

static int tmpfs_mount(struct vfs_filesystem_type *type,
		       struct block_device *device, const void *data,
		       struct vfs_super_block **result)
{
	struct vfs_super_block *superblock;
	struct tmpfs_super *super;

	(void)data;
	if (device)
		return VFS_ERR_INVAL;
	super = malloc(sizeof(*super));
	if (!super)
		return VFS_ERR_NOMEM;
	memset(super, 0, sizeof(*super));
	sleeplock_init(&super->lock, "tmpfs");
	super->next_inode = 1;
	super->root = tmpfs_inode_alloc_locked(super,
					       VFS_INODE_DIRECTORY, 01777,
					       0, 0);
	if (!super->root) {
		free(super);
		return VFS_ERR_NOMEM;
	}
	super->root->nlink = 2;
	super->root->parent = super->root;
	superblock = vfs_super_alloc(type, 0);
	if (!superblock) {
		tmpfs_inode_destroy_locked(super->root);
		free(super);
		return VFS_ERR_NOMEM;
	}
	superblock->operations = &tmpfs_super_operations;
	superblock->block_size = PGSIZE;
	superblock->private = super;
	superblock->root = tmpfs_wrap_locked(superblock, super->root);
	if (!superblock->root) {
		superblock->private = 0;
		vfs_super_free(superblock);
		tmpfs_inode_destroy_locked(super->root);
		free(super);
		return VFS_ERR_NOMEM;
	}
	*result = superblock;
	return VFS_OK;
}

static struct vfs_filesystem_type tmpfs_type = {
	.name = "tmpfs",
	.mount = tmpfs_mount,
};

void tmpfs_init(void)
{
	if (vfs_register_filesystem(&tmpfs_type) != VFS_OK)
		PANIC("register tmpfs");
}
