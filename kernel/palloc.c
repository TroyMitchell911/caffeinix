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

/* Defination is in kernel.ld */
extern char end[];

/* Init the physical memory */
void palloc_init(void)
{
        /* Aligned upward at 4096 bytes */
        char* heap_start = (char*)PGROUNDUP((uint64)end);\
        char* p;

        /* Traverse free memory */
        for(p = heap_start; p <= (char*)(PHY_MEM_STOP - PGSIZE); p += PGSIZE) {
                pfree(p);
        }

}

/* Free the physical memory */
void pfree(void* p)
{
        struct pmem_free_list *pmem_node;

        /* Check the legality of the address of 'p' */
        if(((uint64)p % PGSIZE != 0) || ((char*)p < end) || ((uint64)p > (PHY_MEM_STOP - PGSIZE)))
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
        uint64 count = 0, start;

        if(!page)
                return -1;

        for(i = 0; i < BITMAP_SIZE; i++) {
                mask = 0x80;
                for(j = 0; j < 8; j++) {
                        if((page->bitmap[i] & mask) == 0) {
                                page->bitmap[i] |= mask;
                                if(count == 0) {
                                        start = i * 8 + j;
                                }
                                count++;
                                if(count == blocks) {
                                        page->used += blocks;
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

/**
 * @description: Free memory. (The address of memory has to be alloced by malloc).
 * @param {void*} p: The address of pointer
 * @return {*}
 */
void free(void* p)
{
        block_info_t info;
        page_t page;

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