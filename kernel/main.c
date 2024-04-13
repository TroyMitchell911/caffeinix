#include <string.h>
#include <palloc.h>
#include <mem_layout.h>
#include <vm.h>
#include <uart.h>

void main(void)
{
        palloc_init();
        vm_init();
        uart_init();

        /* Test the palloc function */
        char* p = (char*)palloc();
        memset(p, 0x28, PGSIZE);
        pfree(p);
        uart_putc('h');
        uart_puts("ello, caffeinix");
        while(1);
}