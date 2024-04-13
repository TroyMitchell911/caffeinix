#include <string.h>
#include <palloc.h>
#include <mem_layout.h>
#include <vm.h>

void main(void)
{
        palloc_init();
        vm_init();

        /* Test the palloc function */
        char* p = (char*)palloc();
        memset(p, 0x28, PGSIZE);
        pfree(p);
        while(1);
}