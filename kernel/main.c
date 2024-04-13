#include <mem_layout.h>
#include <vm.h>
#include <uart.h>
#include <palloc.h>
#include <thread.h>

void main(void)
{
        palloc_init();
        vm_init();
        // thread_init();
        uart_init();

        uart_putc('h');
        uart_puts("ello, caffeinix");
        while(1);
}