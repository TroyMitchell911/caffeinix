/*
 * @Author: TroyMitchell
 * @Date: 2024-05-11
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-26
 * @FilePath: /caffeinix/kernel/palloc.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <palloc.h>
#include <mem_layout.h>
#include <mystring.h>

#define MAGIC_NUMBER                            (0x20030528)
#define MIN_SIZE                                (sizeof(pmem_free_list_t))
#define USE_BLOCK(x)                            (((x) / MIN_SIZE) + (((x) % MIN_SIZE) ? 1 : 0))
#define INFO_BLOCK                              USE_BLOCK(sizeof(struct block_info))
#define PAGE_BLOCK                              USE_BLOCK(PGSIZE)

typedef struct pmem_free_list {
        struct pmem_free_list *next;
}*pmem_free_list_t;

typedef struct page {
        /* Used blocks number */
        uint64 used;
        /* Block list */
        pmem_free_list_t list;
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
 * @description: Malloc core function.Clear the used list element
 * @param {page_t} page: Where the memory that will be alloced belongs to 
 * @param {uint64} blocks: The blocks number of memory that will be alloced 
 * @return {*}
 */
static void malloc_core(page_t page, uint64 blocks)
{
        int i;
        for(i = 0; i < blocks; i++) {
                page->list = page->list->next;
        }
        page->used += blocks;
}

/**
 * @description: Free core function. Load the free list element to their parent
 * @param {page_t} page: Where the memory that will be freed belongs to 
 * @param {char*} start: The beginning of memory address
 * @param {uint64} blocks: The blocks number of memory that will be freed 
 * @param {int} flag: Whether to operate the number of used blocks and free pages
 * @return {*}
 */
static void free_core(page_t page, char* start, uint64 blocks, int flag)
{
        char *p;
        char* end;
        pmem_free_list_t temp;
        page_t pg;

        /* -1 is to prevent exceeding the limit */
        end = start + (blocks - 1) * MIN_SIZE;
        
        for(p = start; p <= end; p += MIN_SIZE) {
                temp = (pmem_free_list_t)p;
                temp->next = page->list;
                page->list = temp;
        }
        if(flag) {
                page->used -= blocks;

                if(page->used == 0) {
                        /* Free the page */
                        for(pg = pool.list; pg; pg = pg->next) {
                                if(pg->next == page) {
                                        pg->next = page->next;
                                        break;
                                }
                        }
                        if(pg) {
                              pfree(page);
                                page = 0;  
                        }
                }
        }
}

/**
 * @description: Malloc a page
                 This function will call free_core to load the list element to parent
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

        free_core(page, (char*)page + sizeof(struct page), PGSIZE / MIN_SIZE, 0);
        return page;
}

/**
 * @description: Search enough space from pool
 * @param {uint64} blocks: Space blocks number
 * @return {*}: The pointer of page that has enough space
 */
static page_t search_space(uint64 blocks)
{
        page_t page;
        for(page = pool.list; page; page = page->next) {
                if(page->used >= blocks)
                        return page;
        }
        return 0;
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
        int use_blocks;
        block_info_t info;
        page_t page;
        void* p;

        if(size == 0)
                return 0;

        use_blocks = USE_BLOCK(size);

        page = search_space(use_blocks + INFO_BLOCK);
        
        if(!page) {
                page = malloc_page();
                if(!page)
                        return 0;
        }

        info = (block_info_t)page->list;
        /* Get the memory of block_info */
        malloc_core(page, INFO_BLOCK);
        
        p = (void*)page->list;
        /* Get the memory that the caller uses */
        malloc_core(page, use_blocks);

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
        uint64 used;

        info = (block_info_t)((uint64)p + (INFO_BLOCK * MIN_SIZE));
        page = info->parent;

        if((char*)p <= (char*)page ||
           (char*)p > (char*)page + PGSIZE || 
           p != info->addr || 
           info->magic != MAGIC_NUMBER) {
                /* Illegal address */
                return;
        }

        used = info->used;
        free_core(page, (char*)info, INFO_BLOCK, 1);
        free_core(page, info->addr, used, 1);
}