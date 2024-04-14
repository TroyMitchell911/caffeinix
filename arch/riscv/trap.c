#include <trap.h>
#include <spinlock.h>
#include <uart.h>

extern void kernel_vec(void);

static struct spinlock trap_spinlock;
/* For test */
uint64 count = 0;

void kernel_trap(void)
{
        if(count++ == 1000000) {
                uart_puts("kernel_trap\n");
                count = 0;
        }
        
}

void trap_init_lock(void)
{
        spinlock_init(&trap_spinlock, "trap");
}

void trap_init(void)
{
        stvec_w((uint64)kernel_vec);
}