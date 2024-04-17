#include <mem_layout.h>
#include <vm.h>
#include <console.h>
#include <palloc.h>
#include <thread.h>
#include <trap.h>
#include <printf.h>

volatile static uint8 start = 0;

void main(void)
{
        if(cpuid() == 0) {
                console_init();

                palloc_init();
                vm_create();
                vm_init();
                thread_init();
                trap_init_lock();
                trap_init();

                uart_putc('h');
                uart_puts("ello, caffeinix\n");

                // thread_test();
                
                __sync_synchronize();

                start = 1;
                // scheduler();
                PANIC("test");
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