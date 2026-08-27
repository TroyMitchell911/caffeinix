#include <cpu.h>
#include <list.h>
#include <mmap.h>
#include <mystring.h>
#include <page_cache.h>
#include <palloc.h>
#include <process.h>
#include <riscv.h>
#include <scheduler.h>
#include <sleeplock.h>
#include <vfs.h>

struct page_cache_entry {
	struct list node;
	struct vfs_file *file;
	struct vfs_super_block *superblock;
	uint64 inode_number;
	uint64 offset;
	void *page;
	uint8 dirty;
	uint8 writeback_mapped;
	uint8 executable_mapped;
	uint8 evicting;
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

static int page_cache_same_inode(const struct page_cache_entry *entry,
				 const struct vfs_inode *inode)
{
	return inode && entry->superblock == inode->superblock &&
	       entry->inode_number == inode->number;
}

static void page_cache_release(struct page_cache_entry *entry)
{
	list_remove(&entry->node);
	vfs_file_unhold(entry->file);
	pfree(entry->page);
	free(entry);
	page_cache.stats.pages--;
}

static struct page_cache_entry *page_cache_select_clean_locked(void)
{
	struct page_cache_entry *entry;
	list_t node;

	for (node = page_cache.entries.prev; node != &page_cache.entries;
	     node = node->prev) {
		entry = list_entry(node, struct page_cache_entry, node);
		if (entry->dirty || entry->writeback_mapped || entry->evicting)
			continue;
		entry->evicting = 1;
		return entry;
	}
	return 0;
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
		if (entry->dirty || entry->writeback_mapped || entry->evicting ||
		    palloc_refcount(entry->page) != 1)
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

uint64 page_cache_reclaim_mapped(uint64 target)
{
	struct page_cache_entry *entry;
	uint64 budget, reclaimed = 0;
	int blocked;

	sleeplock_acquire(&page_cache.lock);
	budget = page_cache.stats.pages;
	sleeplock_release(&page_cache.lock);
	while (reclaimed < target && budget--) {
		sleeplock_acquire(&page_cache.lock);
		entry = page_cache_select_clean_locked();
		sleeplock_release(&page_cache.lock);
		if (!entry)
			break;

		blocked = mmap_reclaim_file_page(entry->file, entry->offset,
						 entry->page);

		sleeplock_acquire(&page_cache.lock);
		if (blocked || entry->dirty || entry->writeback_mapped ||
		    palloc_refcount(entry->page) != 1) {
			entry->evicting = 0;
			list_remove(&entry->node);
			list_insert_after(&page_cache.entries, &entry->node);
			sleeplock_release(&page_cache.lock);
			continue;
		}
		page_cache_release(entry);
		reclaimed++;
		page_cache.stats.reclaimed++;
		sleeplock_release(&page_cache.lock);
	}
	return reclaimed;
}

uint64 page_cache_reclaim_unmapped(void)
{
	struct page_cache_entry *entry;
	list_t next, node;
	uint64 reclaimed = 0;

	sleeplock_acquire(&page_cache.lock);
	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = next) {
		next = node->next;
		entry = list_entry(node, struct page_cache_entry, node);
		if (entry->dirty || !entry->writeback_mapped ||
		    entry->evicting || palloc_refcount(entry->page) != 1)
			continue;
		entry->writeback_mapped = 0;
		page_cache_release(entry);
		reclaimed++;
	}
	page_cache.stats.reclaimed += reclaimed;
	sleeplock_release(&page_cache.lock);
	return reclaimed;
}

enum page_cache_get_result page_cache_get(struct vfs_file *file,
					  uint64 offset, uint32 bytes,
					  void **page)
{
	struct page_cache_entry *entry;
	struct vfs_inode *inode;
	enum page_cache_get_result failure = PAGE_CACHE_GET_ERROR;
	void *allocated;
	int64 read_result;

	if (!file || !page || !bytes || bytes > PGSIZE ||
	    offset % PGSIZE || !file->path.dentry ||
	    !(inode = file->path.dentry->inode) ||
	    inode->type != VFS_INODE_REGULAR)
		return PAGE_CACHE_GET_ERROR;
	sleeplock_acquire(&page_cache.lock);
	entry = page_cache_find(inode->superblock, inode->number, offset);
	if (entry) {
		if (entry->evicting) {
			sleeplock_release(&page_cache.lock);
			return PAGE_CACHE_GET_RETRY;
		}
		if (palloc_get(entry->page) < 0) {
			sleeplock_release(&page_cache.lock);
			return PAGE_CACHE_GET_ERROR;
		}
		list_remove(&entry->node);
		list_insert_after(&page_cache.entries, &entry->node);
		page_cache.stats.hits++;
		*page = entry->page;
		sleeplock_release(&page_cache.lock);
		return PAGE_CACHE_GET_OK;
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
	read_result = vfs_file_pread_raw(file, 0, (uint64)allocated, bytes,
					 offset);
	if (read_result != bytes) {
		if (read_result != VFS_ERR_NOMEM)
			failure = PAGE_CACHE_GET_IO;
		goto failed;
	}
	list_init(&entry->node);
	entry->file = vfs_file_hold(file);
	entry->superblock = inode->superblock;
	entry->inode_number = inode->number;
	entry->offset = offset;
	entry->page = allocated;
	entry->dirty = 0;
	entry->writeback_mapped = 0;
	entry->executable_mapped = 0;
	entry->evicting = 0;
	list_insert_after(&page_cache.entries, &entry->node);
	page_cache.stats.pages++;
	page_cache.stats.misses++;
	if (palloc_get(allocated) < 0)
		goto failed_inserted;
	*page = allocated;
	sleeplock_release(&page_cache.lock);
	return PAGE_CACHE_GET_OK;

failed_inserted:
	page_cache_release(entry);
	sleeplock_release(&page_cache.lock);
	return PAGE_CACHE_GET_ERROR;
failed:
	if (allocated)
		pfree(allocated);
	if (entry)
		free(entry);
	sleeplock_release(&page_cache.lock);
	return failure;
}

int page_cache_mark_dirty(struct vfs_file *file, uint64 offset)
{
	struct page_cache_entry *entry;
	struct vfs_file *old_file = 0;
	struct vfs_inode *inode;
	int result = -1;

	if (!file || !(file->flags & VFS_OPEN_WRITE) ||
	    !file->path.dentry || !(inode = file->path.dentry->inode) ||
	    offset % PGSIZE)
		return -1;
	sleeplock_acquire(&page_cache.lock);
	entry = page_cache_find(inode->superblock, inode->number, offset);
	if (!entry)
		goto out;
	if (!(entry->file->flags & VFS_OPEN_WRITE)) {
		old_file = entry->file;
		entry->file = vfs_file_hold(file);
	}
	entry->dirty = 1;
	entry->writeback_mapped = 1;
	result = 0;
out:
	sleeplock_release(&page_cache.lock);
	if (old_file)
		vfs_file_unhold(old_file);
	return result;
}

int page_cache_mark_executable(struct vfs_file *file, uint64 offset)
{
	struct page_cache_entry *entry;
	struct vfs_inode *inode;
	int result = -1;

	if (!file || !file->path.dentry ||
	    !(inode = file->path.dentry->inode) || offset % PGSIZE)
		return -1;
	sleeplock_acquire(&page_cache.lock);
	entry = page_cache_find(inode->superblock, inode->number, offset);
	if (entry && !entry->evicting) {
		entry->executable_mapped = 1;
		result = 0;
	}
	sleeplock_release(&page_cache.lock);
	return result;
}

int page_cache_refresh(struct vfs_file *file, int user_source,
		       uint64 source, uint64 offset, uint64 count,
		       uint64 old_size)
{
	struct page_cache_entry *entry;
	struct vfs_inode *inode;
	uint64 end, finish, refresh_start, start;
	list_t node;
	int flush_icache = 0, result = 0;

	if (!count)
		return 0;
	if (!file || !file->path.dentry ||
	    !(inode = file->path.dentry->inode) ||
	    count > (uint64)-1 - offset || count > (uint64)-1 - source)
		return -1;
	end = offset + count;
	refresh_start = old_size < offset ? old_size : offset;
	sleeplock_acquire(&page_cache.lock);
	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = node->next) {
		entry = list_entry(node, struct page_cache_entry, node);
		if (!page_cache_same_inode(entry, inode) ||
		    end <= entry->offset ||
		    refresh_start >= entry->offset + PGSIZE)
			continue;
		if (entry->executable_mapped)
			flush_icache = 1;
		if (old_size < offset) {
			start = old_size > entry->offset ?
				old_size : entry->offset;
			finish = offset < entry->offset + PGSIZE ?
				offset : entry->offset + PGSIZE;
			if (start < finish)
				memset((char *)entry->page + start - entry->offset,
				       0, finish - start);
		}
		start = offset > entry->offset ? offset : entry->offset;
		finish = end < entry->offset + PGSIZE ?
			end : entry->offset + PGSIZE;
		if (start >= finish)
			continue;
		if (vfs_file_pread_raw(entry->file, 0,
				   (uint64)entry->page + start - entry->offset,
				   finish - start, start) != (int64)(finish - start) &&
		    either_copyin((char *)entry->page + start - entry->offset,
				  user_source, source + start - offset,
				  finish - start) < 0) {
			result = -1;
			break;
		}
	}
	sleeplock_release(&page_cache.lock);
	if (flush_icache)
		cpu_icache_flush_all();
	return result;
}

static int page_cache_writeback_entry(struct page_cache_entry *entry)
{
	struct vfs_stat stat;
	uint64 bytes;

	if (!entry->dirty && !entry->writeback_mapped)
		return 0;
	if (!(entry->file->flags & VFS_OPEN_WRITE) ||
	    vfs_inode_stat(entry->file->path.dentry->inode, &stat) < 0)
		return -1;
	if (entry->offset >= stat.size) {
		entry->dirty = 0;
		entry->writeback_mapped = 0;
		return 0;
	}
	bytes = stat.size - entry->offset;
	if (bytes > PGSIZE)
		bytes = PGSIZE;
	if (vfs_file_pwrite_raw(entry->file, 0, (uint64)entry->page,
				bytes, entry->offset) != (int64)bytes)
		return -1;
	entry->dirty = 0;
	if (palloc_refcount(entry->page) == 1)
		entry->writeback_mapped = 0;
	return 0;
}

static int page_cache_writeback_locked(struct vfs_inode *inode,
				       struct vfs_super_block *superblock)
{
	struct page_cache_entry *entry;
	list_t node;
	int result = 0;

	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = node->next) {
		entry = list_entry(node, struct page_cache_entry, node);
		if (inode && !page_cache_same_inode(entry, inode))
			continue;
		if (superblock && entry->superblock != superblock)
			continue;
		if (page_cache_writeback_entry(entry) < 0)
			result = -1;
	}
	return result;
}

int page_cache_writeback_file(struct vfs_file *file)
{
	struct vfs_inode *inode;

	if (!file || !file->path.dentry ||
	    !(inode = file->path.dentry->inode) ||
	    inode->type != VFS_INODE_REGULAR)
		return 0;
	return page_cache_writeback_inode(inode);
}

int page_cache_writeback_inode(struct vfs_inode *inode)
{
	int result;

	if (!inode || !inode->superblock)
		return -1;
	sleeplock_acquire(&inode->superblock->write_lock);
	sleeplock_acquire(&page_cache.lock);
	result = page_cache_writeback_locked(inode, 0);
	sleeplock_release(&page_cache.lock);
	sleeplock_release(&inode->superblock->write_lock);
	return result;
}

int page_cache_writeback_inode_locked(struct vfs_inode *inode)
{
	int result;

	if (!inode || !inode->superblock ||
	    !sleeplock_holding(&inode->superblock->write_lock))
		return -1;
	sleeplock_acquire(&page_cache.lock);
	result = page_cache_writeback_locked(inode, 0);
	sleeplock_release(&page_cache.lock);
	return result;
}

int page_cache_writeback_super(struct vfs_super_block *superblock)
{
	int result;

	if (!superblock)
		return -1;
	sleeplock_acquire(&superblock->write_lock);
	sleeplock_acquire(&page_cache.lock);
	result = page_cache_writeback_locked(0, superblock);
	sleeplock_release(&page_cache.lock);
	sleeplock_release(&superblock->write_lock);
	return result;
}

int page_cache_evict_super(struct vfs_super_block *superblock)
{
	struct page_cache_entry *entry;
	list_t next, node;
	int result = VFS_OK;

	if (!superblock)
		return VFS_ERR_INVAL;
	sleeplock_acquire(&superblock->write_lock);
	sleeplock_acquire(&page_cache.lock);
	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = node->next) {
		entry = list_entry(node, struct page_cache_entry, node);
		if (entry->superblock != superblock)
			continue;
		if (entry->evicting || palloc_refcount(entry->page) != 1) {
			result = VFS_ERR_BUSY;
			goto out;
		}
	}
	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = node->next) {
		entry = list_entry(node, struct page_cache_entry, node);
		if (entry->superblock == superblock &&
		    page_cache_writeback_entry(entry) < 0) {
			result = VFS_ERR_IO;
			goto out;
		}
	}
	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = next) {
		next = node->next;
		entry = list_entry(node, struct page_cache_entry, node);
		if (entry->superblock == superblock)
			page_cache_release(entry);
	}
out:
	sleeplock_release(&page_cache.lock);
	sleeplock_release(&superblock->write_lock);
	return result;
}

int page_cache_truncate(struct vfs_inode *inode, uint64 old_size,
			uint64 size)
{
	struct page_cache_entry *entry;
	uint64 first = PGROUNDDOWN(size);
	uint64 limit = PGROUNDUP(size);
	uint64 finish, start;
	list_t node;

	if (!inode || (size && !limit))
		return PAGE_CACHE_TRUNCATE_ERROR;
	sleeplock_acquire(&page_cache.lock);
	if (size >= old_size) {
		if (size == old_size || !(old_size % PGSIZE))
			goto out;
		entry = page_cache_find(inode->superblock, inode->number,
					PGROUNDDOWN(old_size));
		if (!entry)
			goto out;
		if (entry->evicting) {
			sleeplock_release(&page_cache.lock);
			return PAGE_CACHE_TRUNCATE_RETRY;
		}
		start = old_size % PGSIZE;
		finish = size - entry->offset;
		if (finish > PGSIZE)
			finish = PGSIZE;
		if (finish > start)
			memset((char *)entry->page + start, 0, finish - start);
		goto out;
	}
	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = node->next) {
		entry = list_entry(node, struct page_cache_entry, node);
		if (!page_cache_same_inode(entry, inode) ||
		    entry->offset < first)
			continue;
		if (entry->evicting) {
			sleeplock_release(&page_cache.lock);
			return PAGE_CACHE_TRUNCATE_RETRY;
		}
	}
	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = node->next) {
		entry = list_entry(node, struct page_cache_entry, node);
		if (page_cache_same_inode(entry, inode) &&
		    entry->offset >= first)
			entry->evicting = 1;
	}
	for (;;) {
		entry = 0;
		for (node = page_cache.entries.next;
		     node != &page_cache.entries; node = node->next) {
			struct page_cache_entry *candidate;

			candidate = list_entry(node, struct page_cache_entry,
			                       node);
			if (page_cache_same_inode(candidate, inode) &&
			    candidate->offset >= first && candidate->evicting) {
				entry = candidate;
				break;
			}
		}
		if (!entry)
			break;
		while (palloc_refcount(entry->page) != 1) {
			sleeplock_release(&page_cache.lock);
			yield();
			sleeplock_acquire(&page_cache.lock);
		}
		if (entry->offset >= limit) {
			page_cache_release(entry);
		} else {
			memset((char *)entry->page + size % PGSIZE, 0,
			       PGSIZE - size % PGSIZE);
			entry->dirty = 0;
			entry->writeback_mapped = 0;
			entry->evicting = 0;
		}
	}
out:
	sleeplock_release(&page_cache.lock);
	return PAGE_CACHE_TRUNCATE_OK;
}

void page_cache_get_stats(struct page_cache_stats *stats)
{
	struct page_cache_entry *entry;
	uint32 references;
	list_t node;

	if (!stats)
		return;
	sleeplock_acquire(&page_cache.lock);
	*stats = page_cache.stats;
	stats->mapped_pages = 0;
	stats->shared_pages = 0;
	stats->mapping_references = 0;
	for (node = page_cache.entries.next; node != &page_cache.entries;
	     node = node->next) {
		entry = list_entry(node, struct page_cache_entry, node);
		references = palloc_refcount(entry->page);
		if (references > 1) {
			stats->mapped_pages++;
			stats->mapping_references += references - 1;
		}
		if (references > 2)
			stats->shared_pages++;
	}
	sleeplock_release(&page_cache.lock);
}
