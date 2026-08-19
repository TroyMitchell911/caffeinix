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
};

enum page_cache_truncate_result {
	PAGE_CACHE_TRUNCATE_OK,
	PAGE_CACHE_TRUNCATE_RETRY,
	PAGE_CACHE_TRUNCATE_ERROR = -1,
};

enum page_cache_get_result {
	PAGE_CACHE_GET_OK,
	PAGE_CACHE_GET_IO,
	PAGE_CACHE_GET_ERROR,
};

void page_cache_init(void);
enum page_cache_get_result page_cache_get(struct vfs_file *file,
					  uint64 offset, uint32 bytes,
					  void **page);
int page_cache_mark_dirty(struct vfs_file *file, uint64 offset);
int page_cache_refresh(struct vfs_file *file, int user_source,
		       uint64 source, uint64 offset, uint64 count,
		       uint64 old_size);
int page_cache_writeback_file(struct vfs_file *file);
int page_cache_writeback_inode(struct vfs_inode *inode);
int page_cache_writeback_inode_locked(struct vfs_inode *inode);
int page_cache_writeback_super(struct vfs_super_block *superblock);
int page_cache_truncate(struct vfs_inode *inode, uint64 old_size,
			uint64 size);
uint64 page_cache_reclaim(uint64 target);
void page_cache_get_stats(struct page_cache_stats *stats);

#endif
