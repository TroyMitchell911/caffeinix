#include <list.h>
#include <mystring.h>
#include <page_cache.h>
#include <palloc.h>
#include <riscv.h>
#include <sleeplock.h>
#include <vfs.h>

struct page_cache_entry {
	struct list node;
	struct vfs_file *file;
	struct vfs_super_block *superblock;
	uint64 inode_number;
	uint64 offset;
	void *page;
};

static struct {
	struct sleeplock lock;
	struct list entries;
	struct page_cache_stats stats;
} page_cache;

void page_cache_init(void)
{
	sleeplock_init(&page_cache.lock, "page cache");
	list_init(&page_cache.entries);
	page_cache.stats = (struct page_cache_stats){0};
}

static struct page_cache_entry *page_cache_find(
	struct vfs_super_block *superblock, uint64 inode_number,
	uint64 offset)
{
	list_t node;

	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = node->next) {
		struct page_cache_entry *entry;

		entry = list_entry(node, struct page_cache_entry, node);
		if (entry->superblock == superblock &&
		    entry->inode_number == inode_number &&
		    entry->offset == offset)
			return entry;
	}
	return 0;
}

static void page_cache_release(struct page_cache_entry *entry)
{
	list_remove(&entry->node);
	vfs_file_put(entry->file);
	pfree(entry->page);
	free(entry);
	page_cache.stats.pages--;
}

static uint64 page_cache_reclaim_locked(uint64 target)
{
	list_t node, next;
	uint64 reclaimed = 0;

	for (node = page_cache.entries.prev;
	     node != &page_cache.entries && reclaimed < target; node = next) {
		struct page_cache_entry *entry;

		next = node->prev;
		entry = list_entry(node, struct page_cache_entry, node);
		if (palloc_refcount(entry->page) != 1)
			continue;
		page_cache_release(entry);
		reclaimed++;
	}
	page_cache.stats.reclaimed += reclaimed;
	return reclaimed;
}

uint64 page_cache_reclaim(uint64 target)
{
	uint64 reclaimed;

	sleeplock_acquire(&page_cache.lock);
	reclaimed = page_cache_reclaim_locked(target);
	sleeplock_release(&page_cache.lock);
	return reclaimed;
}

int page_cache_get(struct vfs_file *file, uint64 offset, void **page)
{
	struct page_cache_entry *entry;
	struct vfs_inode *inode;
	void *allocated;

	if (!file || !page || offset % PGSIZE || !file->path.dentry ||
	    !(inode = file->path.dentry->inode) ||
	    inode->type != VFS_INODE_REGULAR)
		return -1;
	sleeplock_acquire(&page_cache.lock);
	entry = page_cache_find(inode->superblock, inode->number, offset);
	if (entry) {
		if (palloc_get(entry->page) < 0) {
			sleeplock_release(&page_cache.lock);
			return -1;
		}
		list_remove(&entry->node);
		list_insert_after(&page_cache.entries, &entry->node);
		page_cache.stats.hits++;
		*page = entry->page;
		sleeplock_release(&page_cache.lock);
		return 0;
	}
	entry = malloc(sizeof(*entry));
	allocated = palloc_zero();
	if ((!entry || !allocated) && page_cache_reclaim_locked(1)) {
		if (!entry)
			entry = malloc(sizeof(*entry));
		if (!allocated)
			allocated = palloc_zero();
	}
	if (!entry || !allocated)
		goto failed;
	if (vfs_file_pread(file, 0, (uint64)allocated, PGSIZE, offset) !=
	    PGSIZE)
		goto failed;
	list_init(&entry->node);
	entry->file = vfs_file_get(file);
	entry->superblock = inode->superblock;
	entry->inode_number = inode->number;
	entry->offset = offset;
	entry->page = allocated;
	list_insert_after(&page_cache.entries, &entry->node);
	page_cache.stats.pages++;
	page_cache.stats.misses++;
	if (palloc_get(allocated) < 0)
		goto failed_inserted;
	*page = allocated;
	sleeplock_release(&page_cache.lock);
	return 0;

failed_inserted:
	page_cache_release(entry);
	sleeplock_release(&page_cache.lock);
	return -1;
failed:
	if (allocated)
		pfree(allocated);
	if (entry)
		free(entry);
	sleeplock_release(&page_cache.lock);
	return -1;
}

void page_cache_get_stats(struct page_cache_stats *stats)
{
	if (!stats)
		return;
	sleeplock_acquire(&page_cache.lock);
	*stats = page_cache.stats;
	sleeplock_release(&page_cache.lock);
}
