#include <trap.h>
#include <spinlock.h>
#include <uart.h>
#include <debug.h>
#include <scheduler.h>

extern void kernel_vec(void);

static struct spinlock trap_spinlock;
/* For test */
uint64 count = 0;
volatile uint64 tick_count = 0;

void kernel_trap(void)
{
        uint64 status = sstatus_r();
        uint64 cause = scause_r();

        if(!(status & SSTATUS_SPP)) 
                PANIC("[kernel_trap]It is not from supervisor mode");
        if(intr_status())
                PANIC("[kernel_trap]Interrupt enabled");

        if(cause == 0x8000000000000001L) {
                tick_count++;
                if(count++ == 10) {
                        uart_puts("timer interrupt\n");
                        count = 0;
                }
                /* Clear the interrupt flag */
                sip_w(sip_r() &~ 2);
                /* We change the running thread if the current thread != 0 and it's state == ACTIVE */
                if(cur_thread() != 0 && cur_thread()->state == ACTIVE)
                        yield();
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