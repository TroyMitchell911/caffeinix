/*
 * @Author: TroyMitchell
 * @Date: 2024-05-11
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-27
 * @FilePath: /caffeinix/kernel/palloc.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <buddy.h>
#include <palloc.h>
#include <mem_layout.h>
#include <memrange.h>
#include <mystring.h>
#include <of.h>
#include <spinlock.h>

#define MAGIC_NUMBER                            (0x20030528)
#define MIN_SIZE                                (1)
#define USE_BLOCK(x)                            (((x) / MIN_SIZE) + (((x) % MIN_SIZE) ? 1 : 0))
#define INFO_BLOCK                              USE_BLOCK(sizeof(struct block_info))
#define MALLOC_ALIGNMENT                        16
#define MALLOC_ALIGNMENT_BLOCKS                 USE_BLOCK(MALLOC_ALIGNMENT)
#define ALIGN_BLOCKS(x) \
	(((x) + MALLOC_ALIGNMENT_BLOCKS - 1) & \
	 ~(MALLOC_ALIGNMENT_BLOCKS - 1))
#define PAGE_BLOCK                              512
#define BITMAP_SIZE                             ((4096 - PAGE_BLOCK) / 8)


typedef struct page {
        /* Used blocks number */
        uint64 used;
        /* bitmap of block */
        uint8 bitmap[BITMAP_SIZE];
        /* Points next page */
        struct page *next;
}*page_t;

typedef struct block_info {
        /* Magic number */
        uint64 magic;
        /* Used MIN_SIZE numbers */
        uint64 used;
        /* Points the page that the block belongs to */
        page_t parent;
        /* Alloced address of beginning */
        void* addr;
}*block_info_t;

typedef struct pool {
        page_t list;
        uint64 blocks;
}*pool_t;

static struct buddy_allocator page_allocator;
static struct pool pool;
static struct spinlock page_lock;
static struct spinlock heap_lock;

static struct memrange_set managed_ranges;
static uint64 heap_start;
static uint64 usable_bytes;

_Static_assert((MALLOC_ALIGNMENT & (MALLOC_ALIGNMENT - 1)) == 0,
	       "malloc alignment must be a power of two");
_Static_assert(PAGE_BLOCK % MALLOC_ALIGNMENT == 0,
	       "heap payload must be aligned");

/* Defination is in kernel.ld */
extern char end[];

static void palloc_heap_alignment_selftest(void);

int palloc_memory_range_count(void)
{
	return managed_ranges.count;
}

int palloc_memory_range_get(int index, uint64 *start, uint64 *finish)
{
	return memrange_get(&managed_ranges, index, start, finish);
}

int palloc_page_usable(uint64 address)
{
	if (address % PGSIZE || address + PGSIZE < address)
		return 0;
	return memrange_contains(&managed_ranges, address,
				 address + PGSIZE);
}

uint64 palloc_heap_start(void)
{
	return heap_start;
}

uint64 palloc_usable_bytes(void)
{
	return usable_bytes;
}

static void palloc_discover_memory(void)
{
	struct of_memory_range range;
	uint64 kernel_start = KERNEL_BASE;
	uint64 kernel_end = PGROUNDUP((uint64)end);
	uint64 finish, start;
	int count, i;

	memrange_init(&managed_ranges);
	count = of_memory_range_count();
	if (count <= 0)
		PANIC("unsupported memory layout");
	for (i = 0; i < count; i++) {
		if (of_memory_range_get(i, &range) < 0 ||
		    range.start + range.size < range.start)
			PANIC("invalid memory range");
		if (range.start > (uint64)-1 - (PGSIZE - 1))
			continue;
		start = PGROUNDUP(range.start);
		finish = PGROUNDDOWN(range.start + range.size);
		if (start < finish &&
		    memrange_add(&managed_ranges, start, finish) < 0)
			PANIC("unsupported memory layout");
	}
	if (!memrange_contains(&managed_ranges, kernel_start, kernel_end))
		PANIC("kernel outside memory");
	if (memrange_remove(&managed_ranges, 0, kernel_end) < 0)
		PANIC("reserve kernel memory");

	count = of_reserved_memory_range_count();
	if (count < 0)
		PANIC("unsupported reserved memory");
	for (i = 0; i < count; i++) {
		if (of_reserved_memory_range_get(i, &range) < 0 ||
		    range.start + range.size < range.start)
			PANIC("invalid reserved memory");
		start = PGROUNDDOWN(range.start);
		finish = range.start + range.size;
		if (finish > (uint64)-1 - (PGSIZE - 1))
			PANIC("invalid reserved memory");
		finish = PGROUNDUP(finish);
		if (start < finish &&
		    memrange_remove(&managed_ranges, start, finish) < 0)
			PANIC("unsupported reserved memory");
	}
	if (!managed_ranges.count ||
	    memrange_total(&managed_ranges, &usable_bytes) < 0)
		PANIC("no usable memory");
	for (i = 0; i < managed_ranges.count; i++) {
		if (managed_ranges.ranges[i].end > MAXVA)
			PANIC("memory exceeds Sv39 direct map");
	}
	heap_start = kernel_end;
}

/* Init the physical memory */
void palloc_init(void)
{
	uint64 finish, metadata_bytes, pages, start;
	int i;

	spinlock_init(&page_lock, "physical pages");
	spinlock_init(&heap_lock, "kernel heap");
	palloc_discover_memory();
	buddy_init(&page_allocator);
	usable_bytes = 0;
	for (i = 0; i < managed_ranges.count; i++) {
		if (memrange_get(&managed_ranges, i, &start, &finish) < 0)
			PANIC("managed memory range");
		pages = (finish - start) / PGSIZE;
		metadata_bytes = PGROUNDUP(pages);
		if (metadata_bytes >= finish - start)
			continue;
		if (buddy_add_region(&page_allocator, start, finish,
				     start + metadata_bytes, (uint8 *)start,
				     pages) < 0)
			PANIC("initialize page allocator");
		usable_bytes += finish - start - metadata_bytes;
	}
	if (!usable_bytes)
		PANIC("no usable memory");
	palloc_heap_alignment_selftest();
	if (buddy_free_page_count(&page_allocator) != usable_bytes / PGSIZE)
		PANIC("page allocator accounting");
}

void free_pages(void *p, unsigned int order)
{
	spinlock_acquire(&page_lock);
#ifdef CONFIG_PAGE_POISONING
	if (!buddy_allocated(&page_allocator, (uint64)p, order)) {
		spinlock_release(&page_lock);
		PANIC("free pages");
	}
	memset(p, 1, PGSIZE << order);
#endif
	if (buddy_free(&page_allocator, p, order) < 0) {
		spinlock_release(&page_lock);
		PANIC("free pages");
	}
	spinlock_release(&page_lock);
}

void *alloc_pages(unsigned int order, unsigned int flags)
{
	void *p;

	if (flags & ~PALLOC_ZERO)
		return 0;
	spinlock_acquire(&page_lock);
	p = buddy_alloc(&page_allocator, order);
	spinlock_release(&page_lock);
	if (p && (flags & PALLOC_ZERO))
		memset(p, 0, PGSIZE << order);
	return p;
}

void pfree(void *p)
{
	free_pages(p, 0);
}

void *palloc(void)
{
	void *page = alloc_pages(0, 0);

	if (!page)
		PANIC("out of physical memory");
	return page;
}

void *palloc_zero(void)
{
	return alloc_pages(0, PALLOC_ZERO);
}

/**
 * @description: Malloc core function: Set the bitmap of page.
 * @param {page_t} page: Where the memory that will be alloced belongs to 
 * @param {uint64} blocks: The blocks number of memory that will be alloced 
 * @return {*} -1: Failed Other: offset in page
 */
static uint64 malloc_core(page_t page, uint64 blocks)
{
        int i, j, mask;
        uint64 count = 0, start = 0, k, offset;

        if(!page)
                return -1;

        for(i = 0; i < BITMAP_SIZE; i++) {
                mask = 0x80;
                for(j = 0; j < 8; j++) {
                        if((page->bitmap[i] & mask) == 0) {
				offset = i * 8 + j;
                                if(count == 0) {
					if (offset % MALLOC_ALIGNMENT_BLOCKS) {
						mask >>= 1;
						continue;
					}
					start = offset;
                                }
                                count++;
                                if(count == blocks) {
                                        for(k = start; k < start + blocks; k++) {
                                                page->bitmap[k / 8] |= (0x80 >> (k % 8));
                                        }
                                        page->used += blocks;
                                        // printf("malloc_core: %d->%d\n", start, start + blocks);
                                        return start + PAGE_BLOCK;
                                }
                        } else {
                                count = 0;
                                start = 0;
                        }
                        mask >>= 1;
                }
        }
        return -1;
}

/**
 * @description: Free core function: Clean the bitmap of page.
 * @param {page_t} page: Where the memory that will be freed belongs to 
 * @param {char*} start: The beginning of memory address
 * @param {uint64} blocks: The blocks number of memory that will be freed 
 * @return {*}
 */
static void free_core(page_t page, char* start, uint64 blocks)
{
        uint64 s, e, i;
        page_t pg;

        s = (uint64)start - (uint64)page - PAGE_BLOCK;
        e = s + blocks;

        // printf("free_core: %d->%d\n", e, s);

        for(i = s; i < e; i++) {
                page->bitmap[i / 8] &= ~((0x80) >> (i % 8));
        }

        page->used -= blocks;

        if(page->used == 0) {
                /* Free the page */
                if(page != pool.list) {
                       for(pg = pool.list; pg; pg = pg->next) {
                                if(pg->next == page) {
                                        pg->next = page->next;
                                        break;
                                }
                        } 
                } else {
			pool.list = page->next;
                        pg = page;
                }
                
                if(pg) {
                        pfree(page);
                        page = 0;  
                }
        }
}

/**
 * @description: Malloc a page
                 This function will insert page to pool.list
 * @return {*} The pointer of page that be alloced
 */
static page_t malloc_page(void)
{
        page_t page;

	page = palloc_zero();
        if(!page)
                return 0;

	page->used = 0;
        page->next = pool.list;
        pool.list = page;

        return page;
}

/**
 * @description: Malloc memory (byte-level).
                 This function will call malloc_page to grow up
                 if the pool does not have pages that have enough space.
 * @param {uint64} size: How much memory
 * @return {*}: The pointer of memory that be alloced
 */
void* malloc(uint64 size)
{
	uint64 allocation_blocks, use_blocks;
	int re_count = 0;
        block_info_t info;
        page_t page;
        void* p;
        uint64 ret;

	if(size == 0)
		return 0;

        use_blocks = USE_BLOCK(size);
	if (use_blocks > (uint64)-1 - INFO_BLOCK -
	    (MALLOC_ALIGNMENT_BLOCKS - 1))
		return 0;
	allocation_blocks = ALIGN_BLOCKS(use_blocks + INFO_BLOCK);
	spinlock_acquire(&heap_lock);

        page = pool.list;
re:
	ret = malloc_core(page, allocation_blocks);
        if(ret == -1 && re_count != 1) {
                re_count++;
                page = malloc_page();
                goto re;
        }

        if(ret == -1) {
		spinlock_release(&heap_lock);
                return 0;
	}
                
        
        /* Get the memory that the caller uses */
        info = (block_info_t)(ret + (uint64)page);
        
        /* Get the memory of block_info */
        p = (void*)((uint64)info + INFO_BLOCK * MIN_SIZE); 

        /* Change information */
	info->used = allocation_blocks - INFO_BLOCK;
        info->magic = MAGIC_NUMBER;
        info->addr = p;
        info->parent = page;

	spinlock_release(&heap_lock);
        return p;
}

void* calloc(size_t count, size_t size)
{
	void *pointer;
	uint64 total;

	if (size && count > (uint64)-1 / size)
		return 0;
	total = count * size;
	pointer = malloc(total);
	if (pointer)
		memset(pointer, 0, total);
	return pointer;
}

/**
 * @description: Free memory. (The address of memory has to be alloced by malloc).
 * @param {void*} p: The address of pointer
 * @return {*}
 */
void free(void* p)
{
        block_info_t info;
        page_t page;

	if (!p)
		return;
	spinlock_acquire(&heap_lock);
	info = (block_info_t)((uint64)p - (INFO_BLOCK * MIN_SIZE));
        page = info->parent;

        if((char*)p <= (char*)page ||
           (char*)p > (char*)page + PGSIZE || 
           p != info->addr || 
           info->magic != MAGIC_NUMBER) {
                /* Illegal address */
                printf("free: Illegal address\n");
		spinlock_release(&heap_lock);
                return;
        }

        free_core(page, (char*)info, INFO_BLOCK + info->used);
	spinlock_release(&heap_lock);
}

static void palloc_heap_alignment_selftest(void)
{
	static const uint64 sizes[] = { 1, 3, 7, 15, 17, 31, 33, 127 };
	void *allocations[sizeof(sizes) / sizeof(sizes[0])];
	size_t index;

	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		allocations[index] = malloc(sizes[index]);
		if (!allocations[index] ||
		    (uint64)allocations[index] % MALLOC_ALIGNMENT)
			PANIC("unaligned heap allocation");
	}
	for (; index > 0; index--)
		free(allocations[index - 1]);
}
