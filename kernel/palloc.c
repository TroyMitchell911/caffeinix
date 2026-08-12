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
#include <palloc.h>
#include <mem_layout.h>
#include <mystring.h>
#include <of.h>

#define MAGIC_NUMBER                            (0x20030528)
#define MIN_SIZE                                (1)
#define USE_BLOCK(x)                            (((x) / MIN_SIZE) + (((x) % MIN_SIZE) ? 1 : 0))
#define INFO_BLOCK                              USE_BLOCK(sizeof(struct block_info))
#define PAGE_BLOCK                              512
#define BITMAP_SIZE                             ((4096 - PAGE_BLOCK) / 8)


typedef struct pmem_free_list {
        struct pmem_free_list *next;
}*pmem_free_list_t;

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

static pmem_free_list_t head = 0;
static struct pool pool;

#define PALLOC_MEMORY_RANGE_MAX 8
#define PALLOC_RESERVED_RANGE_MAX 32

struct palloc_range {
	uint64 start;
	uint64 end;
};

static struct palloc_range memory_ranges[PALLOC_MEMORY_RANGE_MAX];
static struct palloc_range reserved_ranges[PALLOC_RESERVED_RANGE_MAX];
static int memory_range_count;
static int reserved_range_count;
static uint64 heap_start;

/* Defination is in kernel.ld */
extern char end[];

static int palloc_range_contains(const struct palloc_range *range,
				 uint64 start, uint64 finish)
{
	return start >= range->start && finish <= range->end;
}

static int palloc_range_overlaps(const struct palloc_range *range,
				 uint64 start, uint64 finish)
{
	return start < range->end && finish > range->start;
}

int palloc_page_usable(uint64 address)
{
	int i;

	if (address % PGSIZE || address < heap_start ||
	    address + PGSIZE < address)
		return 0;
	for (i = 0; i < memory_range_count; i++) {
		if (palloc_range_contains(&memory_ranges[i], address,
		                          address + PGSIZE))
			break;
	}
	if (i == memory_range_count)
		return 0;
	for (i = 0; i < reserved_range_count; i++) {
		if (palloc_range_overlaps(&reserved_ranges[i], address,
		                          address + PGSIZE))
			return 0;
	}
	return 1;
}

int palloc_memory_range_count(void)
{
	return memory_range_count;
}

int palloc_memory_range_get(int index, uint64 *start, uint64 *finish)
{
	if (index < 0 || index >= memory_range_count || !start || !finish)
		return -1;
	*start = memory_ranges[index].start;
	*finish = memory_ranges[index].end;
	return 0;
}

uint64 palloc_heap_start(void)
{
	return heap_start;
}

static void palloc_discover_memory(void)
{
	struct of_memory_range range;
	uint64 kernel_start = KERNEL_BASE;
	uint64 kernel_end = PGROUNDUP((uint64)end);
	int count, i, kernel_range = 0;

	count = of_memory_range_count();
	if (count <= 0 || count > PALLOC_MEMORY_RANGE_MAX)
		PANIC("unsupported memory layout");
	for (i = 0; i < count; i++) {
		if (of_memory_range_get(i, &range) < 0)
			PANIC("invalid memory range");
		memory_ranges[i].start = PGROUNDUP(range.start);
		memory_ranges[i].end = PGROUNDDOWN(range.start + range.size);
		if (memory_ranges[i].start >= memory_ranges[i].end)
			PANIC("empty memory range");
		if (palloc_range_contains(&memory_ranges[i], kernel_start,
		                          kernel_end))
			kernel_range++;
	}
	memory_range_count = count;
	if (kernel_range != 1)
		PANIC("kernel outside memory");

	count = of_reserved_memory_range_count();
	if (count < 0 || count > PALLOC_RESERVED_RANGE_MAX)
		PANIC("unsupported reserved memory");
	for (i = 0; i < count; i++) {
		if (of_reserved_memory_range_get(i, &range) < 0 ||
		    range.start + range.size < range.start)
			PANIC("invalid reserved memory");
		reserved_ranges[i].start = PGROUNDDOWN(range.start);
		reserved_ranges[i].end = PGROUNDUP(range.start + range.size);
	}
	reserved_range_count = count;
	heap_start = kernel_end;
}

/* Init the physical memory */
void palloc_init(void)
{
	uint64 address;
	int i, pages = 0;

	palloc_discover_memory();
	for (i = 0; i < memory_range_count; i++) {
		for (address = memory_ranges[i].start;
		     address < memory_ranges[i].end; address += PGSIZE) {
			if (!palloc_page_usable(address))
				continue;
			pfree((void *)address);
			pages++;
		}
	}
	if (!pages)
		PANIC("no usable memory");
}

/* Free the physical memory */
void pfree(void* p)
{
        struct pmem_free_list *pmem_node;

	/* Check the legality of the address of 'p'. */
	if (!palloc_page_usable((uint64)p))
                PANIC("pfree");
        
        /* Clear the memory */
        memset(p, 1, PGSIZE);

        /* 
                Convert the 'p' into 'list'Set the byte before <reg width> 
                that p points to as the pointer to the next free memory
        */
        pmem_node = (struct pmem_free_list*)p;
        pmem_node->next = head;
        head = pmem_node;
}

/* Alloc the physical memory */
void* palloc(void)
{
        char* p = 0;
        /* If the head is not NULL */
        if(head) {
                p = (char*)head;
                head = head->next;
        } else {
                PANIC("palloc");
        }
        
        return p;
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
        uint64 count = 0, start = 0, k;

        if(!page)
                return -1;

        for(i = 0; i < BITMAP_SIZE; i++) {
                mask = 0x80;
                for(j = 0; j < 8; j++) {
                        if((page->bitmap[i] & mask) == 0) {
                                if(count == 0) {
                                        start = i * 8 + j;
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

        page = palloc();
        if(!page)
                return 0;

        memset(page, 0, PGSIZE);
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
        int use_blocks, re_count = 0;
        block_info_t info;
        page_t page;
        void* p;
        uint64 ret;

        if(size == 0)
                return 0;

        use_blocks = USE_BLOCK(size);

        page = pool.list;
re:
        ret = malloc_core(page, use_blocks + INFO_BLOCK);
        if(ret == -1 && re_count != 1) {
                re_count++;
                page = malloc_page();
                goto re;
        }

        if(ret == -1)
                return 0;
                
        
        /* Get the memory that the caller uses */
        info = (block_info_t)(ret + (uint64)page);
        
        /* Get the memory of block_info */
        p = (void*)((uint64)info + INFO_BLOCK * MIN_SIZE); 

        /* Change information */
        info->used = use_blocks;
        info->magic = MAGIC_NUMBER;
        info->addr = p;
        info->parent = page;

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
	info = (block_info_t)((uint64)p - (INFO_BLOCK * MIN_SIZE));
        page = info->parent;

        if((char*)p <= (char*)page ||
           (char*)p > (char*)page + PGSIZE || 
           p != info->addr || 
           info->magic != MAGIC_NUMBER) {
                /* Illegal address */
                printf("free: Illegal address\n");
                return;
        }

        free_core(page, (char*)info, INFO_BLOCK + info->used);
}
