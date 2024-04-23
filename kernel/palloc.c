#include <palloc.h>
#include <mem_layout.h>
#include <string.h>

struct pmem_free_list {
        struct pmem_free_list *next;
};

static struct pmem_free_list *head = 0;

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