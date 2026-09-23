/*
 * File-backed page cache shared by buffered VFS I/O and file VMAs.
 *
 * Entries retain their file and one physical page reference.  Callers which
 * receive a page own a temporary page reference and must pfree() it.
 */
#ifndef __CAFFEINIX_KERNEL_PAGE_CACHE_H
#define __CAFFEINIX_KERNEL_PAGE_CACHE_H

#include <typedefs.h>

struct vfs_file;
struct vfs_inode;
struct vfs_super_block;

struct page_cache_stats {
	uint64 pages;
	uint64 hits;
	uint64 misses;
	uint64 reclaimed;
	uint64 reclaimable_pages;
	uint64 mapped_pages;
	uint64 shared_pages;
	uint64 mapping_references;
};

enum page_cache_truncate_result {
	PAGE_CACHE_TRUNCATE_OK,
	PAGE_CACHE_TRUNCATE_RETRY,
	PAGE_CACHE_TRUNCATE_ERROR = -1,
};

enum page_cache_get_result {
	PAGE_CACHE_GET_OK,
	PAGE_CACHE_GET_RETRY,
	PAGE_CACHE_GET_IO,
	PAGE_CACHE_GET_ERROR,
};

/**
 * page_cache_init() - Initialize the global cache.
 *
 * Context: Early boot once, before cache users exist.
 */
void page_cache_init(void);
/**
 * page_cache_get() - Obtain the cached page containing a file offset.
 * @file: Open file whose inode identifies the cache entry.
 * @offset: Page-aligned file offset.
 * @bytes: Requested bytes in this page; zero is invalid.
 * @page: Receives a page reference on success.
 *
 * Context: Thread context; may perform I/O, allocate, reclaim, and sleep.
 * Return: PAGE_CACHE_GET_OK, RETRY, IO, or ERROR.
 */
enum page_cache_get_result page_cache_get(struct vfs_file *file,
					  uint64 offset, uint32 bytes,
					  void **page);
/**
 * page_cache_mark_dirty() - Mark one cached file page dirty.
 * @file: File identifying the page.
 * @offset: Page-aligned file offset.
 * Context: May take the cache lock.
 *
 * Return: %0 or %-1 if the page is absent.
 */
int page_cache_mark_dirty(struct vfs_file *file, uint64 offset);
int page_cache_mark_executable(struct vfs_file *file, uint64 offset);
int page_cache_refresh(struct vfs_file *file, int user_source,
		       uint64 source, uint64 offset, uint64 count,
		       uint64 old_size);
int page_cache_writeback_file(struct vfs_file *file);
int page_cache_writeback_inode(struct vfs_inode *inode);
int page_cache_writeback_inode_locked(struct vfs_inode *inode);
int page_cache_writeback_super(struct vfs_super_block *superblock);
int page_cache_evict_super(struct vfs_super_block *superblock);
int page_cache_truncate(struct vfs_inode *inode, uint64 old_size,
			uint64 size);
uint64 page_cache_reclaim(uint64 target);
uint64 page_cache_reclaim_mapped(uint64 target);
uint64 page_cache_reclaim_unmapped(void);
/**
 * page_cache_get_stats() - Snapshot cache accounting.
 * @stats: Non-NULL destination for the accounting snapshot.
 *
 * Context: May take the cache lock and sleep while waiting for it.
 */
void page_cache_get_stats(struct page_cache_stats *stats);

#endif
