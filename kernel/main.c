#include <mem_layout.h>
#include <vm.h>
#include <uart.h>
#include <palloc.h>
#include <thread.h>
#include <trap.h>

void main(void)
{
        if(cpuid() == 0) {
                palloc_init();
                vm_init();
                // thread_init();
                uart_init();
                trap_init_lock();
                trap_init();
                /* For test */
                intr_on();

                uart_putc('h');
                uart_puts("ello, caffeinix");
        }
        
        while(1);
}