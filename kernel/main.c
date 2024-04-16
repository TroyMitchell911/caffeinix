#include <mem_layout.h>
#include <vm.h>
#include <uart.h>
#include <palloc.h>
#include <thread.h>
#include <trap.h>

volatile static uint8 start = 0;

void main(void)
{
        if(cpuid() == 0) {
                palloc_init();
                vm_create();
                vm_init();
                thread_init();
                uart_init();
                trap_init_lock();
                trap_init();
                

                uart_putc('h');
                uart_puts("ello, caffeinix\n");

                thread_test();
                
                __sync_synchronize();

                start = 1;
                scheduler();
        } else {
                while(start == 0)
                        ;
                __sync_synchronize();
                vm_init();
                trap_init();
        }

        uart_puts("hartid ");
        uart_putc(cpuid() + '0');
        uart_puts(" started!\n");

        
        
        while(1);
}