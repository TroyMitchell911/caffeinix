#include <string.h>
#include <palloc.h>
#include <mem_layout.h>

void main(void)
{
        palloc_init();

        /* Test the palloc function */
        char* p = (char*)palloc();
        memset(p, 0x28, PGSIZE);
        while(1);
}