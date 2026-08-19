#ifndef __CAFFEINIX_KERNEL_PAGE_CACHE_H
#define __CAFFEINIX_KERNEL_PAGE_CACHE_H

#include <typedefs.h>

struct vfs_file;

struct page_cache_stats {
	uint64 pages;
	uint64 hits;
	uint64 misses;
	uint64 reclaimed;
};

void page_cache_init(void);
int page_cache_get(struct vfs_file *file, uint64 offset, void **page);
uint64 page_cache_reclaim(uint64 target);
void page_cache_get_stats(struct page_cache_stats *stats);

#endif
